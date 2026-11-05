// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jerod Ulicki
#include "VoiceChannel.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>

using namespace juce;

// ------------------------------------------------------------------------------------- modules

const char* VoiceChannel::moduleId (int m)
{
    static const char* ids[numModules] { "hpf", "pse", "feedback", "eq", "deess", "prox", "leveler", "warmth", "comp" };
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
    static const char* names[numModules] { "High-pass", "PSE", "Feedback", "Dynamic EQ", "De-esser", "Proximity", "Leveler", "Warmth", "Compressor" };
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
        default:       return {};
    }
}

static juce::uint64 packOrder (const std::array<int, VoiceChannel::numModules>& o)
{
    juce::uint64 v = 0;
    for (int i = 0; i < VoiceChannel::numModules; ++i) v |= ((juce::uint64) (o[(size_t) i] & 15)) << (4 * i);
    return v;
}

static std::array<int, VoiceChannel::numModules> unpackOrder (juce::uint64 v)
{
    std::array<int, VoiceChannel::numModules> o {};
    for (int i = 0; i < VoiceChannel::numModules; ++i) o[(size_t) i] = (int) ((v >> (4 * i)) & 15);
    return o;
}

// ------------------------------------------------------------------------------------- setup

VoiceChannel::VoiceChannel()
    : AudioProcessor (BusesProperties().withInput ("In", AudioChannelSet::mono(), true)
                                       .withOutput ("Out", AudioChannelSet::mono(), true)),
      apvts (*this, nullptr, "LiveVoice", params::createLayout())
{
    std::array<int, numModules> def {};
    for (int i = 0; i < numModules; ++i) def[(size_t) i] = i;   // LiveVoice's own order
    packedOrder = packOrder (def);

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

std::array<int, VoiceChannel::numModules> VoiceChannel::getOrder() const noexcept { return unpackOrder (packedOrder.load()); }

bool VoiceChannel::setOrder (const std::array<int, numModules>& order)
{
    std::array<bool, numModules> seen {};
    for (int m : order)
    {
        if (! isPositiveAndBelow (m, (int) numModules) || seen[(size_t) m]) return false;
        seen[(size_t) m] = true;
    }
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
    detector.prepare (sampleRate);
    rawBuf.setSize (1, std::max (8192, 2 * samplesPerBlock));
    bypassMix = p.bypass->load() > 0.5f ? 1.0f : 0.0f;
    fullyBypassed = bypassMix >= 1.0f;
    inGainSmooth.reset (sampleRate, 0.02);
    outGainSmooth.reset (sampleRate, 0.02);
    inGainSmooth.setCurrentAndTargetValue (::dsp::dbToGain (p.inGain->load()));
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
    ds.enabled = fs.on && p.bypass->load() < 0.5f;
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
}

void VoiceChannel::resetChain() noexcept
{
    highPass.reset(); pseFx.reset(); fbSup.reset(); dynEq.reset(); deEsser.reset(); warmthFx.reset();
    inGainSmooth.setCurrentAndTargetValue (::dsp::dbToGain (p.inGain->load()));
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
        inGainSmooth.setTargetValue (::dsp::dbToGain (p.inGain->load()));
        if (inGainSmooth.isSmoothing()) for (int i = 0; i < n; ++i) ptr[i] *= inGainSmooth.getNextValue();
        else main.applyGain (0, 0, n, inGainSmooth.getTargetValue());
        peak (inputPeakDb, ptr, n);
        inputTap.push (ptr, n);

        // ---- the modules, in the chosen order (all zero latency) ----
        const auto order = unpackOrder (packedOrder.load (std::memory_order_acquire));
        for (int m : order)
        {
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
        if (ids.size() == numModules)
        {
            std::array<int, numModules> o {};
            for (int i = 0; i < numModules; ++i) o[(size_t) i] = moduleFromId (ids[i].trim());
            setOrder (o);   // ignored when not a valid permutation
        }
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

var VoiceChannel::getView()
{
    auto* o = new DynamicObject();
    Array<var> ord;
    for (int m : getOrder()) ord.add (String (moduleId (m)));
    o->setProperty ("order", ord);
    o->setProperty ("preset", presets.getCurrentName());
    Array<var> names; for (auto& n : presets.getFactoryNames()) names.add (n);
    o->setProperty ("presets", names);
    o->setProperty ("latency", getLatencySamples());

    auto* mods = new DynamicObject();
    for (int m = 0; m < numModules; ++m)
    {
        auto* mo = new DynamicObject();
        mo->setProperty ("name", moduleName (m));
        mo->setProperty ("on", isModuleOn (m));
        mo->setProperty ("onParam", moduleOnParam (m));
        Array<var> ps;
        for (auto& id : moduleParams (m))
            if (auto* rp = apvts.getParameter (id)) ps.add (paramVar (rp));
        mo->setProperty ("params", ps);
        mods->setProperty (moduleId (m), var (mo));
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

var VoiceChannel::notchCommand (const String& cmd, int slot)
{
    using C = ::dsp::FeedbackSuppressor::Command;
    C c;
    if (cmd == "clearFloating") c.type = C::Type::clearFloating;
    else if (cmd == "clearAll") c.type = C::Type::clearAll;
    else if (cmd == "removeSlot") { c.type = C::Type::removeSlot; c.slot = slot; }
    else if (cmd == "makeFixed") { c.type = C::Type::makeFixed; c.slot = slot; }
    else return var (false);
    sendNotch (c);
    return var (true);
}

var VoiceChannel::loadFactoryPreset (int index)
{
    return var (presets.loadFactory (index));
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
    if (module == hpf || module == eq || module == feedback || module == deess)
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
