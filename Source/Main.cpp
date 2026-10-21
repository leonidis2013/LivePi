#include <juce_events/juce_events.h>
#include <memory>
#include "HostEngine.h"
#include "WebServer.h"

class LiveHostApp : public juce::JUCEApplicationBase
{
public:
    const juce::String getApplicationName() override    { return "LiveHost"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        int port = 8080;
        auto args = getCommandLineParameterArray();
        for (int i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "--port")
                port = args[i + 1].getIntValue();

        engine = std::make_unique<HostEngine>();
        engine->start();

        web = std::make_unique<WebServer> (*engine, port);
        web->start();

        juce::Logger::writeToLog ("LiveHost running - open http://<this-pi>:" + juce::String (port));
    }

    void shutdown() override
    {
        web.reset();
        if (engine != nullptr)
            engine->shutdown();
        engine.reset();
    }

    void anotherInstanceStarted (const juce::String&) override {}
    void systemRequestedQuit() override { quit(); }
    void suspended() override {}
    void resumed() override {}
    void unhandledException (const std::exception*, const juce::String&, int) override {}

private:
    std::unique_ptr<HostEngine> engine;
    std::unique_ptr<WebServer> web;
};

START_JUCE_APPLICATION (LiveHostApp)
