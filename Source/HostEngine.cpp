#include "HostEngine.h"
#include <cstdlib>

using namespace juce;

namespace
{
    File appDir()
    {
        auto d = File::getSpecialLocation (File::userHomeDirectory).getChildFile (".livehost");
        d.createDirectory();
        return d;
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

// Plugin editor shown on the (virtual) display so it can be streamed to the browser.
class EditorWindow : public DocumentWindow
{
public:
    EditorWindow (AudioProcessorEditor* ed, const String& title, std::function<void()> onCloseFn)
        : DocumentWindow (title, Colours::darkgrey, DocumentWindow::closeButton),
          onClose (std::move (onCloseFn))
    {
        setUsingNativeTitleBar (false);
        const bool resizable = ed->isResizable();
        setContentOwned (ed, true);
        setResizable (resizable, false);
        setTopLeftPosition (20, 20);
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        MessageManager::callAsync (onClose);
    }

private:
    std::function<void()> onClose;
};

HostEngine::HostEngine()
{
    formatManager.addFormat (new VST3PluginFormat());

    using IO = AudioProcessorGraph::AudioGraphIOProcessor;
    inputNode  = graph.addNode (std::make_unique<IO> (IO::audioInputNode));
    outputNode = graph.addNode (std::make_unique<IO> (IO::audioOutputNode));

    chains.push_back (makeChain (0));
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
    std::unique_ptr<XmlElement> saved = XmlDocument::parse (appDir().getChildFile ("audio.xml"));
    auto e = deviceManager.initialise (16, 16, saved.get(), true);
    if (e.isNotEmpty())
        Logger::writeToLog ("Audio init: " + e);

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
    applyMix (c);
    return c;
}

void HostEngine::removeChain (Chain& c)
{
    for (auto& s : c.slots)
    {
        editors.erase (s.node->nodeID.uid);
        cpu.erase (s.node->nodeID.uid);
        graph.removeNode (s.node->nodeID);
    }
    c.slots.clear();
    graph.removeNode (c.tap->nodeID);
    graph.removeNode (c.strip->nodeID);
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
}

void HostEngine::rebuildConnections()
{
    using NC = AudioProcessorGraph::NodeAndChannel;
    using Conn = AudioProcessorGraph::Connection;
    using UK = AudioProcessorGraph::UpdateKind;

    for (auto& c : graph.getConnections())
        graph.removeConnection (c, UK::none);

    const int n = (int) chains.size();

    for (int ch = 0; ch < n; ++ch)
    {
        auto& chain = chains[(size_t) ch];

        graph.addConnection (Conn { NC { inputNode->nodeID, ch }, NC { chain.tap->nodeID, 0 } }, UK::none);
        NC src { chain.tap->nodeID, 0 };

        for (auto& s : chain.slots)
        {
            if (s.bypassed)
                continue;

            auto* p = s.node->getProcessor();
            for (int i = 0; i < p->getTotalNumInputChannels(); ++i)
                graph.addConnection (Conn { src, NC { s.node->nodeID, i } }, UK::none);

            if (p->getTotalNumOutputChannels() > 0)
                src = NC { s.node->nodeID, 0 };
        }

        graph.addConnection (Conn { src, NC { chain.strip->nodeID, 0 } }, UK::none);
        graph.addConnection (Conn { NC { chain.strip->nodeID, 0 }, NC { outputNode->nodeID, ch } }, UK::none);

        // Single voice channel: feed both L and R outputs
        if (n == 1)
            graph.addConnection (Conn { NC { chain.strip->nodeID, 0 }, NC { outputNode->nodeID, 1 } }, UK::none);
    }

    graph.rebuild();
}

HostEngine::Slot* HostEngine::findSlot (int channel, int slot)
{
    if (channel < 0 || channel >= (int) chains.size())
        return nullptr;
    auto& c = chains[(size_t) channel].slots;
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
    if (channel < 0 || channel >= (int) chains.size())
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

    chains[(size_t) channel].slots.push_back (std::move (s));
    rebuildConnections();
    saveLast();
    return ok();
}

var HostEngine::removePlugin (int channel, int slot)
{
    if (findSlot (channel, slot) == nullptr)
        return err ("no such plugin");

    auto& c = chains[(size_t) channel].slots;
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
    if (ch < 0 || ch >= (int) chains.size())
        return err ("bad channel");

    auto& c = chains[(size_t) ch];
    if (body.hasProperty ("gainDb")) c.gainDb = jlimit (-100.0f, 6.0f, (float) (double) body["gainDb"]);
    if (body.hasProperty ("mute"))   c.mute = (bool) body["mute"];
    applyMix (c);
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

    Array<var> arr;
    auto& ps = s->node->getProcessor()->getParameters();
    for (int i = 0; i < ps.size(); ++i)
    {
        auto* p = ps[i];
        auto* o = new DynamicObject();
        o->setProperty ("index", i);
        o->setProperty ("name", p->getName (48));
        o->setProperty ("value", p->getValue());
        o->setProperty ("text", (p->getText (p->getValue(), 24) + " " + p->getLabel()).trim());
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

    ps[index]->setValueNotifyingHost (jlimit (0.0f, 1.0f, value));
    return ok();
}

var HostEngine::openEditor (int channel, int slot)
{
    auto* s = findSlot (channel, slot);
    if (s == nullptr)
        return err ("no such plugin");

    if (std::getenv ("DISPLAY") == nullptr)
        return err ("No display for plugin windows. Re-run the installer with the editor option, then reboot.");

    const auto key = s->node->nodeID.uid;
    auto it = editors.find (key);
    if (it != editors.end())
    {
        it->second->toFront (true);
        return ok();
    }

    auto* proc = s->node->getProcessor();
    if (! proc->hasEditor())
        return err ("this plugin has no editor");

    auto* ed = proc->createEditorIfNeeded();
    if (ed == nullptr)
        return err ("could not create the plugin editor");

    editors[key] = std::make_unique<EditorWindow> (ed, s->desc.name, [this, key] { editors.erase (key); });
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

// ---------------------------------------------------------------- state

var HostEngine::getState()
{
    auto* o = new DynamicObject();
    o->setProperty ("numChannels", (int) chains.size());
    o->setProperty ("cpu", deviceManager.getCpuUsage());
    o->setProperty ("masterDb", masterDb);
    o->setProperty ("editorAvailable", std::getenv ("DISPLAY") != nullptr);

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
        Array<var> slots;
        for (auto& s : c.slots)
        {
            auto* so = new DynamicObject();
            so->setProperty ("name", s.desc.name);
            so->setProperty ("bypassed", s.bypassed);
            if (auto it = cpu.find (s.node->nodeID.uid); it != cpu.end())
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
        auto* co = new DynamicObject();
        co->setProperty ("name", c.name);
        co->setProperty ("gainDb", c.gainDb);
        co->setProperty ("mute", c.mute);
        co->setProperty ("slots", slots);
        chainArr.add (var (co));
    }
    o->setProperty ("chains", chainArr);

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
        Array<var> slots;
        for (auto& s : c.slots)
        {
            auto* so = new DynamicObject();
            if (auto xml = s.desc.createXml())
                so->setProperty ("desc", xml->toString (XmlElement::TextFormat().singleLine()));
            so->setProperty ("bypassed", s.bypassed);

            MemoryBlock state;
            s.node->getProcessor()->getStateInformation (state);
            so->setProperty ("state", state.toBase64Encoding());
            slots.add (var (so));
        }
        auto* co = new DynamicObject();
        co->setProperty ("name", c.name);
        co->setProperty ("gainDb", c.gainDb);
        co->setProperty ("mute", c.mute);
        co->setProperty ("slots", slots);
        chainArr.add (var (co));
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

            auto* slots = cv.getProperty ("slots", {}).getArray();
            if (slots == nullptr)
                continue;

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

                Slot s;
                s.desc = desc;
                s.bypassed = (bool) sv.getProperty ("bypassed", false);
                s.node = graph.addNode (std::move (inst));
                if (s.node != nullptr)
                    chain.slots.push_back (std::move (s));
            }
        }
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
