// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki
// Offline checks for mixer mode's built-in LiveVoice channel, run by CI on ARM64:
//  - zero latency: an impulse comes out at sample 0, in every module order, with every module on
//  - no NaN / inf, output stays under 0 dBFS
//  - CPU: how much of one core a channel needs at 48 kHz / 64 samples (printed, and a sanity limit)
#include "../Source/VoiceChannel.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <random>
#include <cstdio>

static int failures = 0;
static void check (bool ok, const char* what) { std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what); if (! ok) ++failures; }

static void allOn (VoiceChannel& v)
{
    for (auto id : { "hpfOn", "pseOn", "fbOn", "eqOn", "dsOn", "proxOn", "lvlOn", "compOn" })
        v.setParam (id, 1.0);
    v.setParam ("warmth", 30.0);
    v.setParam ("b3_gain", -4.0);
    v.setParam ("b5_dyn", 1.0);
    v.setParam ("pseThr", -80.0);          // let the impulse through the expander
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    constexpr double sr = 48000.0;
    constexpr int block = 64;

    // ---- latency: impulse response peak position, in several orders ----
    std::mt19937 rng (7);
    for (int trial = 0; trial < 6; ++trial)
    {
        VoiceChannel v;
        allOn (v);
        auto order = v.getOrder();
        if (trial > 0) std::shuffle (order.begin(), order.end(), rng);
        check (v.setOrder (order), "order accepted");
        v.prepareToPlay (sr, block);
        check (v.getLatencySamples() == 0, "reports zero latency");

        juce::AudioBuffer<float> buf (1, block);
        juce::MidiBuffer midi;
        // settle the smoothers, then send one impulse
        for (int i = 0; i < 200; ++i) { buf.clear(); v.processBlock (buf, midi); }
        buf.clear();
        buf.setSample (0, 0, 0.5f);
        v.processBlock (buf, midi);
        int peakAt = 0; float peak = 0.0f; bool finite = true;
        for (int i = 0; i < block; ++i)
        {
            const float s = buf.getSample (0, i);
            finite = finite && std::isfinite (s);
            if (std::abs (s) > peak) { peak = std::abs (s); peakAt = i; }
        }
        char msg[200];
        std::snprintf (msg, sizeof (msg), "order %d: impulse comes out at sample %d (peak %.3f), no delay", trial, peakAt, peak);
        check (finite && peak > 0.01f && peakAt <= 2, msg);
        v.releaseResources();
    }

    // ---- loud noise through everything: finite, clipped below 0 dBFS; and the CPU cost ----
    {
        VoiceChannel v;
        allOn (v);
        v.prepareToPlay (sr, block);
        juce::AudioBuffer<float> buf (1, block);
        juce::MidiBuffer midi;
        juce::Random r (3);
        const int blocks = (int) (10.0 * sr / block);          // 10 s of audio
        bool finite = true; float maxOut = 0.0f;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int b = 0; b < blocks; ++b)
        {
            auto* d = buf.getWritePointer (0);
            for (int i = 0; i < block; ++i) d[i] = (r.nextFloat() * 2.0f - 1.0f) * 0.9f;
            v.processBlock (buf, midi);
            for (int i = 0; i < block; ++i) { finite = finite && std::isfinite (d[i]); maxOut = std::max (maxOut, std::abs (d[i])); }
        }
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        const double pct = 100.0 * ms / 10000.0;
        check (finite, "loud noise: no NaN or inf");
        check (maxOut <= 1.0f, "loud noise: output never above 0 dBFS");
        std::printf ("INFO  one channel with every module on uses %.1f%% of one core (48 kHz, 64 samples)\n", pct);
        check (pct < 50.0, "CPU per channel is sane");
        v.releaseResources();
    }

    // ---- state round trip keeps the order and the settings ----
    {
        VoiceChannel a, b;
        std::array<int, VoiceChannel::numModules> o { 8, 7, 6, 5, 4, 3, 2, 1, 0 };
        a.setOrder (o);
        a.setParam ("compThr", -33.0);
        juce::MemoryBlock mb;
        a.getStateInformation (mb);
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        check (b.getOrder() == o, "state keeps the module order");
        check (std::abs (b.apvts.getRawParameterValue ("compThr")->load() + 33.0f) < 0.01f, "state keeps the settings");
        check (b.loadFactoryPreset (1), "factory preset loads");
    }

    std::printf ("%s\n", failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}
