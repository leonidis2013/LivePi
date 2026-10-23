#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <csignal>
#include <vector>
#include <string>
#include <cstdlib>
#include "LiveView.h"

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
    void release() { pid = 0; }   // forget it without stopping it

private:
    pid_t pid = 0;
    int exitCode = 0;
};

// Plugin windows live on a virtual screen (Xvfb, started here when no DISPLAY was given) and are
// shown inside the web page by LiveView: no VNC, no second port, nothing else to install.
// Constructed on the message thread; getStatus() and live() are safe from any thread.
class EditorViewer
{
public:
    static constexpr int screenW = 1920, screenH = 1080;

    explicit EditorViewer (const juce::File& dataDir) : dir (dataDir)
    {
        dir.createDirectory();
        logFile = dir.getChildFile ("viewer.log");
        logFile.replaceWithText ({});
        startDisplay();
        if (displayOk)
        {
            auto e = view.open (displayName);
            if (e.isNotEmpty()) { problem = e; log (e); }
            else if (! view.canInput()) { problem = "Touch/mouse input needs libxtst6 (re-run the installer, or: sudo apt install libxtst6)"; log (problem); }
        }
    }

    ~EditorViewer()
    {
        // Xvfb is not stopped here: JUCE may still talk to it during shutdown. It ends by itself
        // once LiveHost has exited (PDEATHSIG).
        if (xvfb != nullptr) xvfb->release();
    }

    bool isDisplayAvailable() const { return displayOk; }
    LiveView& live() { return view; }

    juce::var getStatus() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("available", displayOk);
        o->setProperty ("display", displayName);
        o->setProperty ("displaySource", displaySource);
        o->setProperty ("input", view.canInput());
        o->setProperty ("problem", problem);
        o->setProperty ("log", logTail());
        return juce::var (o);
    }

private:
    juce::File dir, logFile;
    std::unique_ptr<HelperProcess> xvfb;
    juce::String displayName, displaySource, problem;
    bool displayOk = false;
    LiveView view;

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

    void log (const juce::String& s) const
    {
        logFile.appendText (juce::Time::getCurrentTime().toString (false, true) + "  " + s + "\n");
        juce::Logger::writeToLog ("viewer: " + s);
    }

    juce::String logTail() const
    {
        auto lines = juce::StringArray::fromLines (logFile.loadFileAsString());
        lines.removeEmptyStrings();
        for (int i = lines.size(); --i >= 0;)
            if (lines[i].contains ("Could not resolve keysym") || lines[i].contains ("xkbcomp") || lines[i].contains ("XKEYBOARD"))
                lines.remove (i);
        while (lines.size() > 12) lines.remove (0);
        return lines.joinIntoString ("\n");
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
            xvfb = std::make_unique<HelperProcess>();
            if (! xvfb->start ({ exe, ":99", "-screen", "0", juce::String (screenW) + "x" + juce::String (screenH) + "x24", "-nolisten", "tcp", "-nocursor" }, logFile))
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
                problem = "Xvfb did not start. See the log.";
                log (problem);
                return;
            }
        }
        ::setenv ("DISPLAY", displayName.toRawUTF8(), 1);
        displayOk = true;
        log ("virtual screen " + displayName + " (" + displaySource + ")");
    }
};
