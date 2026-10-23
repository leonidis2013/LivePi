#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

// A "sound card" with no hardware, so LiveHost can be run and tested on any computer.
//   Inputs : a test tone, pink noise, a sweep, silence, or a looping audio file you uploaded.
//   Output : discarded (listen through "Monitor" on the Mixer tab instead).
// It is paced by the system clock like a real device, so CPU and xrun counters are meaningful.
namespace VirtualAudio
{
    inline const char* const typeName  = "Virtual (no audio hardware)";
    inline const char* const outName   = "Discard (no output)";
    inline const char* const inTone    = "Test tone 440 Hz";
    inline const char* const inPink    = "Pink noise";
    inline const char* const inSweep   = "Sweep 50 Hz - 10 kHz";
    inline const char* const inSilence = "Silence";
    inline const char* const inFile    = "Test file (loop)";

    class Device : public juce::AudioIODevice, private juce::Thread
    {
    public:
        Device (const juce::String& in, const juce::String& out, const juce::File& file)
            : juce::AudioIODevice (in + " > " + out, VirtualAudio::typeName), juce::Thread ("virtual-audio"),
              inName (in), outName_ (out), testFile (file) {}

        ~Device() override { close(); }

        juce::StringArray getOutputChannelNames() override { return names ("Output ", outName_.isNotEmpty()); }
        juce::StringArray getInputChannelNames() override  { return names ("Input ", inName.isNotEmpty()); }
        juce::Array<double> getAvailableSampleRates() override { return { 44100.0, 48000.0, 96000.0 }; }
        juce::Array<int> getAvailableBufferSizes() override    { return { 32, 64, 128, 256, 512, 1024 }; }
        int getDefaultBufferSize() override { return 128; }

        juce::String open (const juce::BigInteger& in, const juce::BigInteger& out, double sr, int bs) override
        {
            close();
            rate = sr > 0 ? sr : 48000.0;
            block = bs > 0 ? bs : 128;
            activeIn = in;
            activeOut = out;
            xruns = 0;
            loadFile();
            phase = 0.0;
            sweepT = 0.0;
            filePos = 0.0;
            opened = true;
            return {};
        }

        void close() override
        {
            stop();
            opened = false;
        }

        bool isOpen() override { return opened; }

        void start (juce::AudioIODeviceCallback* cb) override
        {
            if (! opened || cb == nullptr)
                return;
            stop();
            cb->audioDeviceAboutToStart (this);
            {
                std::lock_guard<std::mutex> l (m);
                callback = cb;
            }
            playing = true;
            startThread (juce::Thread::Priority::high);
        }

        void stop() override
        {
            if (! playing.exchange (false))
                return;
            stopThread (2000);
            juce::AudioIODeviceCallback* cb;
            {
                std::lock_guard<std::mutex> l (m);
                cb = callback;
                callback = nullptr;
            }
            if (cb != nullptr)
                cb->audioDeviceStopped();
        }

        bool isPlaying() override { return playing; }
        juce::String getLastError() override { return {}; }
        int getCurrentBufferSizeSamples() override { return block; }
        double getCurrentSampleRate() override { return rate; }
        int getCurrentBitDepth() override { return 32; }
        juce::BigInteger getActiveOutputChannels() const override { return activeOut; }
        juce::BigInteger getActiveInputChannels() const override { return activeIn; }
        int getOutputLatencyInSamples() override { return 0; }
        int getInputLatencyInSamples() override { return 0; }
        int getXRunCount() const noexcept override { return xruns.load(); }

        const juce::String inName, outName_;

    private:
        static juce::StringArray names (const juce::String& prefix, bool present)
        {
            juce::StringArray a;
            if (present)
                for (int i = 1; i <= 16; ++i)
                    a.add (prefix + juce::String (i));
            return a;
        }

        void loadFile()
        {
            fileData.clear();
            fileRate = 48000.0;
            if (inName != inFile || ! testFile.existsAsFile())
                return;

            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (testFile));
            if (r == nullptr || r->lengthInSamples <= 0 || r->lengthInSamples > (juce::int64) (r->sampleRate * 600))
                return;

            juce::AudioBuffer<float> b ((int) juce::jmax (1u, r->numChannels), (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
            fileRate = r->sampleRate;
            fileData.resize ((size_t) b.getNumSamples());
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                float s = 0.0f;
                for (int c = 0; c < b.getNumChannels(); ++c)
                    s += b.getSample (c, i);
                fileData[(size_t) i] = s / (float) b.getNumChannels();
            }
        }

        float nextSample()
        {
            if (inName == inTone)
            {
                phase += 440.0 / rate;
                phase -= std::floor (phase);
                return 0.1f * (float) std::sin (phase * juce::MathConstants<double>::twoPi);
            }
            if (inName == inSweep)
            {
                const double f = 50.0 * std::pow (200.0, sweepT / 10.0);   // 50 Hz to 10 kHz over 10 s
                phase += f / rate;
                phase -= std::floor (phase);
                sweepT += 1.0 / rate;
                if (sweepT >= 10.0)
                    sweepT = 0.0;
                return 0.1f * (float) std::sin (phase * juce::MathConstants<double>::twoPi);
            }
            if (inName == inPink)
            {
                const float w = rng.nextFloat() * 2.0f - 1.0f;               // Paul Kellet's pink filter
                b0 = 0.99886f * b0 + w * 0.0555179f;
                b1 = 0.99332f * b1 + w * 0.0750759f;
                b2 = 0.96900f * b2 + w * 0.1538520f;
                b3 = 0.86650f * b3 + w * 0.3104856f;
                b4 = 0.55000f * b4 + w * 0.5329522f;
                b5 = -0.7616f * b5 - w * 0.0168980f;
                const float o = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f;
                b6 = w * 0.115926f;
                return o * 0.05f;
            }
            if (inName == inFile && ! fileData.empty())
            {
                const size_t i = (size_t) filePos;
                const size_t j = (i + 1) % fileData.size();
                const float frac = (float) (filePos - (double) i);
                const float s = fileData[i] * (1.0f - frac) + fileData[j] * frac;
                filePos += fileRate / rate;
                if (filePos >= (double) fileData.size())
                    filePos -= (double) fileData.size();
                return s;
            }
            return 0.0f;
        }

        void run() override
        {
            const int nIn = activeIn.countNumberOfSetBits(), nOut = activeOut.countNumberOfSetBits();
            juce::AudioBuffer<float> inBuf (juce::jmax (1, nIn), block), outBuf (juce::jmax (1, nOut), block);
            std::vector<const float*> inPtrs ((size_t) nIn);
            std::vector<float*> outPtrs ((size_t) nOut);
            for (int c = 0; c < nIn; ++c)  inPtrs[(size_t) c]  = inBuf.getReadPointer (c);
            for (int c = 0; c < nOut; ++c) outPtrs[(size_t) c] = outBuf.getWritePointer (c);

            juce::AudioIODeviceCallback* cb;
            {
                std::lock_guard<std::mutex> l (m);
                cb = callback;
            }
            if (cb == nullptr)
                return;

            using clock = std::chrono::steady_clock;
            const auto period = std::chrono::duration_cast<clock::duration> (std::chrono::duration<double> (block / rate));
            auto next = clock::now();

            while (! threadShouldExit())
            {
                if (nIn > 0)
                {
                    auto* d = inBuf.getWritePointer (0);
                    for (int i = 0; i < block; ++i)
                        d[i] = nextSample();
                    for (int c = 1; c < nIn; ++c)
                        inBuf.copyFrom (c, 0, inBuf, 0, 0, block);
                }
                outBuf.clear();

                const auto t0 = clock::now();
                cb->audioDeviceIOCallbackWithContext (nIn > 0 ? inPtrs.data() : nullptr, nIn,
                                                      nOut > 0 ? outPtrs.data() : nullptr, nOut,
                                                      block, {});
                if (clock::now() - t0 > period)
                    ++xruns;

                next += period;
                const auto now = clock::now();
                if (next < now - std::chrono::milliseconds (100))
                    next = now;                                  // far behind: resynchronise
                std::this_thread::sleep_until (next);
            }
        }

        juce::File testFile;
        std::vector<float> fileData;
        double fileRate = 48000.0, filePos = 0.0, phase = 0.0, sweepT = 0.0;
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        juce::Random rng { 42 };

        double rate = 48000.0;
        int block = 128;
        juce::BigInteger activeIn, activeOut;
        std::atomic<int> xruns { 0 };
        std::atomic<bool> playing { false };
        bool opened = false;
        std::mutex m;
        juce::AudioIODeviceCallback* callback = nullptr;
    };

    class DeviceType : public juce::AudioIODeviceType
    {
    public:
        explicit DeviceType (const juce::File& f) : juce::AudioIODeviceType (VirtualAudio::typeName), file (f) {}

        void scanForDevices() override {}

        juce::StringArray getDeviceNames (bool wantInputNames) const override
        {
            if (! wantInputNames)
                return { outName };
            juce::StringArray a { inTone, inPink, inSweep, inSilence };
            if (file.existsAsFile())
                a.add (inFile);
            return a;
        }

        int getDefaultDeviceIndex (bool) const override { return 0; }

        int getIndexOfDevice (juce::AudioIODevice* d, bool asInput) const override
        {
            if (auto* v = dynamic_cast<Device*> (d))
                return getDeviceNames (asInput).indexOf (asInput ? v->inName : v->outName_);
            return -1;
        }

        bool hasSeparateInputsAndOutputs() const override { return true; }

        juce::AudioIODevice* createDevice (const juce::String& outputDeviceName, const juce::String& inputDeviceName) override
        {
            const bool outOk = outputDeviceName == outName;
            const bool inOk = getDeviceNames (true).contains (inputDeviceName);
            if (! outOk && ! inOk)
                return nullptr;
            return new Device (inOk ? inputDeviceName : juce::String(), outOk ? outputDeviceName : juce::String(), file);
        }

    private:
        juce::File file;
    };
}
