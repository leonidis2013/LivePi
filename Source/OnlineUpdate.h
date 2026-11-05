#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include <mutex>
#include <vector>
#include <algorithm>
#include "Updater.h"

// Checks GitHub Releases for LivePi builds and installs the latest one, or any version picked from
// the list (upgrade or downgrade).
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
    // build 0 = the latest release; otherwise that exact build from the list (older or newer)
    void requestInstall (int build = 0) { { std::lock_guard<std::mutex> l (m); wantedBuild = build; } post (Job::install); }

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
        juce::Array<juce::var> list;
        for (auto& r : releases)
        {
            auto* ro = new juce::DynamicObject();
            ro->setProperty ("build", r.build);
            ro->setProperty ("version", r.version);
            ro->setProperty ("date", r.date);
            ro->setProperty ("installed", juce::String (r.build) == cfg.currentBuild);
            list.add (juce::var (ro));
        }
        o->setProperty ("releases", list);
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
                                    cfg.apiBase + "/repos/" + cfg.repo + "/releases?per_page=60" }, 45);
        if (code != 0)
        {
            err = curlProblem (code);
            return false;
        }

        auto v = juce::JSON::parse (json);
        json.deleteFile();
        auto* arr = v.getArray();
        if (arr == nullptr)
        {
            err = "GitHub sent an unexpected answer. Try again later.";
            return false;
        }

        std::vector<Release> list;
        for (auto& rel : *arr)
        {
            if ((bool) rel.getProperty ("draft", false))
                continue;
            const auto tag = rel.getProperty ("tag_name", {}).toString();
            if (! tag.startsWith ("build-") || ! tag.substring (6).containsOnly ("0123456789") || tag.length() < 7)
                continue;
            Release r;
            r.build = tag.substring (6).getIntValue();
            const juce::String wanted = "livepi-build-" + juce::String (r.build) + ".zip";
            if (auto* assets = rel.getProperty ("assets", {}).getArray())
                for (auto& a : *assets)
                    if (a.getProperty ("name", {}).toString() == wanted)
                    {
                        r.url = a.getProperty ("browser_download_url", {}).toString();
                        r.size = (juce::int64) a.getProperty ("size", 0);
                    }
            // Only ever download from this project's own releases
            if (! r.url.startsWith ("https://github.com/" + cfg.repo + "/releases/download/"))
                continue;
            // the release title carries the version people see: "LivePi v0.5.1"
            const auto name = rel.getProperty ("name", {}).toString();
            for (int k = name.length(); --k >= 0;)
                if (name[k] == 'v' && k + 1 < name.length() && juce::CharacterFunctions::isDigit (name[k + 1])) { r.version = name.substring (k).upToFirstOccurrenceOf (" ", false, false); break; }
            if (r.version.isEmpty()) r.version = "build " + juce::String (r.build);
            r.date = rel.getProperty ("published_at", {}).toString().substring (0, 10);
            list.push_back (r);
        }
        std::sort (list.begin(), list.end(), [] (const Release& x, const Release& y) { return x.build > y.build; });
        if (list.empty())
        {
            err = "GitHub has no LivePi release to download yet.";
            return false;
        }

        const bool numeric = cfg.currentBuild.isNotEmpty() && cfg.currentBuild.containsOnly ("0123456789");
        std::lock_guard<std::mutex> l (m);
        releases = list;
        latestBuild = list.front().build;
        latestVersion = list.front().version;
        assetUrl = list.front().url;
        assetSize = list.front().size;
        available = numeric && latestBuild > cfg.currentBuild.getIntValue();
        lastChecked = juce::Time::currentTimeMillis();
        if (! numeric)
            message = "This is a development version, so it will not update itself.";
        return true;
    }

    bool install (juce::String& err)
    {
        juce::String url, shownVersion;
        juce::int64 size;
        {
            std::lock_guard<std::mutex> l (m);
            if (wantedBuild == 0)
            {
                if (! available)
                {
                    err = "Already up to date.";
                    return false;
                }
                url = assetUrl;
                size = assetSize;
                shownVersion = latestVersion;
            }
            else
            {
                const Release* pick = nullptr;
                for (auto& r : releases)
                    if (r.build == wantedBuild) pick = &r;
                if (pick == nullptr)
                {
                    err = "That version is not in the list from GitHub. Check for updates and try again.";
                    return false;
                }
                url = pick->url;
                size = pick->size;
                shownVersion = pick->version;
            }
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
            message = "Installed " + (version.isNotEmpty() ? version : shownVersion) + ". Restarting…";
        }
        if (cfg.onInstalled)
            cfg.onInstalled();
        return true;
    }

    struct Release { int build = 0; juce::String version, date, url; juce::int64 size = 0; };

    Config cfg;
    std::mutex m;
    std::vector<Release> releases;                // newest first
    int wantedBuild = 0;
    Job pending = Job::none;
    juce::String state = "idle", message, assetUrl;
    int latestBuild = 0;
    juce::String latestVersion;
    juce::int64 assetSize = 0, lastChecked = 0;
    bool available = false;
};
