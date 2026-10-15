/**
 * @file Vocal.cpp
 * @brief The voice pack and the voice generator (Vocal.h).
 * @author Rene Weller
 *
 * @par Compile Flags:
 *   @arg @c PHOS_SOURCE_DATA_DIR - the source tree's data directory (Core/CMakeLists.txt sets it), searched after the
 *                                  search path
 */
#include "phos/Vocal.h"
#include "phos/Util.h"
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
constexpr double PiD = 3.141592653589793;   ///< pi

/** @brief The IMA ADPCM step sizes (89 entries) and index changes, as the IMA/DVI recommendation has them. */
constexpr int ImaStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767 };
constexpr int ImaIndex[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };   ///< the index changes

/**
 * @brief Peterson and Barney's vowel formants, male averages F1, F2, F3 in Hz (J. Acoust. Soc. Am.
 *        24(2), 1952): i, I, E, ae, a, open o, U, u, V, er.
 */
constexpr float Vowels[10][3] = {
    { 270, 2290, 3010 }, { 390, 1990, 2550 }, { 530, 1840, 2480 }, { 660, 1720, 2410 }, { 730, 1090, 2440 },
    { 570, 840, 2410 },  { 440, 1020, 2240 }, { 300, 870, 2240 },  { 520, 1190, 2390 }, { 490, 1350, 1690 } };
constexpr int NumVowels = 10;   ///< how many vowels the table has

/**
 * @brief Level of each vocal type against vocal.level, in dB (calibrated 19.09.2026 on the listening
 *        seed, loudest 400 ms of the vocal strip against the rest of the mix, K-weighted).
 *
 * A spoken phrase in a breakdown measured -0.9 dB against the rest before this table, and +1.4 dB once
 * the bed had its own level (the rest of a breakdown is quieter then): on top of the pad rather than
 * in it. 8.5 dB off puts it about 3 dB under the rest. A chop sits in a drop, where the rest is a full
 * mix, and keeps its level to stay a word rather than a click (-7.8 dB against the rest). The
 * synthetic voices were set by the same measure to sit with the phrases.
 * @param[in] t  the vocal type
 * @return the level in dB
 */
float vocalTypeGain(SfxTypeE t)
{
    switch (t) {
    case SfxTypeE::SPOKEN_WORD:   return dbToGain(-8.5f);
    case SfxTypeE::VOICE_CHOP:    return dbToGain(0.0f);
    case SfxTypeE::FORMANT_VOICE: return dbToGain(-6.0f);
    case SfxTypeE::ALIEN_CHATTER: return dbToGain(-6.0f);
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
    std::atomic<bool> m_attempted{ false };   ///< the pack was looked for (the call-once gate)
    std::mutex m_mutex;   ///< serialises the threads that find it false
    std::vector<VoicePhrase> m_phrases;   ///< the phrases of the pack
};
/**
 * @brief The pack, created on first use.
 * @return the pack
 */
Pack& pack() { static Pack p; return p; }
/**
 * @brief Where the voice pack is looked for.
 * @return the directory
 */
std::string& voiceSearchPath() { static std::string s; return s; }

/**
 * @brief A little-endian 32-bit word at @p p.
 * @param[in] p  the bytes
 * @return the word
 */
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

/**
 * @brief Parses a voice pack (PHOSVX01) into its phrases; false, with a reason in @p error, if it is not one.
 * @param[in]  f      the pack's bytes
 * @param[out] out    the phrases
 * @param[out] error  receives the reason, or null
 * @return false if it is not a voice pack
 */
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
        p.m_category = f[at];
        const size_t len = f[at + 1];
        at += 2;
        if (at + len + 28 > n) return fail("truncated phrase header");
        p.m_name.assign(reinterpret_cast<const char*>(f.data() + at), len);
        at += len;
        const uint32_t samples = rd32(f.data() + at);
        p.m_throwAt = rd32(f.data() + at + 4);
        p.m_chopStart = rd32(f.data() + at + 8);
        p.m_chopLength = rd32(f.data() + at + 12);
        const int pred = static_cast<int16_t>(uint16_t(f[at + 16]) | (uint16_t(f[at + 17]) << 8));
        const int index = f[at + 18];
        const uint32_t bytes = rd32(f.data() + at + 20);
        at += 24;
        if (bytes != (samples + 1) / 2 || at + bytes > n || index > 88) return fail("bad phrase data");
        std::vector<int16_t> pcm;
        decodeImaAdpcm(f.data() + at, samples, pred, index, pcm);
        at += bytes;
        p.m_sampleRate = static_cast<int>(rate);
        p.m_samples.resize(pcm.size());
        for (size_t i = 0; i < pcm.size(); ++i) p.m_samples[i] = static_cast<float>(pcm[i]) * (1.0f / 32768.0f);
        p.m_throwAt = std::min<uint32_t>(p.m_throwAt, samples);
        p.m_chopStart = std::min<uint32_t>(p.m_chopStart, samples);
        p.m_chopLength = std::min<uint32_t>(p.m_chopLength, samples - p.m_chopStart);
        out.push_back(std::move(p));
    }
    if (at + 8 > n || std::memcmp(f.data() + at, "PHOSVXE1", 8) != 0) return fail("the end marker PHOSVXE1 is missing");
    return true;
}

/**
 * @brief Raised-cosine ramp.
 * @param[in] u  the place, 0..1
 * @return the ramp, 0..1
 */
inline float ramp(double u) { return u >= 1.0 ? 1.0f : (u <= 0.0 ? 0.0f : static_cast<float>(0.5 - 0.5 * std::cos(PiD * u))); }
} // namespace

void decodeImaAdpcm(const uint8_t* data, size_t samples, int predictor, int stepIndex, std::vector<int16_t>& out)
{
    out.resize(samples);
    int pred = predictor, index = std::clamp(stepIndex, 0, 88);
    for (size_t i = 0; i < samples; ++i) {
        const int nib = (i & 1) ? (data[i >> 1] >> 4) & 0xF : data[i >> 1] & 0xF;
        const int step = ImaStep[index];
        int diff = step >> 3;
        if (nib & 4) diff += step;
        if (nib & 2) diff += step >> 1;
        if (nib & 1) diff += step >> 2;
        pred += (nib & 8) ? -diff : diff;
        pred = std::clamp(pred, -32768, 32767);
        index = std::clamp(index + ImaIndex[nib & 7], 0, 88);
        out[i] = static_cast<int16_t>(pred);
    }
}

void setVoicePackSearchPath(const std::string& directory) { voiceSearchPath() = directory; }

int loadVoicePack(const char* path, std::string* error)
{
    Pack& p = pack();
    // Hot path: no lock once loaded, matching WaveTableFile.cpp's loadWaveTableLibrary().
    if (p.m_attempted.load(std::memory_order_acquire)) return static_cast<int>(p.m_phrases.size());
    std::lock_guard<std::mutex> guard(p.m_mutex);
    if (p.m_attempted.load(std::memory_order_relaxed)) return static_cast<int>(p.m_phrases.size());
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
        p.m_attempted.store(true, std::memory_order_release);
        return 0;
    }
    std::vector<VoicePhrase> phrases;
    if (!parse(file, phrases, error)) {
        p.m_attempted.store(true, std::memory_order_release);
        return 0;   // a broken pack leaves nothing half-loaded
    }
    p.m_phrases = std::move(phrases);
    p.m_attempted.store(true, std::memory_order_release);
    return static_cast<int>(p.m_phrases.size());
}

void resetVoicePack()
{
    Pack& p = pack();
    std::lock_guard<std::mutex> guard(p.m_mutex);   // see resetWaveTableLibrary()'s comment
    p.m_attempted.store(false, std::memory_order_release);
    p.m_phrases.clear();
}

int voicePhraseCount() { return static_cast<int>(pack().m_phrases.size()); }

const VoicePhrase& voicePhrase(int index) { return pack().m_phrases[static_cast<size_t>(index)]; }

// ---------------------------------------------------------------------------------------------

void Vocal::prepare(double sampleRate)
{
    m_sr = sampleRate;
    loadVoicePack();
    reset();
}

void Vocal::reset()
{
    m_voice[0] = Voice{};
    m_voice[1] = Voice{};
}

void Vocal::update(const float* v, int keyRoot)
{
    m_keyRoot = keyRoot;
    m_pitch = v[vocal::PITCH];
    m_drive = v[vocal::DRIVE];
    m_width = v[vocal::WIDTH];
}

int Vocal::active() const { return (m_voice[0].m_on ? 1 : 0) + (m_voice[1].m_on ? 1 : 0); }

void Vocal::trigger(SfxTypeE type, int samples, float velocity, double late, double samplesPerBeat, uint64_t pick)
{
    if (sfxTypePart(type) != PartE::VOCAL) return;
    const bool spoken = type == SfxTypeE::SPOKEN_WORD || type == SfxTypeE::VOICE_CHOP;
    const int phrases = voicePhraseCount();
    if (spoken && phrases == 0) return;
    // The one sounding hands over: it fades out over 10 ms in the other slot.
    int slot = m_voice[0].m_on && !m_voice[0].m_fading ? 1 : 0;
    Voice& old = m_voice[1 - slot];
    if (old.m_on) old.m_fading = true;
    Voice& v = m_voice[slot];
    v = Voice{};
    v.m_rng.seed(pick);
    v.m_type = type;
    v.m_on = true;
    v.m_late = late;
    v.m_velocity = clampv(velocity, 0.0f, 1.0f) * vocalTypeGain(type);
    v.m_spb = std::max(1.0, samplesPerBeat);
    v.m_pan = m_width * 0.4f * v.m_rng.bipolar();
    const float sr = static_cast<float>(m_sr);
    v.m_hp1.setQ(250.0f, 0.70710678f, sr);
    v.m_hp2.setQ(250.0f, 0.70710678f, sr);
    v.m_lp.setQ(4500.0f, 0.70710678f, sr);
    // Pitch by resampling: the pick chooses unshifted (twice as likely), down by vocal.pitch, down by
    // half of it, or up by half of it.
    static const float Shift[5] = { 0.0f, 0.0f, -1.0f, -0.5f, 0.5f };
    const float semis = Shift[v.m_rng.below(5)] * m_pitch;
    const double ratio = std::pow(2.0, semis / 12.0);
    const int pc = ((m_keyRoot % 12) + 12) % 12;
    if (spoken) {
        v.m_phrase = v.m_rng.below(phrases);
        const VoicePhrase& p = voicePhrase(v.m_phrase);
        v.m_rate = static_cast<double>(p.m_sampleRate) / m_sr * ratio;
        if (type == SfxTypeE::SPOKEN_WORD) {
            v.m_length = static_cast<long long>(static_cast<double>(p.m_samples.size()) / v.m_rate) + 1;
        } else {
            // Two to four sixteenths of the first word, then the whole word once more, thrown.
            v.m_length = std::max<long long>(1, samples);
            v.m_read = p.m_chopStart;
        }
    } else {
        v.m_length = std::max<long long>(1, samples);
        if (type == SfxTypeE::FORMANT_VOICE) {
            v.m_length = std::clamp<long long>(v.m_length, static_cast<long long>(0.5 * m_sr), static_cast<long long>(8.0 * m_sr));
            for (int& x : v.m_vowel) x = v.m_rng.below(NumVowels);
            // A sung vowel has a pitch, so its shift lands on an interval every mode has -- the unison, the fourth,
            // the fifth or the octave -- the one nearest the spoken shift (24.09.2026). vocal.pitch at its 3 put two
            // of five vowels 1.5 semitones off the root, between two notes, and one on the major sixth under it.
            static const int Safe[7] = { -12, -7, -5, 0, 5, 7, 12 };
            int sung = 0;
            for (int k : Safe) if (std::fabs(static_cast<float>(k) - semis) < std::fabs(static_cast<float>(sung) - semis)) sung = k;
            v.m_pitchHz = midiToHz(48 + pc + sung);
            v.m_glide = v.m_rng.below(2) == 0 ? 7.0 : -5.0;
        } else {
            v.m_length = std::clamp<long long>(v.m_length, static_cast<long long>(0.2 * m_sr), static_cast<long long>(3.0 * m_sr));
            v.m_ringHz = 400.0 + 700.0 * v.m_rng.uniform();
        }
    }
}

float Vocal::voiceSample(Voice& v, float& throwW)
{
    const float sr = static_cast<float>(m_sr);
    const double posD = static_cast<double>(v.m_pos) + v.m_late;
    const double t = posD / m_sr;
    const double lenS = static_cast<double>(v.m_length) / m_sr;
    float s = 0.0f, target = 0.0f;
    switch (v.m_type) {
    case SfxTypeE::SPOKEN_WORD: {
        const VoicePhrase& p = voicePhrase(v.m_phrase);
        const double at = posD * v.m_rate;
        const size_t i0 = static_cast<size_t>(at);
        if (i0 + 1 < p.m_samples.size()) {
            const float f = static_cast<float>(at - static_cast<double>(i0));
            s = p.m_samples[i0] + f * (p.m_samples[i0 + 1] - p.m_samples[i0]);
        }
        target = at >= static_cast<double>(p.m_throwAt) ? 1.0f : 0.0f;
        break;
    }
    case SfxTypeE::VOICE_CHOP: {
        // Sixteenths of the first word; the last sixteenth-group plays the word out and is thrown.
        const VoicePhrase& p = voicePhrase(v.m_phrase);
        const double s16 = v.m_spb / 4.0;
        const long long reps = std::max<long long>(1, static_cast<long long>(static_cast<double>(v.m_length) / s16));
        const long long k = static_cast<long long>(posD / s16);
        const double j = posD - static_cast<double>(k) * s16;
        const bool last = k >= reps - 1;
        const double sliceLen = last ? static_cast<double>(p.m_chopLength) : std::min(static_cast<double>(p.m_chopLength), 0.8 * s16 * v.m_rate);
        const double at = j * v.m_rate;
        if (at < sliceLen) {
            const size_t i0 = static_cast<size_t>(p.m_chopStart + at);
            if (i0 + 1 < p.m_samples.size()) {
                const float f = static_cast<float>(p.m_chopStart + at - std::floor(p.m_chopStart + at));
                s = p.m_samples[i0] + f * (p.m_samples[i0 + 1] - p.m_samples[i0]);
            }
            const double edge = 0.002 * m_sr * v.m_rate;
            s *= std::min(ramp(at / edge), ramp((sliceLen - at) / edge));
        }
        target = last ? 1.0f : 0.0f;
        if (last && at >= sliceLen) v.m_on = false;
        break;
    }
    case SfxTypeE::FORMANT_VOICE: {
        const double x = std::min(1.0, t / lenS);
        // Three vowels across the event, gliding: the formants are retuned every sample (the SVF is
        // stable under that), the pitch glides a fifth up or a fourth down, vibrato after 0.3 s.
        const double seg = x * 2.0;
        const int a = std::min(1, static_cast<int>(seg));
        const double u = seg - a;
        const float* va = Vowels[v.m_vowel[a]];
        const float* vb = Vowels[v.m_vowel[a + 1]];
        static const float Bw[3] = { 80.0f, 100.0f, 120.0f };
        for (int k = 0; k < 3; ++k) {
            const float fk = va[k] + static_cast<float>(u) * (vb[k] - va[k]);
            v.m_formant[k].setQ(fk, fk / Bw[k], sr);
        }
        v.m_vibPh += 5.5 / m_sr;
        const double vib = std::min(1.0, std::max(0.0, (t - 0.3) / 0.3)) * 0.25 * std::sin(2.0 * PiD * v.m_vibPh);
        const double hz = v.m_pitchHz * std::pow(2.0, (v.m_glide * (0.5 - 0.5 * std::cos(PiD * x)) + vib) / 12.0);
        v.m_osc.set(hz, m_sr, 0.0f, 0.5f);
        // The glottal tilt: a one-pole low pass at 800 Hz on the saw.
        v.m_tilt += (v.m_osc.next() - v.m_tilt) * static_cast<float>(1.0 - std::exp(-2.0 * PiD * 800.0 / m_sr));
        float lp, bp, hp, sum = 0.0f;
        static const float W[3] = { 1.0f, 0.7f, 0.35f };
        for (int k = 0; k < 3; ++k) { v.m_formant[k].tick(v.m_tilt, lp, bp, hp); sum += W[k] * bp * v.m_formant[k].m_k; }
        s = 1.6f * sum * ramp(t / 0.06) * ramp((lenS - t) / 0.2);
        target = x > 0.7 ? 1.0f : 0.0f;
        break;
    }
    case SfxTypeE::ALIEN_CHATTER: {
        if (v.m_pos >= v.m_sylEnd) {
            // A new syllable: 45 .. 90 ms, a random vowel on a vocal tract a third smaller, a new pitch.
            v.m_sylLen = static_cast<long long>((0.045 + 0.045 * v.m_rng.uniform()) * m_sr);
            v.m_sylEnd = v.m_pos + v.m_sylLen;
            const float* vw = Vowels[v.m_rng.below(NumVowels)];
            for (int k = 0; k < 3; ++k) v.m_sylF[k] = std::min(1.35f * vw[k], 0.4f * sr);
            for (int k = 0; k < 3; ++k) v.m_formant[k].setQ(v.m_sylF[k], v.m_sylF[k] / (90.0f + 30.0f * k), sr);
            v.m_pitchHz = 220.0 + 300.0 * v.m_rng.uniform();
        }
        v.m_osc.set(v.m_pitchHz, m_sr, 0.0f, 0.5f);
        v.m_tilt += (v.m_osc.next() - v.m_tilt) * static_cast<float>(1.0 - std::exp(-2.0 * PiD * 1200.0 / m_sr));
        float lp, bp, hp, sum = 0.0f;
        for (int k = 0; k < 3; ++k) { v.m_formant[k].tick(v.m_tilt, lp, bp, hp); sum += (k == 0 ? 1.0f : 0.6f) * bp * v.m_formant[k].m_k; }
        const double u = 1.0 - static_cast<double>(v.m_sylEnd - v.m_pos) / static_cast<double>(v.m_sylLen);
        const float env = static_cast<float>(std::sqrt(std::max(0.0, std::sin(PiD * u))));
        v.m_ringPh += v.m_ringHz / m_sr;
        v.m_ringPh -= std::floor(v.m_ringPh);
        const float ring = static_cast<float>(std::sin(2.0 * PiD * v.m_ringPh));
        s = 1.8f * sum * env * (0.4f + 0.6f * ring) * ramp((lenS - t) / 0.03);
        target = t > 0.75 * lenS ? 1.0f : 0.0f;
        break;
    }
    default: break;
    }
    // Treatment: band pass 250 Hz .. 4.5 kHz, then saturation (normalised so a quiet signal keeps its level).
    float lp, bp, hp;
    v.m_hp1.tick(s, lp, bp, hp);
    v.m_hp2.tick(hp, lp, bp, hp);
    s = v.m_lp.lp(hp);
    const float k = 1.0f + 4.0f * m_drive;
    s = std::tanh(k * s) / k * (1.0f + m_drive);
    if (v.m_fading) {
        v.m_fade -= static_cast<float>(1.0 / (0.010 * m_sr));
        if (v.m_fade <= 0.0f) { v.m_fade = 0.0f; v.m_on = false; }
    }
    // The throw weight follows its target over 10 ms, so the send never clicks.
    v.m_throwW += (target - v.m_throwW) * static_cast<float>(1.0 - std::exp(-1.0 / (0.010 * m_sr)));
    throwW = v.m_throwW * v.m_fade * v.m_velocity;
    ++v.m_pos;
    if (v.m_pos >= v.m_length + static_cast<long long>(0.05 * m_sr)) v.m_on = false;
    return s * v.m_fade * v.m_velocity;
}

void Vocal::process(float* L, float* R, float* throwOut, int n)
{
    for (int i = 0; i < n; ++i) { L[i] = 0.0f; R[i] = 0.0f; throwOut[i] = 0.0f; }
    for (Voice& v : m_voice) {
        if (!v.m_on) continue;
        const double theta = (static_cast<double>(clampv(v.m_pan, -1.0f, 1.0f)) + 1.0) * PiD / 4.0;
        const float gl = static_cast<float>(std::cos(theta) * 1.41421356), gr = static_cast<float>(std::sin(theta) * 1.41421356);
        for (int i = 0; i < n && v.m_on; ++i) {
            float w = 0.0f;
            const float s = voiceSample(v, w);
            L[i] += s * gl;
            R[i] += s * gr;
            throwOut[i] += w;
        }
    }
}

} // namespace phos
