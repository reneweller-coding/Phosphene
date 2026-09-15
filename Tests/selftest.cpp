/**
 * @file selftest.cpp
 * @brief phos_selftest: measured checks of every building block and of the engine as a whole.
 *
 * Each check measures a property against a value derived independently of the code under test --
 * the analytic response of a filter, the closed-form tempo integral, the pattern definition --
 * rather than against a recording of what the code once produced.
 */
#include "phos/Acid.h"
#include "phos/Composer.h"
#include "phos/Corpus.h"
#include "phos/DiodeLadder.h"
#include "phos/Dynamics.h"
#include "phos/Dsp.h"
#include "phos/Engine.h"
#include "phos/Halfband.h"
#include "phos/Harmony.h"
#include "phos/Ladder.h"
#include "phos/Loudness.h"
#include "phos/Midi.h"
#include "phos/Oscillator.h"
#include "phos/Patterns.h"
#include "phos/Melody.h"
#include "phos/Perc.h"
#include "phos/Poly.h"
#include "phos/Reverb.h"
#include "phos/Rhythm.h"
#include "phos/Sfx.h"
#include "phos/TranceGate.h"
#include "phos/WaveTable.h"
#include "phos/WavWriter.h"
#include "TestSupport.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <tuple>

using namespace phos;
using namespace phostest;

namespace {

constexpr double kPiD = 3.141592653589793;

/**
 * @brief Amplitude of the component with exactly @p cycles periods in @p n samples.
 *
 * With an integer number of cycles the rectangular projection is exact for a steady sine.
 */
double toneAmplitude(const float* x, size_t n, double cycles)
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
std::vector<float> renderEngine(Engine& engine, const Composer& composer, double beats, int block, double sr, std::vector<float>* right = nullptr)
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
int firstCoreBar(const ParamStore& p, const Composer& c)
{
    const TrackPlan& t = c.track(p, 0);
    for (int i = 0; i < t.form.count; ++i)
        if (t.form.section[i].type == SectionType::Groove || t.form.section[i].type == SectionType::Drop) return t.form.section[i].startBar;
    return 0;
}

// ---------------------------------------------------------------------------------------------

void testParams()
{
    section("parameters");
    ParamStore p;
    const int expected = static_cast<int>(compose::Count) + static_cast<int>(kick::Count) + static_cast<int>(bass::Count)
                       + static_cast<int>(mix::Count) + static_cast<int>(master::Count) + kPercLanes * static_cast<int>(perc::Count)
                       + static_cast<int>(acid::Count) + kPolyInstances * static_cast<int>(poly::Count)
                       + static_cast<int>(sfx::Count) + static_cast<int>(fx::Count);
    check(p.count() == expected, "every module table registered", fmt("%d parameters", p.count()));
    check(p.find("lead.detune") == p.base(Module::Poly, 0) + poly::Detune && p.find("arp.detune") == p.base(Module::Poly, 1) + poly::Detune
          && p.find("acid.cutoff") == p.base(Module::Acid) + acid::Cutoff && p.get(p.find("arp.amp_sustain")) == 0.0f,
          "named instances (lead, arp) with their own defaults");
    const int id = p.find("kick.pitch_start");
    check(id == p.base(Module::Kick) + kick::PitchStart, "key lookup gives base + index");
    check(p.find("kick.nonsense") < 0, "unknown key is not found");

    std::string err;
    check(p.parseText("compose.key=A; kick.pitch_start=500\nbass.cutoff=220 # comment", &err), "assignments parse", err);
    check(p.getInt(p.base(Module::Compose) + compose::Key) == 9, "choice set by name");
    check(p.parseText("compose.scale=Double Harmonic kick.engine=resonant mix.kick_mute=on", &err)
          && p.getInt(p.base(Module::Compose) + compose::Scale) == 4 && p.getInt(p.base(Module::Kick) + kick::Engine) == 1
          && p.getBool(p.base(Module::Mix) + mix::KickMute), "choice names with spaces, any case, and On/Off", err);
    check(!p.parseText("bass.cutof=1", &err) && err.find("unknown") != std::string::npos, "misspelt key reported", err);

    int worst = 0;
    for (int i = 0; i < p.count(); ++i) {
        const ParamDesc& d = p.desc(i);
        for (float n = 0.0f; n <= 1.0f; n += 0.125f) {
            const float v = p.fromNormalised(i, n);
            const float back = p.toNormalised(i, v);
            if (d.curve == Curve::Linear || d.curve == Curve::Log) {
                if (std::fabs(back - n) > 1e-4f) ++worst;
            }
        }
    }
    check(worst == 0, "normalised mapping inverts for continuous parameters", fmt("%d mismatches", worst));

    for (int i = 0; i < p.count(); ++i) p.setNormalised(i, 0.37f);
    const std::string text = p.toText(false);
    ParamStore q;
    q.parseText(text);
    int diff = 0;
    for (int i = 0; i < p.count(); ++i) if (p.get(i) != q.get(i)) ++diff;
    check(diff == 0, "text form round-trips every value exactly", fmt("%d differ", diff));
}

void testTempo()
{
    section("tempo map");
    TempoMap t;
    t.setConstant(145.0);
    check(std::fabs(t.secondsAt(4.0) - 4.0 * 60.0 / 145.0) < 1e-12, "constant tempo: seconds of four beats");

    t.add(0.0, 140.0, true);
    t.add(128.0, 148.0, false);
    // Numerical integral of 60/bpm over the ramp, as an independent reference.
    double num = 0.0;
    const int steps = 1280000;
    for (int i = 0; i < steps; ++i) {
        const double b = (i + 0.5) * 128.0 / steps;
        num += 60.0 / (140.0 + 8.0 * b / 128.0) * (128.0 / steps);
    }
    check(std::fabs(t.secondsAt(128.0) - num) < 1e-6, "ramp: closed form equals numerical integral", fmt("%.9f vs %.9f", t.secondsAt(128.0), num));
    check(std::fabs(t.bpmAt(64.0) - 144.0) < 1e-9, "ramp: tempo halfway");
    double worst = 0.0;
    for (double b = 0.0; b < 300.0; b += 0.37) worst = std::max(worst, std::fabs(t.beatAt(t.secondsAt(b)) - b));
    check(worst < 1e-9, "beatAt inverts secondsAt", fmt("worst %.3g beats", worst));
    check(std::fabs((t.secondsAt(200.0) - t.secondsAt(199.0)) - 60.0 / 148.0) < 1e-9, "held after the last point");
}

void testHalfband()
{
    section("half-band filters");
    const HalfbandDesign d = designHalfband(96.0, 0.1);
    check(d.count >= 4 && d.count <= kHalfbandMaxCoefs, "design yields a sensible order", fmt("%d coefficients", d.count));
    bool monotone = true;
    for (int i = 0; i < d.count; ++i) monotone = monotone && d.coef[i] > 0.0f && d.coef[i] < 1.0f;
    check(monotone, "all coefficients in (0, 1)");

    // Decimator: a high-rate sine in, base-rate amplitude out. N high-rate samples, cycles per N.
    const size_t N = 1u << 18, M = N / 2, settle = 4096;
    auto decimate = [&](double cycles) {
        HalfbandDown<float> dn;
        dn.setup(d);
        std::vector<float> out(M);
        for (size_t i = 0; i < M + settle; ++i) {
            const size_t j = i * 2;
            const float a = static_cast<float>(std::sin(2.0 * kPiD * cycles * static_cast<double>(j) / N));
            const float b = static_cast<float>(std::sin(2.0 * kPiD * cycles * static_cast<double>(j + 1) / N));
            const float y = dn.process(a, b);
            if (i >= settle) out[i - settle] = y;
        }
        // The base-rate signal has cycles' = cycles folded into [0, M/2].
        double c = std::fmod(cycles, static_cast<double>(M));
        if (c > M / 2.0) c = M - c;
        return toneAmplitude(out.data(), M, c);
    };
    const double pass = decimate(0.18 * N);   // 17.3 kHz at 96 kHz
    const double pass2 = decimate(0.05 * N);  // 4.8 kHz
    const double stop = decimate(0.31 * N);   // 29.8 kHz: would alias to 18.2 kHz
    const double stop2 = decimate(0.45 * N);  // 43.2 kHz: would alias to 4.8 kHz
    check(std::fabs(20.0 * std::log10(pass)) < 0.05 && std::fabs(20.0 * std::log10(pass2)) < 0.05, "decimator passband flat to 17 kHz",
          fmt("%.4f dB at 17.3 kHz, %.4f dB at 4.8 kHz", 20.0 * std::log10(pass), 20.0 * std::log10(pass2)));
    check(20.0 * std::log10(stop) < -90.0 && 20.0 * std::log10(stop2) < -90.0, "decimator rejects what would alias",
          fmt("%.1f dB at 29.8 kHz, %.1f dB at 43.2 kHz", 20.0 * std::log10(stop), 20.0 * std::log10(stop2)));

    // Interpolator: a base-rate sine in; the image at (fs - f) must be gone.
    HalfbandUp<float> up;
    up.setup(d);
    std::vector<float> hi(N);
    const double cyc = 0.1 * M;   // 4.8 kHz at 48 kHz, in cycles per M base samples
    for (size_t i = 0; i < M + settle; ++i) {
        const float x = static_cast<float>(std::sin(2.0 * kPiD * cyc * static_cast<double>(i) / M));
        float o0, o1;
        up.process(x, o0, o1);
        if (i >= settle) { hi[(i - settle) * 2] = o0; hi[(i - settle) * 2 + 1] = o1; }
    }
    const double fund = toneAmplitude(hi.data(), N, cyc);
    const double image = toneAmplitude(hi.data(), N, static_cast<double>(M) - cyc);
    check(std::fabs(20.0 * std::log10(fund)) < 0.05 && 20.0 * std::log10(image / fund) < -90.0, "interpolator removes the image",
          fmt("fundamental %.4f dB, image %.1f dB", 20.0 * std::log10(fund), 20.0 * std::log10(image / fund)));
}

/**
 * @brief Power outside the harmonic bins relative to the harmonics, below @p maxBin, in dB.
 *
 * Measured in the audible band only: a decimator's transition band lets partials between 24 and
 * 29 kHz fold into 19..24 kHz, which is aliasing nobody hears and would otherwise dominate the figure.
 */
double aliasDb(const std::vector<float>& x, size_t n, size_t harmonicBin, size_t maxBin)
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

void testLadder()
{
    section("ladder filter");
    const double sr = 48000.0;
    const float g = static_cast<float>(std::tan(kPiD * 1000.0 / sr));
    auto response = [&](double hz, float k) {
        LadderT<float> f;
        f.reset();
        const size_t n = 96000, settle = 48000;
        std::vector<float> y(n);
        const double cycles = std::round(hz * n / sr);
        for (size_t i = 0; i < n + settle; ++i) {
            const float x = 1.0e-4f * static_cast<float>(std::sin(2.0 * kPiD * cycles * static_cast<double>(i) / n));
            const float o = f.tick(x, g, k, 0.0f);
            if (i >= settle) y[i - settle] = o;
        }
        return 20.0 * std::log10(toneAmplitude(y.data(), n, cycles) / 1.0e-4);
    };
    // Trapezoidal one-pole: |H| = 1 / sqrt(1 + (tan(pi f / fs) / g)^2), four in a row.
    double worst = 0.0;
    std::string detail;
    for (double hz : { 100.0, 500.0, 1000.0, 3000.0, 8000.0 }) {
        const double r = std::tan(kPiD * hz / sr) / g;
        const double expect = 4.0 * 20.0 * std::log10(1.0 / std::sqrt(1.0 + r * r));
        const double got = response(hz, 0.0f);
        worst = std::max(worst, std::fabs(got - expect));
        detail += fmt("%.0f Hz %.2f/%.2f  ", hz, got, expect);
    }
    check(worst < 0.05, "small-signal response matches the analytic four-pole", detail);

    // Self-oscillation: above k = 4 a kick of input starts a sine near the cutoff that stays.
    LadderT<float> f;
    f.reset();
    std::vector<float> y(96000);
    for (size_t i = 0; i < y.size(); ++i) y[i] = f.tick(i < 10 ? 0.1f : 0.0f, g, 5.0f, 0.0f);
    int crossings = 0;
    float amp = 0.0f;
    for (size_t i = 48001; i < y.size(); ++i) {
        if (y[i - 1] < 0.0f && y[i] >= 0.0f) ++crossings;
        amp = std::max(amp, std::fabs(y[i]));
    }
    check(std::abs(crossings - 1000) < 30 && amp > 0.1f && amp < 5.0f, "self-oscillates at the cutoff, bounded",
          fmt("%d Hz, amplitude %.2f", crossings, static_cast<double>(amp)));
}

void testMidi()
{
    section("MIDI export");
    ParamStore p;
    p.parseText("compose.level_match=Off master.auto_gain=Off");
    Composer c(5);
    Score s;
    s.tempo.add(0.0, 140.0, true);
    s.tempo.add(32.0, 146.0, false);
    s.keyRoot = 6;
    c.composeBars(p, 0, 12, s.notes);
    s.sections.push_back(SectionMark{ 16.0, SectionType::Drop, 1.0f, 0 });
    s.sort();
    const std::vector<uint8_t> bytes = encodeMidi(s);
    MidiFileData d;
    std::string err;
    check(decodeMidi(bytes.data(), bytes.size(), d, &err), "decodes what it encodes", err);
    size_t kickN = 0, bassN = 0;
    bool match = true;
    for (const MidiTrackData& t : d.tracks) {
        Part part = t.name == "Kick" ? Part::Kick : (t.name == "Bass" ? Part::Bass : Part::Count);
        if (part == Part::Count) continue;
        size_t j = 0;
        for (const NoteEvent& n : s.notes) {
            if (n.part != part) continue;
            if (j >= t.notes.size()) { match = false; break; }
            const NoteEvent& m = t.notes[j++];
            match = match && std::fabs(m.beat - n.beat) < 0.5 / kMidiPpq && std::fabs(m.length - n.length) < 1.0 / kMidiPpq
                          && m.pitch == n.pitch && m.velocity == n.velocity;
        }
        (part == Part::Kick ? kickN : bassN) = t.notes.size();
        match = match && j == t.notes.size();
        if (part == Part::Kick) match = match && t.channel == 9;
    }
    check(match && kickN > 0 && bassN > 0, "every note back with its beat, length, pitch and velocity", fmt("%zu kick, %zu bass", kickN, bassN));
    check(d.keySharps == 3 && d.keyMinor, "key signature F# minor (three sharps)");
    {
        size_t percNotes = 0;
        bool gm = true;
        for (const NoteEvent& n : s.notes) if (n.part == Part::Perc) ++percNotes;
        const MidiTrackData* pt = nullptr;
        for (const MidiTrackData& t : d.tracks) if (t.name == "Perc") pt = &t;
        if (pt != nullptr) {
            for (const NoteEvent& n : pt->notes) {
                bool known = false;
                for (int note : kPercRoleNote) known = known || n.pitch == note;
                const bool tomRun = n.pitch <= kPercRoleNote[static_cast<int>(PercRole::Tom)] && n.pitch >= kPercRoleNote[static_cast<int>(PercRole::Tom)] - 12;
                gm = gm && (known || tomRun);
            }
        }
        check(pt != nullptr && pt->channel == 9 && pt->notes.size() == percNotes && percNotes > 0 && gm,
              "percussion track on the drum channel with General MIDI notes", fmt("%zu notes", percNotes));
    }
    check(d.markers.size() == 1 && d.markers[0].first == 16.0 && d.markers[0].second.rfind("Drop", 0) == 0, "section marker");

    // The stepped tempo events reproduce the ramp's timing at every beat.
    double secs = 0.0, worst = 0.0;
    for (int beat = 0; beat < 40; ++beat) {
        double bpm = d.tempos.front().bpm;
        for (const TempoPoint& t : d.tempos) if (t.beat <= beat + 1e-9) bpm = t.bpm;
        secs += 60.0 / bpm;
        worst = std::max(worst, std::fabs(secs - s.tempo.secondsAt(beat + 1.0)));
    }
    check(worst < 1e-4, "tempo steps keep the MIDI file in time with the ramp", fmt("worst %.2f microseconds", worst * 1e6));
}

void testWav()
{
    section("WAV writer");
    const char* path = "phos_selftest_tmp.wav";
    std::vector<float> L(1000), R(1000);
    for (int i = 0; i < 1000; ++i) { L[static_cast<size_t>(i)] = std::sin(0.01f * i) * 0.9f; R[static_cast<size_t>(i)] = -L[static_cast<size_t>(i)]; }
    for (WavFormat f : { WavFormat::Float32, WavFormat::Pcm24 }) {
        WavWriter w;
        check(w.open(path, 48000, 2, f) && w.write(L.data(), R.data(), 600) && w.write(L.data() + 600, R.data() + 600, 400) && w.close(), "writes in pieces");
        FILE* fp = std::fopen(path, "rb");
        std::vector<uint8_t> b;
        uint8_t buf[4096];
        size_t n;
        while (fp && (n = std::fread(buf, 1, sizeof(buf), fp)) > 0) b.insert(b.end(), buf, buf + n);
        if (fp) std::fclose(fp);
        const int bps = f == WavFormat::Float32 ? 4 : 3;
        uint32_t dataSize = 0;
        std::memcpy(&dataSize, b.data() + 76, 4);
        bool ok = b.size() == 80 + 1000u * 2 * bps && std::memcmp(b.data(), "RIFF", 4) == 0 && dataSize == 1000u * 2 * bps;
        double worst = 0.0;
        for (int i = 0; ok && i < 1000; ++i) {
            for (int ch = 0; ch < 2; ++ch) {
                const uint8_t* q = b.data() + 80 + (i * 2 + ch) * bps;
                double v;
                if (bps == 4) { float fv; std::memcpy(&fv, q, 4); v = fv; }
                else { int32_t s = (q[0] << 8) | (q[1] << 16) | (q[2] << 24); v = (s >> 8) / 8388608.0; }
                worst = std::max(worst, std::fabs(v - (ch == 0 ? L[static_cast<size_t>(i)] : R[static_cast<size_t>(i)])));
            }
        }
        check(ok && worst <= (bps == 4 ? 0.0 : 1.0 / 8388608.0), bps == 4 ? "float file reads back exactly" : "24-bit file reads back within one step", fmt("worst %.3g", worst));
    }
    std::remove(path);
}

void testLoudness()
{
    section("loudness meter");
    const double sr = 48000.0;
    std::vector<float> x(static_cast<size_t>(sr * 10));
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.1f * static_cast<float>(std::sin(2.0 * kPiD * 997.0 * static_cast<double>(i) / sr));
    LoudnessMeter m;
    m.prepare(sr);
    m.process(x.data(), x.data(), static_cast<int>(x.size()));
    const LoudnessReading r = m.read();
    // BS.1770: a 997 Hz sine at -20 dBFS in both channels reads -20.0 LUFS.
    check(std::fabs(r.integrated + 20.0f) < 0.1f, "-20 dBFS 997 Hz sine in stereo reads -20 LUFS", fmt("%.2f LUFS", static_cast<double>(r.integrated)));
    check(std::fabs(r.truePeak + 20.0f) < 0.1f, "its true peak reads -20 dBTP", fmt("%.2f dBTP", static_cast<double>(r.truePeak)));
}

// ---------------------------------------------------------------------------------------------
// Sound generators, composer, engine
// ---------------------------------------------------------------------------------------------

/** @brief Current values of one module as an array indexed like its table. */
std::vector<float> moduleValues(const ParamStore& p, Module m, int instance = 0)
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

/** @brief Wraps degrees into (-180, 180]. */
double wrapDeg(double d) { d = std::fmod(d + 180.0, 360.0); if (d < 0.0) d += 360.0; return d - 180.0; }

void testOscillator()
{
    section("PolyBLEP oscillator");
    const size_t n = 1u << 16, bin = 2731;
    const double sr = 48000.0, hz = sr * static_cast<double>(bin) / static_cast<double>(n);
    std::vector<float> naive(n), blep(n), over(n);
    VaOscillator osc;
    osc.set(hz, sr, 0.0f, 0.5f);
    for (size_t i = 0; i < n; ++i) {
        double ph = static_cast<double>(i) * hz / sr;
        ph -= std::floor(ph);
        naive[i] = static_cast<float>(2.0 * ph - 1.0);
        blep[i] = osc.next();
    }
    VaOscillator osc2;
    osc2.set(hz, 2.0 * sr, 0.0f, 0.5f);
    HalfbandDown<float> dn;
    dn.setup(designHalfband(96.0, 0.1));
    for (size_t i = 0; i < 8192; ++i) { const float a = osc2.next(); dn.process(a, osc2.next()); }
    for (size_t i = 0; i < n; ++i) { const float a = osc2.next(); over[i] = dn.process(a, osc2.next()); }
    const size_t band = static_cast<size_t>(18000.0 * n / sr);
    const double dNaive = aliasDb(naive, n, bin, band), dBlep = aliasDb(blep, n, bin, band), dOver = aliasDb(over, n, bin, band);
    check(dBlep < dNaive - 12.0, "PolyBLEP saw aliases far less than the naive saw (below 18 kHz)", fmt("naive %.1f dB, PolyBLEP %.1f dB", dNaive, dBlep));
    check(dOver < dBlep - 15.0, "2x oversampled and decimated is cleaner again", fmt("%.1f dB", dOver));

    // The blend from saw to pulse keeps the fundamental. With the pulse's old polarity the two
    // fundamentals were opposite and cancelled at Wave = 1/3.
    const size_t m = 48000;
    const double f = 100.0;
    double minFund = 1e9, maxFund = 0.0, oldAtThird = 0.0;
    for (int w = 0; w <= 20; ++w) {
        VaOscillator o;
        o.set(f, sr, static_cast<float>(w) / 20.0f, 0.5f);
        std::vector<float> y(m);
        for (size_t i = 0; i < m; ++i) y[i] = o.next();
        const double a = toneAmplitude(y.data(), m, f);
        minFund = std::min(minFund, a);
        maxFund = std::max(maxFund, a);
    }
    {
        // The old spelling, computed directly: saw + w (pulse - saw) with pulse = +1 for t < 1/2.
        std::vector<float> y(m);
        for (size_t i = 0; i < m; ++i) {
            double t = static_cast<double>(i) * f / sr;
            t -= std::floor(t);
            const double saw = 2.0 * t - 1.0, pulse = t < 0.5 ? 1.0 : -1.0;
            y[i] = static_cast<float>(saw + (1.0 / 3.0) * (pulse - saw));
        }
        oldAtThird = toneAmplitude(y.data(), m, f);
    }
    check(minFund > 0.6 && oldAtThird < 0.02, "saw-to-pulse blend never loses the fundamental (the old polarity did)",
          fmt("fundamental %.3f .. %.3f; old blend at 1/3: %.4f", minFund, maxFund, oldAtThird));

    // Phase convention: restart(phi) puts the fundamental at sine phase phi, for saw and pulse alike,
    // including a sub-sample offset.
    double worst = 0.0;
    for (float wave : { 0.0f, 1.0f }) {
        for (double phi : { 0.0, 0.3, 0.75 }) {
            for (double late : { 0.0, 0.6 }) {
                VaOscillator o;
                o.set(f, sr, wave, 0.5f);
                o.restart(phi, late);
                std::vector<float> y(4800);
                for (float& v : y) v = o.next();
                const double d = phaseAgainst(y.data(), y.size(), [&](size_t i) { return phi + f * (static_cast<double>(i) + late) / sr; });
                worst = std::max(worst, std::fabs(wrapDeg(d)));
            }
        }
    }
    check(worst < 1.0, "restart sets the fundamental's sine phase, sub-sample exact", fmt("worst %.3f degrees", worst));
}

void testKick()
{
    section("kick");
    check(std::fabs(Kick::tuneToKey(6, 50.0f) - 46.249f) < 0.01f, "F#: end pitch moves to F#1", fmt("%.2f Hz", static_cast<double>(Kick::tuneToKey(6, 50.0f))));
    check(std::fabs(Kick::tuneToKey(9, 50.0f) - 55.0f) < 0.01f, "A: end pitch moves to A1");
    check(std::fabs(Kick::tuneToKey(2, 50.0f) - 55.0f) < 0.01f, "D: end pitch moves to the fifth, A1");

    const double sr = 48000.0;
    ParamStore p;
    p.parseText("kick.tune=Free kick.pitch_end=50 kick.pitch_start=300 kick.pitch_decay=20 kick.punch=0 "
                "kick.drive=0 kick.click_level=0 kick.tone=20000 kick.amp_hold=400 kick.level=0");
    std::vector<float> kv = moduleValues(p, Module::Kick);
    Kick k;
    k.prepare(sr);
    k.update(kv.data(), 6);
    k.trigger(1.0f);
    std::vector<float> y(24000);
    k.process(y.data(), static_cast<int>(y.size()));
    double worst = 0.0;
    std::string detail;
    int last = -1;
    for (int i = 1; i < static_cast<int>(y.size()); ++i) {
        if (!(y[static_cast<size_t>(i) - 1] < 0.0f && y[static_cast<size_t>(i)] >= 0.0f)) continue;
        if (last >= 0) {
            const double tMid = 0.5 * (last + i) / sr;
            const double expect = 50.0 + 250.0 * std::exp(-tMid / 0.020);
            const double got = sr / (i - last);
            if (tMid > 0.02 && tMid < 0.4) {
                const double err = std::fabs(got / expect - 1.0);
                if (err > worst) { worst = err; detail = fmt("worst at %.0f ms: %.1f Hz vs %.1f Hz", tMid * 1000.0, got, expect); }
            }
        }
        last = i;
    }
    check(worst < 0.03, "pitch follows the exponential sweep", detail);
    float peak = 0.0f;
    for (float v : y) peak = std::max(peak, std::fabs(v));
    check(peak > 0.9f && peak < 1.05f, "body peaks at the set level", fmt("peak %.3f", static_cast<double>(peak)));

    // The closed-form output phase: what outputPhaseAt predicts is what comes out, including the
    // chain's phase and a sub-sample start, at the defaults (punch, two segments, drive, tone).
    {
        ParamStore q;
        std::vector<float> qv = moduleValues(q, Module::Kick);
        double worstPh = 0.0;
        for (double late : { 0.0, 0.37, 0.9 }) {
            Kick kk;
            kk.prepare(sr);
            kk.update(qv.data(), 6);
            kk.trigger(1.0f, late);
            std::vector<float> s(9600);
            kk.process(s.data(), static_cast<int>(s.size()));
            for (size_t w0 : { static_cast<size_t>(2400), static_cast<size_t>(4800) }) {
                const double d = phaseAgainst(s.data() + w0, 2200, [&](size_t i) { return kk.outputPhaseAt((static_cast<double>(w0 + i) + late) / sr); });
                worstPh = std::max(worstPh, std::fabs(wrapDeg(d)));
            }
        }
        check(worstPh < 2.0, "rendered kick phase equals the closed-form prediction (with sub-sample starts)", fmt("worst %.2f degrees", worstPh));
    }

    // Separate time constants: the punch segment is set on its own, not as a fixed fraction of the body.
    {
        ParamStore q;
        q.parseText("kick.tune=Free kick.pitch_end=50 kick.pitch_start=800 kick.punch=1 kick.punch_decay=3 kick.pitch_decay=60 kick.drive=0 kick.click_level=0");
        std::vector<float> qv = moduleValues(q, Module::Kick);
        Kick kk;
        kk.prepare(sr);
        kk.update(qv.data(), 6);
        const double f10 = kk.frequencyAt(0.010);
        const double expect = 50.0 + 750.0 * std::exp(-10.0 / 3.0);
        check(std::fabs(f10 - expect) < 0.5, "punch decay of 3 ms under a 60 ms body", fmt("f(10 ms) = %.1f Hz, expected %.1f", f10, expect));
    }

    // Retrigger while loud.
    k.reset();
    k.update(kv.data(), 6);
    k.trigger(1.0f);
    std::vector<float> fresh(64);
    k.process(fresh.data(), 64);
    float freshStep = std::fabs(fresh[0]);
    for (size_t i = 1; i < fresh.size(); ++i) freshStep = std::max(freshStep, std::fabs(fresh[i] - fresh[i - 1]));
    k.reset();
    k.update(kv.data(), 6);
    k.trigger(1.0f);
    std::vector<float> z(9600);
    k.process(z.data(), 4800);
    k.trigger(1.0f);
    k.process(z.data() + 4800, 4800);
    float maxStep = 0.0f;
    for (size_t i = 4790; i < 4864; ++i) maxStep = std::max(maxStep, std::fabs(z[i] - z[i - 1]));
    check(maxStep < freshStep + 0.02f, "retrigger fades the old kick out without a click",
          fmt("step %.4f, from silence %.4f", static_cast<double>(maxStep), static_cast<double>(freshStep)));

    // Tail limit: a long kick is shortened so that at the first sixteenth (103 ms at 145 BPM) its
    // output, saturation included, is 24 dB under its peak -- for tanh and hard clip, soft and hot.
    auto tailAt = [&](const char* settings, bool constrain, double slot) {
        ParamStore q;
        q.parseText(settings);
        std::vector<float> qv = moduleValues(q, Module::Kick);
        if (constrain) Kick::constrainTail(qv.data(), slot);
        Kick kk;
        kk.prepare(sr);
        kk.update(qv.data(), 6);
        kk.trigger(1.0f);
        std::vector<float> s(19200);
        kk.process(s.data(), static_cast<int>(s.size()));
        float pk = 0.0f, tail = 0.0f;
        const size_t a = static_cast<size_t>(slot * sr), b = a + 480;
        for (size_t i = 0; i < s.size(); ++i) {
            pk = std::max(pk, std::fabs(s[i]));
            if (i >= a && i < b) tail = std::max(tail, std::fabs(s[i]));
        }
        return 20.0 * std::log10(tail / pk);
    };
    const double slot = 0.25 * 60.0 / 145.0;
    double worstTail = -1e9, loosest = 1e9;
    std::string tails;
    for (const char* s : { "kick.amp_hold=60 kick.amp_decay=800 kick.drive=0.2",
                           "kick.amp_hold=60 kick.amp_decay=800 kick.drive=1",
                           "kick.amp_hold=60 kick.amp_decay=800 kick.drive=1 kick.clip=Hard",
                           "kick.engine=Resonant kick.amp_decay=900 kick.drive=0.6" }) {
        const double with = tailAt(s, true, slot), without = tailAt(s, false, slot);
        worstTail = std::max(worstTail, with);
        loosest = std::min(loosest, without);
        tails += fmt("%.1f/%.1f  ", with, without);
    }
    check(worstTail < -23.0 && loosest > -24.0, "tail limit holds after saturation (unconstrained these kicks were not)", tails + "dB with/without");

    // Body floor: the shortest recipes still give two periods of the end pitch above -20 dB.
    {
        const double endHz = Kick::tuneToKey(6, 50.0f);
        ParamStore q;
        q.parseText("kick.engine=Resonant kick.amp_decay=40");
        std::vector<float> res = moduleValues(q, Module::Kick);
        Kick::constrain(res.data(), 0.5 * 60.0 / 145.0, 6);
        q.parseText("kick.engine=Sweep kick.amp_hold=5 kick.amp_decay=40");
        std::vector<float> swp = moduleValues(q, Module::Kick);
        Kick::constrain(swp.data(), 0.5 * 60.0 / 145.0, 6);
        const double resBody = res[kick::AmpDecay] * 0.001 / 3.0;
        const double swpBody = (swp[kick::AmpHold] + swp[kick::AmpDecay] / 3.0) * 0.001;
        check(resBody >= 2.0 / endHz - 1e-6 && swpBody >= 2.0 / endHz - 1e-6, "body floor: two periods above -20 dB (knobs asked for 13 and 18 ms)",
              fmt("resonator %.1f ms, sweep %.1f ms, two periods %.1f ms", resBody * 1000.0, swpBody * 1000.0, 2000.0 / endHz));
    }

    // Resonant engine: level under a sweep, unit ring, 60 dB decay.
    p.parseText("kick.engine=Resonant kick.amp_decay=1500 kick.pitch_start=330 kick.pitch_end=50 kick.pitch_decay=20");
    kv = moduleValues(p, Module::Kick);
    k.reset();
    k.update(kv.data(), 6);
    k.trigger(1.0f);
    std::vector<float> sw(9600);
    k.process(sw.data(), static_cast<int>(sw.size()));
    float swEarly = 0.0f, swLate = 0.0f;
    for (size_t i = 0; i < 480; ++i) swEarly = std::max(swEarly, std::fabs(sw[i]));
    for (size_t i = 4800; i < 9600; ++i) swLate = std::max(swLate, std::fabs(sw[i]));
    check(20.0 * std::log10(swLate / swEarly) > -6.0, "swept resonator keeps its level while the pitch falls",
          fmt("%.1f dB from the first 10 ms to 100..200 ms", 20.0 * std::log10(swLate / swEarly)));
    p.parseText("kick.engine=Resonant kick.amp_decay=300 kick.pitch_start=50 kick.pitch_end=50");
    kv = moduleValues(p, Module::Kick);
    k.reset();
    k.update(kv.data(), 6);
    k.trigger(1.0f);
    std::vector<float> r(48000);
    k.process(r.data(), static_cast<int>(r.size()));
    float early = 0.0f, late = 0.0f;
    for (size_t i = 0; i < 2400; ++i) early = std::max(early, std::fabs(r[i]));
    for (size_t i = 14400 - 1200; i < 14400 + 1200; ++i) late = std::max(late, std::fabs(r[i]));
    const double drop = 20.0 * std::log10(late / early);
    check(early > 0.6f && early < 1.4f, "resonator rings near unit amplitude", fmt("peak %.3f", static_cast<double>(early)));
    check(drop < -50.0 && drop > -70.0, "resonator decays about 60 dB over the decay time", fmt("%.1f dB at 300 ms", drop));
}

/** @brief Renders one bass note from silence and returns the samples. */
std::vector<float> bassNote(const char* settings, int pitch, int gate, size_t length, double late, double phase)
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

void testBass()
{
    section("bass");
    const double sr = 48000.0;
    const int pitch = 30;   // F#1, the default root
    const double f0 = midiToHz(pitch);

    // The sub sounds at the fundamental, not an octave below it.
    {
        const std::vector<float> y = bassNote("bass.wave=0 bass.env_amount=0 bass.cutoff=20 bass.amp_sustain=1 bass.amp_decay=5 bass.sub=1", pitch, 96000, 48000, 0.0, 0.0);
        const double atF0 = toneAmplitude(y.data() + 4800, 38400, f0 * 38400 / sr);
        const double atHalf = toneAmplitude(y.data() + 4800, 38400, 0.5 * f0 * 38400 / sr);
        check(atF0 > 20.0 * atHalf && atF0 > 0.3, "sub at the fundamental (46.25 Hz), nothing at 23 Hz", fmt("%.3f at f0, %.4f at f0/2", atF0, atHalf));
    }

    // The fundamental starts at the requested phase, sub-sample exact, and keeps it while the filter
    // envelope closes (Split). In Mixed the filtered voice's own fundamental drags it along.
    auto drift = [&](const char* mode, double& startErr) {
        double lo = 1e9, hi = -1e9;
        startErr = 0.0;
        for (double phase : { 0.0, 0.3 }) {
            const double late = 0.45;
            const std::vector<float> y = bassNote(mode, pitch, 96000, 12000, late, phase);
            const size_t per = static_cast<size_t>(std::lround(sr / f0));
            for (size_t ms : { static_cast<size_t>(0), static_cast<size_t>(11), static_cast<size_t>(22), static_cast<size_t>(44),
                               static_cast<size_t>(88), static_cast<size_t>(140) }) {
                const size_t w0 = ms * 48;
                const double d = wrapDeg(phaseAgainst(y.data() + w0, 2 * per + 8, [&](size_t i) { return phase + f0 * (static_cast<double>(w0 + i) + late) / sr; }));
                if (ms == 0) startErr = std::max(startErr, std::fabs(d));
                lo = std::min(lo, d);
                hi = std::max(hi, d);
            }
        }
        return hi - lo;
    };
    double startSplit = 0.0, startMixed = 0.0;
    const double driftSplit = drift("bass.sub_mode=Split bass.amp_sustain=1 bass.amp_decay=5", startSplit);
    const double driftMixed = drift("bass.sub_mode=Mixed bass.amp_sustain=1 bass.amp_decay=5", startMixed);
    check(startSplit < 5.0, "fundamental starts at the requested phase (sub-sample onset)", fmt("worst %.2f degrees", startSplit));
    check(driftSplit < 6.0 && driftMixed > 2.0 * driftSplit, "fundamental phase holds while the filter closes (Split); Mixed drifts",
          fmt("Split %.1f degrees, Mixed %.1f degrees over 140 ms", driftSplit, driftMixed));

    // From silence at the knob phase (0.5: saw and sub both at zero) the note starts without a step;
    // phase 0.25 puts the sub at its peak, as the old sub phase did.
    auto onset = [&](double fundamentalPhase) {
        const std::vector<float> y = bassNote("", pitch, 4000, 2400, 0.0, fundamentalPhase);
        float early = 0.0f, pk = 0.0f;
        for (size_t i = 0; i < y.size(); ++i) { pk = std::max(pk, std::fabs(y[i])); if (i < 24) early = std::max(early, std::fabs(y[i])); }
        return early / pk;
    };
    const double atZero = onset(0.0), atPeak = onset(0.25);
    check(atZero < 0.5 * atPeak, "note starts at the zero crossing of saw and sub", fmt("first 0.5 ms reaches %.2f of the peak (%.2f with the sub at its peak)", atZero, atPeak));

    // Release floor: never shorter than half a period, even with Release at 1 ms.
    {
        ParamStore q;
        q.parseText("bass.amp_release=1 bass.duck_depth=0");
        std::vector<float> v = moduleValues(q, Module::Bass);
        Bass b;
        b.prepare(sr);
        b.update(v.data());
        b.noteOn(pitch, 1.0f, 2400, 0.0, 0.0);
        std::vector<float> y(9600);
        b.process(y.data(), static_cast<int>(y.size()));
        // Envelope-level decay after the gate: find when the RMS over a half period falls 43 dB below
        // its value just before the gate.
        const size_t half = static_cast<size_t>(sr / f0 / 2);
        auto rms = [&](size_t a) { double s = 0.0; for (size_t i = a; i < a + half; ++i) s += static_cast<double>(y[i]) * y[i]; return std::sqrt(s / half); };
        const double before = rms(2400 - half);
        size_t t43 = 2400;
        while (t43 + half < y.size() && 20.0 * std::log10(rms(t43) / before + 1e-12) > -43.0) ++t43;
        const double ms = (t43 + half / 2 - 2400) * 1000.0 / sr;
        check(b.effectiveRelease() >= static_cast<float>(0.5 / f0) && ms > 0.4 * 1000.0 / f0,
              "release floor of half a period (knob at 1 ms)", fmt("release used %.1f ms, measured %.1f ms to -43 dB; half period %.1f ms",
                                                                    b.effectiveRelease() * 1000.0, ms, 500.0 / f0));
    }

    // Retriggered notes are the same note.
    {
        ParamStore q;
        q.parseText("bass.duck_depth=0");
        std::vector<float> v = moduleValues(q, Module::Bass);
        Bass b;
        b.prepare(sr);
        b.update(v.data());
        const int step = 4966;
        std::vector<float> two(static_cast<size_t>(step) * 2);
        b.noteOn(pitch, 1.0f, 3500, 0.0, 0.0);
        b.process(two.data(), step);
        b.noteOn(pitch, 1.0f, 3500, 0.0, 0.0);
        b.process(two.data() + step, step);
        double num = 0.0, e1 = 0.0, e2 = 0.0;
        for (int i = 0; i < 3000; ++i) {
            num += static_cast<double>(two[static_cast<size_t>(i)]) * two[static_cast<size_t>(i + step)];
            e1 += static_cast<double>(two[static_cast<size_t>(i)]) * two[static_cast<size_t>(i)];
            e2 += static_cast<double>(two[static_cast<size_t>(i + step)]) * two[static_cast<size_t>(i + step)];
        }
        check(num / std::sqrt(e1 * e2) > 0.999, "two retriggered sixteenths are the same waveform", fmt("correlation %.6f", num / std::sqrt(e1 * e2)));
    }

    Ducker d;
    d.prepare(sr);
    d.set(0.8f, 1.0f, 20.0f, 60.0f);
    d.trigger();
    float minGain = 1.0f, maxStep = 0.0f, prev = 1.0f, endGain = 0.0f;
    for (int i = 0; i < 9600; ++i) {
        const float v = d.next();
        minGain = std::min(minGain, v);
        maxStep = std::max(maxStep, std::fabs(v - prev));
        prev = v;
        endGain = v;
    }
    check(std::fabs(minGain - 0.2f) < 1e-5f && endGain == 1.0f, "ducker reaches its depth and returns to unity");
    check(maxStep <= 0.8f / 48.0f + 1e-5f, "ducker moves continuously", fmt("largest step %.4f", static_cast<double>(maxStep)));
}

void testComposer()
{
    section("composer");
    ParamStore p;
    p.parseText("compose.bass_variation=0 compose.level_match=Off master.auto_gain=Off");
    Composer c(7);
    // Sixteen bars of the first core: since Phase 5 bar 0 is the intro, where the kick has not
    // started yet (Form.h). The core's first sixteen bars are where "four on the floor" must hold.
    const TrackPlan& t0 = c.track(p, 0);
    int core0 = 0;
    for (int i = 0; i < t0.form.count; ++i)
        if (t0.form.section[i].type == SectionType::Groove || t0.form.section[i].type == SectionType::Drop) { core0 = t0.form.section[i].startBar; break; }
    std::vector<NoteEvent> ev;
    c.composeBars(p, core0, 16, ev);
    int kicks = 0, bassOnBeat = 0, bass = 0;
    for (const NoteEvent& e : ev) {
        if (e.part == Part::Kick) ++kicks;
        if (e.part == Part::Bass) {
            ++bass;
            if (std::fabs(e.beat - std::round(e.beat)) < 1e-9) ++bassOnBeat;
        }
    }
    check(kicks == 16 * 4 - 2, "four on the floor in a core, with the last beat of every eighth bar left out", fmt("%d kicks in 16 bars from bar %d", kicks, core0));
    check(bass == 16 * 4 * 3 && bassOnBeat == 0, "rolling bass: three notes per beat, never on a kick", fmt("%d notes, %d on the beat", bass, bassOnBeat));
    check(std::is_sorted(ev.begin(), ev.end(), noteLess), "events sorted");

    // Every note in its track's scale, over several tracks with everything varying.
    {
        ParamStore q;
        q.parseText("compose.bass_variation=1 compose.track_variation=1 compose.track_bars=32 compose.level_match=Off master.auto_gain=Off");
        Composer cc(9);
        std::vector<NoteEvent> e;
        cc.composeBars(q, 0, 320, e);
        int outside = 0, n = 0;
        for (const NoteEvent& x : e) {
            if (x.part != Part::Bass) continue;
            const TrackPlan& t = cc.track(q, cc.trackOfBar(q, static_cast<int>(x.beat / kBeatsPerBar)));
            ++n;
            if (!inScale(t.scale, x.pitch - bassRootNote(t.key, 0))) ++outside;
        }
        check(outside == 0 && n > 0, "every bass note is in its track's key and scale", fmt("%d of %d outside", outside, n));
    }

    // Bars and their control events are the same composed alone or in sequence.
    {
        ParamStore q;
        q.parseText("compose.track_bars=32 compose.level_match=Off master.auto_gain=Off");
        Composer cc(13);
        std::vector<NoteEvent> seqN, oneN;
        std::vector<ControlEvent> seqC, oneC;
        cc.composeBars(q, 0, 100, seqN, &seqC);
        Composer fresh(13);
        fresh.composeBars(q, 67, 1, oneN, &oneC);
        std::vector<NoteEvent> sliceN;
        std::vector<ControlEvent> sliceC;
        for (const NoteEvent& x : seqN) if (x.beat >= 268.0 && x.beat < 272.0) sliceN.push_back(x);
        for (const ControlEvent& x : seqC) if (x.beat >= 268.0 && x.beat < 272.0) sliceC.push_back(x);
        bool same = sliceN.size() == oneN.size() && sliceC.size() == oneC.size();
        for (size_t i = 0; same && i < sliceN.size(); ++i) same = sliceN[i].beat == oneN[i].beat && sliceN[i].pitch == oneN[i].pitch && sliceN[i].length == oneN[i].length;
        for (size_t i = 0; same && i < sliceC.size(); ++i) same = sliceC[i].param == oneC[i].param && sliceC[i].value == oneC[i].value && sliceC[i].kind == oneC[i].kind;
        check(same && !oneC.empty(), "a bar composed alone equals the same bar in sequence, notes and sound changes");
    }

    const int expectPerBeat[] = { 3, 2, 2, 1, 2 };
    bool patternsOk = true;
    std::string detail;
    for (int pat = 0; pat < 5; ++pat) {
        ParamStore q;
        q.parseText("compose.level_match=Off master.auto_gain=Off");
        q.set(q.base(Module::Compose) + compose::BassPattern, static_cast<float>(pat));
        Composer cp(3);
        // A bar of the first core: the intro has no bass yet.
        const TrackPlan& tp = cp.track(q, 0);
        int at = 0;
        for (int i = 0; i < tp.form.count; ++i)
            if (tp.form.section[i].type == SectionType::Groove || tp.form.section[i].type == SectionType::Drop) { at = tp.form.section[i].startBar; break; }
        std::vector<NoteEvent> e;
        cp.composeBars(q, at, 1, e);
        const int count = static_cast<int>(std::count_if(e.begin(), e.end(), [](const NoteEvent& x) { return x.part == Part::Bass; }));
        detail += fmt("%s %d  ", kBassPatternNames[pat], count);
        patternsOk = patternsOk && count == 4 * expectPerBeat[pat];
    }
    check(patternsOk, "every bass pattern has its notes per bar", detail);
}

void testVariety()
{
    section("variety over a night");
    // The probes of the level match are not what this looks at (they have their own check below).
    ParamStore p;
    p.parseText("compose.level_match=Off master.auto_gain=Off");
    const int cb = p.base(Module::Compose);
    Composer c(2026);
    const int tracks = 60;
    std::vector<int> keyUse(12, 0), patUse(kNumBassPatterns, 0);
    int engineSwitches = 0, gateViolations = 0, sameAsPrevious = 0, badLength = 0;
    double minKick = 1e9, minBass = 1e9, sumKick = 0.0, bpmLo = 1e9, bpmHi = -1e9;
    const float release = p.get(p.base(Module::Bass) + bass::AmpRelease);
    for (int i = 0; i < tracks; ++i) {
        const TrackPlan t = c.track(p, i);
        ++keyUse[static_cast<size_t>(t.key)];
        ++patUse[static_cast<size_t>(t.primaryPattern)];
        if (t.kickEngine >= 0) ++engineSwitches;
        bpmLo = std::min(bpmLo, t.bpm);
        bpmHi = std::max(bpmHi, t.bpm);
        if (t.bars < 32 || t.bars % 16 != 0) ++badLength;
        // Gate: the lowest note's release (floored at half a period) must end before the next slot.
        const int lowest = bassRootNote(t.key, 0) + scaleDegree(t.scale, 6) - 12;
        for (int pat : { t.primaryPattern, t.secondaryPattern }) {
            const double slot = shortestBassSlot(pat) * 60.0 / t.bpm;
            const double rel = std::max(release * 0.001, 0.5 / midiToHz(lowest));
            if (i > 0 && t.gate * slot + rel > slot + 1e-6) ++gateViolations;
        }
        if (i > 0) {
            const TrackPlan prev = c.track(p, i - 1);
            double dk = 0.0, db = 0.0;
            for (int m = 0; m < kNumKickMacros; ++m) dk += (t.kickMacro[m] - prev.kickMacro[m]) * (t.kickMacro[m] - prev.kickMacro[m]);
            for (int m = 0; m < kNumBassMacros; ++m) db += (t.bassMacro[m] - prev.bassMacro[m]) * (t.bassMacro[m] - prev.bassMacro[m]);
            if (i > 1) { minKick = std::min(minKick, std::sqrt(dk)); minBass = std::min(minBass, std::sqrt(db)); sumKick += std::sqrt(dk); }
            if (t.key == prev.key && t.primaryPattern == prev.primaryPattern && t.bpm == prev.bpm) ++sameAsPrevious;
        }
    }
    const int keys = static_cast<int>(std::count_if(keyUse.begin(), keyUse.end(), [](int n) { return n > 0; }));
    const int pats = static_cast<int>(std::count_if(patUse.begin(), patUse.end(), [](int n) { return n > 0; }));
    const double base = p.get(cb + compose::Bpm), range = p.get(cb + compose::TempoRange);
    check(keys >= 6 && pats >= 4, "keys and bass patterns spread over the night", fmt("%d keys, %d primary patterns in %d tracks", keys, pats, tracks));
    check(bpmLo >= base - range && bpmHi <= base + range && bpmHi - bpmLo >= range, "tempo wanders inside the range", fmt("%.1f .. %.1f BPM", bpmLo, bpmHi));
    check(minKick > 0.35 && minBass > 0.35, "no two consecutive tracks share a sound (best-candidate spread)",
          fmt("closest consecutive recipes: kick %.2f, bass %.2f; mean kick distance %.2f", minKick, minBass, sumKick / (tracks - 2)));
    check(gateViolations == 0 && badLength == 0, "every generated gate lets the release finish; lengths in 16-bar blocks", fmt("%d gate violations", gateViolations));
    check(sameAsPrevious < tracks / 4, "most tracks differ from the last in key, pattern or tempo", fmt("%d of %d unchanged", sameAsPrevious, tracks - 1));

    // Variation at zero: every track is the knobs.
    {
        ParamStore q;
        q.parseText("compose.track_variation=0 compose.sound_variation=0 compose.level_match=Off master.auto_gain=Off");
        Composer cz(2026);
        bool allKnobs = true;
        for (int i = 0; i < 20; ++i) {
            const TrackPlan t = cz.track(q, i);
            float off[64] = {};
            Composer::recipeOffsets(true, t.kickMacro, 0.0f, off);
            float any = 0.0f;
            for (float o : off) any += std::fabs(o);
            allKnobs = allKnobs && t.key == q.getInt(cb + compose::Key) && t.primaryPattern == q.getInt(cb + compose::BassPattern)
                    && t.bpm == q.get(cb + compose::Bpm) && t.bars == q.getInt(cb + compose::TrackBars) && t.kickEngine < 0 && any == 0.0f;
        }
        check(allKnobs, "with Track and Sound Variation at 0 every track plays the knobs");
    }

    // Level match: tracks with very different sounds reach the same loudness. Measured on the real
    // render, bars 4..28 of each 32-bar track, with every variation at full strength.
    {
        auto trackLoudness = [&](bool match, double& spread) {
            auto e = std::make_unique<Engine>();
            e->prepare(48000.0, 512);
            e->params().parseText(fmt("compose.track_bars=128 compose.sound_variation=1 compose.track_variation=1 master.auto_gain=Off master.limiter=Off master.clipper=Off master.clip=Off master.comp_ratio=1 mix.acid_mute=1 mix.lead_mute=1 mix.arp_mute=1 mix.pad_mute=1 mix.sfx_mute=1 compose.level_match=%s", match ? "On" : "Off").c_str());
            Composer ce(31);
            const int tracksN = 4, trackBars = 128;
            const TempoMap tm = ce.tempoMap(e->params(), tracksN * trackBars);
            e->setTempoMap(tm);
            Conductor cond(*e, ce);
            std::vector<float> L(512), R(512);
            double lo = 1e9, hi = -1e9;
            for (int t = 0; t < tracksN; ++t) {
                // Measure inside the track's first core: the intro and the breakdown are part of the
                // form, not of the sound the level match is about.
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
                const double lufs = m.read().integrated;
                lo = std::min(lo, lufs);
                hi = std::max(hi, lufs);
                // Skip to the next track's measuring window.
                const uint64_t next = static_cast<uint64_t>(tm.secondsAt(static_cast<double>(tp.firstBar + tp.bars) * 4.0) * 48000.0);
                while (e->samplePosition() < next) {
                    cond.pump(e->params(), 32.0);
                    e->process(L.data(), R.data(), static_cast<int>(std::min<uint64_t>(512, next - e->samplePosition())));
                }
            }
            spread = hi - lo;
        };
        double withMatch = 0.0, without = 0.0;
        trackLoudness(true, withMatch);
        trackLoudness(false, without);
        check(withMatch < 1.5 && without > withMatch + 1.0, "level match keeps the tracks within 1.5 LU (without it they spread wider)",
              fmt("spread %.2f LU with, %.2f LU without", withMatch, without));
    }

    // The engine plays the recipes: in track 1 the knobs, in track 2 something else, within the constraints.
    {
        auto e = std::make_unique<Engine>();
        e->prepare(48000.0, 256);
        e->params().parseText("compose.track_bars=32 compose.sound_variation=1 compose.level_match=Off master.auto_gain=Off");
        Composer ce(77);
        const TempoMap tm = ce.tempoMap(e->params(), 64);
        e->setTempoMap(tm);
        Conductor cond(*e, ce);
        std::vector<float> L(4096), R(4096);
        const int kb = e->params().base(Module::Kick), bbase = e->params().base(Module::Bass);
        bool firstIsKnobs = false, secondDiffers = false;
        const uint64_t endSamples = static_cast<uint64_t>(tm.secondsAt(64.0 * kBeatsPerBar) * 48000.0);
        while (e->samplePosition() < endSamples) {
            cond.pump(e->params(), 32.0);
            e->process(L.data(), R.data(), 4096);
            const double bar = e->beatPosition() / kBeatsPerBar;
            if (bar > 8.0 && bar < 9.0) firstIsKnobs = e->effective(kb + kick::PitchDecay) == e->params().get(kb + kick::PitchDecay)
                                                    && e->effective(bbase + bass::Resonance) == e->params().get(bbase + bass::Resonance);
            if (bar > 40.0 && bar < 41.0) {
                secondDiffers = e->effective(kb + kick::PitchDecay) != e->params().get(kb + kick::PitchDecay)
                             || e->effective(bbase + bass::Resonance) != e->params().get(bbase + bass::Resonance);
            }
        }
        check(firstIsKnobs && secondDiffers, "engine plays the knobs in track 1 and the recipe in track 2");
    }
}

void testEngine()
{
    section("engine");
    const double sr = 48000.0;
    Composer comp(11);

    std::vector<std::vector<float>> renders;
    for (int block : { 1, 64, 1000, 4096 }) {
        auto e = std::make_unique<Engine>();
        e->prepare(sr, block);
        renders.push_back(renderEngine(*e, comp, 16.0, block, sr));
    }
    bool identical = true;
    for (size_t k = 1; k < renders.size(); ++k) identical = identical && renders[k] == renders[0];
    check(identical, "output identical for blocks of 1, 64, 1000 and 4096 samples");

    auto e = std::make_unique<Engine>();
    e->prepare(sr, 256);
    e->params().parseText("bass.level=-36 mix.bass_mute=1 kick.click_level=0");
    NoteEvent k;
    k.beat = 3.3;
    k.part = Part::Kick;
    k.velocity = 127;
    e->pushEvent(k);
    std::vector<float> L(96000), R(96000);
    e->process(L.data(), R.data(), 96000);
    size_t first = 0;
    while (first < L.size() && L[first] == 0.0f) ++first;
    const double expect = 3.3 * 60.0 / 145.0 * sr + e->latencySamples();
    check(std::fabs(static_cast<double>(first) - expect) <= 2.0, "event fires on its sample (plus the limiter's reported latency)", fmt("first sound at %zu, beat 3.3 is sample %.2f", first, expect));

    auto er = std::make_unique<Engine>();
    er->prepare(sr, 512);
    TempoMap tm;
    tm.add(0.0, 138.0, true);
    tm.add(64.0, 150.0, false);
    er->setTempoMap(tm);
    er->params().parseText("mix.bass_mute=1 kick.click_level=0");
    NoteEvent late = k;
    late.beat = 60.0;
    er->pushEvent(late);
    const size_t total = static_cast<size_t>(tm.secondsAt(61.0) * sr);
    std::vector<float> RL(total), RR(total);
    er->process(RL.data(), RR.data(), static_cast<int>(total));
    size_t onset = 0;
    while (onset < RL.size() && RL[onset] == 0.0f) ++onset;
    const double expectRamp = tm.secondsAt(60.0) * sr + er->latencySamples();
    check(std::fabs(static_cast<double>(onset) - expectRamp) <= 32.0, "event in a tempo ramp lands within a chunk of the integral", fmt("sample %zu vs %.1f", onset, expectRamp));

    auto e1 = std::make_unique<Engine>(), e2 = std::make_unique<Engine>();
    e1->prepare(sr, 256); e2->prepare(sr, 256);
    check(renderEngine(*e1, comp, 16.0, 256, sr) == renderEngine(*e2, comp, 16.0, 256, sr), "two engines, same seed: identical output");

    auto eb = std::make_unique<Engine>();
    eb->prepare(sr, 256);
    // Bass alone, inside the first core: in the intro it does not play yet, and a pad on the downbeat
    // would count as energy in the kick's window.
    eb->params().parseText("mix.kick_mute=1 mix.perc_mute=1 mix.acid_mute=1 mix.lead_mute=1 mix.arp_mute=1 mix.pad_mute=1 mix.sfx_mute=1");
    const int coreBar = firstCoreBar(eb->params(), comp);
    const std::vector<float> bassOnly = renderEngine(*eb, comp, (coreBar + 16) * static_cast<double>(kBeatsPerBar), 256, sr);
    const double beatSamples = 60.0 / 145.0 * sr;
    const size_t from = static_cast<size_t>(coreBar * kBeatsPerBar * beatSamples);
    double inWindow = 0.0, all = 0.0;
    size_t nWindow = 0, nAll = 0;
    for (size_t i = from; i < bassOnly.size(); ++i) {
        const double ph = std::fmod(static_cast<double>(i), beatSamples);
        const double v = static_cast<double>(bassOnly[i]) * bassOnly[i];
        all += v;
        ++nAll;
        if (ph < 0.060 * sr) { inWindow += v; ++nWindow; }
    }
    const double ratio = powDb((inWindow / nWindow) / (all / nAll));
    check(ratio < -30.0, "bass energy in the kick's first 60 ms is negligible", fmt("%.1f dB relative to the bass's mean", ratio));

    auto em = std::make_unique<Engine>();
    em->prepare(sr, 256);
    std::vector<float> mr;
    const std::vector<float> ml = renderEngine(*em, comp, 64.0, 256, sr, &mr);
    LoudnessMeter meter;
    meter.prepare(sr);
    meter.process(ml.data(), mr.data(), static_cast<int>(ml.size()));
    const LoudnessReading rd = meter.read();
    float peak = 0.0f;
    for (float v : ml) peak = std::max(peak, std::fabs(v));
    check(rd.integrated > -20.0f && rd.integrated < -4.0f, "kick and bass loop measures in a sane loudness window", fmt("%.1f LUFS, true peak %.2f dBTP", static_cast<double>(rd.integrated), static_cast<double>(rd.truePeak)));
    check(20.0f * std::log10(peak) <= -0.3f + 0.01f, "sample peak at or below the ceiling", fmt("%.2f dBFS", 20.0 * std::log10(static_cast<double>(peak))));

    // The tail limit inside the engine follows the pattern: an offbeat bass leaves the kick longer.
    auto et = std::make_unique<Engine>();
    et->prepare(sr, 256);
    et->params().parseText("kick.amp_decay=800");
    std::vector<float> tl(512), tr(512);
    et->process(tl.data(), tr.data(), 512);
    const float rolling = et->effective(et->params().base(Module::Kick) + kick::AmpDecay);
    et->params().parseText("compose.bass_pattern=Offbeat");
    et->process(tl.data(), tr.data(), 512);
    const float offbeat = et->effective(et->params().base(Module::Kick) + kick::AmpDecay);
    check(rolling < 800.0f && offbeat > 1.9f * rolling, "kick decay limited by the first bass slot (rolling tighter than offbeat)",
          fmt("rolling %.0f ms, offbeat %.0f ms", static_cast<double>(rolling), static_cast<double>(offbeat)));
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
double measureLock(const char* settings, double bpm, double& spread, double& coherentDb)
{
    const double sr = 48000.0;
    const double slotT = 0.25 * 60.0 / bpm;
    double kickPhaseAtSlot = 0.0;
    Kick kickModel;
    // Measured inside the first core: in the intro neither kick nor bass has started (Form.h).
    int coreBeat = 0;
    auto render = [&](const char* solo, bool keepKick) {
        auto e = std::make_unique<Engine>();
        e->prepare(sr, 256);
        e->params().parseText(fmt("compose.bpm=%g compose.bass_variation=0 compose.kick_pattern=Four master.clip=Off master.limiter=Off master.clipper=Off master.comp_ratio=1 master.auto_gain=Off compose.level_match=Off %s %s", bpm, settings, solo).c_str());
        Composer c(1);
        coreBeat = firstCoreBar(e->params(), c) * kBeatsPerBar;
        std::vector<float> y = renderEngine(*e, c, coreBeat + 32.0, 256, sr);
        if (keepKick) { kickModel = e->kick(); kickPhaseAtSlot = e->kick().outputPhaseAt(slotT); }
        return y;
    };
    // Nothing but the part being measured: since Phase 5 a core carries acid, arp and pad as well, and
    // their energy leaks into a projection that is only two fundamental periods long.
    const char* const kOnlyKick = "mix.bass_mute=1 mix.perc_mute=1 mix.acid_mute=1 mix.lead_mute=1 mix.arp_mute=1 mix.pad_mute=1 mix.sfx_mute=1";
    const char* const kOnlyBass = "mix.kick_mute=1 mix.perc_mute=1 mix.acid_mute=1 mix.lead_mute=1 mix.arp_mute=1 mix.pad_mute=1 mix.sfx_mute=1";
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
        const double pb = phaseAgainst(bassY.data() + w0, 2 * per + 8, refBass);   // bass phase at the onset
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

void testPhaseLock()
{
    section("kick and bass phase at the first sixteenth");
    std::string detail;
    double worstKick = 0.0, worstBass = 0.0, worstSpread = 0.0, offLo = 1e9, offHi = -1e9;
    for (double bpm : { 138.0, 142.0, 145.0, 148.0 }) {
        double spread = 0.0, coh = 0.0;
        const double dk = measureLock("bass.kick_lock=Kick follows bass", bpm, spread, coh);
        worstKick = std::max(worstKick, std::fabs(dk));
        worstSpread = std::max(worstSpread, spread);
        const double db = measureLock("bass.kick_lock=Bass follows kick", bpm, spread, coh);
        worstBass = std::max(worstBass, std::fabs(db));
        const double off = measureLock("bass.kick_lock=Off", bpm, spread, coh);
        offLo = std::min(offLo, off);
        offHi = std::max(offHi, off);
        detail += fmt("%.0f BPM: %+.1f / %+.1f / off %+.1f   ", bpm, dk, db, off);
    }
    check(worstKick < 6.0 && worstBass < 6.0 && offHi - offLo > 60.0,
          "kick lock aligns the phases at every tempo (without it they wander with the tempo)", detail);
    check(worstSpread < 0.05, "bass onset phase constant from beat to beat (sub-sample onsets)", fmt("spread %.3f degrees", worstSpread));
}

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

/** @brief A kit prepared from a parameter store (all lanes updated). */
std::unique_ptr<PercKit> makeKit(const ParamStore& p, int key = 6, int scale = 1)
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
std::vector<float> renderKit(PercKit& kit, size_t n)
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
double lowShareDb(const std::vector<float>& x, double hz, bool windowed = false)
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
        check(modes[0] > 0 && modes[1] > 0 && modes[2] > 0 && backbeat > 5 && backbeat < 35 && maxLayers - minLayers >= 2 && macroSpread > 0.5 && overrides > 0,
              "percussion varies over the night: hat modes, backbeat, layers, kit recipe",
              fmt("hat modes %d/%d/%d, clap backbeat in %d of 40, layers %d..%d, %d lane switches", modes[0], modes[1], modes[2], backbeat, minLayers, maxLayers, overrides));
    }
}

// ---------------------------------------------------------------------------------------------
// Phase 3: constrained sampling, diode ladder, acid, polyphonic engine, melody
// ---------------------------------------------------------------------------------------------

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

/** @brief An acid voice with the given settings. */
std::unique_ptr<Acid> makeAcid(const char* settings, ParamStore& p)
{
    p.parseText(settings);
    auto a = std::make_unique<Acid>();
    a->prepare(48000.0);
    std::vector<float> v = moduleValues(p, Module::Acid);
    a->update(v.data(), 145.0);
    return a;
}

std::vector<float> renderMono(const std::function<void(float*, float*, int)>& proc, size_t n)
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
double centroid(const float* x, double sr, size_t n)
{
    const std::vector<double> pw = powerSpectrum(x, n);
    double num = 0.0, den = 0.0;
    for (size_t k = 1; k < pw.size(); ++k) { num += pw[k] * k * sr / static_cast<double>(n); den += pw[k]; }
    return num / den;
}

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

/** @brief A polyphonic engine instance with the given settings. */
std::unique_ptr<Poly> makePoly(const char* settings, ParamStore& p, PolyInstance inst)
{
    p.parseText(settings);
    auto e = std::make_unique<Poly>();
    e->prepare(48000.0);
    std::vector<float> v = moduleValues(p, Module::Poly, static_cast<int>(inst));
    e->update(v.data(), 145.0);
    return e;
}

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
        auto e = makePoly("lead.detune=1 lead.dynamic_detune=0 lead.mix=0.75 lead.cutoff=18000 lead.env_amount=0 lead.key_track=0 lead.resonance=0 "
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
        auto e = makePoly("lead.osc=FM lead.fm_ratio=3.5 lead.fm_index=4 lead.fm_decay=5 lead.detune=0 lead.mix=0 lead.cutoff=18000 lead.env_amount=0 lead.hp_track=0 lead.hp_floor=150 "
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
                auto e = makePoly(fmt("%s.osc=%d %s.delay_send=0.5 %s.amp_sustain=1", name, osc, name).c_str(), p, inst);
                e->noteOn(inst == PolyInstance::Lead ? kLeadLowest : kArpLowest, 1.0f, 1.0, 30000, 0.0);
                e->noteOn((inst == PolyInstance::Lead ? kLeadLowest : kArpLowest) + 3, 1.0f, 1.0, 30000, 0.0);
                const std::vector<float> y = renderMono([&](float* L, float* R, int n) { e->process(L, R, n); }, 32768);
                worst = std::max(worst, lowShareDb(y, 140.0, true));
            }
        }
        check(worst < -30.0, "lead and arp at their lowest notes, every oscillator: under -30 dB below 140 Hz", fmt("worst %.1f dB", worst));
    }
}

void testMelody()
{
    section("melody: chords, acid, lead, arp");
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
    std::vector<Range> leadR(static_cast<size_t>(tracks * kMaxSections)), arpR(static_cast<size_t>(tracks * kMaxSections));
    for (const NoteEvent& e : ev) {
        if (e.part != Part::Acid && e.part != Part::Lead && e.part != Part::Arp) continue;
        const int bar = static_cast<int>(e.beat / kBeatsPerBar);
        const int ti = c.trackOfBar(p, bar);
        const TrackPlan& t = c.track(p, ti);
        const int inTrack = bar - t.firstBar;
        ++notes;
        if (!inScale(t.scale, e.pitch - t.key)) ++outside;
        const int lowest = e.part == Part::Acid ? kAcidLowest : (e.part == Part::Lead ? kLeadLowest : kArpLowest);
        if (e.pitch < lowest) ++tooLow;
        int pcs[3];
        chordTones(t.scale, t.melody.chordDegree[chordIndexAt(t.melody, inTrack)], pcs);
        const int pc = ((e.pitch - t.key) % 12 + 12) % 12;
        const bool chordTone = pc == pcs[0] || pc == pcs[1] || pc == pcs[2];
        const double inBar = e.beat - bar * kBeatsPerBar;
        if (e.part == Part::Lead && std::fabs(inBar - std::round(inBar / 2.0) * 2.0) < 1e-9) { ++strong; if (!chordTone) ++weak; }
        if (e.part == Part::Arp) { ++arpNotes; if (!chordTone) ++arpOff; }
        if (e.part == Part::Acid) acidNotes.push_back(&e);
        const size_t block = static_cast<size_t>(ti * kMaxSections + sectionOfBar(t.form, inTrack));
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
    check(weak == 0 && arpOff == 0 && strong > 50, "lead on the strong beats and every arp note are chord tones",
          fmt("%d of %d strong lead notes, %d of %d arp notes off the chord", weak, strong, arpOff, arpNotes));
    check(tooLow == 0, "depth rule in the score: acid from D3, lead from B3, arp from G3", fmt("%d notes too low", tooLow));
    check(slides > 10 && slideGaps == 0, "every acid slide overlaps the note it slides into", fmt("%d slides, %d with a gap", slides, slideGaps));
    check(sharedBlocks > 0 && masked == 0, "where lead and arp play together their ranges overlap by at most two semitones",
          fmt("%d shared sections, %d masked", sharedBlocks, masked));

    // Variety over a night.
    {
        ParamStore q;
        q.parseText("compose.level_match=Off master.auto_gain=Off");
        Composer cv(4711);
        std::vector<uint64_t> acids;
        int progressions = 0, styles[4] = {}, oscs[3] = {}, squelch = 0, silent = 0;
        for (int i = 0; i < 24; ++i) {
            const TrackPlan t = cv.track(q, i);
            uint64_t h = 1469598103934665603ull;
            for (const MelodyNote& n : t.melody.acid[0]) h = (h ^ static_cast<uint64_t>(n.step * 131 + n.rel + 40)) * 1099511628211ull;
            acids.push_back(h);
            if (t.melody.chordDegree[1] || t.melody.chordDegree[2] || t.melody.chordDegree[3]) ++progressions;
            ++styles[t.melody.arpStyle];
            if (t.melody.leadOsc >= 0) ++oscs[t.melody.leadOsc];
            squelch += t.melody.acidSquelch == 1 ? 1 : 0;
            if (!t.melody.present[0] && !t.melody.present[1] && !t.melody.present[2]) ++silent;
        }
        std::sort(acids.begin(), acids.end());
        const int distinct = static_cast<int>(std::unique(acids.begin(), acids.end()) - acids.begin());
        const int styleCount = (styles[0] > 0) + (styles[1] > 0) + (styles[2] > 0) + (styles[3] > 0);
        check(distinct == 24 && progressions >= 12 && styleCount >= 3 && oscs[0] > 0 && oscs[1] + oscs[2] > 0 && squelch > 0 && silent == 0,
              "melodic identity changes from track to track",
              fmt("%d distinct acid riffs of 24, %d moving progressions, %d arp styles, lead osc %d/%d/%d, %d squelched, %d without melody",
                  distinct, progressions, styleCount, oscs[0], oscs[1], oscs[2], squelch, silent));
    }

    // Depth rule in the rendered mix: acid, lead and arp together put nothing under 140 Hz.
    {
        auto e = std::make_unique<Engine>();
        e->prepare(48000.0, 512);
        e->params().parseText("compose.track_bars=64 compose.acid_amount=1 compose.lead_amount=1 compose.arp_amount=1 mix.kick_mute=1 mix.bass_mute=1 mix.perc_mute=1");
        Composer cm(3);
        const std::vector<float> y = renderEngine(*e, cm, 64.0 * kBeatsPerBar, 512, 48000.0);
        const std::vector<float> tail(y.begin() + static_cast<long>(y.size() / 2), y.end());
        double pk = 0.0;
        for (float v : tail) pk = std::max(pk, static_cast<double>(std::fabs(v)));
        const double low = lowShareDb(tail, 140.0, true);
        check(pk > 0.01 && low < -30.0, "rendered melodic parts: under -30 dB of their power below 140 Hz", fmt("%.1f dB (peak %.2f)", low, pk));
    }

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

/**
 * @brief Power share of everything outside +-6 bins of the harmonics of @p f0 between 100 Hz and 18 kHz.
 *
 * Six bins, not three: the Blackman-Harris window's main lobe is four bins either side, and at three the
 * leakage of the fundamental counted as aliasing (-39 dB where the signal had -80).
 */
double inharmonicDb(const std::vector<float>& y, size_t offset, double f0, double sr, double relTolerance = 0.0)
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
        auto e = makePoly("pad.table=Classic pad.position=0.5 pad.pos_env=0 pad.pos_lfo_depth=0 pad.detune=0 pad.cutoff=18000 pad.amp_attack=0.3 "
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

void testPads()
{
    section("pads: voicings");
    // Voice leading against an independent enumeration.
    Rng r;
    r.seed(99);
    int mismatches = 0, badNotes = 0, cases = 0;
    for (int trial = 0; trial < 200; ++trial) {
        const int scale = r.below(kNumScales), key = r.below(12);
        std::vector<int> prev = voiceChord(scale, r.below(7), key, nullptr);
        const int degree = r.below(7);
        const std::vector<int> got = voiceChord(scale, degree, key, &prev);
        int pcs[3];
        chordTones(scale, degree, pcs);
        int best = 1 << 30;
        for (int a = kPadLowest; a <= kPadHighest; ++a)
            for (int b = a + 1; b <= kPadHighest; ++b)
                for (int c = b + 1; c <= kPadHighest; ++c)
                    for (int d = c + 1; d <= kPadHighest; ++d) {
                        const int v[4] = { a, b, c, d };
                        bool ok = b - a <= 12 && c - b <= 12 && d - c <= 12, has[3] = {};
                        for (int x : v) {
                            const int pc = ((x - key) % 12 + 12) % 12;
                            bool tone = false;
                            for (int k = 0; k < 3; ++k) if (pc == pcs[k]) { has[k] = true; tone = true; }
                            ok = ok && tone;
                        }
                        if (!ok || !(has[0] && has[1] && has[2])) continue;
                        best = std::min(best, std::abs(a - prev[0]) + std::abs(b - prev[1]) + std::abs(c - prev[2]) + std::abs(d - prev[3]));
                    }
        ++cases;
        if (got.size() != 4 || voicingMovement(got, prev) != best) ++mismatches;
        for (int x : got) if (x < kPadLowest || x > kPadHighest) ++badNotes;
    }
    check(mismatches == 0 && badNotes == 0, "every pad voicing moves the voices as little as any valid voicing could",
          fmt("%d of %d differ from brute force, %d notes out of range", mismatches, cases, badNotes));

    // In the score: pad notes are chord tones of their bar, held to the next chord. The first sixteen
    // bars of a track are left out: there the previous track's pads still sound, on its own chords,
    // which is exactly what the transition asks for (PLAN 6.7) and has its own check in testForm.
    ParamStore p;
    p.parseText("compose.track_bars=128 compose.pad_amount=1 compose.level_match=Off master.auto_gain=Off");
    Composer c(515);
    std::vector<NoteEvent> ev;
    c.composeBars(p, 0, 4 * 128, ev);
    int pads = 0, off = 0;
    for (const NoteEvent& e : ev) {
        if (e.part != Part::Pad) continue;
        const int bar = static_cast<int>(e.beat / kBeatsPerBar);
        const int ti = c.trackOfBar(p, bar);
        const TrackPlan& t = c.track(p, ti);
        if (ti > 0 && bar - t.firstBar < 16) continue;
        int pcs[3];
        chordTones(t.scale, t.melody.chordDegree[chordIndexAt(t.melody, bar - t.firstBar)], pcs);
        const int pc = ((e.pitch - t.key) % 12 + 12) % 12;
        ++pads;
        if (!(pc == pcs[0] || pc == pcs[1] || pc == pcs[2]) || e.pitch < kPadLowest) ++off;
    }
    check(pads > 100 && off == 0, "pad notes are chord tones of their bar, G3 and above", fmt("%d of %d off", off, pads));
}

void testGateAndDuck()
{
    section("trance gate and sidechain");
    // The gate's opening: raised-cosine edges, open for the duty cycle, closed after.
    {
        const double a = 0.02, rel = 0.03;
        const float mid = TranceGate::open(0.0 + 0.5 * a, 0, 0.5f, a, rel);
        const float held = TranceGate::open(0.1, 0, 0.5f, a, rel);
        const float falling = TranceGate::open(0.125 + 0.5 * rel, 0, 0.5f, a, rel);
        const float closed = TranceGate::open(0.2, 0, 0.5f, a, rel);
        const float offStep = TranceGate::open(0.01, 2, 0.5f, a, rel);   // rolling: the first sixteenth of a beat is off
        check(std::fabs(mid - 0.5f) < 1e-6f && held == 1.0f && std::fabs(falling - 0.5f) < 1e-6f && closed == 0.0f && offStep == 0.0f,
              "gate: half open halfway up each raised-cosine edge, open for the duty cycle, shut after and on pattern rests",
              fmt("%.3f %.3f %.3f %.3f %.3f", static_cast<double>(mid), static_cast<double>(held), static_cast<double>(falling), static_cast<double>(closed), static_cast<double>(offStep)));
    }
    // The gate and the duck in the engine, on a held pad chord.
    auto render = [](const char* settings) {
        auto e = std::make_unique<Engine>();
        e->prepare(48000.0, 256);
        e->params().parseText(fmt("mix.kick_mute=1 mix.bass_mute=1 mix.perc_mute=1 master.limiter=Off master.clipper=Off master.clip=Off "
                                  "master.comp_ratio=1 fx.hall_return=-36 fx.room_return=-36 pad.hall_send=0 pad.amp_attack=1 %s", settings).c_str());
        for (int pitch : { 60, 64, 67, 71 }) {
            NoteEvent n;
            n.part = Part::Pad; n.pitch = static_cast<uint8_t>(pitch); n.length = 64.0f; n.velocity = 100;
            e->pushEvent(n);
        }
        for (int b = 0; b < 32; ++b) {
            NoteEvent k;
            k.part = Part::Kick; k.beat = b; k.velocity = 127;
            e->pushEvent(k);
        }
        std::vector<float> L(48000 * 12), R(48000 * 12);
        e->process(L.data(), R.data(), static_cast<int>(L.size()));
        return L;
    };
    const double beat = 60.0 / 145.0 * 48000.0;
    auto windowDb = [&](const std::vector<float>& y, double fromBeat, double toBeat) {
        double s = 0.0;
        size_t n = 0;
        for (int b = 8; b < 24; ++b)
            for (size_t i = static_cast<size_t>((b + fromBeat) * beat); i < static_cast<size_t>((b + toBeat) * beat); ++i) { s += static_cast<double>(y[i]) * y[i]; ++n; }
        return 10.0 * std::log10(s / n);
    };
    {
        const std::vector<float> gated = render("pad.duck=0 pad.gate=On pad.gate_pattern=Eighths pad.gate_depth=0.9 pad.gate_duty=0.5 pad.gate_tone=0");
        // Eighths: open in the first half of each eighth (0 .. 0.25 beats), shut in the second.
        const double open = windowDb(gated, 0.03, 0.22), shut = windowDb(gated, 0.28, 0.47);
        const double want = 20.0 * std::log10(1.0 - 0.9);
        check(std::fabs((shut - open) - want) < 1.0, "gate at depth 0.9 takes the closed half of each eighth 20 dB down", fmt("%.1f dB (expected %.1f)", shut - open, want));
    }
    {
        const std::vector<float> ducked = render("pad.duck=0.5 mix.duck_attack=1 mix.duck_hold=40 mix.duck_release=100");
        const double early = windowDb(ducked, 0.01, 0.08), late = windowDb(ducked, 0.6, 0.9);
        check(std::fabs((early - late) - 20.0 * std::log10(0.5)) < 0.7, "kick sidechain: the pad sits 6 dB down while the duck holds, back after",
              fmt("%.2f dB", early - late));
    }
}

void testReverb()
{
    section("send reverbs");
    const double sr = 48000.0;
    Reverb rv;
    rv.prepare(sr);
    rv.set(1.6f, 2.0f, 0.2f, 0.0f, 150.0f, 20000.0f);
    const size_t n = static_cast<size_t>(sr * 4.0);
    std::vector<float> inL(n, 0.0f), inR(n, 0.0f), outL(n), outR(n);
    // A burst of noise, so the tail is dense from the start.
    Rng r;
    for (size_t i = 0; i < 480; ++i) { inL[i] = r.bipolar(); inR[i] = r.bipolar(); }
    rv.process(inL.data(), inR.data(), outL.data(), outR.data(), static_cast<int>(n));
    // Schroeder backward integration of the energy, T20 fitted between -5 and -25 dB, times three.
    std::vector<double> edc(n);
    double acc = 0.0;
    for (size_t i = n; i-- > 0;) { acc += static_cast<double>(outL[i]) * outL[i] + static_cast<double>(outR[i]) * outR[i]; edc[i] = acc; }
    size_t i5 = 0, i25 = 0;
    for (size_t i = 0; i < n; ++i) {
        const double db = 10.0 * std::log10(edc[i] / edc[0]);
        if (i5 == 0 && db <= -5.0) i5 = i;
        if (i25 == 0 && db <= -25.0) { i25 = i; break; }
    }
    const double t60 = 3.0 * static_cast<double>(i25 - i5) / sr;
    check(std::fabs(t60 / 2.0 - 1.0) < 0.15, "hall: measured decay time within 15 % of the setting", fmt("T60 %.2f s for 2.00 s", t60));
    std::vector<float> mono(n);
    for (size_t i = 0; i < n; ++i) mono[i] = 0.5f * (outL[i] + outR[i]);
    // Noise in, so the return's own spectrum shows.
    Reverb rv2;
    rv2.prepare(sr);
    rv2.set(1.6f, 4.0f, 0.4f, 0.0f, 300.0f, 9000.0f);
    for (size_t i = 0; i < n; ++i) { inL[i] = r.bipolar(); inR[i] = r.bipolar(); }
    rv2.process(inL.data(), inR.data(), outL.data(), outR.data(), static_cast<int>(n));
    for (size_t i = 0; i < n; ++i) mono[i] = 0.5f * (outL[i] + outR[i]);
    const double low = lowShareDb(std::vector<float>(mono.begin() + 48000, mono.begin() + 48000 + 131072), 140.0, true);
    check(low < -30.0, "reverb return with white noise in: under -30 dB of its power below 140 Hz", fmt("%.1f dB", low));
}

void testDynamics()
{
    section("master dynamics");
    const double sr = 48000.0;
    // Compressor: the static curve against Giannoulis et al. eq. 4, and the settled gain on steady input.
    {
        BusCompressor c;
        c.prepare(sr);
        c.set(-20.0f, 4.0f, 10.0f, 5.0f, 50.0f);
        const double knee = c.curve(-20.0), above = c.curve(-5.0), below = c.curve(-30.0);
        const bool formula = std::fabs(knee - (-20.0 - 0.75 * 25.0 / 20.0)) < 1e-9 && std::fabs(above - (-20.0 + 15.0 / 4.0)) < 1e-9 && below == -30.0;
        double worst = 0.0;
        for (double level : { -40.0, -24.0, -20.0, -15.0, -6.0, 0.0 }) {
            BusCompressor d;
            d.prepare(sr);
            d.set(-20.0f, 4.0f, 10.0f, 5.0f, 50.0f);
            std::vector<float> L(48000, static_cast<float>(std::pow(10.0, level / 20.0))), R = L;
            d.process(L.data(), R.data(), 48000);
            worst = std::max(worst, std::fabs(20.0 * std::log10(L.back()) - d.curve(level)));
        }
        check(formula && worst < 0.01, "bus compressor: soft-knee curve of Giannoulis et al. and the gain it settles to", fmt("largest deviation %.4f dB", worst));
    }
    // True-peak estimate on sines between the samples.
    {
        TruePeakInterpolator tp;
        double worst = 0.0, naive = 1.0;
        for (double f : { 0.02, 0.05, 0.13, 0.2, 0.25 }) {
            for (double phase : { 0.0, 0.3, 0.785398, 1.7 }) {
                std::vector<float> x(64);
                for (int i = 0; i < 64; ++i) x[static_cast<size_t>(i)] = static_cast<float>(std::sin(2.0 * kPiD * f * i + phase));
                double est = 0.0, samplePeak = 0.0;
                for (int i = 8; i < 56; ++i) {
                    est = std::max({ est, std::fabs(static_cast<double>(x[static_cast<size_t>(i)])), tp.between(x.data() + i) });
                    samplePeak = std::max(samplePeak, std::fabs(static_cast<double>(x[static_cast<size_t>(i)])));
                }
                worst = std::max(worst, std::fabs(20.0 * std::log10(est)));
                naive = std::min(naive, samplePeak);
            }
        }
        // Four points per sample cannot see the crest of a sine at 0.25 fs to better than cos(pi/16): 0.17 dB.
        check(worst < 0.17 && naive < 0.75, "true peak of sines up to 0.25 fs within the 0.17 dB of 4x sampling (their largest samples read up to 3 dB low)",
              fmt("worst %.3f dB; lowest sample peak %.2f dB", worst, 20.0 * std::log10(naive)));
    }
    // Limiter: program 12 dB over the ceiling comes out at the ceiling, measured with an exact band-limited
    // 16x interpolation; below the ceiling the signal passes, only delayed.
    {
        constexpr size_t N = 1u << 15;
        // Program-like material: a decaying 60 Hz kick every beat, tones at 7 and 9 kHz and noise low-passed
        // at 10 kHz (music has little energy near Nyquist, where 4x interpolation cannot see the peaks).
        Rng r;
        Svf nlL, nlR;
        nlL.setQ(10000.0f, 0.707f, 48000.0f);
        nlR.copyCoefficients(nlL);
        std::vector<float> L(N), R(N);
        for (size_t i = 0; i < N; ++i) {
            const double t = static_cast<double>(i) / sr;
            const double beatPos = std::fmod(t * 145.0 / 60.0, 1.0);
            const double kick = std::exp(-beatPos * 40.0) * std::sin(2.0 * kPiD * 60.0 * t);
            const float nl = nlL.lp(r.bipolar()), nr = nlR.lp(r.bipolar());
            L[i] = static_cast<float>(3.5 * (0.6 * kick + 0.3 * std::sin(2.0 * kPiD * 7000.0 * t + 0.7) + 0.3 * nl));
            R[i] = static_cast<float>(3.5 * (0.6 * kick + 0.3 * std::sin(2.0 * kPiD * 9000.0 * t) + 0.3 * nr));
        }
        auto exactTruePeak = [&](const std::vector<float>& x, size_t from) {
            constexpr size_t M = 1u << 13;
            double peak = 0.0;
            for (size_t start = from; start + M <= x.size(); start += M / 2) {
                std::vector<std::complex<double>> a(M), b(M * 16);
                for (size_t i = 0; i < M; ++i) a[i] = x[start + i];
                fft(a);
                for (size_t k = 0; k < M / 2; ++k) { b[k] = a[k]; b[M * 16 - 1 - k] = a[M - 1 - k]; }
                // inverse by forward FFT of the reversed spectrum: b holds X(k); x(t) = (1/M) sum X(k) e^{+j..}
                std::vector<std::complex<double>> c(M * 16);
                for (size_t k = 0; k < M * 16; ++k) c[k] = b[(M * 16 - k) % (M * 16)];
                fft(c);
                for (size_t i = M * 4; i < M * 12; ++i) peak = std::max(peak, std::fabs(c[i].real()) / M);
            }
            return peak;
        };
        TruePeakLimiter lim;
        lim.prepare(sr, 1.5f);
        lim.set(-1.0f, 20.0f);
        std::vector<float> yl = L, yr = R;
        lim.process(yl.data(), yr.data(), static_cast<int>(N));
        const double tpOut = 20.0 * std::log10(std::max(exactTruePeak(yl, 4096), exactTruePeak(yr, 4096)));
        // The same program only sample-clipped at the ceiling shows what the true-peak estimate is for.
        std::vector<float> cl = L;
        const float ceil1 = dbToGain(-1.0f);
        for (float& v : cl) v = clampv(v, -ceil1, ceil1);
        const double tpClip = 20.0 * std::log10(exactTruePeak(cl, 4096));
        // Below the ceiling: delayed by the latency, otherwise untouched.
        TruePeakLimiter quiet;
        quiet.prepare(sr, 1.5f);
        quiet.set(-1.0f, 20.0f);
        std::vector<float> ql(4096), qr(4096);
        for (size_t i = 0; i < 4096; ++i) { ql[i] = 0.1f * static_cast<float>(std::sin(0.01 * i)); qr[i] = ql[i]; }
        std::vector<float> ol = ql, orr = qr;
        quiet.process(ol.data(), orr.data(), 4096);
        bool passes = true;
        const int lat = quiet.latency();
        for (size_t i = static_cast<size_t>(lat); i < 4096; ++i) passes = passes && ol[i] == ql[i - static_cast<size_t>(lat)];
        // The gain falls as a ramp over the lookahead window, not as a step: a quiet constant with one spike
        // shows the gain directly (output over the delayed input).
        TruePeakLimiter ramp;
        ramp.prepare(sr, 1.5f);
        ramp.set(-1.0f, 20.0f);
        std::vector<float> dl(4096, 0.1f), dr(4096, 0.1f);
        dl[2000] = dr[2000] = 3.0f;
        std::vector<float> gl = dl, gr = dr;
        ramp.process(gl.data(), gr.data(), 4096);
        double largestStep = 0.0, deepest = 1.0;
        for (size_t i = static_cast<size_t>(lat) + 1; i < 4096; ++i) {
            const size_t src = i - static_cast<size_t>(lat);
            if (src == 2000 || src - 1 == 2000) continue;
            const double g0 = gl[i - 1] / dl[src - 1], g1 = gl[i] / dl[src];
            largestStep = std::max(largestStep, std::fabs(g1 - g0));
            deepest = std::min(deepest, g1);
        }
        const int window = lat - TruePeakInterpolator::kHalf + 1;
        check(largestStep <= (1.0 - deepest) / window * 1.05 + 1e-6, "limiter: the gain ramps down over the lookahead window instead of stepping",
              fmt("largest change %.4f per sample, reduction to %.3f over %d samples", largestStep, deepest, window));
        check(tpOut <= -1.0 + 0.17 && tpClip > 0.0 && passes, "limiter: 12 dB over the ceiling comes out at -1 dBTP within 4x sampling (a sample clip leaves intersample overs); quiet input only delayed",
              fmt("true peak %.2f dBTP limited, %+.2f dBTP sample-clipped; latency %d samples", tpOut, tpClip, lat));
    }
}

void testSfx()
{
    section("effects");
    const double sr = 48000.0;
    ParamStore p;
    auto renderType = [&](SfxType type, double seconds, double tail) {
        Sfx s;
        s.prepare(sr);
        std::vector<float> v = moduleValues(p, Module::Sfx);
        s.update(v.data(), 6);
        s.trigger(type, static_cast<int>(seconds * sr), 1.0f, 0.0);
        return renderMono([&](float* L, float* R, int n) { s.process(L, R, n); }, static_cast<size_t>((seconds + tail) * sr));
    };
    auto rms = [](const std::vector<float>& y, size_t a, size_t n) { double e = 0.0; for (size_t i = a; i < a + n && i < y.size(); ++i) e += static_cast<double>(y[i]) * y[i]; return 10.0 * std::log10(e / n + 1e-30); };
    {
        const std::vector<float> y = renderType(SfxType::Riser, 4.0, 0.2);
        const double first = rms(y, 4800, 19200), last = rms(y, 172800, 19200), after = rms(y, 196800, 4800);
        std::vector<float> a(y.begin() + 9600, y.begin() + 9600 + 4096), b(y.begin() + 180000, y.begin() + 180000 + 4096);
        const double c0 = centroid(a.data(), sr, 4096), c1 = centroid(b.data(), sr, 4096);
        check(last > first + 20.0 && c1 > 3.0 * c0 && after < last - 40.0, "riser: louder and brighter to its end, silent 20 ms after it",
              fmt("%+.1f dB, centroid %.0f -> %.0f Hz, %.1f dB after the end", last - first, c0, c1, after - last));
    }
    {
        const std::vector<float> y = renderType(SfxType::ReverseSwell, 2.0, 0.1);
        double best = -1e9;
        size_t at = 0;
        for (size_t i = 0; i + 2400 <= 96000; i += 2400) { const double v = rms(y, i, 2400); if (v > best) { best = v; at = i; } }
        check(at >= 96000 - 4800, "reverse swell: loudest in the last 100 ms before its target", fmt("loudest window at %.2f s of 2.00 s", at / sr));
    }
    {
        const std::vector<float> y = renderType(SfxType::Impact, 0.5, 1.5);
        double best = -1e9;
        size_t at = 0;
        for (size_t i = 0; i + 240 <= 48000; i += 240) { const double v = rms(y, i, 240); if (v > best) { best = v; at = i; } }
        check(at < 960 && rms(y, 48000, 4800) < rms(y, 0, 4800) - 20.0, "impact: loudest 5 ms window within the first 20 ms, 20 dB down after a second",
              fmt("loudest window at %.1f ms", 1000.0 * at / sr));
    }
    {
        double worst = -1e9;
        for (int k = 0; k < kNumSfxTypes; ++k) {
            const std::vector<float> y = renderType(static_cast<SfxType>(k), 2.0, 0.8);
            worst = std::max(worst, lowShareDb(y, 140.0, true));
        }
        check(worst < -30.0, "every effect type: under -30 dB of its power below 140 Hz", fmt("worst %.1f dB", worst));
    }
    // Placement: since Phase 5 the effects sit on the boundaries of the form (Form.h), not on
    // sixteen-bar block entries: a riser climbs into a drop and ends on it, the formant shot is the
    // pre-drop "Abriss" on the last beat of the buildup, the impact marks the drop's downbeat.
    {
        ParamStore q;
        q.parseText("compose.sfx_amount=1 compose.track_bars=256 compose.level_match=Off master.auto_gain=Off");
        Composer c(303);
        int impacts = 0, misplaced = 0, shots = 0, risers = 0, sweeps = 0;
        for (int ti = 0; ti < 8; ++ti) {
            const TrackPlan t = c.track(q, ti);
            // The bars on which a core begins after a buildup, and the last bar of every buildup.
            std::vector<double> dropBeats, pdbEnd;
            for (int i = 0; i < t.form.count; ++i) {
                const Section& sec = t.form.section[i];
                if (sec.type != SectionType::Build) continue;
                pdbEnd.push_back(static_cast<double>(sec.startBar + sec.bars) * kBeatsPerBar);
                dropBeats.push_back(static_cast<double>(sec.startBar + sec.bars) * kBeatsPerBar);
            }
            auto isAt = [](const std::vector<double>& v, double x) {
                for (double b : v) if (std::fabs(b - x) < 1e-6) return true;
                return false;
            };
            for (const SfxEvent& s : t.form.sfx) {
                const double end = s.beat + s.length;
                if (s.type == static_cast<int>(SfxType::Impact)) {
                    ++impacts;
                    // Either the downbeat of a drop that follows a buildup, or of one that follows a
                    // breakdown directly (the flat Progressive body).
                    bool ok = isAt(dropBeats, s.beat);
                    for (int i = 1; i < t.form.count && !ok; ++i)
                        ok = t.form.section[i].type == SectionType::Drop && t.form.section[i - 1].type == SectionType::Break
                          && std::fabs(static_cast<double>(t.form.section[i].startBar) * kBeatsPerBar - s.beat) < 1e-6;
                    if (!ok) ++misplaced;
                }
                if (s.type == static_cast<int>(SfxType::FormantShot)) { ++shots; if (!isAt(pdbEnd, s.beat + 1.0)) ++misplaced; }
                if (s.type == static_cast<int>(SfxType::Riser)) { ++risers; if (!isAt(dropBeats, end)) ++misplaced; }
                if (s.type == static_cast<int>(SfxType::Sweep)) ++sweeps;
            }
            // The outro's sweep ends exactly on the track boundary, where the key changes (PLAN 6.7).
            bool endSweep = false;
            for (const SfxEvent& s : t.form.sfx)
                if (s.type == static_cast<int>(SfxType::Sweep) && std::fabs(s.beat + s.length - static_cast<double>(t.bars) * kBeatsPerBar) < 1e-6) endSweep = true;
            if (!endSweep) ++misplaced;
        }
        check(impacts > 0 && shots > 0 && risers > 0 && sweeps > 0 && misplaced == 0,
              "effects on the boundaries of the form: riser into the drop, formant shot on the last beat of the PDB, impact on the drop, sweep into the key change",
              fmt("%d impacts, %d formant shots, %d risers, %d sweeps, %d misplaced", impacts, shots, risers, sweeps, misplaced));
    }
}

void testMaster()
{
    section("the finished track (Phase 4 milestone)");
    // A complete track of eight minutes at the defaults, through the whole master.
    auto e = std::make_unique<Engine>();
    e->prepare(48000.0, 512);
    e->params().parseText("compose.track_bars=288 compose.pad_amount=1 compose.sfx_amount=1");
    Composer c(8);
    const double target = e->params().get(e->params().base(Module::Master) + master::TargetLufs);
    const TempoMap tm = c.tempoMap(e->params(), 288);
    e->setTempoMap(tm);
    Conductor cond(*e, c);
    const uint64_t total = static_cast<uint64_t>(tm.secondsAt(288.0 * kBeatsPerBar) * 48000.0);
    std::vector<float> L(512), R(512);
    while (e->samplePosition() < total) {
        cond.pump(e->params(), 32.0);
        e->process(L.data(), R.data(), static_cast<int>(std::min<uint64_t>(512, total - e->samplePosition())));
    }
    const LoudnessReading rd = e->meter();
    check(std::fabs(rd.integrated - target) < 1.0 && rd.truePeak <= -0.9f && rd.seconds > 470.0f,
          "eight-minute track at the defaults: integrated loudness within 1 LU of the target, true peak at the -1 dBTP ceiling",
          fmt("%.1f s, %.2f LUFS (target %.1f), true peak %.2f dBTP, range %.1f LU", static_cast<double>(rd.seconds), static_cast<double>(rd.integrated), target,
              static_cast<double>(rd.truePeak), static_cast<double>(rd.range)));
}

} // namespace

int main()
{
    std::printf("phos_selftest (vector path %s)\n", kVecPathName);
    // PHOS_ONLY=testName[,testName...] runs only those tests (for work on one building block).
    const char* only = std::getenv("PHOS_ONLY");
    auto run = [&](const char* name, void (*fn)()) { if (only == nullptr || std::strstr(only, name) != nullptr) fn(); };
    run("testSampler", testSampler);
    run("testDiodeLadder", testDiodeLadder);
    run("testAcid", testAcid);
    run("testPoly", testPoly);
    run("testMelody", testMelody);
    run("testWaveTable", testWaveTable);
    run("testPads", testPads);
    run("testGateAndDuck", testGateAndDuck);
    run("testReverb", testReverb);
    run("testDynamics", testDynamics);
    run("testSfx", testSfx);
    run("testMaster", testMaster);
    run("testParams", testParams);
    run("testTempo", testTempo);
    run("testHalfband", testHalfband);
    run("testOscillator", testOscillator);
    run("testLadder", testLadder);
    run("testKick", testKick);
    run("testBass", testBass);
    run("testComposer", testComposer);
    run("testVariety", testVariety);
    run("testEngine", testEngine);
    run("testPhaseLock", testPhaseLock);
    run("testPercKit", testPercKit);
    run("testRhythm", testRhythm);
    run("testMidi", testMidi);
    run("testMidiKeys", testMidiKeys);
    run("testWav", testWav);
    run("testLoudness", testLoudness);
    return finish();
}
