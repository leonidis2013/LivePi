// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki
#include "VoiceChannel.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <set>

using namespace juce;

// ------------------------------------------------------------------------------------- modules

static const int typeTable[VoiceChannel::numModules]
    { VoiceChannel::tHpf, VoiceChannel::tPse, VoiceChannel::tFeedback, VoiceChannel::tEq, VoiceChannel::tDeess,
      VoiceChannel::tProx, VoiceChannel::tLeveler, VoiceChannel::tWarmth, VoiceChannel::tComp, VoiceChannel::tPeq,
      VoiceChannel::tHpf, VoiceChannel::tEq, VoiceChannel::tComp, VoiceChannel::tComp,
      VoiceChannel::tPeq, VoiceChannel::tPeq, VoiceChannel::tPeq };
static const int instTable[VoiceChannel::numModules] { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 2, 3, 4 };

int VoiceChannel::typeOf (int m) noexcept     { return isPositiveAndBelow (m, (int) numModules) ? typeTable[m] : -1; }
int VoiceChannel::instanceOf (int m) noexcept { return isPositiveAndBelow (m, (int) numModules) ? instTable[m] : 1; }

static int moduleOf (int type, int inst)
{
    for (int m = 0; m < VoiceChannel::numModules; ++m)
        if (typeTable[m] == type && instTable[m] == inst) return m;
    return -1;
}

String VoiceChannel::paramFor (const String& baseId, int instance)
{
    return instance <= 1 ? baseId : baseId + "_" + String (instance);
}

String VoiceChannel::moduleId (int m)
{
    static const char* ids[numTypes] { "hpf", "pse", "feedback", "eq", "deess", "prox", "leveler", "warmth", "comp", "peq" };
    const int t = typeOf (m);
    if (t < 0) return {};
    return String (ids[t]) + (instanceOf (m) > 1 ? String (instanceOf (m)) : String());
}

int VoiceChannel::moduleFromId (const String& id)
{
    for (int m = 0; m < numModules; ++m)
        if (id == moduleId (m)) return m;
    return -1;
}

String VoiceChannel::moduleName (int m)
{
    static const char* names[numTypes] { "High-pass", "PSE", "Feedback", "Dynamic EQ", "De-esser", "Proximity", "Leveler", "Warmth", "Compressor", "Parametric EQ" };
    const int t = typeOf (m);
    if (t < 0) return {};
    return String (names[t]) + (instanceOf (m) > 1 ? " " + String (instanceOf (m)) : String());
}

static StringArray typeParams (int t)
{
    using namespace params;
    switch (t)
    {
        case VoiceChannel::tHpf:      return { hpfOn, hpfFreq, hpfSlope };
        case VoiceChannel::tPse:      return { pseOn, pseThr, pseRange, pseAttack, pseHold, pseRelease, pseScHp, pseScLp };
        case VoiceChannel::tFeedback: return { fbOn, fbMode, fbSens, fbLearn, fbFreeze, fbMaxDepth, fbWidth, fbMaxFixed, fbMaxFloat, fbHold, fbLoFreq, fbHiFreq };
        case VoiceChannel::tEq:
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
        case VoiceChannel::tDeess:    return { dsOn, dsMode, dsFreq, dsQ, dsThr, dsRange, dsRel, dsRelative, dsRelThr, dsFollow,
                                               dsShOn, dsShFreq, dsShQ, dsShThr, dsShRelThr, dsShRange };
        case VoiceChannel::tProx:     return { proxOn, proxAmount };
        case VoiceChannel::tLeveler:  return { lvlOn, lvlTarget, lvlRange, lvlSpeed };
        case VoiceChannel::tWarmth:   return { params::warmth };
        case VoiceChannel::tComp:     return { compOn, compThr, compRatio, compKnee, compAttack, compRelease, compAuto, compMakeup, compMix, compScHp, compAutoGain };
        case VoiceChannel::tPeq:
        {
            StringArray a { "peqOn", "peqPropQ", "peqOut" };
            for (int b = 1; b <= 6; ++b)
                for (auto* k : { "on", "type", "freq", "gain", "q", "slope" }) a.add ("peq" + String (b) + "_" + k);
            return a;
        }
        default: return {};
    }
}

StringArray VoiceChannel::moduleParams (int m)
{
    StringArray a;
    for (auto& id : typeParams (typeOf (m))) a.add (paramFor (id, instanceOf (m)));
    return a;
}

String VoiceChannel::moduleOnParam (int m)
{
    String id;
    switch (typeOf (m))
    {
        case tHpf:      id = params::hpfOn; break;
        case tPse:      id = params::pseOn; break;
        case tFeedback: id = params::fbOn; break;
        case tEq:       id = params::eqOn; break;
        case tDeess:    id = params::dsOn; break;
        case tProx:     id = params::proxOn; break;
        case tLeveler:  id = params::lvlOn; break;
        case tComp:     id = params::compOn; break;
        case tPeq:      id = "peqOn"; break;
        default:        return {};
    }
    return paramFor (id, instanceOf (m));
}

// ------------------------------------------------------------------------------------- parameters

// LiveVoice's parameters plus LivePi's own (polarity, parametric EQ)
static AudioProcessorValueTreeState::ParameterLayout baseLayout()
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

// A parameter-only processor holding the base parameters: the pattern for the extra instances
namespace
{
    struct Pattern : AudioProcessor
    {
        Pattern() : apvts (*this, nullptr, "Pattern", baseLayout()) {}
        const String getName() const override { return "pattern"; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        void processBlock (AudioBuffer<float>&, MidiBuffer&) override {}
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const String getProgramName (int) override { return {}; }
        void changeProgramName (int, const String&) override {}
        void getStateInformation (MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}
        AudioProcessorValueTreeState apvts;
    };
    Pattern& pattern() { static Pattern p; return p; }

    // LiveVoice's own parameter ids: its voice presets set these, and nothing else
    const std::set<String>& liveVoiceIds()
    {
        static const std::set<String> ids = []
        {
            std::set<String> s;
            for (auto* ap : pattern().getParameters())
                if (auto* rp = dynamic_cast<RangedAudioParameter*> (ap))
                {
                    const auto id = rp->getParameterID();
                    if (! id.startsWith ("peq") && id != "inPolarity") s.insert (id);
                }
            return s;
        }();
        return ids;
    }
}

AudioProcessorValueTreeState::ParameterLayout VoiceChannel::createLayout()
{
    auto layout = baseLayout();
    // Extra instances: a copy of each of the type's parameters, same range, default and text, id + "_N"
    for (int m = 0; m < numModules; ++m)
    {
        const int inst = instanceOf (m);
        if (inst <= 1) continue;
        for (auto& id : typeParams (typeOf (m)))
        {
            auto* orig = dynamic_cast<RangedAudioParameter*> (pattern().apvts.getParameter (id));
            if (orig == nullptr) continue;
            const ParameterID pid { paramFor (id, inst), 1 };
            const auto name = orig->getName (64);
            if (auto* c = dynamic_cast<AudioParameterChoice*> (orig))
                layout.add (std::make_unique<AudioParameterChoice> (pid, name, c->choices, roundToInt (orig->convertFrom0to1 (orig->getDefaultValue()))));
            else if (dynamic_cast<AudioParameterBool*> (orig) != nullptr)
                layout.add (std::make_unique<AudioParameterBool> (pid, name, orig->getDefaultValue() > 0.5f));
            else if (auto* i = dynamic_cast<AudioParameterInt*> (orig))
                layout.add (std::make_unique<AudioParameterInt> (pid, name, i->getRange().getStart(), i->getRange().getEnd(),
                                                                 roundToInt (orig->convertFrom0to1 (orig->getDefaultValue()))));
            else if (auto* f = dynamic_cast<AudioParameterFloat*> (orig))
                layout.add (std::make_unique<AudioParameterFloat> (pid, name, f->range, orig->convertFrom0to1 (orig->getDefaultValue()),
                            AudioParameterFloatAttributes().withLabel (f->getLabel())
                                .withStringFromValueFunction ([orig] (float v, int n) { return orig->getText (orig->convertTo0to1 (v), n); })));
        }
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
    p.dsOn = r (dsOn); p.dsFreq = r (dsFreq); p.dsThr = r (dsThr); p.dsRange = r (dsRange); p.dsMode = r (dsMode); p.dsQ = r (dsQ);
    p.dsRel = r (dsRel); p.dsRelative = r (dsRelative); p.dsRelThr = r (dsRelThr); p.dsFollow = r (dsFollow);
    p.dsShOn = r (dsShOn); p.dsShFreq = r (dsShFreq); p.dsShQ = r (dsShQ); p.dsShThr = r (dsShThr); p.dsShRelThr = r (dsShRelThr); p.dsShRange = r (dsShRange);
    p.pseOn = r (pseOn); p.pseThr = r (pseThr); p.pseRange = r (pseRange); p.pseAttack = r (pseAttack); p.pseHold = r (pseHold);
    p.pseRelease = r (pseRelease); p.pseScHp = r (pseScHp); p.pseScLp = r (pseScLp);
    p.lvlOn = r (lvlOn); p.lvlTarget = r (lvlTarget); p.lvlRange = r (lvlRange); p.lvlSpeed = r (lvlSpeed);
    p.proxOn = r (proxOn); p.proxAmount = r (proxAmount); p.warmth = r (params::warmth);
    p.polarity = r ("inPolarity");

    for (int k = 0; k < nHpf; ++k)
    {
        auto q = [&] (const String& id) { return r (paramFor (id, k + 1)); };
        p.hp[(size_t) k] = { q (hpfOn), q (hpfFreq), q (hpfSlope) };
    }
    for (int k = 0; k < nEq; ++k)
    {
        auto q = [&] (const String& id) { return r (paramFor (id, k + 1)); };
        auto& e = p.eqs[(size_t) k];
        e.on = q (eqOn);
        for (int b = 0; b < numEqBands; ++b)
        {
            auto& d = e.bands[(size_t) b];
            d.on = q (bOn (b)); d.type = (b == 0 || b == numEqBands - 1) ? q (bType (b)) : nullptr;
            d.freq = q (bFreq (b)); d.gain = q (bGain (b)); d.q = q (bQ (b)); d.dyn = q (bDyn (b));
            d.thr = q (bThr (b)); d.range = q (bRange (b)); d.ratio = q (bRatio (b)); d.att = q (bAtt (b)); d.rel = q (bRel (b));
        }
    }
    for (int k = 0; k < nComp; ++k)
    {
        auto q = [&] (const String& id) { return r (paramFor (id, k + 1)); };
        p.cp[(size_t) k] = { q (compOn), q (compThr), q (compRatio), q (compKnee), q (compAttack), q (compRelease),
                             q (compAuto), q (compMakeup), q (compMix), q (compScHp), q (compAutoGain) };
    }
    for (int k = 0; k < nPeq; ++k)
    {
        auto q = [&] (const String& id) { return r (paramFor (id, k + 1)); };
        auto& e = p.pq[(size_t) k];
        e.on = q ("peqOn"); e.propQ = q ("peqPropQ"); e.out = q ("peqOut");
        for (int b = 0; b < 6; ++b)
        {
            const auto n = "peq" + String (b + 1) + "_";
            e.b[(size_t) b] = { q (n + "on"), q (n + "type"), q (n + "freq"), q (n + "gain"), q (n + "q"), q (n + "slope") };
        }
    }
}

VoiceChannel::~VoiceChannel()
{
    detector.release();
}

std::vector<int> VoiceChannel::getOrder() const
{
    std::vector<int> o;
    const int n = flowCount.load();
    for (int i = 0; i < n; ++i) o.push_back (flowSlots[(size_t) i].load());
    return o;
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
    // sequence lock: odd while writing, so the audio thread never uses a half-written flow
    flowSeq.fetch_add (1, std::memory_order_acq_rel);
    for (size_t i = 0; i < order.size(); ++i) flowSlots[i].store (order[i], std::memory_order_relaxed);
    flowCount.store ((int) order.size(), std::memory_order_relaxed);
    flowMask.store (mask, std::memory_order_relaxed);
    flowSeq.fetch_add (1, std::memory_order_acq_rel);
    return true;
}

bool VoiceChannel::isModuleOn (int m) const
{
    if (typeOf (m) == tWarmth) return p.warmth->load() > 0.5f;
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
    for (auto& e : dynEq) e.prepare (sampleRate, nCh);
    deEsser.prepare (sampleRate, nCh);
    pseFx.prepare (sampleRate, nCh);
    for (auto& c : compFx) c.prepare (sampleRate, nCh);
    proximity.prepare (sampleRate, nCh);
    for (auto& h : highPass) h.prepare (sampleRate, nCh);
    levelerFx.prepare (sampleRate, nCh);
    warmthFx.prepare (sampleRate, nCh);
    for (int k = 0; k < nPeq; ++k)
    {
        staticEq[(size_t) k].prepare (sampleRate, nCh);
        peqOutSmooth[(size_t) k].reset (sampleRate, 0.02);
        peqOutSmooth[(size_t) k].setCurrentAndTargetValue (::dsp::dbToGain (p.pq[(size_t) k].out->load()));
        peqWasOn[(size_t) k] = true;
    }
    for (auto& w : eqWasOn) w = true;
    detector.prepare (sampleRate);
    lastMask = flowMask.load();
    rawBuf.setSize (1, std::max (8192, 2 * samplesPerBlock));
    bypassMix = p.bypass->load() > 0.5f ? 1.0f : 0.0f;
    fullyBypassed = bypassMix >= 1.0f;
    inGainSmooth.reset (sampleRate, 0.02);
    outGainSmooth.reset (sampleRate, 0.02);
    inGainSmooth.setCurrentAndTargetValue (::dsp::dbToGain (p.inGain->load()) * (p.polarity->load() > 0.5f ? -1.0f : 1.0f));
    outGainSmooth.setCurrentAndTargetValue (::dsp::dbToGain (p.outGain->load()));
    updateSettings();
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

// A static EQ band: any type on any band; proportional Q (console style) narrows a bell as its gain grows
::dsp::DynamicEq::BandSettings VoiceChannel::staticBand (const PeqP& e, int b) const noexcept
{
    using BT = params::BandType;
    const auto& d = e.b[(size_t) b];
    ::dsp::DynamicEq::BandSettings s;
    const int t = jlimit (0, 5, (int) d.type->load());
    s.on = d.on->load() > 0.5f;
    s.type = t == 1 ? BT::lowShelf : t == 2 ? BT::highShelf : t == 3 ? BT::highPass : t == 4 ? BT::lowPass : BT::bell;
    s.slope = params::slopeDbPerOct[jlimit (0, 5, (int) d.slope->load())];
    s.freq = d.freq->load();
    s.gainDb = d.gain->load();
    s.q = d.q->load();
    if (t == 5) { s.gainDb = -30.0f; s.q = jmax (s.q, 2.0f); }   // notch: a deep, narrow cut
    else if (t == 0 && e.propQ->load() > 0.5f) s.q *= std::pow (10.0f, std::abs (s.gainDb) / 40.0f);
    s.q = jlimit (0.1f, 40.0f, s.q);
    s.dynamic = false;
    return s;
}

::dsp::DynamicEq::BandSettings VoiceChannel::dynBand (const EqP& e, int b) const noexcept
{
    const auto& d = e.bands[(size_t) b];
    ::dsp::DynamicEq::BandSettings s;
    s.on = d.on->load() > 0.5f;
    const auto shape = params::bandShapeFromChoice (b, d.type != nullptr ? (int) d.type->load() : 0);
    s.type = shape.type; s.slope = shape.slope;
    s.freq = d.freq->load(); s.gainDb = d.gain->load(); s.q = d.q->load();
    s.dynamic = d.dyn->load() > 0.5f;
    s.thresholdDb = d.thr->load(); s.rangeDb = d.range->load(); s.ratio = d.ratio->load();
    s.attackMs = d.att->load(); s.releaseMs = d.rel->load();
    return s;
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

    for (int k = 0; k < nEq; ++k)
        for (int b = 0; b < params::numEqBands; ++b)
            dynEq[(size_t) k].setBand (b, dynBand (p.eqs[(size_t) k], b));

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

    for (int k = 0; k < nComp; ++k)
    {
        const auto& c = p.cp[(size_t) k];
        ::dsp::VocalCompressor::Settings cs;
        cs.on = c.on->load() > 0.5f;
        cs.thresholdDb = c.thr->load(); cs.ratio = c.ratio->load(); cs.kneeDb = c.knee->load();
        cs.attackMs = c.attack->load(); cs.releaseMs = c.release->load(); cs.autoRelease = c.autoRel->load() > 0.5f;
        cs.makeupDb = c.makeup->load(); cs.mix = c.mix->load() * 0.01f; cs.scHpHz = c.scHp->load();
        cs.autoGain = c.autoGain->load() > 0.5f;
        compFx[(size_t) k].setSettings (cs);
    }

    for (int k = 0; k < nHpf; ++k)
    {
        const auto& h = p.hp[(size_t) k];
        highPass[(size_t) k].setSettings ({ h.on->load() > 0.5f, h.freq->load(), jlimit (0, 5, (int) h.slope->load()) });
    }
    proximity.setSettings ({ p.proxOn->load() > 0.5f, p.proxAmount->load() });
    ::dsp::AutoLeveler::Settings ls;
    ls.on = p.lvlOn->load() > 0.5f; ls.targetDb = p.lvlTarget->load(); ls.rangeDb = p.lvlRange->load();
    ls.speed = jlimit (0, 2, (int) p.lvlSpeed->load());
    levelerFx.setSettings (ls);
    warmthFx.setAmount (p.warmth->load());
    for (int k = 0; k < nPeq; ++k)
        for (int b = 0; b < 6; ++b)
            staticEq[(size_t) k].setBand (b, staticBand (p.pq[(size_t) k], b));
}

void VoiceChannel::resetModule (int m) noexcept
{
    const auto k = (size_t) (instanceOf (m) - 1);
    switch (typeOf (m))
    {
        case tHpf:      highPass[k].reset(); break;
        case tPse:      pseFx.reset(); break;
        case tFeedback: fbSup.reset(); break;
        case tEq:       dynEq[k].reset(); break;
        case tDeess:    deEsser.reset(); break;
        case tProx:     proximity.reset(); break;
        case tLeveler:  levelerFx.reset(); break;
        case tWarmth:   warmthFx.reset(); break;
        case tComp:     compFx[k].reset(); break;
        case tPeq:      staticEq[k].reset(); break;
        default: break;
    }
}

void VoiceChannel::resetChain() noexcept
{
    // the filters (as LiveVoice does after a bypass); the dynamics keep their state
    for (int m = 0; m < numModules; ++m)
    {
        const int t = typeOf (m);
        if (t == tHpf || t == tPse || t == tFeedback || t == tEq || t == tDeess || t == tWarmth || t == tPeq) resetModule (m);
    }
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

    // The flow: take a consistent copy (a write in progress is retried a few times; otherwise last block's)
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto s0 = flowSeq.load (std::memory_order_acquire);
        if (s0 & 1u) continue;
        int tmp[numModules];
        const int n = jlimit (0, (int) numModules, flowCount.load (std::memory_order_relaxed));
        for (int i = 0; i < n; ++i) tmp[i] = flowSlots[(size_t) i].load (std::memory_order_relaxed);
        if (flowSeq.load (std::memory_order_acquire) == s0)
        {
            rtCount = n;
            for (int i = 0; i < n; ++i) rtFlow[i] = isPositiveAndBelow (tmp[i], (int) numModules) ? tmp[i] : -1;
            break;
        }
    }

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

        // ---- input gain and polarity (polarity flips through the 20 ms gain ramp, so it never clicks) ----
        inGainSmooth.setTargetValue (::dsp::dbToGain (p.inGain->load()) * (p.polarity->load() > 0.5f ? -1.0f : 1.0f));
        if (inGainSmooth.isSmoothing()) for (int i = 0; i < n; ++i) ptr[i] *= inGainSmooth.getNextValue();
        else main.applyGain (0, 0, n, inGainSmooth.getTargetValue());
        peak (inputPeakDb, ptr, n);
        inputTap.push (ptr, n);

        // a module put back into the flow starts from silence, not from where it was when taken out
        const juce::uint32 mask = flowMask.load (std::memory_order_acquire), added = mask & ~lastMask;
        if (added != 0)
            for (int m = 0; m < numModules; ++m)
                if ((added >> m) & 1u) resetModule (m);
        lastMask = mask;

        // ---- the modules, in the chosen order (all zero latency) ----
        for (int fi = 0; fi < rtCount; ++fi)
        {
            const int m = rtFlow[fi];
            if (m < 0) continue;
            const auto k = (size_t) (instanceOf (m) - 1);
            switch (typeOf (m))
            {
                case tHpf:      highPass[k].process (main, n); break;
                case tPse:      pseFx.process (main, n); break;
                case tFeedback: fbSup.process (main, n); break;
                case tEq:
                {
                    const bool on = p.eqs[k].on->load() > 0.5f;
                    if (on && ! eqWasOn[k]) dynEq[k].reset();
                    eqWasOn[k] = on;
                    if (on) dynEq[k].process (main, n);
                    break;
                }
                case tDeess:    deEsser.process (main, n); break;
                case tProx:     proximity.process (main, n); break;
                case tLeveler:
                    if (const int dc = detector.getDetectionCount(); dc != lastDetectCount)
                    {
                        lastDetectCount = dc;
                        levelerFx.holdBoost (4.0);   // a ring was just found: never turn up into it
                    }
                    levelerFx.process (main, n);
                    break;
                case tWarmth:   warmthFx.process (main, n); break;
                case tComp:     compFx[k].process (main, n); break;
                case tPeq:
                {
                    const bool on = p.pq[k].on->load() > 0.5f;
                    if (on && ! peqWasOn[k]) staticEq[k].reset();
                    peqWasOn[k] = on;
                    auto& sm = peqOutSmooth[k];
                    sm.setTargetValue (on ? ::dsp::dbToGain (p.pq[k].out->load()) : 1.0f);
                    if (on) staticEq[k].process (main, n);
                    if (sm.isSmoothing()) for (int i = 0; i < n; ++i) ptr[i] *= sm.getNextValue();
                    else if (! approximatelyEqual (sm.getTargetValue(), 1.0f)) main.applyGain (0, 0, n, sm.getTargetValue());
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
    if (range.start < 0.0f && range.end > 0.0f && dynamic_cast<AudioParameterFloat*> (rp) != nullptr)
        o->setProperty ("zero", rp->convertTo0to1 (0.0f));   // where 0 is, for the page's detent
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
    for (int m : getOrder()) ord.add (moduleId (m));
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
        mo->setProperty ("type", moduleId (moduleOf (typeOf (m), 1)));
        mo->setProperty ("instance", instanceOf (m));
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
        mo->setProperty ("type", "input");
        mo->setProperty ("instance", 1);
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
    // the output trim: always last, can't be removed
    {
        auto* mo = new DynamicObject();
        mo->setProperty ("name", "Output trim");
        mo->setProperty ("type", "output");
        mo->setProperty ("instance", 1);
        mo->setProperty ("on", true);
        mo->setProperty ("onParam", "");
        mo->setProperty ("fixed", true);
        if (full)
        {
            Array<var> ps;
            if (auto* rp = apvts.getParameter ("outGain")) ps.add (paramVar (rp));
            mo->setProperty ("params", ps);
        }
        mods->setProperty ("output", var (mo));
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
        l->setProperty ("lvlGain", levelerFx.getGainDb());
        l->setProperty ("lvlLevel", levelerFx.getLevelDb());
        l->setProperty ("lvlHold", levelerFx.isHoldingBoost());
        l->setProperty ("proxCut", proximity.getCutDb());
        l->setProperty ("dsGr", deEsser.getCurrentGainReductionDb());
        // per instance: compressors and dynamic EQs (instance 1 also under the old names)
        Array<var> comps, eqs;
        for (int k = 0; k < nComp; ++k)
        {
            auto& c = compFx[(size_t) k];
            auto* co = new DynamicObject();
            co->setProperty ("gr", c.getGainReductionDb());
            co->setProperty ("in", c.getInputLevelDb());
            co->setProperty ("makeup", c.getMakeupDb());
            co->setProperty ("out", c.getInputLevelDb() - c.getGainReductionDb() + c.getMakeupDb());
            comps.add (var (co));
        }
        for (int k = 0; k < nEq; ++k)
        {
            Array<var> dyn, det;
            for (int b = 0; b < params::numEqBands; ++b) { dyn.add (dynEq[(size_t) k].getDynamicGainDb (b)); det.add (dynEq[(size_t) k].getDetectorLevelDb (b)); }
            auto* eo = new DynamicObject();
            eo->setProperty ("dyn", dyn); eo->setProperty ("det", det);
            eqs.add (var (eo));
        }
        l->setProperty ("comps", comps);
        l->setProperty ("eqs", eqs);
        l->setProperty ("compGr", comps[0].getProperty ("gr", 0));
        l->setProperty ("compIn", comps[0].getProperty ("in", -100));
        l->setProperty ("compMakeup", comps[0].getProperty ("makeup", 0));
        l->setProperty ("compOut", comps[0].getProperty ("out", -100));
        l->setProperty ("eqDyn", eqs[0].getProperty ("dyn", {}));
        l->setProperty ("eqDet", eqs[0].getProperty ("det", {}));
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
    if (liveVoiceIds().count (id) > 0 && PresetManager::isPresetParameter (id) && ! presets.getCurrentName().endsWith (" (edited)"))
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

void VoiceChannel::getPresetState (MemoryBlock& dest)
{
    auto state = apvts.copyState();
    std::unique_ptr<XmlElement> xml (state.createXml());
    StringArray ord;
    for (int m : getOrder()) ord.add (moduleId (m));
    xml->createNewChildElement ("LivePi")->setAttribute ("order", ord.joinIntoString (","));
    copyXmlToBinary (*xml, dest);
}

bool VoiceChannel::loadUserPreset (const void* data, int size, const String& name)
{
    std::unique_ptr<XmlElement> xml (getXmlFromBinary (data, size));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return false;
    // what a preset never changes
    std::vector<std::pair<RangedAudioParameter*, float>> keep;
    for (auto* id : { "inGain", "outGain", "bypass", "fbLearn", "fbFreeze" })
        if (auto* rp = apvts.getParameter (id)) keep.emplace_back (rp, rp->getValue());
    if (auto* lp = xml->getChildByName ("LivePi"))
    {
        std::vector<int> o;
        for (auto& id : StringArray::fromTokens (lp->getStringAttribute ("order"), ",", "")) o.push_back (moduleFromId (id.trim()));
        setOrder (o);
    }
    for (auto* tag : { "FixedNotches", "UiState", "LivePi" })
        xml->deleteAllChildElementsWithTagName (tag);
    for (int i = xml->getNumChildElements(); --i >= 0;)
        if (auto* e = xml->getChildElement (i); e != nullptr && e->hasTagName ("PARAM") && ! std::isfinite (e->getDoubleAttribute ("value", 0.0)))
            xml->removeChildElement (e, true);
    // set every parameter (one that the preset doesn't have goes to its default)
    for (auto* ap : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (ap))
        {
            float norm = rp->getDefaultValue();
            for (auto* e : xml->getChildWithTagNameIterator ("PARAM"))
                if (e->getStringAttribute ("id") == rp->getParameterID()) { norm = rp->convertTo0to1 ((float) e->getDoubleAttribute ("value")); break; }
            rp->setValueNotifyingHost (norm);
        }
    for (auto& [rp, v] : keep) rp->setValueNotifyingHost (v);
    presets.setCurrentName (name);
    return true;
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
        applyStartingPoint();
        return var (true);
    }

    // LiveVoice's voice presets set LiveVoice's parameters only: keep LivePi's own (parametric EQs,
    // polarity, the extra instances) as they are
    std::vector<std::pair<RangedAudioParameter*, float>> keep;
    for (auto* ap : getParameters())
        if (auto* rp = dynamic_cast<RangedAudioParameter*> (ap))
            if (liveVoiceIds().count (rp->getParameterID()) == 0)
                keep.emplace_back (rp, rp->getValue());
    const bool ok = presets.loadFactory (index);
    for (auto& [rp, v] : keep) rp->setValueNotifyingHost (v);
    if (ok) applyStartingPoint();
    return var (ok);
}

// A voice preset is a starting point for the whole channel: every module it sets is switched on,
// and when the Parametric EQ is in the flow the preset's tone curve goes there (the Dynamic EQ
// keeps only its dynamic bands, so the curve is never applied twice).
void VoiceChannel::applyStartingPoint()
{
    auto set = [this] (const String& id, double v)
    {
        if (auto* rp = apvts.getParameter (id))
            rp->setValueNotifyingHost (rp->convertTo0to1 ((float) jlimit ((double) rp->getNormalisableRange().start, (double) rp->getNormalisableRange().end, v)));
    };
    auto get = [this] (const String& id) { auto* a = apvts.getRawParameterValue (id); return a != nullptr ? (double) a->load() : 0.0; };
    using namespace params;
    set (eqOn, 1);
    if (! inFlow (peq)) return;
    set ("peqOn", 1);
    for (int b = 0; b < numEqBands; ++b)
    {
        const auto n = "peq" + String (b + 1) + "_";
        const bool on = get (bOn (b)) > 0.5;
        const auto shape = bandShapeFromChoice (b, (b == 0 || b == numEqBands - 1) ? (int) get (bType (b)) : 0);
        const int type = shape.type == BandType::lowShelf ? 1 : shape.type == BandType::highShelf ? 2
                       : shape.type == BandType::highPass ? 3 : shape.type == BandType::lowPass ? 4 : 0;
        set (n + "on", on ? 1 : 0);
        set (n + "type", type);
        set (n + "freq", get (bFreq (b)));
        set (n + "gain", get (bGain (b)));
        set (n + "q", get (bQ (b)));
        if (inFlow (eq))
        {
            set (bGain (b), 0);                       // the Dynamic EQ keeps the dynamic part only
            if (get (bDyn (b)) < 0.5) set (bOn (b), 0);
            if (b == numEqBands - 1 && type == 4) set (bType (b), 0);
        }
    }
}

// RTA of the channel's input or output (1/12 octave, 20 Hz .. 20 kHz), the selected module's curve,
// and (for the other graphs) the high-pass filters in the flow, drawn faintly behind
var VoiceChannel::getSpectrum (bool output, int module)
{
    // module -2: the overall tone of the channel: every filter-type module in the flow, added up
    if (module == -2)
    {
        std::vector<double> sum;
        var base;
        for (int m : getOrder())
        {
            const int t = typeOf (m);
            if (t != tHpf && t != tEq && t != tPeq && t != tFeedback && t != tDeess) continue;
            auto sub = getSpectrum (output, m);
            if (base.isVoid()) base = sub;
            if (auto* c = sub["curve"].getArray())
            {
                sum.resize ((size_t) c->size(), 0.0);
                for (int i = 0; i < c->size(); ++i) sum[(size_t) i] += (double) (*c)[i];
            }
        }
        if (base.isVoid()) base = getSpectrum (output, -1);
        Array<var> total;
        for (auto d : sum) total.add (jlimit (-60.0, 30.0, d));
        if (auto* o = base.getDynamicObject()) { o->setProperty ("curve", total); o->setProperty ("hpf", Array<var>()); }
        return base;
    }

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

    auto hpfSettings = [this] (int k) -> ::dsp::HighPass::Settings
    {
        const auto& h = p.hp[(size_t) k];
        return { h.on->load() > 0.5f, h.freq->load(), jlimit (0, 5, (int) h.slope->load()) };
    };

    Array<var> curve, hpfBg;
    const int type = typeOf (module), k = instanceOf (module) - 1;
    if (type == tHpf || type == tEq || type == tFeedback || type == tDeess || type == tPeq)
    {
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
        // the other high-pass filters in the flow (on), for the background
        ::dsp::HighPass::Settings bg[nHpf]; int nbg = 0;
        for (int h = 0; h < nHpf; ++h)
        {
            const int hm = moduleOf (tHpf, h + 1);
            if (inFlow (hm) && hm != module && p.hp[(size_t) h].on->load() > 0.5f) bg[nbg++] = hpfSettings (h);
        }

        for (int i = 0; i < points; ++i)
        {
            const double f = freqs[(size_t) i];
            double db = 0.0;
            if (type == tHpf) db = ::dsp::HighPass::responseDb (hpfSettings (k), f, fs);
            else if (type == tFeedback) db = ::dsp::FeedbackSuppressor::responseDb (nts, nn, q, f, fs);
            else if (type == tDeess) db = ::dsp::DeEsser::reductionResponseDb (de, gr, bf, f, fs);
            else if (type == tPeq)
            {
                const auto& e = p.pq[(size_t) k];
                if (e.on->load() > 0.5f)
                {
                    db = e.out->load();
                    for (int b = 0; b < 6; ++b)
                    {
                        const auto sb = staticBand (e, b);
                        if (! sb.on) continue;
                        ::dsp::SvfCoeffs c[::dsp::DynamicEq::maxStages];
                        int ns = 0;
                        ::dsp::DynamicEq::designBand (sb, sb.freq, sb.q, sb.gainDb, fs, c, ns);
                        for (int s = 0; s < ns; ++s) db += c[s].magnitudeDb (f, fs);
                    }
                }
            }
            else if (p.eqs[(size_t) k].on->load() > 0.5f)
            {
                for (int b = 0; b < params::numEqBands; ++b)
                {
                    auto s = dynBand (p.eqs[(size_t) k], b);
                    if (! s.on) continue;
                    const double g = s.gainDb + (s.dynamic ? dynEq[(size_t) k].getDynamicGainDb (b) : 0.0f);
                    ::dsp::SvfCoeffs c[::dsp::DynamicEq::maxStages];
                    int ns = 0;
                    ::dsp::DynamicEq::designBand (s, s.freq, s.q, g, fs, c, ns);
                    for (int st = 0; st < ns; ++st) db += c[st].magnitudeDb (f, fs);
                }
            }
            curve.add (jlimit (-60.0, 30.0, db));
            if (nbg > 0)
            {
                double hb = 0.0;
                for (int h = 0; h < nbg; ++h) hb += ::dsp::HighPass::responseDb (bg[h], f, fs);
                hpfBg.add (jlimit (-60.0, 30.0, hb));
            }
        }
    }

    auto* o = new DynamicObject();
    Array<var> fv; for (auto f : freqs) fv.add (f);
    o->setProperty ("freqs", fv);
    o->setProperty ("rta", rta);
    o->setProperty ("curve", curve);
    o->setProperty ("hpf", hpfBg);
    return var (o);
}
