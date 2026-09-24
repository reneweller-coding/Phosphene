/**
 * @file Vocal.cpp
 * @brief The voice pack and the voice generator (Vocal.h).
 */
#include "phos/Vocal.h"
#include "phos/Params.h"
#include "phos/WaveTableFile.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace phos {

namespace {
constexpr double kPiD = 3.141592653589793;

/** @brief The IMA ADPCM step sizes (89 entries) and index changes, as the IMA/DVI recommendation has them. */
constexpr int kImaStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767 };
constexpr int kImaIndex[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/**
 * @brief Peterson and Barney's vowel formants, male averages F1, F2, F3 in Hz (J. Acoust. Soc. Am.
 *        24(2), 1952): i, I, E, ae, a, open o, U, u, V, er.
 */
constexpr float kVowels[10][3] = {
    { 270, 2290, 3010 }, { 390, 1990, 2550 }, { 530, 1840, 2480 }, { 660, 1720, 2410 }, { 730, 1090, 2440 },
    { 570, 840, 2410 },  { 440, 1020, 2240 }, { 300, 870, 2240 },  { 520, 1190, 2390 }, { 490, 1350, 1690 } };
constexpr int kNumVowels = 10;

/**
 * @brief Level of each vocal type against vocal.level, in dB (calibrated 19.09.2026 on the listening
 *        seed, loudest 400 ms of the vocal strip against the rest of the mix, K-weighted).
 *
 * A spoken phrase in a breakdown measured -0.9 dB against the rest before this table, and +1.4 dB once
 * the bed had its own level (the rest of a breakdown is quieter then): on top of the pad rather than
 * in it. 8.5 dB off puts it about 3 dB under the rest. A chop sits in a drop, where the rest is a full
 * mix, and keeps its level to stay a word rather than a click (-7.8 dB against the rest). The
 * synthetic voices were set by the same measure to sit with the phrases.
 */
float vocalTypeGain(SfxType t)
{
    switch (t) {
    case SfxType::SpokenWord:   return dbToGain(-8.5f);
    case SfxType::VoiceChop:    return dbToGain(0.0f);
    case SfxType::FormantVoice: return dbToGain(-6.0f);
    case SfxType::AlienChatter: return dbToGain(-6.0f);
    default:                    return 1.0f;
    }
}

/**
 * @brief The loaded voice pack: built once, read only through voicePhrase()/voicePhraseCount().
 *
 * Same call-once gate as WaveTableFile.cpp's Library, and for the same reason: a plain `bool
 * attempted` let a second thread see "already attempted" and return an empty `phrases` while the
 * first thread's parse() was still filling it in.
 */
struct Pack {
    std::atomic<bool> attempted{ false };
    std::mutex mutex;
    std::vector<VoicePhrase> phrases;
};
Pack& pack() { static Pack p; return p; }
std::string& voiceSearchPath() { static std::string s; return s; }

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    out.clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

bool parse(const std::vector<uint8_t>& f, std::vector<VoicePhrase>& out, std::string* error)
{
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    const size_t n = f.size();
    if (n < 16 || std::memcmp(f.data(), "PHOSVX01", 8) != 0) return fail("not a voice pack (magic PHOSVX01 missing)");
    const uint32_t count = rd32(f.data() + 8), rate = rd32(f.data() + 12);
    if (count > 4096 || rate < 4000 || rate > 96000) return fail("implausible header");
    size_t at = 16;
    for (uint32_t k = 0; k < count; ++k) {
        if (at + 2 > n) return fail("truncated phrase header");
        VoicePhrase p;
        p.category = f[at];
        const size_t len = f[at + 1];
        at += 2;
        if (at + len + 28 > n) return fail("truncated phrase header");
        p.name.assign(reinterpret_cast<const char*>(f.data() + at), len);
        at += len;
        const uint32_t samples = rd32(f.data() + at);
        p.throwAt = rd32(f.data() + at + 4);
        p.chopStart = rd32(f.data() + at + 8);
        p.chopLength = rd32(f.data() + at + 12);
        const int pred = static_cast<int16_t>(uint16_t(f[at + 16]) | (uint16_t(f[at + 17]) << 8));
        const int index = f[at + 18];
        const uint32_t bytes = rd32(f.data() + at + 20);
        at += 24;
        if (bytes != (samples + 1) / 2 || at + bytes > n || index > 88) return fail("bad phrase data");
        std::vector<int16_t> pcm;
        decodeImaAdpcm(f.data() + at, samples, pred, index, pcm);
        at += bytes;
        p.sampleRate = static_cast<int>(rate);
        p.samples.resize(pcm.size());
        for (size_t i = 0; i < pcm.size(); ++i) p.samples[i] = static_cast<float>(pcm[i]) * (1.0f / 32768.0f);
        p.throwAt = std::min<uint32_t>(p.throwAt, samples);
        p.chopStart = std::min<uint32_t>(p.chopStart, samples);
        p.chopLength = std::min<uint32_t>(p.chopLength, samples - p.chopStart);
        out.push_back(std::move(p));
    }
    if (at + 8 > n || std::memcmp(f.data() + at, "PHOSVXE1", 8) != 0) return fail("the end marker PHOSVXE1 is missing");
    return true;
}

/** @brief Raised-cosine ramp. */
inline float ramp(double u) { return u >= 1.0 ? 1.0f : (u <= 0.0 ? 0.0f : static_cast<float>(0.5 - 0.5 * std::cos(kPiD * u))); }
} // namespace

void decodeImaAdpcm(const uint8_t* data, size_t samples, int predictor, int stepIndex, std::vector<int16_t>& out)
{
    out.resize(samples);
    int pred = predictor, index = std::clamp(stepIndex, 0, 88);
    for (size_t i = 0; i < samples; ++i) {
        const int nib = (i & 1) ? (data[i >> 1] >> 4) & 0xF : data[i >> 1] & 0xF;
        const int step = kImaStep[index];
        int diff = step >> 3;
        if (nib & 4) diff += step;
        if (nib & 2) diff += step >> 1;
        if (nib & 1) diff += step >> 2;
        pred += (nib & 8) ? -diff : diff;
        pred = std::clamp(pred, -32768, 32767);
        index = std::clamp(index + kImaIndex[nib & 7], 0, 88);
        out[i] = static_cast<int16_t>(pred);
    }
}

void setVoicePackSearchPath(const std::string& directory) { voiceSearchPath() = directory; }

int loadVoicePack(const char* path, std::string* error)
{
    Pack& p = pack();
    // Hot path: no lock once loaded, matching WaveTableFile.cpp's loadWaveTableLibrary().
    if (p.attempted.load(std::memory_order_acquire)) return static_cast<int>(p.phrases.size());
    std::lock_guard<std::mutex> guard(p.mutex);
    if (p.attempted.load(std::memory_order_relaxed)) return static_cast<int>(p.phrases.size());
    if (error != nullptr) error->clear();
    const std::string name = (path != nullptr && path[0] != 0) ? path : "voices.phosvx";
    std::vector<uint8_t> file;
    std::string tried = name;
    bool got = readFile(name, file);
    const bool bare = name.find('/') == std::string::npos && name.find('\\') == std::string::npos;
    if (!got && bare && !voiceSearchPath().empty()) {
        const std::string q = voiceSearchPath() + "/" + name;
        tried += ", " + q;
        got = readFile(q, file);
    }
    if (!got && bare && !waveTableSearchPath().empty()) {
        const std::string q = waveTableSearchPath() + "/" + name;
        tried += ", " + q;
        got = readFile(q, file);
    }
#if defined(PHOS_SOURCE_DATA_DIR)
    if (!got && bare) {
        const std::string q = std::string(PHOS_SOURCE_DATA_DIR) + "/" + name;
        tried += ", " + q;
        got = readFile(q, file);
    }
#endif
    if (!got) {
        if (error != nullptr) *error = "cannot open the voice pack (" + tried + ")";
        // The one attempt is made either way; see the matching comment in loadWaveTableLibrary().
        p.attempted.store(true, std::memory_order_release);
        return 0;
    }
    std::vector<VoicePhrase> phrases;
    if (!parse(file, phrases, error)) {
        p.attempted.store(true, std::memory_order_release);
        return 0;   // a broken pack leaves nothing half-loaded
    }
    p.phrases = std::move(phrases);
    p.attempted.store(true, std::memory_order_release);
    return static_cast<int>(p.phrases.size());
}

void resetVoicePack()
{
    Pack& p = pack();
    std::lock_guard<std::mutex> guard(p.mutex);   // see resetWaveTableLibrary()'s comment
    p.attempted.store(false, std::memory_order_release);
    p.phrases.clear();
}

int voicePhraseCount() { return static_cast<int>(pack().phrases.size()); }

const VoicePhrase& voicePhrase(int index) { return pack().phrases[static_cast<size_t>(index)]; }

// ---------------------------------------------------------------------------------------------

void Vocal::prepare(double sampleRate)
{
    sr_ = sampleRate;
    loadVoicePack();
    reset();
}

void Vocal::reset()
{
    voice_[0] = Voice{};
    voice_[1] = Voice{};
}

void Vocal::update(const float* v, int keyRoot)
{
    keyRoot_ = keyRoot;
    pitch_ = v[vocal::Pitch];
    drive_ = v[vocal::Drive];
    width_ = v[vocal::Width];
}

int Vocal::active() const { return (voice_[0].on ? 1 : 0) + (voice_[1].on ? 1 : 0); }

void Vocal::trigger(SfxType type, int samples, float velocity, double late, double samplesPerBeat, uint64_t pick)
{
    if (sfxTypePart(type) != Part::Vocal) return;
    const bool spoken = type == SfxType::SpokenWord || type == SfxType::VoiceChop;
    const int phrases = voicePhraseCount();
    if (spoken && phrases == 0) return;
    // The one sounding hands over: it fades out over 10 ms in the other slot.
    int slot = voice_[0].on && !voice_[0].fading ? 1 : 0;
    Voice& old = voice_[1 - slot];
    if (old.on) old.fading = true;
    Voice& v = voice_[slot];
    v = Voice{};
    v.rng.seed(pick);
    v.type = type;
    v.on = true;
    v.late = late;
    v.velocity = clampv(velocity, 0.0f, 1.0f) * vocalTypeGain(type);
    v.spb = std::max(1.0, samplesPerBeat);
    v.pan = width_ * 0.4f * v.rng.bipolar();
    const float sr = static_cast<float>(sr_);
    v.hp1.setQ(250.0f, 0.70710678f, sr);
    v.hp2.setQ(250.0f, 0.70710678f, sr);
    v.lp.setQ(4500.0f, 0.70710678f, sr);
    // Pitch by resampling: the pick chooses unshifted (twice as likely), down by vocal.pitch, down by
    // half of it, or up by half of it.
    static const float kShift[5] = { 0.0f, 0.0f, -1.0f, -0.5f, 0.5f };
    const float semis = kShift[v.rng.below(5)] * pitch_;
    const double ratio = std::pow(2.0, semis / 12.0);
    const int pc = ((keyRoot_ % 12) + 12) % 12;
    if (spoken) {
        v.phrase = v.rng.below(phrases);
        const VoicePhrase& p = voicePhrase(v.phrase);
        v.rate = static_cast<double>(p.sampleRate) / sr_ * ratio;
        if (type == SfxType::SpokenWord) {
            v.length = static_cast<long long>(static_cast<double>(p.samples.size()) / v.rate) + 1;
        } else {
            // Two to four sixteenths of the first word, then the whole word once more, thrown.
            v.length = std::max<long long>(1, samples);
            v.read = p.chopStart;
        }
    } else {
        v.length = std::max<long long>(1, samples);
        if (type == SfxType::FormantVoice) {
            v.length = std::clamp<long long>(v.length, static_cast<long long>(0.5 * sr_), static_cast<long long>(8.0 * sr_));
            for (int& x : v.vowel) x = v.rng.below(kNumVowels);
            // A sung vowel has a pitch, so its shift lands on an interval every mode has -- the unison, the fourth,
            // the fifth or the octave -- the one nearest the spoken shift (24.09.2026). vocal.pitch at its 3 put two
            // of five vowels 1.5 semitones off the root, between two notes, and one on the major sixth under it.
            static const int kSafe[7] = { -12, -7, -5, 0, 5, 7, 12 };
            int sung = 0;
            for (int k : kSafe) if (std::fabs(static_cast<float>(k) - semis) < std::fabs(static_cast<float>(sung) - semis)) sung = k;
            v.pitchHz = midiToHz(48 + pc + sung);
            v.glide = v.rng.below(2) == 0 ? 7.0 : -5.0;
        } else {
            v.length = std::clamp<long long>(v.length, static_cast<long long>(0.2 * sr_), static_cast<long long>(3.0 * sr_));
            v.ringHz = 400.0 + 700.0 * v.rng.uniform();
        }
    }
}

float Vocal::voiceSample(Voice& v, float& throwW)
{
    const float sr = static_cast<float>(sr_);
    const double posD = static_cast<double>(v.pos) + v.late;
    const double t = posD / sr_;
    const double lenS = static_cast<double>(v.length) / sr_;
    float s = 0.0f, target = 0.0f;
    switch (v.type) {
    case SfxType::SpokenWord: {
        const VoicePhrase& p = voicePhrase(v.phrase);
        const double at = posD * v.rate;
        const size_t i0 = static_cast<size_t>(at);
        if (i0 + 1 < p.samples.size()) {
            const float f = static_cast<float>(at - static_cast<double>(i0));
            s = p.samples[i0] + f * (p.samples[i0 + 1] - p.samples[i0]);
        }
        target = at >= static_cast<double>(p.throwAt) ? 1.0f : 0.0f;
        break;
    }
    case SfxType::VoiceChop: {
        // Sixteenths of the first word; the last sixteenth-group plays the word out and is thrown.
        const VoicePhrase& p = voicePhrase(v.phrase);
        const double s16 = v.spb / 4.0;
        const long long reps = std::max<long long>(1, static_cast<long long>(static_cast<double>(v.length) / s16));
        const long long k = static_cast<long long>(posD / s16);
        const double j = posD - static_cast<double>(k) * s16;
        const bool last = k >= reps - 1;
        const double sliceLen = last ? static_cast<double>(p.chopLength) : std::min(static_cast<double>(p.chopLength), 0.8 * s16 * v.rate);
        const double at = j * v.rate;
        if (at < sliceLen) {
            const size_t i0 = static_cast<size_t>(p.chopStart + at);
            if (i0 + 1 < p.samples.size()) {
                const float f = static_cast<float>(p.chopStart + at - std::floor(p.chopStart + at));
                s = p.samples[i0] + f * (p.samples[i0 + 1] - p.samples[i0]);
            }
            const double edge = 0.002 * sr_ * v.rate;
            s *= std::min(ramp(at / edge), ramp((sliceLen - at) / edge));
        }
        target = last ? 1.0f : 0.0f;
        if (last && at >= sliceLen) v.on = false;
        break;
    }
    case SfxType::FormantVoice: {
        const double x = std::min(1.0, t / lenS);
        // Three vowels across the event, gliding: the formants are retuned every sample (the SVF is
        // stable under that), the pitch glides a fifth up or a fourth down, vibrato after 0.3 s.
        const double seg = x * 2.0;
        const int a = std::min(1, static_cast<int>(seg));
        const double u = seg - a;
        const float* va = kVowels[v.vowel[a]];
        const float* vb = kVowels[v.vowel[a + 1]];
        static const float kBw[3] = { 80.0f, 100.0f, 120.0f };
        for (int k = 0; k < 3; ++k) {
            const float fk = va[k] + static_cast<float>(u) * (vb[k] - va[k]);
            v.formant[k].setQ(fk, fk / kBw[k], sr);
        }
        v.vibPh += 5.5 / sr_;
        const double vib = std::min(1.0, std::max(0.0, (t - 0.3) / 0.3)) * 0.25 * std::sin(2.0 * kPiD * v.vibPh);
        const double hz = v.pitchHz * std::pow(2.0, (v.glide * (0.5 - 0.5 * std::cos(kPiD * x)) + vib) / 12.0);
        v.osc.set(hz, sr_, 0.0f, 0.5f);
        // The glottal tilt: a one-pole low pass at 800 Hz on the saw.
        v.tilt += (v.osc.next() - v.tilt) * static_cast<float>(1.0 - std::exp(-2.0 * kPiD * 800.0 / sr_));
        float lp, bp, hp, sum = 0.0f;
        static const float kW[3] = { 1.0f, 0.7f, 0.35f };
        for (int k = 0; k < 3; ++k) { v.formant[k].tick(v.tilt, lp, bp, hp); sum += kW[k] * bp * v.formant[k].k; }
        s = 1.6f * sum * ramp(t / 0.06) * ramp((lenS - t) / 0.2);
        target = x > 0.7 ? 1.0f : 0.0f;
        break;
    }
    case SfxType::AlienChatter: {
        if (v.pos >= v.sylEnd) {
            // A new syllable: 45 .. 90 ms, a random vowel on a vocal tract a third smaller, a new pitch.
            v.sylLen = static_cast<long long>((0.045 + 0.045 * v.rng.uniform()) * sr_);
            v.sylEnd = v.pos + v.sylLen;
            const float* vw = kVowels[v.rng.below(kNumVowels)];
            for (int k = 0; k < 3; ++k) v.sylF[k] = std::min(1.35f * vw[k], 0.4f * sr);
            for (int k = 0; k < 3; ++k) v.formant[k].setQ(v.sylF[k], v.sylF[k] / (90.0f + 30.0f * k), sr);
            v.pitchHz = 220.0 + 300.0 * v.rng.uniform();
        }
        v.osc.set(v.pitchHz, sr_, 0.0f, 0.5f);
        v.tilt += (v.osc.next() - v.tilt) * static_cast<float>(1.0 - std::exp(-2.0 * kPiD * 1200.0 / sr_));
        float lp, bp, hp, sum = 0.0f;
        for (int k = 0; k < 3; ++k) { v.formant[k].tick(v.tilt, lp, bp, hp); sum += (k == 0 ? 1.0f : 0.6f) * bp * v.formant[k].k; }
        const double u = 1.0 - static_cast<double>(v.sylEnd - v.pos) / static_cast<double>(v.sylLen);
        const float env = static_cast<float>(std::sqrt(std::max(0.0, std::sin(kPiD * u))));
        v.ringPh += v.ringHz / sr_;
        v.ringPh -= std::floor(v.ringPh);
        const float ring = static_cast<float>(std::sin(2.0 * kPiD * v.ringPh));
        s = 1.8f * sum * env * (0.4f + 0.6f * ring) * ramp((lenS - t) / 0.03);
        target = t > 0.75 * lenS ? 1.0f : 0.0f;
        break;
    }
    default: break;
    }
    // Treatment: band pass 250 Hz .. 4.5 kHz, then saturation (normalised so a quiet signal keeps its level).
    float lp, bp, hp;
    v.hp1.tick(s, lp, bp, hp);
    v.hp2.tick(hp, lp, bp, hp);
    s = v.lp.lp(hp);
    const float k = 1.0f + 4.0f * drive_;
    s = std::tanh(k * s) / k * (1.0f + drive_);
    if (v.fading) {
        v.fade -= static_cast<float>(1.0 / (0.010 * sr_));
        if (v.fade <= 0.0f) { v.fade = 0.0f; v.on = false; }
    }
    // The throw weight follows its target over 10 ms, so the send never clicks.
    v.throwW += (target - v.throwW) * static_cast<float>(1.0 - std::exp(-1.0 / (0.010 * sr_)));
    throwW = v.throwW * v.fade * v.velocity;
    ++v.pos;
    if (v.pos >= v.length + static_cast<long long>(0.05 * sr_)) v.on = false;
    return s * v.fade * v.velocity;
}

void Vocal::process(float* L, float* R, float* throwOut, int n)
{
    for (int i = 0; i < n; ++i) { L[i] = 0.0f; R[i] = 0.0f; throwOut[i] = 0.0f; }
    for (Voice& v : voice_) {
        if (!v.on) continue;
        const double theta = (static_cast<double>(clampv(v.pan, -1.0f, 1.0f)) + 1.0) * kPiD / 4.0;
        const float gl = static_cast<float>(std::cos(theta) * 1.41421356), gr = static_cast<float>(std::sin(theta) * 1.41421356);
        for (int i = 0; i < n && v.on; ++i) {
            float w = 0.0f;
            const float s = voiceSample(v, w);
            L[i] += s * gl;
            R[i] += s * gr;
            throwOut[i] += w;
        }
    }
}

} // namespace phos
