/**
 * @file selftest_lowend.cpp
 * @brief The self test's low end and models: acid voicings, the phase lock, the bass rhythm, percussion, rhythm, the constrained sampler and the learned models.
 */
#include "SelfTestHelpers.h"

using namespace phos;

namespace phostest {

/**
 * @brief testAcidVoicing part `.corners` (a). The section was split on 19.09.2026 (round "test-split")
 *        into its three independent blocks: `.corners` (a), `.night` (b), `.engine` (c).
 */
void testAcidVoicingCorners()
{
    section("acid voicings per track: the corners, and track 1 is the knobs");
    ParamStore p;
    auto valueAt = [&](int param, const float* off) { return acidValueAt(p, param, off); };
    // (a) Corners reach the voicings exactly at the default Sound Variation; the first track is the knobs.
    {
        float off[acid::Count];
        int disp = 0;
        const float clean[3] = { 1.0f, 0.0f, 0.0f }, liquid[3] = { 0.0f, 0.0f, 1.0f }, driven[3] = { 0.0f, 1.0f, 0.0f };
        Composer::acidVoicingOffsets(p, clean, 0.5f, off, disp);
        const bool cleanOk = std::fabs(valueAt(acid::Drive, off) - 0.1f) < 1e-3f && std::fabs(valueAt(acid::Accent, off) - 0.8f) < 1e-3f
                          && std::fabs(valueAt(acid::LowCut, off) - 150.0f) < 0.5f && disp == -1;
        Composer::acidVoicingOffsets(p, liquid, 0.5f, off, disp);
        const bool liquidOk = std::fabs(valueAt(acid::Wave, off) - 0.5f) < 1e-3f && std::fabs(valueAt(acid::Cutoff, off) - 450.0f) < 0.5f && disp == 4;
        Composer::acidVoicingOffsets(p, driven, 0.5f, off, disp);
        float any = 0.0f;
        for (float o : off) any += std::fabs(o);
        const bool drivenOk = any == 0.0f && disp == -1;
        Composer::acidVoicingOffsets(p, liquid, 0.0f, off, disp);
        float anyZero = 0.0f;
        for (float o : off) anyZero += std::fabs(o);
        // The listening seed's plans at the default knobs (`p` is default too): planned once in one process.
        ListeningPlanner& lp = listeningPlanner();
        Composer& c = *lp.c;
        const TrackPlan& t0 = c.track(lp.p, 0);
        check(cleanOk && liquidOk && drivenOk && anyZero == 0.0f && disp == -1 && t0.acidVoicing[1] == 1.0f,
              "a track at a corner plays that voicing exactly, the driven corner and Sound Variation 0 play the knobs, track 1 is driven");
        // Not a check: what the listening seed's first tracks draw, for the listening notes.
        for (int i = 0; i < 4; ++i) {
            const TrackPlan t = c.track(lp.p, i);
            std::printf("         listening seed, track %d: acid clean/driven/liquid %.2f/%.2f/%.2f; bass", i + 1,
                        static_cast<double>(t.acidVoicing[0]), static_cast<double>(t.acidVoicing[1]), static_cast<double>(t.acidVoicing[2]));
            for (int m = 0; m < kNumBassMacros; ++m) std::printf(" %s %+.2f", kBassMacroNames[m], static_cast<double>(t.bassMacro[m]));
            std::printf("\n");
        }
    }
}

/** @brief testAcidVoicing part `.night` (b): over 39 tracks the voicings spread over the triangle. */
void testAcidVoicingNight()
{
    section("acid voicings per track: spread over a night");
    // (b) Over a night the tracks spread over the triangle, and consecutive tracks differ.
    {
        ParamStore q;
        q.parseText("compose.level_match=Off master.auto_gain=Off");
        Composer c(2026);
        int dominant[3] = {}, blends = 0;
        double closest = 1e9;
        for (int i = 1; i < 40; ++i) {
            const TrackPlan t = c.track(q, i);
            const float* w = t.acidVoicing;
            const int top = static_cast<int>(std::max_element(w, w + 3) - w);
            ++dominant[top];
            if (w[top] < 0.7f) ++blends;
            if (i > 1) {
                const float* o = c.track(q, i - 1).acidVoicing;
                double d = 0.0;
                for (int k = 0; k < 3; ++k) d += (w[k] - o[k]) * (w[k] - o[k]);
                closest = std::min(closest, std::sqrt(d));
            }
            const float sum = w[0] + w[1] + w[2];
            if (std::fabs(sum - 1.0f) > 1e-4f || *std::min_element(w, w + 3) < 0.0f) closest = -1.0;
        }
        check(dominant[0] >= 6 && dominant[1] >= 6 && dominant[2] >= 6 && blends >= 4 && closest > 0.25,
              "over 39 tracks every voicing leads several of them, some are blends, and no two neighbours share a sound",
              fmt("clean %d, driven %d, liquid %d leading; %d blends (no weight over 0.7); closest neighbours %.2f apart",
                  dominant[0], dominant[1], dominant[2], blends, closest));
    }
}

/** @brief testAcidVoicing part `.engine` (c): the engine plays track 2's voicing. */
void testAcidVoicingEngine()
{
    section("acid voicings per track: the engine plays them");
    ParamStore p;
    const int ab = p.base(Module::Acid), cb = p.base(Module::Compose);
    auto valueAt = [&](int param, const float* off) { return acidValueAt(p, param, off); };
    // (c) The engine plays it: in track 2 of a short-track set the acid's drive and wave are the voicing's.
    {
        auto e = std::make_unique<Engine>();
        e->prepare(48000.0, 256);
        e->params().parseText("compose.track_bars=32 compose.level_match=Off master.auto_gain=Off");
        Composer c(77);
        const TrackPlan t1 = c.track(e->params(), 1);
        float off[acid::Count];
        int disp = 0;
        Composer::acidVoicingOffsets(e->params(), t1.acidVoicing, e->params().get(cb + compose::SoundVariation), off, disp);
        const float wantDrive = valueAt(acid::Drive, off), wantWave = valueAt(acid::Wave, off);
        // From track 2's hand-over (19.09.2026, the DJ overlap): its acid, like its kick and bass, takes over there.
        const int from = handoverBar(t1);
        const TempoMap tm = c.tempoMap(e->params(), from + 8);
        e->setTempoMap(tm);
        Conductor cond(*e, c);
        std::vector<float> L(1024), R(1024);
        float gotDrive = -1.0f, gotWave = -1.0f, gotDisp = -1.0f;
        const uint64_t end = static_cast<uint64_t>(tm.secondsAt((from + 6.0) * kBeatsPerBar) * 48000.0);
        while (e->samplePosition() < end) {
            cond.pump(e->params(), 32.0);
            e->process(L.data(), R.data(), 1024);
            if (e->beatPosition() > (from + 4.0) * kBeatsPerBar && gotDrive < 0.0f) {
                gotDrive = e->effective(ab + acid::Drive);
                gotWave = e->effective(ab + acid::Wave);
                gotDisp = e->effective(ab + acid::Disperse);
            }
        }
        const float wantDisp = disp < 0 ? p.get(ab + acid::Disperse) : static_cast<float>(disp);
        check(std::fabs(gotDrive - wantDrive) < 1e-3f && std::fabs(gotWave - wantWave) < 1e-3f && gotDisp == wantDisp,
              "the engine plays the track's voicing (drive, wave, disperser in track 2)",
              fmt("weights %.2f/%.2f/%.2f: drive %.3f (want %.3f), wave %.3f (%.3f), disperse %.0f (%.0f)",
                  static_cast<double>(t1.acidVoicing[0]), static_cast<double>(t1.acidVoicing[1]), static_cast<double>(t1.acidVoicing[2]),
                  static_cast<double>(gotDrive), static_cast<double>(wantDrive), static_cast<double>(gotWave), static_cast<double>(wantWave),
                  static_cast<double>(gotDisp), static_cast<double>(wantDisp)));
    }
}

/**
 * @brief Kick and bass recipes spread audibly over a night, and the kick lock holds for every recipe.
 *
 * For 20 tracks of the listening seed, each track's recipe is applied to the knobs the way the engine
 * applies offsets (normalised domain), a kick and a stretch of rolling bass are rendered, and the spread
 * over the tracks is read: the kick's click band (2 .. 5 kHz against 40 .. 120 Hz over its first quarter
 * beat), the bass's bite band, and the bass's pluck (the top of the bite band, 700 Hz .. 3 kHz, in a
 * note's first period against its third). The bounds, as interquartile ranges over the tracks: the
 * kicks' click at least half the references' (-31 .. -23 dB, 8 dB); the bite band at least 3 dB --
 * three times the level JND; the pluck at least 6.5 dB. The recipe tables before the round that
 * introduced them, played on its sound, read 3.1, 5.3 and 6.0 dB there, and every floor here sits
 * above that: a fall back to what the tables gave still fails.
 *
 * 22.09.2026: the three readings moved -- 4.8/6.7/9.9 to 8.2/5.8/7.1 -- although this block measures
 * tracks 1..20 and nothing about the kick or the bass changed. The cause is one line further up: the
 * first track draws a recipe now (21.09.2026, at the user's decision), so track 1's "farthest from
 * the two before it" is scored against a real sound rather than against a row of zeros, and the whole
 * walk after it lands elsewhere. The pluck's floor was 8.0, fitted a decibel under the one draw that
 * had been measured; an interquartile range over twenty samples does not hold a decibel, so it is now
 * set by what the claim is actually about -- more spread than the fixed tables gave -- and not by the
 * last number that happened to come out.
 */
void testRecipeSpread()
{
    section("kick and bass recipes spread over a night");
    const double sr = 48000.0, slot = 0.25 * 60.0 / 145.0;
    ParamStore p;
    p.parseText("compose.level_match=Off master.auto_gain=Off");
    const int cb = p.base(Module::Compose);
    const float sv = p.get(cb + compose::SoundVariation);
    Composer c(864566672ull);
    std::vector<double> clicks, bites, plucks;
    double worstLock = 0.0;
    for (int i = 1; i <= 20; ++i) {
        const TrackPlan t = c.track(p, i);
        auto apply = [&](Module m, const float* off) {
            std::vector<float> v = moduleValues(p, m);
            const int base = p.base(m);
            for (size_t k = 0; k < v.size(); ++k)
                if (off[k] != 0.0f) v[k] = p.fromNormalised(base + static_cast<int>(k), p.toNormalised(base + static_cast<int>(k), v[k]) + off[k]);
            return v;
        };
        float ko[64] = {}, bo[64] = {};
        Composer::recipeOffsets(true, t.kickMacro, sv, ko);
        Composer::recipeOffsets(false, t.bassMacro, sv, bo);
        std::vector<float> kv = apply(Module::Kick, ko), bv = apply(Module::Bass, bo);
        if (t.kickEngine >= 0) kv[kick::Engine] = static_cast<float>(t.kickEngine);
        if (t.kickClip >= 0) kv[kick::Clip] = static_cast<float>(t.kickClip);
        Kick::constrain(kv.data(), slot, t.key);
        Kick k;
        k.prepare(sr);
        k.update(kv.data(), t.key);
        // The lock: the kick follows the bass's knob phase at the slot, whatever the recipe.
        k.setPhaseTarget(slot, 0.0);
        const double ph = k.outputPhaseAt(slot);
        worstLock = std::max(worstLock, std::fabs(ph - std::round(ph)));
        k.trigger(1.0f);
        std::vector<float> ky(static_cast<size_t>(slot * sr));
        k.process(ky.data(), static_cast<int>(ky.size()));
        clicks.push_back(powDb(lowendBandPower(ky, 0, ky.size(), 2000.0, 5000.0, sr) / lowendBandPower(ky, 0, ky.size(), 40.0, 120.0, sr)));
        bv[bass::DuckDepth] = 0.0f;
        const std::vector<float> by = lowendRollingBass(bv, 24, bassRootNote(t.key, 0));
        bites.push_back(powDb(lowendBandPower(by, by.size() / 4, by.size(), 300.0, 2000.0, sr) / lowendBandPower(by, by.size() / 4, by.size(), 20.0, 120.0, sr)));
        // The pluck: 700 Hz .. 3 kHz in a note's first period against its third (as in testBassBite).
        {
            const double bs = 60.0 / 145.0 * sr, sl = bs / 4.0;
            const size_t per = static_cast<size_t>(std::lround(sr / midiToHz(bassRootNote(t.key, 0))));
            double e = 0.0, l = 0.0;
            for (int kk = 6; kk < 22; ++kk)
                for (int s = 1; s <= 3; ++s) {
                    const size_t a = static_cast<size_t>(kk * bs + s * sl);
                    e += lowendBandPower(by, a, a + per, 700.0, 3000.0, sr);
                    l += lowendBandPower(by, a + 2 * per, a + 3 * per, 700.0, 3000.0, sr);
                }
            plucks.push_back(powDb(e / l));
        }
    }
    auto iqr = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() * 3 / 4] - v[v.size() / 4]; };
    const double kickIqr = iqr(clicks), bassIqr = iqr(bites), pluckIqr = iqr(plucks);
    check(kickIqr >= 4.0 && bassIqr >= 3.0 && pluckIqr >= 6.5, "twenty tracks' kicks and basses differ audibly: click, bite band and pluck spread over their quartiles",
          fmt("kick click interquartile range %.1f dB (references 8.0, fixed tables 3.1), bass bite band %.1f dB (tables 5.3), pluck %.1f dB (tables 6.0)", kickIqr, bassIqr, pluckIqr));
    check(worstLock < 1e-3, "every recipe's kick still meets the bass phase at the first slot (the lock finds a whole cycle)",
          fmt("worst %.2g cycles off", worstLock));
}

/**
 * @brief The engine tells the kit its tempo, and the auto-pan's period follows (18.09.2026).
 *
 * The swing's period is perc.pan_bars bars, 0.1875 by default: three sixteenths, 0.375 s at 120 BPM
 * and 0.310 s at the 145 BPM the kit assumed while nobody called PercKit::setTempo. The period is
 * read off the lane's pan angle, sampled every 16 samples while the engine runs, as the mean distance
 * between upward crossings of its own mean.
 */
void testPercTempo()
{
    section("the kit's auto-pan runs at the engine's tempo");
    const double sr = 48000.0;
    auto e = std::make_unique<Engine>();
    e->prepare(sr, 512);
    e->params().parseText("compose.bpm=120");
    std::vector<float> L(16), R(16), a;
    for (int i = 0; i < static_cast<int>(2.0 * sr / 16); ++i) {
        e->process(L.data(), R.data(), 16);
        a.push_back(static_cast<float>(e->percKit().panAngle(0)));
    }
    double mean = 0.0;
    for (float v : a) mean += v;
    mean /= static_cast<double>(a.size());
    std::vector<double> ups;
    for (size_t i = 1; i < a.size(); ++i)
        if (a[i - 1] < mean && a[i] >= mean) ups.push_back((static_cast<double>(i) - (a[i] - mean) / (a[i] - a[i - 1])) * 16.0 / sr);
    const double period = ups.size() >= 2 ? (ups.back() - ups.front()) / static_cast<double>(ups.size() - 1) : 0.0;
    const double expect = 0.1875 * 4.0 * 60.0 / 120.0;
    check(std::fabs(e->percKit().tempo() - 120.0) < 1e-6 && std::fabs(period - expect) < 0.01 * expect,
          "at 120 BPM the kit's auto-pan swings with three sixteenths of 120 BPM, not of 145",
          fmt("kit tempo %.2f BPM; swing period %.4f s over %d cycles (expected %.4f s, at 145 BPM it would be %.4f s)",
              e->percKit().tempo(), period, static_cast<int>(ups.size()) - 1, expect, 0.1875 * 4.0 * 60.0 / 145.0));
}

/**
 * @brief Kick and bass phase at the first sixteenth, part `.lock`: the three coupling modes at four tempi.
 *
 * Split on 19.09.2026 (round "test-split") along its two checks, not by mode: the first check is one
 * conjunction over "Kick follows bass", "Bass follows kick" and "Off", and "Off" is only meaningful
 * over all four tempi (it asserts that the phases *wander* with the tempo), so this part keeps all
 * three modes and the other part (`.onsets`) takes the sub-only measurement of the second check.
 * Every measureLock call is independent (its own engine and composer), so the two parts measure
 * exactly what the one section measured.
 */
/**
 * @brief testPhaseLock, part `.lock`: renders the twelve measureLock() calls (four tempi x three
 *        coupling modes) in parallel (round "test-speed-rest", 20.09.2026).
 *
 * Each call was already independent of every other (measureLock's own comment: "its own engine and
 * composer"), so the loop below just hands the twelve closures to phos::probe::runAll instead of
 * calling them one after another. That reuses the "speed" round's pool rather than a new one: runAll
 * is already a generic "run these closures on up to threads() workers, 16 MB stack, the caller works
 * too" pool with nothing probe-specific in its contract, and Composer::measureTrack (Composer.cpp)
 * already dispatches whole audio renders through it the same way (its probeLoudness tasks render full
 * Engine instances, exactly what measureLock does here) -- this is that facility's existing, tested
 * contract, not new machinery bolted on. warmSharedData() is a scheduling choice only since round
 * "threadsafe-loaders" (both loaders now gate their own first load with a mutex), kept so the first
 * stage's workers do not queue behind it instead of rendering.
 *
 * Bit for bit against the serial version: what each task computes does not depend on which thread runs
 * it (own Engine, own Composer, nothing shared but the now-thread-safe immutable library/voice pack/
 * sine table), and the fold below (worstKick/worstBass/offLo/offHi, the detail string) runs on the
 * calling thread, after every task has finished, in the same bpm order the serial loop used -- so the
 * accumulation is not just numerically equivalent, it is the same sequence of floating-point operations.
 */
void testPhaseLockLock()
{
    section("kick and bass phase at the first sixteenth: the lock at every tempo");
    static const double kBpms[4] = { 138.0, 142.0, 145.0, 148.0 };
    double dk[4] = {}, db[4] = {}, off[4] = {};
    phos::probe::warmSharedData();
    std::vector<std::function<void()>> tasks;
    for (int i = 0; i < 4; ++i) {
        tasks.push_back([&, i] { double spread = 0.0, coh = 0.0; dk[i] = measureLock("bass.kick_lock=Kick follows bass", kBpms[i], spread, coh); });
        tasks.push_back([&, i] { double spread = 0.0, coh = 0.0; db[i] = measureLock("bass.kick_lock=Bass follows kick", kBpms[i], spread, coh); });
        tasks.push_back([&, i] { double spread = 0.0, coh = 0.0; off[i] = measureLock("bass.kick_lock=Off", kBpms[i], spread, coh); });
    }
    phos::probe::runAll(tasks);
    std::string detail;
    double worstKick = 0.0, worstBass = 0.0, offLo = 1e9, offHi = -1e9;
    for (int i = 0; i < 4; ++i) {
        worstKick = std::max(worstKick, std::fabs(dk[i]));
        worstBass = std::max(worstBass, std::fabs(db[i]));
        offLo = std::min(offLo, off[i]);
        offHi = std::max(offHi, off[i]);
        detail += fmt("%.0f BPM: %+.1f / %+.1f / off %+.1f   ", kBpms[i], dk[i], db[i], off[i]);
    }
    check(worstKick < 6.0 && worstBass < 6.0 && offHi - offLo > 60.0,
          "kick lock aligns the phases at every tempo (without it they wander with the tempo)", detail);
}

/** @brief Kick and bass phase at the first sixteenth, part `.onsets`: the bass onset spread on the sub alone. */
void testPhaseLockOnsets()
{
    section("kick and bass phase at the first sixteenth: sub-sample bass onsets");
    double worstSpread = 0.0;
    for (double bpm : { 138.0, 142.0, 145.0, 148.0 }) {
        double spread = 0.0, coh = 0.0;
        // The onset spread on the sub alone (19.09.2026): what it checks is that onsets are sub-sample
        // exact, and since the bite and the sub octave the harmonics of a note depend on its velocity,
        // which the pattern varies from beat to beat -- a two-period fit then reads 0.1 degree of
        // "spread" that is timbre, not timing (with every harmonic path closed it reads what it did).
        measureLock("bass.kick_lock=Kick follows bass bass.bite=0 bass.sub_octave=0 bass.cutoff=20 bass.env_amount=0 bass.key_track=0", bpm, spread, coh);
        worstSpread = std::max(worstSpread, spread);
    }
    check(worstSpread < 0.05, "bass onset phase constant from beat to beat (sub-sample onsets)", fmt("spread %.3f degrees", worstSpread));
}

// ---------------------------------------------------------------------------------------------
// The drawn bass rhythm (compose.bass_rhythm = Corpus)
// ---------------------------------------------------------------------------------------------

/** @brief Self test: compose.bass_rhythm: the drawn bass rhythm. */
void testBassRhythm()
{
    section("compose.bass_rhythm: the drawn bass rhythm");
    ParamStore def;
    const int cb = def.base(Module::Compose);
    check(def.getInt(cb + compose::BassRhythm) == 0,
          "the bass rhythm defaults to Pattern, so every earlier render is reproduced",
          fmt("default %s", kBassRhythmNames[def.getInt(cb + compose::BassRhythm)]));

    // 1. The tables. Sorted (both are searched by bisection), inside the clean subspace, and the
    //    counts add up to the number the header claims they were counted on.
    {
        bool sortedBars = true, cleanBars = true, sortedSteps = true, keysInRange = true;
        unsigned long long total = 0;
        for (int i = 0; i < kNumCorpusBassBars; ++i) {
            total += kCorpusBassBars[i].count;
            if (!BassRhythm::clean(kCorpusBassBars[i].mask)) cleanBars = false;
            if (i > 0 && kCorpusBassBars[i - 1].mask >= kCorpusBassBars[i].mask) sortedBars = false;
        }
        const int keyMax = 16 * 3 * 3 * 3 * 3 * 3;
        for (int i = 0; i < kNumCorpusBassSteps; ++i) {
            if (kCorpusBassSteps[i].key >= keyMax) keysInRange = false;
            if (i > 0 && kCorpusBassSteps[i - 1].key >= kCorpusBassSteps[i].key) sortedSteps = false;
        }
        const size_t bytes = sizeof(CorpusBassBar) * static_cast<size_t>(kNumCorpusBassBars)
                           + sizeof(CorpusBassStep) * static_cast<size_t>(kNumCorpusBassSteps);
        check(sortedBars && cleanBars && sortedSteps && keysInRange
              && total == kCorpusBassBarTotal && kNumCorpusBassBars > 0 && kNumCorpusBassSteps > 0,
              "the bass onset tables are sorted, clean and add up",
              fmt("%d bars / %d contexts, %llu counted, %zu bytes", kNumCorpusBassBars, kNumCorpusBassSteps, total, bytes));
        check(bytes < 8192, "the tables fit a Quest build without thinking about it", fmt("%zu bytes", bytes));
    }

    // 2. The context packing, against two keys worked out by hand from the rule in Corpus.h (base
    //    three per neighbour, most distant first, then the step in base sixteen).
    //    Step 10 of a bar with onsets on 2, 6, 7 and 9: the neighbours 8, 4, 3, 2 and 1 steps back are
    //    struck, struck, struck, silent, struck, so ((((1*3+1)*3+1)*3+0)*3+1) * 16 + 10 = 1898.
    //    Step 1 of an empty bar: every neighbour but step 0 is outside the bar, so
    //    ((((2*3+2)*3+2)*3+2)*3+0) * 16 + 1 = 3841.
    {
        const unsigned mask = (1u << 2) | (1u << 6) | (1u << 7) | (1u << 9);
        check(BassRhythm::contextKey(mask, 10) == 1898 && BassRhythm::contextKey(0u, 1) == 3841,
              "the chain context is packed exactly as Tools/corpus/bass_rhythm.py packs it",
              fmt("%d and %d", BassRhythm::contextKey(mask, 10), BassRhythm::contextKey(0u, 1)));
    }

    // 3. The mixture is a probability distribution: over the 4096 bars with no onset on a kick step --
    //    the whole subspace the model lives in -- the probabilities add up to one. That checks both
    //    halves at once: the lookup only if its counts are complete, the chain only if every step's
    //    two outcomes are complemented correctly.
    {
        double sum = 0.0, chain = 0.0;
        for (unsigned bits = 0; bits < 4096u; ++bits) {
            unsigned mask = 0;
            for (int s = 0, k = 0; s < 16; ++s) if (s % 4 != 0) { if ((bits >> k) & 1u) mask |= 1u << s; ++k; }
            sum += BassRhythm::barProbability(mask);
            chain += std::exp2(BassRhythm::chainLogProbability(mask));
        }
        check(std::fabs(sum - 1.0) < 1e-9 && std::fabs(chain - 1.0) < 1e-9,
              "the mixture and the chain are proper distributions over the 4096 clean bars",
              fmt("mixture %.12f, chain alone %.12f, lookup weight %.3f", sum, chain, BassRhythm::mixWeight()));
        check(BassRhythm::barProbability(0x1111u) == 0.0 && BassRhythm::barProbability(0xEEEFu) == 0.0,
              "a bar with an onset on a kick step has probability zero", "");
    }

    // 4. The families live in the same alphabet as the drawn bars, and a note ends at the next onset
    //    or at the next kick, whichever comes first.
    {
        const unsigned rolling = BassRhythm::familyMask(0), gallop = BassRhythm::familyMask(1);
        const unsigned skip = BassRhythm::familyMask(2), offbeat = BassRhythm::familyMask(3);
        const unsigned triplet = BassRhythm::familyMask(4);
        check(rolling == 0xEEEEu && gallop == 0xCCCCu && skip == 0xAAAAu && offbeat == 0x4444u && triplet == skip,
              "the pattern families as onset masks (the Triplet family has no sixteenth image and takes Skip's)",
              fmt("%04X %04X %04X %04X %04X", rolling, gallop, skip, offbeat, triplet));
        // Rolling: every note one sixteenth. Offbeat: two, because the note on step 6 runs to the kick
        // on step 8. A note on step 3 ends on step 4 even when step 5 is struck.
        check(BassRhythm::noteSpan(rolling, 1) == 1 && BassRhythm::noteSpan(rolling, 3) == 1
              && BassRhythm::noteSpan(offbeat, 6) == 2 && BassRhythm::noteSpan(0x0028u, 3) == 1
              && BassRhythm::shortestSpan(rolling) == 1 && BassRhythm::shortestSpan(offbeat) == 2,
              "a note ends at the next onset or at the next kick, whichever is first", "");
    }

    // 5. Ten thousand drawn bars: all clean, none empty, no gap tighter than a sixteenth -- the reason
    //    a drawn rhythm can only relax the gate limit and the release floor, never tighten them.
    {
        Rng r;
        r.seed(0xBA55ull);
        auto u = [&r]() { return static_cast<double>(r.uniform()); };
        int dirty = 0, empty = 0, tight = 0;
        std::set<unsigned> seen;
        for (int i = 0; i < 10000; ++i) {
            const unsigned m = BassRhythm::draw(u);
            if (!BassRhythm::clean(m)) ++dirty;
            if (m == 0) ++empty;
            if (BassRhythm::shortestSpan(m) < 1) ++tight;
            seen.insert(m);
        }
        check(dirty == 0 && empty == 0 && tight == 0 && seen.size() > 100,
              "every drawn bar is clean, non-empty and on the sixteenth grid",
              fmt("%zu distinct bars in 10000 draws, %d dirty, %d empty", seen.size(), dirty, empty));
    }

    const char* kBase = "compose.level_match=Off master.auto_gain=Off compose.track_bars=64";
    auto composeSet = [&](const char* extra, uint64_t seed, int bars, std::vector<NoteEvent>& notes,
                          std::vector<ControlEvent>* controls = nullptr) {
        ParamStore q;
        q.parseText(kBase);
        q.parseText(extra);
        Composer c(seed);
        c.composeBars(q, 0, bars, notes, controls);
    };

    // 6. The prior is kept exactly. With Bass Variation at zero the phrase never leaves its home bar
    //    and the home bar is the pattern family itself, so a Corpus render of a Rolling track is the
    //    Pattern render note for note -- onset, length, pitch and velocity.
    {
        const std::string rolling = "compose.bass_pattern=Rolling compose.track_variation=0 compose.bass_variation=0";
        std::vector<NoteEvent> pat, cor;
        composeSet(rolling.c_str(), 31337, 64, pat);
        composeSet((rolling + " compose.bass_rhythm=Corpus").c_str(), 31337, 64, cor);
        const std::vector<NoteEvent> pb = bassNotesOf(pat), cn = bassNotesOf(cor);
        bool same = pb.size() == cn.size() && !pb.empty();
        for (size_t i = 0; same && i < pb.size(); ++i)
            same = pb[i].beat == cn[i].beat && pb[i].length == cn[i].length
                   && pb[i].pitch == cn[i].pitch && pb[i].velocity == cn[i].velocity;
        check(same, "with no variation the drawn rhythm is the pattern family, note for note",
              fmt("%zu bass notes against %zu", cn.size(), pb.size()));
    }

    // 7. The knob off changes nothing at all and emits no event: a Pattern render is what it was before
    //    16.09.2026, which is what keeps every older render reproducible.
    {
        std::vector<NoteEvent> a, b;
        std::vector<ControlEvent> ca, cbv;
        composeSet("", 4242, 64, a, &ca);
        composeSet("compose.bass_rhythm=Pattern", 4242, 64, b, &cbv);
        bool same = a.size() == b.size() && ca.size() == cbv.size();
        for (size_t i = 0; same && i < a.size(); ++i)
            same = a[i].beat == b[i].beat && a[i].length == b[i].length && a[i].pitch == b[i].pitch
                   && a[i].part == b[i].part && a[i].velocity == b[i].velocity;
        int slotEvents = 0, clearing = 0;
        for (const ControlEvent& e : ca)
            if (e.kind == ControlEvent::Kind::BassSlot) { ++slotEvents; if (e.value <= 0.0f) ++clearing; }
        check(same && slotEvents == 64 && clearing == slotEvents,
              "with the knob on Pattern the score is unchanged and every slot event clears the slot",
              fmt("%zu notes, %zu controls, %d slot events, %d of them clearing", a.size(), ca.size(), slotEvents, clearing));
    }

    // 8. The event the round is about. Every beat of the stretch carries exactly one BassSlot, its
    //    value lies between a quarter and three quarters of a beat, and for a beat that plays it is the
    //    offset of that beat's first bass note -- recomputed here from the note list, which is the only
    //    place the engine's tail limit and phase lock may read it from.
    {
        int wrong = 0, outOfRange = 0, playing = 0, beats = 0, doubled = 0, sets = 0;
        std::set<int> values;
        for (uint64_t seed : { 909ull, 4ull, 64ull, 1024ull, 66000ull }) {
            std::vector<NoteEvent> notes;
            std::vector<ControlEvent> controls;
            composeSet("compose.bass_rhythm=Corpus compose.bass_variation=1", seed, 64, notes, &controls);
            std::map<int, double> firstNote;
            for (const NoteEvent& n : notes) {
                if (n.part != Part::Bass) continue;
                const int b = static_cast<int>(std::floor(n.beat + 1e-9));
                const double off = n.beat - b;
                auto it = firstNote.find(b);
                if (it == firstNote.end() || off < it->second) firstNote[b] = off;
            }
            std::map<int, int> perBeat;
            for (const ControlEvent& e : controls) {
                if (e.kind != ControlEvent::Kind::BassSlot) continue;
                const int b = static_cast<int>(std::lround(e.beat));
                ++perBeat[b];
                if (e.value < 0.25f || e.value > 0.75f) ++outOfRange;
                values.insert(static_cast<int>(std::lround(e.value * 4.0f)));
                auto it = firstNote.find(b);
                if (it == firstNote.end()) continue;
                ++playing;
                if (std::fabs(static_cast<double>(e.value) - it->second) > 1e-9) ++wrong;
            }
            beats += static_cast<int>(perBeat.size());
            for (const auto& kv : perBeat) if (kv.second != 1) ++doubled;
            ++sets;
        }
        check(beats == sets * 64 * kBeatsPerBar && doubled == 0 && wrong == 0 && outOfRange == 0
              && playing > 0 && values.size() >= 2,
              "one slot event per beat, inside the grid, and equal to that beat's first bass note",
              fmt("%d beats over %d sets, %d of them playing, %d wrong, %d out of range, %zu different slots",
                  beats, sets, playing, wrong, outOfRange, values.size()));
    }

    // 9. What the notes themselves may do: never on a kick, never ringing into the next one.
    {
        int onKick = 0, overrun = 0, total = 0;
        std::set<unsigned> bars;
        for (uint64_t seed : { 3ull, 33ull, 333ull, 3333ull }) {
            std::vector<NoteEvent> notes;
            composeSet("compose.bass_rhythm=Corpus compose.bass_variation=1", seed, 64, notes);
            for (const NoteEvent& n : notes) {
                if (n.part != Part::Bass) continue;
                ++total;
                const double off = n.beat - std::floor(n.beat + 1e-9);
                if (std::fabs(off) < 1e-9) ++onKick;
                if (n.beat + n.length > std::floor(n.beat + 1e-9) + 1.0 + 1e-9) ++overrun;
            }
            for (const auto& kv : barMasksOf(notes)) bars.insert(kv.second);
        }
        check(total > 0 && onKick == 0 && overrun == 0,
              "no drawn bass note starts on a kick or rings past the next one",
              fmt("%d notes, %d on a kick, %d overrunning, %zu distinct bar patterns", total, onKick, overrun, bars.size()));
    }

    // 10. Determinism, and a bar composed alone equal to that bar in sequence.
    {
        std::vector<NoteEvent> a, b;
        composeSet("compose.bass_rhythm=Corpus", 77, 32, a);
        composeSet("compose.bass_rhythm=Corpus", 77, 32, b);
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i)
            same = a[i].beat == b[i].beat && a[i].pitch == b[i].pitch && a[i].length == b[i].length;
        ParamStore q;
        q.parseText(kBase);
        q.parseText("compose.bass_rhythm=Corpus");
        Composer fresh(77);
        std::vector<NoteEvent> one;
        std::vector<ControlEvent> oneCtl;
        fresh.composeBars(q, 21, 1, one, &oneCtl);
        std::vector<NoteEvent> slice;
        for (const NoteEvent& x : a) if (x.beat >= 84.0 && x.beat < 88.0) slice.push_back(x);
        bool sliceSame = slice.size() == one.size();
        for (size_t i = 0; sliceSame && i < slice.size(); ++i)
            sliceSame = slice[i].beat == one[i].beat && slice[i].pitch == one[i].pitch;
        int slots = 0;
        for (const ControlEvent& e : oneCtl) if (e.kind == ControlEvent::Kind::BassSlot) ++slots;
        check(same && sliceSame && !one.empty() && slots == kBeatsPerBar,
              "the drawn rhythm is deterministic, and one bar alone carries its own four slot events",
              fmt("%zu notes twice, bar 21 alone %zu notes and %d slot events", a.size(), one.size(), slots));
    }

    // 11. Block-size independence with an event on every beat: the engine must render the same samples
    //     whether the blocks are 64 or 256 long.
    {
        const double sr = 48000.0;
        const char* kSet = "compose.bass_rhythm=Corpus compose.bass_variation=1 compose.level_match=Off "
                           "master.auto_gain=Off compose.track_bars=64";
        auto renderAt = [&](int block) {
            auto e = std::make_unique<Engine>();
            e->prepare(sr, block);
            e->params().parseText(kSet);
            Composer c(11);
            return renderEngine(*e, c, 64.0, block, sr);
        };
        const std::vector<float> a = renderAt(64), b = renderAt(256);
        size_t differ = 0;
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) ++differ;
        check(n > 0 && differ == 0, "the drawn rhythm renders bit-identically at any block size",
              fmt("%zu samples, %zu different", n, differ));
    }

    // 12. The lock itself, measured acoustically at the first onset of every beat while that onset
    //     moves from beat to beat. Without the event the engine would lock every beat to a quarter
    //     beat and the notes on the second and third sixteenth would be out by whole cycles.
    {
        std::string detail;
        double worst = 0.0;
        int slotsSeen = 0, measured = 0;
        for (double bpm : { 142.0, 148.0 }) {
            int onsets = 0, slots = 0, refOnsets = 0, refSlots = 0;
            const double free = measureFreeLock(bpm, "Corpus", onsets, slots);
            const double ref = measureFreeLock(bpm, "Pattern", refOnsets, refSlots);
            worst = std::max(worst, std::fabs(free) - std::fabs(ref));
            slotsSeen = std::max(slotsSeen, slots);
            measured += onsets;
            detail += fmt("%.0f BPM: %+.1f deg over %d onsets on %d sixteenths, families %+.1f deg on %d   ",
                          bpm, free, onsets, slots, ref, refSlots);
        }
        // The bound is what breaking the event costs, not what a perfect measurement would read: with
        // the slot frozen at a quarter beat a note on the second sixteenth is locked to the kick's
        // phase a quarter beat too early, which at 46 Hz and 142 BPM is 4.9 cycles away -- a wrap of
        // well over a hundred degrees. Ten degrees over the families' own reading separates the two
        // by an order of magnitude and still leaves room for the envelope bias the fit carries.
        check(measured > 20 && slotsSeen >= 2 && worst < 10.0,
              "the kick phase still meets the bass at the first onset of every beat, wherever it is", detail);
    }

    // 13. What was won, and that the roll still rolls. The corpus plays 641 distinct bar patterns at
    //     6.059 bit/bar and the pattern families reach a handful of them; a corpus line, though,
    //     repeats one bar in 86.8 % of its bars (Tools/corpus/bass_rhythm.py, section 2), so more
    //     patterns per set must not become more patterns per track.
    {
        auto survey = [&](const char* extra, double& modalShare) {
            std::set<unsigned> all;
            double shares = 0.0;
            int sets = 0;
            for (uint64_t seed : { 7ull, 17ull, 71ull, 107ull, 171ull, 701ull, 1007ull, 1701ull }) {
                std::vector<NoteEvent> notes;
                composeSet((std::string("compose.bass_variation=1 ") + extra).c_str(), seed, 64, notes);
                std::map<unsigned, int> hist;
                int n = 0;
                for (const auto& kv : barMasksOf(notes)) { all.insert(kv.second); ++hist[kv.second]; ++n; }
                int best = 0;
                for (const auto& kv : hist) best = std::max(best, kv.second);
                if (n > 0) { shares += static_cast<double>(best) / n; ++sets; }
            }
            modalShare = sets > 0 ? shares / sets : 0.0;
            return all.size();
        };
        double modalPat = 0.0, modalCor = 0.0;
        const size_t nPat = survey("", modalPat);
        const size_t nCor = survey("compose.bass_rhythm=Corpus", modalCor);
        check(nCor > 3 * nPat && modalCor > 0.55,
              "the drawn rhythm multiplies the patterns of a set without breaking up the track's figure",
              fmt("%zu distinct bars against %zu, the commonest bar of a set still %.0f %% of its bars "
                  "(pattern families %.0f %%)", nCor, nPat, 100.0 * modalCor, 100.0 * modalPat));
    }
}

/** @brief Self test: MIDI key changes. */
void testMidiKeys()
{
    section("MIDI key changes");
    Score s;
    s.keyRoot = 6;
    s.keyChanges.push_back(KeyChange{ 128.0, 1 });
    s.keyChanges.push_back(KeyChange{ 256.0, 8 });
    NoteEvent n;
    n.beat = 300.0;
    n.part = Part::Bass;
    s.notes.push_back(n);
    const std::vector<uint8_t> bytes = encodeMidi(s);
    MidiFileData d;
    decodeMidi(bytes.data(), bytes.size(), d);
    check(d.keys.size() == 3 && d.keys[1].first == 128.0 && d.keys[1].second == minorKeySharps(1) && d.keys[2].second == minorKeySharps(8),
          "key signature changes at track starts", fmt("%zu key signatures", d.keys.size()));
}
// ---------------------------------------------------------------------------------------------
// Percussion kit and rhythm
// ---------------------------------------------------------------------------------------------

/** @brief Self test: percussion kit. */
void testPercKit()
{
    section("percussion kit");
    const double sr = 48000.0;
    ParamStore p;

    // Every lane of the default kit, one hit each: level, and nothing below 140 Hz.
    double worstLow = -1e9;
    std::string detail;
    for (int l = 0; l < kPercLanes; ++l) {
        ParamStore q;
        for (int j = 0; j < kPercLanes; ++j) if (j != l) q.set(q.base(Module::Perc, j) + perc::Active, 0.0f);
        auto kit = makeKit(q);
        kit->trigger(l, 1.0f, 0, 0.0);
        const std::vector<float> y = renderKit(*kit, 32768);
        double pk = 0.0, e = 0.0;
        for (size_t i = 0; i < y.size(); ++i) { pk = std::max(pk, std::fabs(static_cast<double>(y[i]))); if (i < 4800) e += static_cast<double>(y[i]) * y[i]; }
        const double low = lowShareDb(y, 140.0);
        worstLow = std::max(worstLow, low);
        detail += fmt("%s %.1f/%.1f (low %.1f, <100 Hz %.1f)  ", kPercRoleNames[static_cast<int>(kit->role(l))], 20.0 * std::log10(pk + 1e-12), 10.0 * std::log10(e / 4800.0 + 1e-20), low, lowShareDb(y, 100.0));
    }
    std::printf("         hit peak/RMS(100 ms) dBFS per lane: %s\n", detail.c_str());
    check(worstLow < -30.0, "no lane puts more than -30 dB of its power below 140 Hz", fmt("worst %.1f dB", worstLow));

    // Choke: the open hat is silenced by the closed hat within 10 ms -- and without a shared choke
    // group it is not, or the measure would not tell a choke from the open hat's own decay.
    {
        auto chokeDrop = [&](bool grouped) {
            ParamStore q;
            for (int j = 0; j < kPercLanes; ++j) if (j != 1) q.set(q.base(Module::Perc, j) + perc::Level, -36.0f);
            q.set(q.base(Module::Perc, 0) + perc::Active, 0.0f);   // the closed hat itself silent, its choke still acts
            if (!grouped) q.set(q.base(Module::Perc, 0) + perc::Choke, 0.0f);
            auto kit = makeKit(q);
            kit->trigger(1, 1.0f, 0, 0.0);
            std::vector<float> y = renderKit(*kit, 2400);
            kit->trigger(0, 1.0f, 0, 0.0);
            const std::vector<float> z = renderKit(*kit, 4800);
            auto rms = [](const float* s, size_t n) { double e = 0.0; for (size_t i = 0; i < n; ++i) e += static_cast<double>(s[i]) * s[i]; return std::sqrt(e / n); };
            return 20.0 * std::log10(rms(z.data() + 480, 480) / rms(y.data() + 1920, 480) + 1e-12);
        };
        const double with = chokeDrop(true), without = chokeDrop(false);
        check(with < -40.0 && without > -10.0, "closed hat chokes the open hat by 40 dB within 10 ms (unchoked it keeps ringing)",
              fmt("%.1f dB choked, %.1f dB without the group", with, without));
    }

    // Clap: four bursts, nine milliseconds apart.
    {
        ParamStore q;
        for (int j = 0; j < kPercLanes; ++j) if (j != 4) q.set(q.base(Module::Perc, j) + perc::Active, 0.0f);
        q.parseText("perc5.filter=High Pass perc5.cutoff=150 perc5.low_cut=150");   // the bursts, not the band-pass's ring
        auto kit = makeKit(q);
        kit->trigger(4, 1.0f, 0, 0.0);
        const std::vector<float> y = renderKit(*kit, 2400);
        // Envelope in 0.5 ms steps; count rises above half the peak after a fall below it.
        std::vector<double> env;
        for (size_t i = 0; i + 24 <= y.size(); i += 24) {
            double e = 0.0;
            for (size_t j = i; j < i + 24; ++j) e = std::max(e, std::fabs(static_cast<double>(y[j])));
            env.push_back(e);
        }
        const double pk = *std::max_element(env.begin(), env.end());
        int bursts = 0;
        bool below = true;
        std::vector<size_t> starts;
        for (size_t i = 0; i < env.size() && i < 80; ++i) {
            if (below && env[i] > 0.5 * pk) { ++bursts; starts.push_back(i); below = false; }
            if (env[i] < 0.25 * pk) below = true;
        }
        const double spacing = starts.size() >= 2 ? (starts.back() - starts.front()) * 0.5 / (starts.size() - 1) : 0.0;
        check(bursts == 4 && std::fabs(spacing - 9.0) < 1.0, "clap: four bursts nine milliseconds apart", fmt("%d bursts, %.1f ms apart", bursts, spacing));
    }

    // Modal tom: the membrane's first two modes at 1 : 1.593 of the tuned pitch.
    {
        ParamStore q;
        for (int j = 0; j < kPercLanes; ++j) if (j != 8) q.set(q.base(Module::Perc, j) + perc::Active, 0.0f);
        q.parseText("perc9.noise=0 perc9.decay=1500 perc9.filter=Low Pass perc9.cutoff=18000");
        auto kit = makeKit(q);
        kit->trigger(8, 1.0f, 0, 0.0);
        const std::vector<float> y = renderKit(*kit, 1u << 16);
        const double f0 = kit->laneHz(8);
        const std::vector<double> pw = powerSpectrum(y.data(), 1u << 16);
        auto peakNear = [&](double hz) {
            const size_t c = static_cast<size_t>(hz * 65536.0 / sr);
            size_t best = c;
            for (size_t k = c - c / 20; k <= c + c / 20; ++k) if (pw[k] > pw[best]) best = k;
            return static_cast<double>(best) * sr / 65536.0;
        };
        const double m1 = peakNear(f0), m2 = peakNear(1.593 * f0);
        check(std::fabs(m1 / f0 - 1.0) < 0.01 && std::fabs(m2 / m1 - 1.593) < 0.01, "tom: membrane modes at 1 and 1.593",
              fmt("%.1f Hz and %.1f Hz (ratio %.3f), tuned to %.1f Hz", m1, m2, m2 / m1, f0));
        check(PercKit::tuneToScale(180.0, 6, 1) == midiToHz(54), "tune to key: 180 Hz goes to F#3 in F# Phrygian",
              fmt("%.2f Hz", PercKit::tuneToScale(180.0, 6, 1)));
    }

    // Tone lanes: the phasor holds its amplitude over seconds, and a sub-sample start shifts the phase.
    {
        ParamStore q;
        for (int j = 0; j < kPercLanes; ++j) if (j != 11) q.set(q.base(Module::Perc, j) + perc::Active, 0.0f);
        q.parseText("perc12.tune=0 perc12.pitch=1000 perc12.pitch_amount=1 perc12.decay=3000 perc12.filter=High Pass perc12.cutoff=150 perc12.pan=0 perc12.level=0");
        auto kit = makeKit(q);
        kit->trigger(11, 1.0f, 0, 0.0);
        const std::vector<float> y = renderKit(*kit, 96000 + 4800);
        auto amp = [&](size_t at) { double e = 0.0; for (size_t i = at; i < at + 4800; ++i) e += static_cast<double>(y[i]) * y[i]; return std::sqrt(2.0 * e / 4800.0); };
        const double drop = 20.0 * std::log10(amp(96000) / amp(4800));
        const double expect = -60.0 * (2.05 - 0.15) / 3.0;   // -60 dB per 3 s, window centres at 0.15 s and 2.05 s
        check(std::fabs(drop - expect) < 0.5, "tone lane: amplitude follows its decay over two seconds (phasor renormalised)",
              fmt("%.2f dB, expected %.2f dB", drop, expect));

        auto kit2 = makeKit(q), kit3 = makeKit(q);
        kit2->trigger(11, 1.0f, 0, 0.0);
        kit3->trigger(11, 1.0f, 0, 0.5);
        const std::vector<float> a = renderKit(*kit2, 9600), b = renderKit(*kit3, 9600);
        const double pa = phaseAgainst(a.data() + 4800, 480, [&](size_t i) { return 1000.0 * static_cast<double>(4800 + i) / sr; });
        const double pb = phaseAgainst(b.data() + 4800, 480, [&](size_t i) { return 1000.0 * static_cast<double>(4800 + i) / sr; });
        const double diff = wrapDeg(pb - pa), want = 360.0 * 1000.0 * 0.5 / sr;
        check(std::fabs(diff - want) < 0.2, "tone lane: half a sample late starts 3.75 degrees ahead", fmt("%.3f degrees", diff));
    }

    // Zap: the pitch falls.
    {
        ParamStore q;
        for (int j = 0; j < kPercLanes; ++j) if (j != 10) q.set(q.base(Module::Perc, j) + perc::Active, 0.0f);
        q.parseText("perc11.fm_index=0 perc11.decay=400 perc11.filter=High Pass perc11.cutoff=150");
        auto kit = makeKit(q);
        kit->trigger(10, 1.0f, 0, 0.0);
        const std::vector<float> y = renderKit(*kit, 9600);
        auto rate = [&](size_t a, size_t n) { int zc = 0; for (size_t i = a + 1; i < a + n; ++i) if (y[i - 1] < 0.0f && y[i] >= 0.0f) ++zc; return zc * sr / n; };
        const double early = rate(0, 480), late = rate(6000, 2400);
        check(early > 3.0 * late && late > 300.0, "zap: pitch sweeps down to its end pitch", fmt("%.0f Hz in the first 10 ms, %.0f Hz after 125 ms", early, late));
    }
}

/** @brief Self test: rhythm. */
void testRhythm()
{
    section("rhythm");
    // Euclidean necklaces against Toussaint's table (rotation does not matter).
    auto necklace = [](const std::vector<bool>& e) {
        std::string s;
        for (bool b : e) s += b ? 'x' : '.';
        std::string best = s;
        for (size_t r = 1; r < s.size(); ++r) best = std::min(best, s.substr(r) + s.substr(0, r));
        return best;
    };
    auto canon = [&](const char* t) { std::vector<bool> e; for (const char* c = t; *c; ++c) e.push_back(*c == 'x'); return necklace(e); };
    bool ok = true;
    std::string detail;
    for (auto [k, n, t] : { std::tuple{ 3, 8, "x..x..x." }, std::tuple{ 5, 8, "x.xx.xx." }, std::tuple{ 2, 5, "x.x.." },
                            std::tuple{ 4, 9, "x.x.x.x.." }, std::tuple{ 5, 12, "x..x.x..x.x." }, std::tuple{ 7, 16, "x..x.x.x..x.x.x." } }) {
        const bool m = necklace(euclid(k, n, 0)) == canon(t) && necklace(euclid(k, n, 3)) == canon(t);
        ok = ok && m;
        if (!m) detail += fmt("E(%d,%d) wrong  ", k, n);
    }
    check(ok, "Euclidean rhythms match Toussaint's table (E(3,8), E(5,8), E(2,5), E(4,9), E(5,12), E(7,16))", detail);

    bool four[16] = {}, off[16] = {}, clave[16] = {};
    for (int s : { 0, 4, 8, 12 }) four[s] = true;
    for (int s : { 2, 6, 10, 14 }) off[s] = true;
    for (int s : { 0, 3, 6, 10, 12 }) clave[s] = true;
    check(lhlSyncopation(four) == 0 && lhlSyncopation(off) == 7 && lhlSyncopation(clave) == 4,
          "LHL syncopation: four on the floor 0, offbeat eighths 7, son clave 4",
          fmt("%d, %d, %d", lhlSyncopation(four), lhlSyncopation(off), lhlSyncopation(clave)));

    // The hat of the measured references: offbeat eighth loudest, the other sixteenths softer, never the beat.
    {
        ParamStore q;
        q.parseText("compose.perc_density=0");
        PercPlan plan = makePercPlan(q, 12345, true);
        plan.hatMode = 0;
        std::vector<NoteEvent> e;
        PercBarSpec spec;
        spec.layers = plan.layers;
        spec.fills = false;
        composePercBar(q, plan, 12345, 0, 1, 145.0, 6, 1, spec, e);
        int onAnd = 0, elsewhere = 0;
        for (const NoteEvent& n : e) {
            if (n.lane != 0) continue;
            const int step = static_cast<int>(std::lround(n.beat * 4.0));
            if (step % 4 == 2) ++onAnd; else ++elsewhere;
        }
        check(onAnd == 4 && elsewhere == 0, "closed hat on the four offbeat eighths (density 0: no sixteenth layer)", fmt("%d on the offbeat, %d elsewhere", onAnd, elsewhere));
    }

    // Layers enter one per sixteen bars; fills at eight-bar ends; a roll with 32nds at sixteen; crash after it.
    {
        ParamStore q;
        q.parseText("compose.perc_variation=0 compose.perc_density=1");
        const PercPlan plan = makePercPlan(q, 777, true);
        check(plan.layers >= 4, "the kit offers enough layers for the form to build with", fmt("up to %d layers", plan.layers));
        int fillBars = 0, otherFills = 0;
        for (int b = 0; b < 64; ++b) {
            const FillType f = chooseFill(q, 777, b);
            if (b % 8 == 7) fillBars += f != FillType::None ? 1 : 0; else otherFills += f != FillType::None ? 1 : 0;
        }
        std::vector<NoteEvent> e15, e16;
        PercBarSpec full;
        full.layers = plan.layers;
        composePercBar(q, plan, 777, 15, 15, 145.0, 6, 1, full, e15);
        composePercBar(q, plan, 777, 16, 16, 145.0, 6, 1, full, e16);
        int thirtySeconds = 0;
        for (const NoteEvent& n : e15) if (n.lane == 5 && n.beat >= 63.0 && std::fabs(n.beat * 8.0 - std::round(n.beat * 8.0)) < 1e-9 && std::fabs(n.beat * 4.0 - std::round(n.beat * 4.0)) > 1e-9) ++thirtySeconds;
        check(fillBars == 8 && otherFills == 0 && thirtySeconds == 4, "fills in every eighth bar, a 32nd roll at bar sixteen",
              fmt("%d fills, %d elsewhere, %d 32nds", fillBars, otherFills, thirtySeconds));
    }

    // Euclidean lanes: never on the kick's beats, and at a syncopation between their extremes.
    {
        ParamStore q;
        int onBeat = 0, extremes = 0, checked = 0;
        for (uint64_t s = 1; s <= 40; ++s) {
            const PercPlan plan = makePercPlan(q, s, false);
            for (int l = 0; l < kPercLanes; ++l) {
                if (plan.pulses[l] == 0) continue;
                const std::vector<bool> e = euclid(plan.pulses[l], 16, plan.rotation[l]);
                int lo = 1000, hi = -1000, chosen = 0;
                for (int rot = 0; rot < 16; ++rot) {
                    const std::vector<bool> er = euclid(plan.pulses[l], 16, rot);
                    bool st[16] = {};
                    for (int i = 0; i < 16; ++i) st[i] = er[static_cast<size_t>(i)] && i % 4 != 0;
                    const int v = lhlSyncopation(st);
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                    if (rot == plan.rotation[l]) chosen = v;
                }
                ++checked;
                if (hi > lo && (chosen == lo || chosen == hi)) ++extremes;
            }
            std::vector<NoteEvent> notes;
            PercBarSpec sp;
            sp.layers = plan.layers;
            sp.fills = false;
            composePercBar(q, plan, s, 0, 40, 145.0, 6, 1, sp, notes);
            for (const NoteEvent& n : notes) {
                const PercRole role = static_cast<PercRole>(q.getInt(q.base(Module::Perc, n.lane) + perc::Role));
                const bool euclidRole = role == PercRole::Rim || role == PercRole::Tom || role == PercRole::Conga || role == PercRole::Zap || role == PercRole::Blip;
                if (euclidRole && std::lround(n.beat * 4.0) % 4 == 0) ++onBeat;
            }
        }
        check(onBeat == 0 && extremes < checked / 4, "Euclidean lanes avoid the beats and mostly sit between least and most syncopated",
              fmt("%d on a beat, %d of %d at an extreme", onBeat, extremes, checked));
    }

    // Over a night: hat modes, backbeat claps, layer counts and recipes all vary.
    {
        ParamStore q;
        q.parseText("compose.level_match=Off master.auto_gain=Off");
        Composer c(4242);
        int modes[3] = {}, backbeat = 0, minLayers = 99, maxLayers = 0, overrides = 0;
        double macroSpread = 0.0;
        for (int t = 0; t < 40; ++t) {
            const TrackPlan tp = c.track(q, t);
            ++modes[tp.perc.hatMode];
            backbeat += tp.perc.clapBackbeat ? 1 : 0;
            minLayers = std::min(minLayers, tp.perc.layers);
            maxLayers = std::max(maxLayers, tp.perc.layers);
            for (float m : tp.perc.macro) macroSpread = std::max(macroSpread, static_cast<double>(std::fabs(m)));
            for (int l = 0; l < kPercLanes; ++l) overrides += (tp.perc.engineOverride[l] >= 0 || tp.perc.modeSetOverride[l] >= 0) ? 1 : 0;
        }
        // The clap on 2 and 4 in every track since 19.09.2026: it is the groove's first new layer by the
        // user's rule, no longer a draw.
        check(modes[0] > 0 && modes[1] > 0 && modes[2] > 0 && backbeat == 40 && maxLayers - minLayers >= 2 && macroSpread > 0.5 && overrides > 0,
              "percussion varies over the night: hat modes, layers, kit recipe; the clap on the backbeat in every track",
              fmt("hat modes %d/%d/%d, clap backbeat in %d of 40, layers %d..%d, %d lane switches", modes[0], modes[1], modes[2], backbeat, minLayers, maxLayers, overrides));
    }
}

// ---------------------------------------------------------------------------------------------
// Phase 3: constrained sampling, diode ladder, acid, polyphonic engine, melody
// ---------------------------------------------------------------------------------------------

/** @brief Self test: constrained Markov sampling (Pachet and Roy). */
void testSampler()
{
    section("constrained Markov sampling (Pachet and Roy)");
    const ToyModel model(99);
    const int len = 5, A = model.alphabet();
    std::vector<std::vector<uint8_t>> allowed(static_cast<size_t>(len), std::vector<uint8_t>(static_cast<size_t>(A), 1));
    // Constraints that make the unconstrained continuation misleading: a narrow end, holes in between.
    allowed[1][0] = 0; allowed[2][1] = 0; allowed[2][2] = 0; allowed[4] = { 0, 0, 0, 1 };
    for (double temperature : { 1.0, 0.5 }) {
        // Exact conditional distribution by enumeration.
        std::vector<double> exact(static_cast<size_t>(std::pow(A, len)), 0.0);
        double z = 0.0;
        for (size_t code = 0; code < exact.size(); ++code) {
            int s[8], a = 0, b = 0;
            size_t c = code;
            double w = 1.0;
            for (int i = 0; i < len; ++i) { s[i] = static_cast<int>(c % A); c /= A; }
            for (int i = 0; i < len && w > 0.0; ++i) {
                w *= allowed[static_cast<size_t>(i)][static_cast<size_t>(s[i])] ? std::pow(model.prob(a, b, s[i]), 1.0 / temperature) : 0.0;
                a = b; b = s[i];
            }
            exact[code] = w;
            z += w;
        }
        for (double& e : exact) e /= z;
        // The exact sampler, and a greedy one that only renormalises over the allowed symbols of each step.
        Rng r;
        r.seed(5);
        auto uniform = [&]() { return static_cast<double>(r.uniform()); };
        const int draws = 300000;
        std::vector<double> got(exact.size(), 0.0), greedy(exact.size(), 0.0);
        std::vector<int> out;
        int bad = 0;
        for (int d = 0; d < draws; ++d) {
            sampleConstrained(model, allowed, 0, 0, temperature, uniform, out);
            size_t code = 0, mul = 1;
            for (int i = 0; i < len; ++i) { code += static_cast<size_t>(out[static_cast<size_t>(i)]) * mul; mul *= A; if (!allowed[static_cast<size_t>(i)][static_cast<size_t>(out[static_cast<size_t>(i)])]) ++bad; }
            got[code] += 1.0 / draws;
            int a = 0, b = 0;
            code = 0; mul = 1;
            for (int i = 0; i < len; ++i) {
                double w[8] = {}, tot = 0.0;
                for (int c = 0; c < A; ++c) { w[c] = allowed[static_cast<size_t>(i)][static_cast<size_t>(c)] ? std::pow(model.prob(a, b, c), 1.0 / temperature) : 0.0; tot += w[c]; }
                double x = uniform() * tot;
                int c = 0;
                while (c < A - 1 && x >= w[c]) { x -= w[c]; ++c; }
                code += static_cast<size_t>(c) * mul; mul *= A;
                a = b; b = c;
            }
            greedy[code] += 1.0 / draws;
        }
        double tvExact = 0.0, tvGreedy = 0.0;
        for (size_t k = 0; k < exact.size(); ++k) { tvExact += 0.5 * std::fabs(got[k] - exact[k]); tvGreedy += 0.5 * std::fabs(greedy[k] - exact[k]); }
        check(bad == 0 && tvExact < 0.02 && tvGreedy > 3.0 * tvExact, "samples exactly the constrained distribution (a greedy step-by-step sampler does not)",
              fmt("temperature %.1f: total variation %.4f exact, %.4f greedy; %d constraint violations", temperature, tvExact, tvGreedy, bad));
    }
    std::vector<std::vector<uint8_t>> impossible(3, std::vector<uint8_t>(static_cast<size_t>(A), 1));
    impossible[1] = std::vector<uint8_t>(static_cast<size_t>(A), 0);
    Rng r;
    auto uniform = [&]() { return static_cast<double>(r.uniform()); };
    std::vector<int> out;
    check(!sampleConstrained(model, impossible, 0, 0, 1.0, uniform, out), "reports a constraint that cannot be met");

    // The corpus pitch models are proper distributions in every context.
    double worst = 0.0;
    for (CorpusRoleId role : { CorpusRoleId::Acid, CorpusRoleId::Lead, CorpusRoleId::Arp }) {
        const PitchModel& m = corpusPitchModel(role);
        for (int a = 0; a < kCorpusAlphabet; a += 5)
            for (int b = 0; b < kCorpusAlphabet; b += 3) {
                double s = 0.0;
                for (int c = 0; c < kCorpusAlphabet; ++c) s += m.prob(a, b, c);
                worst = std::max(worst, std::fabs(s - 1.0));
            }
    }
    check(worst < 1e-9, "Witten-Bell pitch models sum to one in every context", fmt("largest deviation %.2e", worst));
}

// ================================================================================================
// Phase 8: the neural sequence model (Model.h, ModelKernel.h, docs/MODEL_FORMAT.md).
// ================================================================================================

/** @brief Self test: the model's lane kernels (exponential, matmul, norm, softmax). */
void testModelKernel()
{
    section("the model's lane kernels (exponential, matmul, norm, softmax)");

    // 2^k has to be exact -- it is the whole reason laneExp() does not touch the exponent bits.
    int pow2Bad = 0;
    for (int k = -127; k <= 127; ++k) {
        const float got = lanePow2i<float>(static_cast<float>(k));
        const float want = std::ldexp(1.0f, k);
        if (std::memcmp(&got, &want, sizeof(float)) != 0) ++pow2Bad;
    }
    check(pow2Bad == 0, "lanePow2i is exactly ldexp(1, k) for every k in -127..127", fmt("%d of 255 differ", pow2Bad));

    // The exponential against std::exp in double, over the range a softmax can produce. The argument
    // is rounded to float first and the reference taken of *that*: exp'(x) = exp(x), so the last bit
    // of a float near -85 is already 4e-6 of the answer, and comparing against exp of the unrounded
    // argument would measure that instead of the kernel.
    double worstExp = 0.0, atX = 0.0;
    for (int i = 0; i <= 40000; ++i) {
        const float x = static_cast<float>(-87.0 + 174.0 * i / 40000.0);
        const double want = std::exp(static_cast<double>(x));
        const double got = static_cast<double>(laneExp<float>(x));
        const double rel = std::fabs(got - want) / want;
        if (rel > worstExp) { worstExp = rel; atX = x; }
    }
    check(worstExp < 3e-7, "laneExp is within an ulp of std::exp over -87..87",
          fmt("largest relative error %.2e at x = %.3f (float eps 1.19e-7)", worstExp, atX));

    // GELU in the tanh form, against the same formula in double. Measured absolutely: for a very
    // negative argument the tanh form is 1 + tanh(...) with tanh at -1, and no float32 evaluation of
    // it has relative accuracy there -- what matters is that the answer is near zero, and it is.
    double worstGelu = 0.0;
    for (int i = 0; i <= 20000; ++i) {
        const double x = -12.0 + 24.0 * i / 20000.0;
        const double want = 0.5 * x * (1.0 + std::tanh(0.7978845608028654 * (x + 0.044715 * x * x * x)));
        const double got = static_cast<double>(laneGelu<float>(static_cast<float>(x)));
        worstGelu = std::max(worstGelu, std::fabs(got - want));
    }
    check(worstGelu < 2e-6, "laneGelu matches the tanh form of GELU", fmt("largest absolute error %.2e over -12..12", worstGelu));

    // A matrix-vector product with the output rows in the lanes, against double.
    Rng r;
    r.seed(4711);
    const int rows = 53, cols = 71;   // neither a multiple of 8 nor of 4: the padding must not leak
    std::vector<double> w(static_cast<size_t>(rows) * cols), b(static_cast<size_t>(rows)), x(static_cast<size_t>(cols));
    std::vector<float> wf(w.size()), bf(static_cast<size_t>(roundUpTo(rows, kVecWidth))), xf(static_cast<size_t>(cols));
    for (size_t i = 0; i < w.size(); ++i) { w[i] = 2.0 * r.uniform() - 1.0; wf[i] = static_cast<float>(w[i]); }
    for (int i = 0; i < rows; ++i) { b[static_cast<size_t>(i)] = 2.0 * r.uniform() - 1.0; bf[static_cast<size_t>(i)] = static_cast<float>(b[static_cast<size_t>(i)]); }
    for (int i = 0; i < cols; ++i) { x[static_cast<size_t>(i)] = 2.0 * r.uniform() - 1.0; xf[static_cast<size_t>(i)] = static_cast<float>(x[static_cast<size_t>(i)]); }
    std::vector<float> panels(static_cast<size_t>(roundUpTo(rows, kVecWidth)) * cols, 0.0f), out(bf.size(), 0.0f);
    packPanels<VecF>(wf.data(), nullptr, rows, cols, panels.data());
    matvecPanel<VecF>(panels.data(), bf.data(), xf.data(), rows, cols, out.data());
    // Accuracy is measured against the size of the terms, not against the size of the sum: a row of
    // random signs cancels down to near zero, and a relative error on that says nothing.
    double worstMv = 0.0;
    for (int i = 0; i < rows; ++i) {
        double want = b[static_cast<size_t>(i)], mag = std::fabs(b[static_cast<size_t>(i)]);
        for (int c = 0; c < cols; ++c) {
            const double term = w[static_cast<size_t>(i) * static_cast<size_t>(cols) + static_cast<size_t>(c)] * x[static_cast<size_t>(c)];
            want += term;
            mag += std::fabs(term);
        }
        worstMv = std::max(worstMv, std::fabs(out[static_cast<size_t>(i)] - want) / mag);
    }
    check(worstMv < 1e-6, "matvecPanel equals a double-precision matrix-vector product",
          fmt("largest error %.2e of the row's term magnitude, over %d rows of %d", worstMv, rows, cols));

    // And -- the reason the panels exist -- it equals, bit for bit, the sequential scalar loop that
    // the scalar build of this kernel performs. If this ever needs a tolerance, matvecPanel has
    // started summing across lanes and the three vector paths have stopped agreeing.
    int mvBits = 0;
    for (int i = 0; i < rows; ++i) {
        float acc = bf[static_cast<size_t>(i)];
        for (int c = 0; c < cols; ++c) acc = vfmadd(wf[static_cast<size_t>(i) * static_cast<size_t>(cols) + static_cast<size_t>(c)], xf[static_cast<size_t>(c)], acc);
        if (std::memcmp(&acc, &out[static_cast<size_t>(i)], sizeof(float)) != 0) ++mvBits;
    }
    check(mvBits == 0, "matvecPanel is bit-identical to the sequential scalar loop for every row", fmt("%d of %d rows differ", mvBits, rows));

    // Layer norm and softmax against the same formulas in double.
    const int n = 40;
    std::vector<float> xs(static_cast<size_t>(roundUpTo(n, kVecWidth))), gw(xs.size(), 0.0f), gb(xs.size(), 0.0f), ln(xs.size(), 0.0f);
    std::vector<double> xd(static_cast<size_t>(n)), gwd(static_cast<size_t>(n)), gbd(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        xd[static_cast<size_t>(i)] = 4.0 * r.uniform() - 2.0;
        gwd[static_cast<size_t>(i)] = 0.5 + r.uniform();
        gbd[static_cast<size_t>(i)] = r.uniform() - 0.5;
        xs[static_cast<size_t>(i)] = static_cast<float>(xd[static_cast<size_t>(i)]);
        gw[static_cast<size_t>(i)] = static_cast<float>(gwd[static_cast<size_t>(i)]);
        gb[static_cast<size_t>(i)] = static_cast<float>(gbd[static_cast<size_t>(i)]);
    }
    laneLayerNorm<VecF>(xs.data(), gw.data(), gb.data(), 1e-5f, n, ln.data());
    double mean = 0.0;
    for (int i = 0; i < n; ++i) mean += xd[static_cast<size_t>(i)];
    mean /= n;
    double var = 0.0;
    for (int i = 0; i < n; ++i) var += (xd[static_cast<size_t>(i)] - mean) * (xd[static_cast<size_t>(i)] - mean);
    var /= n;
    double worstLn = 0.0;
    for (int i = 0; i < n; ++i) {
        const double v = (xd[static_cast<size_t>(i)] - mean) / std::sqrt(var + 1e-5) * gwd[static_cast<size_t>(i)] + gbd[static_cast<size_t>(i)];
        worstLn = std::max(worstLn, std::fabs(ln[static_cast<size_t>(i)] - v) / std::max(1e-3, std::fabs(v)));
    }
    check(worstLn < 1e-5, "laneLayerNorm equals the double-precision layer norm", fmt("largest relative error %.2e", worstLn));

    std::vector<float> sm(xs.size(), 0.0f);
    for (int i = 0; i < n; ++i) sm[static_cast<size_t>(i)] = static_cast<float>(6.0 * xd[static_cast<size_t>(i)]);
    laneSoftmax<VecF>(sm.data(), n);
    double mx = -1e300, tot = 0.0;
    std::vector<double> sd(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) mx = std::max(mx, 6.0 * xd[static_cast<size_t>(i)]);
    for (int i = 0; i < n; ++i) { sd[static_cast<size_t>(i)] = std::exp(6.0 * xd[static_cast<size_t>(i)] - mx); tot += sd[static_cast<size_t>(i)]; }
    double worstSm = 0.0, sumSm = 0.0;
    for (int i = 0; i < n; ++i) {
        sumSm += sm[static_cast<size_t>(i)];
        worstSm = std::max(worstSm, std::fabs(sm[static_cast<size_t>(i)] - sd[static_cast<size_t>(i)] / tot));
    }
    check(worstSm < 1e-6 && std::fabs(sumSm - 1.0) < 1e-5, "laneSoftmax equals the double-precision softmax and sums to one",
          fmt("largest absolute error %.2e, total %.7f", worstSm, sumSm));
}

/** @brief Self test: the weight file and the forward pass against the trainer's reference vectors. */
void testModelFile()
{
    section("the weight file and the forward pass against the trainer's reference vectors");
    // melody.phosmdl is the trained export of 16.09.2026, copied from Tools/train/runs; the two tiny
    // models come from Tools/model/make_test_model.py, which implements docs/MODEL_FORMAT.md a second
    // time in NumPy -- a disagreement between the two implementations is what catches a misread of
    // the document rather than a slip in the arithmetic. PHOS_MODEL_BENCH adds a fourth.
    std::vector<std::string> models{ modelPath("melody.phosmdl"), modelPath("bass.phosmdl"),
                                     modelPath("test_tiny.phosmdl"), modelPath("test_tiny_f32.phosmdl") };
    if (const char* extra = std::getenv("PHOS_MODEL_BENCH")) models.push_back(extra);
    for (const std::string& full : models) {
        const std::string name = full.substr(full.find_last_of("/\\") + 1);
        NeuralModel model;
        std::string error;
        if (!model.load(full.c_str(), error)) { check(false, "loads the model", name + ": " + error); continue; }
        const std::vector<RefCase> cases = readRefCases(full + ".ref.txt");
        if (cases.empty()) { check(false, "reads the reference cases", name); continue; }
        double worstLogit = 0.0, worstProb = 0.0;
        size_t longest = 0;
        bool fed = true;
        for (const RefCase& c : cases) {
            if (!feedRefCase(model, c)) { fed = false; break; }
            longest = std::max(longest, c.tok.size());
            for (size_t i = 0; i < c.logits.size(); ++i) {
                worstLogit = std::max(worstLogit, std::fabs(model.logits()[i] - c.logits[i]));
                worstProb = std::max(worstProb, std::fabs(model.prob(static_cast<int>(i)) - c.probs[i]));
            }
        }
        // The tolerance of docs/MODEL_FORMAT.md section 5: 1e-3 on a logit, 1e-5 on a probability.
        check(fed && worstLogit < 1e-3 && worstProb < 1e-5, fmt("%s: matches the reference vectors", name.c_str()).c_str(),
              fmt("%zu cases, longest %zu positions, largest logit error %.2e, largest probability error %.2e",
                  cases.size(), longest, worstLogit, worstProb));
        if (name == "melody.phosmdl") {
            const ModelInfo& in = model.info();
            check(in.arch == ModelArch::Transformer && in.vocab == kCorpusAlphabet && in.tokenVersion == 1
                      && in.relMin == kCorpusRelMin && in.relMax == kCorpusRelMax && in.roles == kNumCorpusRoles,
                  "the trained model's header agrees with the composer's alphabet and roles",
                  fmt("%d layers, dim %d, %d heads, ffn %d, ctx %d, %zu parameters, %zu kB packed, held-out NLL %.3f nats",
                      in.layers, in.dim, in.heads, in.ffn, in.ctx, in.parameters, in.bytes / 1024, in.nll));
            check(in.condKick == 0, "a three-role melodic file carries no kick table",
                  fmt("condKick %d", in.condKick));
            // The eleventh table (18.09.2026). The installed melody model carries it; the bass file
            // and the two tiny test models do not, and the loop above reads all four with the same
            // loader -- which is the whole claim of "additive" in docs/MODEL_FORMAT.md section 3.
            check(in.condMode == kNumScales, "the trained melody model carries the mode table",
                  fmt("condMode %d against the composer's %d modes", in.condMode, kNumScales));
        }
        if (name == "bass.phosmdl") {
            const ModelInfo& in = model.info();
            check(in.arch == ModelArch::Transformer && in.vocab == kCorpusAlphabet && in.tokenVersion == 1
                      && in.relMin == kCorpusRelMin && in.relMax == kCorpusRelMax
                      && in.roles == kNumCorpusRoles + 1 && in.condKick == 3,
                  "the bass model's header is the melodic one plus the fourth role and the kick table",
                  fmt("roles %d, condKick %d, %d layers, dim %d, ctx %d, %zu parameters, %zu kB packed, "
                      "held-out NLL %.4f nats",
                      in.roles, in.condKick, in.layers, in.dim, in.ctx, in.parameters, in.bytes / 1024, in.nll));
        }
    }

    // The kick class is an input of its own, not a second copy of the step: feeding the same line with
    // a different kick class has to move the logits. Two of the reference cases were written with a
    // kick that does not follow from the step for exactly this reason (Tools/train/export_bass.py).
    {
        NeuralModel model;
        std::string error;
        if (model.load(modelPath("bass.phosmdl").c_str(), error)) {
            const std::vector<RefCase> cases = readRefCases(modelPath("bass.phosmdl") + ".ref.txt");
            double worst = 0.0;
            int moved = 0;
            for (const RefCase& c : cases) {
                if (c.kick.empty() || !feedRefCase(model, c)) continue;
                std::vector<double> base(static_cast<size_t>(kCorpusAlphabet));
                for (int i = 0; i < kCorpusAlphabet; ++i) base[static_cast<size_t>(i)] = model.logits()[i];
                RefCase shifted = c;
                for (int& k : shifted.kick) k = (k + 1) % 3;
                if (!feedRefCase(model, shifted)) continue;
                double d = 0.0;
                for (int i = 0; i < kCorpusAlphabet; ++i) d = std::max(d, std::fabs(model.logits()[i] - base[static_cast<size_t>(i)]));
                worst = std::max(worst, d);
                if (d > 1e-4) ++moved;
            }
            check(moved == static_cast<int>(cases.size()) && worst > 1e-3,
                  "the kick class reaches the output: rotating it moves every reference case's logits",
                  fmt("%d of %zu cases moved, largest change %.4f", moved, cases.size(), worst));
        } else {
            check(false, "loads bass.phosmdl for the kick-input check", error);
        }
    }

    // A line longer than the file's positional table is refused, not guessed at (Model.cpp).
    {
        NeuralModel tiny;
        std::string err;
        if (tiny.load(modelPath("test_tiny.phosmdl").c_str(), err)) {
            tiny.begin(0, 0, 1);
            int fed = 0;
            const NoteCond cond;
            while (tiny.step(12, cond)) ++fed;
            check(fed == tiny.info().ctx, "a line stops at the file's ctx positions instead of folding onto the last one",
                  fmt("%d positions accepted, ctx is %d", fed, tiny.info().ctx));
        } else {
            check(false, "loads the tiny model for the ctx check", err);
        }
    }

    // A broken file must say which field and where, not read garbage. Ten mutations of a good one.
    std::vector<uint8_t> good;
    {
        std::ifstream in(modelPath("test_tiny.phosmdl"), std::ios::binary);
        good.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "phos_model_test";
    std::filesystem::create_directories(dir);
    auto headerEnd = [](const std::vector<uint8_t>& f) { return static_cast<size_t>(16) + f[12] + (static_cast<size_t>(f[13]) << 8); };
    struct Mutation { const char* what; const char* expect; std::function<void(std::vector<uint8_t>&)> apply; };
    const std::vector<Mutation> mutations = {
        { "magic", "magic", [](std::vector<uint8_t>& f) { f[3] = 'X'; } },
        { "version", "version", [](std::vector<uint8_t>& f) { f[8] = 2; } },
        { "headerLen", "headerLen", [](std::vector<uint8_t>& f) { f[13] = 0xFF; } },
        { "a tensor's reserved field", "reserved", [&](std::vector<uint8_t>& f) { f[headerEnd(f) + 48 + 2] = 1; } },
        { "a tensor's dtype", "dtype", [&](std::vector<uint8_t>& f) { f[headerEnd(f) + 48] = 7; } },
        { "a width the tensors do not have", "asks for", [](std::vector<uint8_t>& f) {
              const std::string s(reinterpret_cast<const char*>(f.data()), std::min<size_t>(f.size(), 4096));
              const size_t at = s.find("\ndim=32");
              if (at != std::string::npos) f[at + 6] = '6'; } },
        { "the end marker", "past the end", [](std::vector<uint8_t>& f) { f[f.size() - 1] = 'X'; } },
        { "the alphabet in the header", "alphabet", [](std::vector<uint8_t>& f) {
              const std::string s(reinterpret_cast<const char*>(f.data()), std::min<size_t>(f.size(), 4096));
              const size_t at = s.find("vocab=37");
              if (at != std::string::npos) f[at + 7] = '9'; } },
        { "an architecture nobody wrote", "transformer nor ssm", [](std::vector<uint8_t>& f) {
              const std::string s(reinterpret_cast<const char*>(f.data()), std::min<size_t>(f.size(), 4096));
              const size_t at = s.find("arch=transformer");
              if (at != std::string::npos) f[at + 5] = 'X'; } },
        { "arch=ssm", "not implemented", [](std::vector<uint8_t>& f) {
              const std::string s(reinterpret_cast<const char*>(f.data()), std::min<size_t>(f.size(), 4096));
              const size_t at = s.find("arch=transformer");
              if (at != std::string::npos) { const char* r = "ssm\n"; for (int i = 0; i < 4; ++i) f[at + 5 + static_cast<size_t>(i)] = static_cast<uint8_t>(r[i]); } } },
    };
    int caught = 0, named = 0;
    std::string missed;
    for (const Mutation& m : mutations) {
        std::vector<uint8_t> bad = good;
        m.apply(bad);
        const std::filesystem::path p = dir / "broken.phosmdl";
        { std::ofstream out(p, std::ios::binary); out.write(reinterpret_cast<const char*>(bad.data()), static_cast<std::streamsize>(bad.size())); }
        NeuralModel model;
        std::string error;
        const bool ok = model.load(p.string().c_str(), error);
        if (!ok) ++caught;
        if (!ok && error.find(m.expect) != std::string::npos) ++named;
        else if (missed.empty()) missed = std::string(m.what) + " -> \"" + error + "\"";
    }
    check(caught == static_cast<int>(mutations.size()) && named == caught,
          "a damaged or unsupported weight file is refused with the field that is wrong",
          fmt("%d of %zu refused, %d named the field%s%s", caught, mutations.size(), named,
              missed.empty() ? "" : "; missed: ", missed.c_str()));

    // The search path a host sets, and the fallback when there is no model at all.
    std::filesystem::copy_file(modelPath("test_tiny.phosmdl"), dir / "named.phosmdl",
                               std::filesystem::copy_options::overwrite_existing);
    setModelSearchPath(dir.string());
    NeuralModel byName;
    std::string err1, err2;
    const bool found = byName.load("named.phosmdl", err1);
    NeuralModel missing;
    const bool absent = missing.load("there-is-no-such-model.phosmdl", err2);
    check(found && !absent, "a bare name is found in the host's search path, and a missing one is reported",
          fmt("%s / %s", found ? "found" : err1.c_str(), absent ? "loaded a file that is not there" : "reported missing"));
    setModelSearchPath(std::string());
}

/** @brief Self test: masked decoding: what it samples, and what stage A's guarantee cost. */
void testModelDecode()
{
    section("masked decoding: what it samples, and what stage A's guarantee cost");
    const ToyModel toy(99);
    const int len = 5, A = toy.alphabet();
    std::vector<std::vector<uint8_t>> allowed(static_cast<size_t>(len), std::vector<uint8_t>(static_cast<size_t>(A), 1));
    allowed[1][0] = 0; allowed[2][1] = 0; allowed[2][2] = 0; allowed[4] = { 0, 0, 0, 1 };

    // The two distributions, by enumeration: the exact constrained one (what stage A samples) and
    // the product of per-position renormalisations (what masked ancestral sampling samples).
    const size_t codes = static_cast<size_t>(std::pow(A, len));
    std::vector<double> exact(codes, 0.0), product(codes, 0.0);
    double z = 0.0;
    for (size_t code = 0; code < codes; ++code) {
        int s[8], a = 0, b = 0;
        size_t c = code;
        for (int i = 0; i < len; ++i) { s[i] = static_cast<int>(c % static_cast<size_t>(A)); c /= static_cast<size_t>(A); }
        double w = 1.0, q = 1.0;
        for (int i = 0; i < len; ++i) {
            if (!allowed[static_cast<size_t>(i)][static_cast<size_t>(s[i])]) { w = 0.0; q = 0.0; break; }
            double mass = 0.0;
            for (int d = 0; d < A; ++d) if (allowed[static_cast<size_t>(i)][static_cast<size_t>(d)]) mass += toy.prob(a, b, d);
            w *= toy.prob(a, b, s[i]);
            q *= toy.prob(a, b, s[i]) / mass;
            a = b; b = s[i];
        }
        exact[code] = w;
        product[code] = q;
        z += w;
    }
    for (double& e : exact) e /= z;

    Rng r;
    r.seed(5);
    auto uniform = [&]() { return static_cast<double>(r.uniform()); };
    ToyStepper stepper{ &toy, 0, 0 };
    const int draws = 300000;
    std::vector<double> got(codes, 0.0);
    std::vector<int> out;
    int violations = 0, retries = 0;
    MaskedDrawStats stats;
    for (int d = 0; d < draws; ++d) {
        sampleMasked(stepper, allowed, 0, 0, 1.0, uniform, out, &stats);
        retries += stats.retries;
        size_t code = 0, mul = 1;
        for (int i = 0; i < len; ++i) {
            if (!allowed[static_cast<size_t>(i)][static_cast<size_t>(out[static_cast<size_t>(i)])]) ++violations;
            code += static_cast<size_t>(out[static_cast<size_t>(i)]) * mul;
            mul *= static_cast<size_t>(A);
        }
        got[code] += 1.0 / draws;
    }
    double tvProduct = 0.0, tvExact = 0.0;
    for (size_t k = 0; k < codes; ++k) { tvProduct += 0.5 * std::fabs(got[k] - product[k]); tvExact += 0.5 * std::fabs(got[k] - exact[k]); }
    check(violations == 0 && tvProduct < 0.02, "masked decoding always stays inside the constraints and samples the per-position product",
          fmt("%d violations, total variation to the product %.4f, %d retries", violations, tvProduct, retries));
    // The honest part: this is not stage A's distribution, and by how much.
    check(tvExact > 4.0 * tvProduct, "and it is NOT the exactly constrained distribution stage A draws from",
          fmt("total variation to the exact constrained distribution %.4f, against %.4f to the product it really samples", tvExact, tvProduct));

    // The one failure masked decoding can have: a model so sure of a forbidden symbol that its whole
    // allowed set underflows. The draw is restarted, and after kMaskedRetries the weights decide.
    PeakedStepper peaked;
    std::vector<std::vector<uint8_t>> narrow(3, std::vector<uint8_t>(4, 0));
    for (auto& a : narrow) { a[1] = 1; a[2] = 3; }   // the peak (symbol 0) is forbidden everywhere
    MaskedDrawStats ps;
    bool inside = true;
    int fellBack = 0;
    for (int d = 0; d < 200; ++d) {
        sampleMasked(peaked, narrow, 0, 0, 1.0, uniform, out, &ps);
        if (ps.fellBack) ++fellBack;
        for (int i = 0; i < 3; ++i) if (!narrow[static_cast<size_t>(i)][static_cast<size_t>(out[static_cast<size_t>(i)])]) inside = false;
    }
    check(inside && fellBack == 200 && ps.retries == kMaskedRetries + 1,
          "a model whose allowed set has no mass left is retried and then the constraints decide alone",
          fmt("%d of 200 draws fell back after %d retries; every symbol still inside the constraints: %s",
              fellBack, ps.retries, inside ? "yes" : "no"));

    // The trained model with the composer's own constraint sets: the retry rate, and what it costs.
    NeuralModel model;
    std::string error;
    const char* bench = std::getenv("PHOS_MODEL_BENCH");
    const std::string path = bench != nullptr ? std::string(bench) : modelPath("melody.phosmdl");
    if (!model.load(path.c_str(), error)) { check(false, "loads the model for the decoding measurement", error); return; }
    const ModelInfo& in = model.info();

    int totalDraws = 0, totalRetries = 0, totalFallbacks = 0, totalSymbols = 0, smallest = 99;
    const auto t0 = std::chrono::steady_clock::now();
    for (int d = 0; d < 1200; ++d) {
        const int role = d % 3;
        const int scale = d % 6;
        std::vector<std::vector<uint8_t>> sets;
        std::vector<int> steps;
        int bars = 1;
        if (role == 0) {          // acid: the root, then scale tones over an ambitus of 12
            bars = 1 + (d % 2);
            sets.push_back(std::vector<uint8_t>(static_cast<size_t>(kCorpusAlphabet), 0));
            sets.back()[static_cast<size_t>(PitchModel::symbol(0))] = 1;
            for (int i = 1; i < 16; ++i) sets.push_back(scaleAllowed(scale, 0, 12, 8));
            for (int i = 0; i < 16; ++i) steps.push_back(i * bars);
        } else if (role == 1) {   // lead: scale tones with colour, chord tones every eighth step
            bars = 2;
            for (int i = 0; i < 12; ++i) {
                sets.push_back(i % 4 == 0 ? chordAllowed(scale, (d / 6) % 7, -5, 14) : scaleAllowed(scale, -5, 14, 24));
                steps.push_back(i * 2 + (i % 3));
            }
        } else {                  // arp: chord tones over an octave
            for (int i = 0; i < 12; ++i) { sets.push_back(chordAllowed(scale, (d / 6) % 7, 0, 12)); steps.push_back(i); }
        }
        for (const auto& s : sets) {
            int count = 0;
            for (uint8_t v : s) if (v) ++count;
            smallest = std::min(smallest, count);
        }
        std::vector<NoteCond> cond(sets.size());
        for (size_t i = 0; i < sets.size(); ++i) {
            cond[i].step = steps[i] % 16;
            cond[i].bar = (steps[i] / 16) % 8;
            cond[i].gap = noteGapCode(steps[i], i + 1 < steps.size() ? steps[i + 1] : 0, i + 1 < steps.size());
            cond[i].idx = noteIndexBucket(static_cast<int>(i));
        }
        NeuralStepper st{ &model, role, 0, bars, &cond, 0 };
        MaskedDrawStats ds;
        sampleMasked(st, sets, PitchModel::symbol(0), PitchModel::symbol(0), 1.0, uniform, out, &ds);
        ++totalDraws;
        totalRetries += ds.retries;
        totalFallbacks += ds.fellBack ? 1 : 0;
        totalSymbols += static_cast<int>(out.size());
    }
    const double micros = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1e6;
    check(totalFallbacks == 0, "the composer's own constraint sets never exhaust the model's mass",
          fmt("%d draws, %d symbols, %d retries, %d fallbacks; smallest allowed set %d of %d symbols",
              totalDraws, totalSymbols, totalRetries, totalFallbacks, smallest, kCorpusAlphabet));
    std::printf("         cost (%s, %s, dim %d x %d layers, %zu parameters): %.1f us per symbol, %.2f ms per 8-bar lead phrase (4 windows of 14 notes)\n",
                kVecPathName, bench != nullptr ? "bench model" : "melody.phosmdl", in.dim, in.layers, in.parameters,
                micros / totalSymbols, micros / totalSymbols * 56.0 / 1000.0);

    // Every drawn symbol has to reach the model as the next position's token. Reproduced here by
    // hand -- the same random stream, the same weights, but the token passed explicitly -- so that a
    // sampler that forgot to feed its own output back would part company at the second position.
    {
        Rng ra, rb;
        ra.seed(20260916);
        rb.seed(20260916);
        auto ua = [&]() { return static_cast<double>(ra.uniform()); };
        const int start = PitchModel::symbol(0);
        std::vector<std::vector<uint8_t>> sets(14, scaleAllowed(1, -5, 14, 8));
        std::vector<NoteCond> cond(sets.size());
        for (size_t i = 0; i < cond.size(); ++i) {
            cond[i].step = static_cast<int>(i) * 2 % 16;
            cond[i].bar = static_cast<int>(i) / 8;
            cond[i].gap = noteGapCode(static_cast<int>(i) * 2, static_cast<int>(i) * 2 + 2, i + 1 < cond.size());
            cond[i].idx = noteIndexBucket(static_cast<int>(i));
        }
        NeuralStepper st{ &model, 1, 0, 2, &cond, 0 };
        std::vector<int> viaSampler;
        MaskedDrawStats st2;
        sampleMasked(st, sets, start, start, 1.0, ua, viaSampler, &st2);

        std::vector<int> byHand;
        model.begin(1, 0, 2);
        int previous = start;
        bool fed = true;
        for (size_t i = 0; i < sets.size() && fed; ++i) {
            fed = model.step(previous, cond[i]);
            double total = 0.0;
            std::vector<double> w(static_cast<size_t>(kCorpusAlphabet), 0.0);
            for (int c = 0; c < kCorpusAlphabet; ++c) {
                if (sets[i][static_cast<size_t>(c)] == 0) continue;
                w[static_cast<size_t>(c)] = model.prob(c) * sets[i][static_cast<size_t>(c)];
                total += w[static_cast<size_t>(c)];
            }
            double x = static_cast<double>(rb.uniform()) * total;
            int chosen = -1;
            for (int c = 0; c < kCorpusAlphabet; ++c) {
                if (w[static_cast<size_t>(c)] <= 0.0) continue;
                chosen = c;
                if (x < w[static_cast<size_t>(c)]) break;
                x -= w[static_cast<size_t>(c)];
            }
            byHand.push_back(chosen);
            previous = chosen;
        }
        // And the same line drawn again with a different first token must be a different line: if it
        // were not, the context would not be reaching the model at all.
        Rng rc;
        rc.seed(20260916);
        auto uc = [&]() { return static_cast<double>(rc.uniform()); };
        std::vector<int> otherStart;
        sampleMasked(st, sets, start, PitchModel::symbol(7), 1.0, uc, otherStart, &st2);
        check(fed && viaSampler == byHand && viaSampler != otherStart,
              "the sampler feeds every drawn symbol back as the next token, and the context reaches the model",
              fmt("%zu symbols; by hand identical: %s; another first token gives another line: %s",
                  viaSampler.size(), viaSampler == byHand ? "yes" : "no", viaSampler != otherStart ? "yes" : "no"));
    }

    // The key/value cache is not cleared between lines, so this has to be true: what a draw produces
    // may not depend on the draw before it.
    Rng r1, r2;
    r1.seed(1234);
    r2.seed(1234);
    auto u1 = [&]() { return static_cast<double>(r1.uniform()); };
    auto u2 = [&]() { return static_cast<double>(r2.uniform()); };
    std::vector<std::vector<uint8_t>> shortSets(20, scaleAllowed(1, 0, 12, 8)), longSets(100, scaleAllowed(3, -5, 14, 8));
    std::vector<NoteCond> shortCond(20), longCond(100);
    for (size_t i = 0; i < shortCond.size(); ++i) { shortCond[i].step = static_cast<int>(i) % 16; shortCond[i].gap = 1; shortCond[i].idx = noteIndexBucket(static_cast<int>(i)); }
    for (size_t i = 0; i < longCond.size(); ++i) { longCond[i].step = static_cast<int>(i) % 16; longCond[i].bar = (static_cast<int>(i) / 16) % 8; longCond[i].gap = 1; longCond[i].idx = noteIndexBucket(static_cast<int>(i)); }
    NeuralStepper s1{ &model, 1, 0, 2, &shortCond, 0 };
    NeuralStepper s2{ &model, 1, 0, 8, &longCond, 0 };
    std::vector<int> alone, after, dummy;
    MaskedDrawStats ds;
    sampleMasked(s1, shortSets, 3, 7, 1.0, u1, alone, &ds);
    sampleMasked(s2, longSets, 0, 0, 1.0, u2, dummy, &ds);   // a longer line first, filling the cache
    r2.seed(1234);
    sampleMasked(s1, shortSets, 3, 7, 1.0, u2, after, &ds);
    check(alone == after && !alone.empty(), "a line is the same after a longer line as it is alone (the cache cannot leak)",
          fmt("%zu symbols, first three %d %d %d", alone.size(), alone.empty() ? -1 : alone[0],
              alone.size() > 1 ? alone[1] : -1, alone.size() > 2 ? alone[2] : -1));
}

/** @brief Self test: compose.melody_model: the composer on the trained model. */
void testMelodyModelWiring()
{
    section("compose.melody_model: the composer on the trained model");
    ParamStore p;
    const int cb = p.base(Module::Compose);
    check(p.getInt(cb + compose::MelodyModel) == 0,
          "the melody model defaults to Markov until the trained model is measured to be better",
          fmt("default %s", kMelodyModelNames[p.getInt(cb + compose::MelodyModel)]));

    const StyleProfile& style = styleProfile(styleOf(p));
    const MelodyPlan markov = makeMelodyPlan(p, style, 0xBEEF1234u, 6, 1, false, 0.5f);

    std::string note;
    const bool haveModel = sharedMelodyModel(&note) != nullptr;
    check(haveModel, "the shared melody model loads (Core/data/melody.phosmdl)", note);

    p.parseText("compose.melody_model=Neural");
    check(p.getInt(cb + compose::MelodyModel) == 1, "the knob takes its name from a preset line");
    const MelodyPlan a = makeMelodyPlan(p, style, 0xBEEF1234u, 6, 1, false, 0.5f);
    const MelodyPlan b = makeMelodyPlan(p, style, 0xBEEF1234u, 6, 1, false, 0.5f);
    bool same = a.acid[0].size() == b.acid[0].size() && a.lead[0].size() == b.lead[0].size();
    for (size_t i = 0; same && i < a.acid[0].size(); ++i) same = a.acid[0][i].rel == b.acid[0][i].rel;
    for (size_t i = 0; same && i < a.lead[0].size(); ++i) same = a.lead[0][i].rel == b.lead[0][i].rel;
    check(same, "two plans from the same seed are the same plan (the neural draw is deterministic)");

    // Every drawn pitch still obeys the constraints the composer set: in the scale, in the register.
    int outside = 0, notes = 0;
    for (const MelodyNote& n : a.acid[0]) { ++notes; if (!inScale(1, a.root[0] + n.rel - 6)) ++outside; }
    for (const MelodyNote& n : a.lead[0]) { ++notes; if (!inScale(1, a.root[1] + n.rel - 6)) ++outside; }
    check(outside == 0 && notes > 0, "every neural pitch is still a scale tone (the constraint masks hold)",
          fmt("%d notes, %d outside the scale", notes, outside));

    int changed = 0;
    for (size_t i = 0; i < std::min(a.acid[0].size(), markov.acid[0].size()); ++i)
        if (a.acid[0][i].rel != markov.acid[0][i].rel) ++changed;
    check(changed > 0, "the neural model really replaces the Markov model (the line is another line)",
          fmt("%d of %zu acid notes differ from the Markov plan", changed, markov.acid[0].size()));

    // The two models are each other's control: over many tracks the neural lines have to be *lines*,
    // not a constant or a random walk. The step-size distribution is the cheapest thing to compare.
    auto stepStats = [&](bool neural, double& meanStep, double& repeats, int& distinct) {
        ParamStore q;
        q.parseText(neural ? "compose.melody_model=Neural" : "compose.melody_model=Markov");
        double total = 0.0, same2 = 0.0;
        int count = 0;
        std::vector<int> seen(64, 0);
        for (int t = 0; t < 24; ++t) {
            const MelodyPlan m = makeMelodyPlan(q, style, 0x51EEDu + static_cast<uint64_t>(t) * 977u, t % 12, t % 6, false, 0.4f);
            for (const auto& ph : m.lead)
                for (size_t i = 1; i < ph.size(); ++i) {
                    const int d = ph[i].rel - ph[i - 1].rel;
                    total += std::fabs(static_cast<double>(d));
                    if (d == 0) same2 += 1.0;
                    ++count;
                    const size_t k = static_cast<size_t>(std::clamp(ph[i].rel + 12, 0, 63));
                    seen[k] = 1;
                }
        }
        meanStep = count > 0 ? total / count : 0.0;
        repeats = count > 0 ? same2 / count : 0.0;
        distinct = 0;
        for (int v : seen) distinct += v;
    };
    double mStep = 0.0, mRep = 0.0, nStep = 0.0, nRep = 0.0;
    int mDist = 0, nDist = 0;
    stepStats(false, mStep, mRep, mDist);
    stepStats(true, nStep, nRep, nDist);
    check(nStep > 0.3 && nStep < 2.0 * mStep && nDist >= 6,
          "the neural lead moves in steps of a musical size and uses a range of pitches",
          fmt("mean step %.2f semitones against Markov's %.2f, repeated notes %.0f%% against %.0f%%, %d distinct pitches against %d",
              nStep, mStep, 100.0 * nRep, 100.0 * mRep, nDist, mDist));
}

} // namespace phostest
