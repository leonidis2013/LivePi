// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki
#pragma once

// The Pisound's button, read by LivePi itself (Blokas' pisound-btn program is not needed).
// The Pisound driver owns the button's GPIO (17) and shares it through sysfs, so that is read first;
// the GPIO character device is the fallback when no driver holds the line.
// Runs on its own thread, never the audio thread. Gestures:
//   1, 2 or 3 clicks (decided 0.4 s after the last click), hold 2 s (on release), hold 5 s (fires at 5 s).
// The LED flashes once when a hold reaches 2 s and again at 5 s, so you know when to let go.

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#if JUCE_LINUX
 #include <cerrno>
 #include <cstring>
 #include <fcntl.h>
 #include <linux/gpio.h>
 #include <poll.h>
 #include <sys/ioctl.h>
 #include <unistd.h>
#endif

class PisoundButton
{
public:
    enum Gesture { click1 = 0, click2, click3, hold, longHold, numGestures };
    enum Status { absent = 0, ok, denied, failed };
    static constexpr int clickWindowMs = 400, holdMs = 2000, longHoldMs = 5000, line = 17;

    std::function<void (int gesture)> onGesture;   // called on the button thread
    std::atomic<int> status { absent };
    std::atomic<bool> down { false };
    std::atomic<int> lastErrno { 0 };

    ~PisoundButton() { stop(); }

    void start()
    {
        if (th.joinable()) return;
        quit = false;
        th = std::thread ([this] { run(); });
    }
    void stop()
    {
        quit = true;
        if (th.joinable()) th.join();
    }

    static bool present() { return juce::File ("/sys/kernel/pisound/id").exists(); }

    // Flash the Pisound's LEDs (the driver takes a duration; 1 = one short flash)
    static void flashLed (int duration)
    {
       #if JUCE_LINUX
        const int fd = ::open ("/sys/kernel/pisound/led", O_WRONLY | O_CLOEXEC);
        if (fd < 0) return;
        const auto s = std::to_string (juce::jlimit (1, 255, duration)) + "\n";
        [[maybe_unused]] auto r = ::write (fd, s.data(), s.size());
        ::close (fd);
       #else
        juce::ignoreUnused (duration);
       #endif
    }
    static bool ledWritable()
    {
       #if JUCE_LINUX
        return ::access ("/sys/kernel/pisound/led", W_OK) == 0;
       #else
        return false;
       #endif
    }

    // Blokas' own button program, which would act on the same presses
    static bool blokasProgramRunning()
    {
        for (auto& d : juce::File ("/proc").findChildFiles (juce::File::findDirectories, false))
            if (d.getFileName().containsOnly ("0123456789") && d.getChildFile ("comm").loadFileAsString().trim() == "pisound-btn")
                return true;
        return false;
    }

    juce::String method() const { return usingSysfs.load() ? "sysfs" : "gpiochip"; }

private:
    std::thread th;
    std::atomic<bool> quit { false };
    std::atomic<bool> usingSysfs { false };

    static double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

    void sleepFor (int ms)
    {
        for (int t = 0; t < ms && ! quit.load(); t += 100)
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }

   #if JUCE_LINUX
    // ---- sysfs: /sys/class/gpio/gpio<base + 17>, exported by the Pisound driver
    struct Sysfs { int fd = -1; bool activeLow = true; };

    static juce::File sysfsDir()
    {
        static const char* const labels[] = { "pinctrl-rp1", "pinctrl-bcm2835", "pinctrl-bcm2711" };
        for (auto& chip : juce::File ("/sys/class/gpio").findChildFiles (juce::File::findDirectories, false, "gpiochip*"))
        {
            const auto label = chip.getChildFile ("label").loadFileAsString().trim();
            for (auto* l : labels)
                if (label == l)
                {
                    const int base = chip.getChildFile ("base").loadFileAsString().trim().getIntValue();
                    auto d = juce::File ("/sys/class/gpio").getChildFile ("gpio" + juce::String (base + line));
                    if (d.isDirectory()) return d;
                }
        }
        return {};
    }

    bool openSysfs (Sysfs& s)
    {
        auto d = sysfsDir();
        if (d == juce::File()) return false;
        auto edge = d.getChildFile ("edge");
        if (edge.loadFileAsString().trim() != "both")
        {
            const int e = ::open (edge.getFullPathName().toRawUTF8(), O_WRONLY | O_CLOEXEC);
            if (e < 0) { lastErrno = errno; status = (errno == EACCES || errno == EPERM) ? denied : failed; return true; }
            [[maybe_unused]] auto r = ::write (e, "both", 4);
            ::close (e);
        }
        s.activeLow = d.getChildFile ("active_low").loadFileAsString().trim() == "1";
        s.fd = ::open (d.getChildFile ("value").getFullPathName().toRawUTF8(), O_RDONLY | O_CLOEXEC);
        if (s.fd < 0) { lastErrno = errno; status = (errno == EACCES || errno == EPERM) ? denied : failed; }
        return true;
    }

    static int readSysfs (const Sysfs& s)    // 1 = pressed, 0 = released, -1 = error
    {
        char c = 0;
        if (::lseek (s.fd, 0, SEEK_SET) < 0 || ::read (s.fd, &c, 1) != 1) return -1;
        const bool high = c == '1';
        // With active_low set, sysfs already reports "pressed" as 1; the button pulls the pin low
        return (s.activeLow ? high : ! high) ? 1 : 0;
    }

    // ---- character device, when no driver holds the line
    int openChip()
    {
        static const char* const labels[] = { "pinctrl-rp1", "pinctrl-bcm2835", "pinctrl-bcm2711" };
        int denial = 0;
        for (int n = 0; n < 16; ++n)
        {
            const auto path = "/dev/gpiochip" + std::to_string (n);
            const int c = ::open (path.c_str(), O_RDWR | O_CLOEXEC);
            if (c < 0) { if (errno == EACCES || errno == EPERM) denial = errno; continue; }
            gpiochip_info info {};
            bool match = false;
            if (::ioctl (c, GPIO_GET_CHIPINFO_IOCTL, &info) == 0)
                for (auto* l : labels) match = match || std::strcmp (info.label, l) == 0;
            if (! match) { ::close (c); continue; }

            gpio_v2_line_request req {};
            req.offsets[0] = line;
            req.num_lines = 1;
            std::strncpy (req.consumer, "livepi", sizeof (req.consumer) - 1);
            req.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_ACTIVE_LOW | GPIO_V2_LINE_FLAG_BIAS_PULL_UP
                             | GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
            int r = ::ioctl (c, GPIO_V2_GET_LINE_IOCTL, &req);
            const int e = errno;
            ::close (c);
            if (r == 0)
            {
                ::fcntl (req.fd, F_SETFL, O_NONBLOCK);
                return req.fd;
            }
            lastErrno = e;
            status = (e == EACCES || e == EPERM) ? denied : failed;
            return -1;
        }
        lastErrno = denial != 0 ? denial : ENOENT;
        status = denial != 0 ? denied : failed;
        return -1;
    }

    static int readChip (int fd)
    {
        gpio_v2_line_event ev;
        while (::read (fd, &ev, sizeof (ev)) == (ssize_t) sizeof (ev)) {}   // drain
        gpio_v2_line_values v {};
        v.mask = 1;
        if (::ioctl (fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0) return -1;
        return (v.bits & 1) ? 1 : 0;   // active low is set on the request: 1 = pressed
    }
   #endif

    void fire (int g) { if (onGesture) onGesture (g); }

    void run()
    {
       #if JUCE_LINUX
        while (! quit.load())
        {
            if (! present()) { status = absent; sleepFor (5000); continue; }

            Sysfs s;
            int chipFd = -1;
            usingSysfs = openSysfs (s);
            if (usingSysfs.load() && s.fd < 0) { sleepFor (3000); continue; }
            if (! usingSysfs.load())
            {
                chipFd = openChip();
                if (chipFd < 0) { sleepFor (3000); continue; }
            }
            const int fd = usingSysfs.load() ? s.fd : chipFd;
            auto readNow = [&] { return usingSysfs.load() ? readSysfs (s) : readChip (fd); };

            status = ok;
            lastErrno = 0;
            int stable = readNow();
            if (stable < 0) stable = 0;
            down = stable == 1;

            int clicks = 0, holdStage = 0;
            double pressedAt = 0.0, windowEnd = 0.0;
            bool windowOpen = false;
            bool error = false;

            while (! quit.load() && ! error)
            {
                // Wake for the next deadline: click window, hold marks, or a quit check
                double next = nowMs() + 250.0;
                if (windowOpen && stable == 0) next = juce::jmin (next, windowEnd);
                if (stable == 1 && holdStage < 2) next = juce::jmin (next, pressedAt + (holdStage == 0 ? holdMs : longHoldMs));
                const int timeout = juce::jmax (1, (int) (next - nowMs()));

                pollfd p { fd, (short) (usingSysfs.load() ? (POLLPRI | POLLERR) : POLLIN), 0 };
                const int pr = ::poll (&p, 1, timeout);
                if (pr < 0 && errno != EINTR) { error = true; break; }

                if (pr > 0)
                {
                    // Debounce: take the level once it has settled
                    std::this_thread::sleep_for (std::chrono::milliseconds (8));
                    const int v = readNow();
                    if (v < 0) { error = true; break; }
                    if (usingSysfs.load()) { char c; ::lseek (fd, 0, SEEK_SET); [[maybe_unused]] auto r = ::read (fd, &c, 1); }
                    if (v != stable)
                    {
                        stable = v;
                        down = v == 1;
                        const double t = nowMs();
                        if (v == 1)
                        {
                            clicks = windowOpen ? clicks + 1 : 1;
                            windowOpen = false;
                            pressedAt = t;
                            holdStage = 0;
                        }
                        else
                        {
                            if (holdStage == 1) fire (hold);
                            if (holdStage >= 1) { clicks = 0; windowOpen = false; }
                            else { windowOpen = true; windowEnd = t + clickWindowMs; }
                            holdStage = 0;
                        }
                    }
                }

                const double t = nowMs();
                if (stable == 1)
                {
                    if (holdStage == 0 && t - pressedAt >= holdMs) { holdStage = 1; flashLed (1); }
                    if (holdStage == 1 && t - pressedAt >= longHoldMs) { holdStage = 2; flashLed (3); fire (longHold); }
                }
                else if (windowOpen && t >= windowEnd)
                {
                    windowOpen = false;
                    if (clicks >= 1 && clicks <= 3) fire (click1 + clicks - 1);
                    clicks = 0;
                }
            }
            ::close (fd);
            down = false;
            if (error && ! quit.load()) { status = failed; lastErrno = errno; sleepFor (3000); }
        }
       #endif
    }
};
