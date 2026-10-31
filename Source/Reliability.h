#pragma once

// Everything that keeps the audio running without dropouts or silence:
//  - RtPlayer: runs the audio callback on a real-time (SCHED_FIFO) thread, optionally pinned to a CPU
//    core reserved for audio, with denormals flushed to zero. (JUCE starts its ALSA thread as an
//    ordinary thread, so without this any busy moment on the Pi could cause a dropout.)
//  - lockMemory: keeps LivePi's memory in RAM so the audio never waits for the SD card or swap.
//  - SdNotify: talks to systemd's watchdog, so a frozen LivePi is restarted automatically.
//  - PiHealth: undervoltage and throttling, the two most common hidden causes of dropouts on a Pi.

#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <string>
#include <vector>

extern char** environ;

#ifndef MCL_ONFAULT
 #define MCL_ONFAULT 4
#endif

namespace Reliability
{
    inline std::atomic<int> memLockResult { -1 };   // 0 = locked, -2 = only partly (limited), errno otherwise, -1 = not tried

    // Lock pages into RAM as they are first used (MCL_ONFAULT), so nothing the audio has touched can be
    // swapped out or dropped from the page cache, without pre-allocating every thread's full stack.
    // MCL_FUTURE only with an unlimited memlock limit (the installer sets one): with a finite limit,
    // new threads and buffers would start failing once it is used up.
    inline void lockMemory()
    {
        rlimit rl {};
        const bool unlimited = getrlimit (RLIMIT_MEMLOCK, &rl) == 0 && rl.rlim_cur == RLIM_INFINITY;
        memLockResult = mlockall (MCL_CURRENT | MCL_ONFAULT | (unlimited ? MCL_FUTURE : 0)) == 0 ? (unlimited ? 0 : -2) : errno;
    }

    // The highest CPU the kernel keeps free for us (isolcpus=...), or -1.
    inline int isolatedCpu()
    {
        const auto s = juce::File ("/sys/devices/system/cpu/isolated").loadFileAsString().trim();
        if (s.isEmpty())
            return -1;
        int best = -1;
        for (auto part : juce::StringArray::fromTokens (s, ",", ""))
        {
            auto last = part.fromLastOccurrenceOf ("-", false, false).trim();
            if (last.containsOnly ("0123456789") && last.isNotEmpty())
                best = juce::jmax (best, last.getIntValue());
        }
        return best;
    }

    // ------------------------------------------------------------------ audio callback
    class RtPlayer : public juce::AudioProcessorPlayer
    {
    public:
        static constexpr int rtPriority = 80;     // below the kernel's own IRQ/watchdog threads (99), above everything else

        RtPlayer()
        {
            pinCpu = isolatedCpu();
            if (auto* h = std::getenv ("LIVEPI_TEST_HANG_AFTER"))   // CI only: freeze the audio to test the watchdog
                hangAfterSec = std::atof (h);
        }

        std::atomic<juce::uint64> heartbeat { 0 };      // +1 per audio block
        std::atomic<bool> inCallback { false };         // true while the plugins are processing
        std::atomic<double> lastStartMs { 0.0 };        // when the device last (re)started
        std::atomic<bool> rtOk { false };
        std::atomic<int> rtError { 0 };
        std::atomic<int> rtCpu { -1 };
        int pinCpu = -1;

        void audioDeviceAboutToStart (juce::AudioIODevice* d) override
        {
            needPromote = true;
            lastStartMs = juce::Time::getMillisecondCounterHiRes();
            juce::AudioProcessorPlayer::audioDeviceAboutToStart (d);
        }

        void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                               int numSamples, const juce::AudioIODeviceCallbackContext& ctx) override
        {
            const juce::ScopedNoDenormals noDenormals;   // denormals can make filters and reverbs suddenly very slow
            if (needPromote.exchange (false, std::memory_order_relaxed))
                promote();

            inCallback.store (true, std::memory_order_relaxed);
            if (hangAfterSec > 0.0 && (testSamples += numSamples) > (juce::int64) (hangAfterSec * 48000.0))
                for (;;) std::this_thread::sleep_for (std::chrono::seconds (1));

            juce::AudioProcessorPlayer::audioDeviceIOCallbackWithContext (in, numIn, out, numOut, numSamples, ctx);
            inCallback.store (false, std::memory_order_relaxed);
            heartbeat.fetch_add (1, std::memory_order_relaxed);
        }

    private:
        std::atomic<bool> needPromote { true };
        double hangAfterSec = 0.0;
        juce::int64 testSamples = 0;

        // Once per device start, on the audio thread itself
        void promote()
        {
            sched_param p {};
            p.sched_priority = rtPriority;
            const int r = pthread_setschedparam (pthread_self(), SCHED_FIFO, &p);
            rtError = r;
            rtOk = r == 0;
            rtCpu = -1;
            if (pinCpu >= 0)
            {
                cpu_set_t set;
                CPU_ZERO (&set);
                CPU_SET (pinCpu, &set);
                if (pthread_setaffinity_np (pthread_self(), sizeof (set), &set) == 0)
                    rtCpu = pinCpu;
            }
        }
    };

    // ------------------------------------------------------------------ systemd watchdog
    class SdNotify
    {
    public:
        SdNotify()
        {
            if (auto* s = std::getenv ("NOTIFY_SOCKET"); s != nullptr && *s != 0)
            {
                path = s;
                ::unsetenv ("NOTIFY_SOCKET");    // helpers we start must not talk to systemd for us
            }
        }

        bool available() const { return path.isNotEmpty(); }

        bool send (const char* msg) const
        {
            if (path.isEmpty())
                return false;
            const int fd = ::socket (AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            if (fd < 0)
                return false;
            sockaddr_un a {};
            a.sun_family = AF_UNIX;
            const auto p = path.toStdString();
            if (p.size() >= sizeof (a.sun_path)) { ::close (fd); return false; }
            std::memcpy (a.sun_path, p.data(), p.size());
            if (a.sun_path[0] == '@') a.sun_path[0] = 0;     // abstract socket
            const auto len = (socklen_t) (offsetof (sockaddr_un, sun_path) + p.size());
            const bool ok = ::sendto (fd, msg, std::strlen (msg), MSG_NOSIGNAL, (sockaddr*) &a, len) >= 0;
            ::close (fd);
            return ok;
        }

    private:
        juce::String path;
    };

    // ------------------------------------------------------------------ Raspberry Pi power / throttling
    // vcgencmd get_throttled bits: 0 undervoltage now, 1 frequency capped now, 2 throttled now,
    // 3 soft temperature limit now; 16..19 the same "has happened since boot".
    // Runs a command and returns its output. Uses posix_spawn (vfork-style), not fork: forking a
    // process whose memory is locked makes the audio thread hit copy-on-write page faults.
    inline juce::String spawnRead (const std::vector<std::string>& args, int timeoutMs, int* exitCode = nullptr)
    {
        int pfd[2];
        if (::pipe2 (pfd, O_CLOEXEC) != 0)
            return {};
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init (&fa);
        posix_spawn_file_actions_adddup2 (&fa, pfd[1], 1);
        posix_spawn_file_actions_addopen (&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2 (&fa, pfd[1], 2);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back (const_cast<char*> (a.c_str()));
        argv.push_back (nullptr);
        pid_t pid = 0;
        const int r = posix_spawn (&pid, argv[0], &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy (&fa);
        ::close (pfd[1]);
        if (r != 0) { ::close (pfd[0]); if (exitCode != nullptr) *exitCode = -1; return {}; }

        std::string out;
        char buf[256];
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        for (;;)
        {
            const int left = (int) (end - juce::Time::getMillisecondCounter());
            pollfd p { pfd[0], POLLIN, 0 };
            if (left <= 0 || ::poll (&p, 1, left) <= 0) { ::kill (pid, SIGKILL); break; }
            const auto n = ::read (pfd[0], buf, sizeof (buf));
            if (n <= 0) break;
            out.append (buf, (size_t) n);
            if (out.size() > 4096) break;
        }
        ::close (pfd[0]);
        int status = 0;
        ::waitpid (pid, &status, 0);
        if (exitCode != nullptr)
            *exitCode = WIFEXITED (status) ? WEXITSTATUS (status) : -1;
        return juce::String (out);
    }

    class PiHealth
    {
    public:
        PiHealth() : worker ([this] { run(); }) {}
        ~PiHealth()
        {
            stop = true;
            if (worker.joinable()) worker.join();
        }

        std::atomic<int> throttled { -1 };    // -1 = unknown (not a Pi, or no way to read it)

    private:
        std::atomic<bool> stop { false };
        std::thread worker;

        // Newer Raspberry Pi kernels publish the same value in sysfs: no process needed at all
        static juce::File sysfsFile()
        {
            const juce::File plat ("/sys/devices/platform");
            for (auto& a : plat.findChildFiles (juce::File::findDirectories, false))
            {
                if (a.getChildFile ("get_throttled").existsAsFile()) return a.getChildFile ("get_throttled");
                for (auto& b : a.findChildFiles (juce::File::findDirectories, false))
                    if (b.getFileName().contains ("firmware") && b.getChildFile ("get_throttled").existsAsFile())
                        return b.getChildFile ("get_throttled");
            }
            return {};
        }

        static int parseHex (juce::String s)
        {
            s = s.fromFirstOccurrenceOf ("0x", false, true).trim();
            if (s.isEmpty()) return -1;
            s = s.initialSectionContainingOnly ("0123456789abcdefABCDEF");
            return s.isEmpty() ? -1 : s.getHexValue32();
        }

        void run()
        {
            const auto sys = sysfsFile();
            std::string exe;
            for (auto* p : { "/usr/bin/vcgencmd", "/opt/vc/bin/vcgencmd" })
                if (juce::File (p).existsAsFile()) { exe = p; break; }
            if (! sys.existsAsFile() && exe.empty())
                return;

            while (! stop.load())
            {
                int v = -1;
                if (sys.existsAsFile())
                {
                    const auto c = sys.loadFileAsString().trim();
                    v = parseHex (c.containsIgnoreCase ("0x") ? c : "0x" + c);
                }
                if (v < 0 && ! exe.empty())
                    v = parseHex (spawnRead ({ exe, "get_throttled" }, 2000));   // "throttled=0x50000"
                if (v >= 0)
                    throttled = v;
                for (int i = 0; i < 50 && ! stop.load(); ++i)
                    std::this_thread::sleep_for (std::chrono::milliseconds (100));
            }
        }
    };
}
