/**
 * @file Kick.cpp
 * @brief Kick drum implementation.
 */
#include "phos/Kick.h"
#include "phos/Params.h"
#include <cmath>

namespace phos {

namespace {
/** @brief Per-sample factor that takes an exponential to -60 dB after @p seconds. */
float decayFactor(double seconds, double sr)
{
    return static_cast<float>(std::exp(std::log(1.0e-3) / (std::max(seconds, 1.0e-4) * sr)));
}
} // namespace

void Kick::prepare(double sampleRate)
{
    sr_ = sampleRate;
    fadeStep_ = static_cast<float>(1.0 / (0.003 * sr_));
    dc_.prepare(sr_, 3.0f);
    reset();
}

void Kick::reset()
{
    voice_ = Voice{};
    fade_ = Voice{};
    fadeGain_ = 0.0f;
    noise_.seed(0x4B49434Bull);
    clickFilter_.reset();
    toneFilter_.reset();
    tanh_.reset();
    hard_.reset();
    dc_.reset();
}

float Kick::tuneToKey(int keyRoot, float targetHz)
{
    // C0 = 16.35 Hz; the root and the fifth, then every octave of both, nearest in log distance.
    const double rootHz = midiToHz(12.0 + ((keyRoot % 12) + 12) % 12);
    const double candidates[2] = { rootHz, rootHz * std::pow(2.0, 7.0 / 12.0) };
    double best = targetHz, bestDist = 1e9;
    for (double c : candidates) {
        for (int oct = -2; oct <= 5; ++oct) {
            const double f = c * std::pow(2.0, oct);
            const double dist = std::fabs(std::log2(f / targetHz));
            if (dist < bestDist) { bestDist = dist; best = f; }
        }
    }
    return static_cast<float>(best);
}

void Kick::update(const ParamStore& p, int base, int keyRoot, bool /*scaleHasFifth*/)
{
    engine_ = p.getInt(base + kick::Engine);
    const float endParam = p.get(base + kick::PitchEnd);
    endHz_ = p.getInt(base + kick::Tune) == 1 ? tuneToKey(keyRoot, endParam) : endParam;
    startHz_ = std::max(p.get(base + kick::PitchStart), endHz_);
    punch_ = p.get(base + kick::Punch);
    const double tau = p.get(base + kick::PitchDecay) * 0.001;
    d1_ = std::exp(-1.0 / (tau * sr_));
    d2_ = std::exp(-1.0 / (0.3 * tau * sr_));
    attackSamples_ = std::max(1, static_cast<int>(p.get(base + kick::AmpAttack) * 0.001 * sr_));
    holdSamples_ = static_cast<int>(p.get(base + kick::AmpHold) * 0.001 * sr_);
    const double decay = p.get(base + kick::AmpDecay) * 0.001;
    ampDecay_ = decayFactor(decay, sr_);
    resDamping_ = std::exp(std::log(1.0e-3) / (std::max(decay, 1.0e-4) * sr_));
    const float drive = p.get(base + kick::Drive);
    clip_ = p.getInt(base + kick::Clip);
    drive_ = 0.2f + 7.8f * drive;
    // Normalised so a full-scale body leaves at full scale whatever the drive: tanh(g x)/tanh(g),
    // and for the hard clip only below g = 1, where nothing reaches the ceiling.
    driveNorm_ = clip_ == 0 ? 1.0f / std::tanh(drive_) : 1.0f / std::min(drive_, 1.0f);
    clickLevel_ = p.get(base + kick::ClickLevel);
    clickFilter_.setQ(p.get(base + kick::ClickTone), 0.9f, static_cast<float>(sr_));
    clickDecay_ = decayFactor(p.get(base + kick::ClickDecay) * 0.001, sr_);
    toneFilter_.setQ(p.get(base + kick::Tone), 0.7071f, static_cast<float>(sr_));
    level_ = dbToGain(p.get(base + kick::Level));
}

void Kick::trigger(float velocity)
{
    if (engine_ == 0) {
        if (voice_.active) { fade_ = voice_; fadeGain_ = 1.0f; }
        voice_ = Voice{};
    }
    // The resonant engine keeps its resonator: a new trigger adds to the ring rather than replacing it.
    Voice& v = voice_;
    v.active = true;
    v.phase = 0.0;
    v.e1 = 1.0;
    v.e2 = 1.0;
    v.t = 0;
    v.amp = 0.0f;
    v.click = 1.0f;
    v.velocity = clampv(velocity, 0.0f, 1.0f);
    if (engine_ == 1) {
        // Energy into the real part: Im(z) then rings as a sine starting at zero, with the injected
        // amplitude, on top of whatever is still ringing.
        v.zRe += v.velocity;
    }
}

float Kick::voiceSample(Voice& v)
{
    const double f = endHz_ + (startHz_ - endHz_) * ((1.0 - punch_) * v.e1 + punch_ * v.e2);
    v.e1 *= d1_;
    v.e2 *= d2_;
    float body;
    if (engine_ == 0) {
        if (v.t < attackSamples_) v.amp = static_cast<float>(v.t + 1) / static_cast<float>(attackSamples_);
        else if (v.t < attackSamples_ + holdSamples_) v.amp = 1.0f;
        else v.amp *= ampDecay_;
        body = sin01(v.phase) * v.amp;
        v.phase += f / sr_;
        if (v.phase >= 1.0) v.phase -= 1.0;
        if (v.t > attackSamples_ + holdSamples_ && v.amp < 1.0e-5f) v.active = false;
    } else {
        const double w = 2.0 * 3.141592653589793 * std::min(f, 0.45 * sr_) / sr_;
        const double c = resDamping_ * std::cos(w), s = resDamping_ * std::sin(w);
        const double re = v.zRe * c - v.zIm * s;
        v.zIm = v.zRe * s + v.zIm * c;
        v.zRe = re;
        body = static_cast<float>(v.zIm);
        if (v.t > 64 && v.zRe * v.zRe + v.zIm * v.zIm < 1.0e-12) v.active = false;
    }
    float click = 0.0f;
    if (v.click > 1.0e-5f) {
        const float n = noise_.bipolar() * v.click + (v.t == 0 ? 1.0f : 0.0f);
        float lp, bp, hp;
        clickFilter_.tick(n, lp, bp, hp);
        click = bp * clickFilter_.k * clickLevel_ * 1.5f;
        v.click *= clickDecay_;
    }
    ++v.t;
    return (body * (engine_ == 0 ? v.velocity : 1.0f)) + click * v.velocity;
}

void Kick::process(float* out, int n)
{
    for (int i = 0; i < n; ++i) {
        float x = 0.0f;
        if (voice_.active) x += voiceSample(voice_);
        if (fade_.active && fadeGain_ > 0.0f) {
            x += voiceSample(fade_) * fadeGain_;
            fadeGain_ -= fadeStep_;
            if (fadeGain_ <= 0.0f) { fadeGain_ = 0.0f; fade_.active = false; }
        }
        const float driven = x * drive_;
        float y = (clip_ == 0 ? tanh_(driven) : hard_(driven)) * driveNorm_;
        y = toneFilter_.lp(y);
        out[i] = dc_.process(y) * level_;
    }
}

} // namespace phos
