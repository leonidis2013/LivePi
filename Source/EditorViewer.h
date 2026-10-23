#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>

// Makes plugin windows reachable from the browser. Three helpers run as children of LiveHost:
//   Xvfb       a virtual screen (only when no DISPLAY was given to us)
//   x11vnc     shares that screen on localhost:5900 with a random password
//   websockify serves the noVNC viewer on port 6080 and bridges it to x11vnc
// Running them from here (instead of systemd units) means an online update is enough to get
// them, a helper that dies is restarted, and the web page can show exactly what is wrong.
// All methods run on the message thread.
class EditorViewer : private juce::Timer
{
public:
    static constexpr int vncPort = 5900, webPort = 6080;

    explicit EditorViewer (const juce::File& dataDir) : dir (dataDir)
    {
        dir.createDirectory();
        logFile = dir.getChildFile ("viewer.log");
        logFile.replaceWithText ({});
        startDisplay();
        if (displayOk)
            startHelpers();
        startTimer (3000);
    }

    ~EditorViewer() override
    {
        stopTimer();
        stop (vnc);
        stop (web);
        // Xvfb is left alone on purpose: killing it while JUCE still talks to it would abort the
        // shutdown. systemd stops it with the rest of the service.
    }

    bool isDisplayAvailable() const { return displayOk; }

    juce::var getStatus() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("available", displayOk);
        o->setProperty ("display", displayName);
        o->setProperty ("displaySource", displaySource);
        o->setProperty ("vnc", vncState);
        o->setProperty ("web", webState);
        o->setProperty ("webReachable", portListening (webPort));
        o->setProperty ("port", webPort);
        o->setProperty ("password", password);
        o->setProperty ("problem", problem);
        o->setProperty ("log", logTail());
        return juce::var (o);
    }

    void restartHelpers()
    {
        stop (vnc);
        stop (web);
        restarts = 0;
        problem.clear();
        if (displayOk)
            startHelpers();
    }

private:
    juce::File dir, logFile, authFile;
    juce::ChildProcess xvfb, vnc, web;
    juce::String displayName, displaySource, vncState = "stopped", webState = "stopped", password, problem;
    bool displayOk = false, haveVnc = false, haveWeb = false;
    int restarts = 0, healthyTicks = 0;

    static void stop (juce::ChildProcess& p)
    {
        if (p.isRunning())
        {
            p.kill();
            p.waitForProcessToFinish (2000);   // reaps it, so no zombie is left behind
        }
    }

    static juce::String which (const char* name)
    {
        for (auto* d : { "/usr/bin/", "/usr/local/bin/", "/bin/" })
            if (juce::File (juce::String (d) + name).existsAsFile())
                return juce::String (d) + name;
        return {};
    }

    static bool unixSocketAlive (const juce::String& path)
    {
        const int fd = ::socket (AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_un a {};
        a.sun_family = AF_UNIX;
        path.copyToUTF8 (a.sun_path, sizeof (a.sun_path));
        const bool ok = ::connect (fd, (sockaddr*) &a, sizeof (a)) == 0;
        ::close (fd);
        return ok;
    }

    static bool portListening (int port)
    {
        const int fd = ::socket (AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons ((uint16_t) port);
        a.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        const bool ok = ::connect (fd, (sockaddr*) &a, sizeof (a)) == 0;
        ::close (fd);
        return ok;
    }

    void log (const juce::String& s) const
    {
        logFile.appendText (juce::Time::getCurrentTime().toString (false, true) + "  " + s + "\n");
        juce::Logger::writeToLog ("viewer: " + s);
    }

    juce::String logTail() const
    {
        auto all = logFile.loadFileAsString();
        auto lines = juce::StringArray::fromLines (all);
        lines.removeEmptyStrings();
        // Xvfb prints pages of harmless keymap warnings; keep the tail useful
        for (int i = lines.size(); --i >= 0;)
            if (lines[i].contains ("Could not resolve keysym") || lines[i].contains ("xkbcomp") || lines[i].contains ("XKEYBOARD"))
                lines.remove (i);
        while (lines.size() > 12) lines.remove (0);
        return lines.joinIntoString ("\n");
    }

    // Runs "cmd" with its output appended to the log. exec keeps the pid equal to the real program so kill() works.
    bool spawn (juce::ChildProcess& p, const juce::String& cmd)
    {
        return p.start (juce::StringArray { "/bin/sh", "-c", "exec " + cmd + " >> '" + logFile.getFullPathName() + "' 2>&1" }, 0);
    }

    void startDisplay()
    {
        if (auto* d = std::getenv ("DISPLAY"); d != nullptr && *d != 0)
        {
            displayName = d;
            displaySource = "service";
            displayOk = true;
            return;
        }

        displayName = ":99";
        const juce::String sock ("/tmp/.X11-unix/X99");
        if (unixSocketAlive (sock))
        {
            displaySource = "existing";
        }
        else
        {
            auto exe = which ("Xvfb");
            if (exe.isEmpty())
            {
                problem = "Xvfb is not installed. Re-run the installer (without --no-editor) to add plugin window support.";
                log (problem);
                return;
            }
            // A screen big enough for large plugin windows; x11vnc only scans parts that change.
            if (! spawn (xvfb, exe + " :99 -screen 0 1920x1080x24 -nolisten tcp -nocursor"))
            {
                problem = "Could not start Xvfb.";
                log (problem);
                return;
            }
            displaySource = "livehost";
            for (int i = 0; i < 50 && ! unixSocketAlive (sock); ++i)
                juce::Thread::sleep (100);
            if (! unixSocketAlive (sock))
            {
                problem = "Xvfb did not start. See the viewer log.";
                log (problem);
                return;
            }
        }
        ::setenv ("DISPLAY", displayName.toRawUTF8(), 1);
        displayOk = true;
        log ("display " + displayName + " (" + displaySource + ")");
    }

    void startHelpers()
    {
        auto x11vnc = which ("x11vnc"), websockify = which ("websockify");
        haveVnc = x11vnc.isNotEmpty();
        haveWeb = websockify.isNotEmpty() && juce::File ("/usr/share/novnc/vnc.html").existsAsFile();

        if (! haveVnc || ! haveWeb)
        {
            problem = "The viewer programs are missing (x11vnc, websockify, novnc). Re-run the installer to add them.";
            vncState = haveVnc ? vncState : "missing";
            webState = haveWeb ? webState : "missing";
            log (problem);
            return;
        }

        if (! vnc.isRunning())
        {
            if (portListening (vncPort))
            {
                problem = "Port 5900 is already used by another VNC server (an old livehost-vnc service?). Re-run the installer to remove it.";
                vncState = "external";
                log (problem);
            }
            else
            {
                if (password.isEmpty())
                {
                    password = makePassword();
                    authFile = dir.getChildFile ("vnc.pass");
                    juce::ChildProcess mk;
                    mk.start (juce::StringArray { x11vnc, "-storepasswd", password, authFile.getFullPathName() }, 0);
                    mk.waitForProcessToFinish (5000);
                    authFile.setReadOnly (false);
                    chmod (authFile.getFullPathName().toRawUTF8(), 0600);
                    auto txt = dir.getChildFile ("vnc.txt");
                    txt.replaceWithText (password);
                    chmod (txt.getFullPathName().toRawUTF8(), 0600);
                }
                // nice: the audio thread is real-time anyway; this keeps screen polling off the other cores' priorities.
                // -defer/-wait limit the scan rate to ~20 fps so a busy plugin window cannot eat a core.
                const auto cmd = "nice -n 10 " + x11vnc + " -display " + displayName + " -localhost -rfbport " + juce::String (vncPort)
                               + " -rfbauth '" + authFile.getFullPathName() + "' -forever -shared -noxdamage -defer 50 -wait 50 -cursor arrow -quiet";
                vncState = spawn (vnc, cmd) ? "running" : "failed";
                log ("x11vnc " + vncState);
            }
        }

        if (! web.isRunning())
        {
            if (portListening (webPort))
            {
                problem = "Port 6080 is already used by another program (an old livehost-novnc service?). Re-run the installer to remove it.";
                webState = "external";
                log (problem);
            }
            else
            {
                const auto cmd = "nice -n 10 " + websockify + " --web=/usr/share/novnc " + juce::String (webPort) + " localhost:" + juce::String (vncPort);
                webState = spawn (web, cmd) ? "running" : "failed";
                log ("websockify " + webState);
            }
        }
    }

    static juce::String makePassword()
    {
        const char* chars = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        juce::String s;
        auto& r = juce::Random::getSystemRandom();
        for (int i = 0; i < 8; ++i)
            s << chars[r.nextInt ((int) std::strlen (chars))];
        return s;
    }

    void timerCallback() override
    {
        if (! displayOk || ! haveVnc || ! haveWeb)
            return;

        bool died = false;
        if (vncState == "running" && ! vnc.isRunning()) { vncState = "stopped"; died = true; log ("x11vnc exited (" + juce::String (vnc.getExitCode()) + ")"); }
        if (webState == "running" && ! web.isRunning()) { webState = "stopped"; died = true; log ("websockify exited (" + juce::String (web.getExitCode()) + ")"); }

        if (died)
        {
            healthyTicks = 0;
            if (++restarts > 5)
            {
                problem = "The viewer keeps stopping. See the log below, then use Restart viewer.";
                return;
            }
            startHelpers();
        }
        else if (vncState == "running" && webState == "running")
        {
            if (++healthyTicks >= 20) restarts = 0;   // a minute without trouble forgets earlier restarts
            if (problem.isNotEmpty() && ! problem.startsWith ("Port"))
                problem.clear();
        }
    }
};
