// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki
#pragma once

// Mixer mode: one mono channel with the LiveVoice modules built in (LiveVoice's own DSP code, fetched
// from its repository at build time), in an order the user can change. Every module is zero latency
// (no lookahead, no oversampling, no FFT in the audio path), so any order adds no latency.
// Parameters, ids, ranges and factory presets are LiveVoice's, so the sound and settings match the plugin.
// LivePi adds: a 6-band static parametric EQ module ("peq", LiveVoice's zero-latency SVF filters with
// every band free to be any type) and input polarity. Modules can be taken out of the flow and put back;
// a module out of the flow is not processed at all. The input gain is always first.

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
    enum Module { hpf = 0, pse, feedback, eq, deess, prox, leveler, warmth, comp, peq, numModules };
    static constexpr int numDefaultModules = 9;      // a new channel: LiveVoice's nine, in LiveVoice's order
    static const char* moduleId (int m);           // "hpf", "pse", ...
    static int moduleFromId (const juce::String& id);
    static juce::String moduleName (int m);
    static juce::StringArray moduleParams (int m); // the parameter ids shown on that module's panel
    static juce::String moduleOnParam (int m);     // "" for warmth (on = amount above 0)

    VoiceChannel();
    ~VoiceChannel() override;

    // ---- order (message thread writes, audio thread reads) ----
    std::vector<int> getOrder() const;
    bool setOrder (const std::vector<int>& order);   // 1..numModules different modules (the ones in the flow)
    bool inFlow (int m) const noexcept { return (flowMask.load() >> m) & 1u; }

    // ---- for the page ----
    juce::var getView (bool full = true);                        // full: values + order; always: live meters, notches
    juce::var getSpectrum (bool output, int module);             // RTA bins + module curve (dB)
    juce::var setParam (const juce::String& id, double value);   // real (not normalised) value
    juce::var setParamNorm (const juce::String& id, double norm); // 0..1 (from a knob)
    // clearFloating, clearAll, removeSlot, makeFixed; addFixed (freq, depth): a notch you set by hand;
    // setNotch (slot, freq, depth): move or deepen a notch (it becomes fixed)
    juce::var notchCommand (const juce::String& cmd, int slot, double freq = 0.0, double depthDb = -6.0);
    juce::var loadFactoryPreset (int index);
    juce::StringArray factoryPresetNames() const;   // LiveVoice's, then LivePi's own (Lapel)
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
    ::dsp::DynamicEq::BandSettings staticBand (int b) const noexcept;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
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
    dsp::DynamicEq staticEq;                   // the "peq" module: every band static
    dsp::Warmth warmthFx;

    ScopeTap inputTap, outputTap;
    std::atomic<float> inputPeakDb { -100.0f }, outputPeakDb { -100.0f };
    std::atomic<juce::uint64> packedOrder;
    std::atomic<double> sr { 48000.0 };
    juce::SmoothedValue<float> inGainSmooth, outGainSmooth, peqOutSmooth;
    std::atomic<juce::uint32> flowMask { 0 };
    juce::uint32 lastMask = 0;                  // audio thread: modules put back into the flow start clean
    juce::AudioBuffer<float> rawBuf;
    float bypassMix = 0.0f;
    bool fullyBypassed = false, eqWasOn = true, peqWasOn = true;
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
            *lvlOn, *lvlTarget, *lvlRange, *lvlSpeed, *proxOn, *proxAmount, *warmth,
            *polarity, *peqOn, *peqPropQ, *peqOut;
        std::array<Band, params::numEqBands> bands;
        struct PBand { std::atomic<float> *on, *type, *freq, *gain, *q, *slope; };
        std::array<PBand, 6> pbands;
    } p;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceChannel)
};
