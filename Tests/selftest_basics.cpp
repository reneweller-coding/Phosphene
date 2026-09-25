/**
 * @file selftest_basics.cpp
 * @brief The self test's building blocks: parameters, tempo, filters, MIDI and WAV files, loudness, kick, bass, the composer's plans and the engine.
 */
#include "SelfTestHelpers.h"

using namespace phos;

namespace phostest {

/** @brief Self test: parameters. */
void testParams()
{
    section("parameters");
    ParamStore p;
    const int expected = static_cast<int>(compose::Count) + static_cast<int>(kick::Count) + static_cast<int>(bass::Count)
                       + static_cast<int>(mix::Count) + static_cast<int>(master::Count) + kPercLanes * static_cast<int>(perc::Count)
                       + static_cast<int>(acid::Count) + kPolyInstances * static_cast<int>(poly::Count)
                       + static_cast<int>(sfx::Count) + static_cast<int>(fx::Count) + static_cast<int>(cue::Count)
                       // 19.09.2026, round "fx-psychedelia": the bed, the voices, the modulation effects
                       + static_cast<int>(texture::Count) + static_cast<int>(vocal::Count) + static_cast<int>(psyfx::Count);
    check(p.count() == expected, "every module table registered", fmt("%d parameters", p.count()));
    check(p.find("lead.detune") == p.base(PolyInstance::Lead) + poly::Detune && p.find("arp.detune") == p.base(PolyInstance::Arp) + poly::Detune
          && p.find("acid.cutoff") == p.base(Module::Acid) + acid::Cutoff && p.get(p.find("arp.amp_sustain")) == 0.0f,
          "named instances (lead, arp) with their own defaults");
    const int id = p.find("kick.pitch_start");
    check(id == p.base(Module::Kick) + kick::PitchStart, "key lookup gives base + index");
    // 23.09.2026: the compose table's rows against the enum that indexes them. A row appended behind the
    // enum's order is invisible while the defaults agree -- style_mix landed behind presence_match on
    // 22.09.2026 and the presence match silently read another knob (see Params.cpp).
    {
        struct KeyOf { int index; const char* key; };
        static const KeyOf kKeys[] = {
            { compose::Bpm, "bpm" }, { compose::Style, "style" }, { compose::MelodyModel, "melody_model" },
            { compose::BassRhythm, "bass_rhythm" }, { compose::BedDensity, "bed_density" }, { compose::StyleMix, "style_mix" },
            { compose::PresenceMatch, "presence_match" }, { compose::LeadDensity, "lead_density" }, { compose::PitchEntropy, "pitch_entropy" },
            { compose::CounterMode, "counter_mode" },
        };
        std::string off;
        for (const KeyOf& k : kKeys)
            if (std::string(p.desc(p.base(Module::Compose) + k.index).key) != k.key)
                off += fmt(" %s->%s", k.key, p.desc(p.base(Module::Compose) + k.index).key);
        check(off.empty(), "every compose enum entry indexes the row of its own name", off.empty() ? "" : off);
    }
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

/** @brief Self test: tempo map. */
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

/** @brief Self test: half-band filters. */
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

/** @brief Self test: ladder filter. */
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

/** @brief Self test: MIDI export. */
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
    // Bars 16 .. 28 (19.09.2026: the two-drop form's intro has no kick or bass before bar 17).
    c.composeBars(p, 16, 12, s.notes);
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

/** @brief Self test: WAV writer. */
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
        // 24 bits carry a triangular dither of one step since 23.09.2026 (WavWriter::setDither): half a step of
        // rounding plus at most one step of dither.
        check(ok && worst <= (bps == 4 ? 0.0 : 1.5 / 8388608.0 + 1e-12), bps == 4 ? "float file reads back exactly" : "24-bit file reads back within one and a half steps (rounding and dither)",
              fmt("worst %.3g", worst));
    }
    // What the dither is for: a signal below one step survives on average instead of rounding away, digital
    // silence stays exactly zero, and the same input writes the same bytes.
    {
        const int n = 48000;
        std::vector<float> quiet(static_cast<size_t>(n)), silent(static_cast<size_t>(n), 0.0f);
        for (int i = 0; i < n; ++i) quiet[static_cast<size_t>(i)] = static_cast<float>(0.4 / 8388608.0 * std::sin(2.0 * 3.141592653589793 * 997.0 * i / 48000.0));
        auto readBack = [&](bool dither, const std::vector<float>& x, std::vector<int32_t>& out) {
            WavWriter w;
            w.setDither(dither);
            w.open(path, 48000, 1, WavFormat::Pcm24);
            w.write(x.data(), nullptr, n);
            w.close();
            FILE* fp = std::fopen(path, "rb");
            std::vector<uint8_t> b(80 + static_cast<size_t>(n) * 3);
            const size_t got = fp ? std::fread(b.data(), 1, b.size(), fp) : 0;
            if (fp) std::fclose(fp);
            out.assign(static_cast<size_t>(n), 0);
            for (int i = 0; i < n && got == b.size(); ++i) {
                const uint8_t* q = b.data() + 80 + i * 3;
                out[static_cast<size_t>(i)] = ((q[0] << 8) | (q[1] << 16) | (q[2] << 24)) >> 8;
            }
        };
        std::vector<int32_t> plain, dithered, again, quietZero;
        readBack(false, quiet, plain);
        readBack(true, quiet, dithered);
        readBack(true, quiet, again);
        readBack(true, silent, quietZero);
        auto correlation = [&](const std::vector<int32_t>& y) {
            double s = 0.0, e = 0.0;
            for (int i = 0; i < n; ++i) { s += y[static_cast<size_t>(i)] * static_cast<double>(quiet[static_cast<size_t>(i)]); e += static_cast<double>(quiet[static_cast<size_t>(i)]) * quiet[static_cast<size_t>(i)]; }
            return s / std::max(e, 1e-300) / 8388608.0;   // 1 = the signal comes back at its own level
        };
        int nonZero = 0;
        for (int32_t v : quietZero) nonZero += v != 0 ? 1 : 0;
        const double cPlain = correlation(plain), cDither = correlation(dithered);
        check(cPlain == 0.0 && cDither > 0.8 && cDither < 1.2 && dithered == again && nonZero == 0,
              "the 24-bit dither keeps a signal below one step (0.4 of a step at 997 Hz), is reproducible and leaves silence silent",
              fmt("gain of the sub-step sine read back: %.3f undithered, %.3f dithered; %d non-zero samples of silence", cPlain, cDither, nonZero));
    }
    // Markers and tags (23.09.2026, DJ export): a `cue ` chunk with a label per marker and a LIST/INFO, after the
    // audio, with every chunk size and the RIFF size consistent -- walked chunk by chunk as a reader would. Mono
    // 24-bit with an odd frame count, so the data chunk needs its pad byte.
    {
        WavWriter w;
        std::vector<float> x(1001, 0.25f);
        w.addCue(0, "Intro");
        w.addCue(480, "Drop 1");
        w.setInfo("INAM", "Track 1");
        w.setInfo("IGNR", "Psytrance");
        const bool written = w.open(path, 48000, 1, WavFormat::Pcm24) && w.write(x.data(), nullptr, 1001) && w.close();
        FILE* fp = std::fopen(path, "rb");
        std::vector<uint8_t> b;
        uint8_t buf[4096];
        size_t n;
        while (fp && (n = std::fread(buf, 1, sizeof(buf), fp)) > 0) b.insert(b.end(), buf, buf + n);
        if (fp) std::fclose(fp);
        auto u32 = [&](size_t at) { uint32_t v = 0; std::memcpy(&v, b.data() + at, 4); return v; };
        bool sizes = b.size() >= 12 && u32(4) + 8 == b.size();
        int cues = 0;
        uint32_t cue2 = 0;
        std::string labels, info;
        for (size_t at = 12; sizes && at + 8 <= b.size();) {
            const std::string id(reinterpret_cast<const char*>(b.data() + at), 4);
            const uint32_t sz = u32(at + 4);
            if (at + 8 + sz > b.size()) { sizes = false; break; }
            if (id == "cue ") { cues = static_cast<int>(u32(at + 8)); if (cues >= 2) cue2 = u32(at + 12 + 24 + 20); }
            if (id == "LIST") {
                const std::string kind(reinterpret_cast<const char*>(b.data() + at + 8), 4);
                for (size_t k = at + 12; k + 8 <= at + 8 + sz;) {
                    const uint32_t ssz = u32(k + 4);
                    const std::string sub(reinterpret_cast<const char*>(b.data() + k), 4);
                    if (kind == "adtl" && sub == "labl") labels += std::string(reinterpret_cast<const char*>(b.data() + k + 12)) + ";";
                    if (kind == "INFO") info += sub + "=" + std::string(reinterpret_cast<const char*>(b.data() + k + 8)) + ";";
                    k += 8 + ssz + (ssz % 2);
                }
            }
            at += 8 + sz + (sz % 2);
        }
        check(written && sizes && cues == 2 && cue2 == 480 && labels == "Intro;Drop 1;" && info == "INAM=Track 1;IGNR=Psytrance;",
              "markers and tags: a cue chunk with its labels and a LIST/INFO after the audio, every size consistent",
              fmt("%d cues (second at %u), labels '%s', info '%s', %zu bytes", cues, cue2, labels.c_str(), info.c_str(), b.size()));
    }
    std::remove(path);
}

/** @brief Self test: loudness meter. */
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

/** @brief Self test: polyBLEP oscillator. */
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

/** @brief Self test: kick. */
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
    // output, saturation included, is Tail Limit under its peak -- for tanh and hard clip, soft and hot.
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
    // The limit is the knob's default: -15 dB since 19.09.2026, -24 before (docs/rounds/2026-09.md, "Kick-Körper").
    const double limitDb = ParamStore().get(ParamStore().base(Module::Kick) + kick::TailLimit);
    check(worstTail < limitDb + 1.0 && loosest > limitDb, "tail limit holds after saturation (unconstrained these kicks were not)",
          tails + fmt("dB with/without, limit %.0f dB", limitDb));

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

/** @brief Self test: bass. */
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
                const double d = wrapDeg(phaseAgainstH<3>(y.data() + w0, 2 * per + 8, [&](size_t i) { return phase + f0 * (static_cast<double>(w0 + i) + late) / sr; }));
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
        // Without the bite (19.09.2026): the bite is a driven copy of the oscillator, and the pulse part
        // of the default wave has its edge exactly at the fundamental's zero crossing -- the bite's
        // saturator lifts that edge into the attack it exists to give (a band-limited transient under
        // 2 kHz, not a step). What this check is about is where saw and sub start.
        const std::vector<float> y = bassNote("bass.bite=0", pitch, 4000, 2400, 0.0, fundamentalPhase);
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

/** @brief Self test: composer. */
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

/**
 * @brief compose.bass_model: the learned fourth role inside the composer.
 *
 * What this section has to establish is not that a network runs -- testModelFile does that -- but
 * that switching the knob changes *only* what it is allowed to change. The bass is the one part with
 * hard contracts hanging off it: the engine derives the kick's tail limit and the kick phase lock
 * from the first bass slot of the bar's pattern (Engine::firstSlotSeconds, Patterns.h), the gate is
 * computed from the lowest note's release, and the form's rule that two consecutive eight-bar groups
 * of a core never hold the same bass is measured elsewhere in this file. A learned bass may move
 * pitches; it may not move a single onset, length or velocity, and it may not go below the note the
 * gate limit was computed for.
 */
void testBassModel()
{
    section("compose.bass_model: the learned bass as the fourth role");
    // 24.09.2026, the user: "wenn der Bass einsetzt wird es absolut schief". compose.bass_register was added to the
    // root, so at 4 an F# set's bass played A#; every check here measured the bass against that shifted root and saw
    // nothing wrong. The register moves the octave window and never the pitch class (Harmony.h, bassRootNote).
    {
        bool keyed = true, inWindow = true, octavesAsBefore = true;
        for (int key = 0; key < 12; ++key)
            for (int reg = -12; reg <= 12; ++reg) {
                const int n = bassRootNote(key, reg);
                keyed = keyed && ((n - key) % 12 + 12) % 12 == 0;
                inWindow = inWindow && n >= 28 + reg && n < 40 + reg;
                if (reg % 12 == 0) octavesAsBefore = octavesAsBefore && n == 28 + ((key - 4) % 12 + 12) % 12 + reg;
            }
        check(keyed && inWindow && octavesAsBefore, "the bass register moves the bass's octave window, never its note: at every register the root is the key's");
    }
    ParamStore def;
    const int cb = def.base(Module::Compose);
    check(def.getInt(cb + compose::BassModel) == 0,
          "the bass model defaults to Pattern until a listening comparison exists",
          fmt("default %s", kBassModelNames[def.getInt(cb + compose::BassModel)]));

    std::string note;
    const bool haveModel = sharedBassModel(&note) != nullptr;
    check(haveModel, "the shared bass model loads (Core/data/bass.phosmdl)", note);

    const char* kBase = "compose.level_match=Off master.auto_gain=Off compose.track_bars=64";
    auto composeWith = [&](const char* extra, uint64_t seed, int bars, std::vector<NoteEvent>& out) {
        ParamStore q;
        q.parseText(kBase);
        q.parseText(extra);
        Composer c(seed);
        c.composeBars(q, 0, bars, out);
    };

    // 1. The rhythm is untouched. Every bass onset, its length and its velocity must be bit-identical
    //    to what the pattern generator places; only the pitch may differ. This is what keeps
    //    Engine::firstSlotSeconds, the kick tail limit and the phase lock valid without a line of
    //    change in the engine.
    {
        std::vector<NoteEvent> pat, neu;
        composeWith("", 4242, 64, pat);
        composeWith("compose.bass_model=Neural", 4242, 64, neu);
        std::vector<const NoteEvent*> pb, nb;
        for (const NoteEvent& e : pat) if (e.part == Part::Bass) pb.push_back(&e);
        for (const NoteEvent& e : neu) if (e.part == Part::Bass) nb.push_back(&e);
        bool sameRhythm = pb.size() == nb.size() && !pb.empty();
        int pitchChanges = 0;
        for (size_t i = 0; sameRhythm && i < pb.size(); ++i) {
            sameRhythm = pb[i]->beat == nb[i]->beat && pb[i]->length == nb[i]->length
                         && pb[i]->velocity == nb[i]->velocity;
            if (pb[i]->pitch != nb[i]->pitch) ++pitchChanges;
        }
        check(sameRhythm && pitchChanges > 0,
              "the learned bass moves pitches and nothing else: every onset, length and velocity is identical",
              fmt("%zu bass notes against %zu, %d pitches changed", nb.size(), pb.size(), pitchChanges));
        // The kick is not the bass's business either.
        int kickPat = 0, kickNeu = 0;
        for (const NoteEvent& e : pat) if (e.part == Part::Kick) ++kickPat;
        for (const NoteEvent& e : neu) if (e.part == Part::Kick) ++kickNeu;
        check(kickPat == kickNeu && kickPat > 0, "the kick is unchanged by the bass model",
              fmt("%d kicks against %d", kickNeu, kickPat));
    }

    // 2. Determinism from the seed alone, and a bar composed alone equals the same bar in sequence --
    //    the phrase is a function of the track's seed and of nothing that was composed before it.
    {
        std::vector<NoteEvent> a, b;
        composeWith("compose.bass_model=Neural", 77, 32, a);
        composeWith("compose.bass_model=Neural", 77, 32, b);
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i)
            same = a[i].beat == b[i].beat && a[i].pitch == b[i].pitch && a[i].part == b[i].part;
        ParamStore q;
        q.parseText(kBase);
        q.parseText("compose.bass_model=Neural");
        Composer fresh(77);
        std::vector<NoteEvent> one;
        fresh.composeBars(q, 21, 1, one);
        std::vector<NoteEvent> slice;
        for (const NoteEvent& x : a) if (x.beat >= 84.0 && x.beat < 88.0) slice.push_back(x);
        bool sliceSame = slice.size() == one.size();
        for (size_t i = 0; sliceSame && i < slice.size(); ++i)
            sliceSame = slice[i].beat == one[i].beat && slice[i].pitch == one[i].pitch;
        check(same && sliceSame && !one.empty(),
              "the learned bass is deterministic from the seed, and one bar alone equals that bar in sequence",
              fmt("%zu notes twice, bar 21 alone %zu notes", a.size(), one.size()));
    }

    // 3. The constraint masks hold: every pitch is a scale tone, never below the note the gate limit
    //    was computed for (root + the seventh degree - an octave), never above an octave over the root.
    {
        int notes = 0, outside = 0, tooLow = 0, tooHigh = 0, lowestSeen = 127, highestSeen = 0;
        std::set<int> distinct;
        for (uint64_t seed : { 1ull, 19ull, 2026ull, 90210ull }) {
            ParamStore q;
            q.parseText(kBase);
            q.parseText("compose.bass_model=Neural");
            Composer c(seed);
            for (int ti = 0; ti < 3; ++ti) {
                const TrackPlan t = c.track(q, ti);
                if (!t.bassNeural) continue;
                const int root = bassRootNote(t.key, q.getInt(cb + compose::BassRegister));
                const int lowest = root + scaleDegree(t.scale, 6) - 12;
                // From the track's hand-over: before it the previous track's bass still plays (the DJ overlap,
                // 19.09.2026), in the previous track's key.
                std::vector<NoteEvent> e;
                c.composeBars(q, handoverBar(t), std::min(48, t.bars - t.form.handover), e);
                for (const NoteEvent& n : e) {
                    if (n.part != Part::Bass) continue;
                    ++notes;
                    const int rel = static_cast<int>(n.pitch) - root;
                    distinct.insert(rel);
                    lowestSeen = std::min(lowestSeen, static_cast<int>(n.pitch));
                    highestSeen = std::max(highestSeen, static_cast<int>(n.pitch));
                    if (!inScale(t.scale, rel)) ++outside;
                    if (n.pitch < lowest) ++tooLow;
                    if (rel > 12) ++tooHigh;
                }
            }
        }
        check(notes > 0 && outside == 0 && tooLow == 0 && tooHigh == 0,
              "every learned bass note is a scale tone inside the register the gate limit is computed for",
              fmt("%d notes, %d outside the scale, %d below the lowest note, %d above an octave "
                  "(MIDI %d..%d, %zu distinct intervals)",
                  notes, outside, tooLow, tooHigh, lowestSeen, highestSeen, distinct.size()));
        check(distinct.size() >= 3,
              "the learned bass is a line, not a root held down: it uses several intervals",
              fmt("%zu distinct intervals from the root", distinct.size()));
    }

    // 4. Against the pattern generator, measured the way Tools/train/bass_stats.py measures the corpus:
    //    the pattern families play the root in 97 % of their notes, and the whole point of the fourth
    //    role is that a real psytrance bass plays it in 59 %.
    {
        auto rootShare = [&](const char* extra, double& entropyBits) {
            std::map<int, int> hist;
            int total = 0;
            for (uint64_t seed : { 5ull, 55ull, 555ull, 5555ull }) {
                ParamStore q;
                q.parseText(kBase);
                q.parseText(extra);
                Composer c(seed);
                for (int ti = 0; ti < 3; ++ti) {
                    const TrackPlan t = c.track(q, ti);
                    const int root = bassRootNote(t.key, q.getInt(cb + compose::BassRegister));
                    std::vector<NoteEvent> e;
                    c.composeBars(q, handoverBar(t), std::min(48, t.bars - t.form.handover), e);   // this track's bass only (the DJ overlap)
                    for (const NoteEvent& n : e)
                        if (n.part == Part::Bass) { ++hist[static_cast<int>(n.pitch) - root]; ++total; }
                }
            }
            entropyBits = 0.0;
            for (const auto& kv : hist) {
                const double p = static_cast<double>(kv.second) / std::max(1, total);
                if (p > 0.0) entropyBits -= p * std::log2(p);
            }
            return total > 0 ? static_cast<double>(hist[0]) / total : 1.0;
        };
        double hPat = 0.0, hNeu = 0.0;
        const double rPat = rootShare("", hPat);
        const double rNeu = rootShare("compose.bass_model=Neural", hNeu);
        // The corpus measurement is 0.589 root share and 2.755 bits; the pattern generator sits at
        // 0.972 and 0.237, and over the forty full tracks of the plan's A/B the learned bass sits at
        // 0.704 and 1.824. Twelve short tracks are a small sample of two phrases each, so the bounds
        // here are wide on purpose: what they forbid is a bass that is still a held-down root, and a
        // bass that has wandered out of the register altogether.
        check(rNeu < 0.85 && rNeu > 0.35 && hNeu > 0.8 && hNeu > 2.0 * hPat,
              "the learned bass leaves the root the way the corpus does, the pattern families do not",
              fmt("root share %.3f against %.3f (corpus 0.589), entropy %.3f against %.3f bits (corpus 2.755)",
                  rNeu, rPat, hNeu, hPat));
    }

    // 5. The documented limit: the alphabet is intervals to the *tonic*, so a learned line cannot be
    //    re-transposed by a chord degree without leaving the scale, and compose.bass_follows_chords
    //    therefore does nothing to it. Measured here so the limit is a fact and not a sentence.
    {
        std::vector<NoteEvent> off, on, patOff, patOn;
        // Pads on throughout (25.09.2026): the bass follows only where the track's pad sounds -- where there is
        // no chord to follow it is the pedal (Composer.cpp, followsHere).
        composeWith("compose.pad_amount=1 compose.bass_model=Neural", 31337, 128, off);
        composeWith("compose.pad_amount=1 compose.bass_model=Neural compose.bass_follows_chords=1", 31337, 128, on);
        composeWith("compose.pad_amount=1", 31337, 128, patOff);
        composeWith("compose.pad_amount=1 compose.bass_follows_chords=1", 31337, 128, patOn);
        auto bassOnly = [](const std::vector<NoteEvent>& v) {
            std::vector<const NoteEvent*> out;
            for (const NoteEvent& e : v) if (e.part == Part::Bass) out.push_back(&e);
            return out;
        };
        const auto a = bassOnly(off), b = bassOnly(on), c = bassOnly(patOff), d = bassOnly(patOn);
        // The group figure of Form.cpp is applied in both modes and does count from the chord's
        // degree, so it is the one thing a learned bass still lets the chord move. It sits on beat 1
        // of a bar; anything that moves anywhere else would mean the learned line itself was
        // transposed, which is what this check exists to forbid.
        int neuralMoved = 0, neuralMovedOffGroupBeat = 0, patternMoved = 0;
        for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
            if (a[i]->pitch != b[i]->pitch) {
                ++neuralMoved;
                const double inBar = a[i]->beat - 4.0 * std::floor(a[i]->beat / 4.0);
                if (!(inBar >= 1.0 && inBar < 2.0)) ++neuralMovedOffGroupBeat;
            }
        for (size_t i = 0; i < std::min(c.size(), d.size()); ++i) if (c[i]->pitch != d[i]->pitch) ++patternMoved;
        check(neuralMovedOffGroupBeat == 0 && patternMoved > 20 * neuralMoved,
              "compose.bass_follows_chords moves the pattern bass; of the learned one it moves only the group figure",
              fmt("%d of %zu learned notes moved, all on beat 1 (the group figure); "
                  "%d of %zu pattern notes moved", neuralMoved, a.size(), patternMoved, c.size()));
    }
}

/**
 * @brief Variety over a night, part `.plans`: sixty track plans and the zero-variation plans.
 *
 * testVariety was split on 19.09.2026 (round "test-split") into `.plans` (this), `.levelMatch`
 * (the two 512-bar renders, which one check compares and so stay together) and `.recipes`.
 */
void testVarietyPlans()
{
    section("variety over a night: the plans");
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
    check(keys >= 6 && pats >= 4, "keys and bass patterns spread over the night",
          fmt("%d keys, %d primary patterns, %d kick engine switches in %d tracks", keys, pats, engineSwitches, tracks));
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
}

/**
 * @brief Variety over a night, part `.levelMatch`: the level match on the real render.
 *
 * Not split further: the check holds the spread with the match against the spread without it, so it
 * needs both renders, and each is one stream over four tracks (the tracks hand over to each other,
 * so a single call's stream cannot be cut into pieces without breaking that hand-over). The two calls
 * themselves are independent, though (round "test-speed-rest", 20.09.2026): trackLoudness(true, ...)
 * and trackLoudness(false, ...) each open their own Engine, Composer and Conductor and touch nothing
 * outside them, so they render side by side instead of one after the other (see testPhaseLockLock's
 * comment for why this reuses phos::probe::runAll rather than a new pool for two tasks).
 */
void testVarietyLevelMatch()
{
    section("variety over a night: the level match");
    // Level match: tracks with very different sounds reach the same loudness. Measured on the real
    // render, bars 4..28 of each 32-bar track, with every variation at full strength.
    //
    // **The loudness side of the energy arc is taken out of every reading.** The composer adds
    // `energyGainDb(section energy)` -- at most +-2 dB, the loudness half of Farbood's tension model --
    // to the track gain of every section, and that is the form speaking, not the level match. The
    // window below is each track's first core, and a core can be a Groove (energy 0.61) or a Drop
    // (0.87); those two are 1.30 dB apart *by design*. Before 16.09.2026 this check compared them
    // anyway, so most of what it called level-match error was the form: measured over three seeds and
    // six tracks each, the raw spread was 1.92 / 0.49 / 1.59 LU and the same readings with the arc
    // taken out are 0.62 / 0.49 / 0.29 LU. On this check's own four tracks the arc accounts for
    // 1.30 of the 1.56 LU. That is why the bound stood at 1.8 and had to be raised from 1.5 when the
    // kit got heavier: it was not measuring the probe.
    //
    // The foundation probe of `Composer.cpp` was not the culprit, and probing the real bars instead of
    // the synthetic loop made it worse, not better (docs/rounds/2026-09.md, 16.09.2026 Nachtrag; the bench is
    // `PHOS_ONLY=testProbeAudit`). What is left after the arc *is* the probe, and it does grow with the
    // kit -- 0.15 / 0.20 / 0.26 LU at mix.perc_level +1 / +2 / +3 dB -- at 0.056 LU per dB. The bound
    // is 0.8 LU: three times what this check reads at the default kit level, and above the 0.62 LU
    // worst case over the three seeds.
    {
        auto trackLoudness = [&](bool match, double& spread) {
            auto e = std::make_unique<Engine>();
            e->prepare(48000.0, 512);
            e->params().parseText(fmt("compose.track_bars=128 compose.sound_variation=1 compose.track_variation=1 master.auto_gain=Off master.limiter=Off master.clipper=Off master.clip=Off master.comp_ratio=1 "
                                      "compose.acid_amount=0 compose.lead_amount=0 compose.arp_amount=0 compose.pad_amount=0 compose.sfx_amount=0 "
                                      "mix.acid_mute=1 mix.lead_mute=1 mix.arp_mute=1 mix.pad_mute=1 mix.counter_mute=1 mix.stab_mute=1 mix.drone_mute=1 mix.sfx_mute=1 compose.level_match=%s", match ? "On" : "Off").c_str());
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
                float energy = 0.5f;
                for (int i = 0; i < tp.form.count; ++i)
                    if (tp.form.section[i].type == SectionType::Groove || tp.form.section[i].type == SectionType::Drop) {
                        coreBar = tp.form.section[i].startBar;
                        coreBars = tp.form.section[i].bars;
                        energy = 0.5f * (tp.form.section[i].energy + tp.form.section[i].energyTo);
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
                // The section's own gain, written out here rather than taken from the composer, so that
                // a change to either side shows up as a disagreement instead of cancelling out.
                const double arc = std::clamp((static_cast<double>(energy) - 0.7) * 5.0, -2.0, 2.0);
                const double lufs = m.read().integrated - arc;
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
        phos::probe::warmSharedData();
        phos::probe::runAll({
            std::function<void()>([&] { trackLoudness(true, withMatch); }),
            std::function<void()>([&] { trackLoudness(false, without); }),
        });
        check(withMatch < 0.8 && without > withMatch + 1.0, "level match keeps the tracks within 0.8 LU of each other once the form's own energy gain is taken out (without it they spread wider)",
              fmt("spread %.2f LU with, %.2f LU without", withMatch, without));
    }
}

/** @brief Variety over a night, part `.recipes`: every track plays a recipe, and no two the same. */
void testVarietyRecipes()
{
    section("variety over a night: the engine plays the recipes");
    // The engine plays the recipes: one in track 1 too since 21.09.2026, another in track 2.
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
        const int cb = e->params().base(Module::Compose);
        // 21.09.2026, at the user's decision: the first track draws a recipe like every other one.
        // Until then this check read "engine plays the knobs in track 1", and that was the contract:
        // the whole recipe machinery -- kick, bass, acid voicing and the six voices -- began at track
        // 2. A track is 256 bars, so every render shorter than about seven minutes was the knobs and
        // nothing else, whatever the seed, which is what "alle Lieder hoeren sich gleich an" was made
        // of. What the knobs still decide alone are the musical settings somebody typed in: key,
        // mode, tempo and length.
        float k1 = 0.0f, b1 = 0.0f;
        bool firstHasRecipe = false, secondDiffers = false, firstKeepsTheKnobs = false;
        const uint64_t endSamples = static_cast<uint64_t>(tm.secondsAt(64.0 * kBeatsPerBar) * 48000.0);
        while (e->samplePosition() < endSamples) {
            cond.pump(e->params(), 32.0);
            e->process(L.data(), R.data(), 4096);
            const double bar = e->beatPosition() / kBeatsPerBar;
            if (bar > 8.0 && bar < 9.0) {
                k1 = e->effective(kb + kick::PitchDecay);
                b1 = e->effective(bbase + bass::Resonance);
                firstHasRecipe = k1 != e->params().get(kb + kick::PitchDecay)
                              || b1 != e->params().get(bbase + bass::Resonance);
                firstKeepsTheKnobs = e->effective(cb + compose::Key) == e->params().get(cb + compose::Key)
                                  && e->effective(cb + compose::Scale) == e->params().get(cb + compose::Scale);
            }
            if (bar > 40.0 && bar < 41.0) {
                secondDiffers = e->effective(kb + kick::PitchDecay) != k1
                             || e->effective(bbase + bass::Resonance) != b1;
            }
        }
        check(firstHasRecipe && secondDiffers && firstKeepsTheKnobs,
              "the engine plays a recipe from the first track on, and the second track's is another one"
              " -- while key and mode stay the knobs",
              fmt("track 1 %s the knobs' sound, track 2 %s track 1's, key and mode %s",
                  firstHasRecipe ? "leaves" : "IS", secondDiffers ? "differs from" : "REPEATS",
                  firstKeepsTheKnobs ? "kept" : "MOVED"));
    }
}

/** @brief Self test: engine. */
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
    // The bed and the voices too (19.09.2026): the first core is now the groove, where a track may lay eight
    // bars of jaw harp -- a sound this check has never been about.
    eb->params().parseText("mix.kick_mute=1 mix.perc_mute=1 mix.acid_mute=1 mix.lead_mute=1 mix.arp_mute=1 mix.pad_mute=1 mix.counter_mute=1 mix.stab_mute=1 mix.drone_mute=1 mix.sfx_mute=1 "
                           "mix.texture_mute=1 mix.vocal_mute=1");
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
 * @brief The default kick against the kicks of the reference recordings (18.09.2026, "mix-foundation").
 *
 * The reference numbers are `Tools/ref_kick.py` on the 40 recordings of the collection: in 24 of them
 * the first 90 s hold a stretch of eight or more beats where kick and bass play nearly alone (power
 * above 300 Hz at least 8 dB under the power at 30 .. 150 Hz), and the kicks there were measured one
 * by one over the window from the onset to the first bass slot, a quarter beat later. Medians over
 * the recordings (quartiles in brackets): power under 60 Hz against 60 .. 120 Hz -4.7 dB (-9 .. 0),
 * the click band 2 .. 5 kHz against 40 .. 120 Hz -27.7 dB (-31 .. -23), crest 7.1 dB (6 .. 8).
 * The Phosphene kick measured -7.0, -38.7 and 6.0 by the same tool before 19.09.2026.
 *
 * The same quantities are computed here from the kick's own output, independently of the tool but
 * with its window: flat, with a 3 ms half-cosine fade at the end (a Hann window would weigh the click
 * -- the first milliseconds -- with nearly zero), zero-padded, a plain DFT. The bounds: at least the
 * reference median of sub (the user heard "hardly any sub"), the click inside the references'
 * quartiles, the crest at least their lower quartile.
 */
void testKickReference()
{
    section("the kick against the reference kicks");
    const double sr = 48000.0;
    const double slot = 0.25 * 60.0 / 145.0;
    ParamStore p;
    std::vector<float> kv = moduleValues(p, Module::Kick);
    Kick::constrain(kv.data(), slot, 6);
    Kick k;
    k.prepare(sr);
    k.update(kv.data(), 6);
    k.trigger(1.0f);
    const size_t n = static_cast<size_t>(slot * sr);
    std::vector<float> y(n);
    k.process(y.data(), static_cast<int>(n));
    const size_t fade = static_cast<size_t>(0.003 * sr);
    std::vector<double> w(n);
    for (size_t i = 0; i < n; ++i) {
        const double g = i + fade >= n ? 0.5 + 0.5 * std::cos(3.141592653589793 * static_cast<double>(i + fade - n) / fade) : 1.0;
        w[i] = g * y[i];
    }
    const size_t N = 16384;
    auto band = [&](double lo, double hi) {
        double pw = 0.0;
        for (size_t kb = 1; kb < N / 2; ++kb) {
            const double f = static_cast<double>(kb) * sr / N;
            if (f < lo || f >= hi) continue;
            double re = 0.0, im = 0.0;
            const double dw = -2.0 * 3.141592653589793 * static_cast<double>(kb) / N;
            for (size_t i = 0; i < n; ++i) { re += w[i] * std::cos(dw * i); im += w[i] * std::sin(dw * i); }
            pw += re * re + im * im;
        }
        return pw;
    };
    const double low = band(60.0, 120.0);
    const double subLow = 10.0 * std::log10(band(20.0, 60.0) / low);
    const double click = 10.0 * std::log10(band(2000.0, 5000.0) / band(40.0, 120.0));
    double peak = 0.0, ms = 0.0;
    for (size_t i = 0; i < n; ++i) { peak = std::max(peak, std::fabs(static_cast<double>(y[i]))); ms += static_cast<double>(y[i]) * y[i]; }
    const double crest = 20.0 * std::log10(peak / std::sqrt(ms / n));
    check(subLow >= -4.7 && click >= -31.0 && click <= -23.0 && crest >= 6.0,
          "the default kick has the reference kicks' sub, click and crest (Tools/ref_kick.py, 24 recordings)",
          fmt("sub under 60 Hz against 60-120 Hz %+.1f dB (references: median -4.7, at least that), click 2-5 kHz against 40-120 Hz %+.1f dB "
              "(quartiles -31 .. -23), crest %.1f dB (lower quartile 6)", subLow, click, crest));
}

// ---------------------------------------------------------------------------------------------
// Round "lowend-acid" (19.09.2026): bass bite, kick body, per-track variety. docs/rounds/2026-09.md, block
// "Tiefe und Acid: Bass mit Biss, Kick-Körper, Klangvielfalt je Track".
// ---------------------------------------------------------------------------------------------

/**
 * @brief The bass against the reference basses: the bite layer, the octave, the clean fundamental.
 *
 * Reference numbers: `Tools/ref_bass.py` on the 24 of 40 recordings whose first 90 s hold eight or more
 * beats of kick and bass nearly alone, measured over the second and third sixteenth of every beat (the
 * first still carries the kick's tail), each band against the window's own 20 .. 120 Hz. Medians and
 * quartiles: under 60 Hz -7.4 (-10.7 .. -3.9), 60 .. 120 Hz -0.9 (-2.3 .. -0.4), 300 Hz .. 2 kHz
 * -9.8 (-11.7 .. -8.2) dB. The bass before 19.09.2026 read -0.4, -10.6 and -21.6 there: a sine sub with
 * the mid band 21 dB under it. The bounds are the reference quartiles, on the voice alone.
 */
void testBassBite()
{
    section("the bass against the reference basses: bite, octave, clean fundamental");
    const double sr = 48000.0, beat = 60.0 / 145.0 * sr, slot = beat / 4.0;
    auto lateBands = [&](const std::vector<float>& y, double& sub, double& oct, double& bite) {
        double p20 = 0.0, pSub = 0.0, pOct = 0.0, pBite = 0.0;
        for (int k = 4; k < 60; ++k) {   // beats 4..59, slots 2 and 3 of each
            const size_t a = static_cast<size_t>(k * beat + 2.0 * slot), b = static_cast<size_t>((k + 1) * beat);
            p20 += lowendBandPower(y, a, b, 20.0, 120.0, sr);
            pSub += lowendBandPower(y, a, b, 20.0, 60.0, sr);
            pOct += lowendBandPower(y, a, b, 60.0, 120.0, sr);
            pBite += lowendBandPower(y, a, b, 300.0, 2000.0, sr);
        }
        sub = powDb(pSub / p20);
        oct = powDb(pOct / p20);
        bite = powDb(pBite / p20);
    };
    ParamStore p;
    std::vector<float> bv = moduleValues(p, Module::Bass);
    bv[bass::DuckDepth] = 0.0f;   // the voice's own spectrum; the duck is a level, not a timbre
    double sub = 0.0, oct = 0.0, bite = 0.0;
    lateBands(lowendRollingBass(bv, 64), sub, oct, bite);
    check(sub >= -10.7 && sub <= -3.9 && oct >= -2.3 && oct <= -0.4 && bite >= -11.7 && bite <= -8.2,
          "the default bass sits inside the reference basses' quartiles: sub, octave and bite band (Tools/ref_bass.py, 24 recordings)",
          fmt("under 60 Hz %.1f (-10.7..-3.9), 60-120 Hz %.1f (-2.3..-0.4), 300 Hz-2 kHz %.1f (-11.7..-8.2) dB against 20-120 Hz", sub, oct, bite));

    // The bite is what carries the mid band: without it the band falls back towards the old bass.
    {
        std::vector<float> nb = bv;
        nb[bass::Bite] = 0.0f;
        double s2 = 0.0, o2 = 0.0, b2 = 0.0;
        lateBands(lowendRollingBass(nb, 64), s2, o2, b2);
        check(bite - b2 >= 3.0, "the bite layer carries more than half of the 300 Hz .. 2 kHz band", fmt("%.1f dB with it, %.1f without", bite, b2));
    }

    // The fundamental is still the one sine: with sub and octave off, what the filtered paths leave at
    // f0 is the leak of the Split high pass. A fourth-order Butterworth squared at 2 f0 passes f0 at
    // (1/2)^8 / (1 + (1/2)^8) = -48.2 dB; the old second-order pair passed it at -24.6 dB. The saw
    // path's own fundamental is about 13 dB above the default sub (a full-scale saw's 2/pi against the
    // sub's 0.21), which puts the expected leak near -35 dB against the sub. The bound is -30 dB: a leak
    // there turns the fundamental's phase by at most asin(10^(-30/20)) = 1.8 degrees. Measured on a held
    // note over whole periods (40 .. 140 ms).
    {
        const double f0 = midiToHz(30);
        std::vector<float> leakV = bv, subV = bv;
        leakV[bass::Sub] = 0.0f;
        leakV[bass::SubOctave] = 0.0f;
        leakV[bass::AmpSustain] = 1.0f;
        subV[bass::AmpSustain] = 1.0f;
        subV[bass::Bite] = 0.0f;
        subV[bass::SubOctave] = 0.0f;
        subV[bass::Cutoff] = 20.0f;
        subV[bass::EnvAmount] = 0.0f;
        subV[bass::KeyTrack] = 0.0f;
        auto note = [&](const std::vector<float>& v) {
            Bass b;
            b.prepare(sr);
            b.update(v.data());
            b.noteOn(30, 1.0f, 96000, 0.0, 0.0);
            std::vector<float> y(12000);
            b.process(y.data(), static_cast<int>(y.size()));
            return y;
        };
        const std::vector<float> yl = note(leakV), ys = note(subV);
        const size_t per = static_cast<size_t>(std::lround(sr / f0 * 4.0));   // four periods, whole to 0.1 %
        double worst = -1e9;
        for (size_t w0 = 1920; w0 + per <= 6720; w0 += per / 4) {
            const double leak = toneAmplitude(yl.data() + w0, per, 4.0), s = toneAmplitude(ys.data() + w0, per, 4.0);
            worst = std::max(worst, 20.0 * std::log10(leak / s));
        }
        check(worst < -30.0, "the filtered paths leave the fundamental alone: their leak at f0 is under -30 dB against the sub (at most 1.8 degrees of phase)",
              fmt("worst %.1f dB over 40..140 ms", worst));
    }

    // The bite has its own envelope: the top of its band (700 Hz .. 3 kHz) is brightest at a note's
    // start. Read over whole periods of the fundamental -- the first period of each note against the
    // third -- because a saw's upper harmonics bunch at its reset once per period, and a window of any
    // other length measures where the resets fall.
    // The saw path's own filter envelope is closed for this reading (Env Amount 0), so that what falls is
    // the bite's envelope and not the ladder's pluck under it.
    {
        std::vector<float> pv = bv;
        pv[bass::EnvAmount] = 0.0f;
        const std::vector<float> y = lowendRollingBass(pv, 64);
        const size_t per = static_cast<size_t>(std::lround(sr / midiToHz(30)));
        double early = 0.0, late = 0.0;
        for (int k = 4; k < 60; ++k)
            for (int s = 1; s <= 3; ++s) {
                const size_t a = static_cast<size_t>(k * beat + s * slot);
                early += lowendBandPower(y, a, a + per, 700.0, 3000.0, sr);
                late += lowendBandPower(y, a + 2 * per, a + 3 * per, 700.0, 3000.0, sr);
            }
        check(powDb(early / late) >= 6.0, "the bite is a pluck: the top of its band falls 6 dB and more from a note's first period to its third",
              fmt("%.1f dB", powDb(early / late)));
    }
}

/**
 * @brief Kick body and kick against bass, in the engine (first drop of the listening seed).
 *
 * The references' kick bodies stay within 20 dB of their peak up to the first bass slot (Tools/ref_kick.py:
 * median 104 ms, which is where its window ends). The kick before 19.09.2026 fell 20 dB by 76 ms: its
 * decay knob said so, and a longer decay was cut by a -24 dB tail limit at the slot. Since 19.09.2026
 * the limit is -15 dB and the decay 240 ms. What must not happen in exchange is that the tail masks the
 * first bass note, so the check reads the two parts separately in the first sixteenth after the kick.
 * And the level of the bass against the kick, K-weighted, on the same time base as Tools/ref_bass.py
 * ("b/k": the three bass sixteenths against the kick's own quarter beat): references -4.7 dB, quartiles
 * -5.8 .. -2.3; the bass before 19.09.2026 read -12.8.
 */
void testKickBody()
{
    section("kick body, kick tail against the first bass note, bass against kick");
    const double sr = 48000.0;
    // (a) The default kick alone, constrained for the rolling bass at 145 BPM.
    {
        const double slot = 0.25 * 60.0 / 145.0;
        ParamStore p;
        std::vector<float> kv = moduleValues(p, Module::Kick);
        Kick::constrain(kv.data(), slot, 6);
        Kick k;
        k.prepare(sr);
        k.update(kv.data(), 6);
        k.trigger(1.0f);
        std::vector<float> y(24000);
        k.process(y.data(), static_cast<int>(y.size()));
        // Tools/ref_kick.py's "body": the kick band-limited to 20 .. 250 Hz (brick wall, zero-padded to
        // twice its length), its analytic envelope smoothed over 1 ms and read every millisecond, from
        // the onset to the first reading 20 dB under the peak.
        const size_t n = 1u << 16;
        std::vector<std::complex<double>> s(n);
        for (size_t i = 0; i < y.size(); ++i) s[i] = y[i];
        fft(s);
        for (size_t k = 0; k < n; ++k) {
            const double f = static_cast<double>(k) * sr / static_cast<double>(n);
            // analytic signal: positive frequencies doubled, negative ones and everything outside the band gone
            if (k == 0 || k >= n / 2 || f < 20.0 || f > 250.0) s[k] = 0.0;
            else s[k] *= 2.0;
        }
        for (auto& c : s) c = std::conj(c);   // inverse FFT via conjugation
        fft(s);
        const size_t ms = static_cast<size_t>(0.001 * sr);
        std::vector<double> mag(y.size()), env;
        for (size_t i = 0; i < y.size(); ++i) mag[i] = std::abs(s[i]) / static_cast<double>(n);
        for (size_t i = 0; i < y.size(); i += ms) {
            double m = 0.0;
            int c = 0;
            for (size_t j = i >= ms / 2 ? i - ms / 2 : 0; j < std::min(y.size(), i + ms / 2); ++j) { m += mag[j]; ++c; }
            env.push_back(m / c);
        }
        const size_t pk = static_cast<size_t>(std::max_element(env.begin(), env.end()) - env.begin());
        size_t end = pk;
        while (end < env.size() && env[end] >= env[pk] * 0.1) ++end;
        const double body = static_cast<double>(end);
        check(body >= 100.0, "the kick body holds within 20 dB up to the first bass slot, like the references' (Tools/ref_kick.py: 104 ms)",
              fmt("%.0f ms (before 19.09.2026 76 ms)", body));
    }
    // (b) and (c) in the engine: kick alone and bass alone over bars 42..70 of the first drop.
    double latency = 0.0;
    auto render = [&](const char* solo) {
        auto e = std::make_unique<Engine>();
        e->prepare(sr, 512);
        e->params().parseText("master.auto_gain=Off master.limiter=Off master.clipper=Off master.comp_ratio=1 master.clip=Off");
        e->params().parseText(solo);
        Composer c(864566672ull);
        const TempoMap tm = c.tempoMap(e->params(), 72);
        e->setTempoMap(tm);
        latency = e->latencySamples();
        return renderEngine(*e, c, 70.0 * kBeatsPerBar, 512, sr);
    };
    const std::vector<float> ky = render("mix.bass_mute=1 mix.perc_mute=1 mix.acid_mute=1 mix.lead_mute=1 mix.arp_mute=1 mix.pad_mute=1 mix.counter_mute=1 mix.stab_mute=1 mix.drone_mute=1 mix.sfx_mute=1");
    const std::vector<float> by = render("mix.kick_mute=1 mix.perc_mute=1 mix.acid_mute=1 mix.lead_mute=1 mix.arp_mute=1 mix.pad_mute=1 mix.counter_mute=1 mix.stab_mute=1 mix.drone_mute=1 mix.sfx_mute=1");
    const double beat = 60.0 / 145.0 * sr, q = beat / 4.0;
    // Clamped to the render (19.09.2026, round "polish"): the window of the last beat measured ends at
    // beat 280 of a 70-bar render, and the engine's latency pushed that `latency` samples past the buffer --
    // a heap read past the end that the AddressSanitizer build caught (docs/rounds/2026-09.md, "UB-Suche").
    auto at = [&](double beats) { return std::min(ky.size(), static_cast<size_t>(beats * beat + latency)); };
    // K-weighting, ITU-R BS.1770-4's tabulated 48 kHz coefficients (as in testSfxLevel).
    struct Biquad { double b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        double tick(double x) { const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x; y2 = y1; y1 = y; return y; } };
    auto kweight = [&](const std::vector<float>& x) {
        Biquad s{ 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585 };
        Biquad h{ 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 };
        std::vector<float> out(x.size());
        for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(h.tick(s.tick(x[i])));
        return out;
    };
    std::vector<float> mix(ky.size());
    for (size_t i = 0; i < mix.size(); ++i) mix[i] = ky[i] + by[i];
    const std::vector<float> km = kweight(mix);
    std::vector<double> mask, bk;
    for (int bt = 42 * 4; bt < 70 * 4; ++bt) {
        if (bt % 32 >= 28) continue;   // the last bar of an eight-bar group carries its figure and a missing kick
        const size_t k0 = at(bt), s0 = at(bt + 0.25), s1 = at(bt + 0.5), e0 = at(bt + 1.0);
        (void)q;
        if (e0 <= s1 || s0 <= k0) continue;   // the last beat's window runs past the render (see at())
        mask.push_back(powDb(lowendBandPower(ky, s0, s1, 30.0, 150.0, sr) / lowendBandPower(by, s0, s1, 30.0, 150.0, sr)));
        double pk = 0.0, pb = 0.0;
        for (size_t i = k0; i < s0; ++i) pk += static_cast<double>(km[i]) * km[i];
        for (size_t i = s0; i < e0; ++i) pb += static_cast<double>(km[i]) * km[i];
        bk.push_back(powDb((pb / static_cast<double>(e0 - s0)) / (pk / static_cast<double>(s0 - k0))));
    }
    std::sort(mask.begin(), mask.end());
    std::sort(bk.begin(), bk.end());
    const double maskMed = mask[mask.size() / 2], bkMed = bk[bk.size() / 2];
    check(maskMed <= -6.0, "the longer kick tail does not mask the first bass note: in its sixteenth the bass is 6 dB and more above the kick (30..150 Hz)",
          fmt("kick against bass %.1f dB, median over %zu beats", maskMed, mask.size()));
    check(bkMed >= -5.8 && bkMed <= -2.3, "the bass against the kick, K-weighted, inside the reference quartiles (Tools/ref_bass.py: -5.8 .. -2.3 dB)",
          fmt("%.1f dB (references median -4.7; before 19.09.2026 -12.8)", bkMed));
}

} // namespace phostest
