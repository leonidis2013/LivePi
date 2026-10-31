#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cmath>
#include "MonitorRing.h"

// Mono in / mono out processor used at the start (meter only) and end
// (fader + mute + master gain + meter) of every channel chain.
class ChannelStrip : public juce::AudioProcessor
{
public:
    ChannelStrip (const std::atomic<float>* masterGain, bool meterOnlyMode)
        : AudioProcessor (BusesProperties()
                              .withInput  ("In",  juce::AudioChannelSet::mono(), true)
                              .withOutput ("Out", juce::AudioChannelSet::mono(), true)),
          master (masterGain), meterOnly (meterOnlyMode)
    {
    }

    // Message thread
    void setGainDb (float db) { gain.store (db <= -60.0f ? 0.0f : juce::Decibels::decibelsToGain (db)); }
    void setMuted (bool m)    { muted.store (m); }
    float takePeakIn()        { return peakIn.exchange (0.0f); }
    float takePeakOut()       { return peakOut.exchange (0.0f); }
    void setRing (MonitorRing* r)   { ring = r; }       // before the graph runs
    void setMonitorOn (bool on)     { monitorOn.store (on); }

    // Soft start: for `ms` the strip outputs silence and its meters ignore the audio (the plugins
    // before it still run, so their filters and level detectors settle), then it fades in.
    // Used when the audio device starts and when a session or preset is loaded, so start-up
    // thumps, DC from the converters and plugins that begin at full makeup gain never reach
    // the outputs or the meters.
    void settle (int ms = kSettleMs) { settleMs.store (ms); peakIn.store (0.0f); peakOut.store (0.0f); }
    static constexpr int kSettleMs = 800;

    const juce::String getName() const override { return "Strip"; }

    void prepareToPlay (double sampleRate, int) override
    {
        if (ring != nullptr)
            ring->rate.store (sampleRate);
        smoothed.reset (sampleRate, 0.02);
        smoothed.setCurrentAndTargetValue (target());
        fade.reset (sampleRate, 0.25);
        fade.setCurrentAndTargetValue (0.0f);
        sr = sampleRate;
        settleLeft = 0;
        settle();                     // the device (re)started
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
    {
        if (buf.getNumChannels() < 1)
            return;

        auto* d = buf.getWritePointer (0);
        const int n = buf.getNumSamples();

        if (const int ms = settleMs.exchange (-1); ms >= 0)
        {
            settleLeft = (juce::int64) (ms * sr / 1000.0);
            fade.setCurrentAndTargetValue (0.0f);
            if (settleLeft <= 0)
                fade.setTargetValue (1.0f);
        }
        if (settleLeft > 0)
        {
            settleLeft -= n;
            if (! meterOnly)
                buf.clear();
            if (settleLeft <= 0)
                fade.setTargetValue (1.0f);
            return;
        }

        float pi = 0.0f;
        for (int i = 0; i < n; ++i)
            pi = juce::jmax (pi, std::abs (d[i]));

        if (! meterOnly)
        {
            smoothed.setTargetValue (target());
            if (fade.isSmoothing())
                for (int i = 0; i < n; ++i)
                    d[i] *= smoothed.getNextValue() * fade.getNextValue();
            else
                for (int i = 0; i < n; ++i)
                    d[i] *= smoothed.getNextValue();
        }

        float po = 0.0f;
        for (int i = 0; i < n; ++i)
            po = juce::jmax (po, std::abs (d[i]));

        peakIn.store  (juce::jmax (peakIn.load(),  pi));
        peakOut.store (juce::jmax (peakOut.load(), po));

        if (ring != nullptr && monitorOn.load (std::memory_order_relaxed))
            ring->write (d, n);
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    float target() const
    {
        if (muted.load())
            return 0.0f;
        return gain.load() * (master != nullptr ? master->load() : 1.0f);
    }

    const std::atomic<float>* master;
    const bool meterOnly;
    MonitorRing* ring = nullptr;
    std::atomic<bool> monitorOn { false };
    std::atomic<float> gain { 1.0f };
    std::atomic<bool> muted { false };
    std::atomic<float> peakIn { 0.0f }, peakOut { 0.0f };
    juce::SmoothedValue<float> smoothed, fade;
    std::atomic<int> settleMs { -1 };      // set by the message thread, taken by the audio thread
    juce::int64 settleLeft = 0;            // audio thread only
    double sr = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};
