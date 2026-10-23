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

    const juce::String getName() const override { return "Strip"; }

    void prepareToPlay (double sampleRate, int) override
    {
        if (ring != nullptr)
            ring->rate.store (sampleRate);
        smoothed.reset (sampleRate, 0.02);
        smoothed.setCurrentAndTargetValue (target());
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
    {
        if (buf.getNumChannels() < 1)
            return;

        auto* d = buf.getWritePointer (0);
        const int n = buf.getNumSamples();

        float pi = 0.0f;
        for (int i = 0; i < n; ++i)
            pi = juce::jmax (pi, std::abs (d[i]));

        if (! meterOnly)
        {
            smoothed.setTargetValue (target());
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
    juce::SmoothedValue<float> smoothed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};
