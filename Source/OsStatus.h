#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <utility>
#include <vector>

// Helpers for the "Operating system" card: is the Pi online, and what is the apt job doing.
// The apt work itself is done by the root helper scripts (scripts/livepi-os*), which write
// their progress to /var/lib/livepi-os so it survives a LivePi restart.

// Checks every few seconds whether the Pi can reach its package servers.
class InternetMonitor : private juce::Thread
{
public:
    using Target = std::pair<juce::String, int>;

    explicit InternetMonitor (std::vector<Target> t, int intervalMs = 20000, int connectTimeoutMs = 3000)
        : juce::Thread ("internet-check"), targets (std::move (t)), interval (intervalMs), timeout (connectTimeoutMs)
    {
        startThread();
    }

    ~InternetMonitor() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (timeout * (int) targets.size() + 3000);
    }

    bool isKnown() const  { return known.load(); }
    bool isOnline() const { return online.load(); }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            bool ok = false;
            for (auto& t : targets)
            {
                juce::StreamingSocket s;
                if (s.connect (t.first, t.second, timeout))
                {
                    ok = true;
                    s.close();
                    break;
                }
                if (threadShouldExit())
                    return;
            }
            online = ok;
            known = true;
            wait (interval);
        }
    }

    std::vector<Target> targets;
    int interval, timeout;
    std::atomic<bool> online { false }, known { false };
};

namespace OsStatus
{
    inline const char* const helperPath = "/usr/local/sbin/livepi-os";
    inline const char* const stateDir   = "/var/lib/livepi-os";

    inline juce::var read (const juce::File& dir, bool available, bool known, bool online)
    {
        using namespace juce;

        auto txt = [&] (const char* name) { return dir.getChildFile (name).loadFileAsString().trim(); };

        auto state = txt ("state");
        if (state.isEmpty())
            state = "idle";

        // A job that stopped writing for 15 minutes was interrupted (for example by a power cut)
        auto logFile = dir.getChildFile ("log");
        if (state == "running" && logFile.existsAsFile()
            && (Time::getCurrentTime() - logFile.getLastModificationTime()).inMinutes() > 15)
            state = "failed";

        String log;
        if (logFile.existsAsFile())
        {
            FileInputStream in (logFile);
            if (in.openedOk())
            {
                in.setPosition (jmax ((int64) 0, logFile.getSize() - 4000));
                log = in.readEntireStreamAsString();
            }
        }

        auto up = txt ("upgradable");

        auto* o = new DynamicObject();
        o->setProperty ("available", available);
        o->setProperty ("known", known);
        o->setProperty ("online", online);
        o->setProperty ("state", state);
        o->setProperty ("verb", txt ("verb"));
        o->setProperty ("upgradable", up.isEmpty() ? -1 : up.getIntValue());
        o->setProperty ("rebootRecommended", txt ("reboot") == "yes");
        o->setProperty ("finished", (int64) txt ("finished").getLargeIntValue());
        o->setProperty ("log", log);
        return var (o);
    }
}
