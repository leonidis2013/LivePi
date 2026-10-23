#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <csignal>
#include <vector>
#include <string>
#include <cstdlib>

// Makes plugin windows reachable from the browser. Three helpers run as children of LiveHost:
//   Xvfb       a virtual screen (only when no DISPLAY was given to us)
//   x11vnc     shares that screen on localhost:5900 with a random password
//   websockify serves the noVNC viewer on port 6080 and bridges it to x11vnc
// Running them from here (instead of systemd units) means an online update is enough to get
// them, a helper that dies is restarted, and the web page can show exactly what is wrong.
// All methods run on the message thread.
// A child process that dies with LiveHost (PR_SET_PDEATHSIG), however LiveHost ends: a clean stop,
// a crash, or a kill -9. Its output goes to a log file.
class HelperProcess
{
public:
    ~HelperProcess() { stop(); }

    bool start (const juce::StringArray& args, const juce::File& logFile)
    {
        stop();
        std::vector<std::string> strs;
        for (auto& a : args) strs.push_back (a.toStdString());
        std::vector<char*> argv;
        for (auto& x : strs) argv.push_back (x.data());
        argv.push_back (nullptr);
        const auto logPath = logFile.getFullPathName().toStdString();

        const pid_t p = fork();
        if (p < 0) return false;
        if (p == 0)
        {
            prctl (PR_SET_PDEATHSIG, SIGTERM);
            if (getppid() == 1) _exit (1);           // parent already gone
            const int fd = open (logPath.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
            const int in = open ("/dev/null", O_RDONLY);
            if (in >= 0) dup2 (in, STDIN_FILENO);
            if (fd >= 0) { dup2 (fd, STDOUT_FILENO); dup2 (fd, STDERR_FILENO); }
            execvp (argv[0], argv.data());
            _exit (127);
        }
        pid = p;
        return true;
    }

    bool isRunning()
    {
        if (pid <= 0) return false;
        int st = 0;
        const pid_t r = waitpid (pid, &st, WNOHANG);
        if (r == 0) return true;
        if (r == pid) { exitCode = WIFEXITED (st) ? WEXITSTATUS (st) : (WIFSIGNALED (st) ? 128 + WTERMSIG (st) : -1); }
        pid = 0;
        return false;
    }

    // SIGTERM, then SIGKILL if it does not go; reaps it so no zombie stays behind.
    void stop()
    {
        if (pid <= 0) return;
        ::kill (pid, SIGTERM);
        for (int i = 0; i < 20 && isRunning(); ++i) juce::Thread::sleep (50);
        if (isRunning()) { ::kill (pid, SIGKILL); for (int i = 0; i < 20 && isRunning(); ++i) juce::Thread::sleep (50); }
        pid = 0;
    }

    int getExitCode() const { return exitCode; }

private:
    pid_t pid = 0;
    int exitCode = 0;
};

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
        vnc.stop();
        web.stop();
        // Xvfb is left alone here on purpose: killing it while JUCE still talks to it would abort the
        // shutdown. It ends by itself when LiveHost has exited (PDEATHSIG).
        xvfb.release();
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
        vnc.stop();
        web.stop();
        restarts = 0;
        problem.clear();
        if (displayOk)
            startHelpers();
    }

private:
    juce::File dir, logFile, authFile;
    HelperProcess vnc, web;
    std::unique_ptr<HelperProcess> xvfb;
    juce::String displayName, displaySource, vncState = "stopped", webState = "stopped", password, problem;
    bool displayOk = false, haveVnc = false, haveWeb = false;
    int restarts = 0, healthyTicks = 0;

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

    bool spawn (HelperProcess& p, const juce::StringArray& args) { return p.start (args, logFile); }

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
            xvfb = std::make_unique<HelperProcess>();
            if (! spawn (*xvfb, { exe, ":99", "-screen", "0", "1920x1080x24", "-nolisten", "tcp", "-nocursor" }))
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
                    HelperProcess mk;
                    mk.start ({ x11vnc, "-storepasswd", password, authFile.getFullPathName() }, logFile);
                    for (int i = 0; i < 100 && mk.isRunning(); ++i) juce::Thread::sleep (50);
                    authFile.setReadOnly (false);
                    chmod (authFile.getFullPathName().toRawUTF8(), 0600);
                    auto txt = dir.getChildFile ("vnc.txt");
                    txt.replaceWithText (password);
                    chmod (txt.getFullPathName().toRawUTF8(), 0600);
                }
                // nice: the audio thread is real-time anyway; this keeps screen polling off the other cores' priorities.
                // -defer/-wait limit the scan rate to ~20 fps so a busy plugin window cannot eat a core.
                vncState = spawn (vnc, { "nice", "-n", "10", x11vnc, "-display", displayName, "-localhost", "-rfbport", juce::String (vncPort),
                                         "-rfbauth", authFile.getFullPathName(), "-forever", "-shared", "-noxdamage", "-defer", "50", "-wait", "50",
                                         "-cursor", "arrow", "-quiet" }) ? "running" : "failed";
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
                webState = spawn (web, { "nice", "-n", "10", websockify, "--web=/usr/share/novnc", juce::String (webPort), "localhost:" + juce::String (vncPort) }) ? "running" : "failed";
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
