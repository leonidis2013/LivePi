#pragma once
#include <httplib.h>
#include <memory>
#include "OsStatus.h"
#include "OnlineUpdate.h"
#include <juce_cryptography/juce_cryptography.h>
#include <thread>
#include <vector>
#include "HostEngine.h"

class WebServer
{
public:
    WebServer (HostEngine& e, int portToUse);
    ~WebServer();

    void start();
    void stop();

private:
    void setupRoutes();
    void loadAuth();
    void loadTrusted();
    bool checkAuth (const httplib::Request&) const;
    bool isTrusted (const httplib::Request&) const;

    struct Net { juce::uint32 addr, mask; };
    std::vector<Net> trusted;         // from ~/.livehost/trusted.txt (IPv4 only)
    juce::StringArray trustedText;

    HostEngine& engine;
    httplib::Server svr;
    std::thread thread;
    int port;
    juce::String authSalt, authHash;   // empty = no password set
    std::unique_ptr<InternetMonitor> internet;
    std::unique_ptr<OnlineUpdate> ghUpdate;
};
