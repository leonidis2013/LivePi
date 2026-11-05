#include <cstring>
#include "HostEngine.h"
#include "HwMixer.h"
#include <cstdlib>

using namespace juce;

namespace
{
    File appDir()
    {
        auto d = File::getSpecialLocation (File::userHomeDirectory).getChildFile (".livepi");
        d.createDirectory();
        return d;
    }

    // Value text with its unit, without doubling it when the plugin already includes it ("0.0 dB dB")
    String paramText (AudioProcessorParameter& p, float v)
    {
        auto t = p.getText (v, 24).trim();
        const auto label = p.getLabel().trim();
        if (label.isNotEmpty() && ! t.endsWithIgnoreCase (label))
            t << " " << label;
        return t;
    }

    var ok()
    {
        auto* o = new DynamicObject();
        o->setProperty ("ok", true);
        return var (o);
    }

    var err (const String& msg)
    {
        auto* o = new DynamicObject();
        o->setProperty ("error", msg);
        return var (o);
    }

    Array<var> toVarArray (const StringArray& s)
    {
        Array<var> a;
        for (auto& x : s) a.add (x);
        return a;
    }

    ChannelStrip* stripOf (const AudioProcessorGraph::Node::Ptr& n)
    {
        return static_cast<ChannelStrip*> (n->getProcessor());
    }
}

// Plugin editor on the (virtual) screen: borderless, full screen width available, top-left corner.
// The web page shows exactly this area, so there is no title bar or frame to get in the way.
class EditorWindow : public Component, private ComponentListener
{
public:
    explicit EditorWindow (AudioProcessorEditor* ed) : editor (ed)
    {
        setOpaque (true);
        addAndMakeVisible (editor.get());
        editor->addComponentListener (this);
        setSize (editor->getWidth(), editor->getHeight());
        addToDesktop (0);           // no title bar, no border
        setTopLeftPosition (0, 0);
        setVisible (true);
        toFront (false);
    }

    ~EditorWindow() override
    {
        editor->removeComponentListener (this);
        editor = nullptr;           // deletes the plugin editor while the window still exists
    }

    void paint (Graphics& g) override { g.fillAll (Colours::black); }

    // Sizes a resizable plugin to fill as much of the screen as possible with the viewer's aspect
    // ratio (full screen on the phone/tablet); fixed-size plugins keep their own size.
    void fitTo (int viewW, int viewH, int screenW, int screenH)
    {
        if (! editor->isResizable() || viewW < 50 || viewH < 50)
        {
            clampToScreen (screenW, screenH);
            return;
        }
        const double aspect = (double) viewW / (double) viewH;
        int w = screenW, h = (int) std::round (screenW / aspect);
        if (h > screenH) { h = screenH; w = (int) std::round (screenH * aspect); }
        if (auto* c = editor->getConstrainer())
        {
            auto r = editor->getBounds().withSize (w, h);
            c->checkBounds (r, editor->getBounds(), { 0, 0, screenW, screenH }, false, false, true, true);
            w = r.getWidth(); h = r.getHeight();
        }
        editor->setSize (jmax (100, w), jmax (60, h));
        clampToScreen (screenW, screenH);
    }

    Rectangle<int> getEditorScreenArea() const { return editor->getScreenBounds(); }

private:
    std::unique_ptr<AudioProcessorEditor> editor;

    void clampToScreen (int screenW, int screenH)
    {
        if (editor->getWidth() > screenW || editor->getHeight() > screenH)
        {
            const float sc = jmin ((float) screenW / (float) editor->getWidth(), (float) screenH / (float) editor->getHeight());
            if (editor->isResizable())
                editor->setSize ((int) (editor->getWidth() * sc), (int) (editor->getHeight() * sc));
        }
    }

    void componentMovedOrResized (Component&, bool, bool wasResized) override
    {
        if (wasResized)
            setSize (editor->getWidth(), editor->getHeight());
    }
};

HostEngine::HostEngine()
{
    formatManager.addFormat (new VST3PluginFormat());

    using IO = AudioProcessorGraph::AudioGraphIOProcessor;
    inputNode  = graph.addNode (std::make_unique<IO> (IO::audioInputNode));
    outputNode = graph.addNode (std::make_unique<IO> (IO::audioOutputNode));

    chains.push_back (makeChain (0));
    monBus = makeChain (kMonBus);
    monBus.name = "Monitor mix";
    player.setProcessor (&graph);

    if (auto xml = XmlDocument::parse (appDir().getChildFile ("plugins.xml")))
        known.recreateFromXml (*xml);
}

HostEngine::~HostEngine()
{
    shutdown();
}

void HostEngine::start()
{
    // Create the real device types first, then add the virtual card, so both are in the menu
    deviceManager.getAvailableDeviceTypes();
    deviceManager.addAudioDeviceType (std::make_unique<VirtualAudio::DeviceType> (dataDir().getChildFile ("test-input.wav")));

    deviceChanging = true;
    std::unique_ptr<XmlElement> saved = XmlDocument::parse (appDir().getChildFile ("audio.xml"));
    auto e = deviceManager.initialise (16, 16, saved.get(), true);
    if (e.isNotEmpty())
        Logger::writeToLog ("Audio init: " + e);

    // No usable sound card (or none plugged in yet): run on the virtual one so the page still works.
    // The reconnect timer below switches to the real card as soon as it appears.
    if (deviceManager.getCurrentAudioDevice() == nullptr)
        useVirtualFallback();
    deviceChanging = false;

    HwMixer::restore();   // the interface's own levels as you last set them

    deviceManager.addAudioCallback (&player);
    deviceManager.addChangeListener (this);
    running = true;
    deviceChanged();

    // ---- Crash-loop protection. Two kinds of failure are counted:
    //  - starts that did not last 30 s (a crash or a hang while loading the session), and
    //  - restarts by the watchdog (a plugin froze the audio) within 10 minutes.
    // After two failed starts, or three watchdog restarts, LivePi starts in safe mode: the session
    // without plugins, so the audio passes through unprocessed. Safe mode stays on (also after a
    // reboot) until you choose on the page, and last.json keeps your plugins all along.
    auto starts = dataDir().getChildFile ("starts.txt");
    const int unstable = starts.loadFileAsString().trim().getIntValue();
    starts.replaceWithText (String (unstable + 1));
    const int kills = recentWatchdogRestarts();
    const bool wasSafe = dataDir().getChildFile ("safe-mode").existsAsFile();
    safeMode = wasSafe || unstable >= 2 || kills >= 3;
    dryChannels = safeMode;              // mixer mode: in safe mode the channels pass the audio unprocessed

    // The watchdog runs from here on, so a plugin that hangs while loading is caught too
    health = std::make_unique<Reliability::PiHealth>();
    startDeadlineMs = Time::getMillisecondCounterHiRes() + 90000.0;
    messageBeatMs = Time::getMillisecondCounterHiRes();
    watchdog = std::thread ([this] { runWatchdog(); });

    auto note = dataDir().getChildFile ("restart-note.txt");
    if (note.existsAsFile())
    {
        addNotice ("restart", note.loadFileAsString().trim());
        note.deleteFile();
    }

    auto last = appDir().getChildFile ("last.json"), good = appDir().getChildFile ("last-good.json");
    var session = JSON::parse (last);
    if (! session.isObject() && last.existsAsFile())
    {
        session = JSON::parse (good);
        if (session.isObject())
        {
            addNotice ("damaged", "The saved session file was damaged. LivePi loaded the last good copy instead.");
            last.replaceWithText (JSON::toString (session, true));
        }
    }
    if (! session.isObject())
        safeMode = false;                       // nothing to load, so nothing to leave out

    if (safeMode)
    {
        if (! wasSafe)
        {
            dataDir().getChildFile ("safe-mode").replaceWithText (Time::getCurrentTime().toISO8601 (true));
            dataDir().getChildFile ("watchdog-restarts.txt").deleteFile();
            Logger::writeToLog ("Safe mode: started without plugins (" + String (unstable) + " failed starts, " + String (kills) + " watchdog restarts)");
        }
        // What you changed while in safe mode (faders, names...) is in last-safe.json; last.json keeps the plugins
        auto safeSession = JSON::parse (appDir().getChildFile ("last-safe.json"));
        deserialise (safeSession.isObject() ? withoutPlugins (safeSession) : withoutPlugins (session));
        const bool asked = dataDir().getChildFile ("safe-mode").loadFileAsString().startsWith ("button");
        if (asked)
            addNotice ("safe", "Safe mode: you asked for it with the Pisound button, so LivePi is running without plugins. The audio passes through unprocessed. Your plugins are kept.", "bad");
        else
        addNotice ("safe", String ("Safe mode: ") + (kills >= 3 ? "a plugin froze the audio several times" : "LivePi stopped unexpectedly while starting")
                           + ", so it is running without plugins. The audio passes through unprocessed. Your plugins are kept.", "bad");
    }
    else if (session.isObject())
    {
        deserialise (session);
    }

    rebuildConnections();
    startDone = true;

    // The Pisound button: its own thread; each press is handled on the message thread
    buttonCfg = JSON::parse (dataDir().getChildFile ("button.json"));
    button.onGesture = [this, flag = alive] (int g)
    {
        MessageManager::callAsync ([this, flag, g] { if (flag->load()) runButton (g); });
    };
    button.start();

    auto flag = alive;
    // Loading plugins while the audio already runs can cause a few dropouts: don't count those
    Timer::callAfterDelay (3000, [this, flag] { if (flag->load()) resetXruns(); });
    // Ran fine for 30 s: this start counts as good, and the session is kept as the last good copy
    Timer::callAfterDelay (30000, [this, flag]
    {
        if (! flag->load()) return;
        dataDir().getChildFile ("starts.txt").replaceWithText ("0");
        if (! safeMode && JSON::parse (appDir().getChildFile ("last.json")).isObject())
            appDir().getChildFile ("last.json").copyFileTo (appDir().getChildFile ("last-good.json"));
    });

    // Real sound card missing (not plugged in yet, driver still loading at boot, or unplugged): retry
    struct Reconnect : Timer
    {
        HostEngine& e;
        explicit Reconnect (HostEngine& h) : e (h) { startTimer (4000); }
        void timerCallback() override { e.tryReconnect(); }
    };
    reconnectTimer = std::make_unique<Reconnect> (*this);

    // Proof of life from the message thread (the page, plugin loading, device changes all run there)
    struct Beat : Timer
    {
        std::atomic<double>& t;
        explicit Beat (std::atomic<double>& x) : t (x) { startTimer (1000); }
        void timerCallback() override { t = Time::getMillisecondCounterHiRes(); }
    };
    messageBeatMs = Time::getMillisecondCounterHiRes();
    messageBeat = std::make_unique<Beat> (messageBeatMs);
}

int HostEngine::recentWatchdogRestarts()
{
    const auto now = Time::currentTimeMillis() / 1000;
    int n = 0;
    for (auto& l : StringArray::fromLines (dataDir().getChildFile ("watchdog-restarts.txt").loadFileAsString()))
        if (l.trim().isNotEmpty() && now - l.trim().getLargeIntValue() < 600)
            ++n;
    return n;
}

void HostEngine::useVirtualFallback()
{
    autoFallback = true;
    deviceManager.setCurrentAudioDeviceType (VirtualAudio::typeName, true);
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.inputDeviceName = VirtualAudio::inTone;
    setup.outputDeviceName = VirtualAudio::outName;
    setup.sampleRate = 48000.0;
    setup.bufferSize = 128;
    setup.useDefaultInputChannels = setup.useDefaultOutputChannels = true;
    auto fe = deviceManager.setAudioDeviceSetup (setup, true);
    Logger::writeToLog ("No audio device found, using the virtual one. " + fe);
}

void HostEngine::shutdown()
{
    if (! running)
        return;
    running = false;
    *alive = false;
    sd.send ("STOPPING=1");
    watchdogStop = true;                 // first, so closing the device below is not taken for a failure
    button.stop();
    if (watchdog.joinable())
        watchdog.join();
    reconnectTimer.reset();
    messageBeat.reset();
    health.reset();
    if (benchThread.joinable())
        benchThread.join();
    saveLast();
    dataDir().getChildFile ("starts.txt").replaceWithText ("0");   // a clean stop is not a crash
    editors.clear();
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (&player);
    player.setProcessor (nullptr);
    deviceManager.closeAudioDevice();
}

void HostEngine::changeListenerCallback (ChangeBroadcaster*)
{
    xrunBase = 0; xrunDevice = deviceManager.getCurrentAudioDevice();   // device (re)opened: its count starts at 0
    // Only remember a deliberate, working setup (not the automatic fallback, not "no device")
    if (! autoFallback && deviceManager.getCurrentAudioDevice() != nullptr)
      if (auto xml = deviceManager.createStateXml())
        xml->writeTo (appDir().getChildFile ("audio.xml"));
    deviceChanged();
    rebuildConnections();
}

void HostEngine::deviceChanged()
{
    auto* d = deviceManager.getCurrentAudioDevice();
    deviceOpen = d != nullptr && d->isOpen();
    lastDeviceChangeMs = Time::getMillisecondCounterHiRes();
}

// ---------------------------------------------------------------- mixer mode

VoiceChannel* HostEngine::voiceOf (int channel)
{
    if (! isPositiveAndBelow (channel, (int) chains.size())) return nullptr;
    auto& c = chains[(size_t) channel];
    return c.voice != nullptr ? dynamic_cast<VoiceChannel*> (c.voice->getProcessor()) : nullptr;
}

var HostEngine::setMode (const String& mode)
{
    if (mode != "vst" && mode != "mixer")
        return err ("Mode must be vst or mixer");
    if ((mode == "mixer") == mixerMode)
        return ok();
    // Everything is kept: the session is saved, the mode flipped, and it is loaded again. Each
    // channel keeps both its VST plugins and its mixer settings; the unused ones wait untouched.
    auto session = serialise();
    if (auto* o = session.getDynamicObject()) o->setProperty ("mode", mode);
    editors.clear();
    auto r = deserialise (session);
    settleAll();
    saveLast();
    Logger::writeToLog ("Mode: " + mode);
    return r;
}

var HostEngine::voiceView (int channel, bool full)
{
    auto* vc = voiceOf (channel);
    if (vc == nullptr) return err ("not a mixer channel");
    return vc->getView (full);
}

var HostEngine::voiceSpectrum (int channel, bool output, const String& module)
{
    auto* vc = voiceOf (channel);
    if (vc == nullptr) return err ("not a mixer channel");
    return vc->getSpectrum (output, VoiceChannel::moduleFromId (module));
}

var HostEngine::voiceParam (int channel, const String& id, double value, bool normalised)
{
    auto* vc = voiceOf (channel);
    if (vc == nullptr) return err ("not a mixer channel");
    auto r = normalised ? vc->setParamNorm (id, value) : vc->setParam (id, value);
    if (r.isVoid()) return err ("unknown setting");
    markDirty();
    return r;
}

var HostEngine::voiceOrder (int channel, const var& order)
{
    auto* vc = voiceOf (channel);
    auto* a = order.getArray();
    if (vc == nullptr || a == nullptr || a->size() != VoiceChannel::numModules) return err ("bad order");
    std::array<int, VoiceChannel::numModules> o {};
    for (int i = 0; i < VoiceChannel::numModules; ++i) o[(size_t) i] = VoiceChannel::moduleFromId ((*a)[i].toString());
    if (! vc->setOrder (o)) return err ("bad order");
    markDirty();
    return ok();
}

var HostEngine::voicePreset (int channel, int index)
{
    auto* vc = voiceOf (channel);
    if (vc == nullptr) return err ("not a mixer channel");
    if (! (bool) vc->loadFactoryPreset (index)) return err ("no such preset");
    markDirty();
    return ok();
}

var HostEngine::voiceNotch (int channel, const String& cmd, int slot)
{
    auto* vc = voiceOf (channel);
    if (vc == nullptr) return err ("not a mixer channel");
    if (! (bool) vc->notchCommand (cmd, slot)) return err ("unknown command");
    markDirty();
    return ok();
}

var HostEngine::voiceCopy (int from, int to)
{
    auto* a = voiceOf (from);
    auto* b = voiceOf (to);
    if (a == nullptr || b == nullptr || from == to) return err ("bad channel");
    MemoryBlock mb;
    a->getStateInformation (mb);
    b->setStateInformation (mb.getData(), (int) mb.getSize());
    markDirty();
    return ok();
}

// ---------------------------------------------------------------- Pisound button

namespace
{
    const char* const gestureIds[] = { "click1", "click2", "click3", "hold", "longHold" };
    const char* const gestureNames[] = { "1 click", "2 clicks", "3 clicks", "Hold 2 s", "Hold 5 s" };
    struct ButtonAction { const char* id; const char* name; bool channel; int gestures; };   // gestures: bit mask
    constexpr int anyGesture = 0x1f, holds = 0x18, longOnly = 0x10;
    const ButtonAction buttonActions[] = {
        { "none",          "Nothing",                              false, anyGesture },
        { "mute",          "Mute / unmute a channel",              true,  anyGesture },
        { "muteAll",       "Mute / unmute every channel",          false, anyGesture },
        { "bypass",        "Bypass a channel's effects",          true, anyGesture },
        { "nextPreset",    "Next saved preset",                    false, anyGesture },
        { "prevPreset",    "Previous saved preset",                false, anyGesture },
        { "clearFeedback", "Clear learned feedback (mixer mode)",  true,  anyGesture },
        { "monMute",       "Mute / unmute the monitor mix",        false, anyGesture },
        { "safeRestart",   "Restart LivePi in safe mode",          false, holds },
        { "shutdown",      "Shut down the Pi",                     false, longOnly },
    };
    const ButtonAction* findButtonAction (const String& id)
    {
        for (auto& a : buttonActions) if (id == a.id) return &a;
        return nullptr;
    }
}

var HostEngine::getButton()
{
    auto* o = new DynamicObject();
    const bool present = PisoundButton::present();
    const int st = button.status.load();
    o->setProperty ("present", present);
    o->setProperty ("status", st == PisoundButton::ok ? "ok" : st == PisoundButton::denied ? "denied" : st == PisoundButton::failed ? "failed" : "absent");
    o->setProperty ("error", button.lastErrno.load() != 0 ? String (std::strerror (button.lastErrno.load())) : String());
    o->setProperty ("down", button.down.load());
    o->setProperty ("led", PisoundButton::ledWritable());
    o->setProperty ("blokas", present && PisoundButton::blokasProgramRunning());
    o->setProperty ("fixAvailable", File ("/usr/local/sbin/livepi-tune").existsAsFile());
    o->setProperty ("enabled", (bool) buttonCfg.getProperty ("enabled", true));
    o->setProperty ("last", lastPress);
    o->setProperty ("lastAgo", lastPressAt > 0 ? (int) ((Time::currentTimeMillis() - lastPressAt) / 1000) : -1);

    Array<var> gs, as;
    auto* cur = buttonCfg.getProperty ("actions", {}).getDynamicObject();
    for (int g = 0; g < PisoundButton::numGestures; ++g)
    {
        auto* go = new DynamicObject();
        go->setProperty ("id", gestureIds[g]);
        go->setProperty ("name", gestureNames[g]);
        var a = cur != nullptr ? cur->getProperty (gestureIds[g]) : var();
        go->setProperty ("action", a.getProperty ("action", "none"));
        go->setProperty ("channel", a.getProperty ("channel", 0));
        gs.add (var (go));
    }
    for (auto& a : buttonActions)
    {
        auto* ao = new DynamicObject();
        ao->setProperty ("id", a.id);
        ao->setProperty ("name", a.name);
        ao->setProperty ("channel", a.channel);
        Array<var> allowed;
        for (int g = 0; g < PisoundButton::numGestures; ++g) if (a.gestures & (1 << g)) allowed.add (gestureIds[g]);
        ao->setProperty ("gestures", allowed);
        as.add (var (ao));
    }
    o->setProperty ("gestures", gs);
    o->setProperty ("actions", as);
    return var (o);
}

var HostEngine::setButton (const var& body)
{
    auto* cfg = new DynamicObject();
    cfg->setProperty ("enabled", (bool) body.getProperty ("enabled", buttonCfg.getProperty ("enabled", true)));
    auto* acts = new DynamicObject();
    auto* in = body.getProperty ("actions", {}).getDynamicObject();
    auto* old = buttonCfg.getProperty ("actions", {}).getDynamicObject();
    for (int g = 0; g < PisoundButton::numGestures; ++g)
    {
        var a = in != nullptr && in->hasProperty (gestureIds[g]) ? in->getProperty (gestureIds[g])
              : old != nullptr ? old->getProperty (gestureIds[g]) : var();
        const auto id = a.getProperty ("action", "none").toString();
        auto* def = findButtonAction (id);
        if (def == nullptr) return err ("Unknown action: " + id);
        if ((def->gestures & (1 << g)) == 0) return err (String (def->name) + " can't be used for " + gestureNames[g]);
        auto* ao = new DynamicObject();
        ao->setProperty ("action", id);
        ao->setProperty ("channel", jlimit (0, 15, (int) a.getProperty ("channel", 0)));
        acts->setProperty (gestureIds[g], var (ao));
    }
    cfg->setProperty ("actions", var (acts));
    buttonCfg = var (cfg);
    if (! dataDir().getChildFile ("button.json").replaceWithText (JSON::toString (buttonCfg, true)))
        return err ("Could not save the button settings");
    return ok();
}

void HostEngine::runButton (int g)
{
    if (g < 0 || g >= PisoundButton::numGestures) return;
    lastPressAt = Time::currentTimeMillis();
    if (! (bool) buttonCfg.getProperty ("enabled", true))
    {
        lastPress = String (gestureNames[g]) + ": the button is turned off in Settings";
        return;
    }
    var a = buttonCfg.getProperty ("actions", {}).getProperty (gestureIds[g], {});
    const auto id = a.getProperty ("action", "none").toString();
    auto* def = findButtonAction (id);
    const String what = def == nullptr || id == "none" ? String ("nothing set") : buttonAction (id, (int) a.getProperty ("channel", 0));
    lastPress = String (gestureNames[g]) + ": " + what;
    Logger::writeToLog ("Pisound button: " + lastPress);
}

String HostEngine::buttonAction (const String& id, int ch)
{
    auto chName = [this] (int i) { return i >= 0 && i < (int) chains.size() ? chains[(size_t) i].name : "Ch " + String (i + 1); };
    auto done = [] (const String& s) { PisoundButton::flashLed (1); return s; };

    if (id == "mute")
    {
        if (ch >= (int) chains.size()) return chName (ch) + " does not exist";
        auto& c = chains[(size_t) ch];
        c.mute = ! c.mute; applyMix (c); markDirty();
        return done (chName (ch) + (c.mute ? " muted" : " unmuted"));
    }
    if (id == "muteAll")
    {
        const bool any = std::any_of (chains.begin(), chains.end(), [] (const Chain& c) { return ! c.mute; });
        for (auto& c : chains) { c.mute = any; applyMix (c); }
        markDirty();
        return done (any ? "every channel muted" : "every channel unmuted");
    }
    if (id == "monMute")
    {
        monBus.mute = ! monBus.mute; applyMix (monBus); markDirty();
        return done (monBus.mute ? "monitor mix muted" : "monitor mix unmuted");
    }
    if (id == "bypass")
    {
        if (ch >= (int) chains.size()) return chName (ch) + " does not exist";
        if (auto* vc = voiceOf (ch))
        {
            const bool on = vc->apvts.getRawParameterValue ("bypass")->load() < 0.5f;
            vc->setParam ("bypass", on ? 1.0 : 0.0);
            markDirty();
            return done (chName (ch) + (on ? " bypassed" : " back on"));
        }
        auto& c = chains[(size_t) ch];
        if (c.slots.empty()) return chName (ch) + " has no plugins";
        const bool on = std::any_of (c.slots.begin(), c.slots.end(), [] (const Slot& s) { return ! s.bypassed; });
        for (auto& sl : c.slots) sl.bypassed = on;
        rebuildConnections();
        saveLast();
        return done (chName (ch) + (on ? " plugins bypassed" : " plugins back on"));
    }
    if (id == "nextPreset" || id == "prevPreset")
    {
        StringArray names;
        for (auto& f : appDir().getChildFile ("presets").findChildFiles (File::findFiles, false, "*.json"))
            names.add (f.getFileNameWithoutExtension());
        names.sortNatural();
        if (names.isEmpty()) return "no saved presets";
        int i = names.indexOf (currentPreset);
        i = i < 0 ? (id == "nextPreset" ? 0 : names.size() - 1)
                  : (i + (id == "nextPreset" ? 1 : names.size() - 1)) % names.size();
        auto r = loadPreset (names[i]);
        if (r.hasProperty ("error")) return "could not load " + names[i];
        return done ("loaded preset " + names[i]);
    }
    if (id == "clearFeedback")
    {
        auto* vc = voiceOf (ch);
        if (vc == nullptr) return chName (ch) + " is not a mixer channel";
        vc->notchCommand ("clearFloating", 0);
        markDirty();
        return done (chName (ch) + ": learned feedback notches cleared");
    }
    if (id == "safeRestart")
    {
        saveLast();
        appDir().getChildFile ("last-safe.json").deleteFile();
        dataDir().getChildFile ("safe-mode").replaceWithText ("button " + Time::getCurrentTime().toISO8601 (true));
        PisoundButton::flashLed (5);
        Timer::callAfterDelay (600, [] { JUCEApplicationBase::quit(); });
        return "restarting in safe mode";
    }
    if (id == "shutdown")
    {
        saveLast();
        PisoundButton::flashLed (10);
        Timer::callAfterDelay (800, []
        {
            ChildProcess p;
            if (p.start (StringArray { "sudo", "-n", "/usr/bin/systemctl", "poweroff" }))
                p.waitForProcessToFinish (5000);
        });
        return "shutting down";
    }
    return "nothing set";
}

// ---------------------------------------------------------------- reliability

void HostEngine::addNotice (const String& id, const String& text, const String& level)
{
    for (auto& n : notices)
        if (n.id == id) { n.text = text; n.level = level; return; }
    notices.push_back ({ id, text, level });
}

var HostEngine::dismissNotice (const String& id)
{
    notices.erase (std::remove_if (notices.begin(), notices.end(), [&] (const Notice& n) { return n.id == id; }), notices.end());
    if (id == "safe" && safeMode)
    {
        // "Keep going without plugins": a deliberate choice. Keep a copy of the session with its plugins.
        appDir().getChildFile ("last.json").copyFileTo (appDir().getChildFile ("last-before-safe-mode.json"));
        safeMode = false;
        dataDir().getChildFile ("safe-mode").deleteFile();
        appDir().getChildFile ("last-safe.json").deleteFile();
        saveLast();
    }
    return ok();
}

var HostEngine::restoreSession()
{
    auto v = JSON::parse (appDir().getChildFile ("last.json"));     // never overwritten in safe mode
    if (! v.isObject())
        return err ("There is no saved session to load.");
    safeMode = false;
    dryChannels = false;
    dataDir().getChildFile ("safe-mode").deleteFile();
    appDir().getChildFile ("last-safe.json").deleteFile();
    notices.erase (std::remove_if (notices.begin(), notices.end(), [] (const Notice& n) { return n.id == "safe"; }), notices.end());
    auto r = deserialise (v);
    saveLast();
    return r;
}

var HostEngine::withoutPlugins (const var& session)
{
    auto copy = JSON::parse (JSON::toString (session));        // deep copy
    if (auto* chainArr = copy.getProperty ("chains", {}).getArray())
        for (auto& c : *chainArr)
            if (auto* o = c.getDynamicObject())
                o->setProperty ("slots", Array<var>());
    if (auto* mb = copy.getProperty ("monitorBus", {}).getDynamicObject())
        mb->setProperty ("slots", Array<var>());
    return copy;
}

// Watches the audio from its own thread (it keeps working if the rest of LivePi is busy or stuck):
//  - audio stopped but not inside the plugins (the device failed): reopen the device; if that does
//    not bring it back within 12 s, restart LivePi.
//  - stuck inside the plugins for 4 s (a plugin hung): restart LivePi; nothing else can recover it.
// It also tells systemd that LivePi is alive. If LivePi freezes completely, systemd restarts it after 30 s.
void HostEngine::runWatchdog()
{
    sd.send ("READY=1\nWATCHDOG_USEC=30000000");
    auto lastHb = player.heartbeat.load();
    double lastBeat = Time::getMillisecondCounterHiRes(), stageAt = 0.0, changingSince = 0.0;
    int stage = 0;

    while (! watchdogStop.load())
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (250));
        const double t = Time::getMillisecondCounterHiRes();
        const auto hb = player.heartbeat.load (std::memory_order_relaxed);
        if (hb != lastHb)
        {
            lastHb = hb;
            lastBeat = t;
            if (stage != 0) { Logger::writeToLog ("Watchdog: audio is running again"); stage = 0; }
        }

        // Loading the session at start-up must finish (a plugin can hang in its constructor)
        if (! startDone.load() && t > startDeadlineMs.load())
            die ("Loading the session took more than 90 seconds (a plugin hung while loading), so LivePi restarted itself");

        // The message thread runs the page, plugin loading and device changes. Stuck for 3 minutes
        // means a deadlock: nothing can be controlled any more, so restart.
        if (startDone.load() && t - messageBeatMs.load() > 180000.0)
            die ("LivePi stopped responding to the page for 3 minutes, so it restarted itself");

        // A device change (or a restart of the device) must finish too
        if (deviceChanging.load())
        {
            if (changingSince == 0.0) changingSince = t;
            if (t - changingSince > 20000.0)
                die ("The audio interface did not open or close within 20 seconds, so LivePi restarted itself");
        }
        else
        {
            changingSince = 0.0;
        }

        const double quiet = t - jmax (lastBeat, player.lastStartMs.load(), lastDeviceChangeMs.load());
        if (! deviceOpen.load())
        {
            stage = 0;                                  // no device: the reconnect timer is on it
        }
        else if (player.inCallback.load())
        {
            if (quiet > 4000)
                die ("A plugin stopped responding and froze the audio, so LivePi restarted itself");
        }
        else if (! deviceChanging.load() && quiet > 3000)
        {
            if (stage == 0)
            {
                stage = 1;
                stageAt = t;
                Logger::writeToLog ("Watchdog: no audio for 3 s, restarting the audio device");
                auto flag = alive;
                MessageManager::callAsync ([this, flag, hb] { if (flag->load()) restartAudio (hb); });
            }
            else if (t - stageAt > 12000)
            {
                die ("The audio interface stopped and could not be restarted, so LivePi restarted itself");
            }
        }

        sd.send ("WATCHDOG=1");
    }
}

void HostEngine::die (const String& reason)
{
    const auto text = reason + " at " + Time::getCurrentTime().formatted ("%H:%M:%S on %d %b") + ".";
    dataDir().getChildFile ("restart-note.txt").replaceWithText (text);
    dataDir().getChildFile ("watchdog-restarts.txt").appendText (String (Time::currentTimeMillis() / 1000) + "\n");
    Logger::writeToLog ("Watchdog: " + text);
    std::_Exit (70);                    // systemd starts LivePi again within a second
}

void HostEngine::restartAudio (juce::uint64 heartbeatWhenStalled)
{
    if (player.heartbeat.load() != heartbeatWhenStalled)
        return;                          // it came back by itself meanwhile: leave the working device alone
    deviceChanging = true;
    deviceManager.closeAudioDevice();
    deviceManager.restartLastAudioDevice();
    deviceChanging = false;
    deviceChanged();
    if (deviceOpen.load())
    {
        HwMixer::restore();
        addNotice ("audio", "The audio stopped at " + Time::getCurrentTime().formatted ("%H:%M:%S") + " and LivePi restarted the audio interface.");
    }
    else
    {
        addNotice ("audio", "The audio interface stopped at " + Time::getCurrentTime().formatted ("%H:%M:%S")
                            + ". LivePi keeps trying to reconnect it.", "bad");
    }
}

void HostEngine::tryReconnect()
{
    const bool haveDevice = deviceOpen.load();
    if (haveDevice && ! autoFallback)
    {
        reconnectFailures = 0;
        return;
    }
    // After a failed attempt to open it, wait longer each time (up to a minute), so a device that is
    // listed but will not open does not keep the rest of LivePi busy
    if (Time::getMillisecondCounterHiRes() < reconnectNotBeforeMs)
        return;

    std::unique_ptr<XmlElement> saved = XmlDocument::parse (appDir().getChildFile ("audio.xml"));
    if (saved == nullptr)
        return;
    const auto typeName = saved->getStringAttribute ("deviceType");
    if (typeName.isEmpty() || typeName == VirtualAudio::typeName)
        return;

    AudioIODeviceType* type = nullptr;
    for (auto* t : deviceManager.getAvailableDeviceTypes())
        if (t->getTypeName() == typeName) type = t;
    if (type == nullptr)
        return;
    type->scanForDevices();
    const auto in = saved->getStringAttribute ("audioInputDeviceName"), out = saved->getStringAttribute ("audioOutputDeviceName");
    if ((in.isNotEmpty() && ! type->getDeviceNames (true).contains (in)) || (out.isNotEmpty() && ! type->getDeviceNames (false).contains (out)))
        return;                                  // still missing: try again in a few seconds

    deviceChanging = true;
    const bool wasFallback = autoFallback;
    autoFallback = false;
    auto e = deviceManager.initialise (16, 16, saved.get(), false);
    if (deviceManager.getCurrentAudioDevice() == nullptr || ! deviceManager.getCurrentAudioDevice()->isOpen())
    {
        if (reconnectFailures == 0)
            Logger::writeToLog ("Reconnect failed (will keep trying): " + e);
        ++reconnectFailures;
        reconnectNotBeforeMs = Time::getMillisecondCounterHiRes() + jmin (60000.0, 4000.0 * std::pow (2.0, (double) jmin (reconnectFailures, 4)));
        if (wasFallback) useVirtualFallback();
        deviceChanging = false;
        deviceChanged();
        return;
    }
    reconnectFailures = 0;
    HwMixer::restore();
    deviceChanging = false;
    deviceChanged();
    rebuildConnections();
    Logger::writeToLog ("Audio interface connected: " + out);
    addNotice ("audio", "The audio interface (" + (out.isNotEmpty() ? out : in) + ") is connected and in use.", "ok");
}

var HostEngine::getReliability()
{
    auto* o = new DynamicObject();
    o->setProperty ("realtime", player.rtOk.load());
    o->setProperty ("realtimeError", player.rtError.load());
    o->setProperty ("priority", Reliability::RtPlayer::rtPriority);
    o->setProperty ("audioCpu", player.rtCpu.load());
    o->setProperty ("isolatedCpu", Reliability::isolatedCpu());
    o->setProperty ("memoryLocked", Reliability::memLockResult.load() == 0);
    o->setProperty ("watchdog", sd.available());
    o->setProperty ("governor", File ("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor").loadFileAsString().trim());
    o->setProperty ("usbAutosuspend", File ("/sys/module/usbcore/parameters/autosuspend").loadFileAsString().trim());
    o->setProperty ("kernel", File ("/proc/sys/kernel/version").loadFileAsString().trim());
    o->setProperty ("aptTimers", File ("/etc/systemd/system/timers.target.wants/apt-daily.timer").exists()
                                  || File ("/etc/systemd/system/timers.target.wants/apt-daily-upgrade.timer").exists()
                                  || File ("/lib/systemd/system/timers.target.wants/apt-daily.timer").exists());
    const auto tuning = File ("/etc/livepi-tuning").loadFileAsString().trim();
    o->setProperty ("tuning", tuning);
    // The page can fix things itself when the installer has set up the root helper and its sudo rule
    o->setProperty ("fixAvailable", File ("/usr/local/sbin/livepi-tune").existsAsFile());
    // Applied, but the Pi has not been restarted since (boot options, or the service settings)
    bool reboot = false;
    if (tuning.contains ("boot options:"))
    {
        const auto running = File ("/proc/cmdline").loadFileAsString();
        for (auto& tok : StringArray::fromTokens (tuning.fromFirstOccurrenceOf ("boot options:", false, false).trim(), " ", ""))
            if (tok.isNotEmpty() && ! running.contains (tok))
                reboot = true;
    }
    if (File ("/etc/systemd/system/livepi.service.d/10-reliability.conf").existsAsFile() && ! sd.available())
        reboot = true;
    o->setProperty ("rebootNeeded", reboot);
    o->setProperty ("throttled", health != nullptr ? health->throttled.load() : -1);
    o->setProperty ("safeMode", safeMode);
    o->setProperty ("fallback", autoFallback);
    // Started by systemd and enabled to start at boot (the symlink "systemctl enable" creates)
    o->setProperty ("service", File ("/etc/systemd/system/livepi.service").existsAsFile());
    o->setProperty ("bootStart", File ("/etc/systemd/system/multi-user.target.wants/livepi.service").exists());
    return var (o);
}

// ---------------------------------------------------------------- chains / routing

HostEngine::Chain HostEngine::makeChain (int index)
{
    Chain c;
    c.name = "Ch " + String (index + 1);
    c.sendTap = paflMode;
    c.tap   = graph.addNode (std::make_unique<ChannelStrip> (nullptr, true));
    c.strip = graph.addNode (std::make_unique<ChannelStrip> (nullptr, false));
    if (index != kMonBus)
    {
        c.send = graph.addNode (std::make_unique<ChannelStrip> (nullptr, false));
        stripOf (c.send)->setGainDb (c.sendDb);
    }
    if (index != kMonBus && mixerMode && ! dryChannels)
        c.voice = graph.addNode (std::make_unique<VoiceChannel>());
    stripOf (c.tap)->setRing (&monitorRing);
    stripOf (c.strip)->setRing (&monitorRing);
    if (index == monitorChannel)
        stripOf (monitorPost ? c.strip : c.tap)->setMonitorOn (true);
    applyMix (c);
    return c;
}

void HostEngine::clearSlots (Chain& c)
{
    for (auto& sl : c.slots)
    {
        editors.erase (sl.node->nodeID.uid);
        cpu.erase (sl.node->nodeID.uid);
        graph.removeNode (sl.node->nodeID);
    }
    c.slots.clear();
}

void HostEngine::removeChain (Chain& c)
{
    clearSlots (c);
    if (c.voice != nullptr)
        graph.removeNode (c.voice->nodeID);
    graph.removeNode (c.tap->nodeID);
    graph.removeNode (c.strip->nodeID);
    if (c.send != nullptr)
        graph.removeNode (c.send->nodeID);
}

void HostEngine::settle (Chain& c, bool includeInput, int ms)
{
    for (auto* n : { includeInput ? c.tap.get() : nullptr, c.strip.get(), c.send.get() })
        if (n != nullptr)
            if (auto* st = dynamic_cast<ChannelStrip*> (n->getProcessor()))
                st->settle (ms);
}

void HostEngine::settleAll()
{
    for (auto& c : chains)
        settle (c, true, ChannelStrip::kSettleMs);
    settle (monBus, true, ChannelStrip::kSettleMs);
}

void HostEngine::clearChains()
{
    for (auto& c : chains)
        removeChain (c);
    chains.clear();
}

void HostEngine::applyMix (Chain& c)
{
    stripOf (c.strip)->setGainDb (c.gainDb);
    stripOf (c.strip)->setMuted (c.mute);
    if (c.send != nullptr)
        stripOf (c.send)->setGainDb (c.sendDb);
}

void HostEngine::rebuildConnections()
{
    using NC = AudioProcessorGraph::NodeAndChannel;
    using Conn = AudioProcessorGraph::Connection;
    using UK = AudioProcessorGraph::UpdateKind;

    for (auto& c : graph.getConnections())
        graph.removeConnection (c, UK::none);

    const int n = (int) chains.size();

    // Connect a chain's plugins one after another, starting from src; returns where the audio ends up
    auto connectSlots = [&] (Chain& chain, NC src)
    {
        for (auto& sl : chain.slots)
        {
            if (sl.bypassed)
                continue;

            auto* p = sl.node->getProcessor();
            for (int i = 0; i < p->getTotalNumInputChannels(); ++i)
                graph.addConnection (Conn { src, NC { sl.node->nodeID, i } }, UK::none);

            if (p->getTotalNumOutputChannels() > 0)
                src = NC { sl.node->nodeID, 0 };
        }
        return src;
    };

    for (int ch = 0; ch < n; ++ch)
    {
        auto& chain = chains[(size_t) ch];

        graph.addConnection (Conn { NC { inputNode->nodeID, ch }, NC { chain.tap->nodeID, 0 } }, UK::none);
        NC src { chain.tap->nodeID, 0 };
        if (chain.voice != nullptr)
        {
            graph.addConnection (Conn { src, NC { chain.voice->nodeID, 0 } }, UK::none);
            src = NC { chain.voice->nodeID, 0 };
        }
        else
            src = connectSlots (chain, src);

        // Send to the monitor mix: PFL (after the plugins, before the fader), the raw input, or AFL (after the fader)
        const NC sendFrom = chain.sendTap == 1 ? NC { chain.tap->nodeID, 0 }
                          : chain.sendTap == 2 ? NC { chain.strip->nodeID, 0 } : src;
        graph.addConnection (Conn { sendFrom, NC { chain.send->nodeID, 0 } }, UK::none);
        graph.addConnection (Conn { NC { chain.send->nodeID, 0 }, NC { monBus.tap->nodeID, 0 } }, UK::none);

        graph.addConnection (Conn { src, NC { chain.strip->nodeID, 0 } }, UK::none);
        graph.addConnection (Conn { NC { chain.strip->nodeID, 0 }, NC { outputNode->nodeID, ch } }, UK::none);

        // Single voice channel: feed both L and R outputs
        if (n == 1)
            graph.addConnection (Conn { NC { chain.strip->nodeID, 0 }, NC { outputNode->nodeID, 1 } }, UK::none);
    }

    // Monitor mix: sum of the sends -> its own plugins (for example a feedback eliminator) -> fader -> mapped outputs
    {
        NC src = connectSlots (monBus, NC { monBus.tap->nodeID, 0 });
        graph.addConnection (Conn { src, NC { monBus.strip->nodeID, 0 } }, UK::none);
        for (int o : monOutputs)
            graph.addConnection (Conn { NC { monBus.strip->nodeID, 0 }, NC { outputNode->nodeID, o } }, UK::none);
    }

    graph.rebuild();
}

HostEngine::Chain* HostEngine::chainFor (int channel)
{
    if (channel == kMonBus)
        return &monBus;
    if (channel < 0 || channel >= (int) chains.size())
        return nullptr;
    return &chains[(size_t) channel];
}

HostEngine::Slot* HostEngine::findSlot (int channel, int slot)
{
    auto* chain = chainFor (channel);
    if (chain == nullptr)
        return nullptr;
    auto& c = chain->slots;
    if (slot < 0 || slot >= (int) c.size())
        return nullptr;
    return &c[(size_t) slot];
}

std::unique_ptr<AudioPluginInstance> HostEngine::createInstance (const PluginDescription& d, String& error)
{
    double sr = 48000.0;
    int bs = 64;
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        sr = dev->getCurrentSampleRate();
        bs = dev->getCurrentBufferSizeSamples();
    }
    return formatManager.createPluginInstance (d, sr, bs, error);
}

// ---------------------------------------------------------------- devices

var HostEngine::getDevices()
{
    auto* root = new DynamicObject();
    Array<var> types;

    for (auto* t : deviceManager.getAvailableDeviceTypes())
    {
        t->scanForDevices();
        auto* o = new DynamicObject();
        o->setProperty ("type", t->getTypeName());
        o->setProperty ("inputs",  toVarArray (t->getDeviceNames (true)));
        o->setProperty ("outputs", toVarArray (t->getDeviceNames (false)));
        types.add (var (o));
    }
    root->setProperty ("types", types);

    auto setup = deviceManager.getAudioDeviceSetup();
    auto* cur = new DynamicObject();
    cur->setProperty ("type", deviceManager.getCurrentAudioDeviceType());
    cur->setProperty ("input", setup.inputDeviceName);
    cur->setProperty ("output", setup.outputDeviceName);
    cur->setProperty ("sampleRate", setup.sampleRate);
    cur->setProperty ("bufferSize", setup.bufferSize);
    root->setProperty ("testFile", dataDir().getChildFile ("test-input.wav").existsAsFile());

    Array<var> rates, sizes;
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        for (auto r : dev->getAvailableSampleRates()) rates.add (r);
        for (auto b : dev->getAvailableBufferSizes()) sizes.add (b);
    }
    cur->setProperty ("sampleRates", rates);
    cur->setProperty ("bufferSizes", sizes);
    root->setProperty ("current", var (cur));
    return var (root);
}

var HostEngine::setDevice (const String& type, const String& in, const String& out, double sr, int bs)
{
    deviceChanging = true;
    struct Done { HostEngine& e; ~Done() { e.deviceChanging = false; e.deviceChanged(); } } done { *this };
    if (type.isNotEmpty() && type != deviceManager.getCurrentAudioDeviceType())
        deviceManager.setCurrentAudioDeviceType (type, true);

    autoFallback = false;                       // a deliberate choice: remember it
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.inputDeviceName = in;
    setup.outputDeviceName = out;
    if (sr > 0) setup.sampleRate = sr;
    if (bs > 0) setup.bufferSize = bs;
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;

    auto e = deviceManager.setAudioDeviceSetup (setup, true);
    if (e.isNotEmpty())
        return err (e);
    rebuildConnections();
    return ok();
}

// ---------------------------------------------------------------- channels / plugins in chains

var HostEngine::setNumChannels (int n)
{
    n = jlimit (1, 16, n);
    while ((int) chains.size() > n)
    {
        removeChain (chains.back());
        chains.pop_back();
    }
    while ((int) chains.size() < n)
        chains.push_back (makeChain ((int) chains.size()));

    rebuildConnections();
    saveLast();
    return ok();
}

var HostEngine::addPlugin (int channel, const String& pluginId)
{
    if (mixerMode && channel != kMonBus)
        return err ("This Pi is in mixer mode: its channels have the LiveVoice modules built in. Switch to VST mode (Settings > Mode) to load plugins.");
    auto* chain = chainFor (channel);
    if (chain == nullptr)
        return err ("bad channel");

    auto desc = known.getTypeForIdentifierString (pluginId);
    if (desc == nullptr)
        return err ("plugin not found - rescan?");

    String e;
    auto inst = createInstance (*desc, e);
    if (inst == nullptr)
        return err ("load failed: " + e);

    Slot s;
    s.desc = *desc;
    s.node = graph.addNode (std::move (inst));
    if (s.node == nullptr)
        return err ("could not add to graph");

    chain->slots.push_back (std::move (s));
    rebuildConnections();
    settle (*chain, false, 400);   // a new plugin can start with a burst (e.g. a compressor at full makeup gain)
    saveLast();
    return ok();
}

var HostEngine::removePlugin (int channel, int slot)
{
    if (findSlot (channel, slot) == nullptr)
        return err ("no such plugin");

    auto& c = chainFor (channel)->slots;
    editors.erase (c[(size_t) slot].node->nodeID.uid);
    cpu.erase (c[(size_t) slot].node->nodeID.uid);
    graph.removeNode (c[(size_t) slot].node->nodeID);
    c.erase (c.begin() + slot);
    rebuildConnections();
    saveLast();
    return ok();
}

var HostEngine::movePlugin (int channel, int from, int to)
{
    if (findSlot (channel, from) == nullptr)
        return err ("no such plugin");
    auto& c = chainFor (channel)->slots;
    to = jlimit (0, (int) c.size() - 1, to);
    if (to == from) return ok();
    auto s = c[(size_t) from];
    c.erase (c.begin() + from);
    c.insert (c.begin() + to, s);
    rebuildConnections();
    saveLast();
    return ok();
}

var HostEngine::setBypass (int channel, int slot, bool bypassed)
{
    auto* s = findSlot (channel, slot);
    if (s == nullptr)
        return err ("no such plugin");
    s->bypassed = bypassed;
    rebuildConnections();
    saveLast();
    return ok();
}

// ---------------------------------------------------------------- mixer

var HostEngine::setMixer (const var& body)
{
    if (body.hasProperty ("paflMode"))   // global, like PAFL mode on a desk: applies to every channel
    {
        paflMode = jlimit (0, 2, (int) body["paflMode"]);
        for (auto& c : chains) c.sendTap = paflMode;
        rebuildConnections();
        markDirty();
        if (! body.hasProperty ("channel")) return ok();
    }
    const int ch = (int) body.getProperty ("channel", -1);
    auto* cp = chainFor (ch);
    if (cp == nullptr)
        return err ("bad channel");

    auto& c = *cp;
    if (body.hasProperty ("gainDb")) c.gainDb = jlimit (-100.0f, 6.0f, (float) (double) body["gainDb"]);
    if (body.hasProperty ("mute"))   c.mute = (bool) body["mute"];
    if (body.hasProperty ("sendDb") && c.send != nullptr)
        c.sendDb = jlimit (-100.0f, 6.0f, (float) (double) body["sendDb"]);
    applyMix (c);
    if (c.send != nullptr && (body.hasProperty ("sendTap") || body.hasProperty ("sendPre")))
    {
        const int t = body.hasProperty ("sendTap") ? jlimit (0, 2, (int) body["sendTap"]) : ((bool) body["sendPre"] ? 1 : 0);
        if (t != c.sendTap) { c.sendTap = t; rebuildConnections(); }
    }
    markDirty();
    return ok();
}

var HostEngine::setChannelName (int channel, const String& name)
{
    if (channel < 0 || channel >= (int) chains.size())
        return err ("bad channel");
    chains[(size_t) channel].name = name.trim().substring (0, 24);
    markDirty();
    return ok();
}

var HostEngine::getMeters()
{
    Array<var> in, out;
    for (auto& c : chains)
    {
        in.add  (Decibels::gainToDecibels (stripOf (c.tap)->takePeakIn(),    -100.0f));
        out.add (Decibels::gainToDecibels (stripOf (c.strip)->takePeakOut(), -100.0f));
    }
    auto* o = new DynamicObject();
    o->setProperty ("in", in);
    o->setProperty ("out", out);

    auto* mo = new DynamicObject();
    mo->setProperty ("in",  Decibels::gainToDecibels (stripOf (monBus.tap)->takePeakIn(),    -100.0f));
    mo->setProperty ("out", Decibels::gainToDecibels (stripOf (monBus.strip)->takePeakOut(), -100.0f));
    o->setProperty ("mon", var (mo));
    return var (o);
}

// ---------------------------------------------------------------- plugins

var HostEngine::scanPlugins()
{
    auto* fmt = formatManager.getFormat (0);
    FileSearchPath paths = fmt->getDefaultLocationsToSearch();
    paths.add (File::getSpecialLocation (File::userHomeDirectory).getChildFile (".vst3"));
    paths.add (File ("/usr/lib/vst3"));
    paths.add (File ("/usr/local/lib/vst3"));
    paths.add (appDir().getChildFile ("plugins"));

    PluginDirectoryScanner scanner (known, *fmt, paths, true,
                                    appDir().getChildFile ("scan-deadmans.txt"));
    String name;
    while (scanner.scanNextFile (true, name)) {}

    if (auto xml = known.createXml())
        xml->writeTo (appDir().getChildFile ("plugins.xml"));

    return getPlugins();
}

var HostEngine::getPlugins()
{
    Array<var> arr;
    for (auto& d : known.getTypes())
    {
        auto* o = new DynamicObject();
        o->setProperty ("id", d.createIdentifierString());
        o->setProperty ("name", d.name);
        o->setProperty ("vendor", d.manufacturerName);
        arr.add (var (o));
    }
    auto* root = new DynamicObject();
    root->setProperty ("plugins", arr);
    return var (root);
}

var HostEngine::getParams (int channel, int slot)
{
    auto* s = findSlot (channel, slot);
    if (s == nullptr)
        return err ("no such plugin");

    auto* proc = s->node->getProcessor();

    // Section names come from the plugin's own parameter groups (VST3 units), when it has them
    std::map<const AudioProcessorParameter*, String> groupOf;
    std::function<void (const AudioProcessorParameterGroup&, const String&)> walk = [&] (const AudioProcessorParameterGroup& g, const String& path)
    {
        for (auto* node : g)
        {
            if (auto* sub = node->getGroup())
                walk (*sub, path.isEmpty() ? sub->getName() : path + " / " + sub->getName());
            else if (auto* prm = node->getParameter())
                groupOf[prm] = path;
        }
    };
    walk (proc->getParameterTree(), {});

    Array<var> arr;
    auto& ps = proc->getParameters();
    for (int i = 0; i < ps.size(); ++i)
    {
        auto* p = ps[i];
        auto* o = new DynamicObject();
        const auto v = p->getValue();
        o->setProperty ("index", i);
        o->setProperty ("name", p->getName (48));
        o->setProperty ("value", v);
        o->setProperty ("default", p->getDefaultValue());
        o->setProperty ("text", paramText (*p, v));
        o->setProperty ("group", groupOf[p]);
        const int steps = p->getNumSteps();
        if (p->isBoolean() || steps == 2)
            o->setProperty ("type", "bool");
        else if (p->isDiscrete() && steps > 2 && steps <= 32)
        {
            o->setProperty ("type", "choice");
            Array<var> ch;
            for (int k = 0; k < steps; ++k)
                ch.add (p->getText ((float) k / (float) (steps - 1), 24));
            o->setProperty ("choices", ch);
        }
        else
            o->setProperty ("type", "knob");
        if (! p->isAutomatable()) o->setProperty ("hidden", true);
        arr.add (var (o));
    }
    auto* root = new DynamicObject();
    root->setProperty ("params", arr);
    return var (root);
}

var HostEngine::setParam (int channel, int slot, int index, float value)
{
    auto* s = findSlot (channel, slot);
    if (s == nullptr)
        return err ("no such plugin");

    auto& ps = s->node->getProcessor()->getParameters();
    if (index < 0 || index >= ps.size())
        return err ("bad parameter");

    auto* p = ps[index];
    p->setValueNotifyingHost (jlimit (0.0f, 1.0f, value));
    auto* o = new DynamicObject();
    o->setProperty ("ok", true);
    o->setProperty ("value", p->getValue());
    o->setProperty ("text", paramText (*p, p->getValue()));
    return var (o);
}

var HostEngine::openEditor (int channel, int slot, int viewW, int viewH)
{
    auto* s = findSlot (channel, slot);
    if (s == nullptr)
        return err ("no such plugin");

    if (std::getenv ("DISPLAY") == nullptr)
        return err ("No screen for plugin windows. Re-run the installer (without --no-editor), then reboot.");

    const auto key = s->node->nodeID.uid;

    // One plugin window at a time: they all sit in the same corner of the virtual screen
    for (auto it = editors.begin(); it != editors.end();)
        it = it->first == key ? std::next (it) : editors.erase (it);

    if (editors.find (key) == editors.end())
    {
        auto* proc = s->node->getProcessor();
        if (! proc->hasEditor())
            return err ("this plugin has no editor");

        auto* ed = proc->createEditorIfNeeded();
        if (ed == nullptr)
            return err ("could not create the plugin editor");

        editors[key] = std::make_unique<EditorWindow> (ed);
    }

    auto& w = *editors[key];
    if (w.getPeer() == nullptr)
    {
        editors.erase (key);
        return err ("The plugin window could not be shown: LivePi has no connection to its virtual screen. Restart LivePi (Setup > System).");
    }
    auto screen = Rectangle<int> (0, 0, 1920, 1080);
    if (auto* d = Desktop::getInstance().getDisplays().getPrimaryDisplay()) screen = d->totalArea;
    w.fitTo (viewW, viewH, screen.getWidth(), screen.getHeight());
    w.toFront (false);

    auto* o = new DynamicObject();
    o->setProperty ("ok", true);
    o->setProperty ("width", w.getWidth());
    o->setProperty ("height", w.getHeight());
    return var (o);
}

// Self-test for the plugin view: a small solid-colour window on the virtual screen.
// show = true opens it and returns its screen area, false closes it.
var HostEngine::screenTest (bool show)
{
    struct Swatch : Component { void paint (Graphics& g) override { g.fillAll (Colour (0xffe01070)); } };
    static std::unique_ptr<Swatch> sw;
    auto* o = new DynamicObject();
    if (! show) { sw.reset(); o->setProperty ("ok", true); return var (o); }
    sw = std::make_unique<Swatch>();
    sw->setOpaque (true);
    auto screen = Rectangle<int> (0, 0, 1920, 1080);
    if (auto* d = Desktop::getInstance().getDisplays().getPrimaryDisplay()) screen = d->totalArea;
    sw->setBounds (screen.getRight() - 80, screen.getBottom() - 60, 64, 48);   // a corner plugins don't use
    sw->addToDesktop (0);
    sw->setVisible (true);
    sw->toFront (false);
    o->setProperty ("peer", sw->getPeer() != nullptr);
    if (auto* p = sw->getPeer()) p->performAnyPendingRepaintsNow();
    const auto r = sw->getScreenBounds();
    o->setProperty ("x", r.getCentreX()); o->setProperty ("y", r.getCentreY());
    return var (o);
}

Rectangle<int> HostEngine::getEditorArea (int channel, int slot)
{
    auto* s = findSlot (channel, slot);
    if (s == nullptr) return {};
    auto it = editors.find (s->node->nodeID.uid);
    return it == editors.end() ? Rectangle<int>() : it->second->getEditorScreenArea();
}

var HostEngine::installTestFile (const File& f)
{
    AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr || r->lengthInSamples <= 0)
        return err ("Could not read that audio file. Use WAV, AIFF, FLAC or OGG.");
    if (r->lengthInSamples > (int64) (r->sampleRate * 600))
        return err ("The file is longer than 10 minutes.");

    const int n = (int) r->lengthInSamples;
    AudioBuffer<float> in ((int) jmax (1u, r->numChannels), n), mono (1, n);
    r->read (&in, 0, n, 0, true, true);
    mono.clear();
    for (int c = 0; c < in.getNumChannels(); ++c)
        mono.addFrom (0, 0, in, c, 0, n, 1.0f / (float) in.getNumChannels());

    dataDir().createDirectory();
    auto tmp = dataDir().getChildFile ("test-input.new.wav");
    tmp.deleteFile();
    bool wrote = false;
    if (auto os = tmp.createOutputStream())
    {
        WavAudioFormat wav;
        std::unique_ptr<AudioFormatWriter> w (wav.createWriterFor (os.get(), r->sampleRate, 1, 16, {}, 0));
        if (w != nullptr)
        {
            os.release();                       // the writer owns the stream now
            wrote = w->writeFromAudioSampleBuffer (mono, 0, n);
        }
    }
    if (! wrote || ! tmp.moveFileTo (dataDir().getChildFile ("test-input.wav")))
    {
        tmp.deleteFile();
        return err ("Could not save the test file (disk full?).");
    }
    return ok();
}

var HostEngine::removeTestFile()
{
    dataDir().getChildFile ("test-input.wav").deleteFile();
    return ok();
}

var HostEngine::setMonitor (int channel, bool afterPlugins)
{
    for (auto& c : chains)
    {
        stripOf (c.tap)->setMonitorOn (false);
        stripOf (c.strip)->setMonitorOn (false);
    }
    stripOf (monBus.tap)->setMonitorOn (false);
    stripOf (monBus.strip)->setMonitorOn (false);

    auto* c = chainFor (channel);
    monitorChannel = c != nullptr ? channel : -1;
    monitorPost = afterPlugins;
    if (c != nullptr)
        stripOf (afterPlugins ? c->strip : c->tap)->setMonitorOn (true);
    return ok();
}

var HostEngine::setMonitorBus (const var& body)
{
    if (body.hasProperty ("gainDb")) monBus.gainDb = jlimit (-100.0f, 6.0f, (float) (double) body["gainDb"]);
    if (body.hasProperty ("mute"))   monBus.mute = (bool) body["mute"];
    applyMix (monBus);

    if (auto* arr = body.getProperty ("outputs", {}).getArray())
    {
        std::vector<int> o;
        for (auto& v : *arr)
        {
            const int ch = (int) v;
            if (ch >= 0 && ch < 16 && std::find (o.begin(), o.end(), ch) == o.end())
                o.push_back (ch);
        }
        std::sort (o.begin(), o.end());
        if (o != monOutputs)
        {
            monOutputs = o;
            rebuildConnections();
        }
    }
    markDirty();
    return ok();
}

var HostEngine::measurePlugin (int channel, int slot)
{
    auto* s = findSlot (channel, slot);
    if (s == nullptr)
        return err ("no such plugin");
    if (benchBusy)
        return err ("Another measurement is still running.");

    if (benchThread.joinable())
        benchThread.join();                       // previous one has finished (benchBusy is false)

    String e;
    auto inst = createInstance (s->desc, e);
    if (inst == nullptr)
        return err ("load failed: " + e);

    // Same settings as the live plugin
    MemoryBlock state;
    s->node->getProcessor()->getStateInformation (state);
    if (state.getSize() > 0)
        inst->setStateInformation (state.getData(), (int) state.getSize());

    double sr = 48000.0;
    int bs = 64;
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        sr = dev->getCurrentSampleRate();
        bs = dev->getCurrentBufferSizeSamples();
    }

    const int nIn = inst->getTotalNumInputChannels(), nOut = inst->getTotalNumOutputChannels();
    inst->setPlayConfigDetails (nIn, nOut, sr, bs);
    inst->prepareToPlay (sr, bs);

    std::shared_ptr<AudioPluginInstance> shared (inst.release());
    const auto key = s->node->nodeID.uid;
    cpu[key].measuring = true;
    benchBusy = true;

    auto flag = alive;
    benchThread = std::thread ([this, shared, key, nIn, nOut, sr, bs, flag]
    {
        auto r = runCpuBench ([shared] (AudioBuffer<float>& b, MidiBuffer& m) { shared->processBlock (b, m); },
                              jmax (nIn, nOut), sr, bs);

        // Back on the message thread: store the result and destroy the test instance there
        MessageManager::callAsync ([this, shared, key, r, flag]
        {
            shared->releaseResources();
            if (! flag->load())
                return;
            auto it = cpu.find (key);
            if (it != cpu.end())
            {
                it->second.measuring = false;
                it->second.valid = true;
                it->second.avg = r.avgPct;
                it->second.peak = r.peakPct;
            }
            benchBusy = false;
        });
    });

    return ok();
}

var HostEngine::closeEditor (int channel, int slot)
{
    auto* s = findSlot (channel, slot);
    if (s == nullptr)
        return err ("no such plugin");
    editors.erase (s->node->nodeID.uid);
    return ok();
}

// ---------------------------------------------------------------- uploaded plugin management

var HostEngine::resetXruns()
{
    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        xrunDevice = dev;
        xrunBase = dev->getXRunCount();
    }
    return ok();
}

StringArray HostEngine::deviceNames()
{
    auto setup = deviceManager.getAudioDeviceSetup();
    StringArray n { setup.inputDeviceName, setup.outputDeviceName };
    if (auto* d = deviceManager.getCurrentAudioDevice()) n.add (d->getName());
    return n;
}

File HostEngine::dataDir()
{
    return appDir();
}

bool HostEngine::pluginInUse (const File& bundle)
{
    const auto path = bundle.getFullPathName();
    for (auto& c : chains)
        for (auto& s : c.slots)
            if (s.desc.fileOrIdentifier.startsWith (path))
                return true;
    for (auto& s : monBus.slots)
        if (s.desc.fileOrIdentifier.startsWith (path))
            return true;
    return false;
}

var HostEngine::installPlugins (const File& staged, const String& uploadedFileName)
{
    struct Item { File src; String name; };
    std::vector<Item> items;

    // Find .vst3 bundles anywhere in the unpacked zip (but not inside another bundle)
    for (auto& f : staged.findChildFiles (File::findDirectories, true, "*.vst3"))
    {
        bool nested = false;
        for (auto p = f.getParentDirectory(); p != staged && p != p.getParentDirectory(); p = p.getParentDirectory())
            if (p.hasFileExtension ("vst3")) { nested = true; break; }
        if (! nested)
            items.push_back ({ f, f.getFileName() });
    }

    // Zip made from inside the bundle folder: Contents/ at the root
    if (items.empty() && staged.getChildFile ("Contents").isDirectory())
    {
        auto base = File::createLegalFileName (File (uploadedFileName).getFileNameWithoutExtension());
        if (base.isEmpty()) base = "Plugin";
        items.push_back ({ staged, base + ".vst3" });
    }

    if (items.empty())
        return err ("No .vst3 plugin found in that zip. Zip the plugin's .vst3 folder itself.");

    auto dest = appDir().getChildFile ("plugins");
    dest.createDirectory();

    Array<var> installed;
    StringArray problems;

    for (auto& it : items)
    {
        auto contents = it.src.getChildFile ("Contents");
        auto arch = contents.getChildFile ("aarch64-linux");

        if (! arch.isDirectory() || arch.findChildFiles (File::findFiles, false, "*.so").isEmpty())
        {
            StringArray have;
            for (auto& d : contents.findChildFiles (File::findDirectories, false, "*"))
                have.add (d.getFileName());
            problems.add (it.name + " is not a Linux ARM64 build (it contains: "
                          + (have.isEmpty() ? String ("nothing useful") : have.joinIntoString (", "))
                          + "). It needs Contents/aarch64-linux/*.so");
            continue;
        }

        auto target = dest.getChildFile (File::createLegalFileName (it.name));

        if (target.exists())
        {
            if (pluginInUse (target))
            {
                problems.add (it.name + " is in use by a channel. Remove it from the channels first.");
                continue;
            }
            for (auto& d : known.getTypes())
                if (d.fileOrIdentifier.startsWith (target.getFullPathName()))
                    known.removeType (d);
            target.deleteRecursively();
        }

        if (! it.src.moveFileTo (target))
        {
            problems.add ("Could not install " + it.name);
            continue;
        }
        installed.add (target.getFileName());
    }

    if (installed.isEmpty())
        return err (problems.joinIntoString ("\n"));

    scanPlugins();

    auto* o = new DynamicObject();
    o->setProperty ("ok", true);
    o->setProperty ("installed", installed);
    o->setProperty ("skipped", toVarArray (problems));
    return var (o);
}

var HostEngine::deletePlugin (const String& name)
{
    if (! name.endsWithIgnoreCase (".vst3") || name.containsAnyOf ("/\\") || name.contains (".."))
        return err ("bad plugin name");

    auto target = appDir().getChildFile ("plugins").getChildFile (name);
    if (! target.isDirectory())
        return err ("no such plugin");
    if (pluginInUse (target))
        return err (name + " is in use by a channel. Remove it from the channels first.");

    for (auto& d : known.getTypes())
        if (d.fileOrIdentifier.startsWith (target.getFullPathName()))
            known.removeType (d);

    target.deleteRecursively();

    if (auto xml = known.createXml())
        xml->writeTo (appDir().getChildFile ("plugins.xml"));
    return ok();
}

var HostEngine::getInstalledPlugins()
{
    Array<var> arr;
    for (auto& d : appDir().getChildFile ("plugins").findChildFiles (File::findDirectories, false, "*.vst3"))
    {
        int64 bytes = 0;
        for (auto& f : d.findChildFiles (File::findFiles, true, "*"))
            bytes += f.getSize();

        auto* o = new DynamicObject();
        o->setProperty ("name", d.getFileName());
        o->setProperty ("mb", (double) bytes / 1048576.0);
        o->setProperty ("inUse", pluginInUse (d));
        arr.add (var (o));
    }
    auto* root = new DynamicObject();
    root->setProperty ("bundles", arr);
    return var (root);
}

var HostEngine::slotsState (const Chain& c)
{
    Array<var> slots;
    for (auto& sl : c.slots)
    {
        auto* so = new DynamicObject();
        so->setProperty ("name", sl.desc.name);
        so->setProperty ("bypassed", sl.bypassed);
        if (auto it = cpu.find (sl.node->nodeID.uid); it != cpu.end())
        {
            so->setProperty ("cpuMeasuring", it->second.measuring);
            if (it->second.valid)
            {
                so->setProperty ("cpuAvg", it->second.avg);
                so->setProperty ("cpuPeak", it->second.peak);
            }
        }
        slots.add (var (so));
    }
    return slots;
}

var HostEngine::slotsSerialised (Chain& c)
{
    Array<var> slots;
    for (auto& sl : c.slots)
    {
        auto* so = new DynamicObject();
        if (auto xml = sl.desc.createXml())
            so->setProperty ("desc", xml->toString (XmlElement::TextFormat().singleLine()));
        so->setProperty ("bypassed", sl.bypassed);

        MemoryBlock state;
        sl.node->getProcessor()->getStateInformation (state);
        so->setProperty ("state", state.toBase64Encoding());
        slots.add (var (so));
    }
    return slots;
}

void HostEngine::loadSlots (Chain& chain, const var& slotsVar, String& errors)
{
    auto* slots = slotsVar.getArray();
    if (slots == nullptr)
        return;

    for (auto& sv : *slots)
    {
        auto xml = XmlDocument::parse (sv.getProperty ("desc", {}).toString());
        PluginDescription desc;
        if (xml == nullptr || ! desc.loadFromXml (*xml))
            continue;

        String e;
        auto inst = createInstance (desc, e);
        if (inst == nullptr)
        {
            errors << desc.name << ": " << e << "\n";
            continue;
        }

        MemoryBlock state;
        if (state.fromBase64Encoding (sv.getProperty ("state", {}).toString()) && state.getSize() > 0)
            inst->setStateInformation (state.getData(), (int) state.getSize());

        Slot sl;
        sl.desc = desc;
        sl.bypassed = (bool) sv.getProperty ("bypassed", false);
        sl.node = graph.addNode (std::move (inst));
        if (sl.node != nullptr)
            chain.slots.push_back (std::move (sl));
    }
}

// ---------------------------------------------------------------- state

var HostEngine::getState()
{
    auto* o = new DynamicObject();
    o->setProperty ("numChannels", (int) chains.size());
    o->setProperty ("cpu", deviceManager.getCpuUsage());
    o->setProperty ("paflMode", paflMode);
    o->setProperty ("editorAvailable", std::getenv ("DISPLAY") != nullptr);
    o->setProperty ("monitorChannel", monitorChannel);
    o->setProperty ("monitorPost", monitorPost);
    o->setProperty ("virtualDevice", deviceManager.getCurrentAudioDeviceType() == VirtualAudio::typeName);

    // Raspberry Pi / Linux SoC temperature
    {
        auto t = File ("/sys/class/thermal/thermal_zone0/temp").loadFileAsString().trim();
        if (t.isNotEmpty() && t.containsOnly ("0123456789"))
            o->setProperty ("tempC", t.getDoubleValue() / 1000.0);
    }

    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        const double sr = dev->getCurrentSampleRate();
        o->setProperty ("deviceName", dev->getName());
        o->setProperty ("sampleRate", sr);
        o->setProperty ("bufferSize", dev->getCurrentBufferSizeSamples());
        o->setProperty ("inputChannels",  dev->getActiveInputChannels().countNumberOfSetBits());
        o->setProperty ("outputChannels", dev->getActiveOutputChannels().countNumberOfSetBits());
        if (dev != xrunDevice) { xrunDevice = dev; xrunBase = 0; }   // a newly opened device starts at 0
        if (dev->getXRunCount() < xrunBase) xrunBase = 0;
        o->setProperty ("xruns", dev->getXRunCount() - xrunBase);
        o->setProperty ("latencyMs",
                        sr > 0 ? 1000.0 * (dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples()) / sr : 0.0);
    }
    else
    {
        o->setProperty ("deviceName", "none");
    }

    o->setProperty ("safeMode", safeMode);
    o->setProperty ("mode", mixerMode ? "mixer" : "vst");
    o->setProperty ("dryChannels", dryChannels && mixerMode);
    o->setProperty ("realtime", player.rtOk.load());
    o->setProperty ("throttled", health != nullptr ? health->throttled.load() : -1);
    {
        Array<var> ns;
        for (auto& n : notices)
        {
            auto* no = new DynamicObject();
            no->setProperty ("id", n.id); no->setProperty ("text", n.text); no->setProperty ("level", n.level);
            ns.add (var (no));
        }
        o->setProperty ("notices", ns);
    }

    Array<var> chainArr;
    for (auto& c : chains)
    {
        auto* co = new DynamicObject();
        co->setProperty ("name", c.name);
        co->setProperty ("gainDb", c.gainDb);
        co->setProperty ("mute", c.mute);
        co->setProperty ("sendDb", c.sendDb);
        co->setProperty ("sendTap", c.sendTap);
        co->setProperty ("slots", slotsState (c));
        if (auto* vc = c.voice != nullptr ? dynamic_cast<VoiceChannel*> (c.voice->getProcessor()) : nullptr)
        {
            auto* vo = new DynamicObject();
            Array<var> mods;
            for (int m : vc->getOrder())
            {
                auto* mo = new DynamicObject();
                mo->setProperty ("id", String (VoiceChannel::moduleId (m)));
                mo->setProperty ("name", VoiceChannel::moduleName (m));
                mo->setProperty ("on", vc->isModuleOn (m));
                mods.add (var (mo));
            }
            vo->setProperty ("modules", mods);
            vo->setProperty ("preset", vc->getPresetName());
            vo->setProperty ("bypass", vc->apvts.getRawParameterValue ("bypass")->load() > 0.5f);
            co->setProperty ("voice", var (vo));
        }
        chainArr.add (var (co));
    }
    o->setProperty ("chains", chainArr);

    {
        auto* mb = new DynamicObject();
        mb->setProperty ("name", monBus.name);
        mb->setProperty ("gainDb", monBus.gainDb);
        mb->setProperty ("mute", monBus.mute);
        mb->setProperty ("slots", slotsState (monBus));
        Array<var> outs;
        for (int ch : monOutputs) outs.add (ch);
        mb->setProperty ("outputs", outs);
        o->setProperty ("monitorBus", var (mb));
    }

    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        Array<var> names;
        auto all = dev->getOutputChannelNames();
        auto active = dev->getActiveOutputChannels();
        for (int i = 0; i < all.size(); ++i)
            if (active[i])
                names.add (all[i]);
        o->setProperty ("outputNames", names);
    }

    Array<var> presets;
    for (auto& f : appDir().getChildFile ("presets").findChildFiles (File::findFiles, false, "*.json"))
        presets.add (f.getFileNameWithoutExtension());
    o->setProperty ("presets", presets);
    return var (o);
}

// ---------------------------------------------------------------- presets

File HostEngine::presetFile (const String& name)
{
    auto dir = appDir().getChildFile ("presets");
    dir.createDirectory();
    return dir.getChildFile (File::createLegalFileName (name) + ".json");
}

var HostEngine::serialise()
{
    auto* root = new DynamicObject();
    root->setProperty ("numChannels", (int) chains.size());
    root->setProperty ("paflMode", paflMode);
    root->setProperty ("mode", mixerMode ? "mixer" : "vst");

    Array<var> chainArr;
    for (auto& c : chains)
    {
        auto* co = new DynamicObject();
        co->setProperty ("name", c.name);
        co->setProperty ("gainDb", c.gainDb);
        co->setProperty ("mute", c.mute);
        co->setProperty ("sendDb", c.sendDb);
        co->setProperty ("sendTap", c.sendTap);
        co->setProperty ("slots", mixerMode ? c.stashSlots : slotsSerialised (c));
        if (c.voice != nullptr)
        {
            MemoryBlock mb;
            c.voice->getProcessor()->getStateInformation (mb);
            co->setProperty ("voice", mb.toBase64Encoding());
        }
        else if (! c.stashVoice.isVoid())
            co->setProperty ("voice", c.stashVoice);
        chainArr.add (var (co));
    }
    {
        auto* mb = new DynamicObject();
        mb->setProperty ("gainDb", monBus.gainDb);
        mb->setProperty ("mute", monBus.mute);
        mb->setProperty ("slots", slotsSerialised (monBus));
        Array<var> outs;
        for (int ch : monOutputs) outs.add (ch);
        mb->setProperty ("outputs", outs);
        root->setProperty ("monitorBus", var (mb));
    }
    root->setProperty ("chains", chainArr);
    return var (root);
}

var HostEngine::deserialise (const var& v)
{
    if (! v.isObject())
        return err ("bad preset");

    clearChains();

    const int n = jlimit (1, 16, (int) v.getProperty ("numChannels", 1));
    // older sessions stored the tap per channel: take the first channel's as the global mode
    {
        int pm = 0;
        if (v.hasProperty ("paflMode")) pm = (int) v["paflMode"];
        else if (auto* ca = v.getProperty ("chains", {}).getArray(); ca != nullptr && ! ca->isEmpty())
            pm = (*ca)[0].hasProperty ("sendTap") ? (int) (*ca)[0]["sendTap"] : ((bool) (*ca)[0].getProperty ("sendPre", false) ? 1 : 0);
        paflMode = jlimit (0, 2, pm);
    }
    mixerMode = v.getProperty ("mode", "vst").toString() == "mixer";
    for (int i = 0; i < n; ++i)
        chains.push_back (makeChain (i));


    String errors;
    if (auto* chainArr = v.getProperty ("chains", {}).getArray())
    {
        for (int ch = 0; ch < jmin (n, chainArr->size()); ++ch)
        {
            const var& cv = (*chainArr)[ch];
            auto& chain = chains[(size_t) ch];

            if (cv.hasProperty ("name"))   chain.name = cv["name"].toString();
            chain.gainDb = (float) (double) cv.getProperty ("gainDb", 0.0);
            chain.mute   = (bool) cv.getProperty ("mute", false);
            applyMix (chain);

            chain.sendDb = (float) (double) cv.getProperty ("sendDb", -100.0);
            chain.sendTap = paflMode;
            applyMix (chain);
            chain.stashVoice = cv.getProperty ("voice", var());
            if (mixerMode)
            {
                chain.stashSlots = cv.getProperty ("slots", var());     // kept for when VST mode comes back
                if (auto* vc = chain.voice != nullptr ? dynamic_cast<VoiceChannel*> (chain.voice->getProcessor()) : nullptr)
                {
                    MemoryBlock mb;
                    if (mb.fromBase64Encoding (chain.stashVoice.toString()) && mb.getSize() > 0)
                        vc->setStateInformation (mb.getData(), (int) mb.getSize());
                    chain.stashVoice = var();
                }
            }
            else
                loadSlots (chain, cv.getProperty ("slots", {}), errors);
        }
    }

    // Monitor mix
    clearSlots (monBus);
    monOutputs.clear();
    monBus.gainDb = 0.0f;
    monBus.mute = false;
    {
        const var mv = v.getProperty ("monitorBus", {});
        if (mv.isObject())
        {
            monBus.gainDb = (float) (double) mv.getProperty ("gainDb", 0.0);
            monBus.mute = (bool) mv.getProperty ("mute", false);
            if (auto* outs = mv.getProperty ("outputs", {}).getArray())
                for (auto& o : *outs)
                    if ((int) o >= 0 && (int) o < 16)
                        monOutputs.push_back ((int) o);
            loadSlots (monBus, mv.getProperty ("slots", {}), errors);
        }
        applyMix (monBus);
    }

    rebuildConnections();
    settleAll();   // fresh plugins and (at start-up) the converters settle before anything is heard or metered
    return errors.isEmpty() ? ok() : err (errors);
}

void HostEngine::saveLast()
{
    // In safe mode the session has no plugins: save it elsewhere so last.json keeps them
    appDir().getChildFile (safeMode ? "last-safe.json" : "last.json").replaceWithText (JSON::toString (serialise(), true));
}

void HostEngine::markDirty()
{
    if (savePending)
        return;
    savePending = true;
    Timer::callAfterDelay (2000, [this]
    {
        savePending = false;
        if (running)
            saveLast();
    });
}

var HostEngine::listPresets()
{
    auto st = getState();
    auto* o = new DynamicObject();
    o->setProperty ("presets", st.getProperty ("presets", {}));
    return var (o);
}

var HostEngine::savePreset (const String& name)
{
    if (name.trim().isEmpty())
        return err ("name required");
    if (! presetFile (name).replaceWithText (JSON::toString (serialise(), true)))
        return err ("could not write preset");
    currentPreset = name;
    return ok();
}

var HostEngine::deletePreset (const String& name)
{
    auto f = presetFile (name);            // a plain file name inside the presets folder (no paths)
    if (name.trim().isEmpty() || ! f.existsAsFile())
        return err ("no such preset");
    if (! f.deleteFile())
        return err ("could not delete the preset");
    return ok();
}

var HostEngine::loadPreset (const String& name)
{
    auto f = presetFile (name);
    if (! f.existsAsFile())
        return err ("no such preset");
    auto r = deserialise (JSON::parse (f));
    saveLast();
    currentPreset = name;
    return r;
}
