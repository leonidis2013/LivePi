#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include <mutex>
#include "Updater.h"

// Checks GitHub Releases for a newer LivePi build and installs it.
// Uses the system `curl` for HTTPS so LivePi itself needs no TLS library.
class OnlineUpdate : private juce::Thread
{
public:
    struct Config
    {
        juce::String repo;                 // "owner/name"
        juce::String currentBuild;         // "7", or "dev" (internal, always increases)
        juce::String currentVersion;       // "0.5.0" (what people see)
        juce::String curl = "curl";
        juce::String apiBase = "https://api.github.com";
        juce::File dataDir, installDir;
        juce::String exeName;
        std::function<bool()> isOnline;
        std::function<void()> onInstalled; // called after a successful install (restart the app)
    };

    explicit OnlineUpdate (Config c) : juce::Thread ("online-update"), cfg (std::move (c))
    {
        startThread();
    }

    ~OnlineUpdate() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (15000);
    }

    void requestCheck()   { post (Job::check); }
    void requestInstall() { post (Job::install); }

    juce::var status()
    {
        // First look: check once when someone asks and we are online
        bool needCheck = false;
        {
            std::lock_guard<std::mutex> l (m);
            const auto age = juce::Time::currentTimeMillis() - lastChecked;
            needCheck = state == "idle" && (lastChecked == 0 || age > 3600 * 1000);
        }
        if (needCheck && cfg.isOnline && cfg.isOnline())
            requestCheck();

        std::lock_guard<std::mutex> l (m);
        auto* o = new juce::DynamicObject();
        o->setProperty ("state", state);
        o->setProperty ("message", message);
        o->setProperty ("current", cfg.currentBuild);
        o->setProperty ("latest", latestBuild > 0 ? juce::String (latestBuild) : juce::String());
        o->setProperty ("currentVersion", "v" + cfg.currentVersion);
        o->setProperty ("latestVersion", latestVersion);
        o->setProperty ("available", available);
        o->setProperty ("lastChecked", (juce::int64) (lastChecked / 1000));
        o->setProperty ("repo", cfg.repo);
        return juce::var (o);
    }

private:
    enum class Job { none, check, install };

    void post (Job j)
    {
        {
            std::lock_guard<std::mutex> l (m);
            if (state == "checking" || state == "downloading" || state == "installing")
                return;                                    // already busy
            pending = j;
            state = j == Job::check ? "checking" : "downloading";
            message = {};
        }
        notify();
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            Job j;
            {
                std::lock_guard<std::mutex> l (m);
                j = pending;
                pending = Job::none;
            }
            if (j == Job::none)
            {
                wait (-1);
                continue;
            }

            juce::String err;
            bool ok = checkLatest (err);
            if (ok && j == Job::install)
                ok = install (err);

            std::lock_guard<std::mutex> l (m);
            if (! ok)       { state = "error"; message = err; }
            else if (j == Job::check) { state = "idle"; message = {}; }
            // install success leaves state = "ok" (set in install())
        }
    }

    // exit code of curl, -1 = could not start, -2 = timed out / cancelled
    int runCurl (const juce::StringArray& args, int timeoutSec)
    {
        juce::StringArray cmd;
        cmd.add (cfg.curl);
        cmd.addArray (args);

        juce::ChildProcess p;
        if (! p.start (cmd, 0))
            return -1;

        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutSec * 1000u;
        while (p.isRunning())
        {
            if (threadShouldExit() || juce::Time::getMillisecondCounter() > end)
            {
                p.kill();
                return -2;
            }
            juce::Thread::sleep (100);
        }
        return (int) p.getExitCode();
    }

    static juce::String curlProblem (int code)
    {
        if (code == -1) return "curl is not installed on this Pi. Run: sudo apt-get install curl";
        if (code == -2) return "The connection to GitHub timed out.";
        if (code == 22) return "GitHub has no release for this project yet, or is limiting requests. Try again later.";
        return "Could not reach GitHub (curl error " + juce::String (code) + ").";
    }

    bool checkLatest (juce::String& err)
    {
        if (cfg.isOnline && ! cfg.isOnline())
        {
            err = "No internet access";
            return false;
        }

        auto json = cfg.dataDir.getChildFile ("latest-release.json");
        json.deleteFile();

        const int code = runCurl ({ "-fsSL", "--connect-timeout", "10", "--max-time", "30",
                                    "-H", "Accept: application/vnd.github+json", "-H", "User-Agent: LivePi",
                                    "-o", json.getFullPathName(),
                                    cfg.apiBase + "/repos/" + cfg.repo + "/releases/latest" }, 45);
        if (code != 0)
        {
            err = curlProblem (code);
            return false;
        }

        auto v = juce::JSON::parse (json);
        json.deleteFile();

        const auto tag = v.getProperty ("tag_name", {}).toString();
        if (! tag.startsWith ("build-") || ! tag.substring (6).containsOnly ("0123456789"))
        {
            err = "The latest GitHub release has an unexpected name (" + tag + ").";
            return false;
        }
        const int latest = tag.substring (6).getIntValue();

        juce::String url;
        juce::int64 size = 0;
        const juce::String wanted = "livepi-build-" + juce::String (latest) + ".zip";
        if (auto* assets = v.getProperty ("assets", {}).getArray())
            for (auto& a : *assets)
                if (a.getProperty ("name", {}).toString() == wanted)
                {
                    url = a.getProperty ("browser_download_url", {}).toString();
                    size = (juce::int64) a.getProperty ("size", 0);
                }

        // Only ever download from this project's own releases
        if (! url.startsWith ("https://github.com/" + cfg.repo + "/releases/download/"))
        {
            err = "The latest release has no " + wanted + " to download.";
            return false;
        }

        const bool numeric = cfg.currentBuild.isNotEmpty() && cfg.currentBuild.containsOnly ("0123456789");
        // the release title carries the version people see: "LivePi v0.5.1"
        juce::String shown;
        {
            const auto name = v.getProperty ("name", {}).toString();
            const int i = name.indexOfChar ('v');
            for (int k = name.length(); --k >= 0;)
                if (name[k] == 'v' && k + 1 < name.length() && juce::CharacterFunctions::isDigit (name[k + 1])) { shown = name.substring (k).upToFirstOccurrenceOf (" ", false, false); break; }
            juce::ignoreUnused (i);
        }
        std::lock_guard<std::mutex> l (m);
        latestBuild = latest;
        latestVersion = shown.isNotEmpty() ? shown : "build " + juce::String (latest);
        assetUrl = url;
        assetSize = size;
        available = numeric && latest > cfg.currentBuild.getIntValue();
        lastChecked = juce::Time::currentTimeMillis();
        if (! numeric)
            message = "This is a development version, so it will not update itself.";
        return true;
    }

    bool install (juce::String& err)
    {
        juce::String url;
        juce::int64 size;
        int latest;
        {
            std::lock_guard<std::mutex> l (m);
            if (! available)
            {
                err = "Already up to date.";
                return false;
            }
            url = assetUrl;
            size = assetSize;
            latest = latestBuild;
        }

        auto staging = cfg.dataDir.getChildFile ("staging").getChildFile (juce::Uuid().toString());
        staging.createDirectory();
        auto zip = staging.getChildFile ("update.zip");

        const int code = runCurl ({ "-fsSL", "--connect-timeout", "10", "--max-time", "900",
                                    "-H", "User-Agent: LivePi", "-o", zip.getFullPathName(), url }, 920);
        if (code != 0)
        {
            staging.deleteRecursively();
            err = curlProblem (code);
            return false;
        }
        if (size > 0 && zip.getSize() != size)
        {
            staging.deleteRecursively();
            err = "The download was incomplete. Try again.";
            return false;
        }

        { std::lock_guard<std::mutex> l (m); state = "installing"; }

        auto unpacked = staging.getChildFile ("unpacked");
        err = Updater::extractZipSafely (zip, unpacked);
        juce::String version;
        if (err.isEmpty())
            err = Updater::apply (unpacked, cfg.installDir, cfg.exeName, cfg.dataDir, version);
        staging.deleteRecursively();
        if (err.isNotEmpty())
            return false;

        {
            std::lock_guard<std::mutex> l (m);
            state = "ok";
            message = "Installed " + (version.isNotEmpty() ? version : latestVersion) + ". Restarting…";
        }
        if (cfg.onInstalled)
            cfg.onInstalled();
        return true;
    }

    Config cfg;
    std::mutex m;
    Job pending = Job::none;
    juce::String state = "idle", message, assetUrl;
    int latestBuild = 0;
    juce::String latestVersion;
    juce::int64 assetSize = 0, lastChecked = 0;
    bool available = false;
};
