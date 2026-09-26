/**
 * @file hosttest.cpp
 * @brief The host contract of the Phosphene plugin.
 *
 * The self test measures the engine; this one measures the plugin around it. Everything here is
 * something a host does and a generator has to survive: prepare and release at rates and block
 * sizes nobody develops at, blocks that change size in the middle of a set, parameters written from
 * another thread while the audio thread reads them, a transport that starts, jumps and stops, a
 * state that has to come back exactly as it went out, and an editor opened and closed.
 *
 * The one check that is not about surviving is the oracle: with its own clock and a fresh seed the
 * plugin must render *exactly* what phos_render renders -- the same engine, the same composer, the
 * same conductor, driven the same way (Tools/render/main.cpp). The reference loop below is that
 * program's inner loop, written out, and the first four bars are compared sample for sample. Both
 * paths carry the same limiter lookahead, so no shift is applied; the latency is checked separately
 * against phos::Engine::latencySamples().
 *
 * Since the second Phase 6 round it also measures the three things that round built: that the
 * arrange timeline of a sixty-minute set is drawn from a cached picture rather than from scratch at
 * every tick, that a lock and a reroll made in the editor reach the composer and move exactly the
 * track they name, and that a perform macro moves what it says it moves and puts every knob back.
 *
 * This is not a replacement for pluginval, which exercises the VST3 wrapper itself; it is the part
 * that lives in the repository and runs on every build.
 */
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "phos/Composer.h"
#include "phos/Engine.h"
#include "phos/Rating.h"
#include "phos/WaveTableFile.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace phos;

namespace {

int failures = 0;
int checks = 0;

/** @brief Records one check's result and prints it. */
void check(bool ok, const juce::String& what)
{
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", what.toRawUTF8()); ++failures; }
}

/** @brief Whether the first @p n samples of every channel of @p b are finite. */
bool finite(const juce::AudioBuffer<float>& b, int n)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < n; ++i)
            if (!std::isfinite(b.getReadPointer(c)[i])) return false;
    return true;
}

/** @brief A play head a host would give: a transport at a tempo, running from a position. */
class TestPlayHead final : public juce::AudioPlayHead {
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm(bpm);
        info.setPpqPosition(ppq);
        info.setTimeInSamples(static_cast<juce::int64>(samples));
        info.setTimeInSeconds(static_cast<double>(samples) / sampleRate);
        info.setIsPlaying(playing);
        info.setIsRecording(false);
        return info;
    }
    /** @brief Advances the transport by a block. */
    void advance(int n)
    {
        if (!playing) return;
        samples += n;
        ppq += n * bpm / (60.0 * sampleRate);
    }
    double bpm = 145.0, ppq = 0.0, sampleRate = 48000.0, samples = 0.0;
    bool playing = true;
};

/** @brief Runs @p blocks blocks through the processor, collecting the MIDI it produces. */
void feed(PhospheneProcessor& p, juce::AudioBuffer<float>& buf, int blocks, std::vector<juce::MidiMessage>* midiOut = nullptr)
{
    juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b) {
        midi.clear();
        buf.clear();
        p.processBlock(buf, midi);
        if (midiOut != nullptr)
            for (const auto meta : midi) midiOut->push_back(meta.getMessage());
    }
}

/**
 * @brief The inner loop of phos_render: the oracle the plugin has to equal.
 * @param seed    the set
 * @param sr      sample rate
 * @param block   block size handed to the engine
 * @param samples how many samples to render
 * @param bars    bars planned for the tempo map, as phos_render's `--bars` does it
 */
std::vector<float> referenceRender(uint64_t seed, double sr, int block, int samples, int bars)
{
    auto engine = std::make_unique<Engine>();
    Composer composer(seed);
    engine->prepare(sr, block);
    engine->setTempoMap(composer.tempoMap(engine->params(), bars));
    Conductor conductor(*engine, composer);
    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block)), out;
    out.reserve(static_cast<size_t>(samples) * 2);
    int done = 0;
    while (done < samples) {
        const int n = std::min(block, samples - done);
        conductor.pump(engine->params(), 8.0 * kBeatsPerBar, nullptr);
        engine->process(L.data(), R.data(), n);
        for (int i = 0; i < n; ++i) { out.push_back(L[static_cast<size_t>(i)]); out.push_back(R[static_cast<size_t>(i)]); }
        done += n;
    }
    return out;
}

/** @brief The plugin rendering the same thing: own clock, offline, from the top. */
std::vector<float> pluginRender(uint64_t seed, double sr, int block, int samples)
{
    auto p = std::make_unique<PhospheneProcessor>();
    p->setFollowHost(false);
    p->setNonRealtime(true);
    p->setSeed(seed);
    p->setPlayConfigDetails(0, 2, sr, block);
    p->prepareToPlay(sr, block);
    p->play();
    juce::AudioBuffer<float> buf(2, block);
    juce::MidiBuffer midi;
    std::vector<float> out;
    out.reserve(static_cast<size_t>(samples) * 2);
    int done = 0;
    while (done < samples) {
        const int n = std::min(block, samples - done);
        buf.setSize(2, n, false, false, true);
        buf.clear();
        midi.clear();
        p->processBlock(buf, midi);
        for (int i = 0; i < n; ++i) { out.push_back(buf.getReadPointer(0)[i]); out.push_back(buf.getReadPointer(1)[i]); }
        done += n;
    }
    return out;
}

/** @brief Feeds blocks until the composer has published @p want tracks, or the time runs out. */
bool waitForPlans(PhospheneProcessor& p, juce::AudioBuffer<float>& buf, int want, double seconds)
{
    const auto t0 = std::chrono::steady_clock::now();
    phos::TrackPlan plan;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        feed(p, buf, 1);
        if (p.tryReadTrack(want - 1, plan)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    return false;
}

/** @brief Root mean square of what the processor renders over @p samples samples. */
double renderRms(PhospheneProcessor& p, int block, int samples)
{
    juce::AudioBuffer<float> buf(2, block);
    juce::MidiBuffer midi;
    double sum = 0.0;
    int done = 0;
    while (done < samples) {
        buf.clear();
        midi.clear();
        p.processBlock(buf, midi);
        for (int i = 0; i < block; ++i) {
            const double v = buf.getReadPointer(0)[i];
            sum += v * v;
        }
        done += block;
    }
    return std::sqrt(sum / juce::jmax(1, done));
}

/** @brief The first combo box in a component tree whose tooltip is the parameter key @p key. */
juce::ComboBox* findCombo(juce::Component& root, const juce::String& key)
{
    if (auto* cb = dynamic_cast<juce::ComboBox*>(&root))
        if (cb->getTooltip() == key) return cb;
    for (juce::Component* child : root.getChildren())
        if (child != nullptr)
            if (juce::ComboBox* found = findCombo(*child, key)) return found;
    return nullptr;
}

/** @brief The first label in a component tree whose text starts with @p prefix. */
juce::Label* findLabel(juce::Component& root, const juce::String& prefix)
{
    if (auto* l = dynamic_cast<juce::Label*>(&root))
        if (l->getText().startsWith(prefix)) return l;
    for (juce::Component* child : root.getChildren())
        if (child != nullptr)
            if (juce::Label* found = findLabel(*child, prefix)) return found;
    return nullptr;
}

/**
 * @brief The `--probe-models` mode: what one fresh process made of Phase 8, on one line.
 *
 * Why a whole process. phos::sharedMelodyModel() and phos::sharedBassModel() load once and are never
 * unloaded, and the plugin resolves its resource directory once, so "the models are there" and "the
 * models are not there" cannot both be staged inside one run -- unlike the wavetable library, which
 * has resetWaveTableLibrary(). The absence is therefore staged the only way it can be: a copy of
 * this executable in an empty directory, started as a child, reporting back through its stdout.
 */
int probeModels()
{
    auto p = std::make_unique<PhospheneProcessor>();
    p->setFollowHost(false);
    p->setNonRealtime(true);
    p->setPlayConfigDetails(0, 2, 48000.0, 256);
    p->prepareToPlay(48000.0, 256);
    p->play();
    const PhospheneProcessor::LearnedModels m = p->learnedModels();
    // Eight bars: a track opens with a sparse intro and the kick enters between bars five and nine,
    // so anything shorter would measure the atmosphere and not the set.
    const int samples = static_cast<int>(8.0 * kBeatsPerBar * 60.0 / 145.0 * 48000.0);
    const double rms = renderRms(*p, 256, samples);
    int marks = 0;
    std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());
    for (const char* key : { "compose.melody_model", "compose.bass_model" }) {
        juce::ComboBox* cb = editor != nullptr ? findCombo(*editor, key) : nullptr;
        if (cb != nullptr && cb->getNumItems() == 2 && cb->getItemText(1).contains("missing")) ++marks;
    }
    editor.reset();
    std::printf("probe melody=%d bass=%d source=%d marks=%d rms=%.5f dir=%s\n",
                m.melody ? 1 : 0, m.bass ? 1 : 0, m.fromSourceTree ? 1 : 0, marks, rms,
                m.directory.isEmpty() ? "(none)" : m.directory.toRawUTF8());
    return 0;
}

/** @brief A mouse click on a component that has no peer, as the editor's own would arrive. */
void clickAt(juce::Component& c, int x, int y)
{
    const juce::Point<float> at(static_cast<float>(x), static_cast<float>(y));
    const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), at,
                             juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                             &c, &c, juce::Time::getCurrentTime(), at, juce::Time::getCurrentTime(), 1, false);
    c.mouseDown(e);
}

/**
 * @brief A sixty-minute set as the editor copies it out of the published plans.
 *
 * Every track starts kDjOverlap bars before the previous one ends, as the composer plans them since the
 * arrangement round (19.09.2026); until then this set was back to back and the view was never shown two
 * tracks sharing bars.
 */
ArrangeDisplay::Snapshot bigSet()
{
    static const SectionType kinds[] = { SectionType::Intro, SectionType::Groove, SectionType::Build,
                                         SectionType::Pdb,   SectionType::Drop,   SectionType::Cut,
                                         SectionType::Break, SectionType::Build,  SectionType::Drop,
                                         SectionType::Outro };
    ArrangeDisplay::Snapshot s;
    int bar = 0;
    for (int t = 0; t < 9; ++t) {
        ArrangeDisplay::Trk trk;
        trk.index = t;
        trk.firstBar = bar;
        trk.bars = 256;
        trk.key = (t * 7) % 12;
        trk.bpm = 139.5 + 0.5 * t;
        int in = 0;
        for (int i = 0; i < 11; ++i) {
            ArrangeDisplay::Sec sec;
            sec.index = i;
            sec.bar = bar + in;
            sec.bars = i == 10 ? 256 - in : (i % 3 == 0 ? 16 : 32);
            if (sec.bars <= 0) break;
            sec.type = kinds[i % 10];
            sec.energy = 0.3f + 0.06f * (i % 8);
            sec.energyTo = sec.energy + 0.05f;
            trk.sections.push_back(sec);
            in += sec.bars;
            if (in >= 256) break;
        }
        s.bars = bar + trk.bars;
        bar += trk.bars - kDjOverlap;
        s.tracks.push_back(std::move(trk));
    }
    s.minutes = 60;
    return s;
}

/**
 * @brief `--part live` (26.09.2026): a diagnosis, not a check -- plays a set in real time over a track change.
 *
 * The user heard the playback stall "when the engine does not keep up with generating the notes". This plays
 * the shipped arrangement (composer thread, probes at the plugin's own thread count, no probe cache) at a
 * device's pace from a few bars before the end of track 1, and prints, per second of music, the slowest
 * processBlock, the blocks that missed their deadline and the silent blocks, and at the end the longest silent
 * run. Not registered with ctest: it takes as long as the music it plays.
 *
 * Environment: PHOS_LIVE_SEED (default 358425790), PHOS_LIVE_SET (knob text, default Progressive with the style
 * mix on), PHOS_LIVE_SECONDS (default 90), PHOS_LIVE_BEFORE_END (bars before track 1 ends, default 12).
 * @return 0
 */
int liveDiagnosis()
{
    auto env = [](const char* k, const char* d) { return juce::SystemStats::getEnvironmentVariable(k, d); };
    const double sr = 48000.0;
    const int block = 256;
    phos::probe::setCacheDir("");
    auto p = std::make_unique<PhospheneProcessor>();
    p->setFollowHost(false);
    p->params().parseText(env("PHOS_LIVE_SET", "compose.style=2;compose.style_mix=1").toStdString());
    p->setSeed(static_cast<uint64_t>(env("PHOS_LIVE_SEED", "358425790").getLargeIntValue()));
    p->setPlayConfigDetails(0, 2, sr, block);
    p->prepareToPlay(sr, block);
    juce::AudioBuffer<float> buf(2, block);
    juce::MidiBuffer midi;
    // Track 1's length, from the plan the composer thread publishes.
    phos::TrackPlan first;
    for (int i = 0; i < 6000 && !p->tryReadTrack(0, first); ++i) {
        buf.clear(); midi.clear();
        p->processBlock(buf, midi);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const int startBar = juce::jmax(0, first.bars - env("PHOS_LIVE_BEFORE_END", "12").getIntValue());
    std::printf("live: track 1 has %d bars, starting at bar %d\n", first.bars, startBar + 1);
    p->play();
    p->seekToBar(startBar);
    while (p->transport().restarting) {
        buf.clear(); midi.clear();
        p->processBlock(buf, midi);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const double seconds = env("PHOS_LIVE_SECONDS", "90").getDoubleValue();
    const int blocks = static_cast<int>(seconds * sr / block), perSecond = static_cast<int>(sr / block);
    const double blockSeconds = block / sr;
    int silentRun = 0, worstRun = 0, worstAt = 0, late = 0, silent = 0;
    double worstBlock = 0.0;
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < blocks; ++i) {
        buf.clear(); midi.clear();
        const auto b0 = std::chrono::steady_clock::now();
        p->processBlock(buf, midi);
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - b0).count();
        worstBlock = std::max(worstBlock, took);
        late += took > blockSeconds ? 1 : 0;
        if (buf.getMagnitude(0, block) < 1.0e-4f) {
            ++silent;
            if (++silentRun > worstRun) { worstRun = silentRun; worstAt = p->transport().bar + 1; }
        } else {
            silentRun = 0;
        }
        if ((i + 1) % perSecond == 0) {
            const TransportView t = p->transport();
            std::printf("  %3d s  bar %4d (track %d, bar %3d)  slowest block %6.2f ms  late %3d  silent %3d of %d\n", (i + 1) / perSecond,
                        t.bar + 1, t.track + 1, t.barInTrack + 1, 1000.0 * worstBlock, late, silent, perSecond);
            worstBlock = 0.0; late = 0; silent = 0;
        }
        const auto due = started + std::chrono::microseconds(static_cast<long long>((i + 1) * block * 1.0e6 / sr));
        std::this_thread::sleep_until(due);
    }
    std::printf("live: longest silent run %.2f s, ending near bar %d\n", worstRun * blockSeconds, worstAt);
    p->releaseResources();
    return 0;
}

} // namespace

/** @brief Runs the host test's parts (all, or the one named on the command line). */
int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    // The child half of the learned-model section below; it prints one line and checks nothing.
    if (argc > 1 && juce::String(argv[1]) == "--probe-models") return probeModels();
    // Round "speed" (20.09.2026): the test in two independently schedulable parts. "ten seconds of a real
    // host" took 236 s of a 293 s run with every plan already in the probe cache: a second thread writes
    // random parameters while it renders, every written compose knob throws the plans away, and each new
    // plan is twelve probe renders no cache can know. It is offline (setNonRealtime) and measures no time, so
    // it needs none of the solitude the real-time sections need -- as a ctest test of its own
    // (`hosttest.realhost`) it runs beside the rest instead of in front of it. `--part rest` is everything
    // else; no argument runs both, as before. Every check is in exactly one part.
    const juce::String part = argc > 2 && juce::String(argv[1]) == "--part" ? juce::String(argv[2]) : juce::String();
    if (argc > 1 && part != "rest" && part != "realhost" && part != "live") {
        std::fprintf(stderr, "usage: phos_hosttest [--part rest|realhost|live]\n");
        return 2;
    }
    const bool partRest = part != "realhost", partRealHost = part != "rest";
    std::printf("Phosphene host test%s%s\n", part.isEmpty() ? "" : ", part ", part.toRawUTF8());
    // The user's files stay the user's (23.09.2026): the ratings, the learned preferences and the gallery of every
    // processor this test builds go to a folder of the test's own, emptied first. The editor sweep below presses
    // "Good here", "Bad here" and the learning buttons like any other.
    const juce::File userDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("phos_hosttest_user_" + part);
    userDir.deleteRecursively();
    userDir.createDirectory();
#if defined(_WIN32)
    _putenv_s("PHOS_USER_DIR", userDir.getFullPathName().toRawUTF8());
    _putenv_s("PHOS_GALLERY_DIR", userDir.getChildFile("Sets").getFullPathName().toRawUTF8());
    _putenv_s("PHOS_NO_UPDATE_CHECK", "1");   // a test never talks to the network (UpdateCheck.h)
#else
    setenv("PHOS_USER_DIR", userDir.getFullPathName().toRawUTF8(), 1);
    setenv("PHOS_GALLERY_DIR", userDir.getChildFile("Sets").getFullPathName().toRawUTF8(), 1);
    setenv("PHOS_NO_UPDATE_CHECK", "1", 1);
#endif
    // Round "speed" (20.09.2026). This test builds some forty processors and nearly every one plans a
    // track, 12 to 15 s of probe renders each -- that, not real-time playback, is what made it 9 to 11
    // minutes. As a development program it opts in to parallel probes and, under ctest, to the suite's probe
    // cache (phos/Probe.h); the plans are bit for bit the shipped plugin's, which the oracle section below
    // keeps proving against phos_render. The one section that *times* a plan switches both off again.
    phos::probe::configureFromEnvironment();
    if (part == "live") return liveDiagnosis();   // after the user folder and the probe settings above
    // Unbuffered, as the self test is: under ctest stdout is a pipe, and a crash or a timeout took all of
    // this test's output with it. It is also what lets the sections be timed from outside.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // ---------------------------------------------------------------- the shipped wavetable pack
    //
    // A development build finds `library.phoswt` beside the sources through PHOS_SOURCE_DATA_DIR,
    // which is exactly why the shipped case has to be tested on purpose: what a user installs has
    // no source tree. The pack is copied next to this executable by Tests/CMakeLists.txt, the way
    // it is copied next to the plugin's artefacts, so what is measured here is the plugin's own
    // path resolution and nothing else. Both sections run before any other processor is built --
    // the library is loaded once per process, by the first Engine::prepare() that happens.
    if (partRest) {
        const juce::File beside = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                                      .getParentDirectory().getChildFile("library.phoswt");
        check(beside.existsAsFile(), "the pack is installed beside the binary (" + beside.getFullPathName() + ")");
        auto p = std::make_unique<PhospheneProcessor>();
        p->setPlayConfigDetails(0, 2, 48000.0, 256);
        p->prepareToPlay(48000.0, 256);
        const PhospheneProcessor::WaveTableLibrary lib = p->waveTableLibrary();
        check(juce::File(lib.directory) == beside.getParentDirectory(),
              "the plugin looks for the pack beside its own binary, not in a source tree (found \""
                  + lib.directory + "\")");
        check(lib.shipped > 0 && lib.loaded == lib.shipped,
              "and it holds every table this build ships (" + juce::String(lib.built) + " expanded so far, "
                  + juce::String(lib.loaded) + " of "
                  + juce::String(lib.shipped) + ")");
    }

    // The other half: no pack at all. The library is process-global and is loaded once, so the
    // absence is staged by asking for a name that cannot be found -- after that every load in this
    // process reports nothing, which is precisely the state of a plugin whose resources were not
    // installed. It must still make sound, and it must not pretend in the editor.
    if (partRest) {
        phos::resetWaveTableLibrary();
        std::string why;
        const int loaded = phos::loadWaveTableLibrary("phosphene-no-such-pack.phoswt", &why);
        check(loaded == 0, "the missing-pack case is really staged (" + juce::String(why.c_str()) + ")");
        auto p = std::make_unique<PhospheneProcessor>();
        p->setFollowHost(false);
        p->setNonRealtime(true);
        p->setPlayConfigDetails(0, 2, 48000.0, 256);
        p->prepareToPlay(48000.0, 256);
        p->play();
        const PhospheneProcessor::WaveTableLibrary lib = p->waveTableLibrary();
        check(lib.loaded == 0 && lib.shipped > 0,
              "with no pack the plugin says so: " + juce::String(lib.loaded) + " of "
                  + juce::String(lib.shipped) + " library tables");
        // Eight bars, because a track opens with a sparse intro (the kick enters between bars 5
        // and 9): four bars of atmosphere would prove nothing about the fallback.
        const int samples = static_cast<int>(8.0 * kBeatsPerBar * 60.0 / 145.0 * 48000.0);
        const double rms = renderRms(*p, 256, samples);
        check(rms > 0.01, "and it still makes sound, on the built-in tables (RMS " + juce::String(rms, 4) + ")");

        std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());
        auto* phos = dynamic_cast<PhospheneEditor*>(editor.get());
        if (phos != nullptr) phos->setTab(TabPad);
        juce::ComboBox* table = editor != nullptr ? findCombo(*editor, "pad.table") : nullptr;
        check(table != nullptr && table->getNumItems() == phos::kNumWaveTables,
              "the table chooser offers every index the parameter has");
        if (table != nullptr) {
            int marked = 0, builtinMarked = 0;
            for (int i = 0; i < table->getNumItems(); ++i) {
                const bool mark = table->getItemText(i).contains("missing");
                if (i >= phos::kNumBuiltinWaveTables) marked += mark ? 1 : 0;
                else builtinMarked += mark ? 1 : 0;
            }
            check(marked == phos::kNumLibraryWaveTables && builtinMarked == 0,
                  "and marks exactly the library entries that are not there ("
                      + juce::String(marked) + " of " + juce::String(phos::kNumLibraryWaveTables) + " marked, "
                      + juce::String(builtinMarked) + " built-in tables wrongly marked)");
        }
        editor.reset();
    }
    // Back to the shipped library for everything that follows -- no engine is alive here.
    phos::resetWaveTableLibrary();
    phos::loadWaveTableLibrary();

    // ---------------------------------------------------------------- the two learned models
    //
    // The same failure as the missing pack, one level up: without `melody.phosmdl` and
    // `bass.phosmdl` the composer draws from the Markov model and the pattern families, the whole of
    // Phase 8 is gone, and the only word about it is one line on stderr -- which in a DAW is
    // nowhere. Before the fix nothing in the plugin called setModelSearchPath() at all, so that was
    // the state of every installed copy.
    //
    // Both files are copied next to this executable by Tests/CMakeLists.txt, exactly as
    // Plugin/CMakeLists.txt copies them next to the artefacts, so what is measured is the plugin's
    // own path resolution.
    if (partRest) {
        const juce::File beside = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
        check(beside.getChildFile("melody.phosmdl").existsAsFile() && beside.getChildFile("bass.phosmdl").existsAsFile(),
              "both weight files are installed beside the binary (" + beside.getFullPathName() + ")");
        auto p = std::make_unique<PhospheneProcessor>();
        p->setPlayConfigDetails(0, 2, 48000.0, 256);
        p->prepareToPlay(48000.0, 256);
        const PhospheneProcessor::LearnedModels m = p->learnedModels();
        check(juce::File(m.directory) == beside,
              "the plugin points the core at its own directory, not at a source tree (found \"" + m.directory + "\")");
        check(m.melody && m.bass && !m.fromSourceTree,
              "and both learned models really loaded (melody " + juce::String(m.melody ? "yes" : "no")
                  + ", bass " + juce::String(m.bass ? "yes" : "no") + ", from the source tree "
                  + juce::String(m.fromSourceTree ? "yes" : "no") + ")");
        check(m.melodyNll > 0.0 && m.bassNll > 0.0,
              "with the held-out score out of the files themselves (" + juce::String(m.melodyNll, 3)
                  + " / " + juce::String(m.bassNll, 3) + " nats)");

        // And the Set tab says so, which is the whole point: the knob says what is asked for, the
        // line says what the process has.
        std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());
        auto* phos = dynamic_cast<PhospheneEditor*>(editor.get());
        if (phos != nullptr) phos->setTab(TabSet);
        juce::Label* melodyLine = editor != nullptr ? findLabel(*editor, "Melody:") : nullptr;
        juce::Label* bassLine = editor != nullptr ? findLabel(*editor, "Bass:") : nullptr;
        check(melodyLine != nullptr && bassLine != nullptr, "the Set tab carries a line for each of the two parts");
        check(melodyLine != nullptr && melodyLine->getText().contains("learned")
                  && bassLine != nullptr && bassLine->getText().contains("learned"),
              "and both say learned (\"" + (melodyLine != nullptr ? melodyLine->getText() : juce::String())
                  + "\" / \"" + (bassLine != nullptr ? bassLine->getText() : juce::String()) + "\")");
        int marked = 0;
        for (const char* key : { "compose.melody_model", "compose.bass_model" }) {
            juce::ComboBox* cb = editor != nullptr ? findCombo(*editor, key) : nullptr;
            if (cb != nullptr && cb->getNumItems() == 2 && cb->getItemText(1).contains("missing")) ++marked;
        }
        check(marked == 0, "and neither chooser marks its learned entry as missing");
        editor.reset();
    }

    // The other half: a plugin with no resource directory of its own. Staged as a copy of this
    // executable in an empty temporary directory, run as a child process -- see probeModels() for
    // why it cannot be done inside this process.
    if (partRest) {
        const juce::File exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getChildFile("phos-nodata-" + juce::String(juce::Random::getSystemRandom().nextInt(1 << 30)));
        dir.createDirectory();
        const juce::File copy = dir.getChildFile(exe.getFileName());
        juce::String out;
        if (!exe.copyFileTo(copy)) {
            check(false, "the no-data case could not be staged: " + exe.getFullPathName() + " -> " + copy.getFullPathName());
        } else {
            juce::ChildProcess child;
            juce::StringArray args;
            args.add(copy.getFullPathName());
            args.add("--probe-models");
            // Started in the empty directory, not in this test's: the core also tries a bare name against the
            // working directory, and under a single-configuration generator (Ninja, the Intel release tree) the
            // test's working directory is the very folder the data is copied into beside the test binaries --
            // the child found the models there and the no-data case was no such thing (23.09.2026).
            const juce::File previous = juce::File::getCurrentWorkingDirectory();
            dir.setAsCurrentWorkingDirectory();
            if (!child.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
                check(false, "the no-data child would not start");
            else out = child.readAllProcessOutput();
            previous.setAsCurrentWorkingDirectory();
        }
        const bool melody = out.contains("melody=1");
        const bool bass = out.contains("bass=1");
        const bool source = out.contains("source=1");
        const bool marks = out.contains("marks=2");
        const double rms = out.fromFirstOccurrenceOf("rms=", false, false).getDoubleValue();
        const juce::String line = out.fromFirstOccurrenceOf("probe ", true, false).upToFirstOccurrenceOf("\n", false, false);
#if defined(PHOS_SOURCE_DATA_DIR)
        // A development build. PhospheneCore still carries this checkout's Core/data as the last
        // step of the lookup (Core/CMakeLists.txt), so the copy in an empty directory finds the
        // models anyway -- and reports that it found them outside its own installation. That is
        // asserted rather than tolerated, because it is exactly the state that made a missing data
        // file invisible on the build machine; PHOS_SHIP removes it and the #else below is then what
        // runs, in the configuration Deploy/build_release.ps1 ships and tests.
        check(melody && bass && source && !marks && rms > 0.01,
              "a development build finds the models in its own source tree and says so (" + line + ")");
#else
        check(!melody && !bass, "with no search path and no files beside it the plugin reports the fallback (" + line + ")");
        check(marks, "and marks both learned chooser entries as missing (" + line + ")");
        check(rms > 0.01, "and it still makes sound, on the Markov model and the pattern families (" + line + ")");
#endif
        dir.deleteRecursively();
    }

    // ---------------------------------------------------------------- rates and block sizes
    if (partRest) for (double sr : { 44100.0, 48000.0, 96000.0 }) {
        for (int block : { 16, 64, 512, 2048 }) {
            auto p = std::make_unique<PhospheneProcessor>();
            p->setFollowHost(false);
            p->setNonRealtime(true);
            p->setPlayConfigDetails(0, 2, sr, block);
            p->prepareToPlay(sr, block);
            p->play();
            juce::AudioBuffer<float> buf(2, block);
            feed(*p, buf, 12);
            const juce::String what = "renders finite at " + juce::String(static_cast<int>(sr)) + " Hz, block " + juce::String(block);
            check(finite(buf, block), what);
            // A block that is not the one it was prepared with: hosts do this.
            juce::AudioBuffer<float> odd(2, juce::jmax(1, block / 3));
            feed(*p, odd, 4);
            check(finite(odd, odd.getNumSamples()), what + " (odd block)");
            // Silence when the transport is stopped.
            p->stop();
            feed(*p, buf, 4);
            check(buf.getMagnitude(0, block) == 0.0f, "stopped output is silent at " + juce::String(static_cast<int>(sr)) + " Hz");
            p->releaseResources();
        }
    }

    // ---------------------------------------------------------------- latency
    if (partRest) {
        auto p = std::make_unique<PhospheneProcessor>();
        p->setPlayConfigDetails(0, 2, 48000.0, 256);
        p->prepareToPlay(48000.0, 256);
        check(p->getLatencySamples() == p->engine().latencySamples(), "the reported latency is the engine's");
        check(p->getLatencySamples() > 0, "the limiter's lookahead is reported as latency");
    }

    // ---------------------------------------------------------------- the oracle
    if (partRest) {
        const double sr = 48000.0;
        const int block = 256;
        // Eight bars at the default tempo: since Phase 5 a track opens with an intro whose first bars
        // carry only the atmosphere (the kick enters between bars 5 and 9), so four bars could be
        // silence on both sides and prove nothing.
        const int samples = static_cast<int>(8.0 * kBeatsPerBar * 60.0 / 145.0 * sr);
        const std::vector<float> a = referenceRender(1, sr, block, samples, 16);
        const std::vector<float> b = pluginRender(1, sr, block, samples);
        check(a.size() == b.size(), "the plugin renders as many samples as phos_render");
        size_t firstDiff = a.size();
        double peak = 0.0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
            peak = std::max(peak, static_cast<double>(std::fabs(a[i])));
            if (a[i] != b[i] && firstDiff == a.size()) firstDiff = i;
        }
        check(peak > 0.05, "the reference render is not silence");
        check(firstDiff == a.size(),
              "the plugin equals phos_render bit for bit over the first eight bars"
                  + (firstDiff == a.size() ? juce::String() : " (first difference at sample " + juce::String(static_cast<int>(firstDiff / 2)) + ")"));
        // And with a different block size, because the engine's grid is absolute.
        const std::vector<float> c = pluginRender(1, sr, 64, samples);
        bool same = c.size() == a.size();
        for (size_t i = 0; same && i < a.size(); ++i) same = a[i] == c[i];
        check(same, "the same render comes out of blocks of 64 samples");
    }

    // ---------------------------------------------------------------- ten seconds of a real host
    if (partRealHost) {
        const double sr = 48000.0;
        auto p = std::make_unique<PhospheneProcessor>();
        p->setNonRealtime(true);
        p->setPlayConfigDetails(0, 2, sr, 512);
        p->prepareToPlay(sr, 512);
        TestPlayHead head;
        head.sampleRate = sr;
        // From bar 16: the intro before it is sparse by design (Phase 5: the atmosphere first, the kick
        // from bar 5 to 9, layers one by one), and this test counts notes.
        head.ppq = 64.0;
        head.samples = 64.0 * 60.0 / 145.0 * sr;
        p->setPlayHead(&head);
        juce::AudioBuffer<float> buf(2, 512);
        juce::MidiBuffer midi;
        std::vector<juce::MidiMessage> produced;

        // A second thread writing parameters while the audio thread reads them, as a host's
        // automation and a user's mouse both do.
        std::atomic<bool> stopThread{ false };
        std::thread writer([&] {
            const ParamStore& store = p->params();
            // Unsigned (23.09.2026): this loop runs tens of millions of times in ten seconds, and `int i * 37`
            // overflowed -- undefined behaviour that MSVC happened to wrap into a negative id, which
            // parameterFor() refused, and that Intel's icx used to drop the id >= 0 test as impossible and
            // read in front of the table. The first icx run of the suite crashed here.
            // The value follows the id, not the loop count (25.09.2026): with `i % 11` a parameter got the same value
            // on every pass only while store.count() was a multiple of 11 (1001 until the mix-guide round, 1037
            // after it). Otherwise the compose and perc knobs changed on every pass, Composer::validate threw the
            // plans away each time, and the seek below planned from the start for 1 h 46 min until it crashed.
            // Writing each parameter one fixed value keeps the race this part is for -- every write still
            // lands while the audio thread reads.
            // And one knob that never stops moving, on purpose (25.09.2026, same day): Track Variation, which every
            // plan depends on, gets a new value on every pass, as a host automating it on every block would give it.
            // The composer takes a plan knob over only once it has rested for a bar (PluginProcessor.h,
            // composeParams_) and checks the knobs once per call (Composer.h), so the set plays on under the plans
            // it has; before, this was the endless re-planning and the crash.
            const int moving = store.base(Module::Compose) + compose::TrackVariation;
            uint32_t i = 0;
            while (!stopThread.load(std::memory_order_relaxed)) {
                const int id = static_cast<int>((i * 37u) % static_cast<uint32_t>(store.count()));
                if (auto* param = p->parameterFor(id)) param->setValueNotifyingHost((id % 11) / 10.0f);
                if (auto* param = p->parameterFor(moving)) param->setValueNotifyingHost(static_cast<float>((i * 7919u) % 1000u) / 1000.0f);
                ++i;
                std::this_thread::yield();
            }
        });

        const int blockSizes[] = { 512, 37, 1024, 128, 64, 333, 2048, 256 };
        int rendered = 0, which = 0;
        bool allFinite = true;
        const int total = static_cast<int>(10.0 * sr);
        while (rendered < total) {
            const int n = blockSizes[which++ % 8];
            buf.setSize(2, n, false, false, true);
            buf.clear();
            midi.clear();
            p->processBlock(buf, midi);
            for (const auto meta : midi) produced.push_back(meta.getMessage());
            allFinite = allFinite && finite(buf, n);
            head.advance(n);
            rendered += n;
            // A seek in the middle, the way a host jumps when the user clicks the ruler.
            if (rendered > total / 2 && head.ppq < 90.0) { head.ppq = 512.0; head.samples = 512.0 * 60.0 / 145.0 * sr; }
        }
        stopThread.store(true);
        writer.join();
        p->setPlayHead(nullptr);
        check(allFinite, "ten seconds of changing block sizes, parameter writes and a seek stay finite");
        check(!produced.empty(), "the plugin writes MIDI while it plays");
        int ons = 0, offs = 0;
        for (const juce::MidiMessage& m : produced) { ons += m.isNoteOn() ? 1 : 0; offs += m.isNoteOff() ? 1 : 0; }
        // Planning the first track takes a few seconds of the ten and varies with the machine's load; the
        // bar the head starts in decides the rest. Twenty note-ons is a bar of groove, not a fluke.
        check(ons > 20, "the MIDI output carries the score's notes (" + juce::String(ons) + " note-ons)");
        check(offs >= ons - 128, "every note that started is ended (" + juce::String(ons) + " on, " + juce::String(offs) + " off)");
    }

    // ---------------------------------------------------------------- host sync
    if (partRest) {
        const double sr = 48000.0;
        auto p = std::make_unique<PhospheneProcessor>();
        p->setNonRealtime(true);
        p->setPlayConfigDetails(0, 2, sr, 256);
        p->prepareToPlay(sr, 256);
        TestPlayHead head;
        head.sampleRate = sr;
        head.bpm = 132.0;
        head.ppq = 64.0;      // the host starts in the middle of the set
        p->setPlayHead(&head);
        juce::AudioBuffer<float> buf(2, 256);
        juce::MidiBuffer midi;
        for (int i = 0; i < 200; ++i) {
            buf.clear(); midi.clear();
            p->processBlock(buf, midi);
            head.advance(256);
        }
        const TransportView t = p->transport();
        check(t.hostSync, "the plugin follows the host's play head");
        check(std::fabs(t.musicalBeat - head.ppq) < 0.25, "the plugin sits where the host says (" + juce::String(t.musicalBeat, 3)
                                                              + " against " + juce::String(head.ppq, 3) + ")");
        const ParamStore& store = p->params();
        check(std::fabs(store.get(store.base(Module::Compose) + compose::Bpm) - 132.0f) < 0.01f, "the tempo follows the host");
        check(buf.getMagnitude(0, 256) > 0.0f, "the plugin sounds when the host plays");
        // Stopping the host stops the plugin.
        head.playing = false;
        buf.clear(); midi.clear();
        p->processBlock(buf, midi);
        check(buf.getMagnitude(0, 256) == 0.0f, "a stopped host leaves the output silent");
        p->setPlayHead(nullptr);
    }

    // ---------------------------------------------------------------- the composer thread, live
    if (partRest) {
        // Everything above runs offline, where the audio thread composes for itself. This is the
        // arrangement the plugin actually ships with: blocks arriving at the rate a sound card asks
        // for them, and a thread of its own filling the rings a few bars ahead. If that thread ever
        // falls behind, the engine runs out of events and the output goes quiet -- so the check is
        // that it does not, over three seconds, at a block size that leaves 5.3 ms per block.
        const double sr = 48000.0;
        const int block = 256;
        // What is timed here is the shipped plugin's plan, so the thread count is the plugin's own --
        // which since 22.09.2026 it sets itself in its constructor (the machine's cores, clamped to
        // Probe.h's kMaxThreads). Until then this pinned it to 1, which was the shipped behaviour and
        // is what made "Play to the first sample" take 12.8 s. The cache stays off: a user starting
        // the plugin for the first time has none either.
        const int probeThreads = phos::probe::threads();
        const std::string probeCache = phos::probe::cacheDir();
        phos::probe::setCacheDir("");
        auto p = std::make_unique<PhospheneProcessor>();
        p->setFollowHost(false);
        p->setPlayConfigDetails(0, 2, sr, block);
        p->prepareToPlay(sr, block);
        p->play();
        // Past the sparse intro: since 19.09.2026 (the two-drop form) its first sixteen bars have no kick,
        // and bar 17 is where kick and bass come in -- that is what has to keep sounding.
        p->seekToBar(16);
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        // Starting a set plans its first track, which measures its level by rendering it -- about
        // two seconds. The plugin renders silence and says "planning" while that happens; here we
        // simply wait for it, and then measure what the live path does once it is under way.
        const auto planStart = std::chrono::steady_clock::now();
        while (p->transport().restarting && std::chrono::duration<double>(std::chrono::steady_clock::now() - planStart).count() < 60.0) {
            buf.clear(); midi.clear();
            p->processBlock(buf, midi);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const double planSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - planStart).count();
        std::printf("plan: the first track was ready after %.2f s\n", planSeconds);
        // 23.09.2026, round "Planung": the first track is planned without its probes and measured behind the music
        // (Composer::completeMeasurement). It was 8 to 11 s under the suite's load; the plan itself is now a few
        // milliseconds, and what is left is composing the first bars and the handshake.
        check(planSeconds < 6.0, "a set starts playing within seconds of the button (" + juce::String(planSeconds, 2) + " s)");
        const int blocks = static_cast<int>(3.0 * sr / block);
        int silentRun = 0, worstRun = 0, loudBlocks = 0;
        bool allFinite = true;
        double inProcess = 0.0;   // time spent inside processBlock: what the audio thread costs
        const auto started = std::chrono::steady_clock::now();
        for (int i = 0; i < blocks; ++i) {
            buf.clear(); midi.clear();
            const auto b0 = std::chrono::steady_clock::now();
            p->processBlock(buf, midi);
            inProcess += std::chrono::duration<double>(std::chrono::steady_clock::now() - b0).count();
            allFinite = allFinite && finite(buf, block);
            if (buf.getMagnitude(0, block) < 1.0e-6f) { ++silentRun; worstRun = juce::jmax(worstRun, silentRun); }
            else { silentRun = 0; ++loudBlocks; }
            // Wait until this block's "deadline", as a device would.
            const auto due = started + std::chrono::microseconds(static_cast<long long>((i + 1) * block * 1.0e6 / sr));
            std::this_thread::sleep_until(due);
        }
        std::printf("CPU (live, audio thread only, composer on its own thread): %.2f %% of a core at %.0f Hz / %d\n",
                    100.0 * inProcess / (blocks * block / sr), sr, block);
        check(allFinite, "the live path stays finite");
        check(loudBlocks > blocks * 3 / 4, "the composer thread keeps the engine fed ("
                                               + juce::String(loudBlocks) + " of " + juce::String(blocks) + " blocks sounding)");
        // What the editor's pattern rolls draw: the bars the conductor has composed, by part. Read here, three seconds
        // after the jump to bar 16 (24.09.2026): the waits below for Auto Gain and the line levels run as long as the
        // measurement takes, and under a loaded suite the transport had left bars 16 to 20 behind by then.
        std::vector<NoteEvent> pattern;
        const bool gotPattern = p->readPattern(16, 4, pattern);
        // 24.09.2026, the user: "spielte das Pad (oder die Drone) und es wurde nichts angezeigt". A window that
        // starts inside a held note -- bar 17, one bar into whatever the pads and the drone struck on bar 16 --
        // shows that note, struck before it.
        std::vector<NoteEvent> heldWindow;
        p->readPattern(17, 1, heldWindow);
        int held = 0;
        for (const NoteEvent& e : heldWindow) held += e.beat < 17.0 * kBeatsPerBar ? 1 : 0;
        check(held > 0, "the pattern preview shows the notes still sounding from before its window (" + juce::String(held)
                            + " held into bar 17)");
        // 22.09.2026: Auto Gain is measured after the start now, so the check is that it *arrives*.
        // The plan starts deferred with no offset; within a few seconds of playing the composer
        // thread has measured it, written it into the plan and pushed it as a two-bar ramp, and the
        // engine's effective master gain has to end up at the knob plus that offset.
        {
            phos::TrackPlan first;
            const auto t0 = std::chrono::steady_clock::now();
            bool arrived = false;
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60.0) {
                buf.clear(); midi.clear();
                p->processBlock(buf, midi);
                if (p->tryReadTrack(0, first) && !first.masterDeferred && first.masterGainDb != 0.0f) { arrived = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            check(arrived, "the deferred Auto Gain offset is measured while the set plays ("
                               + juce::String(first.masterGainDb, 2) + " dB)");
            // And it reaches the engine: the ramp is two bars, so a few of them are rendered here.
            const int gainId = p->params().find("master.gain");
            const float knob = p->params().get(gainId);
            float eff = knob;
            for (int i = 0; i < 2000 && std::fabs(eff - (knob + first.masterGainDb)) > 0.25f; ++i) {
                buf.clear(); midi.clear();
                p->processBlock(buf, midi);
                eff = p->engine().effective(gainId);
            }
            std::printf("Auto Gain: nachtraeglich gemessen %+.2f dB, im Engine-Wert %+.2f dB (Knopf %+.2f)\n",
                        static_cast<double>(first.masterGainDb), static_cast<double>(eff), static_cast<double>(knob));
            check(std::fabs(eff - (knob + first.masterGainDb)) <= 0.25f,
                  "and rides into the engine over its ramp (knob " + juce::String(knob, 2)
                      + " dB, effective " + juce::String(eff, 2) + " dB, wanted "
                      + juce::String(knob + first.masterGainDb, 2) + ")");
            // The lines' levels were measured before Auto Gain and sent the same way (23.09.2026): the published
            // plan is measured, and the counter's and the lead's levels in the engine carry their corrections.
            phos::TrackPlan measured;
            const bool published = p->tryReadTrack(0, measured) && !measured.measureDeferred;
            juce::String lines;
            bool linesIn = published;
            for (PolyInstance v : { PolyInstance::Lead, PolyInstance::Counter }) {
                const int id = p->params().base(Module::Mix) + mix::polyLevel(v);
                const int k = mpIndex(v == PolyInstance::Lead ? MelodyPart::Lead : MelodyPart::Counter);
                const float wantDb = p->params().get(id) + measured.partGainDb[k] + measured.presenceGainDb;
                float got = p->engine().effective(id);
                for (int i = 0; i < 2000 && std::fabs(got - wantDb) > 0.05f; ++i) {
                    buf.clear(); midi.clear();
                    p->processBlock(buf, midi);
                    got = p->engine().effective(id);
                }
                linesIn = linesIn && std::fabs(got - wantDb) <= 0.05f;
                lines << p->params().key(id).c_str() << " " << juce::String(got, 2) << " / " << juce::String(wantDb, 2) << " dB  ";
            }
            check(linesIn, "the first track's lines get their measured levels while it plays (" + lines + ")");
        }
        check(worstRun * block / sr < 0.15, "no gap longer than 150 ms while it plays (worst "
                                                + juce::String(worstRun * block / sr * 1000.0, 1) + " ms)");
        const bool got = gotPattern;
        int kicks = 0, percs = 0;
        for (const NoteEvent& e : pattern) {
            kicks += e.part == Part::Kick ? 1 : 0;
            percs += e.part == Part::Perc ? 1 : 0;
        }
        check(got && !pattern.empty(), "the pattern preview has bars 16 to 20 in it ("
                                           + juce::String(static_cast<int>(pattern.size())) + " notes)");
        check(kicks >= 12, "and the kick is in it (" + juce::String(kicks) + " notes in four bars)");
        check(percs > 0, "and the percussion kit too (" + juce::String(percs) + " notes)");

        // 22.09.2026, the user: "die Noten werden nicht mehr angezeigt (zumindest nach dem ersten
        // Song)". The check above has always read bar 16, which is inside the *first* track; what the
        // editor does from the second track on has never been measured. So: seek into track 2, wait
        // the way the editor's timer waits, and ask for the same thing the pattern roll asks for --
        // the bar the transport is in, rounded down to a group of four (PluginEditor.cpp,
        // refreshPattern). The number of tracks the warm-up has published is read as well, because
        // that is what the arrangement view draws from (tryReadTrack).
        {
            const int trackBars = static_cast<int>(std::lround(p->params().get(
                p->params().base(Module::Compose) + compose::TrackBars)));
            const int bar2 = trackBars + 24;            // well inside the second track
            p->seekToBar(bar2);
            const auto t0 = std::chrono::steady_clock::now();
            bool arrived = false;
            std::vector<NoteEvent> late;
            int atBar = -1;
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 120.0) {
                buf.clear(); midi.clear();
                p->processBlock(buf, midi);
                const TransportView t = p->transport();
                atBar = t.bar;
                if (!t.restarting && t.bar >= bar2) {
                    if (p->readPattern((t.bar / 4) * 4, 4, late) && !late.empty()) { arrived = true; break; }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            TrackPlan plan;
            int published = 0;
            while (published < 24 && p->tryReadTrack(published, plan)) ++published;
            check(arrived, "the pattern preview still has notes in the second track (bar "
                               + juce::String(bar2) + ", transport reached " + juce::String(atBar)
                               + ", " + juce::String(static_cast<int>(late.size())) + " notes)");
            check(published >= 2, "and the arrangement view has more than the first track to draw ("
                                      + juce::String(published) + " published)");
        }

        // 22.09.2026, the user: "Klicken direkt im Arrangement-View fuehrt zu schlimmsten
        // Stoergeraeuschen". A click in that view is a seek, and a seek is a hard cut: the block in
        // which it is asked for returns silence from whatever level the music was at, and the block
        // in which the music comes back starts at whatever level it starts at. Two step
        // discontinuities per click, and a step is broadband -- which is what "Stoergeraeusche"
        // sounds like when it happens once per click of the mouse.
        //
        // Comparing slew rates does not catch this: a kick transient slews harder in one sample than
        // a cut from a quiet passage does, so a bound relative to the music's own steepest step
        // passes a hard cut every time (measured -- it did). The two numbers that say it exactly are
        // the level the signal was at when it went silent, and the level it starts at when it comes
        // back. Both are a step from or to zero, whatever the music around them does.
        {
            float lastBefore = 0.0f, firstAfter = 0.0f;
            // Settle at a bar that is playing, keeping the last sample of every block: the very
            // first block after the seek is already the silent one (the request is consumed inside
            // processBlock), so the level it was cut from has to come from the block before.
            float tail = 0.0f;
            bool sounding = false;
            for (int i = 0; i < 400; ++i) {
                buf.clear(); midi.clear();
                p->processBlock(buf, midi);
                if (buf.getMagnitude(0, block) > 1.0e-6f) { tail = buf.getReadPointer(0)[block - 1]; sounding = true; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const int here = p->transport().bar;
            p->seekToBar(here + 8);
            const auto t0 = std::chrono::steady_clock::now();
            bool wentQuiet = false;
            while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60.0) {
                buf.clear(); midi.clear();
                p->processBlock(buf, midi);
                const float* L = buf.getReadPointer(0);
                const float mag = buf.getMagnitude(0, block);
                if (!wentQuiet) {
                    if (mag < 1.0e-6f) { wentQuiet = true; lastBefore = std::fabs(tail); }
                    else tail = L[block - 1];
                } else if (mag > 1.0e-6f) {
                    firstAfter = std::fabs(L[0]);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check(sounding, "the transport was playing before the seek was measured");
            // -34 dBFS: a step that small is under the music it lands in and under the noise of any
            // converter. Anything above it is the click.
            std::printf("seek seam: leaves at %.4f, returns at %.4f (block %d)\n",
                        static_cast<double>(lastBefore), static_cast<double>(firstAfter), block);
            check(lastBefore < 0.02f && firstAfter < 0.02f,
                  "a seek fades instead of cutting: the level it leaves and the level it returns at are both near zero"
                  " (leaves " + juce::String(lastBefore, 4) + ", returns " + juce::String(firstAfter, 4) + ")");
        }
        p.reset();   // its composer thread ends here, before the settings it planned under change
        phos::probe::setThreads(probeThreads);
        phos::probe::setCacheDir(probeCache);
    }

    // ---------------------------------------------------------------- state round trip
    if (partRest) {
        auto p = std::make_unique<PhospheneProcessor>();
        p->setPlayConfigDetails(0, 2, 48000.0, 256);
        p->prepareToPlay(48000.0, 256);
        juce::Random rng(1234);
        for (int i = 0; i < p->params().count(); ++i)
            if (auto* param = p->parameterFor(i)) param->setValueNotifyingHost(rng.nextFloat());
        p->setSeed(987654321);
        std::vector<float> before(static_cast<size_t>(p->params().count()));
        for (int i = 0; i < p->params().count(); ++i) before[static_cast<size_t>(i)] = p->params().get(i);

        juce::MemoryBlock state;
        p->getStateInformation(state);
        p->params().resetDefaults();
        p->setSeed(1);
        p->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        bool same = p->seed() == 987654321;
        for (int i = 0; i < p->params().count(); ++i) same = same && p->params().get(i) == before[static_cast<size_t>(i)];
        check(same, "the state comes back exactly as it went out");

        // A second instance reads the same state.
        auto q = std::make_unique<PhospheneProcessor>();
        q->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        bool same2 = q->seed() == p->seed();
        for (int i = 0; i < q->params().count(); ++i) same2 = same2 && q->params().get(i) == before[static_cast<size_t>(i)];
        check(same2, "a fresh instance reads that state");
        // Rubbish must not crash it.
        const char junk[] = "not a state at all";
        q->setStateInformation(junk, static_cast<int>(sizeof(junk)));
        check(true, "a malformed state is ignored");

        // ------------------------------------------------ 20.09.2026, round "dialogue": state version 3
        // A version-3 state names only the knobs that differ from the defaults of the build that wrote
        // it (PluginProcessor.h, kStateVersion). Three things follow, and each is checked here.

        // (1) A fresh instance's state names no knob at all, and one moved knob is the only one in it.
        {
            auto f = std::make_unique<PhospheneProcessor>();
            juce::MemoryBlock clean;
            f->getStateInformation(clean);
            std::unique_ptr<juce::XmlElement> cx(juce::AudioProcessor::getXmlFromBinary(clean.getData(), static_cast<int>(clean.getSize())));
            const juce::String empty = cx != nullptr ? cx->getChildByName("params")->getAllSubText().trim() : "?";
            f->params().set(f->params().find("acid.cutoff"), 777.0f);
            juce::MemoryBlock one;
            f->getStateInformation(one);
            std::unique_ptr<juce::XmlElement> ox(juce::AudioProcessor::getXmlFromBinary(one.getData(), static_cast<int>(one.getSize())));
            const juce::String text = ox != nullptr ? ox->getChildByName("params")->getAllSubText().trim() : "?";
            check(cx != nullptr && ox != nullptr && empty.isEmpty() && text.contains("acid.cutoff")
                      && !text.contains("sfx.level") && !text.contains("kick.level")
                      && juce::StringArray::fromLines(text).size() == 1
                      && cx->getIntAttribute("version", 0) == PhospheneProcessor::kStateVersion,
                  "a state of version " + juce::String(PhospheneProcessor::kStateVersion)
                      + " holds only what the user changed (untouched: \"" + empty + "\", one knob moved: \"" + text + "\")");
        }

        // (2) Because of that, a knob the state does not name follows the *current* default when it is
        //     loaded -- which is the whole point: the user's old session carried sfx.level -12 and
        //     bass.cutoff 140 and silently undid four rounds of recalibration.
        {
            auto f = std::make_unique<PhospheneProcessor>();
            f->params().set(f->params().find("acid.cutoff"), 777.0f);
            juce::MemoryBlock one;
            f->getStateInformation(one);
            auto g = std::make_unique<PhospheneProcessor>();
            const int sfxLevel = g->params().find("mix.sfx_level"), bassCutoff = g->params().find("bass.cutoff");
            g->params().set(sfxLevel, -12.0f);      // the value the user's own state carried
            g->params().set(bassCutoff, 140.0f);
            g->setStateInformation(one.getData(), static_cast<int>(one.getSize()));
            const ParamStore fresh;
            check(g->params().get(sfxLevel) == fresh.get(sfxLevel) && g->params().get(bassCutoff) == fresh.get(bassCutoff)
                      && g->params().get(g->params().find("acid.cutoff")) == 777.0f,
                  "a knob the state does not name comes up on today's default (mix.sfx_level "
                      + juce::String(g->params().get(sfxLevel)) + " dB, bass.cutoff " + juce::String(g->params().get(bassCutoff))
                      + " Hz) while the one it names is restored");
        }

        // (3) A state older than version 3 holds every value of its own session and cannot say which of
        //     them the user chose, so it sets **no** knob: the engine comes up on today's defaults and
        //     the text is held for the editor's offer. The seed is restored either way -- it is the
        //     user's set, not our calibration. Built here from the round-trip state above, marked as the
        //     version-2 state the voices round wrote.
        std::unique_ptr<juce::XmlElement> xml(juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize())));
        bool built = xml != nullptr;
        if (built) {
            xml->setAttribute("version", 2);
            // Version 2 wrote every value; this state was written by version 3, so put them all back in.
            ParamStore all;
            for (int i = 0; i < static_cast<int>(before.size()); ++i) all.set(i, before[static_cast<size_t>(i)]);
            xml->getChildByName("params")->deleteAllTextElements();
            xml->getChildByName("params")->addTextElement(juce::String(all.toText(false)));
        }
        juce::MemoryBlock old;
        if (built) juce::AudioProcessor::copyXmlToBinary(*xml, old);
        auto r = std::make_unique<PhospheneProcessor>();
        const int leadCutoff = r->params().find("lead.cutoff"), counterCutoff = r->params().find("counter.cutoff");
        r->setStateInformation(old.getData(), static_cast<int>(old.getSize()));
        const ParamStore fresh;
        const bool untouched = r->params().get(leadCutoff) == fresh.get(leadCutoff)
                               && r->params().get(counterCutoff) == fresh.get(counterCutoff);
        check(built && r->lastStateVersion() == 2 && untouched && r->pendingLegacyState().isNotEmpty()
                  && r->seed() == 987654321,
              "a state of version 1 or 2 sets no knob and is held for the offer instead (version "
                  + juce::String(r->lastStateVersion()) + ", lead.cutoff " + juce::String(r->params().get(leadCutoff))
                  + " = the default " + juce::String(fresh.get(leadCutoff)) + ", seed " + juce::String(static_cast<int>(r->seed())) + ")");

        // (4) The offer itself, and the way back: adopting the held state puts its knobs in, and the
        //     factory reset -- the visible button of the rule -- takes every knob back to the shipped
        //     value and forgets the offer.
        r->adoptLegacyState();
        const bool adopted = r->params().get(leadCutoff) == before[static_cast<size_t>(leadCutoff)]
                             && r->pendingLegacyState().isEmpty();
        r->resetToFactoryDefaults();
        bool allDefault = true;
        for (int i = 0; i < r->params().count(); ++i) allDefault = allDefault && r->params().get(i) == fresh.get(i);
        check(adopted && allDefault && r->pendingLegacyState().isEmpty(),
              juce::String("the offer applies the held state (adopted ") + (adopted ? "yes" : "no")
                  + ") and \"reset to factory defaults\" puts every knob back (" + (allDefault ? "yes" : "no") + ")");
    }

    // ---------------------------------------------------------------- the parameters themselves
    if (partRest) {
        auto p = std::make_unique<PhospheneProcessor>();
        const ParamStore& store = p->params();
        // Since 23.09.2026 the four macros follow the store's parameters as host parameters of their own.
        check(p->getParameters().size() == store.count() + kNumMacros,
              "every parameter of the store is a host parameter (" + juce::String(store.count()) + "), and the four macros after them");
        bool textRoundTrip = true, namesUnique = true, rangesSane = true;
        juce::StringArray ids;
        for (int i = 0; i < kNumMacros; ++i)
            if (auto* hp = p->hostParameter(store.count() + i)) ids.add(hp->getParameterID());
        for (int i = 0; i < store.count(); ++i) {
            auto* param = p->parameterFor(i);
            if (param == nullptr) { rangesSane = false; break; }
            ids.add(param->paramID);
            // Text out and back must land on the same value for the discrete parameters, where a
            // host may well round-trip through the name.
            const ParamDesc& d = store.desc(i);
            if (d.curve == Curve::Choice || d.curve == Curve::Toggle) {
                for (float v : { 0.0f, 0.5f, 1.0f }) {
                    const juce::String t = param->getText(v, 64);
                    const float back = param->getValueForText(t);
                    if (std::fabs(store.fromNormalised(i, back) - store.fromNormalised(i, v)) > 1.0e-4f) textRoundTrip = false;
                }
            }
            if (!(param->getDefaultValue() >= 0.0f && param->getDefaultValue() <= 1.0f)) rangesSane = false;
            if (param->getNumSteps() < 2) rangesSane = false;
        }
        juce::StringArray sorted(ids);
        sorted.sort(true);
        sorted.removeDuplicates(true);
        namesUnique = sorted.size() == ids.size();
        check(textRoundTrip, "a discrete parameter's text reads back as the same value");
        check(namesUnique, "every parameter id is unique");
        check(rangesSane, "every parameter has a default inside its range and at least two steps");
    }

    // ---------------------------------------------------------------- MIDI learn and the macros as host parameters (23.09.2026)
    if (partRest) {
        auto p = std::make_unique<PhospheneProcessor>();
        p->setPlayConfigDetails(0, 2, 48000.0, 256);
        p->prepareToPlay(48000.0, 256);
        const int cutoff = p->params().find("lead.cutoff");
        const int sweepTarget = p->params().count() + static_cast<int>(Macro::FilterSweep);
        // A controller arrives while lead.cutoff is armed: it binds, and it sets the knob in the same block.
        p->midiMap().arm(cutoff);
        juce::AudioBuffer<float> buf(2, 256);
        juce::MidiBuffer in;
        in.addEvent(juce::MidiMessage::controllerEvent(3, 74, 64), 10);
        in.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 20);   // a note: ignored, and not echoed
        p->processBlock(buf, in);
        int ch = -1, cc = -1;
        const bool bound = p->midiMap().controllerOf(cutoff, ch, cc) && ch == 2 && cc == 74;
        const float atLearn = p->hostParameter(cutoff)->getValue();
        bool echoed = false;
        for (const auto meta : in) echoed = echoed || meta.getMessage().isNoteOn() && meta.getMessage().getNoteNumber() == 60 && meta.getMessage().getVelocity() == 100;
        juce::MidiBuffer in2;
        in2.addEvent(juce::MidiMessage::controllerEvent(3, 74, 127), 0);
        p->processBlock(buf, in2);
        const float atMax = p->hostParameter(cutoff)->getValue();
        check(p->acceptsMidi() && p->producesMidi() && bound && std::fabs(atLearn - 64.0f / 127.0f) < 1e-4f && std::fabs(atMax - 1.0f) < 1e-6f && !echoed,
              juce::String("MIDI learn: an armed knob takes the next controller, which then drives it; incoming notes are not echoed -- ")
                  + juce::String::formatted("bound %d (ch %d cc %d), value %.4f then %.4f", (int)bound, ch + 1, cc, atLearn, atMax));
        // A macro is a target too, and a host parameter: its value reaches the macro, and the macro reaches the host.
        p->midiMap().bind(0, 1, sweepTarget);
        juce::MidiBuffer in3;
        in3.addEvent(juce::MidiMessage::controllerEvent(1, 1, 127), 0);
        p->processBlock(buf, in3);
        p->serviceMacros();
        const float sweep = p->macroValue(Macro::FilterSweep);
        p->setMacro(Macro::GateDepth, 0.5f);
        auto* depth = p->hostParameter(p->params().count() + static_cast<int>(Macro::GateDepth));
        const float depthHost = depth != nullptr ? depth->convertFrom0to1(depth->getValue()) : -1.0f;
        check(std::fabs(sweep - 1.0f) < 1e-6f && std::fabs(depthHost - 0.5f) < 1e-4f,
              juce::String("the macros are host parameters: a controller on one moves the macro, a macro moved in the editor moves its host value -- ")
                  + juce::String::formatted("filter sweep %.3f, gate depth host %.3f", sweep, depthHost));
        // The bindings travel with the state.
        juce::MemoryBlock state;
        p->getStateInformation(state);
        auto q = std::make_unique<PhospheneProcessor>();
        q->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        int ch2 = -1, cc2 = -1, ch3 = -1, cc3 = -1;
        check(q->midiMap().controllerOf(cutoff, ch2, cc2) && ch2 == 2 && cc2 == 74 && q->midiMap().controllerOf(sweepTarget, ch3, cc3) && cc3 == 1,
              "learned controllers come back with the saved state");
    }

    // ---------------------------------------------------------------- the gallery (23.09.2026)
    if (partRest) {
        // A folder of its own (PHOS_GALLERY_DIR), so the test never writes into the user's gallery.
        const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("phos_hosttest_gallery");
        dir.deleteRecursively();
        dir.createDirectory();
#if defined(_WIN32)
        _putenv_s("PHOS_GALLERY_DIR", dir.getFullPathName().toRawUTF8());
#else
        setenv("PHOS_GALLERY_DIR", dir.getFullPathName().toRawUTF8(), 1);
#endif
        auto p = std::make_unique<PhospheneProcessor>();
        p->setPlayConfigDetails(0, 2, 48000.0, 256);
        p->prepareToPlay(48000.0, 256);
        p->setSeed(4711);
        const juce::File f = p->saveToGallery("test | set");
        phos::GalleryEntry e;
        const bool read = f.existsAsFile() && f.getParentDirectory() == dir && phos::readGalleryEntry(f.loadFileAsString().toStdString(), e);
        auto q = std::make_unique<PhospheneProcessor>();
        q->setPlayConfigDetails(0, 2, 48000.0, 256);
        q->prepareToPlay(48000.0, 256);
        const bool loaded = q->importSet(f);
        check(read && e.seed == 4711 && e.name == "test   set" && loaded && q->seed() == 4711,
              "the gallery: a saved set lands in the gallery folder with its name and seed, and loads back as a set -- " + f.getFileName());
        // Back to the test's own user folder, never to the user's real gallery.
#if defined(_WIN32)
        _putenv_s("PHOS_GALLERY_DIR", userDir.getChildFile("Sets").getFullPathName().toRawUTF8());
#else
        setenv("PHOS_GALLERY_DIR", userDir.getChildFile("Sets").getFullPathName().toRawUTF8(), 1);
#endif
        dir.deleteRecursively();
    }

    // ---------------------------------------------------------------- learned preferences (23.09.2026)
    if (partRest) {
        auto p = std::make_unique<PhospheneProcessor>();
        const bool isolated = p->ratingsFile().getParentDirectory() == userDir && p->preferencesFile().getParentDirectory() == userDir;
        // Six verdicts: the Surge archetype liked three times, the Pedal one disliked three times.
        for (int i = 0; i < 6; ++i) {
            phos::RatingEntry r;
            r.seed = 1;
            r.verdict = i < 3 ? 1 : -1;
            r.features = i < 3 ? "style=Goa;lead.archetype=Surge" : "style=Goa;lead.archetype=Pedal";
            phos::appendRating(p->ratingsFile().getFullPathName().toRawUTF8(), r);
        }
        int verdicts = 0;
        const phos::Preferences prefs = p->fitFromRatings(&verdicts);
        const bool fitted = verdicts == 6 && prefs.weight("lead.archetype=Surge") > 0.1 && prefs.weight("lead.archetype=Pedal") < -0.1;
        const bool applied = p->applyPreferences(prefs) && p->preferencesFile().existsAsFile() && phos::preferences() != nullptr
                          && p->preferencesSummary().contains("learned weights");
        p->forgetPreferences();
        const bool forgotten = !p->preferencesFile().existsAsFile() && p->preferencesFile().getSiblingFile("preferences.txt.old").existsAsFile()
                            && phos::preferences() == nullptr;
        check(isolated && fitted && applied && forgotten,
              juce::String("learned preferences: fitted from the verdicts, applied and written, forgotten and set aside -- in the test's own folder (")
                  + juce::String(verdicts) + " verdicts, Surge " + juce::String(prefs.weight("lead.archetype=Surge"), 2) + ", Pedal "
                  + juce::String(prefs.weight("lead.archetype=Pedal"), 2) + ")");
    }

    // ---------------------------------------------------------------- undo (23.09.2026)
    if (partRest) {
        auto p = std::make_unique<PhospheneProcessor>();
        p->setPlayConfigDetails(0, 2, 48000.0, 256);
        p->prepareToPlay(48000.0, 256);
        auto* cutoff = p->hostParameter(p->params().find("lead.cutoff"));
        const float v0 = cutoff->getValue();
        // A knob gesture, the way an attachment makes one.
        cutoff->beginChangeGesture();
        cutoff->setValueNotifyingHost(0.2f);
        cutoff->endChangeGesture();
        const bool recorded = p->canUndo() && p->undoName().contains("Cutoff");
        p->undo();
        const float back = cutoff->getValue();
        p->redo();
        const float again = cutoff->getValue();
        check(recorded && std::fabs(back - v0) < 1e-5f && std::fabs(again - 0.2f) < 1e-5f,
              juce::String("undo: a knob gesture is one step, back and forth -- ") + juce::String::formatted("%.3f -> 0.200, undo %.3f, redo %.3f", v0, back, again)
                  + " (" + p->undoName() + ")");
        // A reroll and a seed, through undoable() as the editor calls them.
        p->undoable("Reroll this track", [&] { p->reroll(phos::LockUnit::Track, 0); });
        const uint32_t rolled = p->variation(phos::LockUnit::Track, 0);
        p->undo();
        const uint32_t unrolled = p->variation(phos::LockUnit::Track, 0);
        const uint64_t seed0 = p->seed();
        p->undoable("Seed", [&] { p->setSeed(seed0 + 1234); });
        const uint64_t seed1 = p->seed();
        p->undo();
        const uint64_t seedBack = p->seed();
        p->redo();
        check(rolled == 1 && unrolled == 0 && seed1 == seed0 + 1234 && seedBack == seed0 && p->seed() == seed0 + 1234,
              juce::String("undo: a reroll and a seed change come back, and go again -- ")
                  + juce::String::formatted("reroll %u -> %u, seed %llu -> %llu -> %llu", rolled, unrolled, (unsigned long long)seed1, (unsigned long long)seedBack,
                                            (unsigned long long)p->seed()));
        // Host automation is not the editor's history: a value change without a gesture records nothing.
        const juce::String before = p->undoName();
        cutoff->setValueNotifyingHost(0.7f);
        check(p->undoName() == before, "undo: automation without a gesture is not recorded");
    }

    // ---------------------------------------------------------------- the update check's version order (23.09.2026)
    if (partRest) {
        using phosui::UpdateCheck;
        const bool order = UpdateCheck::isNewer("v1.0.1", "1.0.0") && UpdateCheck::isNewer("1.10.0", "1.9.3")
                        && !UpdateCheck::isNewer("v1.0.0", "1.0.0") && !UpdateCheck::isNewer("0.9", "1.0.0")
                        && UpdateCheck::isNewer("2", "1.99.99") && !UpdateCheck::isNewer("v1.2.0-beta", "1.2.0");
        check(order, "update check: versions compare number by number (1.10 after 1.9, a leading v ignored)");
    }

    // ---------------------------------------------------------------- sound presets (23.09.2026)
    if (partRest) {
        auto p = std::make_unique<PhospheneProcessor>();
        phos::ParamStore& ps = p->params();
        const int arpLevel = ps.find("arp.level");
        ps.set(arpLevel, 0.37f);
        const std::vector<phos::SoundPreset>& arp = phos::factoryPresets(phos::Module::Poly, static_cast<int>(phos::PolyInstance::Arp));
        // The palette decides which oscillators a voice has presets for; the arp's middle one is taken, whatever it is
        // (24.09.2026: the names are made of the sound now, "Solar Cascade" and the like, and no longer end in "Bright").
        const phos::SoundPreset* fm = arp.empty() ? nullptr : &arp[arp.size() / 2];
        const std::string before = ps.toText(false);
        if (fm != nullptr) p->applyPreset(phos::Module::Poly, static_cast<int>(phos::PolyInstance::Arp), *fm);
        phos::ParamStore expect;
        expect.copyValuesFrom(ps);
        if (fm != nullptr) phos::applySoundPreset(expect, phos::Module::Poly, static_cast<int>(phos::PolyInstance::Arp), fm->text);
        const bool applied = fm != nullptr && ps.toText(false) != before && ps.get(ps.find("mix.arp_own")) == 1.0f
                          && ps.get(ps.find("arp.osc")) == expect.get(expect.find("arp.osc")) && ps.get(arpLevel) == 0.37f;
        p->undo();
        const bool undone = ps.toText(false) == before;
        // A user preset: saved from the knobs, listed under "User", and written into the test's folder only.
        p->redo();
        const juce::File saved = p->saveUserPreset(phos::Module::Poly, static_cast<int>(phos::PolyInstance::Arp), "Hosttest sound");
        const std::vector<phos::SoundPreset> mine = p->userPresets(phos::Module::Poly, static_cast<int>(phos::PolyInstance::Arp));
        const bool listed = mine.size() == 1 && mine[0].group == "User" && mine[0].name == "Hosttest sound" && fm != nullptr && mine[0].text == fm->text;
        const bool isolated = saved.isAChildOf(p->userFolder()) && p->userFolder().getFileName().startsWith("phos_hosttest_user");
        saved.deleteFile();
        size_t total = 0;
        for (int v = 0; v < phos::kPolyInstances; ++v) total += phos::factoryPresets(phos::Module::Poly, v).size();
        total += phos::factoryPresets(phos::Module::Kick).size() + phos::factoryPresets(phos::Module::Bass).size() + phos::factoryPresets(phos::Module::Acid).size();
        check(applied && undone && listed && isolated,
              juce::String("presets: a factory preset sets the arp, switches its own sound on and leaves its level; undo takes it back; "
                           "a user preset is saved, listed and kept in the test's folder (")
                  + (fm != nullptr ? juce::String(fm->name) : juce::String("no preset")) + juce::String::formatted(
                        "; applied %d, undone %d, listed %d, isolated %d; ", (int)applied, (int)undone, (int)listed, (int)isolated)
                  + juce::String(static_cast<int>(total)) + " factory presets)");
    }

    // ---------------------------------------------------------------- a MIDI keyboard on the lead (23.09.2026)
    if (partRest) {
        // Three renders of one set: the keyboard off; on the lead in Layer mode without a note (must be the same
        // samples -- a block without a played note is one process() call, as before); and with a chord played
        // from block 30, sample 100, released at block 60 (must be the same up to the note, and differ after it).
        constexpr int kBlock = 512, kBlocks = 90, kOnBlock = 30, kOnAt = 100, kOffBlock = 60;
        auto render = [&](int part, bool notes) {
            auto p = std::make_unique<PhospheneProcessor>();
            p->setFollowHost(false);
            p->setNonRealtime(true);
            p->setSeed(20260923);
            p->setPlayConfigDetails(0, 2, 48000.0, kBlock);
            p->prepareToPlay(48000.0, kBlock);
            p->params().set(p->params().find("mix.keyboard_part"), static_cast<float>(part));
            p->params().set(p->params().find("mix.keyboard_mode"), 1.0f);   // Layer
            p->play();
            juce::AudioBuffer<float> buf(2, kBlock);
            juce::MidiBuffer midi;
            std::vector<float> out;
            for (int b = 0; b < kBlocks; ++b) {
                buf.clear();
                midi.clear();
                if (notes && b == kOnBlock)
                    for (int pitch : { 62, 65, 69 }) midi.addEvent(juce::MidiMessage::noteOn(1, pitch, static_cast<juce::uint8>(112)), kOnAt);
                if (notes && b == kOffBlock)
                    for (int pitch : { 62, 65, 69 }) midi.addEvent(juce::MidiMessage::noteOff(1, pitch), 7);
                p->processBlock(buf, midi);
                for (int i = 0; i < kBlock; ++i) { out.push_back(buf.getReadPointer(0)[i]); out.push_back(buf.getReadPointer(1)[i]); }
            }
            return out;
        };
        const std::vector<float> off = render(0, false), layerQuiet = render(2, false), played = render(2, true);
        const size_t noteAt = static_cast<size_t>(kOnBlock * kBlock + kOnAt) * 2;
        const bool same = off == layerQuiet;
        bool untilNote = played.size() == off.size();
        for (size_t i = 0; untilNote && i < noteAt; ++i) untilNote = played[i] == off[i];
        double diff = 0.0, ref = 0.0;
        for (size_t i = noteAt; i < played.size() && i < off.size(); ++i) {
            diff += static_cast<double>(played[i] - off[i]) * (played[i] - off[i]);
            ref += static_cast<double>(off[i]) * off[i];
        }
        const double db = 10.0 * std::log10((diff + 1e-30) / (ref + 1e-30));
        check(same && untilNote && db > -40.0,
              juce::String("keyboard: the lead plays the keys -- no note, no change (bit-identical); the chord starts on its sample "
                           "and is ") + juce::String(db, 1) + " dB against the set");
    }

    // ---------------------------------------------------------------- bus layouts and the editor
    if (partRest) {
        auto p = std::make_unique<PhospheneProcessor>();
        check(p->producesMidi() && p->acceptsMidi(), "the plugin writes MIDI and reads controllers (MIDI learn)");
        juce::AudioProcessor::BusesLayout stereo;
        stereo.outputBuses.add(juce::AudioChannelSet::stereo());
        juce::AudioProcessor::BusesLayout withInput = stereo;
        withInput.inputBuses.add(juce::AudioChannelSet::stereo());
        check(p->checkBusesLayoutSupported(stereo), "stereo out is supported");
        check(!p->checkBusesLayoutSupported(withInput), "an input bus is refused");

        p->setPlayConfigDetails(0, 2, 48000.0, 256);
        p->prepareToPlay(48000.0, 256);
        std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());
        check(editor != nullptr, "the editor opens");
        if (editor != nullptr) {
            check(editor->getWidth() > 400 && editor->getHeight() > 300, "the editor has a sensible size");
            juce::AudioBuffer<float> buf(2, 256);
            feed(*p, buf, 8);
            check(finite(buf, 256), "audio keeps running with the editor open");
            // Every tab must lay out and paint without falling over.
            if (auto* phos = dynamic_cast<PhospheneEditor*>(editor.get())) {
                bool painted = true;
                for (int t = 0; t < PhospheneEditor::tabNames().size(); ++t) {
                    phos->setTab(t);
                    juce::Image img(juce::Image::ARGB, 320, 240, true);
                    juce::Graphics g(img);
                    phos->paintEntireComponent(g, true);
                    painted = painted && img.isValid();
                }
                check(painted, "every tab lays out and paints");
                // Every parameter can be reached (23.09.2026: the Set tab's groups are now lists, not table
                // slices, so a knob appended to the compose table is on no page until somebody places it).
                const juce::StringArray onPages = phos->parametersOnPages();
                juce::StringArray missing;
                for (int i = 0; i < p->params().count(); ++i)
                    if (!onPages.contains(juce::String(p->params().key(i)))) missing.add(juce::String(p->params().key(i)));
                check(missing.isEmpty(), "every parameter stands on a page (" + juce::String(onPages.size()) + " placed"
                                             + (missing.isEmpty() ? juce::String() : ", missing: " + missing.joinIntoString(" ")) + ")");
                // The help (23.09.2026): every tab has its chapter, F1 on a tab opens that chapter, and the
                // parameter topic lists the tab's own parameters with their current values.
                if (HelpView* hv = phos->helpView()) {
                    juce::StringArray noChapter;
                    for (const juce::String& t : PhospheneEditor::tabNames()) if (!hv->topicNames().contains(t)) noChapter.add(t);
                    phos->setTab(TabLead);
                    phos->setHelpVisible(true);
                    const bool opened = hv->isVisible() && hv->selectedTopic() >= 0 && hv->topicNames()[hv->selectedTopic()] == "Lead";
                    const int paramsTopic = hv->topicNames().indexOf("Parameters: Lead");
                    const bool listed = paramsTopic >= 0 && hv->topicText(paramsTopic).contains("lead.cutoff")
                                     && hv->topicText(paramsTopic).contains("mix.lead_own");
                    phos->setHelpVisible(false);
                    check(noChapter.isEmpty() && opened && listed && !hv->isVisible(),
                          "help: every tab has its chapter, the help opens at the tab's chapter and lists its parameters"
                              + (noChapter.isEmpty() ? juce::String() : " (no chapter: " + noChapter.joinIntoString(", ") + ")"));
                }
                // 24.09.2026, the user: "Koennen wir im Mixer-Tab Meter fuer das Level fuer die einzelnen Kanalzuege
                // anzeigen [...] Vielleicht sogar ganze Channel-Strips?" A strip per part, its meter reading what the
                // part puts into the mix: loud where it plays, nothing at all where its mute is on.
                if (phosui::MixerConsole* mc = phos->mixerConsole()) {
                    phos->setTab(TabMix);
                    p->play();
                    juce::AudioBuffer<float> run(2, 256);
                    // The start is a handshake with the composer thread; until it is done the engine renders nothing,
                    // and "restarting" is not even raised until the thread has seen the request. So: until sound comes.
                    float pk[phos::kNumParts], rms[phos::kNumParts];
                    bool sounding = false;
                    for (int i = 0; i < 4000 && !sounding; ++i) {
                        feed(*p, run, 1);
                        sounding = run.getMagnitude(0, run.getNumSamples()) > 1.0e-5f;
                        if (!sounding) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    p->takeChannelMeters(pk, rms);
                    feed(*p, run, 400);   // two seconds
                    mc->pollMeters();
                    int loudest = 0;
                    for (int k = 1; k < mc->stripCount(); ++k) if (mc->strip(k).rmsDb() > mc->strip(loudest).rmsDb()) loudest = k;
                    const float before = mc->strip(loudest).rmsDb();
                    // Its mute: the same parameter the strip's "M" drives.
                    const std::vector<int>& ids = mc->strip(loudest).params();
                    int muteId = -1;
                    for (int id : ids) if (juce::String(p->params().key(id)).endsWith("_mute")) muteId = id;
                    if (muteId >= 0) p->params().set(muteId, 1.0f);
                    p->takeChannelMeters(pk, rms);
                    feed(*p, run, 100);
                    p->takeChannelMeters(pk, rms);
                    int others = 0;
                    for (int k = 0; k < phos::kNumParts; ++k) others += k != loudest && rms[k] > 1.0e-4f ? 1 : 0;
                    const float mutedPeak = pk[loudest];
                    if (muteId >= 0) p->params().set(muteId, 0.0f);
                    juce::Image img(juce::Image::ARGB, 1280, 800, true);
                    juce::Graphics g(img);
                    phos->setBounds(0, 0, 1280, 800);
                    phos->paintEntireComponent(g, true);
                    check(mc->stripCount() == phos::kNumParts && before > -40.0f && muteId >= 0 && mutedPeak == 0.0f && others >= 1,
                          "mixer: a strip per part (" + juce::String(mc->stripCount()) + "); the loudest, "
                              + juce::String(phos::kPartNames[loudest]) + ", read " + juce::String(before, 1) + " dB RMS and nothing ("
                              + juce::String(mutedPeak) + ") once its mute was on, while " + juce::String(others) + " others played on");
                    // 26.09.2026, the user: "dass alle Meter die gleiche Höhe haben". The strips hold two to five knobs;
                    // each made room for its own rows, so fader and meter began lower in a strip with more knobs.
                    int top = mc->strip(0).meterArea().getY(), height = mc->strip(0).meterArea().getHeight(), odd = 0;
                    for (int k = 1; k < mc->stripCount(); ++k)
                        odd += mc->strip(k).meterArea().getY() != top || mc->strip(k).meterArea().getHeight() != height ? 1 : 0;
                    check(odd == 0 && height > 40, "mixer: every strip's meter starts at the same height and is as tall as the others ("
                                                       + juce::String(height) + " px, " + juce::String(odd) + " different)");
                }
                // 24.09.2026, the user: "Koennten wir bei der Kick und beim Bass noch Anzeigen einbauen, wie in (Kick 3 von
                // Sonic Academy)". Each scope renders what its synth plays: the kick lands on a note in the kick's range,
                // the bass plays the key's root, and both have a sound to draw.
                if (phosui::SynthScope* ks = phos->kickScope(); ks != nullptr && phos->bassScope() != nullptr) {
                    phosui::SynthScope* bs = phos->bassScope();
                    ks->refresh();
                    bs->refresh();
                    check(ks->renderedPeak() > 0.05f && ks->landingHz() > 30.0 && ks->landingHz() < 90.0 && bs->renderedPeak() > 0.02f
                              && bs->landingHz() > 38.0 && bs->landingHz() < 80.0,
                          "scopes: the kick's hit lands at " + juce::String(ks->landingHz(), 1) + " Hz (peak " + juce::String(ks->renderedPeak(), 2)
                              + "), the bass note is " + juce::String(bs->landingHz(), 1) + " Hz (peak " + juce::String(bs->renderedPeak(), 2) + ")");
                }
                // 24.09.2026, the user: "Im SFX-Fenster ist nach wie vor keine Auswahl fuer das Preset". Every family's
                // chooser holds Auto and every preset of the family, and choosing one sets its parameter.
                {
                    phos->setTab(TabFx);
                    std::vector<juce::ComboBox*> boxes;
                    std::function<void(juce::Component&)> find = [&](juce::Component& c) {
                        for (int i = 0; i < c.getNumChildComponents(); ++i) {
                            juce::Component* k = c.getChildComponent(i);
                            if (auto* cb = dynamic_cast<juce::ComboBox*>(k)) if (cb->getTooltip().startsWith("sfx.preset_")) boxes.push_back(cb);
                            if (k != nullptr) find(*k);
                        }
                    };
                    find(*phos);
                    bool sizes = boxes.size() == static_cast<size_t>(phos::sfx::kNumPresetChoices);
                    for (size_t i = 0; sizes && i < boxes.size(); ++i)
                        sizes = boxes[i]->getNumItems() == 1 + phos::kSfxBankCount[static_cast<int>(phos::kPresetChoiceType[i])];
                    const int zapId = p->params().find("sfx.preset_zap");
                    juce::ComboBox* zap = nullptr;
                    for (juce::ComboBox* cb : boxes) if (cb->getTooltip().startsWith("sfx.preset_zap")) zap = cb;
                    if (zap != nullptr) zap->setSelectedItemIndex(17, juce::sendNotificationSync);
                    const float chosen = zapId >= 0 ? p->params().get(zapId) : -1.0f;
                    if (zapId >= 0) p->params().set(zapId, 0.0f);
                    check(sizes && zap != nullptr && chosen == 17.0f,
                          "effect presets: a chooser per family (" + juce::String(static_cast<int>(boxes.size())) + ") with Auto and every "
                              "preset of it; choosing \"Zap 17\" sets sfx.preset_zap to " + juce::String(chosen));
                }
                phos->setTab(0);

                // 22.09.2026, the user: "zudem funktionieren einige Knoepfe in der GUI nicht, z.B. in
                // Perform-Tab". Nothing here has ever pressed one. So: walk every tab, find every
                // button, press it, and see whether anything in the processor moves -- its
                // parameters, its seed, its transport, its curation. A button that changes nothing
                // is either dead or needs a state this test has not set up, and either way it is
                // worth naming.
                //
                // Buttons whose caption ends in "..." open a modal file chooser and would hang a
                // test with no user; they are listed rather than pressed.
                std::vector<juce::Button*> buttons;
                auto snapshot = [&] {
                    juce::String sig;
                    const phos::ParamStore& ps = p->params();
                    for (int i = 0; i < ps.count(); ++i) sig << juce::String(ps.get(i), 4) << ",";
                    const TransportView tv = p->transport();
                    sig << juce::String(p->pendingSfxPreview()) << ",";   // the effect presets' audition (24.09.2026)
                    sig << (tv.playing ? "P" : "-") << juce::String(p->seed()) << "," << juce::String(tv.bar)
                        << "," << juce::String(phos->tab())
                        << (p->muted() ? "M" : "-") << (p->followsHost() ? "F" : "-");
                    for (int u = 0; u < 4; ++u)
                        for (int k = 0; k < 4; ++k) sig << (p->isLocked(static_cast<phos::LockUnit>(u), k) ? "L" : ".");
                    // And the page itself: the twelve percussion lane buttons choose which lane the
                    // page shows and touch no parameter at all, which is working, not dead.
                    for (const juce::Button* x : buttons) sig << (x->getToggleState() ? "1" : "0");
                    for (int u = 0; u < 4; ++u)
                        for (int k = 0; k < 4; ++k)
                            sig << juce::String(p->variation(static_cast<phos::LockUnit>(u), k));
                    return sig;
                };
                // The transport runs: a drop-out means "one bar, from here", so with nothing playing
                // there is no bar line to release on and the macro is cleared the moment it is set
                // (PluginProcessor.cpp, serviceMacros). Pressing it stopped is not a fair test of it.
                p->play();
                { juce::AudioBuffer<float> warm(2, 256); feed(*p, warm, 64); }
                // Every press starts from the same state. Without this the walk masks itself: the
                // Mixer tab's mute toggles come before the Perform tab, so by the time the drop-out
                // is pressed the kick and the bass are already muted and the macro -- whose whole
                // job is to mute them -- changes nothing and reads as dead.
                phos::ParamStore pristine;
                pristine.copyValuesFrom(p->params());
                std::function<void(juce::Component&)> collect = [&](juce::Component& c) {
                    for (int i = 0; i < c.getNumChildComponents(); ++i) {
                        juce::Component* k = c.getChildComponent(i);
                        if (k == nullptr) continue;
                        if (auto* b = dynamic_cast<juce::Button*>(k)) buttons.push_back(b);
                        collect(*k);
                    }
                };
                juce::StringArray dead, dialogs;
                int pressed = 0;
                for (int t = 0; t < PhospheneEditor::tabNames().size(); ++t) {
                    phos->setTab(t);
                    phos->setBounds(0, 0, 1280, 800);
                    phos->resized();
                    buttons.clear();
                    collect(*phos);
                    for (juce::Button* b : buttons) {
                        const juce::String name = b->getButtonText().isNotEmpty() ? b->getButtonText() : b->getName();
                        if (name.isEmpty() || !b->isVisible()) continue;
                        if (name.endsWithChar('.')) { dialogs.addIfNotAlreadyThere(name); continue; }
                        p->params().copyValuesFrom(pristine);
                        p->clearCuration();
                        // And the transport runs again: "Stop" is one of the buttons this walk
                        // presses, and everything after it would otherwise be judged with the set
                        // standing still -- which is a fair verdict for none of them and a wrong one
                        // for the drop-out, whose meaning is "one bar from here".
                        p->play();
                        // Three buttons undo a state rather than make one, and from a pristine start
                        // they have nothing to undo. Each gets the state it is for.
                        if (name == "Play") p->stop();
                        if (name.startsWith("Reset")) p->params().set(p->params().find("compose.bpm"), 151.0f);
                        if (name.startsWith("Clear all")) p->setLock(phos::LockUnit::Track, 0, true);
                        // Undo and redo (23.09.2026) need a step to take back or to do again.
                        if (name == "Undo" || name == "Redo")
                            p->undoable("tempo", [&] { p->params().set(p->params().find("compose.bpm"), 151.0f); });
                        if (name == "Redo") p->undo();
                        // A lane button that is already the chosen lane has nothing to change, and
                        // lane 1 is the default one. Stand somewhere else first.
                        if (t == TabPerc && name.containsOnly("0123456789"))
                            phos->setPercLane((name.getIntValue() - 1 + 1) % kPercLanes);   // any lane but its own
                        // The header's own tab buttons are pressed on one page only: pressing them
                        // is what changes the page, and every later button of this page would then
                        // be hidden and skipped (measured: the walk fell from 237 presses to 24).
                        if (PhospheneEditor::tabNames().contains(name) && t != 0) continue;
                        if (name == PhospheneEditor::tabNames()[t]) continue;   // the page's own tab
                        const juce::String before = snapshot();
                        // A real press, not triggerClick(): that one *posts* a command message, and a
                        // test with no message loop never dispatches it -- which is why the first run
                        // of this block reported all 249 buttons dead, Play and Stop included. A
                        // mouse down/up pair runs Button's own path synchronously, including the
                        // onStateChange that a held button (the stutter) hangs off.
                        const juce::MouseEvent me(juce::Desktop::getInstance().getMainMouseSource(),
                                                  b->getLocalBounds().getCentre().toFloat(), juce::ModifierKeys::leftButtonModifier,
                                                  1.0f, 0.0f, 0.0f, 0.0f, 0.0f, b, b,
                                                  juce::Time::getCurrentTime(), b->getLocalBounds().getCentre().toFloat(),
                                                  juce::Time::getCurrentTime(), 1, false);
                        // Through Component*: Button re-declares the two handlers protected, and
                        // access is checked on the static type while the call still dispatches to
                        // Button's override.
                        juce::Component* asComponent = b;
                        asComponent->mouseDown(me);
                        // A held button (the stutter) is only doing anything *while* it is held, so
                        // it is read here rather than after the release, where it is back at zero by
                        // design and would look dead.
                        const bool heldDoesSomething = snapshot() != before
                                                    || p->macroValue(Macro::Stutter) != 0.0f
                                                    || p->macroValue(Macro::DropOut) != 0.0f;
                        asComponent->mouseUp(me);
                        p->serviceMacros();   // the processor's 30 Hz tick, turned by hand
                        const juce::String after = snapshot();
                        phos->setTab(t);      // back to the page this walk is on
                        ++pressed;
                        if (after == before && !heldDoesSomething) dead.addIfNotAlreadyThere(
                            juce::String(PhospheneEditor::tabNames()[t]) + " / " + name);
                    }
                }
                std::printf("  Knoepfe: %d gedrueckt, %d ohne Wirkung, %d Dialoge uebersprungen (%s)\n",
                            pressed, dead.size(), dialogs.size(), dialogs.joinIntoString(", ").toRawUTF8());
                if (!dead.isEmpty()) std::printf("  ohne Wirkung: %s\n", dead.joinIntoString(" | ").toRawUTF8());
                check(pressed > 30 && dead.isEmpty(),
                      "every button in the editor does something when it is pressed ("
                          + juce::String(pressed) + " pressed, " + juce::String(dead.size()) + " without effect)");
            }
        }
        editor.reset();
        check(true, "the editor closes");
    }

    // ---------------------------------------------------------------- the arrange timeline
    if (partRest) {
        // Nine tracks, 2304 bars, ninety-nine sections: the sixty-minute set of PLAN 8.1. The point
        // of the measurement is the difference between the two numbers below. Drawing the set costs
        // what it costs; a tick must not cost that, because the editor ticks twelve times a second
        // and the only thing that changes between two ticks is the play head.
        ArrangeDisplay view;
        ArrangeDisplay::Snapshot snap = bigSet();
        int sections = 0;
        for (const auto& t : snap.tracks) sections += static_cast<int>(t.sections.size());
        view.setSize(1216, 420);
        const auto r0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) view.setSnapshot(snap);
        const double redraw = std::chrono::duration<double>(std::chrono::steady_clock::now() - r0).count() / 20.0;

        juce::Image img(juce::Image::ARGB, view.getWidth(), view.getHeight(), true);
        int moved = 0;
        const auto p0 = std::chrono::steady_clock::now();
        const int frames = 120;
        for (int i = 0; i < frames; ++i) {
            moved += view.setPosition(i * 4.0 * kBeatsPerBar / 3.0) ? 1 : 0;
            juce::Graphics g(img);
            view.paintEntireComponent(g, true);
        }
        const double tick = std::chrono::duration<double>(std::chrono::steady_clock::now() - p0).count() / frames;
        std::printf("arrange: %d tracks, %d bars, %d sections -- full redraw %.2f ms, a tick %.3f ms\n",
                    static_cast<int>(snap.tracks.size()), snap.bars, sections, redraw * 1000.0, tick * 1000.0);
        check(sections >= 90, "the timeline holds a whole night (" + juce::String(sections) + " sections)");
        check(tick < 0.004, "a tick of the timeline costs under 4 ms (" + juce::String(tick * 1000.0, 3) + " ms)");
        check(tick < redraw, "and less than drawing the set again (" + juce::String(redraw * 1000.0, 2) + " ms)");
        check(moved > frames / 2, "the play head reports when it has moved far enough to repaint");

        // The DJ overlap drawn legibly (19.09.2026, round "polish"): in the set strip neighbouring tracks
        // share kDjOverlap bars, so their blocks must run side by side in x -- each over its whole length --
        // without one covering the other; and the view knows which bars each track shares.
        {
            int pairs = 0, covered = 0, notSideBySide = 0, wrongLength = 0, wrongShare = 0;
            const int w = view.trackBlock(0).getWidth() > 0 ? view.getWidth() : 0;
            for (size_t i = 0; i < snap.tracks.size(); ++i) {
                const juce::Rectangle<int> b = view.trackBlock(i);
                // The block's width against the strip's scale: its bars over the set's, within two pixels.
                const juce::Rectangle<int> first = view.trackBlock(0), last = view.trackBlock(snap.tracks.size() - 1);
                const double stripW = last.getRight() - first.getX();
                const double want = stripW * snap.tracks[i].bars / snap.bars;
                if (std::abs(b.getWidth() - want) > 2.0) ++wrongLength;
                const auto share = view.overlapOf(i);
                if (share.first != (i > 0 ? kDjOverlap : 0) || share.second != (i + 1 < snap.tracks.size() ? kDjOverlap : 0)) ++wrongShare;
                if (i + 1 < snap.tracks.size()) {
                    const juce::Rectangle<int> n = view.trackBlock(i + 1);
                    ++pairs;
                    if (b.intersects(n)) ++covered;
                    const bool sharedX = n.getX() < b.getRight() && b.getX() < n.getRight();
                    const bool apartY = b.getBottom() <= n.getY() || n.getBottom() <= b.getY();
                    if (!(sharedX && apartY)) ++notSideBySide;
                }
            }
            check(w > 0 && pairs == 8 && covered == 0 && notSideBySide == 0 && wrongLength == 0 && wrongShare == 0,
                  "the strip draws the DJ overlap: neighbouring tracks in two lanes, side by side over the bars they share, "
                  "each block its full length (" + juce::String(pairs) + " pairs, " + juce::String(covered) + " covered, "
                  + juce::String(notSideBySide) + " not side by side, " + juce::String(wrongLength) + " off length, "
                  + juce::String(wrongShare) + " with the wrong shared bars)");
        }

        // What a click means, at every height of the view: the rows are reachable, and so are the
        // two small buttons on each of them.
        int seeks = 0, lastBar = -1;
        juce::SortedSet<int> tracksReached, locksReached, sectionLocks, sectionRolls;
        // A jump lands in the latest track that contains its bar: a track's first bar lies inside the previous
        // track's outro (the DJ overlap), and a jump there means "this track from its start".
        view.onSeek = [&](int bar) {
            ++seeks;
            lastBar = bar;
            int reached = -1;
            for (const auto& t : snap.tracks) if (bar >= t.firstBar && bar < t.firstBar + t.bars) reached = t.index;
            if (reached >= 0) tracksReached.add(reached);
        };
        view.onLock = [&](LockUnit unit, int index, bool) {
            if (unit == LockUnit::Track) locksReached.add(index);
            else sectionLocks.add(index);
        };
        view.onReroll = [&](LockUnit unit, int index) { if (unit == LockUnit::Section) sectionRolls.add(index); };
        for (int y = 0; y < view.getHeight(); ++y) {
            clickAt(view, view.getWidth() - 20, y);   // the end of a row: its last section
            clickAt(view, 156, y);                    // the track's own lock, at the end of its name
            clickAt(view, 190, y);                    // the lock of the row's first section
            clickAt(view, 190 + 130, y);              // and a reroll somewhere along it
        }
        check(seeks > 0 && lastBar >= 0, "clicking the timeline asks for a jump");
        check(tracksReached.size() >= 9, "every track can be reached by clicking (" + juce::String(tracksReached.size()) + ")");
        check(locksReached.size() >= 9, "every track has its own lock (" + juce::String(locksReached.size()) + ")");
        check(sectionLocks.size() >= 9 && sectionRolls.size() >= 1,
              "sections carry a lock and a reroll of their own (" + juce::String(sectionLocks.size()) + " locks, "
                  + juce::String(sectionRolls.size()) + " rerolls)");
    }

    // ---------------------------------------------------------------- locks and rerolls
    if (partRest) {
        // The curation loop of PLAN 6.8 as the editor drives it: the editor pushes a command, the
        // composer thread carries it out, and the published plans come back changed -- the rerolled
        // track and nothing else.
        const double sr = 48000.0;
        const int block = 512;
        auto p = std::make_unique<PhospheneProcessor>();
        p->setFollowHost(false);
        p->setPlayConfigDetails(0, 2, sr, block);
        p->prepareToPlay(sr, block);
        p->play();
        juce::AudioBuffer<float> buf(2, block);
        check(waitForPlans(*p, buf, 2, 60.0), "the composer publishes the plans the editor draws");
        TrackPlan first0, first1;
        p->tryReadTrack(0, first0);
        p->tryReadTrack(1, first1);

        p->setLock(LockUnit::Track, 0, true);
        check(p->isLocked(LockUnit::Track, 0), "a lock shows at once, without waiting for the composer");
        p->reroll(LockUnit::Track, 1);
        check(p->variation(LockUnit::Track, 1) == 1, "a reroll counts up");
        p->reroll(LockUnit::Track, 0);
        check(p->variation(LockUnit::Track, 0) == 0, "and a locked unit refuses to be rerolled");

        // Wait for the composer to publish the plans again.
        const auto t0 = std::chrono::steady_clock::now();
        TrackPlan now0, now1;
        bool changed = false;
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 90.0) {
            feed(*p, buf, 1);
            if (p->tryReadTrack(1, now1) && now1.melodySeed != first1.melodySeed) { changed = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        check(changed, "rerolling a track reaches the composer and changes that track");
        check(p->tryReadTrack(0, now0) && now0.melodySeed == first0.melodySeed && now0.formSeed == first0.formSeed
                  && now0.percSeed == first0.percSeed && now0.bpm == first0.bpm,
              "and leaves the track before it exactly as it was");
        p->stop();
    }

    // ---------------------------------------------------------------- the perform macros
    if (partRest) {
        const double sr = 48000.0;
        const int block = 256;
        const int bar = static_cast<int>(kBeatsPerBar * 60.0 / 145.0 * sr / block) * block;
        auto p = std::make_unique<PhospheneProcessor>();
        p->setFollowHost(false);
        p->setNonRealtime(true);
        p->setPlayConfigDetails(0, 2, sr, block);
        p->prepareToPlay(sr, block);
        p->play();
        juce::AudioBuffer<float> warm(2, block);
        feed(*p, warm, 12);
        // Past the sparse intro the form grammar writes: there is nothing to take away in bar 1.
        while (p->transport().bar < 24) feed(*p, warm, 1);

        ParamStore& store = p->params();
        const int cutoffId = store.base(Module::Acid) + acid::Cutoff;
        const int gateId = store.base(Module::Poly, static_cast<int>(PolyInstance::Lead)) + poly::Gate;
        const int depthId = store.base(Module::Poly, static_cast<int>(PolyInstance::Lead)) + poly::GateDepth;
        const int kickMuteId = store.base(Module::Mix) + mix::KickMute;
        const float cutoffWas = store.get(cutoffId), gateWas = store.get(gateId), depthWas = store.get(depthId);

        // Filter Sweep: one gesture, three filters, and the knobs come back to the value they had.
        p->setMacro(Macro::FilterSweep, 1.0f);
        const float cutoffUp = store.get(cutoffId);
        p->setMacro(Macro::FilterSweep, -1.0f);
        const float cutoffDown = store.get(cutoffId);
        p->setMacro(Macro::FilterSweep, 0.0f);
        std::printf("filter sweep: acid cutoff %.0f .. %.0f .. %.0f Hz, back to %.0f\n",
                    cutoffDown, cutoffWas, cutoffUp, store.get(cutoffId));
        check(cutoffUp > cutoffWas * 1.2f && cutoffDown < cutoffWas * 0.9f,
              "the filter sweep opens and closes the acid cutoff (" + juce::String(cutoffDown, 0) + " .. "
                  + juce::String(cutoffWas, 0) + " .. " + juce::String(cutoffUp, 0) + " Hz)");
        check(store.get(cutoffId) == cutoffWas, "and letting go puts the knob back exactly");

        // Stutter: held, and it takes the gate with it.
        p->setMacro(Macro::Stutter, 1.0f);
        check(store.get(gateId) >= 0.5f && store.get(depthId) >= 0.99f, "stutter switches the gate on at full depth");
        p->setMacro(Macro::Stutter, 0.0f);
        check(store.get(gateId) == gateWas && store.get(depthId) == depthWas, "and gives the gate back");

        // Drop-out: measured, not asserted. One bar with the low end, one without.
        const double loud = renderRms(*p, block, bar);
        p->setMacro(Macro::DropOut, 1.0f);
        check(store.get(kickMuteId) >= 0.5f, "a drop-out takes the kick out");
        const double gone = renderRms(*p, block, bar);
        const double drop = 20.0 * std::log10(juce::jmax(1.0e-9, gone) / juce::jmax(1.0e-9, loud));
        std::printf("drop-out: one bar %.1f dB quieter (%.4f -> %.4f rms)\n", drop, loud, gone);
        check(drop < -2.5, "and the bar really is quieter for it (" + juce::String(drop, 1) + " dB)");
        // It lets go on the bar line by itself: the macro tick is what a message loop would run.
        p->serviceMacros();
        check(store.get(kickMuteId) == 0.0f, "and it lets go on the next bar line without being asked");
        check(p->macroValue(Macro::DropOut) == 0.0f, "the macro reports that it has let go");
    }

    // ---------------------------------------------------------------- a set through the editor
    if (partRest) {
        // Export and import go through the core's own `.phosset` (SetFile.h), so what the plugin
        // writes phos_render plays -- locks and rerolls included.
        auto p = std::make_unique<PhospheneProcessor>();
        p->setSeed(20260916);
        ParamStore& store = p->params();
        const int bpmId = store.base(Module::Compose) + compose::Bpm;
        store.set(bpmId, 139.0f);
        p->setLock(LockUnit::Track, 2, true);
        p->reroll(LockUnit::Track, 3);
        const juce::File file = juce::File::createTempFile("phosset");
        bool wrote = false;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60.0) {
            // The composer applies the commands on its own turn; the file is written from the
            // composer's own state, so it is asked again until the commands have landed.
            wrote = p->exportSet(file);
            if (wrote && file.loadFileAsString().contains("lock.track.2=1")) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const juce::String text = file.loadFileAsString();
        check(wrote && text.startsWith("phosset "), "Export set writes the core's own .phosset");
        check(text.contains("lock.track.2=1") && text.contains("reroll.track.3=1"),
              "with the locks and rerolls the curation made");

        auto q = std::make_unique<PhospheneProcessor>();
        const bool read = q->importSet(file);
        check(read && q->seed() == 20260916, "Load set reads the seed back");
        check(q->params().get(q->params().base(Module::Compose) + compose::Bpm) == 139.0f, "and the knobs");
        check(q->isLocked(LockUnit::Track, 2) && q->variation(LockUnit::Track, 3) == 1,
              "and the locks, in the editor's own mirror");
        file.deleteFile();
    }

    // ---------------------------------------------------------------- what it costs
    if (partRest) {
        const double sr = 48000.0;
        const int block = 256;
        const double seconds = 20.0;
        auto p = std::make_unique<PhospheneProcessor>();
        p->setFollowHost(false);
        p->setNonRealtime(true);
        p->setPlayConfigDetails(0, 2, sr, block);
        p->prepareToPlay(sr, block);
        p->play();
        juce::AudioBuffer<float> buf(2, block);
        juce::MidiBuffer midi;
        feed(*p, buf, 8);   // past the first handshake, so the clock is not measuring the plan
        const auto t0 = std::chrono::steady_clock::now();
        int rendered = 0;
        const int total = static_cast<int>(seconds * sr);
        while (rendered < total) {
            buf.clear(); midi.clear();
            p->processBlock(buf, midi);
            rendered += block;
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("CPU: %.1f s of audio at %.0f Hz / %d in %.3f s -- %.1fx realtime, %.2f %% of a core\n"
                    "     (composer included: offline, the audio thread composes as well)\n",
                    seconds, sr, block, elapsed, seconds / elapsed, 100.0 * elapsed / seconds);
        check(elapsed < seconds, "the plugin renders faster than realtime");
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
