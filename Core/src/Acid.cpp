/**
 * @file Acid.cpp
 * @brief Acid voice implementation.
 */
#include "phos/Acid.h"
#include "phos/Params.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {
constexpr double kPiD = 3.141592653589793;
constexpr float kResonanceMax = 16.0f;       ///< feedback at Resonance 1: below the self-oscillation at 17, as on the TB-303
constexpr float kLadderComp = 0.3f;          ///< input gain (1 + 0.3 k) against the passband loss 1/(1 + k)
constexpr double kAccentDecayMax = 0.2;      ///< an accented note's filter envelope lasts at most 200 ms
constexpr double kSweepPulseTau = 0.06;      ///< accent pulse
constexpr double kSweepTau = 0.15;           ///< accent sweep capacitor
constexpr float kSweepOctaves = 2.0f;        ///< sweep depth at full accent, resonance and charge

const HalfbandDesign& acidHalfband()
{
    static const HalfbandDesign d = designHalfband(96.0, 0.1);
    return d;
}
} // namespace

void Acid::prepare(double sampleRate)
{
    sr_ = sampleRate;
    amp_.setSampleRate(sr_);
    down_.setup(acidHalfband());
    delay_.prepare(sr_);
    size_t n = 1;
    while (static_cast<double>(n) < sr_ / 40.0 + 4.0) n <<= 1;
    comb_.assign(n, 0.0f);
    mono_.assign(256, 0.0f);
    send_.assign(256, 0.0f);
    reset();
}

void Acid::reset()
{
    osc_ = VaOscillator{};
    ladder_.reset();
    down_.reset();
    amp_.kill();
    shaper_.reset();
    lc1_.reset();
    lc2_.reset();
    delay_.reset();
    std::fill(comb_.begin(), comb_.end(), 0.0f);
    combPos_ = 0;
    pitchNow_ = pitchTarget_ = 57.0;
    hz_ = static_cast<float>(midiToHz(57.0));
    gate_ = 0;
    fenv_ = sq_ = pulse_ = sweep_ = 0.0f;
    accentGain_ = accentGainTarget_ = 1.0f;
    slidePending_ = legato_ = accent_ = false;
}

void Acid::update(const float* v, double bpm)
{
    const double sr2 = 2.0 * sr_;
    wave_ = v[acid::Wave];
    cutoff_ = v[acid::Cutoff];
    resonance_ = v[acid::Resonance];
    k_ = kResonanceMax * resonance_;
    envOct_ = v[acid::EnvAmount];
    fDecay_ = static_cast<float>(std::exp(std::log(1.0e-3) / (v[acid::Decay] * 0.001 * sr2)));
    fDecayAccent_ = static_cast<float>(std::exp(std::log(1.0e-3) / (std::min(static_cast<double>(v[acid::Decay]) * 0.001, kAccentDecayMax) * sr2)));
    accentAmt_ = v[acid::Accent];
    glide_ = static_cast<float>(1.0 - std::exp(-1.0 / (v[acid::SlideTime] * 0.001 * sr_)));
    ampDecay_ = v[acid::AmpDecay] * 0.001f;
    keyTrack_ = v[acid::KeyTrack];
    const float drive = v[acid::Drive];
    driveIn_ = 1.0f + 5.0f * drive;
    driveOut_ = 1.0f / (1.0f + 1.5f * drive);
    const bool sq = v[acid::Squelch] >= 0.5f;
    if (sq && !squelch_) std::fill(comb_.begin(), comb_.end(), 0.0f);   // no stale ring from long ago
    squelch_ = sq;
    sqOct_ = static_cast<float>(std::log2(v[acid::SquelchStart]));
    sqDecay_ = static_cast<float>(std::exp(-1.0 / (v[acid::SquelchTime] * 0.001 * sr2)));
    combMix_ = v[acid::CombMix];
    combFb_ = v[acid::CombFeedback];
    lc1_.setQ(std::max(150.0f, v[acid::LowCut]), 0.70710678f, static_cast<float>(sr_));
    lc2_.copyCoefficients(lc1_);
    pulseDecay_ = static_cast<float>(std::exp(-1.0 / (kSweepPulseTau * sr2)));
    sweepCharge_ = static_cast<float>(1.0 - std::exp(-1.0 / (kSweepTau * sr2)));
    accentSmooth_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.004 * sr_)));
    level_ = dbToGain(v[acid::Level]);
    sendAmt_ = v[acid::DelaySend];
    const int dl = std::clamp(static_cast<int>(std::lround(v[acid::DelayLeft])), 0, kNumDelayTimes - 1);
    const int dr = std::clamp(static_cast<int>(std::lround(v[acid::DelayRight])), 0, kNumDelayTimes - 1);
    delay_.set(kDelayBeats[dl], kDelayBeats[dr], bpm, v[acid::DelayFeedback], v[acid::DelayHighPass], v[acid::DelayLowPass]);
}

void Acid::noteOn(int pitch, float velocity, bool accent, bool slide, int gateSamples, double late)
{
    legato_ = slidePending_ && gate_ > 0 && amp_.isActive();
    pitchTarget_ = pitch;
    velocity_ = clampv(velocity, 0.0f, 1.0f);
    accent_ = accent;
    accentGainTarget_ = accent ? 1.0f + accentAmt_ : 1.0f;
    fDecayNote_ = accent ? fDecayAccent_ : fDecay_;
    gate_ = std::max(1, gateSamples);
    slidePending_ = slide;
    const double f0 = midiToHz(pitch);
    // Release floor: half a period (as the bass), and never under 8 ms.
    amp_.setTimes(0.0004f, ampDecay_, 0.0f, static_cast<float>(std::max(0.008, 0.5 / f0)));
    if (!legato_) {
        pitchNow_ = pitch;
        hz_ = static_cast<float>(f0);
        fenv_ = static_cast<float>(std::pow(static_cast<double>(fDecayNote_), 2.0 * late));
        sq_ = static_cast<float>(std::pow(static_cast<double>(sqDecay_), 2.0 * late));
        if (accent) pulse_ = 1.0f;
        amp_.noteOn();
        amp_.advanceAttack(late);
    } else if (accent) {
        pulse_ = 1.0f;
    }
}

void Acid::process(float* L, float* R, int total)
{
    const double sr2 = 2.0 * sr_;
    const float nyq = static_cast<float>(std::min(18000.0, 0.2 * sr2));
    const float sweepDepth = kSweepOctaves * accentAmt_ * resonance_;
    const float accentOct = accent_ ? 1.0f + accentAmt_ : 1.0f;
    const float velGain = 0.7f + 0.3f * velocity_;
    const size_t mask = comb_.size() - 1;
    int done = 0;
    while (done < total) {
        const int n = std::min(total - done, static_cast<int>(mono_.size()));
        for (int i = 0; i < n; ++i) {
            if (std::fabs(pitchTarget_ - pitchNow_) > 1.0e-6) {
                pitchNow_ += (pitchTarget_ - pitchNow_) * glide_;
                hz_ = static_cast<float>(midiToHz(pitchNow_));
            }
            osc_.set(hz_, sr2, wave_, 0.5f);
            accentGain_ += (accentGainTarget_ - accentGain_) * accentSmooth_;
            float o[2];
            for (int j = 0; j < 2; ++j) {
                float oct = envOct_ * fenv_ * accentOct + keyTrack_ * static_cast<float>(pitchNow_ - 57.0) / 12.0f + sweepDepth * sweep_;
                if (squelch_) oct += sqOct_ * sq_;
                const float fc = clampv(cutoff_ * std::pow(2.0f, oct), 20.0f, nyq);
                const float g = 1.41421356f * static_cast<float>(std::tan(kPiD * fc / sr2));
                fenv_ *= fDecayNote_;
                sq_ *= sqDecay_;
                pulse_ *= pulseDecay_;
                sweep_ += (pulse_ - sweep_) * sweepCharge_;
                o[j] = ladder_.tick(osc_.next(), g, k_, kLadderComp);
            }
            float y = down_.process(o[0], o[1]);
            if (squelch_) {
                // Feedback comb tuned to the period, read with linear interpolation.
                const double d = std::clamp(sr_ / static_cast<double>(hz_), 2.0, static_cast<double>(mask - 2));
                const double pos = static_cast<double>(combPos_) - d;
                const double fl = std::floor(pos);
                const float fr = static_cast<float>(pos - fl);
                const size_t i0 = static_cast<size_t>(static_cast<long long>(fl)) & mask;
                const float delayed = comb_[i0] + fr * (comb_[(i0 + 1) & mask] - comb_[i0]);
                const float yc = y + combFb_ * delayed;
                comb_[combPos_] = yc;
                combPos_ = (combPos_ + 1) & mask;
                y += combMix_ * ((1.0f - combFb_) * yc - y);
            }
            if (gate_ > 0 && --gate_ == 0) amp_.noteOff();
            const float a = amp_.process();
            float v = shaper_(y * a * accentGain_ * velGain * driveIn_) * driveOut_;
            float lp, bp, hp;
            lc1_.tick(v, lp, bp, hp);
            lc2_.tick(hp, lp, bp, v);
            v *= level_;
            L[done + i] = v;
            R[done + i] = v;
            send_[static_cast<size_t>(i)] = v * sendAmt_;
        }
        delay_.process(send_.data(), L + done, R + done, n);
        done += n;
    }
}

} // namespace phos
