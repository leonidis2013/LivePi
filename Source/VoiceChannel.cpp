// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki
#include "VoiceChannel.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>

using namespace juce;

// ------------------------------------------------------------------------------------- modules

const char* VoiceChannel::moduleId (int m)
{
    static const char* ids[numModules] { "hpf", "pse", "feedback", "eq", "deess", "prox", "leveler", "warmth", "comp", "peq" };
    return isPositiveAndBelow (m, (int) numModules) ? ids[m] : "";
}

int VoiceChannel::moduleFromId (const String& id)
{
    for (int m = 0; m < numModules; ++m)
        if (id == moduleId (m)) return m;
    return -1;
}

String VoiceChannel::moduleName (int m)
{
    static const char* names[numModules] { "High-pass", "PSE", "Feedback", "Dynamic EQ", "De-esser", "Proximity", "Leveler", "Warmth", "Compressor", "Parametric EQ" };
    return isPositiveAndBelow (m, (int) numModules) ? names[m] : "";
}

String VoiceChannel::moduleOnParam (int m)
{
    switch (m)
    {
        case hpf:      return params::hpfOn;
        case pse:      return params::pseOn;
        case feedback: return params::fbOn;
        case eq:       return params::eqOn;
        case deess:    return params::dsOn;
        case prox:     return params::proxOn;
        case leveler:  return params::lvlOn;
        case comp:     return params::compOn;
        case peq:      return "peqOn";
        default:       return {};
    }
}

StringArray VoiceChannel::moduleParams (int m)
{
    using namespace params;
    switch (m)
    {
        case hpf:      return { hpfOn, hpfFreq, hpfSlope };
        case pse:      return { pseOn, pseThr, pseRange, pseAttack, pseHold, pseRelease, pseScHp, pseScLp };
        case feedback: return { fbOn, fbMode, fbSens, fbLearn, fbFreeze, fbMaxDepth, fbWidth, fbMaxFixed, fbMaxFloat, fbHold, fbLoFreq, fbHiFreq };
        case eq:
        {
            StringArray a { eqOn };
            for (int b = 0; b < numEqBands; ++b)
            {
                a.add (bOn (b));
                if (b == 0 || b == numEqBands - 1) a.add (bType (b));
                for (auto* id : { "freq", "gain", "q", "dyn", "thr", "range", "ratio", "att", "rel" }) a.add (band (b, id));
            }
            return a;
        }
        case deess:    return { dsOn, dsMode, dsFreq, dsQ, dsThr, dsRange, dsRel, dsRelative, dsRelThr, dsFollow,
                                dsShOn, dsShFreq, dsShQ, dsShThr, dsShRelThr, dsShRange };
        case prox:     return { proxOn, proxAmount };
        case leveler:  return { lvlOn, lvlTarget, lvlRange, lvlSpeed };
        case warmth:   return { params::warmth };
        case comp:     return { compOn, compThr, compRatio, compKnee, compAttack, compRelease, compAuto, compMakeup, compMix, compScHp, compAutoGain };
        case peq:
        {
            StringArray a { "peqOn", "peqPropQ", "peqOut" };
            for (int b = 1; b <= 6; ++b)
                for (auto* k : { "on", "type", "freq", "gain", "q", "slope" }) a.add ("peq" + String (b) + "_" + k);
            return a;
        }
        default:       return {};
    }
}

// The flow in one atomic word: 4 bits per module (up to 12), the count in the top 4 bits
static juce::uint64 packOrder (const std::vector<int>& o)
{
    juce::uint64 v = (juce::uint64) o.size() << 60;
    for (size_t i = 0; i < o.size(); ++i) v |= ((juce::uint64) (o[i] & 15)) << (4 * i);
    return v;
}

struct Flow { int n = 0; int m[VoiceChannel::numModules] {}; };
static Flow unpackOrder (juce::uint64 v) noexcept
{
    Flow f;
    f.n = juce::jmin ((int) VoiceChannel::numModules, (int) (v >> 60));
    for (int i = 0; i < f.n; ++i) f.m[i] = (int) ((v >> (4 * i)) & 15);
    return f;
}

// ---- LivePi's own parameters, added to LiveVoice's
juce::AudioProcessorValueTreeState::ParameterLayout VoiceChannel::createLayout()
{
    auto layout = params::createLayout();
    auto dbA = AudioParameterFloatAttributes().withLabel ("dB")
                   .withStringFromValueFunction ([] (float v, int) { return (v > 0.05f ? "+" : "") + String (v, 1) + " dB"; });
    auto hzA = AudioParameterFloatAttributes().withLabel ("Hz")
                   .withStringFromValueFunction ([] (float v, int) { return v >= 1000.0f ? String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz" : String (roundToInt (v)) + " Hz"; });
    auto qA = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, v < 10.0f ? 2 : 1); });
    NormalisableRange<float> fr (20.0f, 20000.0f); fr.setSkewForCentre (1000.0f);
    NormalisableRange<float> qr (0.1f, 18.0f); qr.setSkewForCentre (1.0f);
    using P = AudioParameterFloat;
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "inPolarity", 1 }, "Polarity invert", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "peqOn", 1 }, "EQ On", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "peqPropQ", 1 }, "Proportional Q", false));
    layout.add (std::make_unique<P> (ParameterID { "peqOut", 1 }, "EQ Output", NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f, dbA));
    static const float freqs[6] { 100.0f, 250.0f, 700.0f, 2000.0f, 5000.0f, 10000.0f };
    static const int types[6] { 1, 0, 0, 0, 0, 2 };   // low shelf, 4 bells, high shelf
    for (int b = 1; b <= 6; ++b)
    {
        const auto n = "peq" + String (b) + "_", nm = "Band " + String (b) + " ";
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { n + "on", 1 }, nm + "On", true));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { n + "type", 1 }, nm + "Type",
                    StringArray { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch" }, types[b - 1]));
        layout.add (std::make_unique<P> (ParameterID { n + "freq", 1 }, nm + "Freq", fr, freqs[b - 1], hzA));
        layout.add (std::make_unique<P> (ParameterID { n + "gain", 1 }, nm + "Gain", NormalisableRange<float> (-18.0f, 18.0f, 0.1f), 0.0f, dbA));
        layout.add (std::make_unique<P> (ParameterID { n + "q", 1 }, nm + "Q", qr, b == 1 || b == 6 ? 0.71f : 1.0f, qA));
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { n + "slope", 1 }, nm + "Slope",
                    StringArray { "6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" }, 3));
    }
    return layout;
}

// ------------------------------------------------------------------------------------- setup

VoiceChannel::VoiceChannel()
    : AudioProcessor (BusesProperties().withInput ("In", AudioChannelSet::mono(), true)
                                       .withOutput ("Out", AudioChannelSet::mono(), true)),
      apvts (*this, nullptr, "LiveVoice", createLayout())
{
    std::vector<int> def;
    for (int i = 0; i < numDefaultModules; ++i) def.push_back (i);   // LiveVoice's own order
    setOrder (def);

    auto r = [this] (const String& id) { auto* a = apvts.getRawParameterValue (id); jassert (a != nullptr); return a; };
    using namespace params;
    p.inGain = r (inGain); p.outGain = r (outGain); p.bypass = r (bypass);
    p.fbOn = r (fbOn); p.fbMode = r (fbMode); p.fbSens = r (fbSens); p.fbLearn = r (fbLearn); p.fbFreeze = r (fbFreeze);
    p.fbMaxFixed = r (fbMaxFixed); p.fbMaxFloat = r (fbMaxFloat); p.fbMaxDepth = r (fbMaxDepth); p.fbWidth = r (fbWidth);
    p.fbHold = r (fbHold); p.fbLo = r (fbLoFreq); p.fbHi = r (fbHiFreq);
    p.hpfOn = r (hpfOn); p.hpfFreq = r (hpfFreq); p.hpfSlope = r (hpfSlope); p.eqOn = r (eqOn);
    p.dsOn = r (dsOn); p.dsFreq = r (dsFreq); p.dsThr = r (dsThr); p.dsRange = r (dsRange); p.dsMode = r (dsMode); p.dsQ = r (dsQ);
    p.dsRel = r (dsRel); p.dsRelative = r (dsRelative); p.dsRelThr = r (dsRelThr); p.dsFollow = r (dsFollow);
    p.dsShOn = r (dsShOn); p.dsShFreq = r (dsShFreq); p.dsShQ = r (dsShQ); p.dsShThr = r (dsShThr); p.dsShRelThr = r (dsShRelThr); p.dsShRange = r (dsShRange);
    p.pseOn = r (pseOn); p.pseThr = r (pseThr); p.pseRange = r (pseRange); p.pseAttack = r (pseAttack); p.pseHold = r (pseHold);
    p.pseRelease = r (pseRelease); p.pseScHp = r (pseScHp); p.pseScLp = r (pseScLp);
    p.compOn = r (compOn); p.compThr = r (compThr); p.compRatio = r (compRatio); p.compKnee = r (compKnee); p.compAttack = r (compAttack);
    p.compRelease = r (compRelease); p.compAuto = r (compAuto); p.compMakeup = r (compMakeup); p.compMix = r (compMix);
    p.compScHp = r (compScHp); p.compAutoGain = r (compAutoGain);
    p.lvlOn = r (lvlOn); p.lvlTarget = r (lvlTarget); p.lvlRange = r (lvlRange); p.lvlSpeed = r (lvlSpeed);
    p.proxOn = r (proxOn); p.proxAmount = r (proxAmount); p.warmth = r (params::warmth);
    p.polarity = r ("inPolarity"); p.peqOn = r ("peqOn"); p.peqPropQ = r ("peqPropQ"); p.peqOut = r ("peqOut");
    for (int b = 0; b < 6; ++b)
    {
        const auto n = "peq" + String (b + 1) + "_";
        auto& d = p.pbands[(size_t) b];
        d.on = r (n + "on"); d.type = r (n + "type"); d.freq = r (n + "freq"); d.gain = r (n + "gain"); d.q = r (n + "q"); d.slope = r (n + "slope");
    }
    for (int b = 0; b < numEqBands; ++b)
    {
        auto& d = p.bands[(size_t) b];
        d.on = r (bOn (b)); d.type = (b == 0 || b == numEqBands - 1) ? r (bType (b)) : nullptr;
        d.freq = r (bFreq (b)); d.gain = r (bGain (b)); d.q = r (bQ (b)); d.dyn = r (bDyn (b));
        d.thr = r (bThr (b)); d.range = r (bRange (b)); d.ratio = r (bRatio (b)); d.att = r (bAtt (b)); d.rel = r (bRel (b));
    }
}

VoiceChannel::~VoiceChannel()
{
    detector.release();
}

std::vector<int> VoiceChannel::getOrder() const
{
    const auto f = unpackOrder (packedOrder.load());
    return std::vector<int> (f.m, f.m + f.n);
}

bool VoiceChannel::setOrder (const std::vector<int>& order)
{
    if (order.empty() || order.size() > (size_t) numModules) return false;
    juce::uint32 mask = 0;
    for (int m : order)
    {
        if (! isPositiveAndBelow (m, (int) numModules) || ((mask >> m) & 1u)) return false;
        mask |= 1u << m;
    }
    flowMask = mask;
    packedOrder = packOrder (order);
    return true;
}

bool VoiceChannel::isModuleOn (int m) const
{
    if (m == warmth) return p.warmth->load() > 0.5f;
    const auto id = moduleOnParam (m);
    auto* v = id.isNotEmpty() ? apvts.getRawParameterValue (id) : nullptr;
    return v != nullptr && v->load() > 0.5f;
}

void VoiceChannel::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    setLatencySamples (0);
    constexpr int nCh = 1;
    fbSup.prepare (sampleRate, nCh);
    dynEq.prepare (sampleRate, nCh);
    deEsser.prepare (sampleRate, nCh);
    pseFx.prepare (sampleRate, nCh);
    compFx.prepare (sampleRate, nCh);
    proximity.prepare (sampleRate, nCh);
    highPass.prepare (sampleRate, nCh);
    levelerFx.prepare (sampleRate, nCh);
    warmthFx.prepare (sampleRate, nCh);
    staticEq.prepare (sampleRate, nCh);
    peqOutSmooth.reset (sampleRate, 0.02);
    peqOutSmooth.setCurrentAndTargetValue (::dsp::dbToGain (p.peqOut->load()));
    lastMask = flowMask.load();
    detector.prepare (sampleRate);
    rawBuf.setSize (1, std::max (8192, 2 * samplesPerBlock));
    bypassMix = p.bypass->load() > 0.5f ? 1.0f : 0.0f;
    fullyBypassed = bypassMix >= 1.0f;
    inGainSmooth.reset (sampleRate, 0.02);
    outGainSmooth.reset (sampleRate, 0.02);
    inGainSmooth.setCurrentAndTargetValue (::dsp::dbToGain (p.inGain->load()) * (p.polarity->load() > 0.5f ? -1.0f : 1.0f));
    outGainSmooth.setCurrentAndTargetValue (::dsp::dbToGain (p.outGain->load()));
    updateSettings();
}

// A static EQ band: any type on any band; proportional Q (console style) narrows a bell as its gain grows
::dsp::DynamicEq::BandSettings VoiceChannel::staticBand (int b) const noexcept
{
    using BT = params::BandType;
    const auto& d = p.pbands[(size_t) b];
    ::dsp::DynamicEq::BandSettings s;
    const int t = jlimit (0, 5, (int) d.type->load());
    s.on = d.on->load() > 0.5f;
    s.type = t == 1 ? BT::lowShelf : t == 2 ? BT::highShelf : t == 3 ? BT::highPass : t == 4 ? BT::lowPass : BT::bell;
    s.slope = params::slopeDbPerOct[jlimit (0, 5, (int) d.slope->load())];
    s.freq = d.freq->load();
    s.gainDb = d.gain->load();
    s.q = d.q->load();
    if (t == 5) { s.gainDb = -30.0f; s.q = jmax (s.q, 2.0f); }   // notch: a deep, narrow cut
    else if (t == 0 && p.peqPropQ->load() > 0.5f) s.q *= std::pow (10.0f, std::abs (s.gainDb) / 40.0f);
    s.q = jlimit (0.1f, 40.0f, s.q);
    s.dynamic = false;
    return s;
}

void VoiceChannel::releaseResources()
{
    detector.release();
}

float VoiceChannel::getNotchQ() const noexcept
{
    static constexpr float qs[] { 7.2f, 14.4f, 28.9f, 57.7f };   // 1/5, 1/10, 1/20, 1/40 octave
    return qs[jlimit (0, 3, (int) p.fbWidth->load())];
}

// Same conversion as LiveVoice's updateSettings(): parameters -> module settings, once per block
void VoiceChannel::updateSettings() noexcept
{
    ::dsp::FeedbackSuppressor::Settings fs;
    fs.on = p.fbOn->load() > 0.5f;
    fs.freeze = p.fbFreeze->load() > 0.5f;
    fs.maxFixed = (int) p.fbMaxFixed->load();
    fs.maxFloat = (int) p.fbMaxFloat->load();
    fs.maxDepthDb = p.fbMaxDepth->load();
    fs.q = getNotchQ();
    fs.holdSeconds = p.fbHold->load();
    fbSup.setSettings (fs);

    ::dsp::FeedbackDetector::Settings ds;
    ds.enabled = fs.on && p.bypass->load() < 0.5f && inFlow (feedback);
    ds.learn = p.fbLearn->load() > 0.5f;
    ds.freeze = fs.freeze;
    ds.mode = (int) p.fbMode->load();
    ds.sensitivity = p.fbSens->load();
    ds.loFreq = p.fbLo->load();
    ds.hiFreq = p.fbHi->load();
    detector.setSettings (ds);

    for (int b = 0; b < params::numEqBands; ++b)
    {
        const auto& d = p.bands[(size_t) b];
        ::dsp::DynamicEq::BandSettings s;
        s.on = d.on->load() > 0.5f;
        const auto shape = params::bandShapeFromChoice (b, d.type != nullptr ? (int) d.type->load() : 0);
        s.type = shape.type; s.slope = shape.slope;
        s.freq = d.freq->load(); s.gainDb = d.gain->load(); s.q = d.q->load();
        s.dynamic = d.dyn->load() > 0.5f;
        s.thresholdDb = d.thr->load(); s.rangeDb = d.range->load(); s.ratio = d.ratio->load();
        s.attackMs = d.att->load(); s.releaseMs = d.rel->load();
        dynEq.setBand (b, s);
    }

    ::dsp::DeEsser::Settings de;
    de.on = p.dsOn->load() > 0.5f;
    de.mode = jlimit (0, 2, (int) p.dsMode->load());
    de.relative = p.dsRelative->load() > 0.5f;
    de.follow = p.dsFollow->load() > 0.5f;
    de.releaseMs = p.dsRel->load();
    de.bands[0] = { true, p.dsFreq->load(), p.dsQ->load(), p.dsThr->load(), p.dsRelThr->load(), p.dsRange->load() };
    de.bands[1] = { p.dsShOn->load() > 0.5f, p.dsShFreq->load(), p.dsShQ->load(), p.dsShThr->load(), p.dsShRelThr->load(), p.dsShRange->load() };
    deEsser.setSettings (de);

    ::dsp::PrimarySourceExpander::Settings ps;
    ps.on = p.pseOn->load() > 0.5f;
    ps.thresholdDb = p.pseThr->load(); ps.rangeDb = p.pseRange->load(); ps.attackMs = p.pseAttack->load();
    ps.holdMs = p.pseHold->load(); ps.releaseMs = p.pseRelease->load();
    ps.scHpHz = p.pseScHp->load(); ps.scLpHz = p.pseScLp->load();
    pseFx.setSettings (ps);

    ::dsp::VocalCompressor::Settings cs;
    cs.on = p.compOn->load() > 0.5f;
    cs.thresholdDb = p.compThr->load(); cs.ratio = p.compRatio->load(); cs.kneeDb = p.compKnee->load();
    cs.attackMs = p.compAttack->load(); cs.releaseMs = p.compRelease->load(); cs.autoRelease = p.compAuto->load() > 0.5f;
    cs.makeupDb = p.compMakeup->load(); cs.mix = p.compMix->load() * 0.01f; cs.scHpHz = p.compScHp->load();
    cs.autoGain = p.compAutoGain->load() > 0.5f;
    compFx.setSettings (cs);

    highPass.setSettings ({ p.hpfOn->load() > 0.5f, p.hpfFreq->load(), jlimit (0, 5, (int) p.hpfSlope->load()) });
    proximity.setSettings ({ p.proxOn->load() > 0.5f, p.proxAmount->load() });
    ::dsp::AutoLeveler::Settings ls;
    ls.on = p.lvlOn->load() > 0.5f; ls.targetDb = p.lvlTarget->load(); ls.rangeDb = p.lvlRange->load();
    ls.speed = jlimit (0, 2, (int) p.lvlSpeed->load());
    levelerFx.setSettings (ls);
    warmthFx.setAmount (p.warmth->load());
    for (int b = 0; b < 6; ++b) staticEq.setBand (b, staticBand (b));
}

void VoiceChannel::resetChain() noexcept
{
    highPass.reset(); pseFx.reset(); fbSup.reset(); dynEq.reset(); deEsser.reset(); warmthFx.reset(); staticEq.reset();
    inGainSmooth.setCurrentAndTargetValue (::dsp::dbToGain (p.inGain->load()) * (p.polarity->load() > 0.5f ? -1.0f : 1.0f));
    outGainSmooth.setCurrentAndTargetValue (::dsp::dbToGain (p.outGain->load()));
}

// ------------------------------------------------------------------------------------- audio

void VoiceChannel::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    const ScopedNoDenormals noDenormals;
    const int total = buffer.getNumSamples();
    if (total == 0 || buffer.getNumChannels() == 0)
        return;

    auto peak = [] (std::atomic<float>& meter, const float* d, int n)
    {
        float pk = 0.0f;
        for (int i = 0; i < n; ++i) pk = std::max (pk, std::abs (d[i]));
        const float db = ::dsp::gainToDb (pk);
        if (db > meter.load (std::memory_order_relaxed)) meter.store (db, std::memory_order_relaxed);
    };

    const int cap = rawBuf.getNumSamples();
    for (int start = 0; start < total; start += cap)
    {
        const int n = std::min (cap, total - start);
        float* ptr = buffer.getWritePointer (0, start);
        AudioBuffer<float> main (&ptr, 1, n);
        rawBuf.copyFrom (0, 0, main, 0, 0, n);

        updateSettings();

        const bool wantBypass = p.bypass->load() > 0.5f;
        if (wantBypass && bypassMix >= 1.0f)
        {
            fullyBypassed = true;
            inputTap.push (ptr, n); outputTap.push (ptr, n);
            peak (inputPeakDb, ptr, n); peak (outputPeakDb, ptr, n);
            continue;
        }
        if (fullyBypassed) { resetChain(); fullyBypassed = false; }

        // ---- input trim ----
        // polarity flips through the 20 ms gain ramp, so it never clicks
        inGainSmooth.setTargetValue (::dsp::dbToGain (p.inGain->load()) * (p.polarity->load() > 0.5f ? -1.0f : 1.0f));
        if (inGainSmooth.isSmoothing()) for (int i = 0; i < n; ++i) ptr[i] *= inGainSmooth.getNextValue();
        else main.applyGain (0, 0, n, inGainSmooth.getTargetValue());
        peak (inputPeakDb, ptr, n);
        inputTap.push (ptr, n);

        // ---- the modules, in the chosen order (all zero latency) ----
        const auto flow = unpackOrder (packedOrder.load (std::memory_order_acquire));
        // a module put back into the flow starts from silence, not from where it was when taken out
        const juce::uint32 mask = flowMask.load (std::memory_order_acquire), added = mask & ~lastMask;
        if (added != 0)
        {
            if (added & (1u << hpf)) highPass.reset();
            if (added & (1u << pse)) pseFx.reset();
            if (added & (1u << feedback)) fbSup.reset();
            if (added & (1u << eq)) dynEq.reset();
            if (added & (1u << deess)) deEsser.reset();
            if (added & (1u << prox)) proximity.reset();
            if (added & (1u << leveler)) levelerFx.reset();
            if (added & (1u << warmth)) warmthFx.reset();
            if (added & (1u << comp)) compFx.reset();
            if (added & (1u << peq)) staticEq.reset();
        }
        lastMask = mask;
        for (int fi = 0; fi < flow.n; ++fi)
        {
            const int m = flow.m[fi];
            switch (m)
            {
                case hpf:      highPass.process (main, n); break;
                case pse:      pseFx.process (main, n); break;
                case feedback: fbSup.process (main, n); break;
                case eq:
                {
                    const bool on = p.eqOn->load() > 0.5f;
                    if (on && ! eqWasOn) dynEq.reset();
                    eqWasOn = on;
                    if (on) dynEq.process (main, n);
                    break;
                }
                case deess:    deEsser.process (main, n); break;
                case prox:     proximity.process (main, n); break;
                case leveler:
                    if (const int dc = detector.getDetectionCount(); dc != lastDetectCount)
                    {
                        lastDetectCount = dc;
                        levelerFx.holdBoost (4.0);   // a ring was just found: never turn up into it
                    }
                    levelerFx.process (main, n);
                    break;
                case warmth:   warmthFx.process (main, n); break;
                case comp:     compFx.process (main, n); break;
                case peq:
                {
                    const bool on = p.peqOn->load() > 0.5f;
                    if (on && ! peqWasOn) staticEq.reset();
                    peqWasOn = on;
                    peqOutSmooth.setTargetValue (on ? ::dsp::dbToGain (p.peqOut->load()) : 1.0f);
                    if (on) staticEq.process (main, n);
                    if (peqOutSmooth.isSmoothing()) for (int i = 0; i < n; ++i) ptr[i] *= peqOutSmooth.getNextValue();
                    else if (! approximatelyEqual (peqOutSmooth.getTargetValue(), 1.0f)) main.applyGain (0, 0, n, peqOutSmooth.getTargetValue());
                    break;
                }
                default: break;
            }
        }

        // the feedback detector listens to what goes to the speakers (after every module, before the output trim)
        detector.pushSamples (ptr, n);

        // ---- output trim, then protection: only the top 1 dB is touched, nothing passes 0 dBFS ----
        outGainSmooth.setTargetValue (::dsp::dbToGain (p.outGain->load()));
        if (outGainSmooth.isSmoothing()) for (int i = 0; i < n; ++i) ptr[i] *= outGainSmooth.getNextValue();
        else main.applyGain (0, 0, n, outGainSmooth.getTargetValue());
        ::dsp::SoftClip::process (ptr, n);

        // ---- bypass crossfade (10 ms) ----
        if (wantBypass || bypassMix > 0.0f)
        {
            const float target = wantBypass ? 1.0f : 0.0f;
            const float step = 1.0f / (0.010f * (float) sr.load());
            float mix = bypassMix;
            const float* raw = rawBuf.getReadPointer (0);
            for (int i = 0; i < n; ++i)
            {
                mix = target > mix ? std::min (target, mix + step) : std::max (target, mix - step);
                ptr[i] = (1.0f - mix) * ptr[i] + mix * raw[i];
            }
            bypassMix = mix;
        }

        peak (outputPeakDb, ptr, n);
        outputTap.push (ptr, n);
    }
}

// ------------------------------------------------------------------------------------- state

void VoiceChannel::sendNotch (const ::dsp::FeedbackSuppressor::Command& c)
{
    const SpinLock::ScopedLockType lock (uiCommandLock);   // one producer for the UI queue
    fbSup.pushUiCommand (c);
}

void VoiceChannel::getStateInformation (MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<XmlElement> xml (state.createXml());
    auto* notches = xml->createNewChildElement ("FixedNotches");
    for (int i = 0; i < ::dsp::FeedbackSuppressor::maxSlots; ++i)
    {
        const auto nt = fbSup.getNotch (i);
        if (nt.kind == ::dsp::FeedbackSuppressor::kindFixed && nt.targetDb < -0.01f)
        {
            auto* e = notches->createNewChildElement ("Notch");
            e->setAttribute ("freq", nt.freq);
            e->setAttribute ("depth", nt.targetDb);
        }
    }
    StringArray ord;
    for (int m : getOrder()) ord.add (moduleId (m));
    auto* lp = xml->createNewChildElement ("LivePi");
    lp->setAttribute ("order", ord.joinIntoString (","));
    lp->setAttribute ("preset", presets.getCurrentName());
    copyXmlToBinary (*xml, destData);
}

void VoiceChannel::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    if (auto* lp = xml->getChildByName ("LivePi"))
    {
        auto ids = StringArray::fromTokens (lp->getStringAttribute ("order"), ",", "");
        std::vector<int> o;
        for (auto& id : ids) o.push_back (moduleFromId (id.trim()));
        setOrder (o);   // ignored when not valid
        presets.setCurrentName (lp->getStringAttribute ("preset", "Default"));
    }

    ::dsp::FeedbackSuppressor::Command clear;
    clear.type = ::dsp::FeedbackSuppressor::Command::Type::clearAll;
    sendNotch (clear);
    if (auto* notches = xml->getChildByName ("FixedNotches"))
        for (auto* e : notches->getChildIterator())
        {
            const double f = e->getDoubleAttribute ("freq", 1000.0), d = e->getDoubleAttribute ("depth", -6.0);
            if (! std::isfinite (f) || ! std::isfinite (d) || f < 20.0 || f > 20000.0) continue;
            ::dsp::FeedbackSuppressor::Command c;
            c.type = ::dsp::FeedbackSuppressor::Command::Type::setFixed;
            c.freq = (float) f;
            c.depthDb = (float) jlimit (-40.0, 0.0, d);
            sendNotch (c);
        }

    for (auto* tag : { "FixedNotches", "UiState", "LivePi" })
        xml->deleteAllChildElementsWithTagName (tag);
    // a corrupt value (NaN / inf) would poison the filters: drop it so the parameter keeps its default
    for (int i = xml->getNumChildElements(); --i >= 0;)
        if (auto* e = xml->getChildElement (i); e != nullptr && e->hasTagName ("PARAM") && ! std::isfinite (e->getDoubleAttribute ("value", 0.0)))
            xml->removeChildElement (e, true);
    // ring-out learn never comes back on by itself
    for (auto* e : xml->getChildWithTagNameIterator ("PARAM"))
        if (e->getStringAttribute ("id") == params::fbLearn) e->setAttribute ("value", 0.0);
    apvts.replaceState (ValueTree::fromXml (*xml));
}

// ------------------------------------------------------------------------------------- page

static var paramVar (RangedAudioParameter* rp)
{
    auto* o = new DynamicObject();
    o->setProperty ("id", rp->getParameterID());
    o->setProperty ("name", rp->getName (64));
    const float norm = rp->getValue();
    o->setProperty ("norm", norm);
    o->setProperty ("value", rp->convertFrom0to1 (norm));
    o->setProperty ("text", rp->getText (norm, 64));
    o->setProperty ("def", rp->getDefaultValue());
    const auto& range = rp->getNormalisableRange();
    o->setProperty ("min", range.start);
    o->setProperty ("max", range.end);
    if (auto* c = dynamic_cast<AudioParameterChoice*> (rp))
    {
        o->setProperty ("type", "choice");
        Array<var> ch; for (auto& s : c->choices) ch.add (s);
        o->setProperty ("choices", ch);
        o->setProperty ("index", c->getIndex());
    }
    else if (dynamic_cast<AudioParameterBool*> (rp) != nullptr) o->setProperty ("type", "bool");
    else if (dynamic_cast<AudioParameterInt*> (rp) != nullptr)  o->setProperty ("type", "int");
    else o->setProperty ("type", "float");
    return var (o);
}

var VoiceChannel::getView (bool full)
{
    auto* o = new DynamicObject();
    Array<var> ord;
    for (int m : getOrder()) ord.add (String (moduleId (m)));
    o->setProperty ("order", ord);
    o->setProperty ("preset", presets.getCurrentName());
    Array<var> names; for (auto& n : factoryPresetNames()) names.add (n);
    o->setProperty ("presets", names);
    o->setProperty ("latency", getLatencySamples());

    auto* mods = new DynamicObject();
    for (int m = 0; m < numModules; ++m)
    {
        auto* mo = new DynamicObject();
        mo->setProperty ("name", moduleName (m));
        mo->setProperty ("on", isModuleOn (m));
        mo->setProperty ("onParam", moduleOnParam (m));
        mo->setProperty ("inFlow", inFlow (m));
        if (full)
        {
            Array<var> ps;
            for (auto& id : moduleParams (m))
                if (auto* rp = apvts.getParameter (id)) ps.add (paramVar (rp));
            mo->setProperty ("params", ps);
        }
        mods->setProperty (moduleId (m), var (mo));
    }
    // the input stage: always first in the flow, can't be removed
    {
        auto* mo = new DynamicObject();
        mo->setProperty ("name", "Input");
        mo->setProperty ("on", true);
        mo->setProperty ("onParam", "");
        mo->setProperty ("fixed", true);
        if (full)
        {
            Array<var> ps;
            for (auto* id : { "inGain", "inPolarity" }) if (auto* rp = apvts.getParameter (id)) ps.add (paramVar (rp));
            mo->setProperty ("params", ps);
        }
        mods->setProperty ("input", var (mo));
    }
    o->setProperty ("modules", var (mods));
    for (auto* id : { "inGain", "outGain", "bypass" })
        if (auto* rp = apvts.getParameter (id)) o->setProperty (id, paramVar (rp));

    o->setProperty ("live", [this]
    {
        auto* l = new DynamicObject();
        l->setProperty ("inPeak", inputPeakDb.exchange (-100.0f));
        l->setProperty ("outPeak", outputPeakDb.exchange (-100.0f));
        l->setProperty ("pseKey", pseFx.getKeyLevelDb());
        l->setProperty ("pseGr", pseFx.getGainReductionDb());
        l->setProperty ("pseOpen", pseFx.getIsOpen());
        l->setProperty ("compGr", compFx.getGainReductionDb());
        l->setProperty ("compIn", compFx.getInputLevelDb());
        l->setProperty ("compMakeup", compFx.getMakeupDb());
        l->setProperty ("compOut", compFx.getInputLevelDb() - compFx.getGainReductionDb() + compFx.getMakeupDb());
        l->setProperty ("lvlGain", levelerFx.getGainDb());
        l->setProperty ("lvlLevel", levelerFx.getLevelDb());
        l->setProperty ("lvlHold", levelerFx.isHoldingBoost());
        l->setProperty ("proxCut", proximity.getCutDb());
        l->setProperty ("dsGr", deEsser.getCurrentGainReductionDb());
        Array<var> eqDyn, eqDet;
        for (int b = 0; b < params::numEqBands; ++b) { eqDyn.add (dynEq.getDynamicGainDb (b)); eqDet.add (dynEq.getDetectorLevelDb (b)); }
        l->setProperty ("eqDyn", eqDyn);
        l->setProperty ("eqDet", eqDet);
        Array<var> notches;
        for (int i = 0; i < ::dsp::FeedbackSuppressor::maxSlots; ++i)
        {
            const auto nt = fbSup.getNotch (i);
            if (nt.kind == ::dsp::FeedbackSuppressor::kindInactive) continue;
            auto* no = new DynamicObject();
            no->setProperty ("slot", i);
            no->setProperty ("freq", nt.freq);
            no->setProperty ("depth", nt.depthDb);
            no->setProperty ("target", nt.targetDb);
            no->setProperty ("fixed", nt.kind == ::dsp::FeedbackSuppressor::kindFixed);
            notches.add (var (no));
        }
        l->setProperty ("notches", notches);
        l->setProperty ("fbCount", detector.getDetectionCount());
        l->setProperty ("fbLast", detector.getLastDetectionFreq());
        return var (l);
    }());
    return var (o);
}

var VoiceChannel::setParam (const String& id, double value)
{
    auto* rp = apvts.getParameter (id);
    if (rp == nullptr || ! std::isfinite (value)) return var();
    const float norm = rp->convertTo0to1 ((float) jlimit ((double) rp->getNormalisableRange().start, (double) rp->getNormalisableRange().end, value));
    rp->beginChangeGesture();
    rp->setValueNotifyingHost (norm);
    rp->endChangeGesture();
    // a preset changed by hand is marked as edited (gain staging and bypass are not part of a preset)
    if (PresetManager::isPresetParameter (id) && ! presets.getCurrentName().endsWith (" (edited)"))
        presets.setCurrentName (presets.getCurrentName() + " (edited)");
    return paramVar (rp);
}

var VoiceChannel::setParamNorm (const String& id, double norm)
{
    auto* rp = apvts.getParameter (id);
    if (rp == nullptr || ! std::isfinite (norm)) return var();
    return setParam (id, rp->convertFrom0to1 ((float) jlimit (0.0, 1.0, norm)));
}

var VoiceChannel::notchCommand (const String& cmd, int slot, double freq, double depthDb)
{
    using C = ::dsp::FeedbackSuppressor::Command;
    C c;
    if (cmd == "addFixed" || cmd == "setNotch")
    {
        if (! std::isfinite (freq) || freq < 20.0 || freq > 20000.0 || ! std::isfinite (depthDb)) return var (false);
        if (cmd == "setNotch")
        {
            if (! isPositiveAndBelow (slot, (int) ::dsp::FeedbackSuppressor::maxSlots)) return var (false);
            C r; r.type = C::Type::removeSlot; r.slot = slot; sendNotch (r);
        }
        c.type = C::Type::setFixed;
        c.freq = (float) freq;
        c.depthDb = (float) jlimit (-40.0, -1.0, depthDb);
        sendNotch (c);
        return var (true);
    }
    if (cmd == "clearFloating") c.type = C::Type::clearFloating;
    else if (cmd == "clearAll") c.type = C::Type::clearAll;
    else if (cmd == "removeSlot") { c.type = C::Type::removeSlot; c.slot = slot; }
    else if (cmd == "makeFixed") { c.type = C::Type::makeFixed; c.slot = slot; }
    else return var (false);
    sendNotch (c);
    return var (true);
}

// ---- LivePi's own voice presets, after LiveVoice's
static const char* const livePiPresets[] { "Lapel / Lavalier Mic (Speech)" };

StringArray VoiceChannel::factoryPresetNames() const
{
    auto n = presets.getFactoryNames();
    for (auto* x : livePiPresets) n.add (x);
    return n;
}

var VoiceChannel::loadFactoryPreset (int index)
{
    const int lv = presets.getFactoryNames().size();
    if (index >= lv && index < lv + (int) std::size (livePiPresets))
    {
        // Lapel: worn on the chest, well below and off to the side of the mouth. So: more gain before
        // feedback is needed, no proximity boom, a chest resonance around 700-800 Hz, too little
        // presence (the mouth fires over it), clothing rustle and thumps down low, and the level moves
        // as the head turns. Start from the speech preset and adjust.
        if (! (bool) loadFactoryPreset (0)) return var (false);
        using namespace params;
        const std::pair<String, double> v[] {
            { fbMode, 0 }, { fbSens, 65 },
            { hpfOn, 1 }, { hpfFreq, 130 }, { hpfSlope, 3 },
            { bOn (1), 1 }, { bFreq (1), 200 }, { bGain (1), -1 }, { bDyn (1), 1 },
            { bOn (2), 1 }, { bFreq (2), 750 }, { bGain (2), -3 }, { bQ (2), 1.2 }, { bDyn (2), 0 },
            { bOn (3), 1 }, { bFreq (3), 1200 }, { bGain (3), 0 },
            { bOn (4), 1 }, { bFreq (4), 4000 }, { bGain (4), 3.5 }, { bQ (4), 0.9 }, { bDyn (4), 0 },
            { bOn (5), 1 }, { bFreq (5), 10000 }, { bGain (5), 2 },
            { proxOn, 0 }, { lvlOn, 1 }, { lvlTarget, -26 },
            { pseOn, 1 }, { pseThr, -50 }, { pseRange, 6 },
            { compOn, 1 }, { compThr, -26 }, { compRatio, 2.5 },
        };
        for (auto& [id, val] : v)
            if (auto* rp = apvts.getParameter (id))
                rp->setValueNotifyingHost (rp->convertTo0to1 ((float) jlimit ((double) rp->getNormalisableRange().start, (double) rp->getNormalisableRange().end, val)));
        presets.setCurrentName (livePiPresets[index - lv]);
        return var (true);
    }

    // LiveVoice's voice presets don't know LivePi's parametric EQ or polarity: keep those as they are
    std::vector<std::pair<RangedAudioParameter*, float>> keep;
    for (auto* ap : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (ap))
            if (rp->getParameterID().startsWith ("peq") || rp->getParameterID() == "inPolarity")
                keep.emplace_back (rp, rp->getValue());
    const bool ok = presets.loadFactory (index);
    for (auto& [rp, v] : keep) rp->setValueNotifyingHost (v);
    return var (ok);
}

// RTA of the channel's input or output (1/12 octave, 20 Hz .. 20 kHz) plus the selected module's curve
var VoiceChannel::getSpectrum (bool output, int module)
{
    constexpr int order = 12, size = 1 << order, points = 120;
    static const std::vector<float> freqs = []
    {
        std::vector<float> f ((size_t) points);
        for (int i = 0; i < points; ++i) f[(size_t) i] = (float) (20.0 * std::pow (1000.0, (double) i / (points - 1)));
        return f;
    }();
    const double fs = sr.load();

    Array<var> rta;
    {
        const ScopedLock sl (fftLock);
        static juce::dsp::FFT fft (order);
        static std::vector<float> win, buf;
        if (win.empty())
        {
            win.resize ((size_t) size);
            for (int i = 0; i < size; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (MathConstants<float>::twoPi * (float) i / (float) (size - 1));
            buf.resize ((size_t) size * 2);
        }
        std::fill (buf.begin(), buf.end(), 0.0f);
        (output ? outputTap : inputTap).readLatest (buf.data(), size);
        for (int i = 0; i < size; ++i) buf[(size_t) i] *= win[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buf.data());
        const float norm = 4.0f / (float) size;   // Hann: x2 for the window, x2 for one side
        const float binHz = (float) fs / (float) size;
        for (int k = 0; k < points; ++k)
        {
            const float f = freqs[(size_t) k];
            const float lo = f / std::pow (2.0f, 1.0f / 24.0f), hi = f * std::pow (2.0f, 1.0f / 24.0f);
            int b0 = jlimit (1, size / 2 - 1, (int) std::floor (lo / binHz)), b1 = jlimit (1, size / 2 - 1, (int) std::ceil (hi / binHz));
            float mag = 0.0f;
            for (int b = b0; b <= b1; ++b) mag = std::max (mag, buf[(size_t) b]);
            rta.add (jmax (-120.0f, Decibels::gainToDecibels (mag * norm, -120.0f)));
        }
    }

    Array<var> curve;
    if (module == hpf || module == eq || module == feedback || module == deess || module == peq)
    {
        ::dsp::HighPass::Settings hs { p.hpfOn->load() > 0.5f, p.hpfFreq->load(), jlimit (0, 5, (int) p.hpfSlope->load()) };
        ::dsp::FeedbackSuppressor::NotchInfo nts[::dsp::FeedbackSuppressor::maxSlots];
        int nn = 0;
        for (int i = 0; i < ::dsp::FeedbackSuppressor::maxSlots; ++i)
            if (auto nt = fbSup.getNotch (i); nt.kind != ::dsp::FeedbackSuppressor::kindInactive) nts[nn++] = nt;
        const float q = getNotchQ();
        ::dsp::DeEsser::Settings de;
        de.on = p.dsOn->load() > 0.5f;
        de.mode = jlimit (0, 2, (int) p.dsMode->load());
        de.bands[0] = { true, p.dsFreq->load(), p.dsQ->load(), p.dsThr->load(), p.dsRelThr->load(), p.dsRange->load() };
        de.bands[1] = { p.dsShOn->load() > 0.5f, p.dsShFreq->load(), p.dsShQ->load(), p.dsShThr->load(), p.dsShRelThr->load(), p.dsShRange->load() };
        const float gr[2] { deEsser.getBandGainReductionDb (0), deEsser.getBandGainReductionDb (1) };
        const float bf[2] { deEsser.getBandFrequency (0), deEsser.getBandFrequency (1) };

        for (int k = 0; k < points; ++k)
        {
            const double f = freqs[(size_t) k];
            double db = 0.0;
            if (module == hpf) db = ::dsp::HighPass::responseDb (hs, f, fs);
            else if (module == feedback) db = ::dsp::FeedbackSuppressor::responseDb (nts, nn, q, f, fs);
            else if (module == deess) db = ::dsp::DeEsser::reductionResponseDb (de, gr, bf, f, fs);
            else if (module == peq)
            {
                if (p.peqOn->load() > 0.5f)
                {
                    db = p.peqOut->load();
                    for (int b = 0; b < 6; ++b)
                    {
                        const auto sb = staticBand (b);
                        if (! sb.on) continue;
                        ::dsp::SvfCoeffs c[::dsp::DynamicEq::maxStages];
                        int ns = 0;
                        ::dsp::DynamicEq::designBand (sb, sb.freq, sb.q, sb.gainDb, fs, c, ns);
                        for (int i = 0; i < ns; ++i) db += c[i].magnitudeDb (f, fs);
                    }
                }
            }
            else if (p.eqOn->load() > 0.5f)
            {
                for (int b = 0; b < params::numEqBands; ++b)
                {
                    const auto& d = p.bands[(size_t) b];
                    if (d.on->load() < 0.5f) continue;
                    ::dsp::DynamicEq::BandSettings s;
                    s.on = true;
                    const auto shape = params::bandShapeFromChoice (b, d.type != nullptr ? (int) d.type->load() : 0);
                    s.type = shape.type; s.slope = shape.slope;
                    s.freq = d.freq->load(); s.q = d.q->load();
                    const double g = d.gain->load() + (d.dyn->load() > 0.5f ? dynEq.getDynamicGainDb (b) : 0.0f);
                    ::dsp::SvfCoeffs c[::dsp::DynamicEq::maxStages];
                    int ns = 0;
                    ::dsp::DynamicEq::designBand (s, s.freq, s.q, g, fs, c, ns);
                    for (int i = 0; i < ns; ++i) db += c[i].magnitudeDb (f, fs);
                }
            }
            curve.add (jlimit (-60.0, 30.0, db));
        }
    }

    auto* o = new DynamicObject();
    Array<var> fv; for (auto f : freqs) fv.add (f);
    o->setProperty ("freqs", fv);
    o->setProperty ("rta", rta);
    o->setProperty ("curve", curve);
    return var (o);
}
