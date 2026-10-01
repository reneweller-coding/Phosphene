/**
 * @file vst3test.cpp
 * @brief Loads the built VST3 the way a DAW does, and asks it to behave.
 *
 * The host test (Tests/hosttest.cpp) exercises the processor directly; this one goes through the
 * VST3 wrapper: the module is loaded from disk, the factory is asked what is in it, an instance is
 * created, and everything a host does to a freshly loaded plugin is done to it -- describe the
 * parameters, prepare, play with a transport, read the MIDI it produces, save and restore the state,
 * and take it away again. That is the part of pluginval that can live in the repository; pluginval
 * itself is not on this machine (see docs/rounds/2026-09.md, Phase 6).
 *
 * The path of the plugin comes from the command line, which CMake fills in with the built artefact.
 */
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {

int failures = 0;   ///< checks that failed
int checks = 0;   ///< checks made

/** @brief Records one check's result and prints it. */
void check(bool ok, const juce::String& what)
{
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", what.toRawUTF8()); ++failures; }
}

/** @brief A transport a host would offer: playing, at a tempo, from a position. */
class TestPlayHead final : public juce::AudioPlayHead {
public:
    /** @brief The transport as it stands: playing, at bpm, at ppq and samples. */
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm(bpm);
        info.setPpqPosition(ppq);
        info.setTimeInSamples(static_cast<juce::int64>(samples));
        info.setTimeInSeconds(static_cast<double>(samples) / sampleRate);
        info.setIsPlaying(true);
        return info;
    }
    void advance(int n) { samples += n; ppq += n * bpm / (60.0 * sampleRate); }   ///< moves on by @p n samples
    double bpm = 145.0;   ///< the tempo
    double ppq = 0.0;   ///< the position, quarter notes
    double sampleRate = 48000.0;   ///< the sample rate, Hz
    double samples = 0.0;   ///< the position, samples
};

} // namespace

/** @brief Loads the built VST3 as a host would and runs the checks. */
int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    if (argc < 2) { std::printf("usage: phos_vst3test <path to Phosphene.vst3>\n"); return 2; }
    const juce::File plugin(juce::String::fromUTF8(argv[1]));
    std::printf("Phosphene VST3 test: %s\n", plugin.getFullPathName().toRawUTF8());
    // The user's files stay the user's (23.09.2026), as in the host test: the plugin loads into this process, so
    // these variables reach it -- ratings, preferences, user presets and the gallery go to a folder of the test's own.
    const juce::File userDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("phos_vst3test_user");
    userDir.deleteRecursively();
    userDir.createDirectory();
#if defined(_WIN32)
    _putenv_s("PHOS_USER_DIR", userDir.getFullPathName().toRawUTF8());
    _putenv_s("PHOS_GALLERY_DIR", userDir.getChildFile("Sets").getFullPathName().toRawUTF8());
    _putenv_s("PHOS_NO_UPDATE_CHECK", "1");
#else
    setenv("PHOS_USER_DIR", userDir.getFullPathName().toRawUTF8(), 1);
    setenv("PHOS_GALLERY_DIR", userDir.getChildFile("Sets").getFullPathName().toRawUTF8(), 1);
    setenv("PHOS_NO_UPDATE_CHECK", "1", 1);
#endif
    check(plugin.exists(), "the built VST3 is where the build says it is");
    if (!plugin.exists()) { std::printf("%d checks, %d failures\n", checks, failures); return 1; }

    // ---------------------------------------------------------------- what the bundle carries
    //
    // The three files the core opens by bare name travel inside the bundle, in the directory the
    // VST3 specification keeps a plugin's data in: the wavetable pack and, since Phase 8 reached the
    // plugin, the two learned models. The host test proves that the plugin finds them beside its own
    // binary; this proves that the packaging put them there. The two together are what a user gets,
    // and of the two places the plugin looks this is the one that survives a host copying the bundle
    // somewhere of its own.
    //
    // `plugin` is the module inside the bundle (Contents/<arch>/Phosphene.vst3), so Resources is one
    // level up from the architecture directory.
    {
        const juce::File resources = plugin.getParentDirectory().getParentDirectory().getChildFile("Resources");
        for (const char* name : { "library.phoswt", "melody.phosmdl", "bass.phosmdl" }) {
            const juce::File f = resources.getChildFile(name);
            check(f.existsAsFile(), "the bundle carries " + juce::String(name) + " in " + resources.getFullPathName());
            check(f.getSize() > 100000, juce::String(name) + " is the whole file (" + juce::String(f.getSize()) + " bytes)");
        }
    }

    // ---------------------------------------------------------------- the module and its factory
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile(found, plugin.getFullPathName());
    check(found.size() == 1, "the module holds exactly one plugin (" + juce::String(found.size()) + ")");
    if (found.isEmpty()) { std::printf("%d checks, %d failures\n", checks, failures); return 1; }
    const juce::PluginDescription& desc = *found[0];
    std::printf("  %s %s by %s, %s\n", desc.name.toRawUTF8(), desc.version.toRawUTF8(),
                desc.manufacturerName.toRawUTF8(), desc.isInstrument ? "instrument" : "effect");
    check(desc.name == "Phosphene", "it calls itself Phosphene");
    check(desc.isInstrument, "it says it is an instrument");
    std::printf("  scan says %d in, %d out\n", desc.numInputChannels, desc.numOutputChannels);

    // ---------------------------------------------------------------- an instance
    juce::AudioPluginFormatManager formats;
    formats.addFormat(std::make_unique<juce::VST3PluginFormat>());
    juce::String error;
    std::unique_ptr<juce::AudioPluginInstance> instance(formats.createPluginInstance(desc, 48000.0, 256, error));
    check(instance != nullptr, "the host can create an instance" + (error.isEmpty() ? juce::String() : ": " + error));
    if (instance == nullptr) { std::printf("%d checks, %d failures\n", checks, failures); return 1; }

    const auto& params = instance->getParameters();
    std::printf("  %d parameters, latency %d samples\n", params.size(), instance->getLatencySamples());
    check(params.size() > 500, "the whole parameter set reaches the host (" + juce::String(params.size()) + ")");
    check(instance->producesMidi(), "the host is told it produces MIDI");
    bool named = true;
    for (auto* p : params) named = named && p->getName(128).isNotEmpty();
    check(named, "every parameter has a name");

    // ---------------------------------------------------------------- playing under a transport
    //
    // Offline: the plugin is told the host is bouncing, which through the VST3 wrapper means
    // Vst::kOffline. The generator then composes on the audio thread instead of waiting for its own
    // composer thread -- which is what a host doing an offline render wants anyway, and what lets
    // this test run in a second instead of in realtime. The live path is measured afterwards.
    instance->setNonRealtime(true);
    instance->setPlayConfigDetails(0, 2, 48000.0, 256);
    instance->prepareToPlay(48000.0, 256);
    check(instance->getTotalNumInputChannels() == 0 && instance->getTotalNumOutputChannels() == 2,
          "the instance has no inputs and a stereo output");
    TestPlayHead head;
    head.bpm = 132.0;   // not the default: if compose.bpm follows, the play head really arrived
    head.ppq = 32.0;    // bar 8: the intro before it is sparse by design (Phase 5)
    head.samples = 32.0 * 60.0 / 132.0 * 48000.0;
    instance->setPlayHead(&head);
    juce::AudioProcessorParameter* tempoParam = nullptr;
    for (auto* p : params) if (p->getName(64) == "Set Tempo") tempoParam = p;
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer midi;
    double peak = 0.0;
    int noteOns = 0;
    bool allFinite = true;
    // Ten seconds: long enough for the composer to have planned the first track and for the
    // transport to have moved through several bars.
    for (int i = 0; i < static_cast<int>(10.0 * 48000.0 / 256.0); ++i) {
        buf.clear();
        midi.clear();
        instance->processBlock(buf, midi);
        for (int c = 0; c < 2; ++c)
            for (int s = 0; s < 256; ++s) {
                const float v = buf.getReadPointer(c)[s];
                if (!std::isfinite(v)) allFinite = false;
                peak = juce::jmax(peak, static_cast<double>(std::fabs(v)));
            }
        for (const auto meta : midi) noteOns += meta.getMessage().isNoteOn() ? 1 : 0;
        head.advance(256);
    }
    check(allFinite, "ten seconds through the wrapper stay finite");
    check(peak > 0.05, "it sounds when the host transport runs (peak " + juce::String(peak, 3) + ")");
    check(peak <= 1.01, "and it never leaves the ceiling (peak " + juce::String(peak, 3) + ")");
    check(noteOns > 50, "notes come out of the MIDI output (" + juce::String(noteOns) + ")");
    std::printf("  latency after playing: %d samples\n", instance->getLatencySamples());
    check(instance->getLatencySamples() > 0, "the limiter's lookahead reaches the host");
    // The tempo the plugin plays at is the host's. It is written into the value the engine reads,
    // not through the host parameter -- a plugin that automated its own tempo knob would fight the
    // host over it -- so what the host's controller shows is still the knob the user set.
    if (tempoParam != nullptr)
        std::printf("  \"%s\" still reads %s on the host's side\n",
                    tempoParam->getName(64).toRawUTF8(), tempoParam->getCurrentValueAsText().toRawUTF8());

    // ---------------------------------------------------------------- and in real time
    //
    // Back to kRealtime: now the composer thread has to keep up on its own. It is already warm from
    // the offline pass, so a second of blocks at the rate a device would ask for them must sound.
    instance->setNonRealtime(false);
    double livePeak = 0.0;
    {
        const auto started = std::chrono::steady_clock::now();
        const int blocks = static_cast<int>(3.0 * 48000.0 / 256.0);
        for (int i = 0; i < blocks; ++i) {
            buf.clear();
            midi.clear();
            instance->processBlock(buf, midi);
            livePeak = juce::jmax(livePeak, static_cast<double>(buf.getMagnitude(0, 256)));
            head.advance(256);
            std::this_thread::sleep_until(started + std::chrono::microseconds(static_cast<long long>((i + 1) * 256 * 1.0e6 / 48000.0)));
        }
    }
    check(livePeak > 0.05, "and it sounds in real time as well (peak " + juce::String(livePeak, 3) + ")");
    instance->setPlayHead(nullptr);

    // ---------------------------------------------------------------- state through the wrapper
    {
        // The wrapper adds a bypass parameter of its own; it is not part of the set and is left
        // alone (setting it would silence the plugin, which is exactly what bypass is for).
        juce::Array<juce::AudioProcessorParameter*> ours;
        for (auto* p : params) if (p->getName(64) != "Bypass") ours.add(p);
        check(ours.size() == params.size() - 1, "the wrapper added exactly one parameter of its own (bypass)");
        // A value a host sets reaches the plugin's processor through the parameter queues of the
        // next process call -- not at the moment of the call. A few blocks are therefore run after
        // every write, or the state would be saved before the new values ever arrived.
        auto settle = [&] {
            juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
            for (int i = 0; i < 4; ++i) { buf.clear(); midi.clear(); instance->processBlock(buf, midi); }
        };
        juce::Random rng(7);
        for (auto* p : ours) p->setValueNotifyingHost(rng.nextFloat());
        settle();
        juce::MemoryBlock state;
        instance->getStateInformation(state);
        check(state.getSize() > 1000, "the state has the whole parameter set in it (" + juce::String(static_cast<int>(state.getSize())) + " bytes)");
        for (auto* p : ours) p->setValueNotifyingHost(0.25f);
        settle();
        juce::MemoryBlock other;
        instance->getStateInformation(other);
        check(other != state, "writing over the parameters really changes the state");
        instance->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        settle();
        // The plugin's own account of itself, not the host's cached view of the values: a host
        // remembers the number it sent, while the plugin snaps the discrete parameters to a step.
        juce::MemoryBlock again;
        instance->getStateInformation(again);
        check(again == state, "the state comes back through the wrapper, byte for byte");
    }

    // ---------------------------------------------------------------- rates, and a second instance
    {
        // A host changes the device behind a loaded plugin, and it loads the same plugin twice.
        // Both mean a second engine and a second composer thread inside one module.
        for (double sr : { 44100.0, 96000.0 }) {
            instance->releaseResources();
            instance->setPlayConfigDetails(0, 2, sr, 128);
            instance->prepareToPlay(sr, 128);
            juce::AudioBuffer<float> small(2, 128);
            juce::MidiBuffer m;
            bool ok = true;
            for (int i = 0; i < 40; ++i) {
                small.clear(); m.clear();
                instance->processBlock(small, m);
                for (int c = 0; c < 2; ++c)
                    for (int s = 0; s < 128; ++s) ok = ok && std::isfinite(small.getReadPointer(c)[s]);
            }
            check(ok, "the wrapper survives " + juce::String(static_cast<int>(sr)) + " Hz at block 128");
        }
        juce::String err2;
        std::unique_ptr<juce::AudioPluginInstance> second(formats.createPluginInstance(desc, 48000.0, 256, err2));
        check(second != nullptr, "a second instance loads beside the first" + (err2.isEmpty() ? juce::String() : ": " + err2));
        if (second != nullptr) {
            second->setNonRealtime(true);
            second->setPlayConfigDetails(0, 2, 48000.0, 256);
            second->prepareToPlay(48000.0, 256);
            TestPlayHead head2;
            head2.ppq = 32.0;   // past the sparse intro, as above
            head2.samples = 32.0 * 60.0 / 145.0 * 48000.0;
            second->setPlayHead(&head2);
            juce::AudioBuffer<float> b2(2, 256);
            juce::MidiBuffer m2;
            double peak2 = 0.0;
            for (int i = 0; i < static_cast<int>(6.0 * 48000.0 / 256.0); ++i) {
                b2.clear(); m2.clear();
                second->processBlock(b2, m2);
                peak2 = juce::jmax(peak2, static_cast<double>(b2.getMagnitude(0, 256)));
                head2.advance(256);
            }
            second->setPlayHead(nullptr);
            check(peak2 > 0.05, "and it plays on its own (peak " + juce::String(peak2, 3) + ")");
            second.reset();
            check(true, "and goes away again without taking the first with it");
        }
        instance->releaseResources();
        instance->setPlayConfigDetails(0, 2, 48000.0, 256);
        instance->prepareToPlay(48000.0, 256);
    }

    // ---------------------------------------------------------------- the editor, and away again
    {
        check(instance->hasEditor(), "the wrapper offers an editor");
        std::unique_ptr<juce::AudioProcessorEditor> editor(instance->createEditorAndMakeActive());
        check(editor != nullptr, "the editor opens through the wrapper");
        if (editor != nullptr) {
            check(editor->getWidth() > 400, "and has a size");
            // A host drags the window's corner, which for this editor is a zoom: the body is laid
            // out once at design size and scaled, so the shape has to survive the drag.
            const double ratio = static_cast<double>(editor->getWidth()) / juce::jmax(1, editor->getHeight());
            editor->setBounds(0, 0, editor->getWidth() * 2 / 3, editor->getHeight() * 2 / 3);
            const double after = static_cast<double>(editor->getWidth()) / juce::jmax(1, editor->getHeight());
            check(editor->getWidth() > 200 && std::fabs(after - ratio) < 0.05,
                  "and keeps its shape when the host resizes it (" + juce::String(ratio, 3) + " -> "
                      + juce::String(after, 3) + ")");
            juce::Image img(juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
            juce::Graphics g(img);
            editor->paintEntireComponent(g, true);
            check(img.isValid(), "and paints at the size it was given");
        }
        editor.reset();
    }
    instance->releaseResources();
    instance.reset();
    check(true, "the instance is destroyed without a crash");

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
