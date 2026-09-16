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
 * This is not a replacement for pluginval, which exercises the VST3 wrapper itself; it is the part
 * that lives in the repository and runs on every build.
 */
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "phos/Composer.h"
#include "phos/Engine.h"
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

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf("Phosphene host test\n");

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
        p->seekToBar(8);   // past the sparse intro (Phase 5): the groove is what has to keep sounding
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
        const bool got = p->readPattern(8, 4, pattern);
        int kicks = 0, percs = 0;
        for (const NoteEvent& e : pattern) {
            kicks += e.part == Part::Kick ? 1 : 0;
            percs += e.part == Part::Perc ? 1 : 0;
        }
        check(got && !pattern.empty(), "the pattern preview has bars 8 to 12 in it ("
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
