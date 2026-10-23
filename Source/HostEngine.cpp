#include "HostEngine.h"
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

    std::unique_ptr<XmlElement> saved = XmlDocument::parse (appDir().getChildFile ("audio.xml"));
    auto e = deviceManager.initialise (16, 16, saved.get(), true);
    if (e.isNotEmpty())
        Logger::writeToLog ("Audio init: " + e);

    // No usable sound card (or none plugged in): run on the virtual one so the page still works
    if (deviceManager.getCurrentAudioDevice() == nullptr)
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

    deviceManager.addAudioCallback (&player);
    deviceManager.addChangeListener (this);
    running = true;

    auto last = appDir().getChildFile ("last.json");
    if (last.existsAsFile())
        deserialise (JSON::parse (last));

    rebuildConnections();
}

void HostEngine::shutdown()
{
    if (! running)
        return;
    running = false;
    *alive = false;
    if (benchThread.joinable())
        benchThread.join();
    saveLast();
    editors.clear();
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (&player);
    player.setProcessor (nullptr);
    deviceManager.closeAudioDevice();
}

void HostEngine::changeListenerCallback (ChangeBroadcaster*)
{
    if (! autoFallback)
      if (auto xml = deviceManager.createStateXml())
        xml->writeTo (appDir().getChildFile ("audio.xml"));
    rebuildConnections();
}

// ---------------------------------------------------------------- chains / routing

HostEngine::Chain HostEngine::makeChain (int index)
{
    Chain c;
    c.name = "Ch " + String (index + 1);
    c.tap   = graph.addNode (std::make_unique<ChannelStrip> (nullptr, true));
    c.strip = graph.addNode (std::make_unique<ChannelStrip> (&masterGain, false));
    if (index != kMonBus)
    {
        c.send = graph.addNode (std::make_unique<ChannelStrip> (nullptr, false));
        stripOf (c.send)->setGainDb (c.sendDb);
    }
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
    graph.removeNode (c.tap->nodeID);
    graph.removeNode (c.strip->nodeID);
    if (c.send != nullptr)
        graph.removeNode (c.send->nodeID);
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
        NC src = connectSlots (chain, NC { chain.tap->nodeID, 0 });

        // Pre-fader send to the monitor mix, tapped before or after the plugins
        graph.addConnection (Conn { chain.sendPre ? NC { chain.tap->nodeID, 0 } : src, NC { chain.send->nodeID, 0 } }, UK::none);
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
    if (body.hasProperty ("sendPre") && c.send != nullptr && (bool) body["sendPre"] != c.sendPre)
    {
        c.sendPre = (bool) body["sendPre"];
        rebuildConnections();
    }
    markDirty();
    return ok();
}

var HostEngine::setMasterDb (float db)
{
    masterDb = jlimit (-100.0f, 6.0f, db);
    masterGain.store (masterDb <= -60.0f ? 0.0f : Decibels::decibelsToGain (masterDb));
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
    o->setProperty ("masterDb", masterDb);
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
        o->setProperty ("xruns", dev->getXRunCount());
        o->setProperty ("latencyMs",
                        sr > 0 ? 1000.0 * (dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples()) / sr : 0.0);
    }
    else
    {
        o->setProperty ("deviceName", "none");
    }

    Array<var> chainArr;
    for (auto& c : chains)
    {
        auto* co = new DynamicObject();
        co->setProperty ("name", c.name);
        co->setProperty ("gainDb", c.gainDb);
        co->setProperty ("mute", c.mute);
        co->setProperty ("sendDb", c.sendDb);
        co->setProperty ("sendPre", c.sendPre);
        co->setProperty ("slots", slotsState (c));
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
    root->setProperty ("masterDb", masterDb);

    Array<var> chainArr;
    for (auto& c : chains)
    {
        auto* co = new DynamicObject();
        co->setProperty ("name", c.name);
        co->setProperty ("gainDb", c.gainDb);
        co->setProperty ("mute", c.mute);
        co->setProperty ("sendDb", c.sendDb);
        co->setProperty ("sendPre", c.sendPre);
        co->setProperty ("slots", slotsSerialised (c));
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
    for (int i = 0; i < n; ++i)
        chains.push_back (makeChain (i));

    setMasterDb ((float) (double) v.getProperty ("masterDb", 0.0));

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
            chain.sendPre = (bool) cv.getProperty ("sendPre", false);
            applyMix (chain);
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
    return errors.isEmpty() ? ok() : err (errors);
}

void HostEngine::saveLast()
{
    appDir().getChildFile ("last.json").replaceWithText (JSON::toString (serialise(), true));
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
    return ok();
}

var HostEngine::loadPreset (const String& name)
{
    auto f = presetFile (name);
    if (! f.existsAsFile())
        return err ("no such preset");
    auto r = deserialise (JSON::parse (f));
    saveLast();
    return r;
}
