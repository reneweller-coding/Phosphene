/**
 * @file Bass.cpp
 * @brief Bass implementation.
 */
#include "phos/Bass.h"
#include "phos/Params.h"
#include <cmath>

namespace phos {

namespace {
/** @brief The decimator for every 2x stage in the core: 96 dB stopband, passband to 0.2 of the high rate. */
const HalfbandDesign& bassHalfband()
{
    static const HalfbandDesign d = designHalfband(96.0, 0.1);
    return d;
}
constexpr float kResonanceMax = 3.9f;   ///< feedback at Resonance 1: just short of self-oscillation
} // namespace

void Bass::prepare(double sampleRate)
{
    sr_ = sampleRate;
    amp_.setSampleRate(sr_);
    ducker_.prepare(sr_);
    down_.setup(bassHalfband());
    reset();
}

void Bass::reset()
{
    ladder_.reset();
    down_.reset();
    amp_.kill();
    ducker_.reset();
    osc_ = VaOscillator{};
    gate_ = 0;
    fenv_ = 0.0f;
}

void Bass::update(const ParamStore& p, int base)
{
    wave_ = p.get(base + bass::Wave);
    pw_ = p.get(base + bass::PulseWidth);
    sub_ = p.get(base + bass::Sub);
    retrigger_ = p.getBool(base + bass::Retrigger);
    startPhase_ = p.get(base + bass::StartPhase);
    cutoff_ = p.get(base + bass::Cutoff);
    k_ = kResonanceMax * p.get(base + bass::Resonance);
    envOct_ = p.get(base + bass::EnvAmount);
    keyTrack_ = p.get(base + bass::KeyTrack);
    velCut_ = p.get(base + bass::VelToCutoff);
    fDecay_ = static_cast<float>(std::exp(std::log(1.0e-3) / (p.get(base + bass::FilterDecay) * 0.001 * sr_)));
    const float drive = p.get(base + bass::Drive);
    driveIn_ = 0.5f + 3.5f * drive;
    driveOut_ = 1.0f / (0.5f + 1.5f * drive);
    amp_.setTimes(p.get(base + bass::AmpAttack) * 0.001f, p.get(base + bass::AmpDecay) * 0.001f,
                  p.get(base + bass::AmpSustain), p.get(base + bass::AmpRelease) * 0.001f);
    ducker_.set(p.get(base + bass::DuckDepth), 1.0f, p.get(base + bass::DuckHold), p.get(base + bass::DuckRelease));
    level_ = dbToGain(p.get(base + bass::Level));
}

void Bass::noteOn(int pitch, float velocity, int gateSamples)
{
    pitch_ = pitch;
    velocity_ = clampv(velocity, 0.0f, 1.0f);
    gate_ = std::max(1, gateSamples);
    osc_.set(midiToHz(pitch_), 2.0 * sr_, wave_, pw_);
    if (retrigger_) osc_.resetPhase(startPhase_);
    fenv_ = 1.0f;
    amp_.noteOn();
}

void Bass::process(float* out, int n)
{
    const double sr2 = 2.0 * sr_;
    const float nyq = static_cast<float>(0.45 * sr2);
    const double hz = midiToHz(pitch_);
    osc_.set(hz, sr2, wave_, pw_);
    // Key tracking relative to E1 (MIDI 28), the bottom of the usual psytrance bass register.
    const float track = keyTrack_ * static_cast<float>(pitch_ - 28) / 12.0f;
    const float velScale = 1.0f - velCut_ + velCut_ * velocity_;
    const float kv = k_;
    for (int i = 0; i < n; ++i) {
        const float oct = envOct_ * fenv_ * velScale + track;
        const float fc = clampv(cutoff_ * std::pow(2.0f, oct), 20.0f, nyq);
        const float g = std::tan(kPi * fc / static_cast<float>(sr2));
        fenv_ *= fDecay_;

        float o[2];
        for (int s = 0; s < 2; ++s) {
            const float v = osc_.next();
            const float x = (v + sub_ * osc_.sub()) * driveIn_;
            o[s] = ladder_.tick(x, g, kv, 0.5f);
        }
        const float y = down_.process(o[0], o[1]) * driveOut_;

        if (gate_ > 0 && --gate_ == 0) amp_.noteOff();
        const float a = amp_.process();
        out[i] = y * a * ducker_.next() * level_;
    }
}

} // namespace phos
