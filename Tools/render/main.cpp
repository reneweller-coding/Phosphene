/**
 * @file main.cpp
 * @brief phos_render: renders a set offline, exports its MIDI, measures loudness and speed.
 *
 * Usage:
 * @code
 *   phos_render [options]
 *     --bars N            bars to render (default 16)
 *     --seconds S         render S seconds instead of a bar count
 *     --sr RATE           sample rate (default 48000)
 *     --block N           block size handed to the engine (default 256)
 *     --seed N            set seed (default 1)
 *     --set key=value     set a parameter; repeatable; "key=a;key2=b" also works
 *     --preset FILE       read key=value assignments from a file
 *     --tempo-ramp B:BPM  ramp the tempo from compose.bpm at beat 0 to BPM at beat B
 *     --solo kick|bass    mute everything else
 *     --out FILE.wav      write the audio (32-bit float unless --pcm24)
 *     --pcm24             write 24-bit PCM
 *     --midi FILE.mid     write the score as a Standard MIDI File
 *     --report            print loudness, peak and timing
 *     --bench             render without writing and report the realtime factor
 *     --list              print every parameter with range and default
 * @endcode
 */
#include "phos/Composer.h"
#include "phos/Engine.h"
#include "phos/Loudness.h"
#include "phos/Midi.h"
#include "phos/WavWriter.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace phos;

namespace {

void printList(const ParamStore& p)
{
    for (int i = 0; i < p.count(); ++i) {
        const ParamDesc& d = p.desc(i);
        std::printf("%-26s %-16s ", p.key(i).c_str(), d.name);
        if (d.curve == Curve::Choice && d.choices != nullptr) {
            for (int c = 0; c <= static_cast<int>(d.maxValue); ++c) std::printf("%s%s", c ? "|" : "", d.choices[c]);
            std::printf("  (default %s)\n", d.choices[static_cast<int>(d.defValue)]);
        } else {
            std::printf("%g..%g %s  (default %g)\n", static_cast<double>(d.minValue), static_cast<double>(d.maxValue), d.unit, static_cast<double>(d.defValue));
        }
    }
}

bool readFile(const char* path, std::string& out)
{
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    int bars = 16;
    double seconds = -1.0;
    int sr = 48000, block = 256;
    uint64_t seed = 1;
    std::string out, midi, solo;
    bool report = false, bench = false, pcm24 = false;
    double rampBeat = -1.0, rampBpm = 0.0;
    std::vector<std::string> sets;

    auto engine = std::make_unique<Engine>();
    ParamStore& params = engine->params();

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--bars") bars = std::atoi(next());
        else if (a == "--seconds") seconds = std::atof(next());
        else if (a == "--sr") sr = std::atoi(next());
        else if (a == "--block") block = std::atoi(next());
        else if (a == "--seed") seed = std::strtoull(next(), nullptr, 10);
        else if (a == "--set") sets.push_back(next());
        else if (a == "--preset") {
            std::string text;
            const char* path = next();
            if (!readFile(path, text)) { std::fprintf(stderr, "cannot read %s\n", path); return 2; }
            sets.push_back(text);
        }
        else if (a == "--tempo-ramp") {
            const std::string v = next();
            const size_t c = v.find(':');
            if (c == std::string::npos) { std::fprintf(stderr, "--tempo-ramp wants BEAT:BPM\n"); return 2; }
            rampBeat = std::atof(v.substr(0, c).c_str());
            rampBpm = std::atof(v.substr(c + 1).c_str());
        }
        else if (a == "--solo") solo = next();
        else if (a == "--out") out = next();
        else if (a == "--pcm24") pcm24 = true;
        else if (a == "--midi") midi = next();
        else if (a == "--report") report = true;
        else if (a == "--bench") bench = true;
        else if (a == "--list") { printList(params); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    for (const std::string& s : sets) {
        std::string err;
        if (!params.parseText(s, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    }
    const int mb = params.base(Module::Mix);
    if (solo == "kick") params.set(mb + mix::BassMute, 1.0f);
    else if (solo == "bass") params.set(mb + mix::KickMute, 1.0f);
    else if (!solo.empty()) { std::fprintf(stderr, "--solo wants kick or bass\n"); return 2; }

    const int cb = params.base(Module::Compose);
    TempoMap tempo;
    tempo.setConstant(params.get(cb + compose::Bpm));
    if (rampBeat > 0.0) {
        tempo.add(0.0, params.get(cb + compose::Bpm), true);
        tempo.add(rampBeat, rampBpm, false);
    }

    engine->prepare(sr, block);
    engine->setTempoMap(tempo);
    Composer composer(seed);
    Conductor conductor(*engine, composer);

    const double totalBeats = seconds > 0.0 ? tempo.beatAt(seconds) : static_cast<double>(bars) * kBeatsPerBar;
    const uint64_t totalSamples = static_cast<uint64_t>(std::llround(tempo.secondsAt(totalBeats) * sr));

    WavWriter wav;
    if (!out.empty() && !bench) {
        if (!wav.open(out.c_str(), sr, 2, pcm24 ? WavFormat::Pcm24 : WavFormat::Float32)) {
            std::fprintf(stderr, "cannot write %s\n", out.c_str());
            return 1;
        }
    }
    LoudnessMeter meter;
    meter.prepare(sr);

    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
    std::vector<NoteEvent> recorded;
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t renderedSamples = 0;
    double peak = 0.0;
    while (renderedSamples < totalSamples) {
        const int n = static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(block), totalSamples - renderedSamples));
        conductor.pump(params, 8.0 * kBeatsPerBar, midi.empty() ? nullptr : &recorded);
        engine->process(L.data(), R.data(), n);
        if (!bench) {
            meter.process(L.data(), R.data(), n);
            for (int i = 0; i < n; ++i) peak = std::max(peak, static_cast<double>(std::max(std::fabs(L[static_cast<size_t>(i)]), std::fabs(R[static_cast<size_t>(i)]))));
            if (!out.empty()) wav.write(L.data(), R.data(), n);
        }
        renderedSamples += static_cast<uint64_t>(n);
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    wav.close();

    if (!midi.empty()) {
        Score score;
        score.tempo = tempo;
        score.keyRoot = params.getInt(cb + compose::Key);
        score.scale = params.getInt(cb + compose::Scale);
        for (const NoteEvent& e : recorded) if (e.beat < totalBeats) score.notes.push_back(e);
        score.sort();
        if (!writeMidiFile(score, midi.c_str())) { std::fprintf(stderr, "cannot write %s\n", midi.c_str()); return 1; }
    }

    const double audioSeconds = static_cast<double>(totalSamples) / sr;
    if (bench || report) {
        std::printf("path %s, %.1f s of audio in %.3f s: %.1fx realtime (%.2f %% of a core)\n",
                    kVecPathName, audioSeconds, elapsed, audioSeconds / elapsed, 100.0 * elapsed / audioSeconds);
    }
    if (report && !bench) {
        const LoudnessReading r = meter.read();
        std::printf("integrated %.1f LUFS, short-term %.1f LUFS, range %.1f LU, true peak %.2f dBTP, sample peak %.2f dBFS\n",
                    static_cast<double>(r.integrated), static_cast<double>(r.shortTerm), static_cast<double>(r.range),
                    static_cast<double>(r.truePeak), 20.0 * std::log10(std::max(peak, 1e-12)));
        std::printf("kick end pitch %.2f Hz, %.2f bars\n", static_cast<double>(engine->kick().tunedEndHz()), totalBeats / kBeatsPerBar);
        if (!midi.empty()) std::printf("MIDI: %zu events written to %s\n", recorded.size(), midi.c_str());
    }
    return 0;
}
