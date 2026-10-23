#pragma once
#include <httplib.h>
#include <memory>
#include "OsStatus.h"
#include "OnlineUpdate.h"
#include <juce_cryptography/juce_cryptography.h>
#include <thread>
#include <vector>
#include "HostEngine.h"

class EditorViewer;

class WebServer
{
public:
    WebServer (HostEngine& e, int portToUse);
    void setViewer (EditorViewer* v) { viewer = v; }
    ~WebServer();

    void start();
    void stop();

private:
    void setupRoutes();
    void loadAuth();
    void loadTrusted();
    bool checkAuth (const httplib::Request&) const;
    bool isTrusted (const httplib::Request&) const;

    bool isLockedPath (const juce::String& path) const;
    bool mayUseLocked (const httplib::Request&) const;   // may this client use uploads, updates, reboot...?
    static juce::String clientIp (const httplib::Request&);
    juce::String saveTrusted (const juce::StringArray& entries);   // empty = ok, else error

    struct Net { juce::uint32 addr, mask; };
    mutable juce::CriticalSection trustedLock;   // trusted/trustedText/openAccess change at run time
    std::vector<Net> trusted;         // from ~/.livepi/trusted.txt (IPv4 only)
    juce::StringArray trustedText;
    bool openAccess = true;           // ~/.livepi/access.txt: "open" (default) or "locked"; only used without a password

    HostEngine& engine;
    EditorViewer* viewer = nullptr;
    httplib::Server svr;
    std::thread thread;
    int port;
    juce::String authSalt, authHash;   // empty = no password set
    std::unique_ptr<InternetMonitor> internet;
    std::unique_ptr<OnlineUpdate> ghUpdate;
};
