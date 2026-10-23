#include "WebServer.h"
#include "Updater.h"
#include <atomic>

#ifndef LIVEHOST_REPO_STR
 #define LIVEHOST_REPO_STR "leonidis2013/LivePi"
#endif
#ifndef LIVEHOST_BUILD_STR
 #define LIVEHOST_BUILD_STR "dev"
#endif

using namespace juce;

namespace
{
    // Run f on the JUCE message thread and wait for the result.
    template <typename F>
    var onMT (F&& f, int timeoutMs = 5000)
    {
        var result;
        WaitableEvent ev;
        MessageManager::callAsync ([&]
        {
            result = f();
            ev.signal();
        });

        if (! ev.wait (timeoutMs))
        {
            auto* o = new DynamicObject();
            o->setProperty ("error", "timeout");
            return var (o);
        }
        return result;
    }

    var errVar (const String& msg)
    {
        auto* o = new DynamicObject();
        o->setProperty ("error", msg);
        return var (o);
    }

    void reply (httplib::Response& res, const var& v)
    {
        if (auto* o = v.getDynamicObject())
            if (o->hasProperty ("error"))
                res.status = 400;
        res.set_content (JSON::toString (v, true).toStdString(), "application/json");
    }

    var body (const httplib::Request& req)
    {
        return JSON::parse (String (req.body));
    }

    String qs (const httplib::Request& req, const char* key)
    {
        return req.has_param (key) ? String (req.get_param_value (key)) : String();
    }

    // Run a privileged command shortly after replying (needs the sudoers rule from the installer).
    void scheduleCommand (StringArray args)
    {
        MessageManager::callAsync ([args]
        {
            Timer::callAfterDelay (700, [args]
            {
                ChildProcess p;
                if (p.start (args))
                    p.waitForProcessToFinish (5000);
            });
        });
    }
}

WebServer::WebServer (HostEngine& e, int portToUse) : engine (e), port (portToUse)
{
    internet = std::make_unique<InternetMonitor> (std::vector<InternetMonitor::Target> {
        { "deb.debian.org", 443 }, { "archive.raspberrypi.com", 443 }, { "api.github.com", 443 } });

    OnlineUpdate::Config cfg;
    cfg.repo = LIVEHOST_REPO_STR;
    cfg.currentBuild = LIVEHOST_BUILD_STR;
    cfg.dataDir = HostEngine::dataDir();
    auto exe = File::getSpecialLocation (File::currentExecutableFile);
    cfg.installDir = exe.getParentDirectory();
    cfg.exeName = exe.getFileName();
    cfg.isOnline = [this] { return internet->isOnline(); };
    cfg.onInstalled = []
    {
        // systemd (via run.sh) starts the new version
        MessageManager::callAsync ([] { Timer::callAfterDelay (1500, [] { JUCEApplicationBase::quit(); }); });
    };
    ghUpdate = std::make_unique<OnlineUpdate> (cfg);

    loadAuth();
    loadTrusted();
    setupRoutes();
}

WebServer::~WebServer()
{
    stop();
}

void WebServer::start()
{
    thread = std::thread ([this] { svr.listen ("0.0.0.0", port); });
}

void WebServer::stop()
{
    svr.stop();
    if (thread.joinable())
        thread.join();
}

void WebServer::loadAuth()
{
    auto f = HostEngine::dataDir().getChildFile ("auth.json");
    if (! f.existsAsFile())
    {
        Logger::writeToLog ("No password set (~/.livehost/auth.json missing). Uploads and system controls are disabled.");
        return;
    }
    auto v = JSON::parse (f);
    authSalt = v.getProperty ("salt", {}).toString();
    authHash = v.getProperty ("hash", {}).toString().toLowerCase();
}

namespace
{
    bool parseIPv4 (const String& s, uint32& out)
    {
        auto parts = StringArray::fromTokens (s.trim(), ".", "");
        if (parts.size() != 4)
            return false;
        uint32 v = 0;
        for (auto& p : parts)
        {
            if (p.isEmpty() || p.length() > 3 || ! p.containsOnly ("0123456789") || p.getIntValue() > 255)
                return false;
            v = (v << 8) | (uint32) p.getIntValue();
        }
        out = v;
        return true;
    }
}

void WebServer::loadTrusted()
{
    trusted.clear();
    trustedText.clear();
    auto f = HostEngine::dataDir().getChildFile ("trusted.txt");
    if (! f.existsAsFile())
        return;

    for (auto line : StringArray::fromLines (f.loadFileAsString()))
    {
        line = line.upToFirstOccurrenceOf ("#", false, false).trim();
        if (line.isEmpty())
            continue;

        const int prefix = line.containsChar ('/') ? line.fromFirstOccurrenceOf ("/", false, false).getIntValue() : 32;
        uint32 a = 0;
        // Refuse anything wider than a /8, so a typo cannot open the page to the whole internet
        if (! parseIPv4 (line.upToFirstOccurrenceOf ("/", false, false), a) || prefix < 8 || prefix > 32)
        {
            Logger::writeToLog ("Ignoring trusted network entry: " + line);
            continue;
        }
        const uint32 mask = prefix == 32 ? 0xffffffffu : ~(0xffffffffu >> prefix);
        trusted.push_back ({ a & mask, mask });
        trustedText.add (line);
    }
}

bool WebServer::isTrusted (const httplib::Request& req) const
{
    if (trusted.empty())
        return false;

    String ip (req.remote_addr);
    if (ip.startsWithIgnoreCase ("::ffff:"))
        ip = ip.substring (7);                                  // IPv4 seen through a dual-stack socket

    uint32 a = 0;
    if (! parseIPv4 (ip, a))
        return false;
    for (auto& n : trusted)
        if ((a & n.mask) == n.addr)
            return true;
    return false;
}

bool WebServer::checkAuth (const httplib::Request& req) const
{
    String h (req.get_header_value ("Authorization"));
    if (! h.startsWithIgnoreCase ("Basic "))
        return false;

    MemoryOutputStream out;
    if (! Base64::convertFromBase64 (out, h.substring (6).trim()))
        return false;

    const String creds = out.toString();                       // "user:password"
    const String pass = creds.fromFirstOccurrenceOf (":", false, false);
    const String got = juce::SHA256 ((authSalt + pass).toUTF8(), (size_t) (authSalt + pass).getNumBytesAsUTF8()).toHexString();

    if (got.length() != authHash.length())
        return false;
    int diff = 0;
    for (int i = 0; i < got.length(); ++i)
        diff |= (int) got[i] ^ (int) authHash[i];
    return diff == 0;
}

void WebServer::setupRoutes()
{
    auto webDir = File::getSpecialLocation (File::currentExecutableFile)
                      .getParentDirectory().getChildFile ("web");
    svr.set_mount_point ("/", webDir.getFullPathName().toStdString());

    svr.set_payload_max_length ((size_t) 400 * 1024 * 1024);   // plugin zips
    svr.set_read_timeout (30, 0);
    svr.set_write_timeout (30, 0);

    // Password, CSRF guard, and "no password" lockout for dangerous routes
    svr.set_pre_routing_handler ([this] (const httplib::Request& req, httplib::Response& res)
    {
        const bool passwordSet = authHash.isNotEmpty();
        const bool trustedClient = isTrusted (req);   // listed in trusted.txt: no password needed

        if (passwordSet && ! trustedClient && ! checkAuth (req))
        {
            if (req.has_header ("Authorization"))
                Thread::sleep (400);                            // slow down guessing
            res.status = 401;
            res.set_header ("WWW-Authenticate", "Basic realm=\"LiveHost\"");
            res.set_content ("Password required", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }

        // Browsers cannot add a custom header cross-site, so this blocks CSRF from other web pages
        if (req.method != "GET" && req.method != "HEAD" && req.get_header_value ("X-LiveHost") != "1")
        {
            res.status = 403;
            res.set_content ("{\"error\":\"missing X-LiveHost header\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        const String path (req.path);
        if (! passwordSet && ! trustedClient && (path.startsWith ("/api/plugins/upload") || path.startsWith ("/api/plugins/delete") || path.startsWith ("/api/testfile")
                              || path.startsWith ("/api/system/")))
        {
            res.status = 403;
            res.set_content ("{\"error\":\"Set a password (sudo /opt/livehost/set-password.sh) or trust this network (sudo /opt/livehost/trusted-network.sh add <network>) first\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });

    svr.Get ("/api/info", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto* o = new DynamicObject();
        o->setProperty ("passwordSet", authHash.isNotEmpty());
        o->setProperty ("trusted", isTrusted (req));
        o->setProperty ("trustedNetworks", trustedText.joinIntoString (", "));
        const String build (LIVEHOST_BUILD_STR);
        o->setProperty ("version", build == "dev" ? build : "build " + build);
        o->setProperty ("rolledBack", HostEngine::dataDir().getChildFile ("rolled-back").existsAsFile());
        reply (res, var (o));
    });

    svr.Get ("/api/state", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, onMT ([this] { return engine.getState(); }));
    });

    svr.Get ("/api/meters", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, onMT ([this] { return engine.getMeters(); }));
    });

    svr.Get ("/api/devices", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, onMT ([this] { return engine.getDevices(); }));
    });

    svr.Post ("/api/device", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b]
        {
            return engine.setDevice (b["type"].toString(), b["input"].toString(), b["output"].toString(),
                                     (double) b["sampleRate"], (int) b["bufferSize"]);
        }));
    });

    svr.Post ("/api/channels", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.setNumChannels ((int) b["count"]); }));
    });

    // Mixer
    svr.Post ("/api/mixer", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.setMixer (b); }));
    });

    svr.Post ("/api/master", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.setMasterDb ((float) (double) b["gainDb"]); }));
    });

    svr.Post ("/api/channel/name", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.setChannelName ((int) b["channel"], b["name"].toString()); }));
    });

    // Plugins
    svr.Get ("/api/plugins", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, onMT ([this] { return engine.getPlugins(); }));
    });

    svr.Get ("/api/plugins/installed", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, onMT ([this] { return engine.getInstalledPlugins(); }));
    });

    svr.Post ("/api/scan", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, onMT ([this] { return engine.scanPlugins(); }, 180000));
    });

    svr.Post ("/api/plugins/upload", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto it = req.files.find ("file");
        if (it == req.files.end())
        {
            reply (res, errVar ("No file received."));
            return;
        }

        const auto& up = it->second;
        auto staging = HostEngine::dataDir().getChildFile ("staging").getChildFile (Uuid().toString());
        staging.createDirectory();

        auto zip = staging.getChildFile ("upload.zip");
        if (! zip.replaceWithData (up.content.data(), up.content.size()))
        {
            staging.deleteRecursively();
            reply (res, errVar ("Could not save the upload (disk full?)."));
            return;
        }

        auto unpacked = staging.getChildFile ("unpacked");
        auto e = Updater::extractZipSafely (zip, unpacked);
        if (e.isNotEmpty())
        {
            staging.deleteRecursively();
            reply (res, errVar (e));
            return;
        }

        const String uploadName (up.filename);
        auto result = onMT ([this, unpacked, uploadName] { return engine.installPlugins (unpacked, uploadName); }, 300000);
        staging.deleteRecursively();
        reply (res, result);
    });

    svr.Post ("/api/plugins/delete", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.deletePlugin (b["name"].toString()); }));
    });

    // System
    // Operating system updates (apt), run by a root helper installed by install-pi.sh
    svr.Get ("/api/os/status", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, OsStatus::read (File (OsStatus::stateDir), File (OsStatus::helperPath).existsAsFile(),
                                    internet->isKnown(), internet->isOnline()));
    });

    auto osAction = [this] (const char* verb)
    {
        return [this, verb] (const httplib::Request&, httplib::Response& res)
        {
            if (! File (OsStatus::helperPath).existsAsFile())
            {
                reply (res, errVar ("OS updates are not set up on this Pi. Re-run the installer (sudo ./install-pi.sh)."));
                return;
            }
            if (! internet->isOnline())
            {
                reply (res, errVar ("No internet access"));
                return;
            }

            ChildProcess p;
            if (! p.start (StringArray { "sudo", "-n", OsStatus::helperPath, verb }))
            {
                reply (res, errVar ("Could not start the update helper."));
                return;
            }
            auto output = p.readAllProcessOutput().trim();
            p.waitForProcessToFinish (15000);
            const auto code = p.getExitCode();

            if (code == 3)
                reply (res, errVar ("An OS update is already running."));
            else if (code != 0)
                reply (res, errVar ("Could not start the update" + (output.isEmpty() ? String() : ": " + output)));
            else
            {
                auto* o = new DynamicObject();
                o->setProperty ("ok", true);               // the page then polls /api/os/status
                reply (res, var (o));
            }
        };
    };
    svr.Post ("/api/system/os/check", osAction ("check"));
    svr.Post ("/api/system/os/upgrade", osAction ("upgrade"));

    // Offline update: upload livehost-build-N.zip from GitHub
    svr.Post ("/api/system/update", [] (const httplib::Request& req, httplib::Response& res)
    {
        static std::atomic<bool> busy { false };
        if (busy.exchange (true))
        {
            reply (res, errVar ("An update is already in progress."));
            return;
        }
        struct Release { ~Release() { busy = false; } } release;

        auto it = req.files.find ("file");
        if (it == req.files.end())
        {
            reply (res, errVar ("No file received."));
            return;
        }

        auto staging = HostEngine::dataDir().getChildFile ("staging").getChildFile (Uuid().toString());
        staging.createDirectory();

        auto zip = staging.getChildFile ("update.zip");
        if (! zip.replaceWithData (it->second.content.data(), it->second.content.size()))
        {
            staging.deleteRecursively();
            reply (res, errVar ("Could not save the upload (disk full?)."));
            return;
        }

        auto unpacked = staging.getChildFile ("unpacked");
        auto e = Updater::extractZipSafely (zip, unpacked);

        String version;
        if (e.isEmpty())
        {
            auto exe = File::getSpecialLocation (File::currentExecutableFile);
            e = Updater::apply (unpacked, exe.getParentDirectory(), exe.getFileName(), HostEngine::dataDir(), version);
        }
        staging.deleteRecursively();

        if (e.isNotEmpty())
        {
            reply (res, errVar (e));
            return;
        }

        auto* o = new DynamicObject();
        o->setProperty ("ok", true);
        o->setProperty ("version", version);
        reply (res, var (o));

        // systemd (via run.sh) starts the new version
        MessageManager::callAsync ([] { Timer::callAfterDelay (1500, [] { JUCEApplicationBase::quit(); }); });
    });

    svr.Get ("/api/update/status", [this] (const httplib::Request&, httplib::Response& res)
    {
        auto v = ghUpdate->status();
        if (auto* o = v.getDynamicObject())
        {
            o->setProperty ("known", internet->isKnown());
            o->setProperty ("online", internet->isOnline());
        }
        reply (res, v);
    });

    svr.Post ("/api/system/update/check", [this] (const httplib::Request&, httplib::Response& res)
    {
        if (! internet->isOnline())
            return reply (res, errVar ("No internet access"));
        ghUpdate->requestCheck();
        res.set_content ("{\"ok\":true}", "application/json");
    });

    svr.Post ("/api/system/update/install", [this] (const httplib::Request&, httplib::Response& res)
    {
        if (! internet->isOnline())
            return reply (res, errVar ("No internet access"));
        ghUpdate->requestInstall();
        res.set_content ("{\"ok\":true}", "application/json");
    });

    svr.Post ("/api/system/restart", [] (const httplib::Request&, httplib::Response& res)
    {
        res.set_content ("{\"ok\":true}", "application/json");
        MessageManager::callAsync ([] { Timer::callAfterDelay (500, [] { JUCEApplicationBase::quit(); }); });
    });

    svr.Post ("/api/system/reboot", [] (const httplib::Request&, httplib::Response& res)
    {
        res.set_content ("{\"ok\":true}", "application/json");
        scheduleCommand ({ "sudo", "-n", "/usr/bin/systemctl", "reboot" });
    });

    svr.Post ("/api/system/shutdown", [] (const httplib::Request&, httplib::Response& res)
    {
        res.set_content ("{\"ok\":true}", "application/json");
        scheduleCommand ({ "sudo", "-n", "/usr/bin/systemctl", "poweroff" });
    });

    svr.Post ("/api/chain/add", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.addPlugin ((int) b["channel"], b["id"].toString()); }, 30000));
    });

    svr.Post ("/api/chain/remove", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.removePlugin ((int) b["channel"], (int) b["slot"]); }));
    });

    svr.Post ("/api/chain/bypass", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.setBypass ((int) b["channel"], (int) b["slot"], (bool) b["bypass"]); }));
    });

    svr.Get ("/api/params", [this] (const httplib::Request& req, httplib::Response& res)
    {
        int ch = qs (req, "channel").getIntValue();
        int slot = qs (req, "slot").getIntValue();
        reply (res, onMT ([this, ch, slot] { return engine.getParams (ch, slot); }));
    });

    svr.Post ("/api/param", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b]
        {
            return engine.setParam ((int) b["channel"], (int) b["slot"], (int) b["index"], (float) (double) b["value"]);
        }));
    });

    // Browser monitor: choose a tap, then the page polls raw 16-bit samples and plays them
    svr.Post ("/api/monitor", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        const int ch = b.hasProperty ("channel") ? (int) b["channel"] : -1;
        const bool post = ! b.hasProperty ("post") || (bool) b["post"];
        reply (res, onMT ([this, ch, post] { return engine.setMonitor (ch, post); }));
    });

    svr.Post ("/api/monitorbus", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.setMonitorBus (b); }));
    });

    svr.Get ("/api/monitor/pcm", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const uint64_t since = (uint64_t) qs (req, "since").getLargeIntValue();
        std::vector<int16_t> pcm;
        uint64_t pos = 0;
        engine.monitorTap().read (since, pcm, pos);
        res.set_header ("X-Seq", std::to_string (pos));
        res.set_header ("X-Rate", std::to_string ((int) engine.monitorTap().rate.load()));
        res.set_header ("Cache-Control", "no-store");
        res.set_content (reinterpret_cast<const char*> (pcm.data()), pcm.size() * sizeof (int16_t), "application/octet-stream");
    });

    // Audio file played in a loop by the virtual sound card's "Test file" input
    svr.Post ("/api/testfile", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto it = req.files.find ("file");
        if (it == req.files.end())
            return reply (res, errVar ("No file received."));

        auto staging = HostEngine::dataDir().getChildFile ("staging").getChildFile (Uuid().toString());
        staging.createDirectory();
        // keep the original extension so the right decoder is picked
        auto f = staging.getChildFile ("upload" + File (String (it->second.filename)).getFileExtension().retainCharacters (".abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"));
        if (! f.replaceWithData (it->second.content.data(), it->second.content.size()))
        {
            staging.deleteRecursively();
            return reply (res, errVar ("Could not save the upload (disk full?)."));
        }
        auto r = engine.installTestFile (f);
        staging.deleteRecursively();
        reply (res, r);
    });

    svr.Post ("/api/testfile/delete", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, engine.removeTestFile());
    });

    svr.Post ("/api/plugins/measure", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.measurePlugin ((int) b["channel"], (int) b["slot"]); }, 30000));
    });

    // Plugin editor windows (shown on the virtual display)
    svr.Post ("/api/editor/open", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.openEditor ((int) b["channel"], (int) b["slot"]); }));
    });

    svr.Post ("/api/editor/close", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.closeEditor ((int) b["channel"], (int) b["slot"]); }));
    });

    // Presets
    svr.Get ("/api/presets", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, onMT ([this] { return engine.listPresets(); }));
    });

    svr.Post ("/api/presets/save", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.savePreset (b["name"].toString()); }));
    });

    svr.Post ("/api/presets/load", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        reply (res, onMT ([this, b] { return engine.loadPreset (b["name"].toString()); }, 60000));
    });
}
