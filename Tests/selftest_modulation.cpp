/**
 * @file selftest_modulation.cpp
 * @brief The self test's checks of the filter models and the synths' own modulation (26.09.2026; Filters.h,
 *        Modulation.h): every model sounds and holds its level, the modulation keeps the render independent of the
 *        host's blocks, a synced LFO sits on the beat, and the filter envelope's sustain holds the filter open.
 *
 * The lanes of the models and the matrix are compared bit for bit against the scalar engine in phos_vectest
 * (testPolyModels); these are the behaviours on top.
 */
#include "SelfTestHelpers.h"
#include "phos/SoundPresets.h"
#include "phos/Loudness.h"
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <set>

using namespace phos;

namespace phostest {

namespace {

/** @brief Renders @p notes through a whole engine with @p settings, in blocks of @p block samples; left channel. */
std::vector<float> renderBlocks(const std::string& settings, const std::vector<NoteEvent>& notes, double seconds, int block)
{
    auto e = std::make_unique<Engine>();
    e->prepare(48000.0, block);
    e->params().parseText(("mix.kick_mute=1 mix.perc_mute=1 master.limiter=Off master.clipper=Off master.clip=Off "
                           "master.comp_ratio=1 fx.hall_return=-36 fx.room_return=-36 fx.plate_return=-36 " + settings).c_str());
    std::vector<NoteEvent> sorted = notes;
    std::stable_sort(sorted.begin(), sorted.end(), [](const NoteEvent& x, const NoteEvent& y) { return x.beat < y.beat; });
    for (const NoteEvent& n : sorted) e->pushEvent(n);
    std::vector<float> L(static_cast<size_t>(48000.0 * seconds)), R(L.size());
    for (size_t done = 0; done < L.size();) {
        const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), L.size() - done));
        e->process(L.data() + done, R.data() + done, n);
        done += static_cast<size_t>(n);
    }
    return L;
}

/** @brief RMS of @p y over samples [a, b), dB. */
double rmsDb(const std::vector<float>& y, double a, double b)
{
    double s = 0.0;
    size_t n = 0;
    for (size_t i = static_cast<size_t>(std::max(0.0, a)); i < static_cast<size_t>(b) && i < y.size(); ++i) { s += static_cast<double>(y[i]) * y[i]; ++n; }
    return 10.0 * std::log10(std::max(s / static_cast<double>(std::max<size_t>(n, 1)), 1e-30));
}

/** @brief One note of @p part. */
NoteEvent note(Part part, double beat, float length, int pitch)
{
    NoteEvent n;
    n.part = part; n.beat = beat; n.length = length; n.pitch = static_cast<uint8_t>(pitch); n.velocity = 100;
    return n;
}

} // namespace

/**
 * @brief Filter models, part `.levels`: every model of the lead and the bass sounds, stays finite and holds the level
 *        of the synth's own filter within a few dB -- the calibration of Poly.cpp (kTrimA/B) and the bass's drive.
 */
void testFilterModelsLevels()
{
    section("filter models: every model sounds, stays finite and near the synth's own filter's level");
    std::vector<NoteEvent> leads, basses;
    for (int b = 0; b < 16; ++b) {
        leads.push_back(note(Part::Lead, b, 0.5f, 62 + (b % 4) * 3));
        basses.push_back(note(Part::Bass, b + 0.5, 0.25f, 38));
    }
    std::string worst;
    int bad = 0;
    for (const char* part : { "lead", "bass" }) {
        const bool lead = std::string(part) == "lead";
        double ref = 0.0;
        for (int m = 0; m < kVoiceFilterModels; ++m) {
            const std::string s = fmt("mix.%s=1 %s.filter_model=%d %s.resonance=0.45 %s.delay_send=0", lead ? "bass_mute" : "lead_mute",
                                      part, m, part, part);
            const std::vector<float> y = renderBlocks(s, lead ? leads : basses, 7.0, 256);
            bool finite = true;
            for (float x : y) finite = finite && std::isfinite(x);
            const double r = rmsDb(y, 0.5 * 48000, 6.5 * 48000);
            if (m == 0) ref = r;
            const double d = r - ref;
            worst += fmt(" %s %d %+.2f%s;", part, m, d, finite ? "" : " (not finite)");
            if (!finite || r < -80.0 || std::fabs(d) > (lead ? 2.0 : 1.5)) ++bad;
        }
    }
    check(bad == 0, "nine models of the lead within 2 dB, of the bass within 1.5 dB of the synth's own filter, all finite",
          fmt("resonance 0.45, dB against model 0:%s", worst.c_str()));
}

/**
 * @brief Modulation, part `.blockSize`: with every synth's matrix moving, a render in blocks of 37 samples is the render
 *        in blocks of 512, bit for bit -- the envelope per sample, the matrix on the absolute 16-sample grid, the synced
 *        LFOs on the beat.
 */
void testModulationBlockSize()
{
    section("modulation: independent of the host's blocks");
    const std::string s =
        "compose.track_bars=32 compose.acid_amount=1 compose.lead_amount=1 "
        "lead.filter_model=5 lead.filt_attack=20 lead.filt_sustain=0.4 lead.lfo1_sync=6 lead.lfo2_rate=2.3 lead.lfo2_shape=6 "
        "lead.menv_decay=400 lead.mx1_src=1 lead.mx1_dst=6 lead.mx1_amount=0.5 lead.mx2_src=2 lead.mx2_dst=8 lead.mx2_amount=0.7 "
        "lead.mx3_src=5 lead.mx3_dst=1 lead.mx3_amount=0.1 lead.mx4_src=9 lead.mx4_dst=10 lead.mx4_amount=0.5 "
        "bass.filter_model=2 bass.lfo1_sync=7 bass.mx1_src=1 bass.mx1_dst=2 bass.mx1_amount=0.3 bass.mx2_src=5 bass.mx2_dst=3 bass.mx2_amount=0.4 "
        "acid.lfo1_sync=5 acid.mx1_src=1 acid.mx1_dst=2 acid.mx1_amount=0.3 acid.mx2_src=2 acid.mx2_dst=5 acid.mx2_amount=0.6";
    // Through the conductor, as testMelodyBlockSize: the path every render takes.
    std::vector<std::vector<float>> r;
    for (int block : { 37, 512, 4096 }) {
        auto e = std::make_unique<Engine>();
        e->prepare(48000.0, block);
        e->params().parseText(s.c_str());
        Composer cb(21);
        r.push_back(renderEngine(*e, cb, 20.0 * kBeatsPerBar, block, 48000.0));
    }
    int differ = 0;
    double energy = 0.0;
    for (size_t k = 1; k < r.size(); ++k)
        for (size_t i = 0; i < r[0].size() && i < r[k].size(); ++i) if (std::memcmp(&r[0][i], &r[k][i], sizeof(float)) != 0) ++differ;
    for (float x : r[0]) energy += static_cast<double>(x) * x;
    check(differ == 0 && energy > 1.0, "lead, bass and acid with their matrices: blocks of 37, 512 and 4096 samples, bit for bit",
          fmt("%d differing samples, energy %.1f", differ, energy));
}

/**
 * @brief Modulation, part `.sync`: an LFO synced to quarters (a square on the level, amount 1) opens the first half of
 *        every beat and shuts the second -- on the beat, whatever the note's own start.
 */
void testModulationSync()
{
    section("modulation: a synced LFO sits on the beat");
    const double beat = 60.0 / 145.0 * 48000.0;
    std::vector<NoteEvent> notes { note(Part::Pad, 0.37, 12.0f, 60) };
    const std::vector<float> y = renderBlocks("mix.bass_mute=1 pad.amp_attack=1 pad.amp_sustain=1 pad.delay_send=0 pad.lfo1_shape=4 pad.lfo1_sync=5 "
                                              "pad.mx1_src=1 pad.mx1_dst=9 pad.mx1_amount=1 pad.gate=0",
                                              notes, 6.0, 256);
    double worst = 1e9;
    for (int b = 2; b < 10; ++b) {
        const double open = rmsDb(y, (b + 0.1) * beat, (b + 0.4) * beat);
        const double shut = rmsDb(y, (b + 0.6) * beat, (b + 0.9) * beat);
        worst = std::min(worst, open - shut);
    }
    check(worst > 20.0, "every beat's first half open, its second shut (a square on the level, synced to quarters)",
          fmt("the smallest difference of eight beats %.1f dB", worst));
}

/**
 * @brief Modulation, part `.filterAdsr`: the filter envelope's sustain holds the filter open where the exponential
 *        decay closes it, and attack at its minimum with sustain 0 is the exponential decay itself.
 */
void testModulationFilterAdsr()
{
    section("modulation: the filter envelope as an ADSR");
    const double beat = 60.0 / 145.0 * 48000.0;
    std::vector<NoteEvent> notes { note(Part::Lead, 0.0, 6.0f, 57) };
    auto bright = [&](const char* s) {
        const std::vector<float> y = renderBlocks(std::string("mix.bass_mute=1 lead.amp_sustain=1 lead.delay_send=0 lead.cutoff=300 "
                                                              "lead.env_amount=4 lead.filter_decay=150 lead.lfo_cutoff=0 ") + s, notes, 4.0, 256);
        // The level of the late part of the note: the filter's opening is what reaches it.
        return rmsDb(y, 3.0 * beat, 5.0 * beat);
    };
    const double decay = bright("");
    const double compat = bright("lead.filt_attack=0.1 lead.filt_sustain=0 lead.filt_release=900");
    const double held = bright("lead.filt_sustain=0.6");
    check(held - decay > 3.0 && std::fabs(compat - decay) < 1e-9,
          "sustain 0.6 keeps the filter open (the late part of a held note louder), attack at its minimum and sustain 0 is the decay",
          fmt("late level with sustain %.1f dB, with the decay %.1f dB, the decay written as an ADSR %+.3g dB off", held, decay, compat - decay));
}

/**
 * @brief The preset bank (26.09.2026; PresetBank.cpp): 1024 presets for each of nine synths, names unique within a synth,
 *        every key of the table a real knob, and a sample of every group -- four presets of each, one per quarter of the
 *        grid -- sounds: finite, audible, without a runaway peak.
 */
void testPresetBank()
{
    section("preset bank: 1024 per synth, unique names, every group sounds");
    std::string unknown;
    const int bad = bankUnknownKeys(&unknown);
    struct S { Module m; int inst; Part part; int pitch; const char* key; };
    const S synths[] = { { Module::Poly, 0, Part::Lead, 67, "lead" }, { Module::Poly, 1, Part::Counter, 72, "counter" },
                         { Module::Poly, 2, Part::Arp, 64, "arp" }, { Module::Poly, 3, Part::Stab, 60, "stab" },
                         { Module::Poly, 4, Part::Pad, 57, "pad" }, { Module::Poly, 5, Part::Drone, 45, "drone" },
                         { Module::Bass, 0, Part::Bass, 38, "bass" }, { Module::Acid, 0, Part::Acid, 50, "acid" },
                         { Module::Kick, 0, Part::Kick, 36, "kick" } };
    int wrongCount = 0, dupNames = 0, silent = 0, loud = 0, nonFinite = 0, rendered = 0;
    std::string detail;
    // PHOS_BANK_TRIMS=<Core/src/PresetTrims.inl> (26.09.2026) renders every preset and writes that table: each preset's
    // K-weighted level (kick and bass: their low band) against the synth's default knobs, the correction the composer
    // rides on the synth's Level.
    const char* trimsPath = std::getenv("PHOS_BANK_TRIMS");
    std::vector<std::vector<int>> trims(kSoundSynths);
    // K-weighted RMS (BS.1770's weighting, ungated: a note pattern, not a programme), dB.
    auto kLevel = [](const std::vector<float>& y) {
        KFilter kf;
        kf.prepare(48000.0);
        double acc = 0.0;
        for (float x : y) { const double v = kf.process(x); acc += v * v; }
        return 10.0 * std::log10(std::max(acc / static_cast<double>(std::max<size_t>(y.size(), 1)), 1e-30));
    };
    // The low band (under 140 Hz, two Butterworth low passes in a row), dB: what kick and bass are matched on. K-weighting
    // all but ignores the sub, so a bright bass matched on it lost its low end -- the drops' presence against 40 .. 140 Hz
    // rose by 1.9 dB (testPresence, 26.09.2026).
    auto lowLevel = [](const std::vector<float>& y) {
        const double w = 2.0 * 3.14159265358979 * 140.0 / 48000.0, alpha = std::sin(w) / (2.0 * 0.70710678);
        const double a0 = 1.0 + alpha, b0 = (1.0 - std::cos(w)) / 2.0 / a0, b1 = (1.0 - std::cos(w)) / a0, a1 = -2.0 * std::cos(w) / a0, a2 = (1.0 - alpha) / a0;
        double z[2][2] = {}, acc = 0.0;
        for (float x : y) {
            double v = x;
            for (auto& s : z) {
                const double out = b0 * v + s[0];
                s[0] = b1 * v - a1 * out + s[1];
                s[1] = b0 * v - a2 * out;
                v = out;
            }
            acc += v * v;
        }
        return 10.0 * std::log10(std::max(acc / static_cast<double>(std::max<size_t>(y.size(), 1)), 1e-30));
    };
    for (const S& sy : synths) {
        const bool low = sy.m == Module::Kick || sy.m == Module::Bass;
        // The pad unweighted (26.09.2026): the bed is judged by its body, and K-weighting pulled the bright supersaw pads
        // down until their wide mids went missing from the mix (testStereoWidth, mid band 0.55 dB narrower).
        const bool flat = sy.m == Module::Poly && sy.inst == static_cast<int>(PolyInstance::Pad);
        auto level = [&](const std::vector<float>& y) { return low ? lowLevel(y) : flat ? rmsDb(y, 0.0, static_cast<double>(y.size())) : kLevel(y); };
        const std::vector<SoundPreset>& ps = factoryPresets(sy.m, sy.inst);
        if (ps.size() != 1024) { ++wrongCount; detail += fmt(" %s has %d;", sy.key, static_cast<int>(ps.size())); }
        std::set<std::string> names;
        for (const SoundPreset& x : ps) if (!names.insert(x.name).second) ++dupNames;
        std::vector<NoteEvent> notes;
        for (int b = 0; b < 8; ++b) notes.push_back(note(sy.part, b * 0.5, sy.part == Part::Pad || sy.part == Part::Drone ? 3.0f : 0.25f, sy.pitch + (b % 3) * 2));
        // Pad and drone are measured for the level correction as they play: a triad held for twelve beats, read from 2.5 s
        // to 4.5 s, past every attack of the bank (26.09.2026: eight staggered notes in 3 s under-read the slow attacks, and
        // the Dark Forest breakdown stood 0.2 dB too far under its drop, mix_audit drop_break_db).
        const bool bed = sy.part == Part::Pad || sy.part == Part::Drone;
        // The drone as it plays: its low root and the fifth (Melody.cpp, D2 .. C#3), not the pad's triad.
        std::vector<NoteEvent> held;
        if (sy.part == Part::Drone) {
            for (int iv : { 0, 7 }) held.push_back(note(sy.part, 0.0, 12.0f, 40 + iv));
        } else {
            for (int iv : { 0, 4, 7 }) held.push_back(note(sy.part, 0.0, 12.0f, sy.pitch + iv));
        }
        // Lead and counter as a phrase: four sixteenths, a quarter, an eighth and a held half (26.09.2026). The counter's
        // stage is the breakdown, where it plays long notes alone; matched on sixteenths only, its sustained presets came
        // out 7 dB under the default counter there (mix_audit p90_p10_db).
        const bool line = sy.part == Part::Lead || sy.part == Part::Counter;
        std::vector<NoteEvent> phrase;
        for (int q = 0; q < 4; ++q) phrase.push_back(note(sy.part, q * 0.5, 0.25f, sy.pitch + (q % 3) * 2));
        phrase.push_back(note(sy.part, 2.0, 1.0f, sy.pitch + 3));
        phrase.push_back(note(sy.part, 3.0, 0.5f, sy.pitch + 5));
        phrase.push_back(note(sy.part, 3.5, 2.0f, sy.pitch));
        auto trimLevel = [&](const std::string& text) {
            if (line) return level(renderBlocks(text, phrase, 3.0, 512));
            if (!bed) return level(renderBlocks(text, notes, 3.0, 512));
            const std::vector<float> y = renderBlocks(text, held, 4.5, 512);
            return level(std::vector<float>(y.begin() + static_cast<std::ptrdiff_t>(2.5 * 48000.0), y.end()));
        };
        // Every sixteenth preset under ctest; every one with PHOS_BANK_FULL set (on eight threads).
        const bool full = std::getenv("PHOS_BANK_FULL") != nullptr || trimsPath != nullptr;
        const size_t step = full ? 1 : 16;
        std::vector<size_t> which;
        for (size_t i = 0; i < ps.size(); i += step) which.push_back(i);
        struct R { double rms = 0.0, peak = 0.0, k = 0.0; bool finite = true; };
        std::vector<R> res(which.size());
        std::atomic<size_t> nextJob{ 0 };
        auto work = [&] {
            for (size_t j = nextJob++; j < which.size(); j = nextJob++) {
                std::string oneLine;
                for (char c : ps[which[j]].text) oneLine += c == '\n' ? ' ' : c;
                const std::vector<float> y = renderBlocks("mix.kick_mute=0 " + oneLine, notes, full ? 3.0 : 5.0, 512);
                R& r = res[j];
                for (float x : y) { r.finite = r.finite && std::isfinite(x); r.peak = std::max(r.peak, static_cast<double>(std::fabs(x))); }
                r.rms = rmsDb(y, 0.0, static_cast<double>(y.size()));
                if (trimsPath != nullptr) r.k = bed || line ? trimLevel("mix.kick_mute=0 " + oneLine) : level(y);
            }
        };
        std::vector<std::thread> pool;
        for (int t = 0; t < (trimsPath != nullptr ? 16 : full ? 8 : 1); ++t) pool.emplace_back(work);
        for (std::thread& t : pool) t.join();
        std::vector<double> levels;
        for (size_t j = 0; j < which.size(); ++j) {
            const R& r = res[j];
            const std::string& nm = ps[which[j]].name;
            ++rendered;
            if (!r.finite) { ++nonFinite; detail += fmt(" %s '%s' not finite;", sy.key, nm.c_str()); }
            else if (r.rms < -60.0) { ++silent; detail += fmt(" %s '%s' %.1f dB;", sy.key, nm.c_str(), r.rms); }
            else if (r.peak > 4.0) { ++loud; detail += fmt(" %s '%s' peak %.1f;", sy.key, nm.c_str(), r.peak); }
            if (r.finite && r.rms > -60.0) levels.push_back(r.rms);
        }
        if (trimsPath != nullptr) {
            const double ref = trimLevel("mix.kick_mute=0");
            const int k = sy.m == Module::Kick ? 0 : sy.m == Module::Bass ? 1 : sy.m == Module::Acid ? 2 : 3 + sy.inst;
            for (const R& r : res) trims[static_cast<size_t>(k)].push_back(static_cast<int>(std::lround(10.0 * std::clamp(ref - r.k, -20.0, 20.0))));
        }
        std::sort(levels.begin(), levels.end());
        if (!levels.empty())
            std::printf("    %-8s %4d rendered, level %.1f / %.1f / %.1f dB (10th / median / 90th percentile)\n", sy.key,
                        static_cast<int>(which.size()), levels[levels.size() / 10], levels[levels.size() / 2], levels[levels.size() * 9 / 10]);
    }
    if (trimsPath != nullptr) {
        if (FILE* f = std::fopen(trimsPath, "wb")) {
            std::fprintf(f, "// Generated by phos_selftest --only testPresetBank with PHOS_BANK_TRIMS=<this file> (Tests/selftest_modulation.cpp).\n"
                            "// Do not edit: each preset's level correction in tenths of a dB, kick, bass, acid, lead .. drone, 1024 each.\n"
                            "static const short kPresetTrimTenths[9][1024] = {\n");
            for (const std::vector<int>& t : trims) {
                std::fprintf(f, "{");
                for (size_t i = 0; i < 1024; ++i) std::fprintf(f, "%s%d", i == 0 ? "" : (i % 32 == 0 ? ",\n" : ","), i < t.size() ? t[i] : 0);
                std::fprintf(f, "},\n");
            }
            std::fprintf(f, "};\n");
            std::fclose(f);
            std::printf("    wrote %s\n", trimsPath);
        }
    }
    // The composer's draw keeps the counter off the lead's oscillator (26.09.2026, Composer.cpp walkAt), as the recipes do.
    {
        ParamStore ps;
        ps.parseText("compose.level_match=Off master.auto_gain=Off");   // the plans only, no probe renders
        auto oscOf = [](const SoundPreset& sp) {
            for (const auto& kv : sp.values) if (kv.first == poly::Osc) return static_cast<int>(std::lround(kv.second));
            return -1;
        };
        int same = 0, seen = 0;
        for (uint64_t seed : { 864566672ull, 7ull, 42ull }) {
            Composer c(seed);
            for (int t = 0; t < 8; ++t) {
                const TrackPlan tp = c.track(ps, t);
                const SoundPreset* lead = Composer::presetOf(ps, tp, 3);
                const SoundPreset* counter = Composer::presetOf(ps, tp, 4);
                if (lead == nullptr || counter == nullptr) continue;
                ++seen;
                if (oscOf(*lead) == oscOf(*counter)) ++same;
            }
        }
        check(seen >= 20 && same == 0, "the composer's counter preset never plays the lead preset's oscillator",
              fmt("%d tracks, %d with the same oscillator", seen, same));
    }
    check(bad == 0, "every key of the preset table is a knob of its synth", bad == 0 ? std::string("all found") : fmt("%d unknown, first %s", bad, unknown.c_str()));
    check(wrongCount == 0 && dupNames == 0, "nine synths with 1024 presets each, no name twice within a synth",
          fmt("%d synths off the count, %d repeated names;%s", wrongCount, dupNames, detail.c_str()));
    check(nonFinite == 0 && silent == 0 && loud == 0, "a sample of every group (4 per group) sounds: finite, over -60 dB, peaks under +12 dB",
          fmt("%d rendered: %d not finite, %d silent, %d too loud;%s", rendered, nonFinite, silent, loud, detail.c_str()));
}

} // namespace phostest
