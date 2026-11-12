// End-to-end test of the plugin itself (not just the DSP core): creates the
// real AudioProcessor, feeds it a reference file from disk the way a user
// would, checks that the pedal sets itself, that audio comes out right, that
// state survives a save/restore, and that the editor can be created.
//
// Usage: tonegenie_plugin_test [screenshot.png]

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TestSignals.h"

#include <tonegenie/Analysis.h>

namespace
{

int failures = 0;

#define CHECK_MSG(cond, ...) \
    do { if (! (cond)) { ++failures; std::printf ("  FAILED line %d: %s  [", __LINE__, #cond); std::printf (__VA_ARGS__); std::printf ("]\n"); } } while (false)

#define CHECK(cond) CHECK_MSG (cond, "-")

void writeWav (const juce::File& file, const std::vector<float>& left, const std::vector<float>& right, double sampleRate)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    jassert (stream != nullptr);

    std::unique_ptr<juce::OutputStream> output (std::move (stream));
    auto writer = wav.createWriterFor (output, juce::AudioFormatWriterOptions()
                                                   .withSampleRate (sampleRate)
                                                   .withNumChannels (2)
                                                   .withBitsPerSample (24));
    if (writer == nullptr)
        return;

    const float* channels[] = { left.data(), right.data() };
    writer->writeFromFloatArrays (channels, 2, (int) left.size());
}

/** Runs something on the message thread and waits for it. */
template <typename Fn>
void onMessageThread (Fn&& fn)
{
    juce::WaitableEvent done;
    juce::MessageManager::callAsync ([&]
    {
        fn();
        done.signal();
    });
    done.wait (-1);
}

bool waitForAnalysis (ToneGenieProcessor& proc, int timeoutMs)
{
    const auto start = juce::Time::getMillisecondCounter();
    while (juce::Time::getMillisecondCounter() - start < (juce::uint32) timeoutMs)
    {
        juce::Thread::sleep (50);
        if (! proc.reference.isBusy())
        {
            juce::Thread::sleep (100); // let the completion callback run
            return true;
        }
    }
    return false;
}

std::vector<float> render (ToneGenieProcessor& proc, const std::vector<float>& input, int blockSize)
{
    std::vector<float> out (input.size());
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;

    for (size_t pos = 0; pos < input.size(); pos += (size_t) blockSize)
    {
        const int n = (int) std::min ((size_t) blockSize, input.size() - pos);
        buffer.setSize (2, n, false, false, true);
        buffer.copyFrom (0, 0, input.data() + pos, n);
        buffer.copyFrom (1, 0, input.data() + pos, n);
        proc.processBlock (buffer, midi);

        for (int i = 0; i < n; ++i)
            out[pos + (size_t) i] = 0.5f * (buffer.getSample (0, i) + buffer.getSample (1, i));

        // Real time passes between blocks in a host; give the EQ design thread a chance.
        if ((pos / (size_t) blockSize) % 16 == 0)
            juce::Thread::sleep (1);
    }
    return out;
}

float spectrumError (const tg::BandArray& a, const tg::BandArray& b, float loHz, float hiHz)
{
    const int b0 = (int) std::ceil (tg::bandIndexForHz (loHz)), b1 = (int) std::floor (tg::bandIndexForHz (hiHz));
    float ma = 0.0f, mb = 0.0f;
    for (int i = b0; i <= b1; ++i)
    {
        ma += a[(size_t) i];
        mb += b[(size_t) i];
    }
    const auto n = (float) (b1 - b0 + 1);
    float sum = 0.0f;
    for (int i = b0; i <= b1; ++i)
        sum += std::abs ((a[(size_t) i] - ma / n) - (b[(size_t) i] - mb / n));
    return sum / n;
}

void runTests (const juce::String& screenshotPath)
{
    using namespace tgtest;
    const double fileRate = 44100.0, hostRate = 48000.0;
    const int blockSize = 256;

    // --- a "song": amped guitar panned left, with bass, drums and a vocal ---------------
    TestAmp amp;
    amp.gainDb = 34.0f;
    const auto guitar = amp.process (cleanGuitar (0, fileRate, {}, 3), fileRate);
    const auto song = makeMix (guitar, fileRate, -0.6f);

    const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tonegenie_plugin_test");
    tempDir.createDirectory();
    const auto songFile = tempDir.getChildFile ("song.wav");
    writeWav (songFile, song.left, song.right, fileRate);
    CHECK (songFile.existsAsFile());

    // the clean guitar the "user" plays: a different, brighter and much quieter instrument
    tg::GuitarVoice bright;
    bright.pickupHz = 6000.0f;
    bright.pickupQ = 2.5f;
    bright.seed = 9;
    auto di = cleanGuitar (0, hostRate, bright, 4);
    normalise (di, -32.0f);

    juce::MemoryBlock savedState;
    tg::ToneProfile learned;
    float driveAfterAnalysis = 0.0f;

    {
        std::unique_ptr<ToneGenieProcessor> proc;
        onMessageThread ([&] { proc = std::make_unique<ToneGenieProcessor>(); });

        proc->setPlayConfigDetails (2, 2, hostRate, blockSize);
        proc->prepareToPlay (hostRate, blockSize);
        CHECK (proc->getLatencySamples() == tg::ToneEngine::latencySamples());

        // works before any reference is loaded
        {
            const auto out = render (*proc, di, blockSize);
            bool finite = true;
            for (float v : out)
                finite = finite && std::isfinite (v);
            CHECK (finite);
            CHECK_MSG (rmsDb (out) > -40.0f, "no-reference output level %.1f dB", rmsDb (out));
        }

        // load the song like the editor does
        onMessageThread ([&] { proc->loadReference (songFile, {}); });
        CHECK (proc->reference.isBusy());
        CHECK_MSG (waitForAnalysis (*proc, 120000), "analysis timed out");
        onMessageThread ([] {}); // flush pending callbacks

        const auto snap = proc->reference.getSnapshot();
        std::printf ("  status: %s\n", snap.status.toRawUTF8());
        CHECK (snap.hasProfile);
        CHECK (snap.profile.fromFullMix && snap.profile.isolated);
        CHECK_MSG (snap.profile.isolationPan < 0.0f, "pan %.2f", snap.profile.isolationPan);
        CHECK (! snap.thumbMin.empty() && std::abs (snap.duration - (double) guitar.size() / fileRate) < 0.1);
        CHECK (proc->hasAudition());
        learned = snap.profile;

        // the pedal has set itself
        driveAfterAnalysis = proc->apvts.getRawParameterValue (ParamIDs::drive)->load();
        CHECK_MSG (std::abs (driveAfterAnalysis - learned.drive * 100.0f) < 0.2f, "drive param %.1f vs fitted %.1f", driveAfterAnalysis, learned.drive * 100.0f);
        CHECK (juce::roundToInt (proc->apvts.getRawParameterValue (ParamIDs::voicing)->load()) == learned.voicing);
        CHECK_MSG (tg::DriveStage::gainDbFor (learned.voicing, learned.drive) > 18.0f, "fitted only %.0f dB of gain for a driven tone",
                   tg::DriveStage::gainDbFor (learned.voicing, learned.drive));

        // play: the output must take on the reference guitar's spectrum
        const auto out = render (*proc, di, blockSize);
        std::vector<float> tail (out.begin() + (std::ptrdiff_t) (out.size() / 2), out.end());
        const auto outTone = tg::analyzeTone (tail.data(), tail.size(), hostRate);
        const auto trueTone = tg::analyzeTone (guitar.data(), guitar.size(), fileRate);
        const float err = spectrumError (outTone.ltasDb, trueTone.ltasDb, 120.0f, 5000.0f);
        std::printf ("  spectrum error vs. the real guitar in the song: %.2f dB, output %.1f dBFS, trim %+.1f dB\n",
                     err, rmsDb (tail), proc->getInputTrimDb());
        CHECK_MSG (err < 2.5f, "spectrum error %.2f dB", err);
        CHECK_MSG (std::abs (rmsDb (tail) + 20.0f) < 4.0f, "output level %.1f dB", rmsDb (tail));
        CHECK_MSG (proc->getInputTrimDb() > 6.0f, "trim %.1f dB for a -32 dB guitar", proc->getInputTrimDb());

        // audition plays back the isolated guitar instead of the effect
        onMessageThread ([&] { proc->setAuditioning (true); });
        {
            std::vector<float> silence ((size_t) hostRate, 0.0f);
            const auto aud = render (*proc, silence, blockSize);
            CHECK_MSG (rmsDb (aud) > -40.0f, "audition level %.1f dB", rmsDb (aud));
        }
        onMessageThread ([&] { proc->setAuditioning (false); });

        // bypass passes the dry signal, delayed by the reported latency
        {
            proc->apvts.getParameter (ParamIDs::bypass)->setValueNotifyingHost (1.0f);
            const auto dry = render (*proc, di, blockSize);
            float maxErr = 0.0f;
            const auto lat = (size_t) proc->getLatencySamples();
            for (size_t i = (size_t) hostRate; i < dry.size(); ++i)
                maxErr = std::max (maxErr, std::abs (dry[i] - di[i - lat]));
            CHECK_MSG (maxErr < 1.0e-4f, "bypass error %g", maxErr);
            proc->apvts.getParameter (ParamIDs::bypass)->setValueNotifyingHost (0.0f);
        }

        // editor: create, lay out, draw
        onMessageThread ([&]
        {
            std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());
            CHECK (editor != nullptr);
            if (editor != nullptr)
            {
                CHECK (editor->getWidth() >= 800 && editor->getHeight() >= 600);
                const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);
                CHECK (image.isValid());

                if (screenshotPath.isNotEmpty())
                {
                    // let the editor's timer pull in the reference state before drawing
                    auto* raw = editor.release();
                    juce::Timer::callAfterDelay (400, [raw, screenshotPath]
                    {
                        std::unique_ptr<juce::AudioProcessorEditor> ed (raw);
                        const auto shot = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
                        juce::File pngFile (screenshotPath);
                        pngFile.deleteFile();
                        if (auto os = pngFile.createOutputStream())
                            juce::PNGImageFormat().writeImageToStream (shot, *os);
                    });
                }
            }
        });

        if (screenshotPath.isNotEmpty())
        {
            juce::Thread::sleep (900);
            onMessageThread ([] {});
        }

        proc->getStateInformation (savedState);
        CHECK (savedState.getSize() > 1000);

        proc->releaseResources();
        onMessageThread ([&] { proc.reset(); });
    }

    // --- a fresh instance restored from the saved state behaves the same -----------------------
    {
        std::unique_ptr<ToneGenieProcessor> proc;
        onMessageThread ([&] { proc = std::make_unique<ToneGenieProcessor>(); });
        proc->setPlayConfigDetails (1, 2, 96000.0, 128);  // different layout and sample rate
        proc->prepareToPlay (96000.0, 128);
        proc->setStateInformation (savedState.getData(), (int) savedState.getSize());

        const auto snap = proc->reference.getSnapshot();
        CHECK (snap.hasProfile);
        CHECK (snap.filePath == songFile.getFullPathName());
        CHECK_MSG (std::abs (proc->apvts.getRawParameterValue (ParamIDs::drive)->load() - driveAfterAnalysis) < 0.01f, "drive not restored");

        float maxDiff = 0.0f;
        for (int b = 0; b < tg::kNumBands; ++b)
            maxDiff = std::max (maxDiff, std::abs (snap.profile.targetDb[(size_t) b] - learned.targetDb[(size_t) b]));
        CHECK_MSG (maxDiff < 1.0e-5f, "target spectrum changed by %g dB in save/restore", maxDiff);

        // mono in, stereo out at 96 kHz; the restored instrument knowledge applies from the first note
        auto di96 = tgtest::cleanGuitar (0, 96000.0, bright, 2);
        tgtest::normalise (di96, -32.0f);

        std::vector<float> out96 (di96.size());
        juce::AudioBuffer<float> buffer (2, 128);
        juce::MidiBuffer midi;
        for (size_t pos = 0; pos + 128 <= di96.size(); pos += 128)
        {
            buffer.copyFrom (0, 0, di96.data() + pos, 128);
            buffer.clear (1, 0, 128);
            proc->processBlock (buffer, midi);
            for (int i = 0; i < 128; ++i)
                out96[pos + (size_t) i] = 0.5f * (buffer.getSample (0, i) + buffer.getSample (1, i));
            if ((pos / 128) % 32 == 0)
                juce::Thread::sleep (1);
        }

        const auto tone = tg::analyzeTone (out96.data(), out96.size(), 96000.0);
        const float err = spectrumError (tone.ltasDb, learned.targetDb, 120.0f, 5000.0f);
        std::printf ("  restored instance at 96 kHz: spectrum error %.2f dB, trim %+.1f dB\n", err, proc->getInputTrimDb());
        CHECK_MSG (err < 2.5f, "restored instance spectrum error %.2f dB", err);
        CHECK_MSG (proc->getInputTrimDb() > 6.0f, "restored trim %.1f dB", proc->getInputTrimDb());

        // garbage state must be ignored, not crash
        const char junk[] = "this is not a ToneGenie state";
        proc->setStateInformation (junk, (int) sizeof (junk));
        proc->setStateInformation (nullptr, 0);

        proc->releaseResources();
        onMessageThread ([&] { proc.reset(); });
    }

    tempDir.deleteRecursively();
}

} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String screenshot = argc > 1 ? juce::String (argv[1]) : juce::String();

    // The tests run on their own thread while this thread pumps messages, like a host does.
    std::thread worker ([&]
    {
        runTests (screenshot);
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });

    juce::MessageManager::getInstance()->runDispatchLoop();
    worker.join();

    std::printf ("%s\n", failures == 0 ? "plugin test passed" : "plugin test FAILED");
    return failures == 0 ? 0 : 1;
}
