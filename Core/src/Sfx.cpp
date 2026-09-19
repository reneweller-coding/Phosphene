/**
 * @file Sfx.cpp
 * @brief Effect voices.
 */
#include "phos/Sfx.h"
#include "phos/Params.h"
#include <algorithm>
#include <cmath>

namespace phos {

const char* const kSfxTypeNames[kNumSfxTypes] = { "Riser", "Downlifter", "Impact", "Sweep", "Formant Shot", "Reverse Swell", "Zap",
                                                  "Squelch", "Bubble", "Stutter", "Sub Drop", "Reverse Crash",
                                                  "Formant Voice", "Alien Chatter", "Spoken Word", "Voice Chop",
                                                  "Singing Bowl", "Didgeridoo", "Jaw Harp" };

namespace {
constexpr double kPiD = 3.141592653589793;

/**
 * @brief Level of each type relative to sfx.level, in dB (18.09.2026, round "mix-foundation").
 *
 * The seven types come out of their synthesis at very different levels -- a narrow band pass over
 * noise has a fraction of the power of a broad one -- and until this round they all shared one gain.
 * Measured on the listening seed with the master's dynamics off, as the momentary (400 ms) loudness
 * of the SFX strip at an event's loudest moment against the mix's in the same window: impacts peaked
 * 14.9 dB under the mix, the riser 9.4 and sweeps 8.9, while a downlifter reached -4.7 -- and with
 * sfx.level at -12 dB. sfx.level is now -3 dB, and this table takes the types back apart.
 * The table evens that out towards two targets: the markers of a transition (riser, impact, formant
 * shot, downlifter) about 4 to 6 dB under the mix at their peak, which is prominent without covering
 * the drop, and the short ear candy (sweeps, swells, zaps) about 8 to 10 dB under it, heard as a
 * detail. The measurement and its targets are in docs/PLAN.md (round "mix-foundation").
 */
constexpr float kTypeGainDb[] = {
    +0.0f,    // Riser (19.09.2026: -2 -> 0 dB; it now arrives on beat 4 of the pre-drop break, over the loudest bar of the roll)
    -8.0f,    // Downlifter
    +3.0f,    // Impact
    -7.0f,    // Sweep
    +1.0f,    // Formant shot
    -2.0f,    // Reverse swell
    +2.0f,    // Zap
    // 19.09.2026, round "fx-psychedelia": short ear candy, heard as a detail like the zap (docs/PLAN.md
    // has the measurement against the mix). The types of the other two generators carry 0 here.
    +0.0f,    // Squelch
    +0.0f,    // Bubble
     0.0f,    // Stutter (the engine's buffer repeat; this generator stays silent)
     0.0f,    // Sub drop (its own level, sfx.sub_level)
    -4.0f,    // Reverse crash
     0.0f, 0.0f, 0.0f, 0.0f,   // Vocal types
     0.0f, 0.0f, 0.0f,         // Texture types
};
static_assert(sizeof(kTypeGainDb) / sizeof(kTypeGainDb[0]) == kNumSfxTypes, "one gain per effect type");
/** @brief Band-pass output of an SVF at unity gain in the centre. */
inline float bandPass(Svf& f, float x)
{
    float lp, bp, hp;
    f.tick(x, lp, bp, hp);
    return bp * f.k;
}
} // namespace

void Sfx::prepare(double sampleRate)
{
    sr_ = sampleRate;
    reset();
}

void Sfx::reset()
{
    for (int i = 0; i < kVoices; ++i) {
        voice_[i] = Voice{};
        voice_[i].rng.seed(0x5346580000ull + static_cast<uint64_t>(i));
    }
    counter_ = 0;
}

void Sfx::update(const float* v, int keyRoot)
{
    keyRoot_ = keyRoot;
    level_ = dbToGain(v[sfx::Level]);
    noise_ = v[sfx::Noise];
    resonance_ = v[sfx::Resonance];
    brightness_ = v[sfx::Brightness];
    impactDecay_ = v[sfx::ImpactDecay] * 0.001f;
    vowel_ = v[sfx::Vowel];
    swellDecay_ = v[sfx::SwellDecay] * 0.001f;
    width_ = v[sfx::Width];
    subLevel_ = dbToGain(v[sfx::SubLevel]);
}

int Sfx::active() const
{
    int n = 0;
    for (const Voice& v : voice_) n += v.on ? 1 : 0;
    return n;
}

void Sfx::trigger(SfxType type, int samples, float velocity, double late)
{
    // The stutter is the engine's, and the voices and the bed have generators of their own.
    if (type == SfxType::Stutter || sfxTypePart(type) != Part::Sfx || type >= SfxType::Count) return;
    int slot = 0;
    for (int i = 0; i < kVoices; ++i) {
        if (!voice_[i].on) { slot = i; break; }
        if (voice_[i].age < voice_[slot].age) slot = i;
    }
    Voice& v = voice_[slot];
    const Rng keep = v.rng;
    v = Voice{};
    v.rng = keep;
    v.type = type;
    v.on = true;
    v.velocity = clampv(velocity, 0.0f, 1.0f);
    v.late = late;
    v.age = ++counter_;
    double seconds = samples / sr_;
    if (type == SfxType::Impact) seconds = std::max(seconds, static_cast<double>(impactDecay_));
    if (type == SfxType::FormantShot) seconds = clampv(seconds, 0.08, 0.6);
    if (type == SfxType::Zap) seconds = std::max(seconds, 0.25);
    if (type == SfxType::Squelch) seconds = clampv(seconds, 0.06, 0.5);
    if (type == SfxType::Bubble) seconds = clampv(seconds, 0.12, 0.8);
    if (type == SfxType::SubDrop) seconds = std::max(seconds, 1.2);
    v.length = std::max<long long>(1, static_cast<long long>(seconds * sr_));
    if (type == SfxType::Bubble) {
        // Three to seven bubbles spread over the first two thirds of the event, each drawn once here
        // from the voice's own stream: onset, start frequency (1.2 .. 4 kHz), decay (12 .. 40 ms) and a
        // rise of 60 .. 160 % of the start frequency per second of its life -- small bubbles are high and
        // short, which the draw keeps by tying the decay to the frequency.
        v.bubbles = 3 + v.rng.below(5);
        for (int b = 0; b < v.bubbles; ++b) {
            const double u = v.rng.uniform();
            v.bubT[b] = b == 0 ? 0.0 : (2.0 / 3.0) * seconds * v.rng.uniform();
            v.bubF[b] = 1200.0 * std::pow(4000.0 / 1200.0, u);
            v.bubTau[b] = 0.040 - 0.028 * u;
            v.bubRise[b] = (0.6 + v.rng.uniform()) / (4.0 * v.bubTau[b]);   // over ~4 tau the pitch rises 0.6 .. 1.6 x
            v.bubPh[b] = 0.0;
        }
    }
    const float sr = static_cast<float>(sr_);
    for (Svf* f : { &v.hpL1, &v.hpL2, &v.hpR1, &v.hpR2 }) f->setQ(150.0f, 0.70710678f, sr);
    if (type == SfxType::FormantShot) {
        // Peterson and Barney formants, a (0) to u (1), as in the vocal wavetable.
        static const double kA[3] = { 730, 1090, 2440 }, kU[3] = { 300, 870, 2240 };
        for (int i = 0; i < 3; ++i) v.formant[i].setQ(static_cast<float>(kA[i] + vowel_ * (kU[i] - kA[i])), 6.0f, sr);
    }
    v.panPh = 0.25;
    v.typeGain = dbToGain(kTypeGainDb[static_cast<int>(type)]);
}

float Sfx::voiceSample(Voice& v, float& pan)
{
    const float sr = static_cast<float>(sr_);
    const double posD = static_cast<double>(v.pos) + v.late;
    const double t = posD / sr_;
    const double x = std::min(1.0, posD / static_cast<double>(v.length));
    const double rootHz = midiToHz(60 + ((keyRoot_ % 12) + 12) % 12);   // the key's root in the octave from C4
    float s = 0.0f, amp = 0.0f;
    double panRate = 0.3;
    switch (v.type) {
    case SfxType::Riser:
    case SfxType::Downlifter: {
        const double u = v.type == SfxType::Riser ? x : 1.0 - x;
        const float fc = static_cast<float>(400.0 * std::pow(2.0, (3.6 + 0.8 * brightness_) * u));
        v.bp.set(fc, static_cast<float>(0.2 + 0.7 * resonance_ * u), sr);
        const float n = bandPass(v.bp, v.rng.bipolar());
        const double hz = rootHz * std::pow(2.0, 2.0 * u);
        v.saw1.set(hz * 1.006, sr_, 0.0f, 0.5f);
        v.saw2.set(hz * 0.994, sr_, 0.0f, 0.5f);
        v.lp.set(static_cast<float>(600.0 * std::pow(2.0, 4.0 * u)), 0.1f, sr);
        const float tone = v.lp.lp(0.5f * (v.saw1.next() + v.saw2.next()));
        s = noise_ * n + (1.0f - noise_) * tone;
        amp = static_cast<float>(u * u);
        if (v.type == SfxType::Downlifter) amp *= static_cast<float>(std::min(1.0, t / 0.005));
        panRate = 0.5 + 7.5 * u * u;
        break;
    }
    case SfxType::Impact: {
        v.lp.set(static_cast<float>(500.0 + 6000.0 * std::exp(-t / 0.35)), 0.1f, sr);
        const float n = v.lp.lp(v.rng.bipolar()) * static_cast<float>(std::exp(-6.9 * t / impactDecay_));
        const double f = 160.0 + 260.0 * std::exp(-t / 0.04);
        v.sinePh += f / sr_;
        const float thump = static_cast<float>(std::sin(2.0 * kPiD * v.sinePh) * 0.9 * std::exp(-t / 0.25));
        s = 0.6f * n + thump;
        amp = static_cast<float>(std::min(1.0, t / 0.001));
        panRate = 0.0;
        break;
    }
    case SfxType::Sweep: {
        const float fc = static_cast<float>(300.0 * std::pow(2.0, 5.0 * std::sin(kPiD * x)));
        v.bp.set(fc, static_cast<float>(0.6 + 0.35 * resonance_), sr);
        s = bandPass(v.bp, v.rng.bipolar());
        amp = static_cast<float>(std::sqrt(std::max(0.0, std::sin(kPiD * x))));
        panRate = 0.25;
        break;
    }
    case SfxType::FormantShot: {
        v.saw1.set(2.0 * rootHz * std::pow(2.0, -5.0 * x / 12.0), sr_, 0.0f, 0.5f);
        const float src = v.saw1.next();
        s = bandPass(v.formant[0], src) + 0.5f * bandPass(v.formant[1], src) + 0.25f * bandPass(v.formant[2], src);
        const double lenS = static_cast<double>(v.length) / sr_;
        amp = static_cast<float>(std::min(1.0, t / 0.003) * std::exp(-t / (0.3 * lenS)));
        panRate = 0.0;
        break;
    }
    case SfxType::ReverseSwell: {
        const double lenS = static_cast<double>(v.length) / sr_;
        v.lp.set(static_cast<float>(2500.0 + 5000.0 * brightness_), 0.1f, sr);
        const float n = v.lp.lp(v.rng.bipolar());
        float chord = 0.0f;
        static const int kInterval[3] = { 0, 7, 12 };
        for (int i = 0; i < 3; ++i) {
            v.chordPh[i] += rootHz * std::pow(2.0, kInterval[i] / 12.0) / sr_;
            if (v.chordPh[i] >= 1.0) v.chordPh[i] -= 1.0;
            chord += static_cast<float>(std::sin(2.0 * kPiD * v.chordPh[i]));
        }
        s = n + 0.12f * chord;
        amp = static_cast<float>(std::exp(-6.9 * (lenS - t) / std::max(0.05f, swellDecay_)));
        panRate = 0.15;
        break;
    }
    case SfxType::Zap: {
        const double f = 200.0 + 2800.0 * std::exp(-t / 0.02);
        v.sinePh += f / sr_;
        s = static_cast<float>(std::sin(2.0 * kPiD * v.sinePh));
        amp = static_cast<float>(std::exp(-t / 0.08));
        panRate = 0.0;
        break;
    }
    case SfxType::Squelch: {
        // A saw two octaves over the key's root (a bright, harmonic-rich source) through a resonant band
        // pass that sweeps from 3 kHz up to 3 * 2^1.3 = 7.4 kHz and back on a half sine: the squelch is
        // the resonance, so the resonance is what has to sit in the 3 .. 8 kHz pocket (a sweep from
        // 1.4 kHz measured 29 % of the power there, the saw's fundamental taking the rest). A small
        // upward pitch blip in the first 30 ms makes it liquid rather than a filter sweep.
        const double hz = 4.0 * rootHz * (1.0 + 0.5 * std::exp(-t / 0.03));
        v.saw1.set(hz, sr_, 0.0f, 0.5f);
        const double sweep = std::sin(kPiD * x);
        v.bp.setQ(static_cast<float>(3000.0 * std::pow(2.0, 1.3 * sweep)), static_cast<float>(4.0 + 6.0 * resonance_), sr);
        s = bandPass(v.bp, v.saw1.next());
        amp = static_cast<float>(std::min(1.0, t / 0.002) * std::exp(-2.0 * x));
        panRate = 0.0;
        break;
    }
    case SfxType::Bubble: {
        // van den Doel's bubble: a decaying sinusoid whose frequency rises, f(t) = f0 (1 + rise t).
        s = 0.0f;
        for (int b = 0; b < v.bubbles; ++b) {
            const double tb = t - v.bubT[b];
            if (tb < 0.0 || tb > 6.0 * v.bubTau[b]) continue;
            v.bubPh[b] += v.bubF[b] * (1.0 + v.bubRise[b] * tb) / sr_;
            if (v.bubPh[b] >= 1.0) v.bubPh[b] -= 1.0;
            s += static_cast<float>(std::sin(2.0 * kPiD * v.bubPh[b]) * std::min(1.0, tb / 0.001) * std::exp(-tb / v.bubTau[b]));
        }
        amp = 0.6f;
        panRate = 2.0;
        break;
    }
    case SfxType::SubDrop: {
        // A sine falling from 110 Hz to 32 Hz with a time constant of 0.3 s; its phase is integrated, so
        // the glide never steps. It is the effect that lives where the kick lives -- processSplit sends
        // it to the engine on its own output, which ducks it under every kick (Engine.cpp).
        const double f = 32.0 + 78.0 * std::exp(-t / 0.3);
        v.sinePh += f / sr_;
        if (v.sinePh >= 1.0) v.sinePh -= 1.0;
        s = static_cast<float>(std::sin(2.0 * kPiD * v.sinePh));
        const double lenS = static_cast<double>(v.length) / sr_;
        amp = static_cast<float>(std::min(1.0, t / 0.005) * std::exp(-2.5 * t / lenS));
        panRate = 0.0;
        break;
    }
    case SfxType::ReverseCrash: {
        // A cymbal wash: noise above 5 kHz, ring-modulated by two inharmonic square waves (the metallic
        // partials), under an envelope that rises exponentially -- a reversed crash ends on its beat.
        v.bp.setQ(7000.0f, 0.8f, sr);
        const float n = bandPass(v.bp, v.rng.bipolar());
        v.chordPh[0] += 540.0 / sr_; if (v.chordPh[0] >= 1.0) v.chordPh[0] -= 1.0;
        v.chordPh[1] += 1173.0 / sr_; if (v.chordPh[1] >= 1.0) v.chordPh[1] -= 1.0;
        const float sq = (v.chordPh[0] < 0.5 ? 1.0f : -1.0f) * (v.chordPh[1] < 0.5 ? 1.0f : -1.0f);
        v.lp.set(10000.0f, 0.1f, sr);
        s = v.lp.lp(n * (0.6f + 0.4f * sq));
        amp = static_cast<float>(std::exp(-5.0 * (1.0 - x)));
        panRate = 0.2;
        break;
    }
    default: break;
    }
    // Rising effects end on their beat with a short fade; the others end when they have decayed.
    if (v.pos >= v.length) {
        const bool riseToEnd = v.type == SfxType::Riser || v.type == SfxType::ReverseSwell || v.type == SfxType::ReverseCrash;
        const double fade = riseToEnd ? 0.02 : 0.005;
        const double over = static_cast<double>(v.pos - v.length) / sr_;
        amp *= static_cast<float>(std::max(0.0, 1.0 - over / fade));
        if (over >= fade) v.on = false;
    }
    v.panPh += panRate / sr_;
    if (v.panPh >= 1.0) v.panPh -= 1.0;
    pan = width_ * static_cast<float>(std::sin(2.0 * kPiD * v.panPh));
    ++v.pos;
    return s * amp * v.velocity * v.typeGain;
}

void Sfx::process(float* L, float* R, int n)
{
    float sub[64];
    for (int done = 0; done < n; done += 64) {
        const int m = std::min(64, n - done);
        processSplit(L + done, R + done, sub, m);
        for (int i = 0; i < m; ++i) { L[done + i] += sub[i]; R[done + i] += sub[i]; }
    }
}

void Sfx::processSplit(float* L, float* R, float* sub, int n)
{
    for (int i = 0; i < n; ++i) { L[i] = 0.0f; R[i] = 0.0f; sub[i] = 0.0f; }
    for (Voice& v : voice_) {
        if (!v.on) continue;
        if (v.type == SfxType::SubDrop) {
            // Mono and unfiltered: the engine puts it in the centre with kick and bass.
            for (int i = 0; i < n && v.on; ++i) {
                float pan = 0.0f;
                sub[i] += voiceSample(v, pan) * level_ * subLevel_;
            }
            continue;
        }
        for (int i = 0; i < n && v.on; ++i) {
            float pan = 0.0f;
            const float s = voiceSample(v, pan) * level_;
            const double theta = (static_cast<double>(clampv(pan, -1.0f, 1.0f)) + 1.0) * kPiD / 4.0;
            float l = s * static_cast<float>(std::cos(theta) * 1.41421356);
            float r = s * static_cast<float>(std::sin(theta) * 1.41421356);
            float lp, bp, hp;
            v.hpL1.tick(l, lp, bp, hp); v.hpL2.tick(hp, lp, bp, l);
            v.hpR1.tick(r, lp, bp, hp); v.hpR2.tick(hp, lp, bp, r);
            L[i] += l;
            R[i] += r;
        }
    }
}

} // namespace phos
