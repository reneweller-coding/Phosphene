/**
 * @file selftest_synths.cpp
 * @brief The self test's synths: the diode ladder, the acid, the polyphonic engine, the melody, the wavetables and the pads.
 */
#include "SelfTestHelpers.h"

using namespace phos;

namespace phostest {

/** @brief Self test: diode ladder. */
void testDiodeLadder()
{
    section("diode ladder");
    const double sr = 96000.0, fc = 1000.0;
    const float g = static_cast<float>(std::sqrt(2.0) * std::tan(kPiD * fc / sr));
    double worst = 0.0;
    std::string detail;
    for (float k : { 0.0f, 8.0f, 16.0f }) {
        for (double hz : { 100.0, 400.0, 900.0, 1000.0, 1100.0, 2000.0, 5000.0 }) {
            DiodeLadderT<float> f;
            f.reset();
            const double got = sineGain([&](float x) { return f.tick(x, lanes<float>(g), lanes<float>(k), lanes<float>(0.0f)); }, hz, sr, 1e-3, 96000, 48000);
            // Zavalishin eq. 5.29 through the bilinear transform: s = j tan(pi f / fs) / g.
            const std::complex<double> s(0.0, std::tan(kPiD * hz / sr) / g);
            const std::complex<double> one(1.0, 0.0);
            const std::complex<double> den = 8.0 * std::pow(one + s, 4) - 8.0 * std::pow(one + s, 2) + one + static_cast<double>(k);
            const double want = 1.0 / std::abs(den);
            const double errDb = 20.0 * std::log10(got / want);
            worst = std::max(worst, std::fabs(errDb));
            if (hz == 1000.0) detail += fmt("k=%.0f at the peak %.2f dB  ", static_cast<double>(k), 20.0 * std::log10(got));
        }
    }
    check(worst < 0.05, "small-signal response equals Zavalishin's transfer function (k = 0, 8, 16)", fmt("worst %.3f dB; %s", worst, detail.c_str()));

    // Self-oscillation from k = 17 at the peak frequency; the Moog ladder's 4 does nothing here.
    auto ring = [&](float k, double& hz) {
        DiodeLadderT<float> f;
        f.reset();
        std::vector<float> y(96000);
        for (size_t i = 0; i < y.size(); ++i) y[i] = f.tick(i == 0 ? 1e-4f : 0.0f, g, k, 0.0f);
        double early = 0.0, late = 0.0;
        for (size_t i = 0; i < 4800; ++i) early = std::max(early, std::fabs(static_cast<double>(y[i])));
        for (size_t i = 91200; i < 96000; ++i) late = std::max(late, std::fabs(static_cast<double>(y[i])));
        int zc = 0;
        for (size_t i = 48001; i < 96000; ++i) if (y[i - 1] < 0.0f && y[i] >= 0.0f) ++zc;
        hz = zc / 0.5;
        return 20.0 * std::log10((late + 1e-30) / early);
    };
    double hzAbove = 0.0, hzBelow = 0.0, hzMoog = 0.0;
    const double above = ring(17.0f * 1.03f, hzAbove), below = ring(17.0f * 0.97f, hzBelow), moog = ring(4.0f, hzMoog);
    check(above > 20.0 && below < -20.0 && moog < -20.0 && std::fabs(hzAbove / fc - 1.0) < 0.01,
          "self-oscillates from k = 17 (not below, not at the transistor ladder's 4), at the resonance peak",
          fmt("k=17.5: %+.0f dB at %.1f Hz; k=16.5: %+.0f dB; k=4: %+.0f dB", above, hzAbove, below, moog));

    // DC gain 1/(1+k).
    DiodeLadderT<float> f;
    f.reset();
    float y = 0.0f;
    for (int i = 0; i < 96000; ++i) y = f.tick(1e-3f, g, 8.0f, 0.0f);
    check(std::fabs(y / 1e-3f * 9.0f - 1.0f) < 1e-3f, "DC gain is 1/(1 + k)", fmt("%.5f at k = 8 (expected %.5f)", static_cast<double>(y / 1e-3f), 1.0 / 9.0));
}

/** @brief Self test: acid voice. */
void testAcid()
{
    section("acid voice");
    const double sr = 48000.0;
    // Slide: legato, and the pitch approaches the new note with the slide time as time constant.
    {
        ParamStore p;
        auto a = makeAcid("acid.slide_time=50 acid.delay_send=0", p);
        a->noteOn(57, 1.0f, false, true, 48000, 0.0);
        renderMono([&](float* L, float* R, int n) { a->process(L, R, n); }, 4800);
        a->noteOn(64, 1.0f, false, false, 48000, 0.0);
        const bool legato = a->lastLegato();
        renderMono([&](float* L, float* R, int n) { a->process(L, R, n); }, 2400);   // 50 ms
        const double atTau = a->currentPitch();
        const double want = 64.0 - 7.0 * std::exp(-1.0);
        auto b = makeAcid("acid.slide_time=50 acid.delay_send=0", p);
        b->noteOn(57, 1.0f, false, false, 2400, 0.0);
        renderMono([&](float* L, float* R, int n) { b->process(L, R, n); }, 4800);
        b->noteOn(64, 1.0f, false, false, 48000, 0.0);
        check(legato && std::fabs(atTau - want) < 0.05 && !b->lastLegato() && b->currentPitch() == 64.0,
              "slide: legato glide reaching 1 - 1/e after the slide time; without slide a retrigger and a jump",
              fmt("pitch %.3f after 50 ms (expected %.3f)", atTau, want));
    }
    // Accent: louder, and consecutive accents charge the sweep higher and higher.
    {
        ParamStore p;
        auto rms = [](const std::vector<float>& y, size_t a, size_t n) { double e = 0.0; for (size_t i = a; i < a + n; ++i) e += static_cast<double>(y[i]) * y[i]; return std::sqrt(e / n); };
        // Without drive, so the saturation does not squeeze the difference.
        auto plain = makeAcid("acid.delay_send=0 acid.drive=0", p);
        auto acc = makeAcid("acid.delay_send=0 acid.drive=0", p);
        plain->noteOn(57, 1.0f, false, false, 4000, 0.0);
        acc->noteOn(57, 1.0f, true, false, 4000, 0.0);
        const std::vector<float> y0 = renderMono([&](float* L, float* R, int n) { plain->process(L, R, n); }, 4800);
        const std::vector<float> y1 = renderMono([&](float* L, float* R, int n) { acc->process(L, R, n); }, 4800);
        const double gainDb = 20.0 * std::log10(rms(y1, 0, 4800) / rms(y0, 0, 4800));
        const size_t sixteenth = static_cast<size_t>(0.25 * 60.0 / 145.0 * sr);
        auto run = makeAcid("acid.delay_send=0", p);
        double s[3];
        for (int k = 0; k < 3; ++k) {
            run->noteOn(57, 1.0f, true, false, static_cast<int>(sixteenth / 2), 0.0);
            renderMono([&](float* L, float* R, int n) { run->process(L, R, n); }, sixteenth);
            s[k] = run->accentSweep();
        }
        check(gainDb > 3.0 && s[1] > 1.2 * s[0] && s[2] > s[1], "accent: louder, and a run of accents climbs on the sweep capacitor",
              fmt("%+.1f dB; sweep after 1, 2, 3 accents %.3f %.3f %.3f", gainDb, s[0], s[1], s[2]));
    }
    // Squelch: the note opens several octaves brighter and closes within tens of milliseconds.
    {
        ParamStore p;
        auto sq = makeAcid("acid.squelch=On acid.env_amount=0 acid.delay_send=0 acid.drive=0", p);
        ParamStore q;
        auto dry = makeAcid("acid.squelch=Off acid.env_amount=0 acid.delay_send=0 acid.drive=0", q);
        sq->noteOn(57, 1.0f, false, false, 20000, 0.0);
        dry->noteOn(57, 1.0f, false, false, 20000, 0.0);
        const std::vector<float> ys = renderMono([&](float* L, float* R, int n) { sq->process(L, R, n); }, 16000);
        const std::vector<float> yd = renderMono([&](float* L, float* R, int n) { dry->process(L, R, n); }, 16000);
        // 21 ms windows, the first holding the squelch, the second well after it: the share of power above
        // 1.5 kHz (the fundamental at 220 Hz dominates a centroid whatever the filter does).
        auto highDb = [&](const float* x) {
            const std::vector<double> pw = powerSpectrum(x, 1024);
            double hi = 0.0, all = 0.0;
            for (size_t k = 1; k < pw.size(); ++k) { all += pw[k]; if (static_cast<double>(k) * sr / 1024.0 > 1500.0) hi += pw[k]; }
            return powDb(hi / all);
        };
        const double sEarly = highDb(ys.data()), sLate = highDb(ys.data() + 9600), dEarly = highDb(yd.data());
        check(sEarly > sLate + 12.0 && sEarly > dEarly + 12.0, "squelch: a bright attack that falls back within tens of milliseconds, unlike the plain note",
              fmt("power above 1.5 kHz: squelch %.1f dB -> %.1f dB, plain %.1f dB", sEarly, sLate, dEarly));
    }
    // Depth rule: the lowest acid note with drive and accent keeps under -30 dB below 140 Hz.
    {
        ParamStore p;
        auto a = makeAcid("acid.drive=1 acid.delay_send=0.6 acid.resonance=1", p);
        a->noteOn(kAcidLowest, 1.0f, true, false, 30000, 0.0);
        const std::vector<float> y = renderMono([&](float* L, float* R, int n) { a->process(L, R, n); }, 32768);
        const double low = lowShareDb(y, 140.0, true);
        check(low < -30.0, "acid at D3 with full drive and resonance: under -30 dB of its power below 140 Hz", fmt("%.1f dB", low));
    }
}

/** @brief Self test: polyphonic engine (supersaw, VA, FM). */
void testPoly()
{
    section("polyphonic engine (supersaw, VA, FM)");
    const double sr = 48000.0;
    // Szabo's measured tables against the curves.
    {
        static const double kDetune[][2] = { { 0.055118, 0.00967268 }, { 0.118110, 0.0220363 }, { 0.181102, 0.0339636 }, { 0.244094, 0.0467636 },
                                             { 0.307086, 0.0591273 }, { 0.370078, 0.0714909 }, { 0.433070, 0.0838545 }, { 0.496062, 0.0967273 },
                                             { 0.559055, 0.121527 }, { 0.622047, 0.147127 }, { 0.685039, 0.193455 }, { 0.748031, 0.243418 },
                                             { 0.811023, 0.2933815 }, { 0.874015, 0.343345 }, { 0.937007, 0.3928 }, { 1.0, 1.0 } };
        static const double kMix[][3] = { { 0.0, 1.0, 0.03836 }, { 0.244094, 0.86, 0.31 }, { 0.496062, 0.72, 0.5 }, { 0.748031, 0.585, 0.59 }, { 1.0, 0.445, 0.59 } };
        double wd = 0.0, wm = 0.0;
        for (const auto& d : kDetune) wd = std::max(wd, std::fabs(Poly::detuneCurve(d[0]) - d[1]));
        for (const auto& m : kMix) { double c, s; Poly::mixGains(m[0], c, s); wm = std::max({ wm, std::fabs(c - m[1]), std::fabs(s - m[2]) }); }
        check(wd < 0.02 && wm < 0.04, "detune and mix curves match Szabo's JP-8000 measurements (tables 2 and 3)",
              fmt("largest deviation: detune %.4f, mix %.4f", wd, wm));
        check(std::fabs(Poly::dynamicDetune(0.8, 0.6, 0.25) - 0.32) < 1e-9 && std::fabs(Poly::dynamicDetune(0.8, 0.6, 1.0) - 0.8) < 1e-9
              && std::fabs(Poly::dynamicDetune(0.8, 0.6, 0.5) - 0.56) < 1e-9, "dynamic detune: narrow on sixteenths, the knob from a beat, log2 between");
        double se = 0.0;
        for (int i = 0; i <= 20000; ++i) { const float x = -3.0f + 6.0f * static_cast<float>(i) / 20000.0f; se = std::max(se, std::fabs(static_cast<double>(laneSin01<float>(x)) - std::sin(2.0 * kPiD * x))); }
        check(se < 2e-6, "lane sine (folded Taylor series) matches sin to float precision", fmt("largest error %.2e", se));
    }
    // Supersaw spectrum: seven lines at Szabo's offsets, centre and sides in the mix ratio.
    {
        ParamStore p;
        auto e = makePoly("lead.detune=1 lead.dynamic_detune=0 lead.mix=0.75 lead.osc2=Off lead.lfo_cutoff=0 lead.lfo_pitch=0 lead.lfo_amp=0 lead.cutoff=18000 lead.env_amount=0 lead.key_track=0 lead.resonance=0 "
                          "lead.hp_track=0.2 lead.hp_floor=150 lead.width=0 lead.delay_send=0 lead.amp_attack=0.3 lead.amp_sustain=1", p, PolyInstance::Lead);
        e->noteOn(69, 1.0f, 4.0, 200000, 0.0);
        const std::vector<float> y = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 4800 + 65536);
        const std::vector<double> pw = powerSpectrum(y.data() + 4800, 65536);
        const double y1 = Poly::detuneCurve(1.0);
        double c, s;
        Poly::mixGains(0.75, c, s);
        double worstHz = 0.0, amps[kPolyUnison] = {};
        for (int u = 0; u < kPolyUnison; ++u) {
            const double hz = 440.0 * (1.0 + kSupersawOffsets[u] * y1);
            const size_t k0 = static_cast<size_t>(std::lround(hz * 65536.0 / sr));
            size_t best = k0;
            for (size_t k = k0 - 3; k <= k0 + 3; ++k) if (pw[k] > pw[best]) best = k;
            worstHz = std::max(worstHz, std::fabs(static_cast<double>(best) * sr / 65536.0 - hz));
            amps[u] = std::sqrt(pw[best - 1] + pw[best] + pw[best + 1]);
        }
        double sideDb = 0.0;
        for (int u : { 0, 1, 2, 4, 5, 6 }) sideDb += 20.0 * std::log10(amps[u] / amps[3]) / 6.0;
        const double wantDb = 20.0 * std::log10(s / c);
        check(worstHz < 1.5 && std::fabs(sideDb - wantDb) < 1.0, "supersaw: seven lines at the detune offsets, side-to-centre ratio of the mix curve",
              fmt("worst line %.2f Hz off; sides %+.2f dB vs %+.2f dB", worstHz, sideDb, wantDb));
    }
    // Random phases: two identical notes do not start with the same waveform.
    {
        ParamStore p;
        auto e = makePoly("lead.delay_send=0 lead.amp_attack=0.3", p, PolyInstance::Lead);
        e->noteOn(69, 1.0f, 0.25, 2000, 0.0);
        const std::vector<float> a = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 24000);
        e->noteOn(69, 1.0f, 0.25, 2000, 0.0);
        const std::vector<float> b = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 2400);
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (size_t i = 0; i < 2400; ++i) { ab += static_cast<double>(a[i]) * b[i]; aa += static_cast<double>(a[i]) * a[i]; bb += static_cast<double>(b[i]) * b[i]; }
        const double corr = ab / std::sqrt(aa * bb);
        check(corr < 0.9, "every note starts its oscillators at new random phases (Szabo)", fmt("correlation of two attacks %.3f", corr));
    }
    // FM: sidebands at the Bessel amplitudes of the remaining index.
    {
        ParamStore p;
        // The key-tracked high pass out of the way: at 0.7 f0 it takes 1.9 dB off the carrier alone.
        auto e = makePoly("lead.osc=FM lead.fm_ratio=3.5 lead.fm_index=4 lead.fm_decay=5 lead.detune=0 lead.mix=0 lead.osc2=Off lead.lfo_cutoff=0 lead.lfo_pitch=0 lead.lfo_amp=0 lead.cutoff=18000 lead.env_amount=0 lead.hp_track=0 lead.hp_floor=150 "
                          "lead.key_track=0 lead.resonance=0 lead.width=0 lead.delay_send=0 lead.amp_sustain=1 lead.amp_attack=0.3", p, PolyInstance::Lead);
        e->noteOn(69, 1.0f, 4.0, 200000, 0.0);
        const std::vector<float> y = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 9600 + 32768);
        const std::vector<double> pw = powerSpectrum(y.data() + 9600, 32768);
        auto amp = [&](double hz) {
            const size_t k0 = static_cast<size_t>(std::lround(hz * 32768.0 / sr));
            double best = 0.0;
            for (size_t k = k0 - 2; k <= k0 + 2; ++k) best = std::max(best, pw[k - 1] + pw[k] + pw[k + 1]);
            return std::sqrt(best);
        };
        // Index 0.3 x 4 = 1.2 after the decay: J0 = 0.6711, J1 = 0.4983, J2 = 0.1593.
        const double c0 = amp(440.0), up1 = amp(1980.0), lo1 = amp(1100.0), up2 = amp(3520.0);
        const double e1 = 20.0 * std::log10(up1 / c0) - 20.0 * std::log10(0.4983 / 0.6711);
        const double e1l = 20.0 * std::log10(lo1 / c0) - 20.0 * std::log10(0.4983 / 0.6711);
        const double e2 = 20.0 * std::log10(up2 / c0) - 20.0 * std::log10(0.1593 / 0.6711);
        check(std::fabs(e1) < 0.3 && std::fabs(e1l) < 0.3 && std::fabs(e2) < 0.5, "FM: first and second sidebands at J1/J0 and J2/J0 of the index",
              fmt("errors %+.2f / %+.2f / %+.2f dB", e1, e1l, e2));
    }
    // Depth rule for lead and arp at their lowest notes.
    {
        double worst = -1e9;
        for (PolyInstance inst : { PolyInstance::Lead, PolyInstance::Arp }) {
            for (int osc = 0; osc < 3; ++osc) {
                ParamStore p;
                const char* name = inst == PolyInstance::Lead ? "lead" : "arp";
                // Three %s, three names: the third argument was missing here, so the third conversion
                // read whatever stood on the stack. It happened not to crash until the allocations of
                // this file changed; a garbage module name also meant amp_sustain was never set.
                auto e = makePoly(fmt("%s.osc=%d %s.delay_send=0.5 %s.amp_sustain=1", name, osc, name, name).c_str(), p, inst);
                e->noteOn(inst == PolyInstance::Lead ? kLeadLowest : kArpLowest, 1.0f, 1.0, 30000, 0.0);
                e->noteOn((inst == PolyInstance::Lead ? kLeadLowest : kArpLowest) + 3, 1.0f, 1.0, 30000, 0.0);
                const std::vector<float> y = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 32768);
                worst = std::max(worst, lowShareDb(y, 140.0, true));
            }
        }
        check(worst < -30.0, "lead and arp at their lowest notes, every oscillator: under -30 dB below 140 Hz", fmt("worst %.1f dB", worst));
    }
    // The supersaw reads the mipmapped saw, not a PolyBLEP ramp (measured 16.09.2026: the ramp left
    // -46.6 / -43.3 / -40.9 dB at C5 / C6 / A6 with detune 1, the table -72.4 / -68.9 / -61.6 dB).
    {
        const char* const kBench = "lead.osc2=Off lead.lfo_cutoff=0 lead.lfo_pitch=0 lead.lfo_amp=0 lead.cutoff=18000 lead.env_amount=0 lead.key_track=0 lead.resonance=0 lead.hp_track=0 lead.hp_floor=150 "
                                   "lead.width=0 lead.delay_send=0 lead.dynamic_detune=0 lead.amp_attack=1 lead.amp_sustain=1 "
                                   "lead.amp_decay=4000 lead.vel_sens=0 lead.mix=0.75 lead.detune=1";
        double worst = 1e9, alias[3] = {};
        int i = 0;
        for (int pitch : { 72, 84, 93 }) {
            ParamStore p;
            auto e = makePoly(kBench, p, PolyInstance::Lead);
            e->noteOn(pitch, 1.0f, 8.0, 1 << 24, 0.0);
            const std::vector<float> y = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 9600 + 65536);
            alias[i] = supersawAliasDb(y, 9600, midiToHz(pitch), Poly::detuneCurve(1.0), sr);
            worst = std::min(worst, -alias[i]);
            ++i;
        }
        check(worst > 58.0, "supersaw: the mipmapped saw keeps the aliasing of high notes under -58 dB (a PolyBLEP ramp left -41 dB at A6)",
              fmt("C5 %.1f dB, C6 %.1f dB, A6 %.1f dB", alias[0], alias[1], alias[2]));
    }
    // The table frame is normalised to another RMS than the ramp; the compensation keeps the level.
    {
        ParamStore p;
        auto e = makePoly("lead.osc=Supersaw lead.detune=0.55 lead.mix=0.75 lead.osc2=Off lead.lfo_cutoff=0 lead.lfo_pitch=0 lead.lfo_amp=0 lead.cutoff=18000 lead.env_amount=0 lead.key_track=0 lead.resonance=0 "
                          "lead.hp_track=0 lead.hp_floor=150 lead.width=0 lead.delay_send=0 lead.dynamic_detune=0 lead.amp_attack=1 "
                          "lead.amp_sustain=1 lead.amp_decay=4000 lead.vel_sens=0 lead.level=0", p, PolyInstance::Lead);
        e->noteOn(60, 1.0f, 8.0, 1 << 24, 0.0);
        const std::vector<float> y = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 4800 + 48000);
        double e2 = 0.0;
        for (size_t k = 4800; k < y.size(); ++k) e2 += static_cast<double>(y[k]) * y[k];
        // Szabo's mix normalises the incoherent sum of the seven oscillators to one, so the voice is
        // as loud as one ramp: RMS 1/sqrt 3.
        const double db = 20.0 * std::log10(std::sqrt(e2 / static_cast<double>(y.size() - 4800)) * std::sqrt(3.0));
        check(std::fabs(db) < 0.5, "supersaw: reading the table instead of the ramp does not change the level",
              fmt("%+.2f dB against the RMS of the ramp 2t - 1", db));
    }
    // PolyBLEP stays the VA oscillator: it reaches harmonics the table's octave levels no longer hold.
    {
        auto harmonic = [&](const char* osc, int pitch, int h) {
            ParamStore p;
            auto e = makePoly(fmt("lead.osc=%s lead.wave=0 lead.detune=0 lead.mix=0 lead.osc2=Off lead.lfo_cutoff=0 lead.lfo_pitch=0 lead.lfo_amp=0 lead.cutoff=18000 lead.env_amount=0 lead.key_track=0 "
                                  "lead.resonance=0 lead.hp_track=0 lead.hp_floor=150 lead.width=0 lead.delay_send=0 lead.dynamic_detune=0 "
                                  "lead.amp_attack=1 lead.amp_sustain=1 lead.amp_decay=4000 lead.vel_sens=0", osc).c_str(), p, PolyInstance::Lead);
            e->noteOn(pitch, 1.0f, 8.0, 1 << 24, 0.0);
            const std::vector<float> y = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 9600 + 65536);
            const std::vector<double> pw = powerSpectrum(y.data() + 9600, 65536);
            const size_t k0 = static_cast<size_t>(std::lround(h * midiToHz(pitch) * 65536.0 / sr));
            const size_t k1 = static_cast<size_t>(std::lround(midiToHz(pitch) * 65536.0 / sr));
            double hi = 0.0, lo = 0.0;
            for (size_t k = k0 - 3; k <= k0 + 3; ++k) hi += pw[k];
            for (size_t k = k1 - 3; k <= k1 + 3; ++k) lo += pw[k];
            return powDb(hi / lo);
        };
        // A6: the table level that a 1760 Hz cycle reads keeps eight harmonics, so the ninth is gone;
        // the PolyBLEP ramp of the VA oscillator still has it. A ramp's ninth harmonic is 1/9 of the
        // fundamental (-19.1 dB), of which the voice's low pass at 18 kHz takes 5 dB at 15.8 kHz.
        const double va = harmonic("VA", 93, 9), sup = harmonic("Supersaw", 93, 9);
        check(va > -30.0 && sup < va - 25.0, "VA keeps the PolyBLEP ramp: at A6 it still has the ninth harmonic, the table saw does not",
              fmt("VA %.1f dB below the fundamental, supersaw %.1f dB", va, sup));
    }
    // The FM index is limited per note to the bandwidth Carson's rule allows. Without the limit,
    // index 10 at ratio 7.3 put 6 dB more power into aliasing than into its own carrier at C6.
    {
        struct Case { double ratio; int pitch; const char* what; };
        double worst = -1e9;
        std::string detail;
        for (const Case& c : { Case{ 7.5, 84, "C6 r=7.5" }, Case{ 7.5, 93, "A6 r=7.5" }, Case{ 3.5, 84, "C6 r=3.5" } }) {   // 7.3 until 24.09.2026: a voice plays the ratio to the nearest half (Poly.cpp, harmonicFmRatio)
            ParamStore p;
            auto e = makePoly(fmt("lead.osc=FM lead.fm_index=10 lead.fm_ratio=%g lead.fm_decay=2000 lead.detune=0 lead.mix=0 lead.osc2=Off lead.lfo_cutoff=0 lead.lfo_pitch=0 lead.lfo_amp=0 lead.cutoff=18000 "
                                  "lead.env_amount=0 lead.key_track=0 lead.resonance=0 lead.hp_track=0 lead.hp_floor=150 lead.width=0 "
                                  "lead.delay_send=0 lead.dynamic_detune=0 lead.amp_attack=1 lead.amp_sustain=1 lead.amp_decay=4000 lead.vel_sens=0",
                                  c.ratio).c_str(), p, PolyInstance::Lead);
            e->noteOn(c.pitch, 1.0f, 8.0, 1 << 24, 0.0);
            const std::vector<float> y = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 9600 + 65536);
            const double a = lineAliasDb(y, 9600, fmLines(midiToHz(c.pitch), c.ratio, sr), midiToHz(c.pitch), sr, 12000.0);
            worst = std::max(worst, a);
            detail += fmt("%s %.1f dB  ", c.what, a);
        }
        check(worst < -45.0, "FM: the index is limited to the bandwidth Carson's rule allows, so a high note does not alias over its own carrier", detail);
    }
    // 24.09.2026, the user on the first track's pad: "klingen einfach nur schraeg". Its FM ratio was 1.91 -- the knob's
    // 2 plus the recipe's offset -- and every chord tone carried sidebands a quarter tone off the key. A voice plays
    // the ratio to the nearest half (Poly.cpp, harmonicFmRatio), so at 1.91 everything it sounds is a harmonic of its
    // note (the pad stem of seed 1 had carried those sidebands 7 dB under the chord).
    {
        ParamStore p;
        auto e = makePoly("lead.osc=FM lead.fm_ratio=1.91 lead.fm_index=3 lead.fm_decay=2000 lead.detune=0 lead.mix=0 lead.osc2=Off lead.lfo_cutoff=0 "
                          "lead.lfo_pitch=0 lead.lfo_amp=0 lead.cutoff=18000 lead.env_amount=0 lead.key_track=0 lead.resonance=0 lead.hp_track=0 "
                          "lead.hp_floor=40 lead.width=0 lead.delay_send=0 lead.dynamic_detune=0 lead.drift=0 lead.amp_attack=1 lead.amp_sustain=1 "
                          "lead.amp_decay=4000 lead.vel_sens=0", p, PolyInstance::Lead);
        e->noteOn(57, 1.0f, 8.0, 1 << 24, 0.0);
        const std::vector<float> y = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 9600 + 65536);
        const double off = lineAliasDb(y, 9600, harmonicLines(midiToHz(57), sr), midiToHz(57), sr, 20.0);
        check(off < -50.0, "FM: a ratio between the halves plays the nearest half, so the voice stays harmonic (no bell in a chord)",
              fmt("power off the harmonics of A3 at fm_ratio 1.91: %.1f dB", off));
    }
    // The supersaw reads the saw frame, not the instance's table and position: two engines whose
    // table, position, position envelope and LFO stand at opposite ends must give the same samples.
    {
        auto render = [&](const char* tablePos) {
            ParamStore p;
            auto e = makePoly(fmt("lead.osc=Supersaw lead.detune=0.55 %s lead.pos_env=0.7 lead.pos_lfo_depth=0.4 lead.delay_send=0 "
                                  "lead.amp_attack=1 lead.amp_sustain=1", tablePos).c_str(), p, PolyInstance::Lead);
            e->noteOn(69, 1.0f, 2.0, 1 << 20, 0.0);
            return renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 24000);
        };
        const std::vector<float> a = render("lead.table=Classic lead.position=0");
        const std::vector<float> b = render("lead.table=Vocal lead.position=1");
        size_t bad = 0;
        double energy = 0.0;
        for (size_t i = 0; i < a.size(); ++i) { bad += a[i] != b[i] ? 1u : 0u; energy += static_cast<double>(a[i]) * a[i]; }
        check(bad == 0 && energy > 1.0, "supersaw: table, position, its envelope and its LFO belong to the wavetable oscillator and do not touch it",
              fmt("%zu of %zu samples differ, energy %.1f", bad, a.size(), energy));
    }
    // A group of eight oscillator slots can hold two voices of different types; every source the
    // group needs must be weighed in for the whole group, not only for its first slot.
    {
        ParamStore p;
        // Voice 0 is FM (slots 0..6), voice 1 a supersaw (slots 7..13): slot 7 is the supersaw's
        // outermost oscillator and lies in the first group, whose first slot belongs to the FM voice.
        // The FM voice plays at velocity 0 and full velocity sensitivity, so its gain is zero and its
        // own spectrum cannot stand in for a supersaw line that has gone missing -- only its source
        // weights remain, which is what decides the group's flags.
        auto e = makePoly("lead.osc=FM lead.fm_index=3 lead.fm_ratio=2 lead.detune=1 lead.dynamic_detune=0 lead.mix=0.75 lead.osc2=Off lead.lfo_cutoff=0 lead.lfo_pitch=0 lead.lfo_amp=0 lead.cutoff=18000 "
                          "lead.env_amount=0 lead.key_track=0 lead.resonance=0 lead.hp_track=0 lead.hp_floor=150 lead.width=0 "
                          "lead.delay_send=0 lead.amp_attack=1 lead.amp_sustain=1 lead.amp_decay=4000 lead.vel_sens=1", p, PolyInstance::Lead);
        e->noteOn(48, 0.0f, 8.0, 1 << 24, 0.0);
        p.parseText("lead.osc=Supersaw lead.vel_sens=0");
        std::vector<float> v = moduleValues(p, Module::Poly, polyIndex(PolyInstance::Lead));
        e->update(v.data(), 145.0);
        e->noteOn(69, 1.0f, 8.0, 1 << 24, 0.0);
        const std::vector<float> y = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 9600 + 65536);
        const std::vector<double> pw = powerSpectrum(y.data() + 9600, 65536);
        const double y1 = Poly::detuneCurve(1.0);
        auto line = [&](double hz) {
            const size_t k0 = static_cast<size_t>(std::lround(hz * 65536.0 / sr));
            double best = 0.0;
            for (size_t k = k0 - 3; k <= k0 + 3; ++k) best = std::max(best, pw[k]);
            return best;
        };
        double weakest = 1e30;
        for (int u = 0; u < kPolyUnison; ++u) weakest = std::min(weakest, line(440.0 * (1.0 + kSupersawOffsets[u] * y1)));
        const double db = powDb(weakest / line(440.0));
        check(db > -12.0, "an FM voice and a supersaw voice sharing a slot group: all seven supersaw lines still sound",
              fmt("weakest of the seven %.1f dB under the centre", db));
    }
    // The unison limit of the Quest level (Quality.h): the middle three of Szabo's seven lines at the
    // level of all seven -- and the work really left out, not only its gain set to zero. The second
    // check is the one that tells the two apart: zeroing the outer gains gives exactly the same
    // spectrum and the same level while the pre-pass still reads seven tables per sample.
    {
        const char* const kLimit = "lead.osc=Supersaw lead.detune=1 lead.dynamic_detune=0 lead.mix=0.75 lead.osc2=Off lead.lfo_cutoff=0 lead.lfo_pitch=0 lead.lfo_amp=0 lead.cutoff=18000 lead.env_amount=0 "
                                   "lead.key_track=0 lead.resonance=0 lead.hp_track=0 lead.hp_floor=150 lead.width=0 lead.delay_send=0 "
                                   "lead.amp_attack=1 lead.amp_sustain=1 lead.amp_decay=4000 lead.vel_sens=0";
        constexpr size_t kSkip = 4800, kLen = 65536;
        double rms[2] = {};
        uint64_t reads[2] = {};
        std::vector<float> y[2];
        int which = 0;
        for (int unison : { kPolyUnison, 3 }) {
            ParamStore p;
            auto e = makePoly(kLimit, p, PolyInstance::Lead);
            e->setQuality(unison, kPolyVoices);
            e->noteOnLimited(69, 1.0f, 8.0, 1 << 24, 0.0);
            y[which] = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, kSkip + kLen);
            double e2 = 0.0;
            for (size_t k = kSkip; k < y[which].size(); ++k) e2 += static_cast<double>(y[which][k]) * y[which][k];
            rms[which] = std::sqrt(e2 / static_cast<double>(y[which].size() - kSkip));
            reads[which] = e->tableReads();
            ++which;
        }
        const std::vector<double> pw = powerSpectrum(y[1].data() + kSkip, kLen);
        const double y1 = Poly::detuneCurve(1.0);
        auto line = [&](int u) {
            const size_t k0 = static_cast<size_t>(std::lround(440.0 * (1.0 + kSupersawOffsets[u] * y1) * static_cast<double>(kLen) / sr));
            double best = 0.0;
            for (size_t k = k0 - 3; k <= k0 + 3; ++k) best = std::max(best, pw[k]);
            return best;
        };
        double kept = 1e30, dropped = 0.0;
        for (int u : { 2, 3, 4 }) kept = std::min(kept, line(u));
        for (int u : { 0, 1, 5, 6 }) dropped = std::max(dropped, line(u));
        const double sep = powDb(kept / dropped), level = 20.0 * std::log10(rms[1] / rms[0]);
        check(sep > 30.0 && std::fabs(level) < 0.5, "quest unison 3: only the middle three lines (Szabo's +-0.01952356 y and 0), at the level of all seven",
              fmt("weakest kept line %.1f dB over the loudest dropped one, level %+.2f dB against unison 7", sep, level));
        const uint64_t want = static_cast<uint64_t>(kSkip + kLen);
        check(reads[1] == want * 3 && reads[0] == want * static_cast<uint64_t>(kPolyUnison),
              "quest unison 3: the wavetable pre-pass reads three tables per sample, not seven of which four are multiplied by zero",
              fmt("%llu reads at unison 3 (want %llu), %llu at unison 7 (want %llu)",
                  static_cast<unsigned long long>(reads[1]), static_cast<unsigned long long>(want * 3),
                  static_cast<unsigned long long>(reads[0]), static_cast<unsigned long long>(want * kPolyUnison)));
    }
}

/**
 * @brief The acid's colour, the dispersion and the analogue movement (round of 16.09.2026).
 *
 * Five subjects, each measured against a value derived somewhere other than in the code under test:
 * the clustering of the accents, the all-pass disperser, the comb's fractional-delay interpolator,
 * the even harmonic content of our acid line, and the thermal drift of the polyphonic engine.
 */
void testAcidColour()
{
    section("acid colour, dispersion and analogue movement");
    const double sr = 48000.0;

    // ------------------------------------------------------------------ the evenness measure itself
    // Before measuring our acid with it: the measure reproduces its analytic values. A synthetic
    // sawtooth must give 10 log10(1 - 1/n^2) and a square wave must fall off the bottom.
    {
        std::vector<float> saw(40000, 0.0f), sq(40000, 0.0f);
        const double f0 = 220.0;
        for (int h = 1; h <= 40; ++h)
            for (size_t i = 0; i < saw.size(); ++i) {
                const double s = std::sin(2.0 * kPiD * h * f0 * static_cast<double>(i) / sr) / h;
                saw[i] += static_cast<float>(s);
                if (h % 2 == 1) sq[i] += static_cast<float>(s);
            }
        const double s2 = evennessDb(saw, 0, f0, sr, 2), s4 = evennessDb(saw, 0, f0, sr, 4);
        const double q2 = evennessDb(sq, 0, f0, sr, 2);
        const double w2 = 10.0 * std::log10(0.75), w4 = 10.0 * std::log10(1.0 - 1.0 / 16.0);
        check(std::fabs(s2 - w2) < 0.05 && std::fabs(s4 - w4) < 0.05 && q2 < -40.0,
              "the evenness measure reproduces its analytic values (sawtooth, square wave)",
              fmt("saw E2 %.3f (want %.3f), E4 %.3f (want %.3f); square E2 %.1f dB", s2, w2, s4, w4, q2));
    }

    // ------------------------------------------------------------------ accents cluster
    // Measured in the corpus (Tools/ref_accent_runs.py, 16.09.2026): for the acid role the next onset
    // after an accent is accented with probability 0.493 against 0.197 after a plain note -- a lift of
    // 2.51. Melody.cpp draws the accent from a two-state chain with that lift and holds the
    // per-position marginal where it was, so the *number* of accents must not move, only their order.
    {
        long long runAfterAcc = 0, nAfterAcc = 0, runAfterPlain = 0, nAfterPlain = 0, accents = 0, onsets = 0;
        double wantMarginal = 0.0;
        ParamStore q;
        q.parseText("compose.level_match=Off master.auto_gain=Off");
        for (uint64_t s = 0; s < 8; ++s) {
            Composer c(1000 + s);
            for (int i = 0; i < 40; ++i) {
                const std::vector<MelodyNote>& a = c.track(q, i).melody.acid[0];
                for (size_t k = 0; k < a.size(); ++k) {
                    const bool acc = (a[k].flags & kNoteAccent) != 0;
                    const int pos = a[k].step % 4;
                    // 18.09.2026, rule 8 of the genre rules: accents on the "e" and the "a", none on
                    // the beat (the kick step), few on the "and" (MelodyParts.cpp, makeAcid).
                    wantMarginal += (pos == 1 || pos == 3) ? 0.36 : (pos == 2 ? 0.06 : 0.0);
                    ++onsets;
                    accents += acc ? 1 : 0;
                    // The lift is measured where it is defined: at the "e" and the "a", the positions
                    // that carry accents at all. Pooled over positions with rates of 0, 0.06 and 0.36
                    // the ratio of two mixtures says more about the positions than about clustering.
                    // Both onsets on an "e" or an "a": then both carry the same target rate and the
                    // chain's ratio is its lift exactly (Melody.cpp); a kick step before would mix in a
                    // rate of 0.
                    if (k + 1 < a.size() && a[k + 1].step % 2 == 1 && a[k].step % 2 == 1) {
                        const bool nxt = (a[k + 1].flags & kNoteAccent) != 0;
                        if (acc) { ++nAfterAcc; runAfterAcc += nxt ? 1 : 0; }
                        else { ++nAfterPlain; runAfterPlain += nxt ? 1 : 0; }
                    }
                }
            }
        }
        const double pa = static_cast<double>(runAfterAcc) / static_cast<double>(nAfterAcc);
        const double pp = static_cast<double>(runAfterPlain) / static_cast<double>(nAfterPlain);
        const double marg = static_cast<double>(accents) / static_cast<double>(onsets);
        wantMarginal /= static_cast<double>(onsets);
        // Measured at the "e" and the "a" (the positions of rule 8), the lift is the chain's own 2.51 up
        // to the sampling error and the mixture over the rate of the onset before (Melody.cpp).
        check(pa / pp > 2.1 && pa / pp < 2.9,
              "acid accents cluster with the lift measured in the corpus (2.51), instead of being drawn independently",
              fmt("at the e and the a: after an accent %.3f, after a plain note %.3f, lift %.2f "
                  "(n = %lld and %lld)", pa, pp, pa / pp, static_cast<long long>(nAfterAcc), static_cast<long long>(nAfterPlain)));
        // The independent draw produced exactly the per-position mean; the chain holds the marginal
        // only up to the same mixture effect, and it comes out 8 % under it. That is the whole change
        // in the amount of accenting -- the rest of the change is where the accents sit.
        check(marg > wantMarginal - 0.025 && marg < wantMarginal + 0.025,
              "clustering rearranges the accents and barely changes how many there are",
              fmt("%.4f against the independent draw's %.4f (%+.1f %%) over %lld onsets",
                  marg, wantMarginal, 100.0 * (marg / wantMarginal - 1.0), static_cast<long long>(onsets)));

        // And what it was all for: the sweep capacitor. The composed patterns are played into a real
        // acid voice **on their own step grid** -- a note only where the pattern puts one, silence
        // where it does not -- and the charge is read at the end of every sixteenth. A lone accent
        // cannot get the capacitor past 0.216 at the end of its own sixteenth (the value the accent
        // check of testAcid measures), so a higher reading is a second accent arriving before the
        // first has discharged. What is measured is the charge *the accented notes themselves see*,
        // because that is what lifts their cutoff: kSweepOctaves x Accent x Resonance x charge.
        {
            ParamStore q;
            q.parseText("compose.level_match=Off master.auto_gain=Off");
            const size_t sixteenth = static_cast<size_t>(0.25 * 60.0 / 145.0 * sr);
            std::vector<float> L(sixteenth), R(sixteenth);
            double peak = 0.0, sumAcc = 0.0;
            long long nAcc = 0;
            ParamStore ap;
            auto voice = makeAcid("acid.delay_send=0", ap);
            for (uint64_t s = 0; s < 6; ++s) {
                Composer c(2000 + s);
                for (int i = 0; i < 20; ++i) {
                    const MelodyPlan& m = c.track(q, i).melody;
                    const std::vector<MelodyNote>& a = m.acid[0];
                    voice->reset();
                    size_t next = 0;
                    for (int step = 0; step < m.acidSteps; ++step) {
                        bool accented = false;
                        if (next < a.size() && a[next].step == step) {
                            accented = (a[next].flags & kNoteAccent) != 0;
                            voice->noteOn(50 + a[next].rel, a[next].velocity / 127.0f, accented, false,
                                          static_cast<int>(sixteenth / 2), 0.0);
                            ++next;
                        }
                        voice->process(L.data(), R.data(), static_cast<int>(sixteenth));
                        const double sw = voice->accentSweep();
                        peak = std::max(peak, sw);
                        if (accented) { ++nAcc; sumAcc += sw; }
                    }
                }
            }
            const double mean = nAcc ? sumAcc / static_cast<double>(nAcc) : 0.0;
            // This is a guard, not a proof, and the measurement says why. The independent draw already
            // reached a mean charge of 0.248 and a peak of 0.406; the chain reaches 0.261 and 0.405.
            // The clustering nearly doubles the number of accents that follow an accent (0.20 -> 0.39)
            // and moves the charge under an accented note by 5 %, that is 0.011 of an octave of cutoff
            // at the default Accent and Resonance -- inaudible. The reason is the time constant: at
            // 145 BPM a sixteenth is 103 ms and the capacitor's tau is 150 ms, so accents *two* steps
            // apart already find it far from discharged. The physical argument for clustering is right
            // about the mechanism and wrong about the size of it at this tempo.
            check(peak > 0.35 && mean > 0.20,
                  "acid: the accented notes sit on a sweep charge above a lone accent's (a guard -- the "
                  "independent draw reached 0.248 and 0.406 too)",
                  fmt("highest charge %.3f (a lone accent reaches 0.216); mean charge under the %lld accented "
                      "notes %.3f, against 0.248 under 284 with independent draws", peak,
                      static_cast<long long>(nAcc), mean));
        }
    }

    // --------------------------------------------------------------- the disperser is an all-pass
    {
        Disperser d;
        d.set(kDisperseStages, 1250.0, sr);
        DisperserChannel ch;
        constexpr size_t N = 32768;
        std::vector<std::complex<double>> a(N);
        std::vector<float> ir(N);
        for (size_t i = 0; i < N; ++i) { ir[i] = ch.tick(i == 0 ? 1.0f : 0.0f, d.c, d.d, d.stages); a[i] = ir[i]; }
        fft(a);
        double worst = 0.0, worstHz = 0.0;
        for (size_t k = 1; k < N / 2; ++k) {
            const double hz = static_cast<double>(k) * sr / N;
            if (hz < 50.0 || hz > 20000.0) continue;
            const double db = std::fabs(20.0 * std::log10(std::abs(a[k])));
            if (db > worst) { worst = db; worstHz = hz; }
        }
        check(worst < 0.01, "disperser: the magnitude response is flat -- an all-pass moves energy in time, never between bands",
              fmt("worst deviation %.4f dB at %.0f Hz (eight sections over 395 to 3953 Hz)", worst, worstHz));

        // The group delay from the impulse response against the closed form of the coefficients.
        const double at[5] = { 140.0, 400.0, 1250.0, 4000.0, 16000.0 };
        double gd[5] = {}, want[5] = {}, mag = 0.0, worstGd = 0.0;
        auto dft = [&](double hz) {
            std::complex<double> s(0.0, 0.0);
            for (size_t n2 = 0; n2 < N; ++n2) s += static_cast<double>(ir[n2]) * std::polar(1.0, -2.0 * kPiD * hz * static_cast<double>(n2) / sr);
            return s;
        };
        for (int i = 0; i < 5; ++i) {
            disperserResponse(d, at[i], sr, mag, want[i]);
            const double e = std::max(0.5, at[i] * 1e-3);
            gd[i] = -std::arg(dft(at[i] + e) / dft(at[i])) / (2.0 * kPiD * e) * 1000.0;
            worstGd = std::max(worstGd, std::fabs(gd[i] - want[i]));
        }
        check(worstGd < 0.05 && gd[1] > gd[0] && gd[1] > 3.0 && gd[4] < 0.1,
              "disperser: the group delay is the designed one -- largest inside the band, small above it, "
              "and smaller under 140 Hz than at the band's foot",
              fmt("140 Hz %.2f ms, 400 %.2f, 1250 %.2f, 4000 %.2f, 16k %.2f (closed form %.2f/%.2f/%.2f/%.2f/%.2f)",
                  gd[0], gd[1], gd[2], gd[3], gd[4], want[0], want[1], want[2], want[3], want[4]));
    }

    // --------------------------------------------- what the disperser costs: crest factor and level
    {
        auto line = [&](const char* extra) {
            ParamStore p;
            auto a = makeAcid(fmt("acid.delay_send=0 acid.level=0 %s", extra).c_str(), p);
            std::vector<float> out;
            for (int n = 0; n < 8; ++n) {
                a->noteOn(57 + (n % 3) * 4, 1.0f, n % 2 == 0, false, 2400, 0.0);
                const std::vector<float> y = renderMono([&](float* L, float* R, int k) { a->process(L, R, k); }, 4800);
                out.insert(out.end(), y.begin(), y.end());
            }
            return out;
        };
        const std::vector<float> dry = line("acid.disperse=0"), wet = line("acid.disperse=8");
        auto crest = [](const std::vector<float>& y) {
            double e = 0.0, pk = 0.0;
            for (float v : y) { e += static_cast<double>(v) * v; pk = std::max(pk, std::fabs(static_cast<double>(v))); }
            return 20.0 * std::log10(pk / std::sqrt(e / static_cast<double>(y.size())));
        };
        auto rms = [](const std::vector<float>& y) {
            double e = 0.0;
            for (float v : y) e += static_cast<double>(v) * v;
            return 10.0 * std::log10(e / static_cast<double>(y.size()) + 1e-30);
        };
        const double cd = crest(dry), cw = crest(wet);
        const double rd = rms(dry), rw = rms(wet);
        check(std::fabs(rw - rd) < 0.25 && cw < cd - 0.5,
              "disperser: the loudness survives (an all-pass moves no energy) while the crest factor falls -- "
              "the price a peak-matched chain pays for the colour",
              fmt("RMS %+.2f -> %+.2f dB (%+.2f), crest %.2f -> %.2f dB (%+.2f)", rd, rw, rw - rd, cd, cw, cw - cd));

        ParamStore p;
        auto a = makeAcid("acid.disperse=8 acid.drive=1 acid.resonance=1 acid.delay_send=0", p);
        a->noteOn(kAcidLowest, 1.0f, true, false, 30000, 0.0);
        const std::vector<float> y = renderMono([&](float* L, float* R, int n) { a->process(L, R, n); }, 32768);
        const double low = lowShareDb(y, 140.0, true);
        check(low < -30.0, "disperser: the depth rule holds with the chain at full length",
              fmt("%.1f dB under 140 Hz at D3 with full drive and resonance", low));
    }

    // ------------------------------------------------------- the comb's fractional-delay interpolator
    {
        const float fb = 0.82f;
        const int delay = 61;
        float lag0[4], lag5[4], lin5[4] = { 0.0f, 0.5f, 0.5f, 0.0f };
        combTaps(0.0f, lag0[0], lag0[1], lag0[2], lag0[3]);
        combTaps(0.5f, lag5[0], lag5[1], lag5[2], lag5[3]);
        double worstLag = 0.0, worstLin = 0.0;
        std::string detail;
        for (double hz : { 2000.0, 5000.0, 10000.0 }) {
            const double ideal = combPeakDb(lag0, delay, fb, hz, sr);
            const double lag = combPeakDb(lag5, delay, fb, hz, sr);
            const double lin = combPeakDb(lin5, delay, fb, hz, sr);
            worstLag = std::max(worstLag, ideal - lag);
            worstLin = std::max(worstLin, ideal - lin);
            detail += fmt("%.0fk %.2f/%.2f/%.2f  ", hz / 1000.0, ideal, lag, lin);
        }
        check(worstLag < 2.5 && worstLin > 5.0 && worstLag < 0.5 * worstLin,
              "comb: a half-sample tuning keeps its resonance with Lagrange 3 where linear interpolation loses it",
              fmt("peak in dB, integer / Lagrange / linear -- %s(worst loss %.2f dB against %.2f dB)",
                  detail.c_str(), worstLag, worstLin));

        // Against the closed form: the peak of a comb is 1 / (1 - fb |H_interp|).
        double worstErr = 0.0;
        for (double hz : { 2000.0, 5000.0, 10000.0 }) {
            std::complex<double> h(0.0, 0.0);
            for (int t = 0; t < 4; ++t) h += static_cast<double>(lag5[t]) * std::polar(1.0, -2.0 * kPiD * hz / sr * t);
            const double want = 20.0 * std::log10(1.0 / (1.0 - static_cast<double>(fb) * std::abs(h)));
            worstErr = std::max(worstErr, std::fabs(combPeakDb(lag5, delay, fb, hz, sr) - want));
        }
        check(worstErr < 0.5, "comb: the measured resonance is the one the interpolator's magnitude predicts",
              fmt("worst deviation from 1 / (1 - fb |H|): %.2f dB", worstErr));

        // What the all-pass interpolator the literature prefers would cost here. It is exact in
        // magnitude, but it has state, and this comb is retuned at every note. Worse, its coefficient
        // a = (1 - fr) / (1 + fr) goes to 1 as the fraction goes to 0, which puts its pole on the unit
        // circle at z = -1: at that tuning the filter no longer forgets. The test drives the comb with
        // noise, retunes it and takes the noise away in the same sample, and asks what is left 100 ms
        // later -- by then the comb itself has decayed by 0.82^(100 ms / 1.3 ms) = -136 dB.
        auto ring = [&](bool allpass, double newDelay, double& after5ms) {
            std::vector<float> buf(4096, 0.0f);
            size_t pos = 0;
            const size_t mask = buf.size() - 1;
            float apState = 0.0f, w[4];
            double d = 61.5, steady = 0.0, tail = 0.0;
            after5ms = 0.0;
            Rng r;
            r.seed(7);
            for (size_t i = 0; i < 24000; ++i) {
                if (i == 12000) d = newDelay;                   // the retune a new note performs
                const int di = static_cast<int>(d);
                const float fr = static_cast<float>(d - di);
                const long long i0 = static_cast<long long>(pos) - di;
                float del;
                if (allpass) {
                    // First-order all-pass interpolator (Laakso et al. 1996, the Thiran form):
                    // y[n] = a x[n] + x[n-1] - a y[n-1] with a = (1 - fr) / (1 + fr).
                    const float av = (1.0f - fr) / (1.0f + fr);
                    del = av * buf[static_cast<size_t>(i0) & mask] + buf[static_cast<size_t>(i0 - 1) & mask] - av * apState;
                    apState = del;
                } else {
                    combTaps(fr, w[0], w[1], w[2], w[3]);
                    del = 0.0f;
                    for (int t = 0; t < 4; ++t) del += w[t] * buf[static_cast<size_t>(i0 - 1 + t) & mask];
                }
                const float v = (i < 12000 ? 0.05f * r.bipolar() : 0.0f) + fb * del;
                buf[pos] = v;
                pos = (pos + 1) & mask;
                if (i >= 11000 && i < 12000) steady = std::max(steady, std::fabs(static_cast<double>(v)));
                if (i >= 12000 && i < 12240) after5ms = std::max(after5ms, std::fabs(static_cast<double>(v)));
                if (i >= 16800) tail = std::max(tail, std::fabs(static_cast<double>(v)));
            }
            after5ms = 20.0 * std::log10(after5ms / steady + 1e-30);
            return 20.0 * std::log10(tail / steady + 1e-30);
        };
        double worstAp = -300.0, worstLg = -300.0, apEarly = 0.0, lgEarly = 0.0, e = 0.0;
        double atFrac = 0.0;
        for (double frac : { 0.02, 0.25, 0.5, 0.98 }) {
            const double a = ring(true, 47.0 + frac, e);
            if (a > worstAp) { worstAp = a; apEarly = e; atFrac = frac; }
            const double l = ring(false, 47.0 + frac, e);
            if (l > worstLg) { worstLg = l; lgEarly = e; }
        }
        check(worstAp > worstLg + 40.0 && worstLg < -100.0,
              "comb: the all-pass interpolator keeps ringing after a retune -- which happens at every note -- "
              "because its pole walks onto the unit circle as the fraction goes to zero; Lagrange has no state to ring",
              fmt("100 ms after the retune and with the input taken away: all-pass %+.0f dB (worst at fraction %.2f, "
                  "%+.1f dB in the first 5 ms), Lagrange %+.0f dB (%+.1f dB)",
                  worstAp, atFrac, apEarly, worstLg, lgEarly));
    }

    // ---------------------------------------- and the acid's own comb is the one that uses it
    // The checks above measure the interpolator; this one measures that Acid.cpp reads its delay line
    // with it. A note's comb delay is sr / f0, and its fractional part is whatever the tuning happens
    // to give: at A4 (pitch 69) it is 109.091 samples, almost on a sample, and one semitone lower
    // (pitch 68) it is 115.578, more than half a sample between two taps. The measure is the depth of
    // the comb's teeth -- for every comb period between 6 and 11 kHz the ratio of the largest to the
    // smallest bin -- which is a local contrast and therefore blind to the ladder's slope. An
    // interpolator that damps the top makes the teeth shallow, and it does so only at the tuning with
    // the large fraction, so the *difference* between the two notes is the fingerprint.
    {
        auto teeth = [&](int pitch) {
            ParamStore p;
            auto a = makeAcid("acid.squelch=On acid.env_amount=0 acid.key_track=0 acid.cutoff=8000 acid.drive=0 "
                              "acid.comb_feedback=0.9 acid.comb_mix=1 acid.amp_decay=4000 acid.delay_send=0 "
                              "acid.squelch_start=2 acid.low_cut=150", p);
            a->noteOn(pitch, 1.0f, false, false, 1 << 20, 0.0);
            const std::vector<float> y = renderMono([&](float* L, float* R, int n) { a->process(L, R, n); }, 9600 + 32768);
            constexpr size_t N = 32768;
            const std::vector<double> pw = powerSpectrum(y.data() + 9600, N);
            const double f0 = midiToHz(pitch), bin = sr / static_cast<double>(N);
            // The comb's peaks against its own harmonic series: the power of the band 6 to 11 kHz
            // against the power of the band one octave under the cutoff, which the comb damps equally
            // whatever the interpolator does. A ratio of two band powers has no numerical floor in it,
            // unlike a peak-to-valley depth, whose valleys sit in the denormal range.
            double hi = 0.0, lo = 0.0;
            for (size_t k = 1; k < N / 2; ++k) {
                const double hz = static_cast<double>(k) * bin;
                if (hz >= 6000.0 && hz < 11000.0) hi += pw[k];
                if (hz >= 1000.0 && hz < 2000.0) lo += pw[k];
            }
            (void)f0;
            return 10.0 * std::log10(hi / lo);
        };
        const double onSample = teeth(69), between = teeth(68);
        check(between > onSample - 3.0,
              "acid: the squelch comb reads its delay line with the Lagrange taps -- its top survives "
              "a half-sample tuning as well as a whole-sample one",
              fmt("power 6 to 11 kHz against 1 to 2 kHz: %.1f dB at A4 (fraction 0.09), %.1f dB at G#4 "
                  "(fraction 0.58), difference %+.1f dB", onSample, between, between - onSample));
    }

    // ------------------------------------------------ even harmonics of our acid (the bias question)
    {
        ParamStore p;
        auto a = makeAcid("acid.delay_send=0 acid.squelch=Off acid.env_amount=0 acid.cutoff=3000 acid.drive=0.45", p);
        a->noteOn(57, 1.0f, false, false, 1 << 20, 0.0);
        const std::vector<float> y = renderMono([&](float* L, float* R, int n) { a->process(L, R, n); }, 4800 + 32768);
        const double f0 = midiToHz(57);
        const double e2 = evennessDb(y, 4800, f0, sr, 2), e4 = evennessDb(y, 4800, f0, sr, 4);
        check(e2 > -6.0 && e4 > -6.0,
              "acid: the line already carries its even harmonics -- a sawtooth through a point-symmetric filter "
              "is not an odd-only spectrum",
              fmt("E2 %.2f dB, E4 %.2f dB (a sawtooth gives -1.25 and -0.28, a square wave minus infinity)", e2, e4));
    }

    // ------------------------------------------------------------------------------- thermal drift
    {
        // The standing deviation of a walk is the parameter, in cents. Independently derived: for
        // x += (w - x) alpha with w uniform on [-1, 1] the standing variance is alpha / (2 - alpha)
        // times var(w) = 1/3, and update() divides exactly that out again.
        ParamStore p;
        auto e = makePoly("lead.drift=2 lead.delay_send=0", p, PolyInstance::Lead);
        std::vector<float> L(4800), R(4800);
        double s = 0.0, s2 = 0.0, worst = 0.0;
        int n = 0;
        for (int b = 0; b < 400; ++b) {          // 40 s: about fifty correlation times
            e->process(L.data(), R.data(), 4800);
            for (int slot = 0; slot < kPolySlots; ++slot) {
                const double v = e->slotDrift(slot);
                s += v;
                s2 += v * v;
                ++n;
                worst = std::max(worst, std::fabs(v));
            }
        }
        const double sd = std::sqrt(s2 / n - (s / n) * (s / n));
        check(std::fabs(sd / 2.0 - 1.0) < 0.2 && worst < 12.0,
              "drift: the standing deviation of a walk is the parameter in cents, and it stays inside a few sigma",
              fmt("%.2f cents for a parameter of 2 (largest excursion %.2f cents over 40 s and 56 slots)", sd, worst));
    }
    {
        // Determinism from the seed and independence of the block size: the walks step on the absolute
        // sample grid, so cutting the render differently must change nothing at all.
        auto render = [&](uint64_t seed, int block) {
            ParamStore p;
            auto e = makePoly("lead.drift=3 lead.delay_send=0.4", p, PolyInstance::Lead);
            e->seedPhases(seed);
            std::vector<float> L(48000), R(48000);
            int done = 0, note = 0;
            while (done < 48000) {
                if (done % 3000 == 0) e->noteOnLimited(60 + (note++ % 5), 1.0f, 0.5, 2000, 0.0);
                const int k = std::min(block, 48000 - done);
                e->process(L.data() + done, R.data() + done, k);
                done += k;
            }
            return L;
        };
        const std::vector<float> a1 = render(11, 1000), a2 = render(11, 1000);
        const std::vector<float> b1 = render(11, 3), b2 = render(11, 125), c1 = render(12, 1000);
        auto differing = [](const std::vector<float>& x, const std::vector<float>& y) {
            int n = 0;
            for (size_t i = 0; i < x.size(); ++i) n += x[i] != y[i] ? 1 : 0;
            return n;
        };
        const int d3 = differing(a1, b1), d125 = differing(a1, b2), dseed = differing(a1, c1);
        check(a1 == a2 && d3 == 0 && d125 == 0 && dseed > 1000,
              "drift: the same seed gives the same samples at every block size; a different seed gives different samples",
              fmt("against block 1000: block 3 differs in %d samples, block 125 in %d, a repeat in %d; seed 12 differs in %d of %d",
                  d3, d125, differing(a1, a2), dseed, static_cast<int>(c1.size())));
    }
    {
        // With the drift switched off nothing drifts at all, and the start phases are untouched either
        // way -- the drift has its own generator, so drift 0 is the engine that never had one.
        ParamStore p;
        auto off = makePoly("lead.drift=0 lead.delay_send=0", p, PolyInstance::Lead);
        std::vector<float> L(9600), R(9600);
        off->process(L.data(), R.data(), 9600);
        double worst = 0.0;
        for (int s2 = 0; s2 < kPolySlots; ++s2) worst = std::max(worst, std::fabs(static_cast<double>(off->slotDrift(s2))));
        for (int v = 0; v < kPolyVoices; ++v) worst = std::max(worst, std::fabs(static_cast<double>(off->voiceDrift(v))));
        check(worst == 0.0, "drift: at 0 cents no walk ever leaves zero, so the sound from before it existed is reachable exactly",
              fmt("largest walk value %g after 0.2 s", worst));
    }
    {
        // The drift must not spoil the supersaw's aliasing figures, and this is the only place that
        // can say so: the benchmark in testPoly starts its note at sample 0, where every walk is still
        // at zero, so it never sees a drifted oscillator at all. Here the engine runs for a second
        // first, so the walks stand at their full spread when the note begins.
        //
        // The mask must be the *drifted* line set, and that is the whole point rather than a
        // convenience: a drift held for the note multiplies one oscillator's entire harmonic series by
        // one constant, so the series stays a series and only its spacing changes. Measured against
        // the undrifted lines the same signal reads -22.9 dB, because +-6 bins is +-4.4 Hz and one
        // cent at the tenth harmonic of A6 is already 10 Hz -- which is exactly the reading a drift
        // applied *during* the note would deserve, and the reason it is held (Poly.h).
        const char* const kBench = "lead.osc2=Off lead.lfo_cutoff=0 lead.lfo_pitch=0 lead.lfo_amp=0 lead.cutoff=18000 lead.env_amount=0 lead.key_track=0 lead.resonance=0 lead.hp_track=0 "
                                   "lead.hp_floor=150 lead.width=0 lead.delay_send=0 lead.dynamic_detune=0 lead.amp_attack=1 "
                                   "lead.amp_sustain=1 lead.amp_decay=4000 lead.vel_sens=0 lead.mix=0.75 lead.detune=1";
        double worst = 1e9, worstNominal = 1e9;
        std::string detail;
        for (int pitch : { 84, 93 }) {
            ParamStore p;
            auto e = makePoly(fmt("%s lead.drift=1", kBench).c_str(), p, PolyInstance::Lead);
            e->seedPhases(4711);
            std::vector<float> warm(48000), warmR(48000);
            e->process(warm.data(), warmR.data(), 48000);      // the walks reach their standing spread
            e->noteOnLimited(pitch, 1.0f, 8.0, 1 << 24, 0.0);
            // The voice the allocator took is voice 0 (every voice is idle), so its seven slots are 0..6.
            const double f0 = midiToHz(pitch), yDet = Poly::detuneCurve(1.0);
            std::vector<double> lines;
            double moved = 0.0;
            for (int u = 0; u < kPolyUnison; ++u) {
                const double cents = e->slotDrift(u);
                moved = std::max(moved, std::fabs(cents));
                const double fu = f0 * (1.0 + kSupersawOffsets[u] * yDet) * (1.0 + cents * (0.6931471805599453 / 1200.0));
                for (int h = 1; static_cast<double>(h) * fu < 0.5 * sr; ++h) lines.push_back(static_cast<double>(h) * fu);
            }
            const std::vector<float> y = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 9600 + 65536);
            const double a = maskedAliasDb(y, 9600, lines, sr);
            const double nominal = supersawAliasDb(y, 9600, f0, yDet, sr);
            worst = std::min(worst, -a);
            worstNominal = std::min(worstNominal, -nominal);
            detail += fmt("%s %.1f dB (drift up to %.2f ct)  ", pitch == 84 ? "C6" : "A6", a, moved);
        }
        check(worst > 58.0 && worstNominal < 40.0,
              "drift: a drifted supersaw still keeps its aliasing under -58 dB -- the pitch is held for the note, "
              "so each oscillator's harmonics stay a harmonic series",
              fmt("after a second of drifting at 1 cent: %s(against the *undrifted* line set the same signal "
                  "reads only -%.1f dB, which is what a drift inside a note would cost)", detail.c_str(), worstNominal));
    }
    {
        // It really moves the pitch, by cents, and only when it is switched on.
        const char* kOne = "lead.osc=Va lead.detune=0 lead.mix=0 lead.width=0 lead.delay_send=0 lead.osc2=Off lead.lfo_cutoff=0 lead.lfo_pitch=0 lead.lfo_amp=0 lead.cutoff=18000 "
                           "lead.env_amount=0 lead.key_track=0 lead.resonance=0 lead.hp_track=0 lead.hp_floor=150 "
                           "lead.amp_attack=1 lead.amp_sustain=1 lead.amp_decay=4000 lead.vel_sens=0 lead.dynamic_detune=0";
        auto f0Of = [&](double drift, uint64_t seed) {
            ParamStore p;
            auto e = makePoly(fmt("%s lead.drift=%g", kOne, drift).c_str(), p, PolyInstance::Lead);
            e->seedPhases(seed);
            std::vector<float> L(96000), R(96000);
            e->process(L.data(), R.data(), 48000);            // let the walk reach its standing spread
            e->noteOnLimited(69, 1.0f, 8.0, 1 << 20, 0.0);
            e->process(L.data(), R.data(), 96000);
            constexpr size_t N = 65536;
            const std::vector<double> pw = powerSpectrum(L.data() + 4800, N);
            const int k0 = static_cast<int>(std::lround(440.0 * static_cast<double>(N) / sr));
            int best = k0;
            for (int k = k0 - 40; k <= k0 + 40; ++k) if (pw[static_cast<size_t>(k)] > pw[static_cast<size_t>(best)]) best = k;
            const double lm = std::log(pw[static_cast<size_t>(best - 1)] + 1e-30);
            const double l0 = std::log(pw[static_cast<size_t>(best)] + 1e-30);
            const double lp = std::log(pw[static_cast<size_t>(best + 1)] + 1e-30);
            return (static_cast<double>(best) + 0.5 * (lm - lp) / (lm - 2.0 * l0 + lp)) * sr / static_cast<double>(N);
        };
        const double plain = f0Of(0.0, 5);
        double spread = 0.0;
        std::string detail;
        for (uint64_t seed : { 5ull, 6ull, 7ull, 8ull }) {
            const double cents = 1200.0 * std::log2(f0Of(6.0, seed) / plain);
            spread = std::max(spread, std::fabs(cents));
            detail += fmt("%+.2f ", cents);
        }
        check(std::fabs(1200.0 * std::log2(f0Of(0.0, 9) / plain)) < 0.02 && spread > 1.0 && spread < 40.0,
              "drift: the pitch of a note really moves, and only when the drift is on",
              fmt("at 6 cents over four seeds: %scents; at 0 cents the same note to 0.02 cents", detail.c_str()));
    }
}

/**
 * @brief Melody, part `.score`: 16 tracks of 128 bars read against the scale, the chords, the depth
 *        rule, the slides and the register rule.
 *
 * testMelody was split on 19.09.2026 (round "test-split") into its five independent blocks: `.score`
 * (this), `.variety`, `.depthRender`, `.blockSize` and `.midi`.
 */
void testMelodyScore()
{
    section("melody: chords, acid, lead, arp -- the score");
    ParamStore p;
    p.parseText("compose.track_bars=128 compose.acid_amount=0.8 compose.lead_amount=0.8 compose.arp_amount=0.8 compose.level_match=Off master.auto_gain=Off");
    Composer c(606);
    std::vector<NoteEvent> ev;
    const int tracks = 16;
    c.composeBars(p, 0, tracks * 128, ev);
    int outside = 0, notes = 0, weak = 0, strong = 0, arpOff = 0, arpNotes = 0, tooLow = 0, slides = 0, slideGaps = 0, masked = 0, sharedBlocks = 0;
    std::vector<const NoteEvent*> acidNotes;
    struct Range { int lo = 127, hi = 0; };
    // Grouped by section, not by sixteen-bar block: since Phase 5 the masking rule is decided per
    // section of the form (Form.h), and a sixteen-bar block may straddle two of them.
    // 18.09.2026: grouped by bar -- the arp stays in its own register now and never changes octave
    // inside a section, so the masking that matters is what sounds together (Melody.cpp).
    std::vector<Range> leadR(static_cast<size_t>(tracks * 128)), arpR(static_cast<size_t>(tracks * 128));
    for (const NoteEvent& e : ev) {
        if (e.part != Part::Acid && e.part != Part::Lead && e.part != Part::Arp) continue;
        const int bar = static_cast<int>(e.beat / kBeatsPerBar);
        const int ti = c.trackOfBar(p, bar);
        const TrackPlan t = c.track(p, ti);
        const int inTrack = bar - t.firstBar;
        ++notes;
        // The mode of the *section*, not of the track: since 16.09.2026 a section may borrow another
        // mode over the tonic pedal (Form.h), and the melodic layer is drawn in the mode its section
        // plays. The tonic never moves, so "in the scale" is still measured against the track's key.
        const int sc = t.form.section[sectionOfBar(t.form, inTrack)].scale;
        if (!inScale(sc, e.pitch - t.key)) ++outside;
        const int lowest = e.part == Part::Acid ? kAcidLowest : (e.part == Part::Lead ? kLeadLowest : kArpLowest);
        if (e.pitch < lowest) ++tooLow;
        int pcs[3];
        chordTones(sc, 0, pcs);   // the lines anchor on the tonic chord over the Bordun bass (22.09.2026)
        const int pc = ((e.pitch - t.key) % 12 + 12) % 12;
        const bool chordTone = pc == pcs[0] || pc == pcs[1] || pc == pcs[2];
        const double inBar = e.beat - bar * kBeatsPerBar;
        if (e.part == Part::Lead && std::fabs(inBar - std::round(inBar / 2.0) * 2.0) < 1e-9) { ++strong; if (!chordTone) ++weak; }
        if (e.part == Part::Arp) {
            // Since 18.09.2026 (rule 13) the arp plays sus2 / sus4 / add9 material over the chord root --
            // over the tonic where the chord root is itself a colour tone -- and a colour tone only as a
            // glint; RuleRef derives the material from the scale table.
            ++arpNotes;
            const int d0 = t.melody.chordDegree[chordIndexAt(t.melody, inTrack)];
            const int d = RuleRef::colour(sc, RuleRef::chordRoot(sc, d0)) ? 0 : d0;
            std::set<int> material = { RuleRef::chordRoot(sc, d), RuleRef::deg(sc, d + 4) % 12 };
            if (t.melody.arpTones == 0) material.insert(RuleRef::deg(sc, d + 1) % 12);
            if (t.melody.arpTones == 1) material.insert(RuleRef::deg(sc, d + 3) % 12);
            if (t.melody.arpTones == 2) { material.insert(RuleRef::deg(sc, d + 2) % 12); material.insert(RuleRef::deg(sc, d + 1) % 12); }
            if (material.count(pc) == 0 && !RuleRef::colour(sc, pc)) ++arpOff;
        }
        if (e.part == Part::Acid) acidNotes.push_back(&e);
        const size_t block = static_cast<size_t>(bar);
        if (e.part == Part::Lead) { leadR[block].lo = std::min(leadR[block].lo, int(e.pitch)); leadR[block].hi = std::max(leadR[block].hi, int(e.pitch)); }
        if (e.part == Part::Arp) { arpR[block].lo = std::min(arpR[block].lo, int(e.pitch)); arpR[block].hi = std::max(arpR[block].hi, int(e.pitch)); }
    }
    for (size_t i = 0; i + 1 < acidNotes.size(); ++i) {
        if (!(acidNotes[i]->flags & kNoteSlide)) continue;
        ++slides;
        if (acidNotes[i]->beat + acidNotes[i]->length <= acidNotes[i + 1]->beat) ++slideGaps;
    }
    for (size_t b = 0; b < leadR.size(); ++b) {
        if (leadR[b].hi == 0 || arpR[b].hi == 0) continue;
        ++sharedBlocks;
        if (std::min(leadR[b].hi, arpR[b].hi) - std::max(leadR[b].lo, arpR[b].lo) > 2) ++masked;
    }
    check(notes > 1000 && outside == 0, "every acid, lead and arp note in its track's scale", fmt("%d of %d outside", outside, notes));
    check(weak == 0 && arpOff == 0 && strong > 50, "lead on the strong beats tones of the tonic chord, every arp note on its chord's sus / add9 material",
          fmt("%d of %d strong lead notes off the chord, %d of %d arp notes off the material", weak, strong, arpOff, arpNotes));
    check(tooLow == 0, "depth rule in the score: acid from D3, lead from B3, arp from G3", fmt("%d notes too low", tooLow));
    check(slides > 10 && slideGaps == 0, "every acid slide overlaps the note it slides into", fmt("%d slides, %d with a gap", slides, slideGaps));
    // Since 19.09.2026 (round "voices") lead and arp share bars -- a drop carries both -- and the
    // masking rule is kept where it matters, at the sixteenth: no two line voices sound in one register
    // at the same instant (RuleRef::registerClashes, independent of Melody.cpp's guard). The bar-wide
    // overlap the rule used to be measured by is reported, not required: an arp under the lead in one
    // half of a bar and over it in the other overlaps the lead's range without ever meeting a note.
    const int clashes = RuleRef::registerClashes(ev, 0, tracks * 128);
    check(clashes == 0 && sharedBlocks > 50, "where lead and arp play together they never sound in one register at the same instant",
          fmt("%d shared bars (%d with overlapping bar ranges), %d sixteenths with two line voices closer than %d semitones",
              sharedBlocks, masked, clashes, kRegisterGap));
}

/** @brief Melody, part `.variety`: the melodic identity changes from track to track (24 plans). */
void testMelodyVariety()
{
    section("melody: variety over a night");
    // Variety over a night.
    {
        ParamStore q;
        q.parseText("compose.level_match=Off master.auto_gain=Off");
        Composer cv(4711);
        std::vector<uint64_t> acids;
        int progressions = 0, styles[kNumArpStyles] = {}, oscs[4] = {}, squelch = 0, silent = 0;
        for (int i = 0; i < 24; ++i) {
            const TrackPlan t = cv.track(q, i);
            uint64_t h = 1469598103934665603ull;
            for (const MelodyNote& n : t.melody.acid[0]) h = (h ^ static_cast<uint64_t>(n.step * 131 + n.rel + 40)) * 1099511628211ull;
            acids.push_back(h);
            if (t.melody.chordDegree[1] || t.melody.chordDegree[2] || t.melody.chordDegree[3]) ++progressions;
            ++styles[t.melody.arpStyle];
            // Since 19.09.2026 the lead's oscillator is part of its voice recipe (Composer.h, VoiceRecipe).
            if (t.voice[polyIndex(PolyInstance::Lead)].osc >= 0) ++oscs[t.voice[polyIndex(PolyInstance::Lead)].osc];
            squelch += t.melody.acidSquelch == 1 ? 1 : 0;
            if (!t.melody.present[mpIndex(MelodyPart::Acid)] && !t.melody.present[mpIndex(MelodyPart::Lead)] && !t.melody.present[mpIndex(MelodyPart::Arp)]) ++silent;
        }
        std::sort(acids.begin(), acids.end());
        const int distinct = static_cast<int>(std::unique(acids.begin(), acids.end()) - acids.begin());
        int styleCount = 0;
        for (int s : styles) styleCount += s > 0 ? 1 : 0;
        check(distinct == 24 && progressions >= 12 && styleCount >= 3 && oscs[0] > 0 && oscs[1] + oscs[2] + oscs[3] > 0 && squelch > 0 && silent == 0,
              "melodic identity changes from track to track",
              fmt("%d distinct acid riffs of 24, %d moving progressions, %d arp styles, lead osc %d/%d/%d/%d, %d squelched, %d without melody",
                  distinct, progressions, styleCount, oscs[0], oscs[1], oscs[2], oscs[3], squelch, silent));
    }
}

/** @brief Melody, part `.depthRender`: the depth rule in a rendered mix of acid, lead and arp. */
void testMelodyDepthRender()
{
    section("melody: the depth rule rendered");
    // Depth rule in the rendered mix: acid, lead and arp together put nothing under 140 Hz.
    {
        auto e = std::make_unique<Engine>();
        e->prepare(48000.0, 512);
        // Acid, lead and arp alone (19.09.2026: the pad, drone, effects, bed and voices muted too -- the
        // two-drop form puts a drop with its sub drop into this window, and the sub drop is the one effect
        // that belongs under 140 Hz; the check has always been about the three lines).
        e->params().parseText("compose.track_bars=64 compose.acid_amount=1 compose.lead_amount=1 compose.arp_amount=1 mix.kick_mute=1 mix.bass_mute=1 mix.perc_mute=1 "
                              "mix.pad_mute=1 mix.drone_mute=1 mix.sfx_mute=1 mix.texture_mute=1 mix.vocal_mute=1");
        Composer cm(3);
        const std::vector<float> y = renderEngine(*e, cm, 64.0 * kBeatsPerBar, 512, 48000.0);
        const std::vector<float> tail(y.begin() + static_cast<long>(y.size() / 2), y.end());
        double pk = 0.0;
        for (float v : tail) pk = std::max(pk, static_cast<double>(std::fabs(v)));
        const double low = lowShareDb(tail, 140.0, true);
        check(pk > 0.01 && low < -30.0, "rendered melodic parts: under -30 dB of their power below 140 Hz", fmt("%.1f dB (peak %.2f)", low, pk));
    }
}

/** @brief Melody, part `.blockSize`: block-size independence with every melodic part sounding. */
void testMelodyBlockSize()
{
    section("melody: block-size independence");
    // Block-size independence with every melodic part sounding (32-bar tracks: acid from bar 0, lead
    // and arp from bar 16), not only kick and bass as in the engine test.
    {
        std::vector<std::vector<float>> renders;
        for (int block : { 1, 77, 1000, 4096 }) {
            auto e = std::make_unique<Engine>();
            e->prepare(48000.0, block);
            // With pads, the trance gate, effects, both reverbs, the clipper and the limiter as well.
            e->params().parseText("compose.track_bars=32 compose.acid_amount=1 compose.lead_amount=1 compose.arp_amount=1 "
                                  "compose.pad_amount=1 compose.gate_chance=1 compose.sfx_amount=1");
            Composer cb(21);
            renders.push_back(renderEngine(*e, cb, 20.0 * kBeatsPerBar, block, 48000.0));
        }
        bool identical = true;
        for (size_t k = 1; k < renders.size(); ++k) identical = identical && renders[k] == renders[0];
        check(identical, "with acid, lead and arp playing: output identical for blocks of 1, 77, 1000 and 4096 samples");
    }
}

/** @brief Melody, part `.midi`: slides carry portamento, accents are loud. */
void testMelodyMidi()
{
    section("melody: acid slides and accents in MIDI");
    // MIDI: slides carry portamento, accents are loud.
    {
        Score s;
        NoteEvent a;
        a.part = Part::Acid; a.beat = 0.0; a.length = 0.28f; a.pitch = 57; a.velocity = 120; a.flags = kNoteSlide | kNoteAccent;
        s.notes.push_back(a);
        a.beat = 0.25; a.length = 0.1f; a.pitch = 60; a.velocity = 88; a.flags = 0;
        s.notes.push_back(a);
        const std::vector<uint8_t> bytes = encodeMidi(s);
        int on = 0, off = 0;
        for (size_t i = 0; i + 2 < bytes.size(); ++i) {
            if (bytes[i] == 0xB1 && bytes[i + 1] == 65 && bytes[i + 2] == 127) ++on;
            if (bytes[i] == 0xB1 && bytes[i + 1] == 65 && bytes[i + 2] == 0) ++off;
        }
        MidiFileData d;
        decodeMidi(bytes.data(), bytes.size(), d);
        bool overlap = false;
        for (const MidiTrackData& t : d.tracks)
            if (t.notes.size() == 2) overlap = t.notes[0].beat + t.notes[0].length > t.notes[1].beat && t.notes[0].velocity >= 100;
        check(on == 1 && off == 1 && overlap, "MIDI acid: slide as overlapping notes with CC 65, accent as velocity", fmt("CC65 on %d, off %d", on, off));
    }
}

// ---------------------------------------------------------------------------------------------
// Phase 4: wavetables, pads, gate, sidechain, reverb, dynamics, effects, the finished master
// ---------------------------------------------------------------------------------------------

/** @brief Self test: wavetables. */
void testWaveTable()
{
    section("wavetables");
    const double sr = 48000.0;
    // The classic table's saw frame: harmonic h at 1/h, phases of a saw.
    {
        const WaveTable& t = builtinWaveTable(0);
        std::vector<float> cyc(4096);
        for (int n = 0; n < 4096; ++n) cyc[static_cast<size_t>(n)] = t.cycle(0, 2)[n];
        std::vector<std::complex<double>> a(4096);
        for (int n = 0; n < 4096; ++n) a[static_cast<size_t>(n)] = cyc[static_cast<size_t>(n)];
        fft(a);
        double worst = 0.0;
        const double a1 = std::abs(a[1]);
        for (int h = 2; h <= 256; ++h) worst = std::max(worst, std::fabs(20.0 * std::log10(std::abs(a[static_cast<size_t>(h)]) * h / a1)));
        check(t.frames == 5 && worst < 0.01, "classic table: the saw frame has its harmonics at 1/h up to the 256th", fmt("largest deviation %.4f dB", worst));
    }
    // Anti-aliasing: a saw read at A6 from the level the note chooses, against the same saw read from
    // the full-resolution level.
    {
        const WaveTable& t = builtinWaveTable(0);
        const double f0 = 1760.0;
        auto render = [&](int level) {
            std::vector<float> y(65536 + 128);
            double ph = 0.0;
            for (size_t i = 0; i < y.size(); ++i) { y[i] = t.sample(level, 2, ph); ph += f0 / sr; if (ph >= 1.0) ph -= 1.0; }
            return y;
        };
        const int lvl = waveLevelFor(f0, sr, -1);
        const double chosen = inharmonicDb(render(lvl), 64, f0, sr);
        const double full = inharmonicDb(render(0), 64, f0, sr);
        // -55 dB: what the Catmull-Rom read leaves with eight samples per cycle of the top harmonic.
        check(chosen < -55.0 && full > chosen + 30.0, "a note reads the level whose harmonics stay under Nyquist (the full table aliases)",
              fmt("inharmonic power %.1f dB at the chosen level, %.1f dB from level 0", chosen, full));
    }
    // The same through the pad engine: every oscillator of a voice reads the level its own pitch allows.
    {
        ParamStore p;
        auto e = makePoly("pad.table=Classic pad.position=0.5 pad.pos_env=0 pad.pos_lfo_depth=0 pad.detune=0 pad.osc2=Off pad.lfo_cutoff=0 pad.lfo_pitch=0 pad.lfo_amp=0 pad.cutoff=18000 pad.amp_attack=0.3 "
                          "pad.hp_track=0 pad.hp_floor=150 pad.delay_send=0 pad.width=0", p, PolyInstance::Pad);
        e->noteOn(93, 1.0f, 8.0, 1 << 20, 0.0);   // A6
        const std::vector<float> y = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 9600 + 65536);
        // Szabo's detune polynomial is 0.003 at its zero, so the seven oscillators still spread each harmonic by
        // 0.03 % of its frequency: that much is counted as the harmonic.
        const double inh = inharmonicDb(y, 9600, midiToHz(93), 48000.0, 0.0004);
        check(inh < -55.0, "pad engine at A6: the wavetable oscillators alias no more than the table read itself", fmt("inharmonic power %.1f dB", inh));
    }
    // Vocal table: the second formant moves up from a to i.
    {
        const WaveTable& t = builtinWaveTable(1);
        auto harmonicDb = [&](int frame, int h) {
            std::vector<std::complex<double>> a(4096);
            for (int n = 0; n < 4096; ++n) a[static_cast<size_t>(n)] = t.cycle(0, frame)[n];
            fft(a);
            return 20.0 * std::log10(std::abs(a[static_cast<size_t>(h)]) * h);   // relative to the saw's 1/h
        };
        // Frame 16 of 32 lies at vowel i, frame 0 at a. The first formant falls from 730 Hz (a) to 270 Hz (i):
        // harmonic 2 of C3 (262 Hz) must grow and harmonic 6 (785 Hz) fall. (The second formant is a poor
        // witness: the third formant of a sits next to the second of i.)
        const double rise = harmonicDb(16, 2) - harmonicDb(0, 2), fall = harmonicDb(0, 6) - harmonicDb(16, 6);
        check(rise > 10.0 && fall > 10.0, "vocal table: the first formant moves from 730 Hz (a) down to 270 Hz (i)",
              fmt("harmonic 2 %+.1f dB, harmonic 6 %+.1f dB", rise, -fall));
    }
}

// ---------------------------------------------------------------------------------------------
// The measurement bench of the DSP quality round of 16.09.2026 (docs/rounds/2026-09.md): aliasing of the
// supersaw against the table saw and against two-times oversampling, the top end of both paths, and
// the aliasing of the FM and the VA oscillator at high notes. It checks nothing, it prints the tables
// the plan quotes, and it runs only when PHOS_ONLY names it.
// ---------------------------------------------------------------------------------------------

/**
 * @brief Measurement bench for the foundation probe: what it predicts against what the track plays.
 *
 * The level match gives track @em i the gain `reference - probe_i`, so after the match its measured
 * loudness is `reference - (probe_i - real_i)`: the spread that survives the match is exactly the
 * spread of the probe's error @c e_i = probe_i - real_i. This bench prints @c e_i per track over
 * several seeds, which is the quantity `testVariety`'s bound sees, and alongside it the same spread
 * with the section's own energy gain taken out -- the decomposition of 16.09.2026 that showed most of
 * what the bound used to see was the form, not the probe. Runs only when named.
 * PHOS_PROBE_LEVELS sweeps mix.perc_level on one seed instead, to see how the spread moves with the kit.
 */
void testProbeAudit()
{
    section("PROBE AUDIT BENCH");
    const bool levelSweep = std::getenv("PHOS_PROBE_LEVELS") != nullptr;
    const uint64_t seedsWide[] = { 31, 7, 2026 };
    const char* levelsOne[] = { "1", "2", "3" };
    const int cases = 3;
    for (int c = 0; c < cases; ++c) {
        const uint64_t seed = levelSweep ? 31 : seedsWide[c];
        const char* lv = levelSweep ? levelsOne[c] : "3";
        const int tracksN = levelSweep ? 4 : 6;
        const std::string knobs = fmt("compose.track_bars=128 compose.sound_variation=1 compose.track_variation=1 master.auto_gain=Off master.limiter=Off master.clipper=Off master.clip=Off master.comp_ratio=1 "
                                      "compose.acid_amount=0 compose.lead_amount=0 compose.arp_amount=0 compose.pad_amount=0 compose.sfx_amount=0 "
                                      "mix.acid_mute=1 mix.lead_mute=1 mix.arp_mute=1 mix.pad_mute=1 mix.counter_mute=1 mix.stab_mute=1 mix.drone_mute=1 mix.sfx_mute=1 mix.perc_level=%s ", lv);
        // The probes, with the level match on.
        auto pe = std::make_unique<Engine>();
        pe->prepare(48000.0, 512);
        pe->params().parseText((knobs + "compose.level_match=On").c_str());
        Composer pc(seed);
        std::vector<double> probe(static_cast<size_t>(tracksN));
        for (int t = 0; t < tracksN; ++t) probe[static_cast<size_t>(t)] = pc.track(pe->params(), t).loudness;
        // The real tracks, measured in their first core, with the match off.
        auto e = std::make_unique<Engine>();
        e->prepare(48000.0, 512);
        e->params().parseText((knobs + "compose.level_match=Off").c_str());
        Composer ce(seed);
        const TempoMap tm = ce.tempoMap(e->params(), tracksN * 128);
        e->setTempoMap(tm);
        Conductor cond(*e, ce);
        std::vector<float> L(512), R(512);
        std::vector<double> real(static_cast<size_t>(tracksN));
        for (int t = 0; t < tracksN; ++t) {
            const TrackPlan tp = ce.track(e->params(), t);
            int coreBar = 0, coreBars = 16;
            for (int i = 0; i < tp.form.count; ++i)
                if (tp.form.section[i].type == SectionType::Groove || tp.form.section[i].type == SectionType::Drop) {
                    coreBar = tp.form.section[i].startBar;
                    coreBars = tp.form.section[i].bars;
                    break;
                }
            const int window = std::min(24, coreBars - 4);
            const uint64_t a = static_cast<uint64_t>(tm.secondsAt((tp.firstBar + coreBar + 2) * 4.0) * 48000.0);
            const uint64_t b = static_cast<uint64_t>(tm.secondsAt((tp.firstBar + coreBar + 2 + window) * 4.0) * 48000.0);
            LoudnessMeter m;
            m.prepare(48000.0);
            while (e->samplePosition() < b) {
                cond.pump(e->params(), 32.0);
                const int n = static_cast<int>(std::min<uint64_t>(512, b - e->samplePosition()));
                e->process(L.data(), R.data(), n);
                if (e->samplePosition() > a) m.process(L.data(), R.data(), n);
            }
            real[static_cast<size_t>(t)] = m.read().integrated;
            const uint64_t next = static_cast<uint64_t>(tm.secondsAt(static_cast<double>(tp.firstBar + tp.bars) * 4.0) * 48000.0);
            while (e->samplePosition() < next) {
                cond.pump(e->params(), 32.0);
                e->process(L.data(), R.data(), static_cast<int>(std::min<uint64_t>(512, next - e->samplePosition())));
            }
        }
        double lo = 1e9, hi = -1e9, nlo = 1e9, nhi = -1e9;
        std::string line;
        for (int t = 0; t < tracksN; ++t) {
            const double err = probe[static_cast<size_t>(t)] - real[static_cast<size_t>(t)];
            lo = std::min(lo, err);
            hi = std::max(hi, err);
            // The loudness side of the energy arc: the real render adds energyGainDb(core energy) to the
            // track gain, the foundation probe does not. Taking it back out shows what is left.
            const TrackPlan tp = ce.track(e->params(), t);
            float energy = 0.5f;
            int coreType = 0;
            for (int i = 0; i < tp.form.count; ++i)
                if (tp.form.section[i].type == SectionType::Groove || tp.form.section[i].type == SectionType::Drop) {
                    energy = 0.5f * (tp.form.section[i].energy + tp.form.section[i].energyTo);
                    coreType = static_cast<int>(tp.form.section[i].type);
                    break;
                }
            const double arc = std::clamp((static_cast<double>(energy) - 0.7) * 5.0, -2.0, 2.0);
            nlo = std::min(nlo, err + arc);
            nhi = std::max(nhi, err + arc);
            line += fmt("  t%d %7.2f/%7.2f e%+6.2f ty%d en%.2f arc%+5.2f", t, probe[static_cast<size_t>(t)], real[static_cast<size_t>(t)], err, coreType, static_cast<double>(energy), arc);
        }
        std::printf("      spread without the energy arc: %.3f LU\n", nhi - nlo);
        std::printf("  seed %4llu perc_level %s dB:%s   spread %.3f LU\n",
                    static_cast<unsigned long long>(seed), lv, line.c_str(), hi - lo);
    }
}

/** @brief Self test: the measurement bench (prints figures, checks nothing). */
void testMeasure()
{
    section("MEASUREMENT BENCH");
    const double sr = 48000.0;
    const int kPitches[4] = { 60, 72, 84, 93 };
    const char* const kNames[4] = { "C4", "C5", "C6", "A6" };
    const std::string kWt = "pad.osc=Wavetable pad.table=Classic pad.position=0.5 pad.pos_env=0 pad.pos_lfo_depth=0 ";
    std::printf("\n  supersaw aliasing, sustained note, 65536-sample window 0.2 s after the onset\n");
    std::printf("  %-4s %-7s | %-22s | %-22s | %-22s | legit\n", "note", "detune", "lead (supersaw)", "pad path (wavetable)", "lead at 2x + halfband");
    for (double detune : { 1.0, 0.55 }) {
        const double yDet = Poly::detuneCurve(detune);
        for (int i = 0; i < 4; ++i) {
            const double f0 = midiToHz(kPitches[i]);
            const std::string lead = commonPoly("lead", detune);
            const std::string pad = kWt + commonPoly("pad", detune);
            double cv = 0.0;
            const std::vector<float> a = renderPolyNote(lead, PolyInstance::Lead, kPitches[i], 9600 + 65536, 48000.0);
            const std::vector<float> b = renderPolyNote(pad, PolyInstance::Pad, kPitches[i], 9600 + 65536, 48000.0);
            const std::vector<float> c = renderPolyNote(lead, PolyInstance::Lead, kPitches[i], 9600 + 65536, 96000.0);
            const double aa = supersawAliasDb(a, 9600, f0, yDet, sr, &cv);
            const double ab = supersawAliasDb(b, 9600, f0, yDet, sr);
            const double ac = supersawAliasDb(c, 9600, f0, yDet, sr);
            const double ia = inharmonicDb(a, 9600, f0, sr), ib = inharmonicDb(b, 9600, f0, sr), ic = inharmonicDb(c, 9600, f0, sr);
            std::printf("  %-4s %-7.2f | alias %6.1f  inh %5.1f | alias %6.1f  inh %5.1f | alias %6.1f  inh %5.1f | %.0f %%\n",
                        kNames[i], detune, aa, ia, ab, ib, ac, ic, 100.0 * cv);
        }
    }
    std::printf("\n  top end, detune 0.55: power above 8 kHz relative to 100 Hz .. 18 kHz\n");
    for (int i = 0; i < 4; ++i) {
        const double f0 = midiToHz(kPitches[i]);
        auto share = [&](const std::vector<float>& y) {
            const std::vector<double> pw = powerSpectrum(y.data() + 9600, 65536);
            double hi = 0.0, all = 0.0;
            for (size_t k = 1; k < 32768; ++k) {
                const double hz = static_cast<double>(k) * sr / 65536.0;
                if (hz < 100.0 || hz > 18000.0) continue;
                all += pw[k];
                if (hz > 8000.0) hi += pw[k];
            }
            return powDb(hi / all);
        };
        const std::vector<float> a = renderPolyNote(commonPoly("lead", 0.55), PolyInstance::Lead, kPitches[i], 9600 + 65536, 48000.0);
        const std::vector<float> b = renderPolyNote(kWt + commonPoly("pad", 0.55), PolyInstance::Pad, kPitches[i], 9600 + 65536, 48000.0);
        const int lvl = waveLevelFor(f0, sr, -1);
        std::printf("  %-4s f0 %7.1f Hz  level %d keeps %3d harmonics (to %5.0f Hz)  lead %+6.1f dB, pad path %+6.1f dB\n",
                    kNames[i], f0, lvl, WaveTable::levelHarmonics(lvl), f0 * WaveTable::levelHarmonics(lvl), share(a), share(b));
    }
    // Task 3: does the lead need 2x? The FM and VA oscillators, aliasing above 12 kHz against the fundamental.
    std::printf("\n  FM and VA lead, aliasing above 12 kHz relative to the fundamental (1x, and 2x + halfband)\n");
    struct Case { const char* name; const char* set; double ratio; };
    const Case kCases[] = {
        { "VA saw",        "lead.osc=VA lead.wave=0", 0.0 },
        { "VA pulse 25 %", "lead.osc=VA lead.wave=1 lead.pulse_width=0.25", 0.0 },
        { "FM I=2.5 r=2",  "lead.osc=FM lead.fm_index=2.5 lead.fm_ratio=2 lead.fm_decay=2000", 2.0 },
        { "FM I=10 r=2",   "lead.osc=FM lead.fm_index=10 lead.fm_ratio=2 lead.fm_decay=2000", 2.0 },
        { "FM I=2.5 r=3.5","lead.osc=FM lead.fm_index=2.5 lead.fm_ratio=3.5 lead.fm_decay=2000", 3.5 },
        { "FM I=10 r=3.5", "lead.osc=FM lead.fm_index=10 lead.fm_ratio=3.5 lead.fm_decay=2000", 3.5 },
        { "FM I=10 r=7.5", "lead.osc=FM lead.fm_index=10 lead.fm_ratio=7.5 lead.fm_decay=2000", 7.5 },
    };
    for (const Case& c : kCases) {
        std::printf("  %-14s", c.name);
        for (int i = 1; i < 4; ++i) {
            const double f0 = midiToHz(kPitches[i]);
            const std::string s = std::string(c.set) + " " + commonPoly("lead", 0.0);
            const std::vector<double> lines = c.ratio > 0.0 ? fmLines(f0, c.ratio, sr) : harmonicLines(f0, sr);
            const std::vector<float> a = renderPolyNote(s, PolyInstance::Lead, kPitches[i], 9600 + 65536, 48000.0);
            const std::vector<float> b = renderPolyNote(s, PolyInstance::Lead, kPitches[i], 9600 + 65536, 96000.0);
            std::printf("  %s 1x %6.1f / 2x %6.1f", kNames[i], lineAliasDb(a, 9600, lines, f0, sr, 12000.0), lineAliasDb(b, 9600, lines, f0, sr, 12000.0));
        }
        std::printf("\n");
    }
    // The same, with the voice filter where the lead really stands (cutoff 10 kHz, the Phase-4 value).
    std::printf("\n  the same at the lead's own cutoff of 10 kHz (what actually reaches the mix)\n");
    for (const Case& c : kCases) {
        std::printf("  %-14s", c.name);
        for (int i = 1; i < 4; ++i) {
            const double f0 = midiToHz(kPitches[i]);
            std::string s = std::string(c.set) + " " + commonPoly("lead", 0.0) + " lead.cutoff=10000";
            const std::vector<double> lines = c.ratio > 0.0 ? fmLines(f0, c.ratio, sr) : harmonicLines(f0, sr);
            const std::vector<float> a = renderPolyNote(s, PolyInstance::Lead, kPitches[i], 9600 + 65536, 48000.0);
            std::printf("  %s 1x %6.1f", kNames[i], lineAliasDb(a, 9600, lines, f0, sr, 12000.0));
        }
        std::printf("\n");
    }
}

// ---------------------------------------------------------------------------------------------
// Phase "wavetable library" (16.09.2026): the shipped `.phoswt`, its reader, and the promise that
// the parameter's first six indices never move.
// ---------------------------------------------------------------------------------------------

/** @brief Self test: wavetable library (.phoswt). */
void testWaveTableLibrary()
{
    section("wavetable library (.phoswt)");
    const double sr = 48000.0;

    // What the shipped tables must measure. Generated by `python Tools/wt_pack.py --reference`,
    // which computes them in Python **from the source .wav files of Noctuary's library** and from
    // its own model of the level build and the Catmull-Rom read -- never from the pack this test
    // reads. Two independent paths therefore have to agree: Python from the WAV, and C++ from the
    // packed coefficients through buildFromHarmonics().
    struct LibraryRef { const char* name; int frames; double rms0; double aliasC5; double aliasC6; };
    // The block itself is generated (`python Tools/wt_pack.py --reference`) -- 464 tables since
    // 22.09.2026, which is no longer something to keep by hand -- and the static_assert below is
    // what makes a stale one a build error rather than a quiet half-check.
#include "WaveTableRef.inl"

    static_assert(sizeof(kLibraryRef) / sizeof(kLibraryRef[0]) == kNumLibraryWaveTables,
                  "the reference block and the shipped selection have come apart");

    // 22.09.2026: the pack is only *indexed* at load now -- a table is expanded into its mip levels
    // when a track that uses it is planned (WaveTableFile.h, ensureWaveTables), which is what lets
    // the selection be 464 tables instead of 35. So every check below that wants to look at a real
    // table has to ask for it first, exactly as the composer does. Expanding all 464 at once would
    // be 900 MB, so the ones that walk the whole library walk it in chunks and reload between them.
    const auto expand = [](int first, int count) {
        std::vector<int> want;
        for (int i = first; i < first + count && i < kNumWaveTables; ++i) want.push_back(i);
        if (!want.empty()) ensureWaveTables(want.data(), static_cast<int>(want.size()));
    };
    constexpr int kChunk = 32;   // 32 tables is about 62 MB, well inside the default budget

    // The compatibility contract, first, because it is the one a saved set depends on: the six
    // built-in tables keep indices 0..5 and the library begins at 6. A `.phosset` and a plugin
    // state store the index as a number ("lead.table=1"), so moving the six would silently change
    // the sound of every set ever saved.
    {
        int wrong = 0;
        for (int i = 0; i < kNumBuiltinWaveTables; ++i) if (&waveTable(i) != &builtinWaveTable(i)) ++wrong;
        ParamStore p;
        const int id = p.find("lead.table");
        p.parseText("lead.table=1");
        const bool one = p.format(id) == std::string("Vocal");
        p.parseText("lead.table=Sync");
        const bool sync = p.getInt(id) == 4;
        p.parseText("pad.table=5");
        const bool five = p.format(p.find("pad.table")) == std::string("Formant Saw");
        check(wrong == 0 && one && sync && five && kNumWaveTables > kNumBuiltinWaveTables,
              "old sets: indices 0..5 are still the six built-in tables and the library begins at 6",
              fmt("%d of the six moved; \"1\" reads %s, \"Sync\" reads %d, \"5\" reads %s; %d tables in all",
                  wrong, one ? "Vocal" : "something else", p.getInt(id), five ? "Formant Saw" : "something else", kNumWaveTables));
    }

    // The fallback, before anything is loaded: a set saved on a machine with the library must still
    // play on one without it, and it must play the built-in the descriptor names rather than
    // whatever happens to sit at index 0.
    {
        resetWaveTableLibrary();
        int wrong = 0;
        for (int i = 0; i < kNumLibraryWaveTables; ++i) {
            if (&waveTable(kNumBuiltinWaveTables + i) != &builtinWaveTable(kLibraryTables[i].fallback)) ++wrong;
            if (waveTableLoaded(kNumBuiltinWaveTables + i)) ++wrong;
        }
        check(wrong == 0 && waveTableLibraryBytes() == 0,
              "no library file: every library index falls back to the built-in its descriptor names",
              fmt("%d of %d wrong, %zu bytes held", wrong, kNumLibraryWaveTables, waveTableLibraryBytes()));
    }

    // A broken file must leave nothing half-built. The pack is copied with its end marker cut off,
    // which is the error a truncated download always makes.
    {
        std::vector<char> good;
        {
            std::ifstream f(std::string(PHOS_SOURCE_DATA_DIR) + "/library.phoswt", std::ios::binary);
            good.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        const std::string cut = (std::filesystem::temp_directory_path() / "phos_broken.phoswt").string();
        {
            std::ofstream f(cut, std::ios::binary);
            if (good.size() > 9) f.write(good.data(), static_cast<std::streamsize>(good.size() - 9));
        }
        resetWaveTableLibrary();
        std::string error;
        const int n = loadWaveTableLibrary(cut.c_str(), &error);
        int fellBack = 0;
        for (int i = 0; i < kNumLibraryWaveTables; ++i)
            if (&waveTable(kNumBuiltinWaveTables + i) == &builtinWaveTable(kLibraryTables[i].fallback)) ++fellBack;
        std::error_code ec;
        std::filesystem::remove(cut, ec);
        check(!good.empty() && n == 0 && !error.empty() && fellBack == kNumLibraryWaveTables,
              "a truncated pack loads nothing at all and says why, rather than half a library",
              fmt("%d tables, %d of %d fell back, error \"%s\"", n, fellBack, kNumLibraryWaveTables, error.c_str()));
    }

    // The frame limit (the Quest lever): frames thinned evenly, both ends kept. Checked before the
    // real load, because the limit is read once, when the library is built.
    {
        resetWaveTableLibrary();
        setWaveTableFrameLimit(16);
        const int n = loadWaveTableLibrary();
        expand(kNumBuiltinWaveTables, 1);
        std::vector<float> first, last;
        int frames = 0;
        {
            const WaveTable& a = waveTable(kNumBuiltinWaveTables);
            frames = a.frames;
            first.assign(a.cycle(0, 0), a.cycle(0, 0) + WaveTable::levelLength(0));
            last.assign(a.cycle(0, frames - 1), a.cycle(0, frames - 1) + WaveTable::levelLength(0));
        }
        resetWaveTableLibrary();
        setWaveTableFrameLimit(0);
        loadWaveTableLibrary();
        expand(kNumBuiltinWaveTables, 1);
        const WaveTable& b = waveTable(kNumBuiltinWaveTables);
        // Not bit-equal, and not even equal in level: the whole table is scaled by its loudest
        // frame, and thinning can drop that frame -- on a lead-lane table, whose frames travel a long
        // way in brightness, that is a decibel or so. The claim is about the *wave*, so each end is
        // compared after its own RMS is divided out. (Until 22.09.2026 this compared raw samples
        // against a thousandth of full scale, which held only because the table at index 6 happened
        // to be a pad-lane one whose loudest frame survived the thinning.)
        const auto shapeDiff = [](const std::vector<float>& x, const float* y, int len) {
            double px = 0.0, py = 0.0;
            for (int i = 0; i < len; ++i) { px += x[static_cast<size_t>(i)] * x[static_cast<size_t>(i)]; py += y[i] * y[i]; }
            if (px <= 0.0 || py <= 0.0) return 1.0;
            const double gx = 1.0 / std::sqrt(px), gy = 1.0 / std::sqrt(py);
            double worst = 0.0;
            for (int i = 0; i < len; ++i)
                worst = std::max(worst, std::fabs(x[static_cast<size_t>(i)] * gx - y[i] * gy));
            return worst * std::sqrt(static_cast<double>(len));
        };
        const double dFirst = shapeDiff(first, b.cycle(0, 0), WaveTable::levelLength(0));
        const double dLast = shapeDiff(last, b.cycle(0, b.frames - 1), WaveTable::levelLength(0));
        check(n == kNumLibraryWaveTables && frames == 16 && b.frames == 64 && dFirst < 1e-3 && dLast < 1e-3,
              "the frame limit thins evenly and keeps both ends of the table",
              fmt("%d frames at the limit, %d without it; ends differ by %.2e / %.2e", frames, b.frames, dFirst, dLast));
    }

    // The library itself, against the reference block.
    {
        std::string error;
        int n = 0;
        int badFrames = 0, badRms = 0, badAlias = 0, badName = 0;
        double worstRms = 0.0, worstAlias = 0.0;
        for (int i = 0; i < kNumLibraryWaveTables; ++i) {
            if (i % kChunk == 0) {                      // a fresh library, so the memory does not pile up
                resetWaveTableLibrary();
                setWaveTableFrameLimit(0);
                n = loadWaveTableLibrary(nullptr, &error);
                expand(kNumBuiltinWaveTables + i, kChunk);
            }
            const LibraryRef& r = kLibraryRef[i];
            const WaveTable& t = waveTable(kNumBuiltinWaveTables + i);
            if (t.frames != r.frames) ++badFrames;
            const double rms = tableFrameRms(t, 0, 0);
            worstRms = std::max(worstRms, std::fabs(rms - r.rms0));
            if (std::fabs(rms - r.rms0) > 1e-3) ++badRms;
            for (const double hz : { 523.2511, 1046.502 }) {
                const double want = hz < 700.0 ? r.aliasC5 : r.aliasC6;
                const double got = tableFrameAliasDb(t, 0, hz, sr);
                worstAlias = std::max(worstAlias, std::fabs(got - want));
                if (std::fabs(got - want) > 1.5) ++badAlias;
            }
            if (std::strcmp(kLibraryTables[i].name, r.name) != 0) ++badName;
        }
        check(n == kNumLibraryWaveTables && badFrames == 0 && badRms == 0 && badAlias == 0 && badName == 0,
              "the pack decodes to the tables Python measured from the source files",
              fmt("%d tables; %d wrong frame counts, %d names, %d RMS (worst %.2e), %d aliasing (worst %.2f dB)",
                  n, badFrames, badName, badRms, worstRms, badAlias, worstAlias));
    }

    // A loaded table and a built-in one are the same object downstream. The proof is the table's
    // own contract: buildFromHarmonics() scales the loudest frame to kTargetRms, and the library
    // goes through exactly that call, so the loudest frame of every shipped table must land there.
    {
        double worst = 0.0;
        for (int i = 0; i < kNumLibraryWaveTables; ++i) {
            if (i % kChunk == 0) {
                resetWaveTableLibrary();
                setWaveTableFrameLimit(0);
                loadWaveTableLibrary();
                expand(kNumBuiltinWaveTables + i, kChunk);
            }
            const WaveTable& t = waveTable(kNumBuiltinWaveTables + i);
            double loudest = 0.0;
            for (int f = 0; f < t.frames; ++f) loudest = std::max(loudest, tableFrameRms(t, 0, f));
            worst = std::max(worst, std::fabs(loudest - static_cast<double>(WaveTable::kTargetRms)));
        }
        check(worst < 1e-4, "a loaded table carries the same normalisation a built-in one does",
              fmt("the loudest frame is off kTargetRms by at most %.2e", worst));
    }

    // The supersaw does not read the library, whatever the table parameter says: Poly keeps a
    // second pointer at the Classic table for it (Poly.h), and the DSP round's aliasing figures
    // depend on that frame and no other.
    {
        const std::string common = "pad.osc=Supersaw pad.detune=0 pad.mix=0 pad.osc2=Off pad.lfo_cutoff=0 pad.lfo_pitch=0 pad.lfo_amp=0 pad.cutoff=18000 pad.env_amount=0 pad.resonance=0 "
                                   "pad.hp_track=0 pad.hp_floor=150 pad.delay_send=0 pad.width=0 pad.amp_attack=0.3 pad.amp_sustain=1 ";
        auto render = [&](const char* table) {
            ParamStore q;
            auto e = makePoly((common + table).c_str(), q, PolyInstance::Pad);
            e->seedPhases(0x5AFE5AFEull);
            e->noteOn(69, 1.0f, 8.0, 1 << 20, 0.0);
            return renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 4096);
        };
        expand(6, 2);
        const std::vector<float> a = render("pad.table=Classic"), b = render("pad.table=6");
        int bad = 0;
        double energy = 0.0;
        for (size_t i = 0; i < a.size(); ++i) { if (a[i] != b[i]) ++bad; energy += a[i] * a[i]; }
        check(bad == 0 && energy > 1.0, "the supersaw reads the Classic saw frame whatever the table parameter says",
              fmt("%d differing samples, energy %.1f", bad, energy));
    }

    // And the wavetable oscillator does read it: the same note on two library tables must not be
    // the same sound, and the pad must alias no more than the table read itself allows.
    {
        const std::string common = "pad.osc=Wavetable pad.position=0 pad.pos_env=0 pad.pos_lfo_depth=0 pad.detune=0 pad.mix=0 "
                                   "pad.osc2=Off pad.lfo_cutoff=0 pad.lfo_pitch=0 pad.lfo_amp=0 pad.cutoff=18000 pad.env_amount=0 pad.resonance=0 pad.hp_track=0 pad.hp_floor=150 "
                                   "pad.delay_send=0 pad.width=0 pad.amp_attack=0.3 pad.amp_sustain=1 ";
        auto render = [&](const char* table, int pitch, size_t n) {
            ParamStore q;
            auto e = makePoly((common + table).c_str(), q, PolyInstance::Pad);
            e->seedPhases(0x5AFE5AFEull);
            e->noteOn(pitch, 1.0f, 8.0, 1 << 20, 0.0);
            return renderMono([&](float* L, float* R, int n2) { e->process(L, R, n2); }, n);
        };
        expand(6, 2);   // makePoly builds a bare Poly; nothing else here asks for these two
        const std::vector<float> a = render("pad.table=6", 69, 4096), b = render("pad.table=7", 69, 4096);
        double same = 0.0, ea = 0.0;
        for (size_t i = 0; i < a.size(); ++i) { same += (a[i] - b[i]) * (a[i] - b[i]); ea += a[i] * a[i]; }
        const std::vector<float> c = render("pad.table=6", 84, 9600 + 65536);   // C6
        const double inh = inharmonicDb(c, 9600, midiToHz(84), sr, 0.0004);
        check(ea > 1.0 && same > 0.1 * ea && inh < -55.0,
              "the pad plays the library table it is pointed at, and aliases no more than the read allows",
              fmt("difference %.1f dB of the signal, inharmonic power %.1f dB at C6", powDb(same / ea), inh));
    }

    // What the library costs, and what the two formats cost to load. Not thresholds -- the numbers
    // the format decision in docs/rounds/2026-09.md rests on. The `.wav` side is only measured where
    // Noctuary's library is on the machine: PHOS_WT_SOURCE names its Wavetables directory.
    {
        // 22.09.2026: the question this block answers changed with the library. It used to be "what
        // do the 35 tables cost once they are all expanded at load", and the answer was a constant,
        // 66 MB. The pack now holds 464, which expanded in full would be about 900 MB -- so they are
        // not expanded in full, and the number that matters is what a *track* costs: the composer
        // asks for one table per voice while it plans, six at a time (Composer.cpp, makeTrack).
        resetWaveTableLibrary();
        setWaveTableFrameLimit(0);
        const auto t0 = std::chrono::steady_clock::now();
        const int n = loadWaveTableLibrary();
        const double packMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const size_t idle = waveTableLibraryBytes();
        const auto t1 = std::chrono::steady_clock::now();
        expand(kNumBuiltinWaveTables, 6);
        const double trackMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        const size_t track = waveTableLibraryBytes();
        expand(kNumBuiltinWaveTables + 6, kChunk - 6);
        const size_t chunk = waveTableLibraryBytes();
        int frames = 0;
        for (int i = 0; i < kChunk; ++i) frames += waveTable(kNumBuiltinWaveTables + i).frames;
        const double perTable = static_cast<double>(chunk) / kChunk;
        std::error_code ec;
        const double packMb = static_cast<double>(std::filesystem::file_size(
            std::string(PHOS_SOURCE_DATA_DIR) + "/library.phoswt", ec)) / (1024.0 * 1024.0);
        std::printf("  %d library tables in the pack (%.2f MB, indexed in %.1f ms, %zu bytes expanded);"
                    " a track's six voices cost %.2f MB and %.0f ms, a table %.2f MB,"
                    " the whole library would be %.0f MB\n",
                    kNumLibraryWaveTables, packMb, packMs, idle, track / (1024.0 * 1024.0), trackMs,
                    perTable / (1024.0 * 1024.0), perTable * kNumLibraryWaveTables / (1024.0 * 1024.0));
        const char* src = std::getenv("PHOS_WT_SOURCE");
        if (src != nullptr && src[0] != 0) {
            const auto t2 = std::chrono::steady_clock::now();
            int read = 0;
            size_t wavBytes = 0;
            for (int i = 0; i < kChunk; ++i) {
                const std::string q = std::string(src) + "/" + kLibraryTables[i].id + ".wav";
                std::vector<std::vector<std::complex<double>>> coeffs;
                int cycleLen = 0;
                if (!readWaveTableWav(q.c_str(), coeffs, cycleLen)) continue;
                wavBytes += static_cast<size_t>(std::filesystem::file_size(q, ec));
                WaveTable t;
                if (t.buildFromHarmonics(coeffs)) ++read;
            }
            const double wavMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();
            std::printf("  the same %d tables from the source .wav files: %.2f MB on disk, %.1f ms\n",
                        read, wavBytes / (1024.0 * 1024.0), wavMs);
        }
        // Three numbers, and the middle one is the point. A load that expands nothing holds nothing
        // (the pack's own bytes are kept, and are not counted here). A track's six voices have to
        // stay small enough that planning one is not an allocation event -- 16 MB is about 1.4x the
        // 11.6 MB six unthinned tables actually measure, the same kind of headroom the old ceiling
        // carried. And a table must still cost about what it always did: the on-demand path goes
        // through the very same buildFromHarmonics(), so a table that suddenly costs half as much
        // would mean frames were being dropped somewhere.
        check(n == kNumLibraryWaveTables && idle == 0 && track > 0 && track < 16u * 1024u * 1024u
              && perTable > 1.0e6 && perTable < 3.0e6,
              "the library's memory is what the plan says it is: nothing until a track asks, then a table at a time",
              fmt("%zu bytes idle, %zu for a track's six voices, %.0f bytes a table", idle, track, perTable));
    }

    // The ceiling (WaveTableFile.h, setWaveTableBudgetBytes). Nothing is ever freed -- a prepared
    // Poly holds a raw pointer into a table -- so the bound has to be at the asking end: once the
    // built tables reach the budget a further one is simply not built, and the voice that asked for
    // it sounds its built-in fallback, which is the same path a machine without the pack takes.
    {
        resetWaveTableLibrary();
        setWaveTableFrameLimit(0);
        loadWaveTableLibrary();
        setWaveTableBudgetBytes(8u * 1024u * 1024u);   // room for four tables or so
        expand(kNumBuiltinWaveTables, 64);
        const size_t held = waveTableLibraryBytes();
        const int built = waveTablesBuilt();
        // Every index still answers, and the ones that were refused answer with their fallback.
        int wrong = 0;
        for (int i = 0; i < 64; ++i) {
            const int idx = kNumBuiltinWaveTables + i;
            const WaveTable& t = waveTable(idx);
            if (!waveTableLoaded(idx) && &t != &builtinWaveTable(kLibraryTables[i].fallback)) ++wrong;
        }
        setWaveTableBudgetBytes(192u * 1024u * 1024u);
        check(built > 0 && built < 64 && held <= 10u * 1024u * 1024u && wrong == 0,
              "the memory ceiling holds: past it a table is not built and its voice falls back",
              fmt("%d of 64 built, %.2f MB held against an 8 MB ceiling, %d indices without a table to answer with",
                  built, held / (1024.0 * 1024.0), wrong));
    }
}

// ---------------------------------------------------------------------------------------------
// 20.09.2026, round "threadsafe-loaders": loadWaveTableLibrary() and loadVoicePack() used to guard
// their one real load with a bare bool, not a lock (Probe.h's warmSharedData() comment names the
// consequence: the first worker set the flag and kept parsing while the others, told "already
// attempted," rendered with the built-in fallback tables / no phrases at all). Both now gate on a
// call-once pattern instead (an atomic flag checked without a lock once it is set, a mutex around the
// load for whichever threads arrive first) -- the same shape sharedMelodyModel()/sharedBassModel()
// (Model.cpp) and installSearchPaths() (PluginProcessor.cpp) already use, adapted because, unlike
// those, resetWaveTableLibrary()/resetVoicePack() have to open the gate again for the next test.
// ---------------------------------------------------------------------------------------------

/**
 * @brief Many threads call a shared-data loader for the very first time at once, repeatedly, and every
 *        one of them must see the finished load -- never the "already attempted, but the real load is
 *        still in flight" window a bare flag left open.
 *
 * One thread loads first, outside any race, to learn what a clean load returns; every threaded call in
 * every round must then return exactly that, and it must do so without `probe::warmSharedData()`
 * having run first (that pre-warm is a scheduling optimisation now, not what makes this correct -- see
 * docs/rounds/2026-09.md). Seen to FAIL before the fix, reliably, not just occasionally: with the bare
 * `bool attempted`, three runs against the pre-fix source each found ~450 of the 480 raced calls
 * (16 threads x 30 rounds) coming back empty against a positive reference, for both loaders. Two
 * threads racing past a bare flag and both calling parsePack()/parse() -- writing `Library::entry[]`
 * or `Pack::phrases` at once -- is also what the ASan pass is there to catch.
 */
void testLoaderThreadSafety()
{
    section("shared-data loaders are thread-safe at the source (WaveTableFile.cpp, Vocal.cpp)");
    const unsigned hw = std::thread::hardware_concurrency();
    const int threadsPerRound = static_cast<int>(std::clamp(hw == 0 ? 8u : hw, 4u, 16u));
    constexpr int kRounds = 30;

    auto hammer = [&](const char* what, const std::function<void()>& reset, const std::function<int()>& load) {
        reset();
        const int reference = load();
        int mismatches = 0, cameBackEmpty = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int round = 0; round < kRounds; ++round) {
            reset();
            // A start gate: every thread spins on it so its first loadXxx() call lands as close to the
            // others as the scheduler allows. Threads started one after another would mostly just queue
            // up on the loader's mutex and never exercise the race the fix removed.
            std::atomic<bool> go{ false };
            std::vector<int> results(static_cast<size_t>(threadsPerRound), -1);
            std::vector<std::thread> threads;
            threads.reserve(static_cast<size_t>(threadsPerRound));
            for (int t = 0; t < threadsPerRound; ++t)
                threads.emplace_back([&, t] {
                    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                    results[static_cast<size_t>(t)] = load();
                });
            go.store(true, std::memory_order_release);
            for (std::thread& th : threads) th.join();
            for (int r : results) {
                if (r != reference) ++mismatches;
                if (r == 0 && reference != 0) ++cameBackEmpty;
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(reference > 0 && mismatches == 0,
              fmt("%s: %d threads x %d rounds of a fresh first load, every call agrees", what, threadsPerRound, kRounds).c_str(),
              fmt("reference %d; %d of %d raced calls disagreed (%d came back empty); %.0f ms", reference, mismatches,
                  threadsPerRound * kRounds, cameBackEmpty, ms));
    };

    hammer("wavetable library", [] { resetWaveTableLibrary(); }, [] { return loadWaveTableLibrary(); });
    hammer("voice pack", [] { resetVoicePack(); }, [] { return loadVoicePack(); });
}

// ---------------------------------------------------------------------------------------------
// Phase "wavetable library", Nachtrag (16.09.2026): the Quest lever. What thinning a table costs
// in the measure the selection was made with, and that Engine::prepare() pulls the lever -- which
// is the path the plugin and the Quest app take, not setWaveTableFrameLimit() by hand.
// ---------------------------------------------------------------------------------------------

/** @brief Self test: wavetable library: the Quest lever. */
void testWaveTableQuality()
{
    section("wavetable library: the Quest lever");

    // What thinning costs, in the measure the selection was made with. Thinning cannot hurt
    // `directness` -- the first and the last frame stay, so `travel` is unchanged, and `path` can
    // only shrink (triangle inequality), so directness can only rise. The number that gets worse is
    // the *step*: the position knob reads two neighbouring frames and crossfades between them, so
    // the step between them is the grain of the morph, and a table that steps instead of gliding is
    // exactly what the selection's `directness` gate threw out. The bound comes from the same
    // place: the WaveEdit banks the first selection wrongly chose for the pad had a median step of
    // 0.49 and up, and thinning must not walk a chosen pad table into that range.
    //
    // The bound is the **pad lane's** and no other. `directness` was never asked of the lead or the
    // arp -- a sixteenth note is over before a sweep arrives (docs/rounds/2026-09.md, 16.09.2026) -- and three
    // of those seven tables step by 0.5 and more at every frame count, thinned or not. Holding them
    // to a gliding table's bound would measure the selection, not the thinning.
    constexpr double kShakerMove = 0.49;
    const int kLimits[] = { 0, 48, 32, 24, 16 };
    std::map<int, std::vector<MorphMeasure>> byLimit;
    for (int limit : kLimits) {
        resetWaveTableLibrary();
        setWaveTableFrameLimit(limit);
        loadWaveTableLibrary();
        std::vector<MorphMeasure> ms;
        for (int i = 0; i < kNumLibraryWaveTables; ++i) ms.push_back(measureMorph(waveTable(kNumBuiltinWaveTables + i)));
        byLimit[limit] = std::move(ms);
    }
    std::printf("  what the frame limit costs the morph (median step / largest single step, per frame limit)\n");
    std::printf("  %-22s %-5s", "table", "lane");
    for (int limit : kLimits) std::printf("  %11s", limit == 0 ? "64 (all)" : fmt("%d", limit).c_str());
    std::printf("\n");
    for (int i = 0; i < kNumLibraryWaveTables; ++i) {
        const char* lane = kLibraryTables[i].lane == WaveTableLane::Pad ? "pad"
                         : (kLibraryTables[i].lane == WaveTableLane::Lead ? "lead"
                         : (kLibraryTables[i].lane == WaveTableLane::Arp ? "arp" : "drone"));
        std::printf("  %-22s %-5s", kLibraryTables[i].name, lane);
        for (int limit : kLimits) {
            const MorphMeasure& m = byLimit[limit][static_cast<size_t>(i)];
            std::printf("  %.3f/%.3f", m.move, m.moveMax);
        }
        std::printf("\n");
    }
    {
        double worstMove = 0.0, wholeMove = 0.0, worstDirect = 1.0, wholeDirect = 1.0, worst16 = 0.0;
        const char* worstName = "";
        for (int i = 0; i < kNumLibraryWaveTables; ++i) {
            if (kLibraryTables[i].lane != WaveTableLane::Pad) continue;
            const MorphMeasure& m = byLimit[32][static_cast<size_t>(i)];
            const MorphMeasure& all = byLimit[0][static_cast<size_t>(i)];
            if (m.move > worstMove) { worstMove = m.move; wholeMove = all.move; worstName = kLibraryTables[i].name; }
            worst16 = std::max(worst16, byLimit[16][static_cast<size_t>(i)].move);
            worstDirect = std::min(worstDirect, m.directness);
            wholeDirect = std::min(wholeDirect, all.directness);
        }
        check(worstMove < kShakerMove && worstDirect >= wholeDirect - 1e-9,
              "at 32 frames the pad tables still glide: no step near the ones the selection rejected",
              fmt("worst median pad step %.3f (%s, %.3f with all 64 frames) against the %.2f of a rejected bank;"
                  " 16 frames would make it %.3f. Worst pad directness %.3f, and %.3f unthinned",
                  worstMove, worstName, wholeMove, kShakerMove, worst16, worstDirect, wholeDirect));
    }

    // The lever itself, through the path a host takes: Engine::prepare(sr, block, quality). The
    // Quest level thins, the desktop level does not, and the first and the last frame stay -- the
    // ends of the table are what a position sweep starts and finishes on.
    {
        std::vector<float> first, last;
        int desktopFrames = 0, questFrames = 0;
        size_t desktopBytes = 0, questBytes = 0;
        double desktopMs = 0.0, questMs = 0.0;
        {
            resetWaveTableLibrary();
            setWaveTableFrameLimit(0);
            Engine e;
            const auto t0 = std::chrono::steady_clock::now();
            e.prepare(48000.0, 256, Quality::desktop());
            desktopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            // The frame limit is read when a table is built, and since 22.09.2026 that is when
            // somebody asks. prepare() asked for whatever the parameters name; this check is about
            // the *limit*, so it asks for a known library table itself.
            const int one = kNumBuiltinWaveTables;
            ensureWaveTables(&one, 1);
            const WaveTable& t = waveTable(kNumBuiltinWaveTables);
            desktopFrames = t.frames;
            desktopBytes = waveTableLibraryBytes();
            const int len = WaveTable::levelLength(0);
            first.assign(t.cycle(0, 0), t.cycle(0, 0) + len);
            last.assign(t.cycle(0, t.frames - 1), t.cycle(0, t.frames - 1) + len);
        }
        double dFirst = 0.0, dLast = 0.0;
        {
            resetWaveTableLibrary();
            setWaveTableFrameLimit(0);
            Engine e;
            const auto t0 = std::chrono::steady_clock::now();
            e.prepare(48000.0, 256, Quality::quest());
            questMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const int one = kNumBuiltinWaveTables;
            ensureWaveTables(&one, 1);
            const WaveTable& t = waveTable(kNumBuiltinWaveTables);
            questFrames = t.frames;
            questBytes = waveTableLibraryBytes();
            // Shapes, not samples: thinning can drop the loudest frame and rescale the table
            // (the same comparison the library section's frame-limit check uses, 22.09.2026).
            const auto shape = [](const std::vector<float>& x, const float* y, int len) {
                double px = 0.0, py = 0.0;
                for (int i = 0; i < len; ++i) { px += x[static_cast<size_t>(i)] * x[static_cast<size_t>(i)]; py += y[i] * y[i]; }
                if (px <= 0.0 || py <= 0.0) return 1.0;
                const double gx = 1.0 / std::sqrt(px), gy = 1.0 / std::sqrt(py);
                double worst = 0.0;
                for (int i = 0; i < len; ++i)
                    worst = std::max(worst, std::fabs(x[static_cast<size_t>(i)] * gx - y[i] * gy));
                return worst * std::sqrt(static_cast<double>(len));
            };
            dFirst = shape(first, t.cycle(0, 0), WaveTable::levelLength(0));
            dLast = shape(last, t.cycle(0, t.frames - 1), WaveTable::levelLength(0));
        }
        // Not bit-equal: the table is scaled by its loudest frame and thinning can drop that frame
        // (the same tolerance the frame-limit check of the library section uses).
        check(desktopFrames == WaveTable::kMaxFrames && questFrames == 32 && dFirst < 1e-3 && dLast < 1e-3,
              "Engine::prepare pulls the lever: the Quest level thins to 32 frames, the desktop level keeps all 64",
              fmt("desktop %d frames, quest %d; the kept ends differ by %.2e / %.2e", desktopFrames, questFrames, dFirst, dLast));
        check(questBytes * 3 < desktopBytes * 2,
              "and the Quest level really costs the memory back",
              fmt("%.2f MB against %.2f MB, loaded in %.0f ms against %.0f ms",
                  questBytes / (1024.0 * 1024.0), desktopBytes / (1024.0 * 1024.0), questMs, desktopMs));
        std::printf("  quality levels: desktop %d frames, %.2f MB, Engine::prepare %.0f ms;"
                    " quest %d frames, %.2f MB, %.0f ms\n",
                    desktopFrames, desktopBytes / (1024.0 * 1024.0), desktopMs,
                    questFrames, questBytes / (1024.0 * 1024.0), questMs);
    }

    // Whatever this section did to the library, every test after it must see the shipped one.
    resetWaveTableLibrary();
    setWaveTableFrameLimit(0);
    loadWaveTableLibrary();
}

/** @brief Self test: pads: voicings. */
void testPads()
{
    section("pads: voicings");
    // Voice leading against an independent enumeration (rewritten 22.09.2026, round "Harmonik": chord
    // types and open voicings). The rule: the chord root lowest in D3 .. C#4, the type's fifth (the
    // tritone for m(b5)) directly over it, the colour tones above at whatever octave keeps adjacent
    // voices a minor third to an octave apart and the top at or under G5; as many of the colour tones
    // as fit that way, and among those placements the one that moves least from the voicing before.
    // The enumeration here walks every pitch over the fifth with a colour tone's pitch class, which is
    // not how voiceChord searches (it steps octaves from a base position), so the two agree only if
    // the rule is really what both implement.
    Rng r;
    r.seed(99);
    int mismatches = 0, badNotes = 0, cases = 0;
    for (int trial = 0; trial < 300; ++trial) {
        const int scale = r.below(kNumScales), key = r.below(12);
        const int prevDegree = r.below(7), prevType = r.below(kNumChordTypes);
        const std::vector<int> prev = voiceChord(scale, key, prevDegree, prevType, nullptr);
        const int degree = r.below(7), type = r.below(kNumChordTypes);
        const std::vector<int> got = voiceChord(scale, key, degree, type, &prev);
        // The chord the pad may hold (Harmony.h, padChordIntervals): no b9, the bII as the tonic's minor sixth.
        int iv[4], rootSemis = 0;
        const int n = padChordIntervals(static_cast<ChordType>(type), scale, degree, rootSemis, iv);
        const int rootPc = (key + rootSemis) % 12;
        const int root = kPadLowest + ((rootPc - kPadLowest) % 12 + 12) % 12;
        const int fifth = root + iv[0];
        std::vector<std::vector<int>> cand;
        for (int i = 1; i < n; ++i) {
            std::vector<int> c;
            for (int x = fifth + 1; x <= kPadHighest; ++x)
                if (((x - root) % 12 + 12) % 12 == ((iv[i] % 12) + 12) % 12) c.push_back(x);
            cand.push_back(c);
        }
        int best = 1 << 30;
        size_t bestSize = 0;
        for (int use = n - 1; use >= 0 && bestSize == 0; --use) {
            bool feasible = true;
            for (int i = 0; i < use; ++i) feasible = feasible && !cand[static_cast<size_t>(i)].empty();
            if (!feasible) continue;
            std::vector<size_t> idx(static_cast<size_t>(use), 0);
            while (true) {
                std::vector<int> v = { root, fifth };
                for (int i = 0; i < use; ++i) v.push_back(cand[static_cast<size_t>(i)][idx[static_cast<size_t>(i)]]);
                std::sort(v.begin() + 2, v.end());
                bool ok = true;
                for (size_t k = 1; k < v.size(); ++k) { const int gap = v[k] - v[k - 1]; ok = ok && gap >= 3 && gap <= 12; }
                if (ok) { bestSize = v.size(); best = std::min(best, voicingMovement(v, prev)); }
                int i = use - 1;
                while (i >= 0 && ++idx[static_cast<size_t>(i)] == cand[static_cast<size_t>(i)].size()) { idx[static_cast<size_t>(i)] = 0; --i; }
                if (i < 0) break;
            }
        }
        ++cases;
        if (got.size() != bestSize || voicingMovement(got, prev) != best) {
            ++mismatches;
            if (std::getenv("PHOS_DEBUG_RULES")) {
                std::printf("DBG voicing: scale %d key %d degree %d type %d, got", scale, key, degree, type);
                for (int x : got) std::printf(" %d", x);
                std::printf(" (move %d), best size %zu move %d, prev", voicingMovement(got, prev), bestSize, best);
                for (int x : prev) std::printf(" %d", x);
                std::printf("\n");
            }
        }
        for (int x : got) if (x < kPadLowest || x > kPadHighest) ++badNotes;
    }
    check(mismatches == 0 && badNotes == 0,
          "every pad voicing is the open root-position placement of its chord type that moves the voices least (rule 19, 22.09.2026)",
          fmt("%d of %d differ from brute force, %d notes out of range", mismatches, cases, badNotes));

    // In the score: pad notes are chord tones of their bar, held to the next chord. Over the DJ overlap
    // (19.09.2026) the pads that sound are the incoming track's, on its own chords.
    ParamStore p;
    p.parseText("compose.track_bars=128 compose.pad_amount=1 compose.level_match=Off master.auto_gain=Off");
    Composer c(515);
    std::vector<NoteEvent> ev;
    c.composeBars(p, 0, 4 * 128, ev);
    // Under D3 only the sub foundation (rule 20): the chord root, in a bar without kick and bass.
    std::set<int> loud;
    for (const NoteEvent& e : ev) if (e.part == Part::Kick || e.part == Part::Bass) loud.insert(static_cast<int>(e.beat / kBeatsPerBar));
    int pads = 0, off = 0;
    for (const NoteEvent& e : ev) {
        if (e.part != Part::Pad) continue;
        const int bar = static_cast<int>(e.beat / kBeatsPerBar);
        const int incoming = c.incomingOfBar(p, bar);
        const int ti = incoming >= 0 ? incoming : c.trackOfBar(p, bar);
        const TrackPlan t = c.track(p, ti);
        // The section's mode, not the track's: since 16.09.2026 a section may borrow another mode
        // over the tonic pedal (Form.h), and the pad is voiced in the mode its section plays.
        const int sc = t.form.section[sectionOfBar(t.form, bar - t.firstBar)].scale;
        // The pad's chord for this bar (22.09.2026): the pendulum, or in the main breakdown its own
        // progression; its tones are the chord type's over its root, not the scale triad's.
        const PadChord pcd = padChordAt(t.melody, t.form, bar - t.firstBar);
        int iv[4], chordRootPc = 0;
        const int n = padChordIntervals(static_cast<ChordType>(pcd.type), sc, pcd.degree, chordRootPc, iv);
        std::set<int> tones = { chordRootPc };
        for (int i = 0; i < n; ++i) tones.insert(((chordRootPc + iv[i]) % 12 + 12) % 12);
        const int pc = ((e.pitch - t.key) % 12 + 12) % 12;
        ++pads;
        const bool sub = e.pitch < kPadLowest;
        if (tones.count(pc) == 0 || (sub && (loud.count(bar) != 0 || pc != chordRootPc || e.pitch < kPadFoundationLowest))) ++off;
    }
    check(pads > 100 && off == 0, "pad notes are tones of their bar's chord, D3 and above -- under it only the sub root (F#1 and up) where kick and bass rest",
          fmt("%d of %d off", off, pads));
}

} // namespace phostest
