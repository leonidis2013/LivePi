// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki
#pragma once

// Mixer mode: one mono channel with the LiveVoice modules built in (LiveVoice's own DSP code, fetched
// from its repository at build time), in an order the user can change. Every module is zero latency
// (no lookahead, no oversampling, no FFT in the audio path), so any order adds no latency.
// Parameters, ids, ranges and factory presets are LiveVoice's, so the sound and settings match the plugin.

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <vector>
#include "Params.h"
#include "PresetManager.h"
#include "dsp/DynamicEq.h"
#include "dsp/DeEsser.h"
#include "dsp/FeedbackSuppressor.h"
#include "dsp/FeedbackDetector.h"
#include "dsp/PrimarySourceExpander.h"
#include "dsp/VocalCompressor.h"
#include "dsp/LevelTone.h"
#include "dsp/HighPass.h"
#include "dsp/SoftClip.h"

class VoiceChannel : public juce::AudioProcessor
{
public:
    enum Module { hpf = 0, pse, feedback, eq, deess, prox, leveler, warmth, comp, numModules };
    static const char* moduleId (int m);           // "hpf", "pse", ...
    static int moduleFromId (const juce::String& id);
    static juce::String moduleName (int m);
    static juce::StringArray moduleParams (int m); // the parameter ids shown on that module's panel
    static juce::String moduleOnParam (int m);     // "" for warmth (on = amount above 0)

    VoiceChannel();
    ~VoiceChannel() override;

    // ---- order (message thread writes, audio thread reads) ----
    std::array<int, numModules> getOrder() const noexcept;
    bool setOrder (const std::array<int, numModules>& order);   // must be a permutation

    // ---- for the page ----
    juce::var getView (bool full = true);                        // full: values + order; always: live meters, notches
    juce::var getSpectrum (bool output, int module);             // RTA bins + module curve (dB)
    juce::var setParam (const juce::String& id, double value);   // real (not normalised) value
    juce::var setParamNorm (const juce::String& id, double norm); // 0..1 (from a knob)
    juce::var notchCommand (const juce::String& cmd, int slot);  // clearFloating, clearAll, removeSlot, makeFixed
    juce::var loadFactoryPreset (int index);
    juce::StringArray factoryPresetNames() const { return presets.getFactoryNames(); }
    juce::String getPresetName() const { return presets.getCurrentName(); }
    bool isModuleOn (int m) const;

    // ---- AudioProcessor ----
    const juce::String getName() const override { return "LivePi Voice"; }
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainInputChannelSet() == juce::AudioChannelSet::mono() && l.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
    }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    // Lock-free ring the audio thread writes and the page's RTA reads
    struct ScopeTap
    {
        static constexpr int size = 1 << 14;
        void push (const float* d, int n) noexcept
        {
            int w = writePos.load (std::memory_order_relaxed);
            for (int i = 0; i < n; ++i) { buffer[(size_t) w] = d[i]; w = (w + 1) & (size - 1); }
            writePos.store (w, std::memory_order_release);
        }
        void readLatest (float* dest, int n) const noexcept
        {
            const int w = writePos.load (std::memory_order_acquire);
            for (int i = 0; i < n; ++i) dest[i] = buffer[(size_t) ((w - n + i) & (size - 1))];
        }
        std::array<float, size> buffer {};
        std::atomic<int> writePos { 0 };
    };

    void updateSettings() noexcept;
    void resetChain() noexcept;
    float getNotchQ() const noexcept;
    void sendNotch (const dsp::FeedbackSuppressor::Command& c);

    PresetManager presets { apvts };

    dsp::HighPass highPass;
    dsp::FeedbackSuppressor fbSup;
    dsp::FeedbackDetector detector { fbSup };
    dsp::DynamicEq dynEq;
    dsp::DeEsser deEsser;
    dsp::PrimarySourceExpander pseFx;
    dsp::VocalCompressor compFx;
    dsp::ProximityComp proximity;
    dsp::AutoLeveler levelerFx;
    dsp::Warmth warmthFx;

    ScopeTap inputTap, outputTap;
    std::atomic<float> inputPeakDb { -100.0f }, outputPeakDb { -100.0f };
    std::atomic<juce::uint64> packedOrder;
    std::atomic<double> sr { 48000.0 };
    juce::SmoothedValue<float> inGainSmooth, outGainSmooth;
    juce::AudioBuffer<float> rawBuf;
    float bypassMix = 0.0f;
    bool fullyBypassed = false, eqWasOn = true;
    int lastDetectCount = 0;
    juce::SpinLock uiCommandLock;
    juce::CriticalSection fftLock;

    // Parameter values, looked up once (never by name on the audio thread)
    struct Band { std::atomic<float> *on, *type, *freq, *gain, *q, *dyn, *thr, *range, *ratio, *att, *rel; };
    struct Cache
    {
        std::atomic<float> *inGain, *outGain, *bypass,
            *fbOn, *fbMode, *fbSens, *fbLearn, *fbFreeze, *fbMaxFixed, *fbMaxFloat, *fbMaxDepth, *fbWidth, *fbHold, *fbLo, *fbHi,
            *hpfOn, *hpfFreq, *hpfSlope, *eqOn,
            *dsOn, *dsFreq, *dsThr, *dsRange, *dsMode, *dsQ, *dsRel, *dsRelative, *dsRelThr, *dsFollow,
            *dsShOn, *dsShFreq, *dsShQ, *dsShThr, *dsShRelThr, *dsShRange,
            *pseOn, *pseThr, *pseRange, *pseAttack, *pseHold, *pseRelease, *pseScHp, *pseScLp,
            *compOn, *compThr, *compRatio, *compKnee, *compAttack, *compRelease, *compAuto, *compMakeup, *compMix, *compScHp, *compAutoGain,
            *lvlOn, *lvlTarget, *lvlRange, *lvlSpeed, *proxOn, *proxAmount, *warmth;
        std::array<Band, params::numEqBands> bands;
    } p;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceChannel)
};
