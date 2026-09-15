/**
 * @file Sfx.cpp
 * @brief Effect voices.
 */
#include "phos/Sfx.h"
#include "phos/Params.h"
#include <algorithm>
#include <cmath>

namespace phos {

const char* const kSfxTypeNames[kNumSfxTypes] = { "Riser", "Downlifter", "Impact", "Sweep", "Formant Shot", "Reverse Swell", "Zap" };

namespace {
constexpr double kPiD = 3.141592653589793;
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
}

int Sfx::active() const
{
    int n = 0;
    for (const Voice& v : voice_) n += v.on ? 1 : 0;
    return n;
}

void Sfx::trigger(SfxType type, int samples, float velocity, double late)
{
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
    v.length = std::max<long long>(1, static_cast<long long>(seconds * sr_));
    const float sr = static_cast<float>(sr_);
    for (Svf* f : { &v.hpL1, &v.hpL2, &v.hpR1, &v.hpR2 }) f->setQ(150.0f, 0.70710678f, sr);
    if (type == SfxType::FormantShot) {
        // Peterson and Barney formants, a (0) to u (1), as in the vocal wavetable.
        static const double kA[3] = { 730, 1090, 2440 }, kU[3] = { 300, 870, 2240 };
        for (int i = 0; i < 3; ++i) v.formant[i].setQ(static_cast<float>(kA[i] + vowel_ * (kU[i] - kA[i])), 6.0f, sr);
    }
    v.panPh = 0.25;
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
    default: break;
    }
    // Rising effects end on their beat with a short fade; the others end when they have decayed.
    if (v.pos >= v.length) {
        const bool riseToEnd = v.type == SfxType::Riser || v.type == SfxType::ReverseSwell;
        const double fade = riseToEnd ? 0.02 : 0.005;
        const double over = static_cast<double>(v.pos - v.length) / sr_;
        amp *= static_cast<float>(std::max(0.0, 1.0 - over / fade));
        if (over >= fade) v.on = false;
    }
    v.panPh += panRate / sr_;
    if (v.panPh >= 1.0) v.panPh -= 1.0;
    pan = width_ * static_cast<float>(std::sin(2.0 * kPiD * v.panPh));
    ++v.pos;
    return s * amp * v.velocity;
}

void Sfx::process(float* L, float* R, int n)
{
    for (int i = 0; i < n; ++i) { L[i] = 0.0f; R[i] = 0.0f; }
    for (Voice& v : voice_) {
        if (!v.on) continue;
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
