#include <juce_events/juce_events.h>
#include <atomic>
#include <csignal>
#include <memory>
#include "HostEngine.h"
#include "WebServer.h"
#include "EditorViewer.h"

namespace
{
    std::atomic<bool> stopRequested { false };
    extern "C" void onStopSignal (int) { stopRequested = true; }   // only sets a flag: safe in a signal handler

    // Turns systemd's "stop" (SIGTERM) or Ctrl-C into a normal, clean shutdown
    struct StopWatcher : juce::Timer
    {
        StopWatcher() { startTimer (200); }
        void timerCallback() override
        {
            if (stopRequested.load())
            {
                juce::Logger::writeToLog ("Stop requested (SIGTERM/SIGINT): shutting down");
                stopTimer();
                juce::JUCEApplicationBase::quit();
            }
        }
    };
}

class LivePiApp : public juce::JUCEApplicationBase
{
public:
    const juce::String getApplicationName() override    { return "LivePi"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        int port = 8080;
        auto args = getCommandLineParameterArray();
        for (int i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "--port")
                port = args[i + 1].getIntValue();

        std::signal (SIGTERM, onStopSignal);
        std::signal (SIGINT, onStopSignal);
        stopWatcher = std::make_unique<StopWatcher>();

        // Virtual screen for plugin windows (skipped with --no-editor). This must come first: it sets
        // DISPLAY, and JUCE connects to the X display only once, the first time anything needs it
        // (loading a plugin can). Done later, plugin windows would stay invisible (black).
        if (! args.contains ("--no-editor"))
            viewer = std::make_unique<EditorViewer> (HostEngine::dataDir());

        engine = std::make_unique<HostEngine>();
        engine->start();

        web = std::make_unique<WebServer> (*engine, port);
        web->setViewer (viewer.get());
        web->start();

        // Healthy for a minute: an update (if one was just installed) is accepted, no rollback needed
        juce::Timer::callAfterDelay (60000, [] { HostEngine::dataDir().getChildFile ("update-pending").deleteFile(); });

        juce::Logger::writeToLog ("LivePi running - open http://<this-pi>:" + juce::String (port));
    }

    void shutdown() override
    {
        stopWatcher.reset();
        web.reset();
        if (engine != nullptr)
            engine->shutdown();
        engine.reset();
        viewer.reset();
        juce::Logger::writeToLog ("Shutdown complete");
    }

    void anotherInstanceStarted (const juce::String&) override {}
    void systemRequestedQuit() override { quit(); }
    void suspended() override {}
    void resumed() override {}
    void unhandledException (const std::exception*, const juce::String&, int) override {}

private:
    std::unique_ptr<StopWatcher> stopWatcher;
    std::unique_ptr<HostEngine> engine;
    std::unique_ptr<WebServer> web;
    std::unique_ptr<EditorViewer> viewer;
};

START_JUCE_APPLICATION (LivePiApp)
