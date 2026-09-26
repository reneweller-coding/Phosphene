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
#include <cstring>

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

} // namespace phostest
