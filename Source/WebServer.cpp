#include "WebServer.h"
#include "Updater.h"
#include <atomic>

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

    // Unpack an uploaded zip into dest, refusing anything that could escape it.
    String extractZipSafely (const File& zipFile, const File& dest)
    {
        ZipFile zip (zipFile);
        const int n = zip.getNumEntries();
        if (n <= 0)
            return "That file is not a valid zip, or it is empty.";
        if (n > 20000)
            return "The zip has too many files.";

        int64 total = 0;
        for (int i = 0; i < n; ++i)
        {
            auto* e = zip.getEntry (i);
            const String name = e->filename;

            if (e->isSymbolicLink)
                return "The zip contains symbolic links, which are not allowed.";
            if (name.containsChar ('\\'))
                return "The zip uses Windows-style paths. Re-zip it with 7-Zip, macOS Archive Utility or the zip command.";
            if (name.startsWithChar ('/') || name.containsChar (':'))
                return "Unsafe path in zip: " + name;
            for (auto& part : StringArray::fromTokens (name, "/", ""))
                if (part == "..")
                    return "Unsafe path in zip: " + name;

            total += e->uncompressedSize;
        }

        if (total > (int64) 1500 * 1024 * 1024)
            return "The zip is too large when unpacked (limit 1.5 GB).";

        dest.createDirectory();
        for (int i = 0; i < n; ++i)
        {
            auto r = zip.uncompressEntry (i, dest, true);
            if (r.failed())
                return "Could not unpack the zip: " + r.getErrorMessage();
        }
        return {};
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
        { "deb.debian.org", 443 }, { "archive.raspberrypi.com", 443 } });
    loadAuth();
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

        if (passwordSet && ! checkAuth (req))
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
        if (! passwordSet && (path.startsWith ("/api/plugins/upload") || path.startsWith ("/api/plugins/delete")
                              || path.startsWith ("/api/system/")))
        {
            res.status = 403;
            res.set_content ("{\"error\":\"Set a password first: sudo /opt/livehost/set-password.sh\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });

    svr.Get ("/api/info", [this] (const httplib::Request&, httplib::Response& res)
    {
        auto* o = new DynamicObject();
        o->setProperty ("passwordSet", authHash.isNotEmpty());
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
        auto e = extractZipSafely (zip, unpacked);
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
        auto e = extractZipSafely (zip, unpacked);

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
