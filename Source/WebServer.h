#pragma once
#include <httplib.h>
#include <juce_cryptography/juce_cryptography.h>
#include <thread>
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
    bool checkAuth (const httplib::Request&) const;

    HostEngine& engine;
    httplib::Server svr;
    std::thread thread;
    int port;
    juce::String authSalt, authHash;   // empty = no password set
};
