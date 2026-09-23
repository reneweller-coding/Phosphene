/**
 * @file Texture.cpp
 * @brief The shamanic bed (Texture.h).
 */
#include "phos/Texture.h"
#include "phos/Params.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {
constexpr double kPiD = 3.141592653589793;

/**
 * @name Levels of the bed against texture.level (calibrated 19.09.2026)
 * Measured on the listening seed with the master's dynamics off, as the loudest 400 ms of the texture
 * strip against the rest of the mix in the same window (K-weighted): before these constants the bowls
 * stood 11 dB and the drones 13 dB *over* the rest of an intro or breakdown; with these constants the
 * bed measures about 9 dB under it. The bed should be felt rather than heard (docs/PLAN.md, round
 * "fx-psychedelia").
 * @{ */
constexpr float kBowlGain  = 0.08f;   ///< -22 dB
constexpr float kDroneGain = 0.064f;  ///< -24 dB
/** @} */

/** @brief Rayleigh's thin-ring bending frequency for circumferential order n (unnormalised). */
double ringMode(int n)
{
    const double nn = static_cast<double>(n);
    return nn * (nn * nn - 1.0) / std::sqrt(nn * nn + 1.0);
}

/** @brief Additive harmonic source: sum_k w[k-1] sin(2 pi k phase). */
float harmonics(double phase, const float* w, int count)
{
    float s = 0.0f;
    for (int k = 1; k <= count; ++k) {
        const double p = phase * k;
        s += w[k - 1] * sin01(p - std::floor(p));
    }
    return s;
}
} // namespace

double Texture::bowlRatio(int mode) { return ringMode(mode + 2) / ringMode(2); }

void Texture::prepare(double sampleRate)
{
    sr_ = sampleRate;
    reset();
}

void Texture::reset()
{
    for (int i = 0; i < kVoices; ++i) {
        voice_[i] = Voice{};
        voice_[i].rng.seed(0x5445585400ull + static_cast<uint64_t>(i));
    }
    counter_ = 0;
}

void Texture::update(const float* v, int keyRoot)
{
    keyRoot_ = keyRoot;
    width_ = v[texture::Width];
    bowlDecay_ = v[texture::BowlDecay];
    bowlBright_ = v[texture::BowlBright];
    didgeFormant_ = v[texture::DidgeFormant];
    didgeBreath_ = v[texture::DidgeBreath];
    jawSweep_ = v[texture::JawSweep];
}

int Texture::active() const
{
    int n = 0;
    for (const Voice& v : voice_) n += v.on ? 1 : 0;
    return n;
}

void Texture::trigger(SfxType type, int samples, float velocity, double late, double samplesPerBeat, uint64_t pick)
{
    if (type != SfxType::Bowl && type != SfxType::Didgeridoo && type != SfxType::JawHarp) return;
    // The same instrument restarts in its own voice (a drone does not stack on itself); otherwise the
    // free voice, or the oldest.
    int slot = -1;
    for (int i = 0; i < kVoices && slot < 0; ++i) if (voice_[i].on && voice_[i].type == type && type != SfxType::Bowl) slot = i;
    if (slot < 0) {
        slot = 0;
        for (int i = 0; i < kVoices; ++i) {
            if (!voice_[i].on) { slot = i; break; }
            if (voice_[i].age < voice_[slot].age) slot = i;
        }
    }
    Voice& v = voice_[slot];
    v = Voice{};
    v.rng.seed(pick);
    v.type = type;
    v.on = true;
    v.late = late;
    v.velocity = clampv(velocity, 0.0f, 1.0f);
    v.age = ++counter_;
    v.spb = std::max(1.0, samplesPerBeat);
    const float sr = static_cast<float>(sr_);
    const int pc = ((keyRoot_ % 12) + 12) % 12;
    v.panPh = v.rng.uniform();
    if (type == SfxType::Bowl) {
        // A bowl in the key: its fundamental on the root in the octave from C4, one time in three an
        // octave higher (a small bowl). It rings out its own decay whatever the event's length.
        const double f0 = midiToHz(60 + pc + (v.rng.below(3) == 0 ? 12 : 0));
        const double t60 = bowlDecay_;
        v.length = static_cast<long long>(t60 * sr_);
        const double bright = 0.5 + static_cast<double>(bowlBright_);
        static const double kAmp[kModes] = { 1.0, 0.55, 0.30, 0.18 };
        for (int m = 0; m < kModes; ++m) {
            const double fm = f0 * bowlRatio(m);
            const double split = 0.002 + 0.002 * v.rng.uniform();   // the doublet: 0.2 .. 0.4 %
            const double t60m = t60 / std::pow(bowlRatio(m), 0.6);
            const double r = std::exp(-6.9 / (t60m * sr_));
            for (int d = 0; d < 2; ++d) {
                const int k = 2 * m + d;
                const double f = fm * (1.0 + (d == 0 ? -0.5 : 0.5) * split);
                const bool audible = f < 0.45 * sr_;
                const double w = 2.0 * kPiD * f / sr_;
                v.c[k] = r * std::cos(w);
                v.s[k] = r * std::sin(w);
                v.re[k] = 0.0;
                v.im[k] = 1.0;
                v.amp[k] = audible ? 0.5 * kAmp[m] * (m == 0 ? 1.0 : std::pow(bright, static_cast<double>(m))) : 0.0;
            }
        }
    } else {
        v.length = std::max<long long>(1, samples);
        // Didgeridoo on the root from C2 (65 .. 123 Hz), jaw harp a fifth above that octave.
        v.f0 = midiToHz((type == SfxType::Didgeridoo ? 36 : 43) + pc);
        // The harmonic weights: the didgeridoo's odd harmonics strong (a tube closed at the lips) and
        // the whole falling as k^-0.7 up to 5 kHz; the jaw harp's buzz brighter, k^-0.4 up to 6 kHz.
        const bool didge = type == SfxType::Didgeridoo;
        v.harmonics = std::min(Voice::kMaxHarmonics, static_cast<int>((didge ? 5000.0 : 6000.0) / v.f0));
        for (int k = 1; k <= v.harmonics; ++k)
            v.hw[k - 1] = static_cast<float>(didge ? std::pow(static_cast<double>(k), -0.7) * ((k & 1) ? 1.0 : 0.55)
                                                   : std::pow(static_cast<double>(k), -0.4));
        // Eight eighths per bar: beat one always, three to five more.
        uint8_t mask = 1;
        const int extra = 2 + v.rng.below(3) + (type == SfxType::JawHarp ? 1 : 0);
        for (int e = 0; e < extra; ++e) mask |= static_cast<uint8_t>(1u << (1 + v.rng.below(7)));
        v.pattern = mask;
        // The depth rule (Texture.h): 36 dB/octave, Butterworth (three sections with Q 0.518, 0.707, 1.932).
        const float hpHz = type == SfxType::Didgeridoo ? 200.0f : 300.0f;
        v.hp1.setQ(hpHz, 0.51763809f, sr);
        v.hp2.setQ(hpHz, 0.70710678f, sr);
        v.hp3.setQ(hpHz, 1.93185165f, sr);
        v.f1.setQ(type == SfxType::Didgeridoo ? 600.0f : 450.0f, 3.0f, sr);
        v.breathBp.setQ(1500.0f, 1.0f, sr);
        v.formant = 0.3f;
        v.target = 0.3f;
    }
}

void Texture::voiceSample(Voice& v, float& outL, float& outR)
{
    const float sr = static_cast<float>(sr_);
    const double posD = static_cast<double>(v.pos) + v.late;
    const double t = posD / sr_;
    const double lenS = static_cast<double>(v.length) / sr_;
    float s = 0.0f, amp = 1.0f, side = 0.0f;
    double panRate = 0.05;
    if (v.type == SfxType::Bowl) {
        // The two partials of each doublet lean to opposite sides, so the bowl's beating also moves
        // between the speakers -- the way a bowl turning in the hand sounds.
        //
        // The rotation and the two sums are two loops, not one (23.09.2026). As one loop, adding into sum[k & 1],
        // Intel's icx vectorised it wrongly at /O2: the bowl came out at 1/260 of its energy on the wrong
        // frequencies (it was right at /O1, at /Od and with /Qvec-, and under MSVC). The sums still add the
        // partials in the same order -- 0, 2, 4, 6 and 1, 3, 5, 7 -- so MSVC's result is the same to the bit.
        for (int k = 0; k < 2 * kModes; ++k) {
            const double re = v.re[k] * v.c[k] - v.im[k] * v.s[k];
            const double im = v.re[k] * v.s[k] + v.im[k] * v.c[k];
            v.re[k] = re;
            v.im[k] = im;
        }
        double sum[2] = { 0.0, 0.0 };
        for (int m = 0; m < kModes; ++m) {
            sum[0] += v.amp[2 * m] * v.im[2 * m];
            sum[1] += v.amp[2 * m + 1] * v.im[2 * m + 1];
        }
        s = static_cast<float>(sum[0] + sum[1]) * kBowlGain;
        side = static_cast<float>(sum[0] - sum[1]) * kBowlGain * 0.6f * width_;
        amp = static_cast<float>(std::min(1.0, t / 0.002));
        panRate = 0.08;
    } else {
        // Where in the bar we are, in eighths, for the accent and pluck pattern.
        const double beat = posD / v.spb;
        const int eighth = static_cast<int>(std::floor(beat * 2.0));
        if (eighth != v.lastEighth) {
            v.lastEighth = eighth;
            const bool hit = ((v.pattern >> (eighth & 7)) & 1u) != 0;
            if (hit) {
                v.pluck = 1.0f;
                v.up = !v.up;
            }
            v.target = hit ? 1.0f : 0.25f;
        }
        v.ph += v.f0 / sr_;
        v.ph -= std::floor(v.ph);
        const float fadeIn = static_cast<float>(std::min(1.0, t / (v.type == SfxType::Didgeridoo ? 1.5 : 0.3)));
        const float fadeOut = static_cast<float>(std::clamp((lenS - t) / (v.type == SfxType::Didgeridoo ? 2.0 : 0.5), 0.0, 1.0));
        if (v.type == SfxType::Didgeridoo) {
            const float src = harmonics(v.ph, v.hw, v.harmonics);
            // The vocal tract's formant follows the accents with a 40 ms lag: "doo-da-da-doo".
            v.formant += (v.target - v.formant) * static_cast<float>(1.0 - std::exp(-1.0 / (0.04 * sr_)));
            const float fc = static_cast<float>(350.0 * std::pow(2.0, 2.3 * (0.6 * didgeFormant_ + 0.4 * v.formant)));
            v.f2.setQ(fc, 4.0f, sr);
            float lp, bp, hp;
            v.f2.tick(src, lp, bp, hp);
            const float formantOut = bp * v.f2.k;
            // Circular breathing: every two bars, over the last eighth, a dip and an inhale.
            const double inBar2 = std::fmod(beat, 8.0);
            const bool breath = inBar2 >= 7.5;
            const float breathAmt = didgeBreath_;
            const float dip = breath ? 1.0f - 0.7f * breathAmt : 1.0f;
            v.breathBp.tick(v.rng.bipolar(), lp, bp, hp);
            const float inhale = breath ? breathAmt * 0.6f * bp * v.breathBp.k * static_cast<float>(std::sin(kPiD * (inBar2 - 7.5) / 0.5)) : 0.0f;
            v.accent += ((v.target > 0.5f ? 1.0f : 0.0f) - v.accent) * static_cast<float>(1.0 - std::exp(-1.0 / (0.03 * sr_)));
            s = (0.25f * src + formantOut) * dip * (0.75f + 0.25f * v.accent) * 0.35f + inhale;
            amp = fadeIn * fadeOut;
        } else {
            // Jaw harp: a bright buzz, re-plucked on the pattern and never quite silent in between.
            const float src = harmonics(v.ph, v.hw, v.harmonics);
            v.pluck *= static_cast<float>(std::exp(-1.0 / (0.35 * sr_)));
            // The mouth resonance swings to the other end of its range with every pluck, over 60 ms.
            const float hi = static_cast<float>(900.0 * std::pow(2.0, 1.5 * jawSweep_));
            const float goal = v.up ? hi : 900.0f;
            v.formant += (goal - v.formant) * static_cast<float>(1.0 - std::exp(-1.0 / (0.06 * sr_)));
            if (v.formant < 100.0f) v.formant = goal;
            v.f2.setQ(v.formant, 10.0f, sr);
            float lp, bp, hp;
            v.f1.tick(src, lp, bp, hp);
            const float a1 = bp * v.f1.k;
            v.f2.tick(src, lp, bp, hp);
            const float a2 = bp * v.f2.k;
            s = (0.1f * src + 0.5f * a1 + 1.2f * a2) * (0.15f + 0.85f * v.pluck) * 0.3f;
            amp = fadeIn * fadeOut;
        }
        // The depth rule: 36 dB/octave at 200 or 300 Hz (Texture.h).
        float lp, bp, hp;
        v.hp1.tick(s, lp, bp, hp);
        v.hp2.tick(hp, lp, bp, hp);
        v.hp3.tick(hp, lp, bp, s);
        s *= kDroneGain;
    }
    if (v.pos >= v.length) v.on = false;
    v.panPh += panRate / sr_;
    if (v.panPh >= 1.0) v.panPh -= 1.0;
    const float pan = 0.5f * width_ * static_cast<float>(std::sin(2.0 * kPiD * v.panPh));
    ++v.pos;
    const double theta = (static_cast<double>(clampv(pan, -1.0f, 1.0f)) + 1.0) * kPiD / 4.0;
    const float g = amp * v.velocity;
    outL = g * (s * static_cast<float>(std::cos(theta) * 1.41421356) + side);
    outR = g * (s * static_cast<float>(std::sin(theta) * 1.41421356) - side);
}

void Texture::process(float* L, float* R, int n)
{
    for (int i = 0; i < n; ++i) { L[i] = 0.0f; R[i] = 0.0f; }
    for (Voice& v : voice_) {
        if (!v.on) continue;
        for (int i = 0; i < n && v.on; ++i) {
            float l = 0.0f, r = 0.0f;
            voiceSample(v, l, r);
            L[i] += l;
            R[i] += r;
        }
    }
}

} // namespace phos
