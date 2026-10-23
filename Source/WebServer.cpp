#include "WebServer.h"
#include "EditorViewer.h"
#include "Updater.h"
#include <atomic>
#if JUCE_LINUX
 #include <sys/resource.h>
 #include <sys/syscall.h>
 #include <unistd.h>
#endif

#ifndef LIVEPI_REPO_STR
 #define LIVEPI_REPO_STR "leonidis2013/LivePi"
#endif
#ifndef LIVEPI_BUILD_STR
 #define LIVEPI_BUILD_STR "dev"
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
    cfg.repo = LIVEPI_REPO_STR;
    cfg.currentBuild = LIVEPI_BUILD_STR;
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
        Logger::writeToLog ("No password set (~/.livepi/auth.json missing). Uploads and system controls are disabled.");
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
    const ScopedLock sl (trustedLock);
    trusted.clear();
    trustedText.clear();

    // Without a password, "open" (the default) lets everyone on the network use everything;
    // "locked" keeps uploads, updates, reboot and plugin windows for trusted networks only.
    openAccess = HostEngine::dataDir().getChildFile ("access.txt").loadFileAsString().trim() != "locked";

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

String WebServer::clientIp (const httplib::Request& req)
{
    String ip (req.remote_addr);
    if (ip.startsWithIgnoreCase ("::ffff:"))
        ip = ip.substring (7);                                  // IPv4 seen through a dual-stack socket
    return ip;
}

bool WebServer::isLockedPath (const String& path) const
{
    return path.startsWith ("/api/plugins/upload") || path.startsWith ("/api/plugins/delete") || path.startsWith ("/api/testfile")
        || path.startsWith ("/api/editor") || path.startsWith ("/api/system/");
}

bool WebServer::mayUseLocked (const httplib::Request& req) const
{
    if (authHash.isNotEmpty()) return true;      // a password exists: whoever got past it may
    if (isTrusted (req)) return true;
    const ScopedLock sl (trustedLock);
    return openAccess;
}

String WebServer::saveTrusted (const StringArray& entries)
{
    StringArray clean;
    for (auto e : entries)
    {
        e = e.trim();
        if (e.isEmpty()) continue;
        const int prefix = e.containsChar ('/') ? e.fromFirstOccurrenceOf ("/", false, false).getIntValue() : 32;
        uint32 a = 0;
        if (! parseIPv4 (e.upToFirstOccurrenceOf ("/", false, false), a) || prefix < 8 || prefix > 32
            || (e.containsChar ('/') && ! e.fromFirstOccurrenceOf ("/", false, false).containsOnly ("0123456789")))
            return "Not a valid IPv4 address or network (prefix /8 to /32): " + e;
        clean.addIfNotAlreadyThere (e);
    }
    auto f = HostEngine::dataDir().getChildFile ("trusted.txt");
    if (! f.replaceWithText (clean.joinIntoString ("\n") + (clean.isEmpty() ? "" : "\n")))
        return "Could not save the trusted networks.";
    f.setExecutePermission (false);
    loadTrusted();
    return {};
}

bool WebServer::isTrusted (const httplib::Request& req) const
{
    const ScopedLock sl (trustedLock);
    if (trusted.empty())
        return false;

    const String ip = clientIp (req);

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
            res.set_header ("WWW-Authenticate", "Basic realm=\"LivePi\"");
            res.set_content ("Password required", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }

        // Browsers cannot add a custom header cross-site, so this blocks CSRF from other web pages
        if (req.method != "GET" && req.method != "HEAD" && req.get_header_value ("X-LivePi") != "1")
        {
            res.status = 403;
            res.set_content ("{\"error\":\"missing X-LivePi header\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        const String path (req.path);
        if (! passwordSet && ! trustedClient && isLockedPath (path) && ! mayUseLocked (req))
        {
            res.status = 403;
            res.set_content ("{\"error\":\"Access is locked: this device is not in a trusted network. Set a password, or on the Pi run: sudo /opt/livepi/trusted-network.sh open\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Access settings: open/locked (without a password) and the trusted networks.
    // Anyone may lock; opening or editing the list needs a client that may already use locked features.
    svr.Get ("/api/access", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto* o = new DynamicObject();
        {
            const ScopedLock sl (trustedLock);
            o->setProperty ("mode", openAccess ? "open" : "locked");
            Array<var> nets; for (auto& t : trustedText) nets.add (t);
            o->setProperty ("networks", nets);
        }
        o->setProperty ("passwordSet", authHash.isNotEmpty());
        o->setProperty ("trusted", isTrusted (req));
        o->setProperty ("allowed", mayUseLocked (req));
        o->setProperty ("canChange", mayUseLocked (req));
        const auto ip = clientIp (req);
        o->setProperty ("clientIp", ip);
        uint32 a = 0;
        if (parseIPv4 (ip, a))
            o->setProperty ("clientNet", String ((a >> 24) & 255) + "." + String ((a >> 16) & 255) + "." + String ((a >> 8) & 255) + ".0/24");
        reply (res, var (o));
    });

    svr.Post ("/api/access", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        const bool may = mayUseLocked (req);

        if (b.hasProperty ("networks"))
        {
            if (! may) { reply (res, errVar ("Only a device that already has full access can change the trusted networks.")); return; }
            StringArray list;
            if (auto* arr = b["networks"].getArray()) for (auto& v : *arr) list.add (v.toString());
            auto e = saveTrusted (list);
            if (e.isNotEmpty()) { reply (res, errVar (e)); return; }
        }

        if (b.hasProperty ("mode"))
        {
            const auto m = b["mode"].toString();
            if (m != "open" && m != "locked") { reply (res, errVar ("mode must be open or locked")); return; }
            if (m == "open" && ! may) { reply (res, errVar ("Only a trusted device can unlock. On the Pi run: sudo /opt/livepi/trusted-network.sh open")); return; }
            auto f = HostEngine::dataDir().getChildFile ("access.txt");
            if (! f.replaceWithText (m + "\n")) { reply (res, errVar ("Could not save the setting.")); return; }
            loadTrusted();
        }

        auto* o = new DynamicObject();
        o->setProperty ("ok", true);
        reply (res, var (o));
    });

    svr.Get ("/api/info", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto* o = new DynamicObject();
        o->setProperty ("passwordSet", authHash.isNotEmpty());
        o->setProperty ("trusted", isTrusted (req));
        o->setProperty ("allowed", mayUseLocked (req));
        { const ScopedLock sl (trustedLock); o->setProperty ("trustedNetworks", trustedText.joinIntoString (", ")); o->setProperty ("access", openAccess ? "open" : "locked"); }
        const String build (LIVEPI_BUILD_STR);
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

    // Accepts a .zip, a single .vst3/.so file, or a whole .vst3 folder (many parts named "files",
    // each carrying its relative path as the filename).
    svr.Post ("/api/plugins/upload", [this] (const httplib::Request& req, httplib::Response& res)
    {
        std::vector<const httplib::MultipartFormData*> parts;
        for (auto& kv : req.files)
            if (kv.first == "file" || kv.first == "files")
                parts.push_back (&kv.second);

        if (parts.empty())
        {
            reply (res, errVar ("No file received."));
            return;
        }

        auto staging = HostEngine::dataDir().getChildFile ("staging").getChildFile (Uuid().toString());
        staging.createDirectory();
        auto unpacked = staging.getChildFile ("unpacked");
        unpacked.createDirectory();
        String uploadName (parts.front()->filename);

        auto fail = [&] (const String& m) { staging.deleteRecursively(); reply (res, errVar (m)); };

        // Turns a browser-supplied relative path into safe components, or an empty array.
        auto safeParts = [] (String path)
        {
            StringArray out;
            path = path.replaceCharacter ('\\', '/');
            for (auto& c : StringArray::fromTokens (path, "/", ""))
            {
                if (c.isEmpty() || c == ".") continue;
                if (c == ".." || c.containsAnyOf (":\r\n\t") || c.containsChar (0)) return StringArray();
                out.add (c);
            }
            return out;
        };

        const auto lower = uploadName.toLowerCase();
        if (parts.size() == 1 && lower.endsWith (".zip"))
        {
            auto zip = staging.getChildFile ("upload.zip");
            if (! zip.replaceWithData (parts[0]->content.data(), parts[0]->content.size()))
                return fail ("Could not save the upload (disk full?).");
            auto e = Updater::extractZipSafely (zip, unpacked);
            if (e.isNotEmpty())
                return fail (e);
        }
        else if (parts.size() == 1 && (lower.endsWith (".so") || lower.endsWith (".vst3")))
        {
            // A bare binary: wrap it as Name.vst3/Contents/aarch64-linux/Name.so
            auto comps = safeParts (uploadName);
            if (comps.isEmpty())
                return fail ("Invalid file name.");
            auto base = File::createLegalFileName (comps[comps.size() - 1].upToLastOccurrenceOf (".", false, false));
            if (base.isEmpty())
                return fail ("Invalid file name.");
            auto dir = unpacked.getChildFile (base + ".vst3").getChildFile ("Contents").getChildFile ("aarch64-linux");
            dir.createDirectory();
            if (! dir.getChildFile (base + ".so").replaceWithData (parts[0]->content.data(), parts[0]->content.size()))
                return fail ("Could not save the upload (disk full?).");
        }
        else
        {
            // A folder: every part is one file inside it.
            int saved = 0;
            for (auto* p : parts)
            {
                auto comps = safeParts (p->filename);
                if (comps.isEmpty())
                    return fail ("Unsafe or empty file path in upload: " + p->filename);
                auto target = unpacked;
                for (auto& c : comps) target = target.getChildFile (c);
                if (! target.isAChildOf (unpacked))
                    return fail ("Unsafe file path in upload: " + p->filename);
                target.getParentDirectory().createDirectory();
                if (! target.replaceWithData (p->content.data(), p->content.size()))
                    return fail ("Could not save the upload (disk full?).");
                ++saved;
            }
            if (saved == 0)
                return fail ("The folder was empty.");
            auto first = safeParts (parts.front()->filename);
            if (! first.isEmpty()) uploadName = first[0];
        }

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

    // Offline update: upload livepi-build-N.zip from GitHub
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

    // Continuous low-latency stream of the chosen tap (one long-running response). The audio thread only
    // copies samples into a ring; everything here runs on web threads, at lower priority than the audio.
    svr.Get ("/api/monitor/stream", [this] (const httplib::Request&, httplib::Response& res)
    {
        static std::atomic<int> listeners { 0 };
        if (listeners.fetch_add (1) >= 2)
        {
            --listeners;
            res.status = 429;
            res.set_content ("{\"error\":\"Two browsers are already listening.\"}", "application/json");
            return;
        }

        res.set_header ("X-Rate", std::to_string ((int) engine.monitorTap().rate.load()));
        res.set_header ("Cache-Control", "no-store");

        struct State { uint64_t since = 0; bool niced = false; int idle = 0; };
        auto st = std::make_shared<State>();

        res.set_chunked_content_provider ("application/octet-stream",
            [this, st] (size_t, httplib::DataSink& sink)
            {
               #if JUCE_LINUX
                if (! st->niced)
                {
                    st->niced = true;
                    setpriority (PRIO_PROCESS, (id_t) syscall (SYS_gettid), 10);   // below the audio thread and the rest of the app
                }
               #endif
                thread_local std::vector<int16_t> pcm;
                uint64_t pos = 0;
                engine.monitorTap().read (st->since, pcm, pos, 256);
                st->since = pos;

                if (! pcm.empty())
                {
                    st->idle = 0;
                    if (! sink.write (reinterpret_cast<const char*> (pcm.data()), pcm.size() * sizeof (int16_t)))
                        return false;
                }
                else if (++st->idle > 4000)          // 20 s of silence from the device: give up
                {
                    return false;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
                return sink.is_writable();
            },
            [this] (bool)
            {
                // Browser went away (or stopped): free the slot, and stop copying audio if nobody is listening
                if (--listeners == 0)
                    MessageManager::callAsync ([this] { engine.setMonitor (-1, true); });
            });
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
    svr.Get ("/api/editor/viewer", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, viewer != nullptr ? viewer->getStatus() : errVar ("Plugin windows are turned off (--no-editor)."));
    });

    svr.Post ("/api/editor/open", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        if (viewer == nullptr || ! viewer->isDisplayAvailable())
        {
            reply (res, errVar (viewer == nullptr ? "Plugin windows are turned off (--no-editor)." : viewer->getStatus()["problem"].toString()));
            return;
        }
        reply (res, onMT ([this, b] { return engine.openEditor ((int) b["channel"], (int) b["slot"], (int) b["viewW"], (int) b["viewH"]); }));
    });

    // Live view: the plugin window as a JPEG. Waits (up to ~1.5 s) until it differs from `since`,
    // so an idle plugin costs almost nothing; 204 = unchanged.
    svr.Get ("/api/editor/frame", [this] (const httplib::Request& req, httplib::Response& res)
    {
        if (viewer == nullptr) { res.status = 404; return; }
        const int ch = qs (req, "channel").getIntValue(), slot = qs (req, "slot").getIntValue();
        const int maxW = qs (req, "w").getIntValue();
        const auto since = (juce::uint64) qs (req, "since").getLargeIntValue();

        LiveView::Frame f;
        const auto until = Time::getMillisecondCounter() + 1500;
        for (;;)
        {
            const auto area = onMT ([this, ch, slot]
            {
                auto r = engine.getEditorArea (ch, slot);
                return var (Array<var> { r.getX(), r.getY(), r.getWidth(), r.getHeight() });
            });
            const Rectangle<int> r ((int) area[0], (int) area[1], (int) area[2], (int) area[3]);
            if (r.isEmpty()) { res.status = 410; return; }   // window closed
            f = {};
            if (! viewer->live().grab (r, maxW, since, f)) { res.status = 503; return; }
            if (f.jpeg.getSize() > 0 || Time::getMillisecondCounter() > until) break;
            Thread::sleep (66);   // ~15 checks a second while nothing changes
        }

        res.set_header ("Cache-Control", "no-store");
        res.set_header ("X-Frame", String ((juce::int64) f.hash).toStdString());
        res.set_header ("X-Size", (String (f.width) + "x" + String (f.height)).toStdString());
        if (f.jpeg.getSize() == 0) { res.status = 204; return; }
        res.set_content ((const char*) f.jpeg.getData(), f.jpeg.getSize(), "image/jpeg");
    });

    // Live view input: [{t:"down"|"up"|"move"|"wheel"|"key", x, y, b, dy, k}], x/y in plugin window pixels
    svr.Post ("/api/editor/input", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        if (viewer == nullptr) { reply (res, errVar ("Plugin windows are turned off.")); return; }
        const int ch = (int) b["channel"], slot = (int) b["slot"];
        const auto area = onMT ([this, ch, slot] { auto r = engine.getEditorArea (ch, slot); return var (Array<var> { r.getX(), r.getY(), r.getWidth(), r.getHeight() }); });
        const Rectangle<int> r ((int) area[0], (int) area[1], (int) area[2], (int) area[3]);
        if (r.isEmpty()) { reply (res, errVar ("The plugin window is closed.")); return; }

        auto& lv = viewer->live();
        if (auto* evs = b["events"].getArray())
            for (auto& e : *evs)
            {
                const auto t = e["t"].toString();
                const Point<int> p (r.getX() + jlimit (0, r.getWidth() - 1, (int) e["x"]), r.getY() + jlimit (0, r.getHeight() - 1, (int) e["y"]));
                const int btn = e.hasProperty ("b") ? jlimit (1, 3, (int) e["b"]) : 1;
                if (t == "move")       lv.pointer (p, 0, false);
                else if (t == "down")  lv.pointer (p, btn, true);
                else if (t == "up")    lv.pointer (p, btn, false);
                else if (t == "wheel") lv.wheel (p, (int) e["dy"]);
                else if (t == "key")   lv.key (e["k"].toString());
            }
        reply (res, var (new DynamicObject()));
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
