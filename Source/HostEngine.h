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
#include "PisoundButton.h"
#include "Reliability.h"
#include "VoiceChannel.h"
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
    juce::StringArray deviceNames();   // the audio device in use (message thread)
    juce::var resetXruns();            // start the dropout count again from 0

    // ---- mixer mode (built-in LiveVoice channels) ----
    bool isMixerMode() const { return mixerMode; }
    juce::var setMode (const juce::String& mode);   // "vst" or "mixer"
    juce::var voiceView (int channel, bool full = true);
    juce::var voiceSpectrum (int channel, bool output, const juce::String& module);
    juce::var voiceParam (int channel, const juce::String& id, double value, bool normalised = false);
    juce::var voiceOrder (int channel, const juce::var& order);
    juce::var voicePreset (int channel, int index);
    juce::var voiceNotch (int channel, const juce::String& cmd, int slot);
    juce::var voiceCopy (int from, int to);
    juce::var getReliability();        // the checklist on the System page
    juce::var restoreSession();        // leave safe mode: load the session with its plugins again
    juce::var dismissNotice (const juce::String& id);
    juce::var getButton();                         // the Pisound button: status and what each press does
    juce::var setButton (const juce::var& body);   // { enabled?, actions?: { click1: { action, channel }, ... } }

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
    juce::var movePlugin (int channel, int from, int to);   // reorder a channel's plugins

    // Mixer
    juce::var setMixer (const juce::var& body);          // { channel, gainDb?, mute? }
    juce::var setChannelName (int channel, const juce::String& name);
    juce::var getMeters();

    // Plugins
    juce::var scanPlugins();
    juce::var getPlugins();
    juce::var getParams (int channel, int slot);
    juce::var setParam (int channel, int slot, int index, float value);
    juce::var openEditor (int channel, int slot, int viewW = 0, int viewH = 0);   // view size = the browser's, to fill it
    juce::Rectangle<int> getEditorArea (int channel, int slot);
    juce::var screenTest (bool show);                  // screen area of an open plugin window (empty if closed)
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
    juce::var deletePreset (const juce::String& name);

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
        juce::AudioProcessorGraph::Node::Ptr voice; // mixer mode: the built-in LiveVoice channel
        juce::var stashSlots, stashVoice;      // the other mode's settings, kept untouched while unused
        int sendTap = 0;                       // monitor tap point: 0 = PFL (after plugins, pre-fader), 1 = input (before plugins), 2 = AFL (after fader)
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void rebuildConnections();
    Chain makeChain (int index);
    void removeChain (Chain&);
    void clearChains();
    void applyMix (Chain&);
    void settle (Chain&, bool includeInput, int ms);   // soft start, see ChannelStrip::settle
    void settleAll();
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

    MonitorRing monitorRing;                  // must outlive the graph
    int monitorChannel = -1;
    bool monitorPost = true;
    bool autoFallback = false;                // virtual card chosen automatically: do not remember it
    int paflMode = 0;                                   // global PAFL tap for every channel: 0 = PFL, 1 = input, 2 = AFL

    juce::AudioProcessorGraph graph;
    Reliability::RtPlayer player;
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
    int xrunBase = 0;                    // device xrun count at the last reset
    juce::AudioIODevice* xrunDevice = nullptr;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    // ---- reliability (see Reliability.h)
    Reliability::SdNotify sd;
    std::unique_ptr<Reliability::PiHealth> health;
    std::thread watchdog;
    std::atomic<bool> watchdogStop { false };
    std::atomic<bool> deviceOpen { false }, deviceChanging { false };
    std::atomic<double> lastDeviceChangeMs { 0.0 };
    std::atomic<double> startDeadlineMs { 0.0 }, messageBeatMs { 0.0 };
    std::atomic<bool> startDone { false };
    std::unique_ptr<juce::Timer> messageBeat;
    int reconnectFailures = 0;
    double reconnectNotBeforeMs = 0.0;
    bool safeMode = false;
    bool mixerMode = false;
    bool dryChannels = false;            // safe mode in mixer mode: channels without processing
    VoiceChannel* voiceOf (int channel);
    std::unique_ptr<juce::Timer> reconnectTimer;
    struct Notice { juce::String id, text, level; };
    std::vector<Notice> notices;                         // shown as banners on the page (message thread)

    void runWatchdog();
    [[noreturn]] void die (const juce::String& reason);
    void restartAudio (juce::uint64 heartbeatWhenStalled);
    int recentWatchdogRestarts();
    void tryReconnect();
    void useVirtualFallback();
    void deviceChanged();                               // message thread, after any device change
    void addNotice (const juce::String& id, const juce::String& text, const juce::String& level = "warn");
    static juce::var withoutPlugins (const juce::var& session);

    // ---- Pisound button (see PisoundButton.h)
    PisoundButton button;
    juce::var buttonCfg;                                 // ~/.livepi/button.json
    juce::String lastPress, currentPreset;
    juce::int64 lastPressAt = 0;
    void runButton (int gesture);                        // message thread
    juce::String buttonAction (const juce::String& action, int channel);   // does it; returns what happened
};
