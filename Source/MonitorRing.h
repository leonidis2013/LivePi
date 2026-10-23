#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

// Last couple of seconds of one mono signal, written by the audio thread and read by the web server
// so the browser can play it ("Listen in this browser"). Only one strip writes at a time.
struct MonitorRing
{
    static constexpr uint64_t cap = 1u << 17, mask = cap - 1;

    void write (const float* d, int n) noexcept
    {
        const uint64_t p = pos.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
            buf[(p + (uint64_t) i) & mask] = d[i];
        pos.store (p + (uint64_t) n, std::memory_order_release);
    }

    // Copies everything after `since` as 16-bit samples. A new or far-behind reader starts ~100 ms back.
    void read (uint64_t since, std::vector<int16_t>& out, uint64_t& newPos) const
    {
        const uint64_t p = pos.load (std::memory_order_acquire);
        if (since == 0 || since > p || p - since > cap / 2)
            since = p > 4800 ? p - 4800 : 0;

        out.clear();
        out.reserve ((size_t) (p - since));
        for (uint64_t i = since; i < p; ++i)
        {
            float v = buf[i & mask];
            v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
            out.push_back ((int16_t) (v * 32767.0f));
        }
        newPos = p;
    }

    std::atomic<uint64_t> pos { 0 };
    std::atomic<double> rate { 48000.0 };

private:
    float buf[cap] {};
};
