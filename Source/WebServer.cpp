#include "WebServer.h"

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
}

WebServer::WebServer (HostEngine& e, int portToUse) : engine (e), port (portToUse)
{
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

void WebServer::setupRoutes()
{
    auto webDir = File::getSpecialLocation (File::currentExecutableFile)
                      .getParentDirectory().getChildFile ("web");
    svr.set_mount_point ("/", webDir.getFullPathName().toStdString());

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

    svr.Post ("/api/scan", [this] (const httplib::Request&, httplib::Response& res)
    {
        reply (res, onMT ([this] { return engine.scanPlugins(); }, 180000));
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
