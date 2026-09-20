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

void check(bool ok, const juce::String& what)
{
    ++checks;
    if (!ok) { std::printf("FAIL: %s\n", what.toRawUTF8()); ++failures; }
}

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

} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    // The child half of the learned-model section below; it prints one line and checks nothing.
    if (argc > 1 && juce::String(argv[1]) == "--probe-models") return probeModels();
    std::printf("Phosphene host test\n");

    // ---------------------------------------------------------------- the shipped wavetable pack
    //
    // A development build finds `library.phoswt` beside the sources through PHOS_SOURCE_DATA_DIR,
    // which is exactly why the shipped case has to be tested on purpose: what a user installs has
    // no source tree. The pack is copied next to this executable by Tests/CMakeLists.txt, the way
    // it is copied next to the plugin's artefacts, so what is measured here is the plugin's own
    // path resolution and nothing else. Both sections run before any other processor is built --
    // the library is loaded once per process, by the first Engine::prepare() that happens.
    {
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
              "and it has every table this build ships (" + juce::String(lib.loaded) + " of "
                  + juce::String(lib.shipped) + ")");
    }

    // The other half: no pack at all. The library is process-global and is loaded once, so the
    // absence is staged by asking for a name that cannot be found -- after that every load in this
    // process reports nothing, which is precisely the state of a plugin whose resources were not
    // installed. It must still make sound, and it must not pretend in the editor.
    {
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
    // nowhere. Until this round nothing in the plugin called setModelSearchPath() at all, so that was
    // the state of every installed copy.
    //
    // Both files are copied next to this executable by Tests/CMakeLists.txt, exactly as
    // Plugin/CMakeLists.txt copies them next to the artefacts, so what is measured is the plugin's
    // own path resolution.
    {
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
    {
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
            if (!child.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
                check(false, "the no-data child would not start");
            else out = child.readAllProcessOutput();
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
    for (double sr : { 44100.0, 48000.0, 96000.0 }) {
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
    {
        auto p = std::make_unique<PhospheneProcessor>();
        p->setPlayConfigDetails(0, 2, 48000.0, 256);
        p->prepareToPlay(48000.0, 256);
        check(p->getLatencySamples() == p->engine().latencySamples(), "the reported latency is the engine's");
        check(p->getLatencySamples() > 0, "the limiter's lookahead is reported as latency");
    }

    // ---------------------------------------------------------------- the oracle
    {
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
    {
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
            int i = 0;
            while (!stopThread.load(std::memory_order_relaxed)) {
                const int id = (i * 37) % store.count();
                if (auto* param = p->parameterFor(id)) param->setValueNotifyingHost((i % 11) / 10.0f);
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
    {
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
    {
        // Everything above runs offline, where the audio thread composes for itself. This is the
        // arrangement the plugin actually ships with: blocks arriving at the rate a sound card asks
        // for them, and a thread of its own filling the rings a few bars ahead. If that thread ever
        // falls behind, the engine runs out of events and the output goes quiet -- so the check is
        // that it does not, over three seconds, at a block size that leaves 5.3 ms per block.
        const double sr = 48000.0;
        const int block = 256;
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
        check(planSeconds < 30.0, "a set starts playing within half a minute of the button");
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
        check(worstRun * block / sr < 0.15, "no gap longer than 150 ms while it plays (worst "
                                                + juce::String(worstRun * block / sr * 1000.0, 1) + " ms)");
        // What the editor's pattern rolls draw: the bars the conductor has composed, by part.
        std::vector<NoteEvent> pattern;
        const bool got = p->readPattern(16, 4, pattern);
        int kicks = 0, percs = 0;
        for (const NoteEvent& e : pattern) {
            kicks += e.part == Part::Kick ? 1 : 0;
            percs += e.part == Part::Perc ? 1 : 0;
        }
        check(got && !pattern.empty(), "the pattern preview has bars 16 to 20 in it ("
                                           + juce::String(static_cast<int>(pattern.size())) + " notes)");
        check(kicks >= 12, "and the kick is in it (" + juce::String(kicks) + " notes in four bars)");
        check(percs > 0, "and the percussion kit too (" + juce::String(percs) + " notes)");
    }

    // ---------------------------------------------------------------- state round trip
    {
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
              "a state from before this round sets no knob and is held for the offer instead (version "
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
    {
        auto p = std::make_unique<PhospheneProcessor>();
        const ParamStore& store = p->params();
        check(p->getParameters().size() == store.count(),
              "every parameter of the store is a host parameter (" + juce::String(store.count()) + ")");
        bool textRoundTrip = true, namesUnique = true, rangesSane = true;
        juce::StringArray ids;
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

    // ---------------------------------------------------------------- bus layouts and the editor
    {
        auto p = std::make_unique<PhospheneProcessor>();
        check(p->producesMidi() && !p->acceptsMidi(), "the plugin writes MIDI and reads none");
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
            }
        }
        editor.reset();
        check(true, "the editor closes");
    }

    // ---------------------------------------------------------------- the arrange timeline
    {
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
    {
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
    {
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
    {
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
    {
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
