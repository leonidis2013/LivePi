#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <map>
#include <memory>
#include <vector>
#include "ChannelStrip.h"

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

    static juce::File dataDir();   // ~/.livehost

    // Audio device
    juce::var getDevices();
    juce::var setDevice (const juce::String& type, const juce::String& in, const juce::String& out,
                         double sampleRate, int bufferSize);

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
    juce::var openEditor (int channel, int slot);
    juce::var closeEditor (int channel, int slot);

    // Uploaded plugin management (bundles live in ~/.livehost/plugins)
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
        juce::AudioProcessorGraph::Node::Ptr tap, strip;
        std::vector<Slot> slots;
        float gainDb = 0.0f;
        bool mute = false;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void rebuildConnections();
    Chain makeChain (int index);
    void removeChain (Chain&);
    void clearChains();
    void applyMix (Chain&);
    Slot* findSlot (int channel, int slot);
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
    float masterDb = 0.0f;

    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;
    juce::AudioProcessorGraph::Node::Ptr inputNode, outputNode;

    std::vector<Chain> chains;
    std::map<juce::uint32, std::unique_ptr<EditorWindow>> editors;
    bool running = false;
    bool savePending = false;
};
