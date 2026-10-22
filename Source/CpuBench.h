#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <chrono>
#include <functional>
#include <thread>

// Measures how much of one CPU core a processor needs, by feeding it noise in real-time-paced blocks.
struct CpuBenchResult
{
    double avgPct = 0.0;    // average time per block, as % of the time one block lasts
    double peakPct = 0.0;   // slowest block
    int blocks = 0;
};

inline CpuBenchResult runCpuBench (const std::function<void (juce::AudioBuffer<float>&, juce::MidiBuffer&)>& process,
                                   int channels, double sampleRate, int blockSize, double seconds = 1.5)
{
    using clock = std::chrono::steady_clock;

    juce::AudioBuffer<float> buf (juce::jmax (1, channels), blockSize);
    juce::MidiBuffer midi;
    juce::Random rng (1234);

    const double budget = blockSize / sampleRate;
    const int warmup = 50;
    const int total = juce::jmax (50, (int) (sampleRate * seconds / blockSize));

    double sum = 0.0, peak = 0.0;
    auto next = clock::now();

    for (int i = 0; i < warmup + total; ++i)
    {
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        {
            auto* d = buf.getWritePointer (ch);
            for (int s = 0; s < blockSize; ++s)
                d[s] = (rng.nextFloat() * 2.0f - 1.0f) * 0.1f;
        }
        midi.clear();

        const auto t0 = clock::now();
        process (buf, midi);
        const double dt = std::chrono::duration<double> (clock::now() - t0).count();

        if (i >= warmup)
        {
            sum += dt;
            peak = juce::jmax (peak, dt);
        }

        // Pace like a real audio callback so caches and CPU clocks behave as they would live
        next += std::chrono::duration_cast<clock::duration> (std::chrono::duration<double> (budget));
        std::this_thread::sleep_until (next);
    }

    return { 100.0 * (sum / total) / budget, 100.0 * peak / budget, total };
}
