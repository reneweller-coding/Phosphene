/**
 * @file SelfTestHelpers.h
 * @brief The self test's shared measurement helpers: renders, spectra, alias figures, reference cases and toy models.
 *
 * Header-only, and only for the self test's sources (selftest_*.cpp). Until 25.09.2026 they were one 15 000-line
 * file; the helpers were its anonymous namespace and are these inline functions now, word for word.
 */
#pragma once
#include "phos/Acid.h"
#include "phos/Audibility.h"
#include "phos/Composer.h"
#include "phos/Corpus.h"
#include "phos/Cue.h"
#include "phos/DiodeLadder.h"
#include "phos/Dynamics.h"
#include "phos/Dsp.h"
#include "phos/Engine.h"
#include "phos/Halfband.h"
#include "phos/Harmony.h"
#include "phos/Ladder.h"
#include "phos/Loudness.h"
#include "phos/Midi.h"
#include "phos/MidiMap.h"
#include "phos/Model.h"
#include "phos/Oscillator.h"
#include "phos/Patterns.h"
#include "phos/Melody.h"
#include "phos/Perc.h"
#include "phos/Poly.h"
#include "phos/Preferences.h"
#include "phos/Probe.h"
#include "phos/Rating.h"
#include "phos/Reverb.h"
#include "phos/Rhythm.h"
#include "phos/Form.h"
#include "phos/Gallery.h"
#include "phos/SetFile.h"
#include "phos/Sfx.h"
#include "phos/SoundPresets.h"
#include "phos/TranceGate.h"
#include "phos/WaveTable.h"
#include "phos/WaveTableFile.h"
#include "phos/WavWriter.h"
#include "TestSupport.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <tuple>
#include "SelfTests.h"

namespace phostest {

using namespace phos;

constexpr double kPiD = 3.141592653589793;

/**
 * @brief Amplitude of the component with exactly @p cycles periods in @p n samples.
 *
 * With an integer number of cycles the rectangular projection is exact for a steady sine.
 */
inline double toneAmplitude(const float* x, size_t n, double cycles)
{
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * kPiD * cycles * static_cast<double>(i) / static_cast<double>(n);
        re += x[i] * std::cos(ph);
        im += x[i] * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n);
}

/** @brief Renders an engine for @p beats with a given block size, pumping a conductor. */
inline std::vector<float> renderEngine(Engine& engine, const Composer& composer, double beats, int block, double sr, std::vector<float>* right = nullptr)
{
    Conductor conductor(engine, composer);
    const int cb = engine.params().base(Module::Compose);
    const double bpm = engine.params().get(cb + compose::Bpm);
    const size_t total = static_cast<size_t>(std::llround(beats * 60.0 / bpm * sr));
    std::vector<float> L(total), R(total);
    size_t done = 0;
    while (done < total) {
        const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), total - done));
        conductor.pump(engine.params(), 32.0);
        engine.process(L.data() + done, R.data() + done, n);
        done += static_cast<size_t>(n);
    }
    if (right != nullptr) *right = std::move(R);
    return L;
}

// ---------------------------------------------------------------------------------------------

/** @brief Bar on which the first core of track 0 begins (since Phase 5 bar 0 is the intro). */
inline int firstCoreBar(const ParamStore& p, const Composer& c)
{
    const TrackPlan t = c.track(p, 0);
    for (int i = 0; i < t.form.count; ++i)
        if (t.form.section[i].type == SectionType::Groove || t.form.section[i].type == SectionType::Drop) return t.form.section[i].startBar;
    return 0;
}

/**
 * @brief The genre rules' own reference: pitch classes derived straight from the scale table.
 *
 * Nothing here calls the code under test. The colour tones are the flat second and the upper note
 * of every three-semitone step between neighbouring degrees; a chord is the degrees d, d+2, d+4.
 */
struct RuleRef {
    /** @brief Semitones of degree @p d of @p scale above the tonic, octaves folded in. */
    static int deg(int scale, int d) { return kScaleSteps[scale][d % 7] + 12 * (d / 7); }   // d >= 0
    /** @brief Whether pitch class @p pc (above the tonic) is a colour tone of @p scale. */
    static bool colour(int scale, int pc)
    {
        pc = ((pc % 12) + 12) % 12;
        bool in = false;
        for (int d = 0; d < 7; ++d) in = in || kScaleSteps[scale][d] == pc;
        if (!in) return false;
        if (pc == 1) return true;
        for (int d = 1; d < 7; ++d)
            if (kScaleSteps[scale][d] - kScaleSteps[scale][d - 1] == 3 && kScaleSteps[scale][d] == pc) return true;
        return false;
    }
    /** @brief Pitch class (above the tonic) of the root of the chord on degree @p d. */
    static int chordRoot(int scale, int d) { return ((deg(scale, d) % 12) + 12) % 12; }
    /**
     * @brief The register rule of 19.09.2026, read off a score: at how many sixteenths of bars
     *        [@p from, @p to) two of the line voices (lead, counter-lead, stab, arp) sound with their
     *        pitch spans less than kRegisterGap semitones apart.
     *
     * Independent of Melody.cpp's guard: a note sounds at a sixteenth when its onset is at or before the
     * sixteenth and its written length reaches past it -- nothing about spans in steps or priorities.
     */
    static int registerClashes(const std::vector<NoteEvent>& ev, int from, int to)
    {
        const Part lines[4] = { Part::Lead, Part::Counter, Part::Stab, Part::Arp };
        std::map<int, std::array<std::pair<int, int>, 4>> at;   // sixteenth -> (lo, hi) per voice
        for (const NoteEvent& e : ev) {
            int v = -1;
            for (int k = 0; k < 4; ++k) if (e.part == lines[k]) v = k;
            if (v < 0) continue;
            const int first = static_cast<int>(std::ceil(e.beat * 4.0 - 1e-6));
            for (int s = first; s * 0.25 < e.beat + e.length - 1e-6; ++s) {
                if (s < from * 16 || s >= to * 16) continue;
                auto it = at.find(s);
                if (it == at.end()) { std::array<std::pair<int, int>, 4> init; init.fill({ 1000, -1000 }); it = at.emplace(s, init).first; }
                auto& span = it->second[static_cast<size_t>(v)];
                span.first = std::min(span.first, static_cast<int>(e.pitch));
                span.second = std::max(span.second, static_cast<int>(e.pitch));
            }
        }
        int clashes = 0;
        for (const auto& kv : at) {
            bool clash = false;
            for (int a = 0; a < 4; ++a)
                for (int b = a + 1; b < 4; ++b) {
                    const auto& x = kv.second[static_cast<size_t>(a)];
                    const auto& y = kv.second[static_cast<size_t>(b)];
                    if (x.first > x.second || y.first > y.second) continue;
                    if (!(x.first - y.second >= kRegisterGap || y.first - x.second >= kRegisterGap)) clash = true;
                }
            clashes += clash ? 1 : 0;
        }
        return clashes;
    }
};

/** @brief Longest run of identical consecutive values, the sequence read as a loop when @p cyclic. */
inline int longestRun(const std::vector<int>& v, bool cyclic)
{
    if (v.empty()) return 0;
    const size_t n = v.size();
    int best = 1, run = 1;
    const size_t len = cyclic ? 2 * n : n;
    for (size_t i = 1; i < len; ++i) {
        run = v[i % n] == v[(i - 1) % n] ? run + 1 : 1;
        best = std::max(best, std::min(run, static_cast<int>(n)));
    }
    return best;
}

// ---------------------------------------------------------------------------------------------

/**
 * @brief Power outside the harmonic bins relative to the harmonics, below @p maxBin, in dB.
 *
 * Measured in the audible band only: a decimator's transition band lets partials between 24 and
 * 29 kHz fold into 19..24 kHz, which is aliasing nobody hears and would otherwise dominate the figure.
 */
inline double aliasDb(const std::vector<float>& x, size_t n, size_t harmonicBin, size_t maxBin)
{
    std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < n; ++i) a[i] = x[i];
    fft(a);
    double harm = 0.0, rest = 0.0;
    for (size_t k = 1; k < maxBin; ++k) {
        const double p = std::norm(a[k]);
        if (k % harmonicBin == 0) harm += p; else rest += p;
    }
    return powDb(rest / harm);
}

/** @brief Current values of one module as an array indexed like its table. */
inline std::vector<float> moduleValues(const ParamStore& p, Module m, int instance = 0)
{
    std::vector<float> v(static_cast<size_t>(ParamStore::moduleCount(m)));
    p.readModule(m, instance, v.data());
    return v;
}

/**
 * @brief Phase of @p y against the reference sin(2 pi ref(i)), in degrees, at the window's centre.
 *
 * A plain projection onto sin and cos is biased twice over when the window holds a fractional number
 * of cycles or the amplitude changes inside it -- both by several degrees on a decaying kick. So the
 * window is cut to a whole number of reference cycles (as many as fit in @p n, at least one), and y is
 * fitted by least squares to (a + b u) sin(2 pi ref) + (c + d u) cos(2 pi ref), u the position in the
 * window from -1/2 to 1/2: an amplitude that varies linearly no longer leaks into the phase, which is
 * atan2(c, a) at the centre.
 */
template <class RefFn>
double phaseAgainst(const float* y, size_t n, RefFn&& ref)
{
    const double r0 = ref(0);
    const double whole = std::floor(ref(n - 1) - r0);
    size_t m = n;
    if (whole >= 1.0) { m = 0; while (m < n && ref(m) - r0 < whole) ++m; }
    double A[4][5] = {};
    for (size_t i = 0; i < m; ++i) {
        const double ph = 2.0 * kPiD * ref(i);
        const double u = (static_cast<double>(i) + 0.5) / static_cast<double>(m) - 0.5;
        const double col[4] = { std::sin(ph), std::cos(ph), u * std::sin(ph), u * std::cos(ph) };
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) A[r][c] += col[r] * col[c];
            A[r][4] += col[r] * y[i];
        }
    }
    for (int k = 0; k < 4; ++k) {   // Gauss-Jordan with partial pivoting
        int piv = k;
        for (int r = k + 1; r < 4; ++r) if (std::fabs(A[r][k]) > std::fabs(A[piv][k])) piv = r;
        for (int c = 0; c < 5; ++c) std::swap(A[k][c], A[piv][c]);
        for (int r = 0; r < 4; ++r) {
            if (r == k || A[k][k] == 0.0) continue;
            const double f = A[r][k] / A[k][k];
            for (int c = k; c < 5; ++c) A[r][c] -= f * A[k][c];
        }
    }
    const double a = A[0][4] / A[0][0], c = A[1][4] / A[1][1];
    return std::atan2(c, a) * 180.0 / kPiD;
}

/**
 * @brief phaseAgainst() with the 2nd to Hth harmonics of the reference in the fit as well (19.09.2026).
 *
 * Over a whole number of cycles a *stationary* harmonic is orthogonal to the fundamental, but one whose
 * amplitude moves is not: over whole cycles u sin(2 x 2 pi ref) -- a second harmonic growing or
 * shrinking linearly -- has a component along cos(2 pi ref), which the fit reads as a phase of the
 * fundamental. And a bass note's harmonics do move -- the filter envelope and the bite
 * envelope sweep them within the two periods the fit spans. That did not matter while the bass was
 * almost pure sub; with the bite of 19.09.2026 the second harmonic stands within a few dB of the
 * fundamental, and a note whose fundamental is 48 dB clear of any filtered leak (Split, LR8 at 2 f0)
 * measured a "drift" of 10 degrees. Each harmonic k therefore gets its own four columns
 * (a_k + b_k u) sin(2 pi k ref) + (c_k + d_k u) cos(2 pi k ref); the fundamental's phase is read as
 * before. With H = 1 this is exactly phaseAgainst().
 */
template <int H, class RefFn>
double phaseAgainstH(const float* y, size_t n, RefFn&& ref)
{
    constexpr int N = 4 * H;
    const double r0 = ref(0);
    const double whole = std::floor(ref(n - 1) - r0);
    size_t m = n;
    if (whole >= 1.0) { m = 0; while (m < n && ref(m) - r0 < whole) ++m; }
    double A[N][N + 1] = {};
    for (size_t i = 0; i < m; ++i) {
        const double ph = 2.0 * kPiD * ref(i);
        const double u = (static_cast<double>(i) + 0.5) / static_cast<double>(m) - 0.5;
        double col[N];
        for (int h = 0; h < H; ++h) {
            const double s = std::sin((h + 1) * ph), c = std::cos((h + 1) * ph);
            col[4 * h] = s; col[4 * h + 1] = c; col[4 * h + 2] = u * s; col[4 * h + 3] = u * c;
        }
        for (int r = 0; r < N; ++r) {
            for (int c = 0; c < N; ++c) A[r][c] += col[r] * col[c];
            A[r][N] += col[r] * y[i];
        }
    }
    for (int k = 0; k < N; ++k) {   // Gauss-Jordan with partial pivoting
        int piv = k;
        for (int r = k + 1; r < N; ++r) if (std::fabs(A[r][k]) > std::fabs(A[piv][k])) piv = r;
        for (int c = 0; c <= N; ++c) std::swap(A[k][c], A[piv][c]);
        for (int r = 0; r < N; ++r) {
            if (r == k || A[k][k] == 0.0) continue;
            const double f = A[r][k] / A[k][k];
            for (int c = k; c <= N; ++c) A[r][c] -= f * A[k][c];
        }
    }
    const double a = A[0][N] / A[0][0], c = A[1][N] / A[1][1];
    return std::atan2(c, a) * 180.0 / kPiD;
}

/** @brief Wraps degrees into (-180, 180]. */
inline double wrapDeg(double d) { d = std::fmod(d + 180.0, 360.0); if (d < 0.0) d += 360.0; return d - 180.0; }

/** @brief Renders one bass note from silence and returns the samples. */
inline std::vector<float> bassNote(const char* settings, int pitch, int gate, size_t length, double late, double phase)
{
    ParamStore q;
    q.parseText("bass.duck_depth=0 bass.level=0");
    q.parseText(settings);
    std::vector<float> v = moduleValues(q, Module::Bass);
    Bass b;
    b.prepare(48000.0);
    b.update(v.data());
    b.noteOn(pitch, 1.0f, gate, late, phase);
    std::vector<float> y(length);
    b.process(y.data(), static_cast<int>(length));
    return y;
}

/**
 * @brief Phase relation of kick and bass at the instant the first bass note begins, measured on
 *        solo renders.
 *
 * The kick is still a little above the fundamental there (about 1 Hz at the defaults), so over a
 * two-period window it runs ahead of any fixed-frequency reference by several degrees. Each signal is
 * therefore measured against its own phase course -- the bass against its fundamental, the kick
 * against its closed-form output phase, which the kick test confirms to 0.1 degree -- and both
 * deviations are carried back to the onset instant, where the condition is defined.
 *
 * @return mean kick-minus-bass phase at the onset in degrees; @p spread receives the largest
 *         beat-to-beat change of the bass onset phase against the ideal grid
 */
inline double measureLock(const char* settings, double bpm, double& spread, double& coherentDb)
{
    const double sr = 48000.0;
    const double slotT = 0.25 * 60.0 / bpm;
    double kickPhaseAtSlot = 0.0;
    Kick kickModel;
    // Measured inside the first core: in the intro neither kick nor bass has started (Form.h). The set
    // seed is the first whose track begins its core after an eight-bar intro, so that the window is
    // reached with the shortest possible render -- this test runs twelve of them.
    int coreBeat = 0;
    uint64_t seed = 1;
    auto render = [&](const char* solo, bool keepKick) {
        auto e = std::make_unique<Engine>();
        e->prepare(sr, 256);
        // Concatenated, not formatted: this string is over 600 characters and fmt() used to cut it off
        // silently (TestSupport.h, buf). The presence probe is off -- nothing here is about levels.
        const std::string cfg = fmt("compose.bpm=%g ", bpm)
            + "compose.bass_variation=0 compose.kick_pattern=Four master.clip=Off master.limiter=Off master.clipper=Off "
              "master.comp_ratio=1 master.auto_gain=Off compose.level_match=Off compose.presence_match=Off "
              "compose.acid_amount=0 compose.lead_amount=0 compose.arp_amount=0 compose.pad_amount=0 compose.sfx_amount=0 "
            + std::string(settings) + " " + solo;
        e->params().parseText(cfg.c_str());
        for (uint64_t k = 1; k <= 40; ++k) {
            Composer probe(k);
            if (firstCoreBar(e->params(), probe) <= 8) { seed = k; break; }
        }
        Composer c(seed);
        coreBeat = firstCoreBar(e->params(), c) * kBeatsPerBar;
        std::vector<float> y = renderEngine(*e, c, coreBeat + 32.0, 256, sr);
        if (keepKick) { kickModel = e->kick(); kickPhaseAtSlot = e->kick().outputPhaseAt(slotT); }
        return y;
    };
    // Nothing but the part being measured: since Phase 5 a core carries acid, arp and pad as well, and
    // their energy leaks into a projection that is only two fundamental periods long.
    const char* const kOnlyKick = "mix.bass_mute=1 mix.perc_mute=1 mix.acid_mute=1 mix.lead_mute=1 mix.arp_mute=1 mix.pad_mute=1 mix.counter_mute=1 mix.stab_mute=1 mix.drone_mute=1 mix.sfx_mute=1";
    const char* const kOnlyBass = "mix.kick_mute=1 mix.perc_mute=1 mix.acid_mute=1 mix.lead_mute=1 mix.arp_mute=1 mix.pad_mute=1 mix.counter_mute=1 mix.stab_mute=1 mix.drone_mute=1 mix.sfx_mute=1";
    const std::vector<float> kickY = render(kOnlyKick, true), bassY = render(kOnlyBass, false);
    const double f0 = midiToHz(30);
    const double beat = 60.0 / bpm * sr;
    const size_t per = static_cast<size_t>(std::lround(sr / f0));
    double sumD = 0.0, lo = 1e9, hi = -1e9, coh = 0.0;
    int n = 0;
    // Bars 2 to 6 of the core: the last bar of every eight-bar group carries the group's figure, which
    // changes the bass pitch on purpose (Form.h), so it stays outside the window.
    for (int b = coreBeat + 8; b < coreBeat + 28; ++b) {
        const double kickIdeal = b * beat;
        const double ideal = (b + 0.25) * beat;
        const size_t w0 = static_cast<size_t>(std::ceil(ideal));
        auto refBass = [&](size_t i) { return f0 * (static_cast<double>(w0 + i) - ideal) / sr; };
        auto refKick = [&](size_t i) { return kickModel.outputPhaseAt((static_cast<double>(w0 + i) - kickIdeal) / sr); };
        const double dk = phaseAgainst(kickY.data() + w0, 2 * per + 8, refKick);   // kick = its course + dk
        const double pb = phaseAgainstH<3>(bassY.data() + w0, 2 * per + 8, refBass);   // bass phase at the onset (harmonics fitted too: phaseAgainstH)
        const double pk = 360.0 * (kickPhaseAtSlot - std::floor(kickPhaseAtSlot)) + dk;
        sumD += wrapDeg(pk - pb);
        lo = std::min(lo, pb);
        hi = std::max(hi, pb);
        double sum = 0.0, pow2 = 0.0;
        for (size_t i = 0; i < per; ++i) {
            const double s = static_cast<double>(kickY[w0 + i]) + bassY[w0 + i];
            sum += s * s;
            pow2 += static_cast<double>(kickY[w0 + i]) * kickY[w0 + i] + static_cast<double>(bassY[w0 + i]) * bassY[w0 + i];
        }
        coh += 10.0 * std::log10(sum / pow2);
        ++n;
    }
    spread = hi - lo;
    coherentDb = coh / n;
    return sumD / n;
}

/**
 * @brief Power of x[a, b) in [lo, hi) Hz, the way Tools/ref_kick.py and Tools/ref_bass.py take it.
 *
 * A flat window with a 3 ms half-cosine fade at the end only (a segment that starts on an onset must not
 * have its attack weighted down), zero-padded to a power of two of at least 2^16, the squared FFT
 * magnitudes summed over the band. Written out here rather than shared with the tools, so that the self
 * test and the Python measurement are two implementations of one definition.
 */
inline double lowendBandPower(const std::vector<float>& x, size_t a, size_t b, double lo, double hi, double sr)
{
    const size_t len = b - a;
    size_t n = 1u << 16;
    while (n < len) n <<= 1;
    std::vector<std::complex<double>> s(n);
    const size_t fade = std::min(len / 4, static_cast<size_t>(0.003 * sr));
    for (size_t i = 0; i < len; ++i) {
        double w = 1.0;
        if (fade > 0 && i + fade >= len) w = 0.5 + 0.5 * std::cos(kPiD * static_cast<double>(i + fade - len) / static_cast<double>(fade));
        s[i] = static_cast<double>(x[a + i]) * w;
    }
    fft(s);
    double p = 0.0;
    for (size_t k = 1; k < n / 2; ++k) {
        const double f = static_cast<double>(k) * sr / static_cast<double>(n);
        if (f >= lo && f < hi) p += std::norm(s[k]);
    }
    return p + 1e-30;
}

/**
 * @brief The default bass voice, rolling sixteenths on F#1 at 145 BPM with a duck on every beat, as
 *        the engine plays it (gate 0.7 of the slot, velocity 110), for @p beats beats.
 */
inline std::vector<float> lowendRollingBass(const std::vector<float>& bv, int beats, int pitch = 30)
{
    const double sr = 48000.0, beat = 60.0 / 145.0 * sr, slot = beat / 4.0;
    Bass b;
    b.prepare(sr);
    b.update(bv.data());
    std::vector<float> y(static_cast<size_t>(beats * beat) + 16);
    size_t done = 0;
    for (int k = 0; k < beats * 4; ++k) {
        const double t = k * slot;
        const size_t at = static_cast<size_t>(std::ceil(t));
        if (at > done) { b.process(y.data() + done, static_cast<int>(at - done)); done = at; }
        if (k % 4 == 0) { b.duck(static_cast<double>(at) - t); continue; }   // the kick's step: no note
        b.noteOn(pitch, 110.0f / 127.0f, static_cast<int>(0.7 * slot), static_cast<double>(at) - t, 0.0);
    }
    b.process(y.data() + done, static_cast<int>(y.size() - done));
    return y;
}

/**
 * @brief The acid voicings of a track: a point in the triangle clean / driven / liquid (19.09.2026).
 *
 * Checked against the voicing values written out here, independently of the table in Composer.cpp:
 * clean = drive 0.1, accent 0.8, low cut 150 Hz; liquid = wave 0.5, cutoff 450 Hz, 4 disperser stages.
 */
/**
 * @brief A composer of the listening seed 864566672 at the default knobs, for its track *plans*.
 *
 * With the default knobs the level match is on, and every plan renders about 48 bars of level probes
 * (about 10 s a plan on 19.09.2026). testVoices parts `.sound` (tracks 1-20) and `.counterSoundListening`
 * (0-23) and testAcidVoicing part `.corners` (0-3) all plan these same tracks with the same knobs; in
 * one process (the full serial run) they are planned once. As separate ctest tests each process
 * plans what it needs, as before. Only plans are asked of it -- nothing is composed with it -- so
 * the plans are the ones a fresh composer gives (plans are made in track order either way).
 */
struct ListeningPlanner {
    ParamStore p;                    ///< default knobs
    std::unique_ptr<Composer> c;     ///< the composer, asked only for track()
};

/** @brief The shared ListeningPlanner, made on first use. */
inline ListeningPlanner& listeningPlanner()
{
    static std::unique_ptr<ListeningPlanner> s;
    if (!s) {
        s = std::make_unique<ListeningPlanner>();
        s->c = std::make_unique<Composer>(864566672ull);
    }
    return *s;
}

/**
 * @brief The value an acid parameter takes at the default knobs plus a voicing offset (normalised domain,
 *        as the engine applies it). Shared by testAcidVoicing's parts `.corners` and `.engine`.
 */
inline float acidValueAt(const ParamStore& p, int param, const float* off)
{
    const int id = p.base(Module::Acid) + param;
    return p.fromNormalised(id, p.toNormalised(id, p.get(id)) + off[param]);
}

/**
 * @brief The phase of the kick against the bass at the **first onset of every beat**, with a rhythm
 *        whose first onset is a different sixteenth from beat to beat.
 *
 * testPhaseLock measures the same thing for the pattern families and can assume the onset is always
 * a quarter beat after the kick; here the instant moves, which is the whole point of the round, so
 * the onsets are read from the composer's own note list and each one is fitted against a reference
 * sine at its own pitch. "Bass follows kick" is the mode measured because there the kick is not
 * retrimmed for a phase target (`Kick::setPhaseTarget(0, 0)` in `Engine::applyParams`), so one kick
 * model describes every beat and the kick's own phase at an arbitrary instant can be predicted.
 *
 * The measurement carries a bias of its own that has nothing to do with the lock: the fit runs over
 * two periods of the note's fundamental, during which the bass's own filter envelope moves, and how
 * far it moves depends on how long the note is -- which under a drawn rhythm is no longer the same
 * for every note. The caller therefore runs the same function over the **pattern families** as well
 * and compares the two, instead of holding the free rhythm against an absolute bound that would be
 * measuring the envelope as much as the lock.
 *
 * @param bpm           tempo
 * @param rhythm        "Pattern" or "Corpus" for compose.bass_rhythm
 * @param onsets        receives how many onsets were measured
 * @param distinctSlots receives how many different sixteenths carried the first onset of a beat
 * @return the mean of kick phase minus bass phase at the onsets, degrees
 */
inline double measureFreeLock(double bpm, const char* rhythm, int& onsets, int& distinctSlots)
{
    const double sr = 48000.0;
    Kick kickModel;
    int coreBeat = 0;
    uint64_t seed = 1;
    std::vector<NoteEvent> notes;
    const std::string base =
        fmt("compose.bpm=%g compose.bass_rhythm=%s compose.bass_variation=1 bass.kick_lock=Bass follows kick "
            "compose.kick_pattern=Four master.clip=Off master.limiter=Off master.clipper=Off master.comp_ratio=1 "
            "master.auto_gain=Off compose.level_match=Off compose.acid_amount=0 compose.lead_amount=0 "
            "compose.arp_amount=0 compose.pad_amount=0 compose.sfx_amount=0", bpm, rhythm);
    auto render = [&](const char* solo, bool keepKick) {
        auto e = std::make_unique<Engine>();
        e->prepare(sr, 256);
        e->params().parseText((base + " " + solo).c_str());
        // A seed whose drawn rhythm moves its first onset between sixteenths within the eight bars of the
        // window. Until 19.09.2026 this took the first seed whose core began by bar 8 and relied on its
        // rhythm moving; the two-drop form starts every groove on bar 33, and the seed found that way played
        // one slot only. The window covers one whole eight-bar phrase of the masks.
        for (uint64_t k = 1; k <= 40; ++k) {
            Composer probe(k);
            const TrackPlan t = probe.track(e->params(), 0);
            std::set<int> firsts;
            for (int b = 0; b < kBassPhraseBars; ++b)
                for (int beat = 0; beat < kBeatsPerBar; ++beat)
                    for (int s = 1; s < 4; ++s)
                        if ((t.bassMask[0][b] >> (beat * 4 + s)) & 1u) { firsts.insert(s); break; }
            if (!t.bassRhythm || firsts.size() >= 2) { seed = k; break; }
        }
        Composer c(seed);
        coreBeat = firstCoreBar(e->params(), c) * kBeatsPerBar;
        if (notes.empty()) c.composeBars(e->params(), 0, coreBeat / kBeatsPerBar + 32, notes);
        std::vector<float> y = renderEngine(*e, c, coreBeat + 32.0, 256, sr);
        if (keepKick) kickModel = e->kick();
        return y;
    };
    const char* const kOnlyKick = "mix.bass_mute=1 mix.perc_mute=1 mix.acid_mute=1 mix.lead_mute=1 mix.arp_mute=1 mix.pad_mute=1 mix.counter_mute=1 mix.stab_mute=1 mix.drone_mute=1 mix.sfx_mute=1";
    const char* const kOnlyBass = "mix.kick_mute=1 mix.perc_mute=1 mix.acid_mute=1 mix.lead_mute=1 mix.arp_mute=1 mix.pad_mute=1 mix.counter_mute=1 mix.stab_mute=1 mix.drone_mute=1 mix.sfx_mute=1";
    const std::vector<float> kickY = render(kOnlyKick, true), bassY = render(kOnlyBass, false);

    // The first bass onset of every beat of the window, with its pitch.
    std::map<int, std::pair<double, int>> first;   // beat -> (offset in beats, pitch)
    for (const NoteEvent& n : notes) {
        if (n.part != Part::Bass) continue;
        const int b = static_cast<int>(std::floor(n.beat + 1e-9));
        const double off = n.beat - b;
        auto it = first.find(b);
        if (it == first.end() || off < it->second.first) first[b] = { off, static_cast<int>(n.pitch) };
    }
    const double beatSamples = 60.0 / bpm * sr;
    double sumD = 0.0;
    std::set<int> slots;
    int n = 0;
    for (int b = coreBeat + 8; b < coreBeat + 28; ++b) {
        auto it = first.find(b);
        if (it == first.end()) continue;
        const double slot = it->second.first;
        const double f0 = midiToHz(it->second.second);
        const double kickIdeal = b * beatSamples;
        const double ideal = (b + slot) * beatSamples;
        const size_t w0 = static_cast<size_t>(std::ceil(ideal));
        const size_t win = static_cast<size_t>(std::lround(2.0 * sr / f0)) + 8;
        if (w0 + win >= bassY.size() || w0 + win >= kickY.size()) continue;
        auto refBass = [&](size_t i) { return f0 * (static_cast<double>(w0 + i) - ideal) / sr; };
        auto refKick = [&](size_t i) { return kickModel.outputPhaseAt((static_cast<double>(w0 + i) - kickIdeal) / sr); };
        const double dk = phaseAgainst(kickY.data() + w0, win, refKick);
        const double pb = phaseAgainstH<3>(bassY.data() + w0, win, refBass);   // harmonics fitted too (phaseAgainstH)
        const double atSlot = kickModel.outputPhaseAt(slot * beatSamples / sr);
        const double pk = 360.0 * (atSlot - std::floor(atSlot)) + dk;
        sumD += wrapDeg(pk - pb);
        slots.insert(static_cast<int>(std::lround(slot * 4.0)));
        ++n;
    }
    onsets = n;
    distinctSlots = static_cast<int>(slots.size());
    return n > 0 ? sumD / n : 1e9;
}

/** @brief The bass notes of a composed stretch. */
inline std::vector<NoteEvent> bassNotesOf(const std::vector<NoteEvent>& all)
{
    std::vector<NoteEvent> out;
    for (const NoteEvent& n : all) if (n.part == Part::Bass) out.push_back(n);
    return out;
}

/** @brief Onset mask per bar of a composed stretch, from the note beats alone. */
inline std::map<int, unsigned> barMasksOf(const std::vector<NoteEvent>& all)
{
    std::map<int, unsigned> bars;
    for (const NoteEvent& n : all) {
        if (n.part != Part::Bass) continue;
        const int bar = static_cast<int>(std::floor(n.beat / kBeatsPerBar + 1e-9));
        const int step = static_cast<int>(std::lround((n.beat - static_cast<double>(bar) * kBeatsPerBar) * 4.0));
        if (step >= 0 && step < 16) bars[bar] |= 1u << step;
    }
    return bars;
}

/** @brief A kit prepared from a parameter store (all lanes updated). */
inline std::unique_ptr<PercKit> makeKit(const ParamStore& p, int key = 6, int scale = 1)
{
    auto kit = std::make_unique<PercKit>();
    kit->prepare(48000.0);
    for (int l = 0; l < kPercLanes; ++l) {
        std::vector<float> v = moduleValues(p, Module::Perc, l);
        kit->update(l, v.data(), key, scale);
    }
    return kit;
}

/** @brief Renders @p n samples of a kit into mono (L + R) / 2. */
inline std::vector<float> renderKit(PercKit& kit, size_t n)
{
    const DenormalGuard guard;
    std::vector<float> L(n), R(n), m(n);
    for (size_t done = 0; done < n;) {
        const int k = static_cast<int>(std::min<size_t>(64, n - done));
        kit.process(L.data() + done, R.data() + done, k);
        done += static_cast<size_t>(k);
    }
    for (size_t i = 0; i < n; ++i) m[i] = 0.5f * (L[i] + R[i]);
    return m;
}

/** @brief Share of power below @p hz, in dB relative to the total. */
inline double lowShareDb(const std::vector<float>& x, double hz, bool windowed = false)
{
    size_t n = 1;
    while (n * 2 <= x.size()) n *= 2;
    // Windowed (Blackman-Harris) for sustained notes, where the leakage of a strong line just above the
    // limit (an acid note at 147 Hz) would otherwise count as power below it. Unwindowed for hits that
    // start at the first sample, whose attack a window would hide.
    std::vector<double> spec;
    if (windowed) {
        spec = powerSpectrum(x.data(), n);
    } else {
        std::vector<std::complex<double>> a(n);
        for (size_t i = 0; i < n; ++i) a[i] = x[i];
        fft(a);
        spec.resize(n / 2 + 1);
        for (size_t k = 0; k <= n / 2; ++k) spec[k] = std::norm(a[k]);
    }
    double lo = 0.0, all = 0.0;
    for (size_t k = 1; k < n / 2; ++k) {
        const double pw = spec[k];
        all += pw;
        if (static_cast<double>(k) * 48000.0 / n < hz) lo += pw;
    }
    return powDb(lo / all);
}

/** @brief A small order-2 model with a dense random table, for checks against brute force. */
struct ToyModel {
    int n = 4;
    std::vector<double> p;
    explicit ToyModel(uint64_t seed)
    {
        Rng r;
        r.seed(seed);
        p.resize(static_cast<size_t>(n * n * n));
        for (int ab = 0; ab < n * n; ++ab) {
            double sum = 0.0;
            for (int c = 0; c < n; ++c) { p[static_cast<size_t>(ab * n + c)] = 0.05 + r.uniform(); sum += p[static_cast<size_t>(ab * n + c)]; }
            for (int c = 0; c < n; ++c) p[static_cast<size_t>(ab * n + c)] /= sum;
        }
    }
    int alphabet() const { return n; }
    double prob(int a, int b, int c) const { return p[static_cast<size_t>((a * n + b) * n + c)]; }
};

/** @brief One case of a `<model>.ref.txt`: what the trainer's PyTorch computed for that input. */
struct RefCase {
    int role = 0, style = 0, bars = 0;
    std::vector<int> tok, step, bar, idx, gap;
    std::vector<int> kick;   ///< empty in a reference file of a three-role model (MODEL_FORMAT 3)
    int mode = -1;           ///< -1 when the case records no mode, i.e. a file without condMode
    std::vector<double> logits, probs;
};

/**
 * @brief Reads a reference file (docs/MODEL_FORMAT.md, section 5).
 *
 * Deliberately forgiving about everything it does not know, because the file is written by another
 * program on the other side of the contract: unknown keys are skipped and fields inside a case may
 * come in any order.
 */
inline std::vector<RefCase> readRefCases(const std::string& path)
{
    std::vector<RefCase> cases;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.compare(0, 5, "case ") == 0) { cases.push_back(RefCase{}); continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || cases.empty()) continue;
        const std::string key = line.substr(0, eq);
        std::istringstream vals(line.substr(eq + 1));
        RefCase& c = cases.back();
        auto ints = [&](std::vector<int>& dst) { dst.clear(); int v; while (vals >> v) dst.push_back(v); };
        auto reals = [&](std::vector<double>& dst) { dst.clear(); double v; while (vals >> v) dst.push_back(v); };
        if (key == "role") vals >> c.role;
        else if (key == "style") vals >> c.style;
        else if (key == "bars") vals >> c.bars;
        else if (key == "mode") vals >> c.mode;
        else if (key == "tok") ints(c.tok);
        else if (key == "step") ints(c.step);
        else if (key == "bar") ints(c.bar);
        else if (key == "gap") ints(c.gap);
        else if (key == "idx") ints(c.idx);
        else if (key == "kick") ints(c.kick);
        else if (key == "logits") reals(c.logits);
        else if (key == "probs") reals(c.probs);
    }
    return cases;
}

/** @brief The directory the models live in (Core/data, compiled in by Core/CMakeLists.txt). */
inline std::string modelPath(const char* name) { return std::string(PHOS_SOURCE_DATA_DIR) + "/" + name; }

/** @brief Feeds one reference case into a model; false when the file and the case disagree. */
inline bool feedRefCase(NeuralModel& model, const RefCase& c)
{
    const size_t n = c.tok.size();
    if (n == 0 || c.step.size() != n || c.bar.size() != n || c.gap.size() != n || c.idx.size() != n) return false;
    // A four-role file's cases carry a kick class per position; a three-role file's do not, and then
    // the model has no kick.emb either and the field is not read (docs/MODEL_FORMAT.md, section 3).
    if (!c.kick.empty() && c.kick.size() != n) return false;
    // A model that declares condMode has to be fed the case's mode, and a case that records none
    // cannot judge such a model at all: the logits would be recomputed under a row the trainer never
    // used and the mismatch would read as a reader bug. Refusing instead is the rule the other side
    // of the contract follows too (_conditioning_gap in Tools/train/export.py).
    if (model.info().condMode > 0 && c.mode < 0) return false;
    model.begin(c.role, c.style, c.bars + 1, c.mode < 0 ? 0 : c.mode);   // the file stores bars - 1
    for (size_t t = 0; t < n; ++t) {
        NoteCond cond;
        cond.step = c.step[t];
        cond.bar = c.bar[t];
        cond.gap = c.gap[t];
        cond.idx = c.idx[t];
        cond.kick = c.kick.empty() ? 0 : c.kick[t];
        if (!model.step(c.tok[t], cond)) return false;
    }
    return true;
}

/** @brief The order-2 toy model of testSampler, driven the way sampleMasked() drives a network. */
struct ToyStepper {
    const ToyModel* model = nullptr;
    int a = 0, b = 0;
    int alphabet() const { return model->alphabet(); }
    void begin(int start2, int start1) { a = start2; b = start1; }
    bool advance(int) { return true; }
    void observe(int s) { a = b; b = s; }
    double prob(int c) const { return model->prob(a, b, c); }
};

/** @brief A stepper whose distribution sits entirely on one symbol: the numerical failure case. */
struct PeakedStepper {
    int n = 4, peak = 0;
    int alphabet() const { return n; }
    void begin(int, int) {}
    bool advance(int) { return true; }
    void observe(int) {}
    double prob(int c) const { return c == peak ? 1.0 - 3e-9 : 1e-9; }
};

/** @brief Scale tones in [lo, hi] semitones above the root, with the colour weight on the flat second. */
inline std::vector<uint8_t> scaleAllowed(int scale, int lo, int hi, int colour)
{
    std::vector<uint8_t> a(static_cast<size_t>(kCorpusAlphabet), 0);
    for (int rel = lo; rel <= hi; ++rel) {
        if (rel < kCorpusRelMin || rel > kCorpusRelMax || !inScale(scale, rel)) continue;
        a[static_cast<size_t>(PitchModel::symbol(rel))] = static_cast<uint8_t>(((rel % 12 + 12) % 12) == 1 ? colour : 8);
    }
    return a;
}

/** @brief The tones of the chord on @p degree of @p scale, in [lo, hi]. */
inline std::vector<uint8_t> chordAllowed(int scale, int degree, int lo, int hi)
{
    int pcs[3];
    chordTones(scale, degree, pcs);
    std::vector<uint8_t> a(static_cast<size_t>(kCorpusAlphabet), 0);
    for (int rel = lo; rel <= hi; ++rel) {
        if (rel < kCorpusRelMin || rel > kCorpusRelMax) continue;
        const int pc = ((rel % 12) + 12) % 12;
        if (pc == pcs[0] || pc == pcs[1] || pc == pcs[2]) a[static_cast<size_t>(PitchModel::symbol(rel))] = 1;
    }
    return a;
}

/** @brief Steady-state amplitude of a filter's response to a sine (amplitude @p amp) after @p settle samples. */
template <class Tick>
double sineGain(Tick&& tick, double hz, double sr, double amp, size_t settle, size_t window)
{
    std::vector<float> y(window);
    for (size_t i = 0; i < settle + window; ++i) {
        const float x = static_cast<float>(amp * std::sin(2.0 * kPiD * hz * static_cast<double>(i) / sr));
        const float o = tick(x);
        if (i >= settle) y[i - settle] = o;
    }
    return toneAmplitude(y.data(), window, hz * static_cast<double>(window) / sr) / amp;
}

/** @brief An acid voice with the given settings. */
inline std::unique_ptr<Acid> makeAcid(const char* settings, ParamStore& p)
{
    p.parseText(settings);
    auto a = std::make_unique<Acid>();
    a->prepare(48000.0);
    std::vector<float> v = moduleValues(p, Module::Acid);
    a->update(v.data(), 145.0);
    return a;
}

/** @brief Renders @p n samples with @p proc and returns their mono mix. */
inline std::vector<float> renderMono(const std::function<void(float*, float*, int)>& proc, size_t n)
{
    const DenormalGuard guard;
    std::vector<float> L(n), R(n), m(n);
    for (size_t done = 0; done < n;) {
        const int k = static_cast<int>(std::min<size_t>(64, n - done));
        proc(L.data() + done, R.data() + done, k);
        done += static_cast<size_t>(k);
    }
    for (size_t i = 0; i < n; ++i) m[i] = 0.5f * (L[i] + R[i]);
    return m;
}

/** @brief Power-weighted spectral centroid of @p n samples (Blackman-Harris). */
inline double centroid(const float* x, double sr, size_t n)
{
    const std::vector<double> pw = powerSpectrum(x, n);
    double num = 0.0, den = 0.0;
    for (size_t k = 1; k < pw.size(); ++k) { num += pw[k] * k * sr / static_cast<double>(n); den += pw[k]; }
    return num / den;
}

/**
 * @brief Power outside +-6 bins of any line the seven detuned oscillators legitimately make.
 *
 * inharmonicDb() counts everything that is not a harmonic of f0 as aliasing, which for a detuned
 * supersaw is six of its seven oscillators -- at detune 1 it reads +8 dB whatever the oscillator
 * does, and says nothing about aliasing. The legitimate line set of a supersaw is
 * {h (1 + a_u y) f0}; everything else between 100 Hz and 18 kHz is aliasing. The bands around the
 * legitimate lines cover a part of the spectrum (@p coverage), and that part of the aliasing is not
 * counted; at the notes measured here it is 4 to 25 per cent, less than a dB of bias.
 */
inline double supersawAliasDb(const std::vector<float>& y, size_t offset, double f0, double detuneY, double sr, double* coverage = nullptr)
{
    constexpr size_t N = 65536;
    const std::vector<double> pw = powerSpectrum(y.data() + offset, N);
    std::vector<char> legit(N / 2 + 1, 0);
    const double nyq = 0.5 * sr;
    for (int u = 0; u < kPolyUnison; ++u) {
        const double fu = f0 * (1.0 + kSupersawOffsets[u] * detuneY);
        for (int h = 1; static_cast<double>(h) * fu < nyq; ++h) {
            const double k = static_cast<double>(h) * fu * static_cast<double>(N) / sr;
            for (int b = static_cast<int>(std::floor(k)) - 6; b <= static_cast<int>(std::ceil(k)) + 6; ++b)
                if (b >= 0 && b <= static_cast<int>(N / 2)) legit[static_cast<size_t>(b)] = 1;
        }
    }
    double on = 0.0, off = 0.0;
    size_t bins = 0, onBins = 0;
    for (size_t k = 1; k < N / 2; ++k) {
        const double hz = static_cast<double>(k) * sr / N;
        if (hz < 100.0 || hz > 18000.0) continue;
        ++bins;
        if (legit[k] != 0) { on += pw[k]; ++onBins; } else off += pw[k];
    }
    if (coverage != nullptr) *coverage = static_cast<double>(onBins) / static_cast<double>(bins);
    return powDb(off / on);
}

/**
 * @brief Power above @p fromHz that sits on none of @p lines, against the power at @p fundHz.
 *
 * A two-operator FM spectrum is the carrier plus sidebands at |f_c + k f_m| for every integer k; with
 * a non-integer ratio those are not harmonics of f_c, so a harmonic mask would count the instrument's
 * own sidebands as aliasing. Only the sidebands whose *unfolded* frequency lies above Nyquist come
 * back as aliasing, and they land where no legitimate line is.
 */
inline double lineAliasDb(const std::vector<float>& y, size_t offset, const std::vector<double>& lines, double fundHz, double sr, double fromHz)
{
    constexpr size_t N = 65536;
    const std::vector<double> pw = powerSpectrum(y.data() + offset, N);
    std::vector<char> legit(N / 2 + 1, 0);
    for (double hz : lines) {
        const double k = hz * static_cast<double>(N) / sr;
        for (int b = static_cast<int>(std::floor(k)) - 6; b <= static_cast<int>(std::ceil(k)) + 6; ++b)
            if (b >= 0 && b <= static_cast<int>(N / 2)) legit[static_cast<size_t>(b)] = 1;
    }
    double rest = 0.0, fund = 0.0;
    const size_t k1 = static_cast<size_t>(std::lround(fundHz * static_cast<double>(N) / sr));
    for (size_t k = k1 - 4; k <= k1 + 4; ++k) fund += pw[k];
    for (size_t k = 1; k < N / 2; ++k) {
        const double hz = static_cast<double>(k) * sr / N;
        if (hz < fromHz || hz > 23000.0) continue;
        if (legit[k] == 0) rest += pw[k];
    }
    return powDb(rest / fund);
}

/** @brief The lines a two-operator voice at @p f0 with modulator ratio @p ratio may legitimately show. */
inline std::vector<double> fmLines(double f0, double ratio, double sr)
{
    std::vector<double> v;
    for (int k = -400; k <= 400; ++k) {
        const double hz = std::fabs(f0 * (1.0 + k * ratio));
        if (hz > 20.0 && hz < 0.5 * sr) v.push_back(hz);
    }
    return v;
}

/** @brief The harmonics of @p f0 below Nyquist. */
inline std::vector<double> harmonicLines(double f0, double sr)
{
    std::vector<double> v;
    for (int h = 1; h * f0 < 0.5 * sr; ++h) v.push_back(h * f0);
    return v;
}

/** @brief A polyphonic engine instance with the given settings. */
inline std::unique_ptr<Poly> makePoly(const char* settings, ParamStore& p, PolyInstance inst)
{
    p.parseText(settings);
    auto e = std::make_unique<Poly>();
    e->prepare(48000.0);
    std::vector<float> v = moduleValues(p, Module::Poly, static_cast<int>(inst));
    e->update(v.data(), 145.0);
    return e;
}

/**
 * @brief Magnitude of a partial: the root of the summed power of its main lobe around @p hz.
 *
 * The same estimator as Tools/ref_harmonics.py, and for the same reason: a Blackman-Harris main lobe
 * is eight bins wide and a partial almost never sits on a bin centre, so the largest single bin
 * underestimates it by a frequency-dependent amount that would go straight into a ratio of two
 * partials.
 */
inline double partialMag(const std::vector<double>& pw, double hz, double sr, size_t n)
{
    const double k = hz * static_cast<double>(n) / sr;
    const int lo = std::max(0, static_cast<int>(std::lround(k)) - 4);
    const int hi = std::min(static_cast<int>(n / 2), static_cast<int>(std::lround(k)) + 4);
    double s = 0.0;
    for (int b = lo; b <= hi; ++b) s += pw[static_cast<size_t>(b)];
    return std::sqrt(s);
}

/**
 * @brief Slope-free evenness at @p f0: 20 log10(a_n / sqrt(a_(n-1) a_(n+1))) for the even harmonic @p n.
 *
 * Comparing an even partial with the geometric mean of its two odd neighbours divides out any smooth
 * spectral envelope through that region, so the number says how much more or less even content there
 * is than a smoothly falling series would carry -- not how bright the signal is. For a sawtooth
 * (a_n = 1/n) the value is 10 log10(1 - 1/n^2): -1.249 dB at n = 2, -0.280 dB at n = 4; for a square
 * wave it is minus infinity. It is the measure of Tools/ref_harmonics.py, so our renders and the
 * reference recordings produce the same kind of number.
 */
inline double evennessDb(const std::vector<float>& y, size_t offset, double f0, double sr, int n)
{
    constexpr size_t N = 32768;
    const std::vector<double> pw = powerSpectrum(y.data() + offset, N);
    const double a = partialMag(pw, static_cast<double>(n - 1) * f0, sr, N);
    const double b = partialMag(pw, static_cast<double>(n) * f0, sr, N);
    const double c = partialMag(pw, static_cast<double>(n + 1) * f0, sr, N);
    return 20.0 * std::log10(b / (std::sqrt(a * c) + 1e-30) + 1e-30);
}

/** @brief |H| in dB and the group delay in ms of a disperser chain at @p hz, from its coefficients. */
inline void disperserResponse(const Disperser& d, double hz, double sr, double& magDb, double& gdMs)
{
    auto at = [&](double f) {
        const double w = 2.0 * kPiD * f / sr;
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
        std::complex<double> h(1.0, 0.0);
        for (int i = 0; i < d.stages; ++i)
            h *= (static_cast<double>(d.c[i]) + static_cast<double>(d.d[i]) * z1 + z2)
               / (1.0 + static_cast<double>(d.d[i]) * z1 + static_cast<double>(d.c[i]) * z2);
        return h;
    };
    const std::complex<double> h = at(hz);
    magDb = 20.0 * std::log10(std::abs(h));
    const double e = std::max(0.5, hz * 1e-3);
    gdMs = -std::arg(at(hz + e) / h) / (2.0 * kPiD * e) * 1000.0;
}

/**
 * @brief Power between 100 Hz and 18 kHz that sits on none of @p lines, against the power that does.
 *
 * The same rule as supersawAliasDb() -- +-6 bins of a 65536-point spectrum around every legitimate
 * line -- but with the line set handed in rather than reconstructed from f0 and the detune, so that
 * a drifted oscillator can be measured against the lines it actually makes.
 */
inline double maskedAliasDb(const std::vector<float>& y, size_t offset, const std::vector<double>& lines, double sr)
{
    constexpr size_t N = 65536;
    const std::vector<double> pw = powerSpectrum(y.data() + offset, N);
    std::vector<char> legit(N / 2 + 1, 0);
    for (double hz : lines) {
        const double k = hz * static_cast<double>(N) / sr;
        for (int b = static_cast<int>(std::floor(k)) - 6; b <= static_cast<int>(std::ceil(k)) + 6; ++b)
            if (b >= 0 && b <= static_cast<int>(N / 2)) legit[static_cast<size_t>(b)] = 1;
    }
    double on = 0.0, off = 0.0;
    for (size_t k = 1; k < N / 2; ++k) {
        const double hz = static_cast<double>(k) * sr / N;
        if (hz < 100.0 || hz > 18000.0) continue;
        if (legit[k] != 0) on += pw[k]; else off += pw[k];
    }
    return powDb(off / on);
}

/**
 * @brief Resonance peak of a feedback comb read with four interpolation weights, in the time domain.
 * @param taps  weights of the samples at offsets -1, 0, +1, +2 from the integer read position
 * @param delay integer part of the delay in samples
 * @param fb    feedback
 * @param nearHz the comb peak nearest this frequency is the one reported
 * @return the peak's gain in dB over the direct path
 *
 * The comb is excited with a unit impulse and its response transformed without a window, so the
 * measured spectrum is the transfer function itself and the reference is the unit input.
 * @param sr     sample rate, Hz
 */
inline double combPeakDb(const float taps[4], int delay, float fb, double nearHz, double sr)
{
    constexpr size_t N = 32768;
    std::vector<float> buf(4096, 0.0f);
    std::vector<std::complex<double>> y(N);
    size_t pos = 0;
    const size_t mask = buf.size() - 1;
    for (size_t i = 0; i < N; ++i) {
        const long long i0 = static_cast<long long>(pos) - delay;
        float d = 0.0f;
        for (int t = 0; t < 4; ++t) d += taps[t] * buf[static_cast<size_t>(i0 - 1 + t) & mask];
        const float v = (i == 0 ? 1.0f : 0.0f) + fb * d;
        buf[pos] = v;
        pos = (pos + 1) & mask;
        y[i] = v;
    }
    fft(y);
    // Half the peak spacing (sr / delay) on both sides, so exactly one peak is always inside.
    const int span = static_cast<int>(N) / (2 * delay) + 2;
    const int k0 = static_cast<int>(std::lround(nearHz * static_cast<double>(N) / sr));
    double best = 0.0;
    for (int k = std::max(1, k0 - span); k <= std::min(static_cast<int>(N / 2), k0 + span); ++k)
        best = std::max(best, std::abs(y[static_cast<size_t>(k)]));
    return 20.0 * std::log10(best);
}

/**
 * @brief Power share of everything outside +-6 bins of the harmonics of @p f0 between 100 Hz and 18 kHz.
 *
 * Six bins, not three: the Blackman-Harris window's main lobe is four bins either side, and at three the
 * leakage of the fundamental counted as aliasing (-39 dB where the signal had -80).
 */
inline double inharmonicDb(const std::vector<float>& y, size_t offset, double f0, double sr, double relTolerance = 0.0)
{
    constexpr size_t N = 65536;
    const std::vector<double> pw = powerSpectrum(y.data() + offset, N);
    double harm = 0.0, rest = 0.0;
    for (size_t k = 1; k < N / 2; ++k) {
        const double hz = static_cast<double>(k) * sr / N;
        if (hz < 100.0 || hz > 18000.0) continue;
        const double h = std::round(hz / f0);
        const bool onHarmonic = h >= 1.0 && std::fabs(hz - h * f0) <= 6.0 * sr / N + relTolerance * hz;
        (onHarmonic ? harm : rest) += pw[k];
    }
    return powDb(rest / harm);
}

/** @brief Renders one sustained note of a Poly at @p rate; 96 kHz is decimated to 48 kHz. */
inline std::vector<float> renderPolyNote(const std::string& settings, PolyInstance inst, int pitch, size_t n, double rate)
{
    ParamStore p;
    p.parseText(settings);
    auto e = std::make_unique<Poly>();
    e->prepare(rate);
    std::vector<float> v = moduleValues(p, Module::Poly, static_cast<int>(inst));
    e->update(v.data(), 145.0);
    e->noteOn(pitch, 1.0f, 8.0, 1 << 24, 0.0);
    if (rate <= 48000.5) return renderMono([&](float* L, float* R, int k) { e->process(L, R, k); }, n);
    HalfbandDown<float> down;
    down.setup(designHalfband(96.0, 0.1));
    const std::vector<float> hi = renderMono([&](float* L, float* R, int k) { e->process(L, R, k); }, n * 2);
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = down.process(hi[2 * i], hi[2 * i + 1]);
    return out;
}

/** @brief The settings both paths share, with an instance prefix. */
inline std::string commonPoly(const char* pfx, double detune)
{
    std::string s;
    static const char* const kKeys[] = { "cutoff=18000", "env_amount=0", "key_track=0", "resonance=0", "hp_track=0", "hp_floor=150",
                                         "width=0", "delay_send=0", "dynamic_detune=0", "amp_attack=1", "amp_sustain=1",
                                         "amp_decay=4000", "vel_sens=0", "mix=0.75", "gate=0" };
    for (const char* k : kKeys) s += std::string(pfx) + "." + k + " ";
    return s + fmt("%s.detune=%g ", pfx, detune);
}

/**
 * @brief Aliasing of one frame of a table played at @p noteHz, in dB under its harmonic power.
 *
 * The same measurement `Tools/wt_select.py` makes, so the two can be compared: the pitch is moved
 * to the nearest exact bin of the 65536-point analysis, the note reads the level `waveLevelFor()`
 * gives it, and every bin between 100 Hz and 18 kHz that is not a multiple of the fundamental is
 * alias. No window and no band around a line, because with a bin-exact pitch there is no leakage
 * to argue about -- which is what makes it comparable across two implementations at all.
 *
 * The phase is `fmod(i * step, 1)` rather than an accumulator: over 65536 samples an accumulator
 * drifts by its own rounding, and the drift would show up here as a broadening of every line. The
 * engine accumulates (Poly.cpp) because it has to; a measurement need not.
 */
inline double tableFrameAliasDb(const WaveTable& t, int frame, double noteHz, double sr)
{
    constexpr size_t N = 65536;
    const int k = std::max(1, static_cast<int>(std::lround(noteHz * N / sr)));
    const double f0 = k * sr / N;
    const int level = waveLevelFor(f0, sr, -1);
    const double step = f0 / sr;
    std::vector<std::complex<double>> a(N);
    for (size_t i = 0; i < N; ++i)
        a[i] = t.sample(level, frame, std::fmod(static_cast<double>(i) * step, 1.0));
    fft(a);
    const size_t lo = static_cast<size_t>(std::ceil(100.0 * N / sr)), hi = static_cast<size_t>(std::floor(18000.0 * N / sr));
    double sig = 0.0, noise = 0.0;
    for (size_t b = lo; b <= hi && b < N / 2; ++b)
        ((b % static_cast<size_t>(k)) == 0 ? sig : noise) += std::norm(a[b]);
    return powDb(noise / sig);
}

/** @brief RMS of the stored cycle of one frame at a level. */
inline double tableFrameRms(const WaveTable& t, int level, int frame)
{
    const int len = WaveTable::levelLength(level);
    double e = 0.0;
    for (int n = 0; n < len; ++n) { const double x = t.cycle(level, frame)[n]; e += x * x; }
    return std::sqrt(e / len);
}

/**
 * @brief Normalised power spectrum of one frame at level 0, as `Tools/wt_select.py` computes it.
 *
 * The selection measured the source `.wav` cycles; this measures the stored level-0 cycle, which is
 * the same waveform after buildFromHarmonics(). The absolute numbers therefore need not equal the
 * table in docs/rounds/2026-09.md to the last digit -- what is compared here is one frame limit against
 * another *inside this one measurement*, and for that both sides go through the same code.
 * @return bins 1 .. 512 (harmonic h in element h-1), summing to 1; empty for a silent frame
 */
inline std::vector<double> framePowerSpectrum(const WaveTable& t, int frame)
{
    const size_t n = static_cast<size_t>(WaveTable::levelLength(0));
    std::vector<std::complex<double>> a(n);
    const float* c = t.cycle(0, frame);
    for (size_t i = 0; i < n; ++i) a[i] = c[i];
    fft(a);
    std::vector<double> p(static_cast<size_t>(WaveTable::levelHarmonics(0)), 0.0);
    double total = 0.0;
    for (size_t h = 1; h <= p.size() && h < n / 2; ++h) { p[h - 1] = std::norm(a[h]); total += p[h - 1]; }
    if (total <= 0.0) return {};
    for (double& x : p) x /= total;
    return p;
}

/** @brief The spectral-evolution numbers of one table: the steps between neighbouring frames. */
struct MorphMeasure {
    int frames = 0;
    double move = 0.0;        ///< median total variation between neighbouring frames, in [0, 1]
    double moveMax = 0.0;     ///< the largest single step: the worst jump the position knob walks over
    double travel = 0.0;      ///< total variation between the first and the last frame
    double path = 0.0;        ///< the sum of all the steps
    double directness = 0.0;  ///< travel / path, at most 1 by the triangle inequality
};

/** @brief measure_table()'s `move`, `travel`, `path` and `directness` for a built table. */
inline MorphMeasure measureMorph(const WaveTable& t)
{
    MorphMeasure m;
    m.frames = t.frames;
    std::vector<std::vector<double>> spec;
    for (int f = 0; f < t.frames; ++f) {
        std::vector<double> p = framePowerSpectrum(t, f);
        if (!p.empty()) spec.push_back(std::move(p));
    }
    if (spec.size() < 2) return m;
    std::vector<double> step;
    for (size_t f = 1; f < spec.size(); ++f) {
        double d = 0.0;
        for (size_t h = 0; h < spec[f].size(); ++h) d += std::fabs(spec[f][h] - spec[f - 1][h]);
        step.push_back(0.5 * d);
    }
    for (size_t h = 0; h < spec.back().size(); ++h) m.travel += std::fabs(spec.back()[h] - spec.front()[h]);
    m.travel *= 0.5;
    for (double s : step) m.path += s;
    m.moveMax = *std::max_element(step.begin(), step.end());
    std::vector<double> sorted = step;
    std::sort(sorted.begin(), sorted.end());
    m.move = sorted[sorted.size() / 2];
    m.directness = m.path > 1e-12 ? m.travel / m.path : 0.0;
    return m;
}

/** @brief Share of a signal's power in [lo, hi) Hz: one unwindowed FFT over the zero-padded signal. */
inline double bandShare(const std::vector<float>& x, double sr, double lo, double hi)
{
    size_t n = 1;
    while (n < x.size()) n *= 2;
    std::vector<std::complex<double>> a(n);
    for (size_t i = 0; i < x.size(); ++i) a[i] = x[i];
    fft(a);
    double in = 0.0, all = 0.0;
    for (size_t k = 1; k < n / 2; ++k) {
        const double f = static_cast<double>(k) * sr / static_cast<double>(n), p = std::norm(a[k]);
        all += p;
        if (f >= lo && f < hi) in += p;
    }
    return all > 0.0 ? in / all : 0.0;
}

/**
 * @brief Streaming power spectrum: Blackman-Harris windows of 65536 samples, back to back.
 *
 * A whole track does not fit in a buffer worth keeping, and it does not have to: band *ratios* are
 * what the calibration is about, and both the window's normalisation and the number of windows
 * cancel in a ratio. One FFT per 1.4 s of audio is all this costs.
 */
struct BandAccumulator {
    static constexpr size_t kN = 1u << 16;
    std::vector<float> buf = std::vector<float>(kN, 0.0f);
    std::vector<double> power = std::vector<double>(kN / 2 + 1, 0.0);
    size_t fill = 0;
    int windows = 0;

    void push(const float* x, int n)
    {
        while (n > 0) {
            const size_t take = std::min(static_cast<size_t>(n), kN - fill);
            std::copy(x, x + take, buf.begin() + static_cast<ptrdiff_t>(fill));
            fill += take;
            x += take;
            n -= static_cast<int>(take);
            if (fill == kN) {
                const std::vector<double> p = powerSpectrum(buf.data(), kN);
                for (size_t i = 0; i < p.size(); ++i) power[i] += p[i];
                fill = 0;
                ++windows;
            }
        }
    }

    double band(double lo, double hi) const
    {
        double s = 0.0;
        for (size_t k = 1; k < kN / 2; ++k) {
            const double f = static_cast<double>(k) * 48000.0 / static_cast<double>(kN);
            if (f >= lo && f < hi) s += power[k];
        }
        return s;
    }
};

/**
 * @brief Auto- and cross-spectra of a stereo signal, accumulated over whole windows.
 *
 * The same arrangement as BandAccumulator, with the one term a width measurement needs that a power
 * spectrum cannot give: Re{X_L conj(X_R)}. Everything else follows from the three sums, because for
 * two channels P_M = (P_L + P_R + 2 Re C) / 4 and P_S = (P_L + P_R - 2 Re C) / 4 exactly -- so a
 * width figure and the inter-channel correlation behind it come out of one pass and cannot
 * contradict each other (Blauert, "Spatial Hearing", MIT Press 1997, ch. 3, on interaural coherence).
 */
struct StereoBandAccumulator {
    static constexpr size_t kN = 1u << 16;
    std::vector<float> bufL = std::vector<float>(kN, 0.0f), bufR = std::vector<float>(kN, 0.0f);
    std::vector<double> pl = std::vector<double>(kN / 2 + 1, 0.0), pr = std::vector<double>(kN / 2 + 1, 0.0),
                        cc = std::vector<double>(kN / 2 + 1, 0.0);
    size_t fill = 0;
    int windows = 0;

    /** @brief Adds @p n samples of both channels; complete windows are transformed as they fill. */
    void push(const float* l, const float* r, int n)
    {
        while (n > 0) {
            const size_t take = std::min(static_cast<size_t>(n), kN - fill);
            std::copy(l, l + take, bufL.begin() + static_cast<ptrdiff_t>(fill));
            std::copy(r, r + take, bufR.begin() + static_cast<ptrdiff_t>(fill));
            fill += take;
            l += take;
            r += take;
            n -= static_cast<int>(take);
            if (fill == kN) {
                std::vector<std::complex<double>> a(kN), b(kN);
                for (size_t i = 0; i < kN; ++i) {
                    const double t = 2.0 * 3.141592653589793 * static_cast<double>(i) / static_cast<double>(kN);
                    const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
                    a[i] = static_cast<double>(bufL[i]) * w;
                    b[i] = static_cast<double>(bufR[i]) * w;
                }
                fft(a);
                fft(b);
                for (size_t i = 0; i <= kN / 2; ++i) {
                    pl[i] += std::norm(a[i]);
                    pr[i] += std::norm(b[i]);
                    cc[i] += (a[i] * std::conj(b[i])).real();
                }
                fill = 0;
                ++windows;
            }
        }
    }

    /** @brief The three sums inside one band. */
    void sums(double lo, double hi, double& a, double& b, double& c) const
    {
        a = b = c = 0.0;
        for (size_t k = 1; k < kN / 2; ++k) {
            const double f = static_cast<double>(k) * 48000.0 / static_cast<double>(kN);
            if (f >= lo && f < hi) { a += pl[k]; b += pr[k]; c += cc[k]; }
        }
    }
    /** @brief Side-over-mid power in a band, in dB. Mono reads far below zero, a pure side signal 0. */
    double width(double lo, double hi) const
    {
        double a, b, c;
        sums(lo, hi, a, b, c);
        return powDb(((a + b - 2.0 * c) / 4.0) / std::max((a + b + 2.0 * c) / 4.0, 1e-300));
    }
    /** @brief Zero-lag inter-channel correlation in a band. */
    double rho(double lo, double hi) const
    {
        double a, b, c;
        sums(lo, hi, a, b, c);
        return c / std::max(std::sqrt(a * b), 1e-300);
    }
    /** @brief Channel imbalance in a band, in dB: L over R. */
    double balance(double lo, double hi) const
    {
        double a, b, c;
        sums(lo, hi, a, b, c);
        return powDb(a / std::max(b, 1e-300));
    }
};

/**
 * @brief Largest value the band-limited reconstruction of @p x reaches between its samples.
 *
 * Not an interpolator with taps but the reconstruction itself: each block's spectrum is padded with
 * zeros to sixteen times the length and transformed back, which is sinc interpolation with every tap.
 * Only the middle half of each block is trusted, so the circular wrap at the block edges never
 * carries into the answer. The same computation stands in testDynamics and in
 * `Tools/metrics.py --selftest` (`true_peak_exact`), where it is held against four signals whose
 * peak between the samples is known in closed form.
 */
inline double exactPeak(const std::vector<float>& x, size_t from)
{
    constexpr size_t M = 1u << 13;
    double peak = 0.0;
    for (size_t start = from; start + M <= x.size(); start += M / 2) {
        std::vector<std::complex<double>> a(M), b(M * 16);
        for (size_t i = 0; i < M; ++i) a[i] = x[start + i];
        fft(a);
        for (size_t k = 0; k < M / 2; ++k) { b[k] = a[k]; b[M * 16 - 1 - k] = a[M - 1 - k]; }
        std::vector<std::complex<double>> c(M * 16);
        for (size_t k = 0; k < M * 16; ++k) c[k] = b[(M * 16 - k) % (M * 16)];
        fft(c);
        for (size_t i = M * 4; i < M * 12; ++i) peak = std::max(peak, std::fabs(c[i].real()) / M);
    }
    return peak;
}

/** @brief Power in a band, in dB, over the largest power of two that fits in @p n samples. */
inline double bandPowerDb(const std::vector<float>& x, size_t from, size_t n, double lo, double hi)
{
    if (from >= x.size()) return -200.0;
    n = std::min(n, x.size() - from);
    size_t m = 1;
    while (m * 2 <= n) m *= 2;
    if (m < 256) return -200.0;
    const std::vector<double> spec = powerSpectrum(x.data() + from, m);
    double p = 0.0;
    for (size_t k = 1; k < m / 2; ++k) {
        const double f = static_cast<double>(k) * 48000.0 / static_cast<double>(m);
        if (f >= lo && f < hi) p += spec[k];
    }
    return 10.0 * std::log10(p / static_cast<double>(m) + 1e-30);
}

/** @brief Root mean square of a stretch, in dB. */
inline double rmsDb(const std::vector<float>& x, size_t from, size_t n)
{
    if (from >= x.size()) return -200.0;
    n = std::min(n, x.size() - from);
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += static_cast<double>(x[from + i]) * x[from + i];
    return 10.0 * std::log10(s / static_cast<double>(std::max<size_t>(n, 1)) + 1e-30);
}

/** @brief Lerdahl instability of a MIDI note against a key root, as measure_tension.py counts it. */
inline int instabilityOf(int pitch, int key) { return lerdahlInstability(pitch - key); }

/// Seed slices per mode of testModalInterchange's presence measurement (12 tracks at most per mode).
constexpr int kPresenceSlices = 2;
/// Tracks one slice renders: the qualifying seeds with index [slice * kPresencePerSlice, + kPresencePerSlice).
constexpr int kPresencePerSlice = 6;
static_assert(kPresenceSlices * kPresencePerSlice == 12, "the slices must cover exactly the 12 tracks the measurement takes per mode");

/**
 * @brief testModalInterchange, part (4): what the borrowed mode costs the presence band across the
 *        break -- one mode, one slice of the seeds.
 *
 * Until 19.09.2026 one check covered both modes and all up to 24 rendered tracks (620-800 s, the
 * floor of the whole ctest run). The check was `count[0] >= 8 && count[1] >= 8 && worst[0] > -1.5
 * && worst[1] > -1.5`; it is now asserted per mode and per slice as `count >= 8 && worst over the
 * slice > -1.5`, and the four parts together assert exactly the old conjunction: a minimum over a
 * union is under -1.5 exactly when the minimum over one of its slices is.
 *
 * Which seeds count must not depend on the slicing. The loop takes seeds 1..40 in order, skips every
 * seed whose first track lacks the full break routine, and stops at twelve that have it; a slice
 * over a *seed range* would count other seeds. So every slice walks the same loop and plans every
 * seed (plans only, cheap: level match is off), numbers the qualifying ones, and renders only those
 * whose number falls in its own slice. `count` is the full number of qualifying seeds, the same in
 * every slice of a mode -- the number the old check held against 8.
 *
 * @param mode  0 = compose.modal_interchange On, 1 = Off
 * @param slice 0 .. kPresenceSlices - 1
 */
inline void modalPresence(int mode, int slice)
{
    section(fmt("modal interchange over the tonic pedal: presence band across the break, interchange %s, "
                "qualifying tracks %d..%d of 12", mode == 0 ? "on" : "off",
                slice * kPresencePerSlice + 1, (slice + 1) * kPresencePerSlice).c_str());

    // What the borrowed mode costs the presence band across the break. Solberg and Dibben's Track 2
    // rule (testSectionRules) asks that the drop after a breakdown be no duller between 1.5 and
    // 6 kHz than the core before it. With one mode for a whole track the two sections played the
    // same notes in the same register and the band matched to a tenth of a decibel; with a borrowed
    // mode they legitimately play different notes, and the band wanders. This measures by how much,
    // over every seed whose first track carries the full break routine, and it is where the
    // tolerance in testSectionRules comes from.
    {
        ParamStore p;
        p.parseText("compose.track_bars=128 compose.acid_amount=1 compose.lead_amount=1 compose.arp_amount=1 "
                    "compose.pad_amount=1 compose.sfx_amount=1 compose.level_match=Off master.auto_gain=Off");
        // Before the split the Off pass parsed "On" first and then "Off" into the same store; the
        // store's values are the same either way, since both lines set the one key.
        p.parseText(mode == 0 ? "compose.modal_interchange=On" : "compose.modal_interchange=Off");
        double worst = 1e9, sum = 0.0, changedSum = 0.0;
        int count = 0, rendered = 0, changed = 0;
        const int from = slice * kPresencePerSlice, to = from + kPresencePerSlice;
        // Parallelised 20.09.2026 (round "test-speed-rest"): which of the seeds 1..40 qualify (carry the
        // break routine) stays exactly this serial loop -- it is cheap (a plan only, no audio) and its
        // *order* is what the function comment's slicing contract depends on (`count` must be the same
        // number every slice would compute). Only the up to kPresencePerSlice full track renders this
        // slice owns are deferred into a second, parallel phase below: a candidate keeps the Composer
        // its qualifying plan came from (the render walks it further through a Conductor, so it must be
        // the same instance, not a new one), and phos::probe::runAll hands the candidates to its worker
        // pool exactly as Composer::measureTrack already does for probe renders -- see
        // testPhaseLockLock's comment for why this reuses runAll instead of a new pool. The fold after
        // (worst/sum/count/rendered/changed) runs on the calling thread once every render has returned,
        // walking the candidates in the same ascending-seed order the serial loop rendered them in, so
        // it is bit for bit the same sequence of additions and comparisons as before.
        struct Candidate {
            std::unique_ptr<Composer> comp;
            TrackPlan plan;
            int coreA = -1, drop = -1;
        };
        std::vector<Candidate> slice_;
        for (uint64_t s = 1; s <= 40 && count < 12; ++s) {
            auto comp = std::make_unique<Composer>(s);
            const TrackPlan plan = comp->track(p, 0);
            int coreA = -1, drop = -1;
            for (int i = 3; i < plan.form.count; ++i) {
                if (plan.form.section[i - 3].type != SectionType::Groove && plan.form.section[i - 3].type != SectionType::Drop) continue;
                if (plan.form.section[i - 2].type != SectionType::Break || plan.form.section[i - 1].type != SectionType::Build) continue;
                if (plan.form.section[i].type != SectionType::Drop) continue;
                coreA = i - 3;
                drop = i;
                break;
            }
            if (coreA < 0) continue;
            // The qualifying seed number `count`: another slice renders it.
            if (count < from || count >= to) { ++count; continue; }
            slice_.push_back({ std::move(comp), plan, coreA, drop });
            ++count;
        }
        struct Result {
            double d = 0.0;
            bool changed = false;
        };
        std::vector<Result> results(slice_.size());
        std::vector<std::function<void()>> tasks;
        for (size_t i = 0; i < slice_.size(); ++i) {
            tasks.push_back([&, i] {
                Candidate& c = slice_[i];
                constexpr double sr = 48000.0;
                auto e = std::make_unique<Engine>();
                e->prepare(sr, 512);
                e->params().copyValuesFrom(p);
                TempoMap tm = c.comp->tempoMap(e->params(), c.plan.bars);
                e->setTempoMap(tm);
                Conductor cond(*e, *c.comp);
                const double barSec = kBeatsPerBar * 60.0 / c.plan.bpm;
                const size_t total = static_cast<size_t>(tm.secondsAt(c.plan.bars * static_cast<double>(kBeatsPerBar)) * sr);
                std::vector<float> mono(total), L(512), R(512);
                size_t done = 0;
                while (done < total) {
                    cond.pump(e->params(), 32.0);
                    const int n = static_cast<int>(std::min<size_t>(512, total - done));
                    e->process(L.data(), R.data(), n);
                    for (int k = 0; k < n; ++k) mono[done + static_cast<size_t>(k)] = 0.5f * (L[static_cast<size_t>(k)] + R[static_cast<size_t>(k)]);
                    done += static_cast<size_t>(n);
                }
                const size_t lat = static_cast<size_t>(e->latencySamples());
                auto band = [&](int fromBar, int bars) {
                    return bandPowerDb(mono, static_cast<size_t>(fromBar * barSec * sr) + lat,
                                       static_cast<size_t>(bars * barSec * sr), 1500.0, 6000.0);
                };
                const Section& sc = c.plan.form.section[c.coreA];
                const Section& sd = c.plan.form.section[c.drop];
                const int w = std::min(16, sc.bars - 2);
                results[i].d = band(sd.startBar, std::min(16, sd.bars)) - band(sc.startBar + sc.bars - w, w);
                results[i].changed = mode == 0 && sc.scale != sd.scale;
            });
        }
        phos::probe::warmSharedData();
        phos::probe::runAll(tasks);
        for (const Result& r : results) {
            worst = std::min(worst, r.d);
            sum += r.d;
            ++rendered;
            if (r.changed) { changedSum += r.d; ++changed; }
        }
        // `count >= 8` is the old bound on the tracks one mode is measured over (the qualifying seeds,
        // not this slice's share of them); `worst` is this slice's minimum.
        check(count >= 8 && worst > -1.5,
              mode == 0 ? (slice == 0 ? "a borrowed mode costs the presence band across the break less than 1.5 dB (interchange on, tracks 1-6)"
                                      : "a borrowed mode costs the presence band across the break less than 1.5 dB (interchange on, tracks 7-12)")
                        : (slice == 0 ? "a borrowed mode costs the presence band across the break less than 1.5 dB (interchange off, tracks 1-6)"
                                      : "a borrowed mode costs the presence band across the break less than 1.5 dB (interchange off, tracks 7-12)"),
              fmt("interchange %s: %d qualifying tracks, this slice %d of them: mean %+.2f dB, worst %+.2f dB (%d changed mode "
                  "across the break, mean %+.2f dB)",
                  mode == 0 ? "on" : "off", count, rendered, sum / std::max(1, rendered), worst, changed,
                  changed > 0 ? changedSum / changed : 0.0));
    }
}

/**
 * @brief testModalInterchange, part (4)'s blind spot on the energy arc, closed on the set's *second*
 *        track (20.09.2026, round "presence-test"; PLAN_FOR_SONNET.md A5).
 *
 * Found by the test-split round's mutation check (19.09.2026, M4: `energyGainDb` inverted so a drop
 * stands under its groove instead of over it) and confirmed here: `modalPresence` above always renders
 * `Composer::track(p, 0)`, the set's *first* track, and `trackStartControls`'s `knobs` flag
 * (`plan.index == 0 && bar.index == 0`) plays it from the raw knobs for its own first section only, so
 * that alone does not explain the miss (the break routine this measurement looks for falls well past
 * the intro). Reproducing M4 (`Composer.cpp`, `energyGainDb`, `(energy - 0.7f)` negated) against
 * `modalPresence(0, 0)` (interchange on, tracks 1-6) measured worst +0.95 dB against a clean +2.25 dB --
 * degraded by exactly the mutation's own swing, but nowhere near the -1.5 dB floor: this specific
 * measurement (a spectral band, not a level) is dominated by the arc's *cutoff* component
 * (`trackStartControls`'s `cutoffAt`, untouched by `energyGainDb`), so a mutation that only inverts the
 * arc's loudness side moves it by a fraction of a dB regardless of which track is rendered -- track 1
 * is not structurally blind here, this one check's tolerance is just too wide for a loudness-only fault
 * to cross it. Measuring a second track closes the gap anyway, honouring the brief's instruction and
 * exercising material that (unlike track 1's) is never the level match's own reference (`gainDb`,
 * `partGainDb` fixed at the composer's raw values only for `t.index == 0`, `Composer::measureTrack`) --
 * though with `compose.level_match=Off` here that difference does not apply to this particular render.
 *
 * A second track needed two fixes a first track's own math hides for free, both because
 * `Section::startBar` is local to its own track (Form.h) while the Conductor always composes and
 * renders the set's timeline from bar 0: (1) a section's position on what is actually rendered is
 * `plan.firstBar + section.startBar` -- the arithmetic `testPresence` uses for a second track's drops --
 * not the local number alone, which for track 0 is a no-op (`firstBar` 0) and for any other track reads
 * the wrong, never-rendered bars of track 0 with no warning; (2) bar-to-sample has to go through the
 * real tempo map (`tm.secondsAt`), not a constant BPM from the track's own settled tempo
 * (`testVarietyPlans`, "tempo wanders": a later track can hand over at a different one). A first
 * implementation that skipped both produced numbers that never moved between the clean tree and the M4
 * mutation -- not "improved on track 1", silently wrong, because it measured track 0's own audio at
 * bar offsets that belonged to track 1's form.
 *
 * With both fixed (interchange on, tracks 1-6): clean, mean +1.89 dB, worst -2.03 dB; with M4, mean
 * +0.61 dB, worst -3.35 dB. The worst single seed is not the right gate here -- it is legitimately
 * noisy on a second track (clean -2.03 dB already sits under `modalPresence`'s own -1.5 dB floor: this
 * track's "core" reference window can still carry the tail of the previous track's hand-over, which
 * track 0 never has to). The mean is not noisy: it drops by 1.28 dB under M4, and that is the same
 * 1.28 dB `modalPresence(0, 0)` itself drops by (+3.38 to +2.10 dB) -- the arc's own swing, not sampling
 * error -- so this check gates on the mean instead, `> 1.0 dB`, measured with margin on both sides of
 * the clean/M4 gap. `docs/rounds/2026-09.md` (the block of the round that added this check) has the numbers; only interchange-on, tracks
 * 1-6 was measured before and after M4 -- the other three parts share the same implementation and bound
 * without their own before/after run.
 *
 * Not a replacement: `modalPresence` above is kept exactly as it was (same code, same numbers) -- it
 * may still be the one to catch some other, unrelated fault local to track 0 -- and this function adds
 * coverage beside it rather than subsuming it (different track, different bound, different rationale).
 *
 * @param mode  0 = compose.modal_interchange On, 1 = Off
 * @param slice 0 .. kPresenceSlices - 1
 */
inline void modalPresenceArc(int mode, int slice)
{
    section(fmt("modal interchange over the tonic pedal: presence band across the break on the set's "
                "second track, interchange %s, qualifying tracks %d..%d of 12", mode == 0 ? "on" : "off",
                slice * kPresencePerSlice + 1, (slice + 1) * kPresencePerSlice).c_str());

    {
        ParamStore p;
        p.parseText("compose.track_bars=128 compose.acid_amount=1 compose.lead_amount=1 compose.arp_amount=1 "
                    "compose.pad_amount=1 compose.sfx_amount=1 compose.level_match=Off master.auto_gain=Off");
        p.parseText(mode == 0 ? "compose.modal_interchange=On" : "compose.modal_interchange=Off");
        double worst = 1e9, sum = 0.0, changedSum = 0.0;
        int count = 0, rendered = 0, changed = 0;
        const int from = slice * kPresencePerSlice, to = from + kPresencePerSlice;
        // Parallelised 20.09.2026 (round "test-speed-rest"), the same way as modalPresence above (see
        // its comment): the cheap, order-defining qualification loop over seeds 1..40 stays serial, and
        // only the up to kPresencePerSlice full track renders this slice owns move to a second, parallel
        // phase over phos::probe::runAll. The fold afterwards walks the candidates in the same
        // ascending-seed order the serial loop rendered them in, so it is bit for bit the same
        // sequence of additions and comparisons as before.
        struct Candidate {
            std::unique_ptr<Composer> comp;
            TrackPlan plan;
            int coreA = -1, drop = -1;
        };
        std::vector<Candidate> slice_;
        for (uint64_t s = 1; s <= 40 && count < 12; ++s) {
            auto comp = std::make_unique<Composer>(s);
            // Track 1 (index 1): the set's second track, which unlike track 0 is never the level
            // match's own uncorrected reference.
            const TrackPlan plan = comp->track(p, 1);
            int coreA = -1, drop = -1;
            for (int i = 3; i < plan.form.count; ++i) {
                if (plan.form.section[i - 3].type != SectionType::Groove && plan.form.section[i - 3].type != SectionType::Drop) continue;
                if (plan.form.section[i - 2].type != SectionType::Break || plan.form.section[i - 1].type != SectionType::Build) continue;
                if (plan.form.section[i].type != SectionType::Drop) continue;
                coreA = i - 3;
                drop = i;
                break;
            }
            if (coreA < 0) continue;
            // The qualifying seed number `count`: another slice renders it.
            if (count < from || count >= to) { ++count; continue; }
            slice_.push_back({ std::move(comp), plan, coreA, drop });
            ++count;
        }
        struct Result {
            double d = 0.0;
            bool changed = false;
        };
        std::vector<Result> results(slice_.size());
        std::vector<std::function<void()>> tasks;
        for (size_t idx = 0; idx < slice_.size(); ++idx) {
            tasks.push_back([&, idx] {
                Candidate& c = slice_[idx];
                constexpr double sr = 48000.0;
                auto e = std::make_unique<Engine>();
                e->prepare(sr, 512);
                e->params().copyValuesFrom(p);
                const Section& sc = c.plan.form.section[c.coreA];
                const Section& sd = c.plan.form.section[c.drop];
                const int w = std::min(16, sc.bars - 2);
                const int dropLen = std::min(16, sd.bars);
                // Global bar numbers on the set's timeline (see the function comment): local
                // Section::startBar plus the track's own hand-over bar.
                const int coreEnd = c.plan.firstBar + sc.startBar + sc.bars;
                const int dropStart = c.plan.firstBar + sd.startBar;
                // The whole track, on the set's timeline (plan.firstBar is 0 only for track 0).
                const int renderBars = c.plan.firstBar + c.plan.bars;
                TempoMap tm = c.comp->tempoMap(e->params(), renderBars);
                e->setTempoMap(tm);
                Conductor cond(*e, *c.comp);
                const size_t total = static_cast<size_t>(tm.secondsAt(renderBars * static_cast<double>(kBeatsPerBar)) * sr);
                std::vector<float> mono(total), L(512), R(512);
                size_t done = 0;
                while (done < total) {
                    cond.pump(e->params(), 32.0);
                    const int n = static_cast<int>(std::min<size_t>(512, total - done));
                    e->process(L.data(), R.data(), n);
                    for (int k = 0; k < n; ++k) mono[done + static_cast<size_t>(k)] = 0.5f * (L[static_cast<size_t>(k)] + R[static_cast<size_t>(k)]);
                    done += static_cast<size_t>(n);
                }
                const size_t lat = static_cast<size_t>(e->latencySamples());
                // Bar-to-sample through the real tempo map, not a constant BPM (see the function comment).
                auto band = [&](int fromBar, int bars) {
                    const double t0 = tm.secondsAt(fromBar * static_cast<double>(kBeatsPerBar));
                    const double t1 = tm.secondsAt((fromBar + bars) * static_cast<double>(kBeatsPerBar));
                    return bandPowerDb(mono, static_cast<size_t>(t0 * sr) + lat, static_cast<size_t>((t1 - t0) * sr), 1500.0, 6000.0);
                };
                results[idx].d = band(dropStart, dropLen) - band(coreEnd - w, w);
                results[idx].changed = mode == 0 && sc.scale != sd.scale;
            });
        }
        phos::probe::warmSharedData();
        phos::probe::runAll(tasks);
        for (const Result& r : results) {
            worst = std::min(worst, r.d);
            sum += r.d;
            ++rendered;
            if (r.changed) { changedSum += r.d; ++changed; }
        }
        const double mean = sum / std::max(1, rendered);
        // Gated on the mean, not the worst case (unlike modalPresence above): measured on this track,
        // the worst single seed is legitimately noisy (clean: -2.03 dB, below modalPresence's own
        // -1.5 dB floor, on tracks 1-6 alone -- a second track's "core" reference window can still
        // carry the tail of the previous track's hand-over, which the first track never has to). The
        // mean is not: clean +1.89 dB against M4-mutated (energyGainDb inverted) +0.61 dB on the same
        // tracks, a drop of 1.28 dB that lines up with modalPresence's own (+3.38 to +2.10, 1.28 dB) --
        // the arc's own swing, not sampling noise. +1.0 dB sits between the two with margin either way;
        // docs/rounds/2026-09.md (the block of the round that added this check) has the measurement.
        check(count >= 8 && mean > 1.0,
              fmt("on the set's second track, a borrowed mode costs the presence band across the break less than 1.0 dB "
                  "on average (interchange %s, tracks %d-%d)", mode == 0 ? "on" : "off",
                  slice * kPresencePerSlice + 1, (slice + 1) * kPresencePerSlice).c_str(),
              fmt("interchange %s: %d qualifying tracks, this slice %d of them: mean %+.2f dB, worst %+.2f dB (%d changed mode "
                  "across the break, mean %+.2f dB)",
                  mode == 0 ? "on" : "off", count, rendered, sum / std::max(1, rendered), worst, changed,
                  changed > 0 ? changedSum / changed : 0.0));
    }
}

/**
 * @brief The score testModalInterchange part (5) and testModeColour block 3 both measure: seed 31337,
 *        Goa, interchange on, 12 tracks of 128 bars, composed with the Markov (0) or the neural (1) model.
 *
 * The two sections composed it byte for byte the same, each for itself. In one process (the full
 * serial run, ctest's `selftest`) it is now composed once and read twice; as separate ctest tests
 * each process composes it once, as before. The composer is kept with the notes because both readers
 * ask it for track plans (`track`, `trackOfBar`) after the fact.
 */
struct BorrowedScore {
    ParamStore p;                    ///< the knobs the score was composed with
    std::unique_ptr<Composer> c;     ///< the composer that wrote it, asked for its plans afterwards
    std::vector<NoteEvent> ev;       ///< the 1536 bars' notes
};

/** @brief BorrowedScore for @p which (0 Markov, 1 neural), composed on first use. */
inline BorrowedScore& borrowedScore(int which)
{
    static std::unique_ptr<BorrowedScore> cache[2];
    std::unique_ptr<BorrowedScore>& s = cache[which];
    if (!s) {
        s = std::make_unique<BorrowedScore>();
        s->p.parseText("compose.track_bars=128 compose.lead_amount=1 compose.acid_amount=1 compose.arp_amount=1 "
                       "compose.style=Goa compose.modal_interchange=On compose.level_match=Off master.auto_gain=Off");
        s->p.parseText(which == 0 ? "compose.melody_model=Markov" : "compose.melody_model=Neural");
        s->c = std::make_unique<Composer>(31337);
        s->c->composeBars(s->p, 0, 12 * 128, s->ev);
    }
    return *s;
}

/**
 * @brief Drains a cue ring into a vector, so a test can look at what a block produced.
 */
inline std::vector<Cue> drainCues(CueRing& ring)
{
    std::vector<Cue> out;
    Cue c;
    while (ring.pop(c)) out.push_back(c);
    return out;
}

/** @brief The lane that plays @p role in the knobs' kit, or -1 (test side: read from the knobs, not from Rhythm.cpp). */
inline int testLaneOfRole(const ParamStore& p, PercRole role)
{
    for (int l = 0; l < kPercLanes; ++l) {
        const int b = p.base(Module::Perc, l);
        if (p.getBool(b + perc::Active) && p.getInt(b + perc::Role) == static_cast<int>(role)) return l;
    }
    return -1;
}

/**
 * @brief The value of an Offset parameter at @p beat, replayed from control events (test side).
 *
 * Every event ramps from wherever the parameter stands at its own beat to its value over its length, by a
 * raised cosine (Score.h, ControlEvent::Kind::Offset); a later event replaces a ramp in flight. Replayed in
 * order, independently of the engine's dispatcher.
 */
inline double offsetAt(const std::vector<ControlEvent>& controls, int param, double beat)
{
    double value = 0.0, from = 0.0, target = 0.0, start = 0.0, length = 0.0;
    auto at = [&](double t) {
        if (length <= 0.0 || t >= start + length) return target;
        const double u = (t - start) / length;
        return from + (target - from) * 0.5 * (1.0 - std::cos(kPiD * u));
    };
    for (const ControlEvent& c : controls) {
        if (c.param != param || c.kind != ControlEvent::Kind::Offset) continue;
        if (c.beat > beat) break;
        value = at(c.beat);
        from = value;
        target = c.value;
        start = c.beat;
        length = c.length;
    }
    return at(beat);
}

/**
 * @brief The melodic rules of 18.09.2026 (docs/rounds/2026-09.md, "Melodik nach Regeln"): acid, arp, lead, pad.
 *
 * Measured two ways. Over a population of plans (every mode, random keys and seeds) each rule is
 * checked on the material itself, where a violation can be named exactly; and on the user's
 * listening seed 864566672 (three tracks, default settings) the score is measured with the same
 * statistics the brief's table used, so the before and after of the report come from here.
 */
/**
 * @brief The listening seed's score as testGenreRules part `.listeningSeed` and testFoundation part
 *        `.score` both read it: 864566672, default knobs, bars 0..767.
 *
 * Default knobs mean the level match is on, so composing it renders the level probes of every track --
 * the expensive half of both sections. In one process (the full serial run) it is composed once for
 * both; as separate ctest tests each composes it once, as before. The composer is kept with the notes
 * because both readers ask it for track plans after the fact.
 */
struct ListeningScore {
    ParamStore q;                    ///< default knobs
    std::unique_ptr<Composer> c;     ///< the composer that wrote the score
    std::vector<NoteEvent> ev;       ///< 768 bars of notes
};

/** @brief The ListeningScore, composed on first use. */
inline ListeningScore& listeningScore()
{
    static std::unique_ptr<ListeningScore> s;
    if (!s) {
        s = std::make_unique<ListeningScore>();
        s->c = std::make_unique<Composer>(864566672ull);
        s->c->composeBars(s->q, 0, 768, s->ev);
    }
    return *s;
}

/**
 * @brief Voices, parts `.counterSound*` (h2): the counter-lead never plays the lead's sound, over the 24
 *        tracks of one of the three seeds.
 * @param seed 864566672 (the listening seed), 77 or 2026
 */
inline void voicesCounterSound(uint64_t seed)
{
    section(fmt("voices: the counter-lead's sound against the lead's, seed %llu", static_cast<unsigned long long>(seed)).c_str());
    // (h2) The counter-lead against the lead of the same track (19.09.2026, round "arrangement"; the user:
    //      "Die Counter-Lead sollte natuerlich einen anderen Sound haben als die Haupt-Lead"). Over 24 tracks
    //      of the listening seed and of two others: the oscillator and table each voice really plays (the
    //      recipe's, or the instance's knob where the recipe says -1), and both rendered on the same note --
    //      the power centroid and the share above 2 kHz. No track may give both the same oscillator and
    //      table, and none may share either.
    {
        // Split by seed on 19.09.2026 (round "test-split"): 687 s in one process, almost all of it the 72
        // track plans with their level probes. The check is a count of offending tracks held at zero, so
        // "zero over three seeds" is exactly "zero in each"; the listening seed's plans are the ones part
        // `.sound` plans too, and in one process they are planned once (listeningPlanner()).
        std::unique_ptr<ListeningPlanner> own;
        if (seed != 864566672ull) {
            own = std::make_unique<ListeningPlanner>();
            own->c = std::make_unique<Composer>(seed);
        }
        ListeningPlanner& lp = own ? *own : listeningPlanner();
        ParamStore& p = lp.p;
        const float sv = p.get(p.base(Module::Compose) + compose::SoundVariation);
        int tracks = 0, samePair = 0, shareOne = 0;
        std::vector<double> dCents, dBright;
        const int lb = p.base(PolyInstance::Lead), cbI = p.base(PolyInstance::Counter);
        {
            Composer& lc = *lp.c;
            for (int ti = 0; ti < 24; ++ti) {
                const TrackPlan& plan = lc.track(p, ti);
                ++tracks;
                double cents[2] = {}, bright[2] = {};
                int osc[2] = {}, table[2] = {};
                for (int k = 0; k < 2; ++k) {
                    const PolyInstance inst = k == 0 ? PolyInstance::Lead : PolyInstance::Counter;
                    const int v = polyIndex(inst);
                    const VoiceRecipe& rc = plan.voice[v];
                    ParamStore s;
                    const int b = s.base(inst);
                    float off[poly::Count] = {};
                    Composer::voiceRecipeOffsets(inst, rc, sv, off);
                    if (rc.osc >= 0) s.set(b + poly::Osc, static_cast<float>(rc.osc));
                    if (rc.table >= 0) s.set(b + poly::Table, static_cast<float>(rc.table));
                    if (rc.filter >= 0) s.set(b + poly::FilterType, static_cast<float>(rc.filter));
                    for (int q = 0; q < poly::Count; ++q)
                        if (off[q] != 0.0f) s.set(b + q, s.fromNormalised(b + q, s.toNormalised(b + q, s.get(b + q)) + off[q]));
                    s.set(b + poly::DelaySend, 0.0f);
                    s.set(b + poly::Drift, 0.0f);
                    osc[k] = s.getInt(b + poly::Osc);
                    table[k] = s.getInt(b + poly::Table);
                    auto e = std::make_unique<Poly>();
                    e->prepare(48000.0);
                    std::vector<float> vals = moduleValues(s, Module::Poly, v);
                    e->update(vals.data(), 145.0);
                    e->noteOn(69, 1.0f, 4.0, 1 << 24, 0.0);
                    const size_t n = 16384;
                    const std::vector<float> y = renderMono([&](float* L, float* R, int m) { e->process(L, R, m); }, n + 4800);
                    cents[k] = 1200.0 * std::log2(std::max(1.0, centroid(y.data() + 4800, 48000.0, n)));
                    const std::vector<double> pw = powerSpectrum(y.data() + 4800, n);
                    double hi = 0.0, all = 0.0;
                    for (size_t q = 1; q < pw.size(); ++q) { all += pw[q]; if (q * 48000.0 / n > 2000.0) hi += pw[q]; }
                    bright[k] = powDb(hi / std::max(all, 1e-30));
                }
                (void)lb; (void)cbI;
                // A table only colours the wavetable oscillator; the others read none.
                const bool sameTable = osc[0] == static_cast<int>(PolyOsc::Wavetable) && osc[1] == osc[0] && table[0] == table[1];
                if (osc[0] == osc[1] && (sameTable || osc[0] != static_cast<int>(PolyOsc::Wavetable))) ++samePair;
                if (osc[0] == osc[1] || (table[0] == table[1] && osc[0] == static_cast<int>(PolyOsc::Wavetable) && osc[1] == osc[0])) ++shareOne;
                dCents.push_back(std::fabs(cents[1] - cents[0]));
                dBright.push_back(std::fabs(bright[1] - bright[0]));
            }
        }
        std::sort(dCents.begin(), dCents.end());
        std::sort(dBright.begin(), dBright.end());
        const std::string what = fmt("the counter-lead never plays the lead's sound: another oscillator (and so another table) in every track (seed %llu)",
                                     static_cast<unsigned long long>(seed));
        check(samePair == 0 && shareOne == 0, what.c_str(),
              fmt("%d tracks, %d with the same oscillator and table, %d sharing the oscillator; on one note the centroid differs by "
                  "%.0f ct (median, lowest %.0f), the share above 2 kHz by %.1f dB (median, lowest %.1f)",
                  tracks, samePair, shareOne, dCents[dCents.size() / 2], dCents[0], dBright[dBright.size() / 2], dBright[0]));
    }
}

} // namespace phostest
