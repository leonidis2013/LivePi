#pragma once
#include <httplib.h>
#include <memory>
#include "OsStatus.h"
#include "OnlineUpdate.h"
#include <juce_cryptography/juce_cryptography.h>
#include <thread>
#include <vector>
#include <map>
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
    void saveAuth();
    void loadTrusted();
    bool checkBasic (const httplib::Request&) const;     // old "Authorization: Basic" (scripts); any user name
    bool isTrusted (const httplib::Request&) const;
    bool verifyPassword (const juce::String& pw) const;
    bool protectionActive() const;                       // a password exists and protection is on
    bool hasSession (const httplib::Request&) const;
    bool isAuthed (const httplib::Request&) const;       // may use LivePi right now
    std::string newSession();                            // returns the Set-Cookie value
    void saveSessions();
    static juce::String clientIp (const httplib::Request&);
    juce::String saveTrusted (const juce::StringArray& entries);   // empty = ok, else error
    juce::var sessionInfo (const httplib::Request&);
    int lockoutSeconds (const std::string& ip) const;      // > 0: this client must wait before trying again
    static constexpr int kMaxFails = 10;
    static constexpr juce::int64 kLockMs = 10 * 60 * 1000;

    struct Net { juce::uint32 addr, mask; };
    mutable juce::CriticalSection trustedLock;   // everything below can change at run time
    std::vector<Net> trusted;         // from ~/.livepi/trusted.txt (IPv4 only)
    juce::StringArray trustedText;
    bool trustedAccess = false;       // ~/.livepi/access.txt "on": trusted networks skip the login (needs active protection)

    juce::String authSalt, authHash;  // empty = no password set (~/.livepi/auth.json)
    bool protectionOn = true;         // auth.json "enabled"
    struct Session { juce::String hash; juce::int64 expires; };
    std::vector<Session> sessions;    // ~/.livepi/sessions.json (hashed tokens)
    std::map<std::string, std::pair<int, juce::int64>> loginFails;   // per client: count, last time

    HostEngine& engine;
    EditorViewer* viewer = nullptr;
    httplib::Server svr;
    std::thread thread;
    int port;
    std::unique_ptr<InternetMonitor> internet;
    std::unique_ptr<OnlineUpdate> ghUpdate;
};
