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
#ifndef LIVEPI_VERSION_STR
 #define LIVEPI_VERSION_STR "0.0.0"
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
    cfg.currentVersion = LIVEPI_VERSION_STR;
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
    const ScopedLock sl (trustedLock);
    authSalt.clear(); authHash.clear(); protectionOn = true;
    auto f = HostEngine::dataDir().getChildFile ("auth.json");
    if (f.existsAsFile())
    {
        auto v = JSON::parse (f);
        authSalt = v.getProperty ("salt", {}).toString();
        authHash = v.getProperty ("hash", {}).toString().toLowerCase();
        protectionOn = (bool) v.getProperty ("enabled", true);
    }
    if (authHash.isEmpty())
        Logger::writeToLog ("No password set: everyone on the network can use LivePi.");

    sessions.clear();
    auto sj = JSON::parse (HostEngine::dataDir().getChildFile ("sessions.json"));
    const auto now = Time::currentTimeMillis();
    if (auto* arr = sj.getArray())
        for (auto& e : *arr)
            if ((int64) e["expires"] > now)
                sessions.push_back ({ e["hash"].toString(), (int64) e["expires"] });
}

void WebServer::saveAuth()   // caller holds trustedLock
{
    auto* o = new DynamicObject();
    o->setProperty ("salt", authSalt);
    o->setProperty ("hash", authHash);
    o->setProperty ("enabled", protectionOn);
    auto f = HostEngine::dataDir().getChildFile ("auth.json");
    f.replaceWithText (JSON::toString (var (o)));
    chmod (f.getFullPathName().toRawUTF8(), 0600);
}

void WebServer::saveSessions()   // caller holds trustedLock
{
    Array<var> arr;
    for (auto& x : sessions)
    {
        auto* o = new DynamicObject();
        o->setProperty ("hash", x.hash);
        o->setProperty ("expires", x.expires);
        arr.add (var (o));
    }
    auto f = HostEngine::dataDir().getChildFile ("sessions.json");
    f.replaceWithText (JSON::toString (var (arr)));
    chmod (f.getFullPathName().toRawUTF8(), 0600);
}

namespace
{
    String sha (const String& s) { return SHA256 (s.toUTF8(), (size_t) s.getNumBytesAsUTF8()).toHexString(); }

    bool sameText (const String& a, const String& b)
    {
        if (a.length() != b.length()) return false;
        int diff = 0;
        for (int i = 0; i < a.length(); ++i) diff |= (int) a[i] ^ (int) b[i];
        return diff == 0;
    }

    String randomHex (int bytes)
    {
        // /dev/urandom for tokens and salts
        MemoryBlock mb ((size_t) bytes);
        FileInputStream in (File ("/dev/urandom"));
        if (! in.openedOk() || in.read (mb.getData(), bytes) != bytes)
        {
            Random r (Time::getHighResolutionTicks());
            for (int i = 0; i < bytes; ++i) ((uint8*) mb.getData())[i] = (uint8) r.nextInt (256);
        }
        return String::toHexString (mb.getData(), bytes, 0);
    }

    String cookieValue (const httplib::Request& req, const char* name)
    {
        const String all (req.get_header_value ("Cookie"));
        for (auto part : StringArray::fromTokens (all, ";", ""))
        {
            part = part.trim();
            if (part.startsWith (String (name) + "="))
                return part.fromFirstOccurrenceOf ("=", false, false);
        }
        return {};
    }
}

bool WebServer::verifyPassword (const String& pw) const
{
    const ScopedLock sl (trustedLock);
    return authHash.isNotEmpty() && sameText (sha (authSalt + pw), authHash);
}

bool WebServer::protectionActive() const
{
    const ScopedLock sl (trustedLock);
    return authHash.isNotEmpty() && protectionOn;
}

bool WebServer::hasSession (const httplib::Request& req) const
{
    const auto tok = cookieValue (req, "lp_session");
    if (tok.length() < 32) return false;
    const auto h = sha (tok);
    const auto now = Time::currentTimeMillis();
    const ScopedLock sl (trustedLock);
    for (auto& x : sessions)
        if (x.expires > now && sameText (x.hash, h))
            return true;
    return false;
}

std::string WebServer::newSession()
{
    const auto tok = randomHex (24);
    const auto now = Time::currentTimeMillis();
    {
        const ScopedLock sl (trustedLock);
        sessions.erase (std::remove_if (sessions.begin(), sessions.end(), [now] (const Session& x) { return x.expires <= now; }), sessions.end());
        while (sessions.size() > 50) sessions.erase (sessions.begin());
        sessions.push_back ({ sha (tok), now + (int64) 30 * 24 * 3600 * 1000 });
        saveSessions();
    }
    return ("lp_session=" + tok + "; Path=/; Max-Age=2592000; HttpOnly; SameSite=Strict").toStdString();
}

bool WebServer::isAuthed (const httplib::Request& req) const
{
    if (! protectionActive()) return true;
    {
        const ScopedLock sl (trustedLock);
        if (trustedAccess && ! trusted.empty() && isTrusted (req)) return true;
    }
    return hasSession (req) || checkBasic (req);
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

    // "on": with password protection on, devices in the trusted networks skip the login
    const auto acc = HostEngine::dataDir().getChildFile ("access.txt").loadFileAsString().trim().toLowerCase();
    trustedAccess = acc == "on" || acc == "locked" || acc == "trusted";

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

bool WebServer::claimControl (const httplib::Request& req, bool force, var* busyInfo)
{
    const auto id = req.get_header_value ("X-LivePi-Client");
    if (id.empty() || id.size() > 64) return true;              // scripts and tools: not limited
    const auto now = Time::currentTimeMillis();
    const ScopedLock sl (trustedLock);
    const bool free = ctl.id.empty() || now - ctl.seen > kCtlIdleMs;
    if (force || free || ctl.id == id)
    {
        if (ctl.id != id) ctl.since = now;
        ctl.id = id;
        ctl.ip = clientIp (req).toStdString();
        ctl.seen = now;
        return true;
    }
    if (busyInfo != nullptr)
    {
        auto* o = new DynamicObject();
        o->setProperty ("error", "Another device is using LivePi");
        o->setProperty ("busy", true);
        o->setProperty ("by", String (ctl.ip));
        o->setProperty ("forSeconds", (int) ((now - ctl.since) / 1000));
        *busyInfo = var (o);
    }
    return false;
}

int WebServer::lockoutSeconds (const std::string& ip) const
{
    const ScopedLock sl (trustedLock);
    auto it = loginFails.find (ip);
    if (it == loginFails.end() || it->second.first < kMaxFails) return 0;
    const auto left = kLockMs - (Time::currentTimeMillis() - it->second.second);
    return left > 0 ? (int) ((left + 999) / 1000) : 0;
}

var WebServer::sessionInfo (const httplib::Request& req)
{
    auto* o = new DynamicObject();
    if (const int w = lockoutSeconds (clientIp (req).toStdString()); w > 0)
        o->setProperty ("retryAfter", w);
    const bool active = protectionActive();
    {
        const ScopedLock sl (trustedLock);
        o->setProperty ("passwordSet", authHash.isNotEmpty());
        o->setProperty ("protection", protectionOn && authHash.isNotEmpty());
        o->setProperty ("trustedAccess", trustedAccess && active);
        Array<var> nets; for (auto& t : trustedText) nets.add (t);
        o->setProperty ("networks", nets);
    }
    const bool authed = isAuthed (req);
    o->setProperty ("authed", authed);
    o->setProperty ("loggedIn", hasSession (req));
    o->setProperty ("trusted", isTrusted (req));
    if (authed)
    {
        const auto ip = clientIp (req);
        o->setProperty ("clientIp", ip);
        uint32 a = 0;
        if (parseIPv4 (ip, a))
            o->setProperty ("clientNet", String ((a >> 24) & 255) + "." + String ((a >> 16) & 255) + "." + String ((a >> 8) & 255) + ".0/24");
    }
    return var (o);
}

String WebServer::clientIp (const httplib::Request& req)
{
    String ip (req.remote_addr);
    if (ip.startsWithIgnoreCase ("::ffff:"))
        ip = ip.substring (7);                                  // IPv4 seen through a dual-stack socket
    return ip;
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

bool WebServer::checkBasic (const httplib::Request& req) const
{
    String h (req.get_header_value ("Authorization"));
    if (! h.startsWithIgnoreCase ("Basic "))
        return false;
    MemoryOutputStream out;
    if (! Base64::convertFromBase64 (out, h.substring (6).trim()))
        return false;
    return verifyPassword (out.toString().fromFirstOccurrenceOf (":", false, false));
}

void WebServer::setupRoutes()
{
    auto webDir = File::getSpecialLocation (File::currentExecutableFile)
                      .getParentDirectory().getChildFile ("web");
    svr.set_mount_point ("/", webDir.getFullPathName().toStdString());

    svr.set_payload_max_length ((size_t) 400 * 1024 * 1024);   // plugin zips
    svr.set_read_timeout (30, 0);
    svr.set_write_timeout (30, 0);

    // CSRF guard, then the login (only while password protection is on). The page itself (static
    // files) always loads, and shows its own password-only login screen when needed.
    svr.set_pre_routing_handler ([this] (const httplib::Request& req, httplib::Response& res)
    {
        // Browsers cannot add a custom header cross-site, so this blocks CSRF from other web pages
        if (req.method != "GET" && req.method != "HEAD" && req.get_header_value ("X-LivePi") != "1")
        {
            res.status = 403;
            res.set_content ("{\"error\":\"missing X-LivePi header\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        const String path (req.path);
        const bool open = path == "/api/session" || path == "/api/login" || path == "/api/logout";
        if (path.startsWith ("/api/") && ! open && ! isAuthed (req))
        {
            res.status = 401;
            res.set_content ("{\"error\":\"Please log in\",\"login\":true}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        // one device at a time (scripts without the header are not limited)
        if (path.startsWith ("/api/") && ! open && path != "/api/control/claim")
        {
            var busy;
            if (! claimControl (req, false, &busy))
            {
                res.status = 409;
                res.set_content (JSON::toString (busy, true).toStdString(), "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Take over control from another device
    svr.Post ("/api/control/claim", [this] (const httplib::Request& req, httplib::Response& res)
    {
        claimControl (req, true, nullptr);
        auto* o = new DynamicObject(); o->setProperty ("ok", true); reply (res, var (o));
    });

    // Login state for the page
    svr.Get ("/api/session", [this] (const httplib::Request& req, httplib::Response& res)
    {
        reply (res, sessionInfo (req));
    });

    svr.Post ("/api/login", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto ip = clientIp (req).toStdString();
        const auto now = Time::currentTimeMillis();
        const int wait = lockoutSeconds (ip);
        if (wait > 0)
        {
            auto v = errVar ("Too many wrong passwords.");
            v.getDynamicObject()->setProperty ("retryAfter", wait);
            reply (res, v);
            return;
        }
        auto b = body (req);
        if (! verifyPassword (b["password"].toString()))
        {
            int left = 0;
            {
                const ScopedLock sl (trustedLock);
                auto& f = loginFails[ip];
                if (now - f.second > kLockMs) f.first = 0;
                ++f.first; f.second = now;
                left = jmax (0, kMaxFails - f.first);
            }
            Thread::sleep (600);   // slows down guessing
            auto v = errVar (left > 0 ? "Wrong password. " + String (left) + (left == 1 ? " try" : " tries") + " left."
                                      : String ("Too many wrong passwords."));
            if (left == 0) v.getDynamicObject()->setProperty ("retryAfter", lockoutSeconds (ip));
            reply (res, v);
            return;
        }
        { const ScopedLock sl (trustedLock); loginFails.erase (ip); }
        res.set_header ("Set-Cookie", newSession());
        reply (res, sessionInfo (req));
    });

    svr.Post ("/api/logout", [this] (const httplib::Request& req, httplib::Response& res)
    {
        const auto tok = cookieValue (req, "lp_session");
        if (tok.isNotEmpty())
        {
            const auto h = sha (tok);
            const ScopedLock sl (trustedLock);
            sessions.erase (std::remove_if (sessions.begin(), sessions.end(), [&h] (const Session& x) { return x.hash == h; }), sessions.end());
            saveSessions();
        }
        res.set_header ("Set-Cookie", "lp_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
        auto* o = new DynamicObject(); o->setProperty ("ok", true); reply (res, var (o));
    });

    // Set or change the password. Needs the current one while protection is on.
    // Setting a password turns protection on; this device stays logged in, all others must log in again.
    svr.Post ("/api/password", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        const auto pw = b["password"].toString();
        if (pw.length() < 4) { reply (res, errVar ("Use at least 4 characters.")); return; }
        if (protectionActive() && ! verifyPassword (b["current"].toString()))
        {
            Thread::sleep (600);
            reply (res, errVar ("The current password is wrong."));
            return;
        }
        {
            const ScopedLock sl (trustedLock);
            authSalt = randomHex (16);
            authHash = sha (authSalt + pw);
            protectionOn = true;
            saveAuth();
            sessions.clear();
            saveSessions();
        }
        res.set_header ("Set-Cookie", newSession());
        reply (res, sessionInfo (req));
    });

    // Password protection on/off. Turning it off never asks for the password (whoever can reach this
    // is already in). Turning it on always asks for the password to use from now on (it may be the
    // old one or a new one), so nobody switches it on without knowing the password.
    svr.Post ("/api/password/protection", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        const bool on = (bool) b["enabled"];
        if (on)
        {
            const auto pw = b["password"].toString();
            if (pw.length() < 4) { reply (res, errVar ("Enter the password to use (at least 4 characters).")); return; }
            const ScopedLock sl (trustedLock);
            authSalt = randomHex (16);
            authHash = sha (authSalt + pw);
            protectionOn = true;
            saveAuth();
            sessions.clear();
            saveSessions();
        }
        else
        {
            const ScopedLock sl (trustedLock);
            protectionOn = false;
            saveAuth();
        }
        if (on) res.set_header ("Set-Cookie", newSession());   // keep this device logged in
        reply (res, sessionInfo (req));
    });

    // Trusted access (skip the login from trusted networks) and the list of networks.
    // Only available while password protection is on.
    svr.Post ("/api/access", [this] (const httplib::Request& req, httplib::Response& res)
    {
        auto b = body (req);
        if (! protectionActive()) { reply (res, errVar ("Trusted access needs a password with protection turned on.")); return; }
        if (b.hasProperty ("networks"))
        {
            StringArray list;
            if (auto* arr = b["networks"].getArray()) for (auto& v : *arr) list.add (v.toString());
            auto e = saveTrusted (list);
            if (e.isNotEmpty()) { reply (res, errVar (e)); return; }
        }
        if (b.hasProperty ("trustedAccess"))
        {
            auto f = HostEngine::dataDir().getChildFile ("access.txt");
            if (! f.replaceWithText ((bool) b["trustedAccess"] ? "on\n" : "off\n")) { reply (res, errVar ("Could not save the setting.")); return; }
            loadTrusted();
        }
        reply (res, sessionInfo (req));
    });

    svr.Get ("/api/info", [this] (const httplib::Request&, httplib::Response& res)
    {
        auto* o = new DynamicObject();
        const String build (LIVEPI_BUILD_STR);
        o->setProperty ("version", build == "dev" ? String ("v" LIVEPI_VERSION_STR " (development)") : String ("v" LIVEPI_VERSION_STR));
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

    // Draws a small coloured window on the virtual screen and reads it back: proves the plugin view works
    svr.Get ("/api/editor/selftest", [this] (const httplib::Request&, httplib::Response& res)
    {
        if (viewer == nullptr || ! viewer->isDisplayAvailable()) { reply (res, errVar ("No virtual screen.")); return; }
        auto t = onMT ([this] { return engine.screenTest (true); });
        Thread::sleep (400);
        const auto px = viewer->live().pixelAt ((int) t["x"], (int) t["y"]);
        onMT ([this] { return engine.screenTest (false); });
        auto* o = new DynamicObject();
        o->setProperty ("windowCreated", (bool) t["peer"]);
        o->setProperty ("pixel", String::toHexString ((int) (px & 0xffffff)).paddedLeft ('0', 6));
        const int r = (int) ((px >> 16) & 255), g = (int) ((px >> 8) & 255), b = (int) (px & 255);
        o->setProperty ("ok", (bool) t["peer"] && r > 180 && g < 60 && b > 70 && b < 160);
        reply (res, var (o));
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
