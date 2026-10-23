#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <map>
#include <thread>
#include <memory>
#include <vector>
#include "ChannelStrip.h"
#include "CpuBench.h"
#include "MonitorRing.h"
#include "VirtualAudio.h"

class EditorWindow;

// All public methods must be called on the JUCE message thread.
// WebServer takes care of that.
class HostEngine : private juce::ChangeListener
{
public:
    HostEngine();
    ~HostEngine() override;

    void start();
    void shutdown();

    static juce::File dataDir();   // ~/.livepi

    // Audio device
    juce::var getDevices();
    juce::var setDevice (const juce::String& type, const juce::String& in, const juce::String& out,
                         double sampleRate, int bufferSize);

    // Test file for the virtual sound card, and browser monitoring
    juce::var installTestFile (const juce::File& audioFile);
    juce::var removeTestFile();
    juce::var setMonitor (int channel, bool afterPlugins);   // browser listening tap; channel < 0 = off, kMonBus = monitor mix
    juce::var setMonitorBus (const juce::var& body);         // { gainDb?, mute?, outputs?: [0-based output channels] }

    static constexpr int kMonBus = 100;                      // "channel" number of the monitor mix
    MonitorRing& monitorTap() { return monitorRing; }

    // Channels / chains
    juce::var setNumChannels (int n);
    juce::var addPlugin (int channel, const juce::String& pluginId);
    juce::var removePlugin (int channel, int slot);
    juce::var setBypass (int channel, int slot, bool bypassed);

    // Mixer
    juce::var setMixer (const juce::var& body);          // { channel, gainDb?, mute? }
    juce::var setMasterDb (float db);
    juce::var setChannelName (int channel, const juce::String& name);
    juce::var getMeters();

    // Plugins
    juce::var scanPlugins();
    juce::var getPlugins();
    juce::var getParams (int channel, int slot);
    juce::var setParam (int channel, int slot, int index, float value);
    juce::var openEditor (int channel, int slot, int viewW = 0, int viewH = 0);   // view size = the browser's, to fill it
    juce::Rectangle<int> getEditorArea (int channel, int slot);                  // screen area of an open plugin window (empty if closed)
    juce::var closeEditor (int channel, int slot);
    juce::var measurePlugin (int channel, int slot);   // CPU estimate on a separate instance

    // Uploaded plugin management (bundles live in ~/.livepi/plugins)
    juce::var installPlugins (const juce::File& unpackedDir, const juce::String& uploadedFileName);
    juce::var deletePlugin (const juce::String& bundleName);
    juce::var getInstalledPlugins();

    // State / presets
    juce::var getState();
    juce::var listPresets();
    juce::var savePreset (const juce::String& name);
    juce::var loadPreset (const juce::String& name);

private:
    struct Slot
    {
        juce::AudioProcessorGraph::Node::Ptr node;
        juce::PluginDescription desc;
        bool bypassed = false;
    };

    struct Chain
    {
        juce::String name;
        juce::AudioProcessorGraph::Node::Ptr tap, strip, send;
        std::vector<Slot> slots;
        float gainDb = 0.0f;
        bool mute = false;
        float sendDb = -100.0f;                // level sent to the monitor mix (pre-fader); -100 = off
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void rebuildConnections();
    Chain makeChain (int index);
    void removeChain (Chain&);
    void clearChains();
    void applyMix (Chain&);
    Slot* findSlot (int channel, int slot);
    Chain* chainFor (int channel);                      // a channel, or the monitor mix (kMonBus)
    void clearSlots (Chain&);
    juce::var slotsState (const Chain&);
    juce::var slotsSerialised (Chain&);
    void loadSlots (Chain&, const juce::var& slotsVar, juce::String& errors);
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription&, juce::String& error);
    juce::var serialise();
    juce::var deserialise (const juce::var&);
    void saveLast();
    void markDirty();                       // save last.json shortly after fader/name changes
    bool pluginInUse (const juce::File& bundle);
    juce::File presetFile (const juce::String& name);

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList known;

    std::atomic<float> masterGain { 1.0f };   // must outlive the graph
    MonitorRing monitorRing;                  // must outlive the graph
    int monitorChannel = -1;
    bool monitorPost = true;
    bool autoFallback = false;                // virtual card chosen automatically: do not remember it
    float masterDb = 0.0f;

    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;
    juce::AudioProcessorGraph::Node::Ptr inputNode, outputNode;

    std::vector<Chain> chains;
    Chain monBus;                                       // monitor mix: sends -> plugins -> fader -> mapped outputs
    std::vector<int> monOutputs;                        // 0-based device output channels
    std::map<juce::uint32, std::unique_ptr<EditorWindow>> editors;
    bool running = false;
    bool savePending = false;

    struct CpuInfo { double avg = 0.0, peak = 0.0; bool measuring = false, valid = false; };
    std::map<juce::uint32, CpuInfo> cpu;                 // keyed by graph node id
    std::thread benchThread;
    bool benchBusy = false;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
};
