/**
 * @file selftest_mixguide.cpp
 * @brief The self test's mix-guide checks (25.09.2026): the ducking matrix, the two ducks the lead drives, the planes'
 *        cues and pre-delays, distance, the monitor, the slow movement, and the mix by phase in the control events.
 *
 * The render-level measurements of the guide -- correlation, mono loss, tilt, the kick-bass gap, the contrast
 * of drop and break -- are Tools/mix_audit.py's (ctest `mixaudit`); these are the mechanisms behind them.
 */
#include "SelfTestHelpers.h"

using namespace phos;

namespace phostest {

/**
 * @brief Mix guide, part `.matrix`: every strip's duck under the kick and its release, at the defaults, inside the
 *        guide's ducking matrix.
 *
 * The matrix (the guide's "Ducking-Matrix", 25.09.2026): the bass 2 .. 4 dB back within 40 .. 70 ms; the lines
 * (acid, lead, counter, arp, stab) 1 .. 2 dB -- "nur fuehlbar" -- within 50 .. 80 ms; the back plane (pad, drone,
 * the bed, the voices) 4 .. 8 dB within 100 .. 150 ms; the returns 3 .. 6 dB within 80 .. 120 ms. The stab counts
 * as a line here, at the upper edge (it plays chords, but short ones). The effects are left out on purpose: an
 * impact falls on the drop's kick, and a duck would take its hit.
 */
void testMixGuideMatrix()
{
    section("mix guide: the ducking matrix at the defaults");
    ParamStore p;
    auto db = [&](const char* key) { return 20.0 * std::log10(std::max(1e-6, 1.0 - static_cast<double>(p.get(p.find(key))))); };
    auto v = [&](const char* key) { return static_cast<double>(p.get(p.find(key))); };
    struct Row { const char* key; double lo, hi; };
    static const Row kDepth[] = {
        { "bass.duck_depth", -4.0, -2.0 }, { "acid.duck", -2.0, -1.0 }, { "lead.duck", -2.0, -1.0 },
        { "counter.duck", -2.0, -1.0 }, { "arp.duck", -2.0, -1.0 }, { "stab.duck", -2.0, -1.0 },
        { "pad.duck", -8.0, -4.0 }, { "drone.duck", -8.0, -4.0 }, { "texture.duck", -8.0, -4.0 }, { "vocal.duck", -8.0, -4.0 },
        { "fx.return_duck", -6.0, -3.0 },
    };
    std::string detail;
    int bad = 0;
    for (const Row& r : kDepth) {
        const double d = db(r.key);
        detail += fmt("%s %.1f  ", r.key, d);
        if (d < r.lo - 0.05 || d > r.hi + 0.05) ++bad;
    }
    check(bad == 0, "every strip's duck under the kick inside the guide's matrix (bass 2..4, lines 1..2, back plane 4..8, returns 3..6 dB)", detail);
    const double rBass = v("bass.duck_release"), rLines = v("mix.duck_release_lines"), rBack = v("mix.duck_release"), rRet = v("fx.return_duck_release");
    check(rBass >= 40 && rBass <= 70 && rLines >= 50 && rLines <= 80 && rBack >= 100 && rBack <= 150 && rRet >= 80 && rRet <= 120,
          "the releases: bass 40..70, lines 50..80, back plane 100..150, returns 80..120 ms",
          fmt("bass %.0f, lines %.0f, back %.0f, returns %.0f ms", rBass, rLines, rBack, rRet));
}

/** @brief Renders @p seconds of an engine with @p settings and the notes @p notes: the left channel, the right into @p right if given. */
static std::vector<float> renderNotes(const std::string& settings, const std::vector<NoteEvent>& notes, double seconds,
                                      std::vector<float>* right = nullptr)
{
    auto e = std::make_unique<Engine>();
    e->prepare(48000.0, 256);
    e->params().parseText(("mix.kick_mute=1 mix.bass_mute=1 mix.perc_mute=1 master.limiter=Off master.clipper=Off master.clip=Off "
                           "master.comp_ratio=1 fx.hall_return=-36 fx.room_return=-36 fx.plate_return=-36 " + settings).c_str());
    // The engine's rings are in time order: an event queued behind a later one waits for it.
    std::vector<NoteEvent> sorted = notes;
    std::stable_sort(sorted.begin(), sorted.end(), [](const NoteEvent& x, const NoteEvent& y) { return x.beat < y.beat; });
    for (const NoteEvent& n : sorted) e->pushEvent(n);
    std::vector<float> L(static_cast<size_t>(48000.0 * seconds)), R(L.size());
    e->process(L.data(), R.data(), static_cast<int>(L.size()));
    if (right != nullptr) *right = R;
    return L;
}

/**
 * @brief Mix guide, part `.leadDucks`: the counter gives way to the lead, and the pad's presence band does.
 *
 * The lead's own strip is muted in both renders, so what is measured is the other voice alone and the lead is
 * only the trigger -- the duck is driven by its notes, not by its sound (Engine.h).
 */
void testMixGuideLeadDucks()
{
    section("mix guide: the counter and the pad's band under the lead");
    const double beat = 60.0 / 145.0 * 48000.0;
    auto rms = [&](const std::vector<float>& y, double a, double b) {
        double s = 0.0;
        size_t n = 0;
        for (size_t i = static_cast<size_t>(a); i < static_cast<size_t>(b) && i < y.size(); ++i) { s += static_cast<double>(y[i]) * y[i]; ++n; }
        return 10.0 * std::log10(std::max(s / std::max<size_t>(n, 1), 1e-30));
    };
    std::vector<NoteEvent> leads;
    for (int b = 8; b < 24; b += 2) {
        NoteEvent l;
        l.part = Part::Lead; l.beat = b; l.length = 0.25f; l.pitch = 67; l.velocity = 100;
        leads.push_back(l);
    }
    // (a) The counter: a held note, lead notes on every other beat. Right after a lead onset against the same
    //     window a beat later, averaged over eight onsets.
    {
        auto run = [&](const char* duck) {
            std::vector<NoteEvent> notes = leads;
            NoteEvent c;
            c.part = Part::Counter; c.pitch = 76; c.length = 40.0f; c.velocity = 100;
            notes.push_back(c);
            const std::vector<float> y = renderNotes(std::string("mix.lead_mute=1 counter.amp_sustain=1 counter.amp_decay=5000 "
                                                                 "counter.delay_send=0 counter.duck=0 ") + duck, notes, 12.0);
            double d = 0.0;
            for (int b = 8; b < 24; b += 2) d += rms(y, b * beat + 0.002 * 48000, b * beat + 0.020 * 48000) - rms(y, (b + 1) * beat + 0.002 * 48000, (b + 1) * beat + 0.020 * 48000);
            return d / 8.0;
        };
        // The counter's own movement (its LFO, its envelope) is in both renders; the duck is the difference.
        const double on = run("mix.counter_duck=0.2"), off = run("mix.counter_duck=0");
        check(on - off < -1.0 && on - off > -2.5, "the counter gives way to the lead by 1..3 dB (mix.counter_duck)",
              fmt("%.2f dB right after a lead note against the beat after it, %.2f dB with the duck off", on, off));
    }
    // (b) The pad's band: a held chord, lead notes a beat long on every other beat. The band 800 Hz .. 2 kHz while a
    //     lead note sounds against the beat after it, and the band 100 .. 300 Hz the same way.
    {
        std::vector<NoteEvent> notes;
        for (NoteEvent l : leads) { l.length = 1.0f; notes.push_back(l); }
        for (int pitch : { 48, 55, 60, 64, 67 }) {
            NoteEvent n;
            n.part = Part::Pad; n.pitch = static_cast<uint8_t>(pitch); n.length = 40.0f; n.velocity = 100;
            notes.push_back(n);
        }
        // Rendered with the band's duck and without: the pad's own movement (its tremolo, its position envelope) is
        // in both, and the band's step is the difference.
        auto measure = [&](const char* duck, double& mid, double& low) {
            const std::vector<float> y = renderNotes(std::string("mix.lead_mute=1 pad.duck=0 pad.gate=Off pad.hall_send=0 pad.amp_attack=1 "
                                                                 "pad.hp_floor=40 ") + duck, notes, 12.0);
            auto bandDb = [&](double a, double lo, double hi) {
                const size_t n = 8192;
                std::vector<float> seg(y.begin() + static_cast<long>(a), y.begin() + static_cast<long>(a) + static_cast<long>(n));
                const std::vector<double> pw = powerSpectrum(seg.data(), n);
                double s = 0.0;
                for (size_t k = 1; k < n / 2; ++k) {
                    const double f = static_cast<double>(k) * 48000.0 / static_cast<double>(n);
                    if (f >= lo && f < hi) s += pw[k];
                }
                return 10.0 * std::log10(s + 1e-30);
            };
            mid = low = 0.0;
            for (int b = 8; b < 24; b += 2) {
                mid += bandDb(b * beat + 0.2 * beat, 800.0, 2000.0) - bandDb((b + 1) * beat + 0.4 * beat, 800.0, 2000.0);
                low += bandDb(b * beat + 0.2 * beat, 100.0, 300.0) - bandDb((b + 1) * beat + 0.4 * beat, 100.0, 300.0);
            }
            mid /= 8.0;
            low /= 8.0;
        };
        double midOn = 0, lowOn = 0, midOff = 0, lowOff = 0;
        measure("mix.pad_lead_duck=3", midOn, lowOn);
        measure("mix.pad_lead_duck=0", midOff, lowOff);
        const double mid = midOn - midOff, low = lowOn - lowOff;
        check(mid < -1.5 && mid > -4.0 && std::fabs(low) < 0.5,
              "the pad's presence band (500 Hz .. 3 kHz) steps back by 2..4 dB while the lead speaks, its body stays",
              fmt("800 Hz .. 2 kHz %.2f dB, 100 .. 300 Hz %.2f dB (against the same render without the band's duck)", mid, low));
    }
}

/**
 * @brief Mix guide, part `.planes`: the three planes' cues agree, voice by voice, at the defaults.
 *
 * The Dark-Ambient addon's first lesson: distance is five cues at once -- level, the loss of the highs, the ratio of
 * direct to room, the pre-delay and the transients -- and a voice that is far by one and near by another sticks to
 * the speakers. So every voice's defaults are held to its plane: the front (the lead) dry, into the near room at
 * most, sharp; the middle (acid, counter, arp, stab) into the plate at 15 .. 25 %; the back (pad, drone) into the
 * plate at 30 % and more (the far room is fed from the plate), its highs rolled off under 8 kHz, a slow attack and
 * full width. And the rooms' pre-delays follow the addon's correction: near sources long, far ones short -- the
 * near room 40 .. 60 ms, the plate 20 .. 30, the hall 0 .. 10 -- no longer tempo values.
 */
void testMixGuidePlanes()
{
    section("mix guide: the planes' cues agree, and the pre-delays are distance cues");
    ParamStore p;
    auto v = [&](const char* key) { return static_cast<double>(p.get(p.find(key))); };
    std::string bad;
    auto want = [&](bool ok, const char* what) { if (!ok) bad += std::string(what) + "; "; };
    want(v("lead.distance") == 0.0 && v("lead.plate_send") == 0.0 && v("lead.hall_send") == 0.0 && v("lead.room_send") > 0.0
         && v("lead.room_send") <= 0.15 && v("lead.amp_attack") <= 10.0, "lead: front");
    for (const char* k : { "counter", "arp", "stab" }) {
        const std::string b(k);
        want(v((b + ".distance").c_str()) == 0.5 && v((b + ".hall_send").c_str()) == 0.0 && v((b + ".plate_send").c_str()) >= 0.1
             && v((b + ".plate_send").c_str()) <= 0.25, (b + ": middle").c_str());
    }
    want(v("acid.plate_send") >= 0.1 && v("acid.plate_send") <= 0.25 && v("acid.hall_send") == 0.0, "acid: middle");
    for (const char* k : { "pad", "drone" }) {
        const std::string b(k);
        want(v((b + ".distance").c_str()) == 1.0 && v((b + ".plate_send").c_str()) >= 0.3 && v((b + ".hall_send").c_str()) == 0.0
             && v((b + ".cutoff").c_str()) <= 8000.0 && v((b + ".amp_attack").c_str()) >= 50.0 && v((b + ".width").c_str()) >= 0.8,
             (b + ": back").c_str());
    }
    check(bad.empty(), "every voice's level of wet, send, highs, attack and width fits its plane (front, middle, back)", bad);
    const double hallMs = v("fx.hall_pre_delay") * 60.0 / 145.0 * 1000.0;
    check(v("fx.room_pre_delay") >= 40.0 && v("fx.room_pre_delay") <= 60.0 && v("fx.plate_pre_delay") >= 20.0 && v("fx.plate_pre_delay") <= 30.0
              && hallMs <= 10.0,
          "pre-delays by distance: the near room 40..60 ms, the plate 20..30, the hall 0..10 (the addon's correction)",
          fmt("room %.0f, plate %.0f, hall %.1f ms", v("fx.room_pre_delay"), v("fx.plate_pre_delay"), hallMs));
    Reverb r;
    r.prepare(48000.0);
    const double samples = v("fx.plate_pre_delay") * 48.0;
    r.set(static_cast<float>(v("fx.plate_size")), static_cast<float>(v("fx.plate_decay")), static_cast<float>(v("fx.plate_damping")),
          static_cast<float>(samples), static_cast<float>(v("fx.plate_low_cut")), static_cast<float>(v("fx.plate_high_cut")));
    std::vector<float> inL(48000, 0.0f), inR(48000, 0.0f), outL(48000), outR(48000);
    inL[0] = inR[0] = 1.0f;
    r.process(inL.data(), inR.data(), outL.data(), outR.data(), 48000);
    size_t first = outL.size();
    double peak = 0.0;
    for (float x : outL) peak = std::max(peak, std::fabs(static_cast<double>(x)));
    for (size_t i = 0; i < outL.size() && first == outL.size(); ++i) if (std::fabs(outL[i]) > 1e-3 * peak) first = i;
    check(first + 2 >= static_cast<size_t>(samples), "nothing leaves the plate before its pre-delay",
          fmt("pre-delay %.1f ms, first output at %.1f ms", samples / 48.0, static_cast<double>(first) / 48.0));
}

/**
 * @brief Mix guide, part `.distance`: poly.distance moves a voice with every cue at once, the monitor listens as it
 *        says, and the slow movement moves.
 */
void testMixGuideDistance()
{
    section("mix guide: distance, the monitor and the slow movement");
    // (a) The lead from its plane (0) to the back (1): about 15 dB down, the highs further down than the lows.
    std::vector<NoteEvent> notes;
    NoteEvent n;
    n.part = Part::Lead; n.pitch = 64; n.length = 6.0f; n.velocity = 100;
    notes.push_back(n);
    const char* base = "lead.amp_sustain=1 lead.amp_decay=5000 lead.delay_send=0 lead.room_send=0 lead.duck=0 lead.lfo_amp=0 ";
    // The mid signal: level and low pass alone (the far lead is also wider, which a single channel would count).
    auto mid = [&](const char* d) {
        std::vector<float> R;
        std::vector<float> L = renderNotes(std::string(base) + d, notes, 2.0, &R);
        for (size_t i = 0; i < L.size(); ++i) L[i] = 0.5f * (L[i] + R[i]);
        return L;
    };
    const std::vector<float> nearY = mid("lead.distance=0"), farY = mid("lead.distance=1");
    // Band power of the 32768 samples from @p from on (half a second in: past the note's attack).
    auto band = [](const std::vector<float>& y, double lo, double hi, size_t from = 24000) {
        const size_t N = 32768;
        const std::vector<double> pw = powerSpectrum(y.data() + from, N);
        double s = 0.0;
        for (size_t k = 1; k < N / 2; ++k) { const double f = k * 48000.0 / N; if (f >= lo && f < hi) s += pw[k]; }
        return 10.0 * std::log10(s + 1e-30);
    };
    const double low = band(farY, 200.0, 2000.0) - band(nearY, 200.0, 2000.0), high = band(farY, 8000.0, 16000.0) - band(nearY, 8000.0, 16000.0);
    check(low < -12.0 && low > -18.0 && high < low - 3.0, "distance 0 -> 1: about 15 dB down, and the highs further than the lows",
          fmt("200 Hz .. 2 kHz %.1f dB, 8 .. 16 kHz %.1f dB", low, high));
    // (b) The monitor: mono is one signal on both sides, side is (L - R) / 2, sub keeps only the bottom.
    {
        std::vector<NoteEvent> pad;
        for (int pitch : { 48, 60, 67, 76 }) {
            NoteEvent q;
            q.part = Part::Pad; q.pitch = static_cast<uint8_t>(pitch); q.length = 6.0f; q.velocity = 100;
            pad.push_back(q);
        }
        auto render2 = [&](const char* mon, std::vector<float>& R) {
            auto e = std::make_unique<Engine>();
            e->prepare(48000.0, 256);
            e->params().parseText((std::string("mix.kick_mute=1 mix.bass_mute=1 mix.perc_mute=1 master.limiter=Off master.clipper=Off master.clip=Off "
                                               "master.comp_ratio=1 pad.amp_attack=1 master.monitor=") + mon).c_str());
            for (const NoteEvent& q : pad) e->pushEvent(q);
            std::vector<float> L(48000 * 2);
            R.assign(L.size(), 0.0f);
            e->process(L.data(), R.data(), static_cast<int>(L.size()));
            return L;
        };
        std::vector<float> nR, mR, sR, bR;
        const std::vector<float> nL = render2("Normal", nR), mL = render2("Mono", mR), sL = render2("Side", sR), bL = render2("Sub", bR);
        double monoErr = 0.0, sideErr = 0.0;
        for (size_t i = 0; i < nL.size(); ++i) {
            monoErr = std::max(monoErr, std::fabs(static_cast<double>(mL[i]) - 0.5 * (nL[i] + nR[i])) + std::fabs(static_cast<double>(mL[i]) - mR[i]));
            sideErr = std::max(sideErr, std::fabs(static_cast<double>(sL[i]) - 0.5 * (nL[i] - nR[i])) + std::fabs(static_cast<double>(sL[i]) - sR[i]));
        }
        const double subTop = band(bL, 400.0, 16000.0) - band(nL, 400.0, 16000.0);
        check(monoErr < 1e-6 && sideErr < 1e-6 && subTop < -40.0, "the monitor: mono is (L+R)/2 on both sides, side (L-R)/2, sub only the bottom",
              fmt("mono %.1e, side %.1e, sub above 400 Hz %.1f dB", monoErr, sideErr, subTop));
    }
    // (c) The slow movement: a pad held for 30 s with its tempo LFOs, its unison and its second oscillator off (their
    //     beating alone moves a window's brightness by 7 dB); the brightness of the windows varies with
    //     poly.slow_mod and not without it.
    {
        std::vector<NoteEvent> pad;
        for (int pitch : { 48, 55, 60, 64 }) {
            NoteEvent q;
            q.part = Part::Pad; q.pitch = static_cast<uint8_t>(pitch); q.length = 80.0f; q.velocity = 100;
            pad.push_back(q);
        }
        // Per 0.7-second window, every two seconds: the brightness (2 .. 8 kHz against 100 Hz .. 1 kHz). The unison's
        // own residual beating is the same in both renders (the same notes, the same random phases), so the
        // difference of the two is the slow movement alone.
        auto brightness = [&](const char* slow) {
            const std::vector<float> y = renderNotes(std::string("pad.gate=Off pad.duck=0 pad.amp_attack=1 pad.lfo_cutoff=0 pad.lfo_amp=0 "
                                                                 "pad.pos_lfo_depth=0 pad.pos_env=0 pad.drift=0 pad.detune=0 pad.osc2_mix=0 pad.slow_mod=") + slow, pad, 30.0);
            std::vector<double> out;
            for (int w = 1; w < 14; ++w) {
                const std::vector<float> seg(y.begin() + w * 96000, y.begin() + w * 96000 + 32768);
                out.push_back(band(seg, 2000.0, 8000.0, 0) - band(seg, 100.0, 1000.0, 0));
            }
            return out;
        };
        const std::vector<double> moving = brightness("1"), still = brightness("0");
        double lo = 1e9, hi = -1e9;
        for (size_t w = 0; w < moving.size(); ++w) { lo = std::min(lo, moving[w] - still[w]); hi = std::max(hi, moving[w] - still[w]); }
        check(hi - lo > 3.0, "the slow movement: a held pad's brightness wanders with slow_mod against the same pad without it",
              fmt("the difference of the two renders' brightness spans %.1f dB over 26 s (%+.1f .. %+.1f)", hi - lo, lo, hi));
    }
}

/**
 * @brief Mix guide, part `.phase`: the mix by phase in the control events of a composed track.
 *
 * The second track of a set (the first section of the first keeps the knobs): its breakdown stands kBreakTrimDb
 * and more under its first drop; the pad is as wide there as in the drop and the lead further back; a buildup
 * ramps the plate's decay down to 1.2 s and the lines' high pass up over its length, both back with the drop's
 * first bar; the far room (the hall) is open in the breakdown and muted in the drop; and the effects are thrown into
 * the plate on the last beat before an eight-bar change of a groove or a drop, back on the next downbeat.
 */
void testMixGuidePhase()
{
    section("mix guide: the mix by phase in the control events");
    ParamStore p;
    p.parseText("compose.level_match=Off master.auto_gain=Off");
    Composer c(4242);
    const TrackPlan t = c.track(p, 1);
    std::vector<NoteEvent> ev;
    std::vector<ControlEvent> ctl;
    c.composeBars(p, t.firstBar, t.bars, ev, &ctl);
    auto at = [&](int id, double beat, float& value, float& length) {
        for (const ControlEvent& e : ctl)
            if (e.param == id && std::fabs(e.beat - beat) < 1e-6 && e.kind == ControlEvent::Kind::Offset) { value = e.value; length = e.length; return true; }
        return false;
    };
    int breakIdx = -1, dropIdx = -1, buildIdx = -1;
    for (int i = 0; i < t.form.count; ++i) {
        const Section& s = t.form.section[i];
        if (s.type == SectionType::Drop && !s.climax && dropIdx < 0) dropIdx = i;
        if (s.type == SectionType::Break && breakIdx < 0 && dropIdx >= 0) breakIdx = i;
        if (s.type == SectionType::Build && buildIdx < 0 && breakIdx >= 0) buildIdx = i;
    }
    check(dropIdx >= 0 && breakIdx >= 0 && buildIdx >= 0, "the track has a drop, a breakdown after it and a buildup after that",
          fmt("drop %d, break %d, build %d", dropIdx, breakIdx, buildIdx));
    if (dropIdx < 0 || breakIdx < 0 || buildIdx < 0) return;
    auto startBeat = [&](int i) { return static_cast<double>(t.firstBar + t.form.section[i].startBar) * kBeatsPerBar; };
    const int mb = p.base(Module::Mix);
    float gDrop = 0, gBreak = 0, len = 0;
    const bool gainOk = at(mb + mix::TrackGain, startBeat(dropIdx), gDrop, len) && at(mb + mix::TrackGain, startBeat(breakIdx), gBreak, len);
    const float span = p.desc(mb + mix::TrackGain).maxValue - p.desc(mb + mix::TrackGain).minValue;
    const double fall = (gDrop - gBreak) * span;
    check(gainOk && fall >= kBreakTrimDb - 1e-3, "the breakdown stands at least kBreakTrimDb under the drop before it",
          fmt("%.2f dB (kBreakTrimDb %.1f)", fall, static_cast<double>(kBreakTrimDb)));
    const int padW = p.base(PolyInstance::Pad) + poly::Width, leadH = p.base(PolyInstance::Lead) + poly::Distance;
    float wDrop = 0, wBreak = 0, hDrop = 0, hBreak = 0, lDrop = 0, lBreak = 0;
    const bool widthOk = at(padW, startBeat(dropIdx), wDrop, lDrop) && at(padW, startBeat(breakIdx), wBreak, lBreak);
    const bool hallOk = at(leadH, startBeat(dropIdx), hDrop, len) && at(leadH, startBeat(breakIdx), hBreak, len);
    check(widthOk && hallOk && std::fabs(wBreak - wDrop) < 1e-4f && hBreak - hDrop > 0.5f
              && std::fabs(lDrop - kBeatsPerBar) < 1e-4f && std::fabs(lBreak - 4.0f * kBeatsPerBar) < 1e-4f,
          "the pad as wide in the drop as in the breakdown, the lead further back there; one bar into the drop, four into the break",
          fmt("pad width %+.2f against %+.2f, lead distance %+.2f against %+.2f, ramps %.0f and %.0f beats", wBreak, wDrop, hBreak, hDrop, lDrop, lBreak));
    const int decay = p.base(Module::Fx) + fx::PlateDecay, hp = p.base(PolyInstance::Lead) + poly::HpFloor;
    const int hallRet = p.base(Module::Fx) + fx::HallReturn;
    float dBuild = 0, dLen = 0, dDrop = 1, hBuild = 0, hLen = 0;
    const int after = buildIdx + 1;
    const bool buildOk = at(decay, startBeat(buildIdx), dBuild, dLen) && at(hp, startBeat(buildIdx), hBuild, hLen)
                         && after < t.form.count && at(decay, startBeat(after), dDrop, len);
    const float want = p.toNormalised(decay, 1.2f) - p.toNormalised(decay, p.get(decay));
    float rDrop = 0, rBreak = 1;
    const bool roomsOk = at(hallRet, startBeat(dropIdx), rDrop, len) && at(hallRet, startBeat(breakIdx), rBreak, len);
    const float muted = p.toNormalised(hallRet, p.desc(hallRet).minValue) - p.toNormalised(hallRet, p.get(hallRet));
    check(buildOk && roomsOk && std::fabs(dBuild - want) < 1e-4f && std::fabs(dLen - t.form.section[buildIdx].bars * kBeatsPerBar) < 1e-3f
              && hBuild > 0.0f && dDrop == 0.0f && std::fabs(rDrop - muted) < 1e-4f && rBreak == 0.0f,
          "a buildup pulls the plate down to 1.2 s and lifts the lines' high pass; the drop takes both back, the far room is open in the break and muted in the drop",
          fmt("plate decay offset %+.3f (want %+.3f) over %.0f beats, lead high pass %+.3f, at the drop %+.3f; hall return %+.3f in the drop, %+.3f in the break",
              dBuild, want, dLen, hBuild, dDrop, rDrop, rBreak));
    int throws = 0, backs = 0, wrong = 0;
    const int sh = p.base(Module::Sfx) + sfx::PlateSend;
    for (const ControlEvent& e : ctl) {
        if (e.param != sh) continue;
        const double inBar = e.beat - std::floor(e.beat / kBeatsPerBar) * kBeatsPerBar;
        const int bar = static_cast<int>(std::floor(e.beat / kBeatsPerBar)) - t.firstBar;
        if (e.value > 0.4f) { ++throws; if (std::fabs(inBar - 3.0) > 1e-6 || bar % 8 != 7) ++wrong; }
        else if (e.value == 0.0f) { ++backs; if (inBar != 0.0 || bar % 8 != 0) ++wrong; }
    }
    check(throws > 3 && backs >= throws && wrong == 0, "the effects thrown into the plate on beat 4 before every eight-bar change, back on the downbeat",
          fmt("%d throws, %d returns, %d off the grid", throws, backs, wrong));
}

} // namespace phostest
