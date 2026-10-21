#pragma once
#include <httplib.h>
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

    HostEngine& engine;
    httplib::Server svr;
    std::thread thread;
    int port;
};
