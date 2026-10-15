/**
 * @file Texture.cpp
 * @brief The shamanic bed (Texture.h).
 * @author Rene Weller
 */
#include "phos/Texture.h"
#include "phos/Params.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {
constexpr double PiD = 3.141592653589793;   ///< pi

/**
 * @name Levels of the bed against texture.level (calibrated 19.09.2026)
 * Measured on the listening seed with the master's dynamics off, as the loudest 400 ms of the texture
 * strip against the rest of the mix in the same window (K-weighted): before these constants the bowls
 * stood 11 dB and the drones 13 dB *over* the rest of an intro or breakdown; with these constants the
 * bed measures about 9 dB under it. The bed should be felt rather than heard (docs/rounds/2026-09.md, round
 * "fx-psychedelia").
 * @{ */
constexpr float BowlGain  = 0.08f;   ///< -22 dB
constexpr float DroneGain = 0.064f;  ///< -24 dB
/** @} */

/**
 * @brief Rayleigh's thin-ring bending frequency for circumferential order n (unnormalised).
 * @param[in] n  the order
 * @return the frequency, unnormalised
 */
double ringMode(int n)
{
    const double nn = static_cast<double>(n);
    return nn * (nn * nn - 1.0) / std::sqrt(nn * nn + 1.0);
}

/**
 * @brief Additive harmonic source: sum_k w[k-1] sin(2 pi k phase).
 * @param[in] phase  the phase in turns
 * @param[in] w      the harmonics' weights
 * @param[in] count  their count
 * @return the sample
 */
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
    m_sr = sampleRate;
    reset();
}

void Texture::reset()
{
    for (int i = 0; i < Voices; ++i) {
        m_voice[i] = Voice{};
        m_voice[i].m_rng.seed(0x5445585400ull + static_cast<uint64_t>(i));
    }
    m_counter = 0;
}

void Texture::update(const float* v, int keyRoot)
{
    m_keyRoot = keyRoot;
    m_width = v[texture::WIDTH];
    m_bowlDecay = v[texture::BOWL_DECAY];
    m_bowlBright = v[texture::BOWL_BRIGHT];
    m_didgeFormant = v[texture::DIDGE_FORMANT];
    m_didgeBreath = v[texture::DIDGE_BREATH];
    m_jawSweep = v[texture::JAW_SWEEP];
}

int Texture::active() const
{
    int n = 0;
    for (const Voice& v : m_voice) n += v.m_on ? 1 : 0;
    return n;
}

void Texture::trigger(SfxTypeE type, int samples, float velocity, double late, double samplesPerBeat, uint64_t pick)
{
    if (type != SfxTypeE::BOWL && type != SfxTypeE::DIDGERIDOO && type != SfxTypeE::JAW_HARP) return;
    // The same instrument restarts in its own voice (a drone does not stack on itself); otherwise the
    // free voice, or the oldest.
    int slot = -1;
    for (int i = 0; i < Voices && slot < 0; ++i) if (m_voice[i].m_on && m_voice[i].m_type == type && type != SfxTypeE::BOWL) slot = i;
    if (slot < 0) {
        slot = 0;
        for (int i = 0; i < Voices; ++i) {
            if (!m_voice[i].m_on) { slot = i; break; }
            if (m_voice[i].m_age < m_voice[slot].m_age) slot = i;
        }
    }
    Voice& v = m_voice[slot];
    v = Voice{};
    v.m_rng.seed(pick);
    v.m_type = type;
    v.m_on = true;
    v.m_late = late;
    v.m_velocity = clampv(velocity, 0.0f, 1.0f);
    v.m_age = ++m_counter;
    v.m_spb = std::max(1.0, samplesPerBeat);
    const float sr = static_cast<float>(m_sr);
    const int pc = ((m_keyRoot % 12) + 12) % 12;
    v.m_panPh = v.m_rng.uniform();
    if (type == SfxTypeE::BOWL) {
        // A bowl in the key: its fundamental on the root in the octave from C4, one time in three an
        // octave higher (a small bowl). It rings out its own decay whatever the event's length.
        const double f0 = midiToHz(60 + pc + (v.m_rng.below(3) == 0 ? 12 : 0));
        const double t60 = m_bowlDecay;
        v.m_length = static_cast<long long>(t60 * m_sr);
        const double bright = 0.5 + static_cast<double>(m_bowlBright);
        static const double Amp[Modes] = { 1.0, 0.55, 0.30, 0.18 };
        for (int m = 0; m < Modes; ++m) {
            const double fm = f0 * bowlRatio(m);
            const double split = 0.002 + 0.002 * v.m_rng.uniform();   // the doublet: 0.2 .. 0.4 %
            const double t60m = t60 / std::pow(bowlRatio(m), 0.6);
            const double r = std::exp(-6.9 / (t60m * m_sr));
            for (int d = 0; d < 2; ++d) {
                const int k = 2 * m + d;
                const double f = fm * (1.0 + (d == 0 ? -0.5 : 0.5) * split);
                const bool audible = f < 0.45 * m_sr;
                const double w = 2.0 * PiD * f / m_sr;
                v.m_c[k] = r * std::cos(w);
                v.m_s[k] = r * std::sin(w);
                v.m_re[k] = 0.0;
                v.m_im[k] = 1.0;
                v.m_amp[k] = audible ? 0.5 * Amp[m] * (m == 0 ? 1.0 : std::pow(bright, static_cast<double>(m))) : 0.0;
            }
        }
    } else {
        v.m_length = std::max<long long>(1, samples);
        // Didgeridoo on the root from C2 (65 .. 123 Hz), jaw harp a fifth above that octave.
        v.m_f0 = midiToHz((type == SfxTypeE::DIDGERIDOO ? 36 : 43) + pc);
        // The harmonic weights: the didgeridoo's odd harmonics strong (a tube closed at the lips) and
        // the whole falling as k^-0.7 up to 5 kHz; the jaw harp's buzz brighter, k^-0.4 up to 6 kHz.
        const bool didge = type == SfxTypeE::DIDGERIDOO;
        v.m_harmonics = std::min(Voice::MaxHarmonics, static_cast<int>((didge ? 5000.0 : 6000.0) / v.m_f0));
        for (int k = 1; k <= v.m_harmonics; ++k)
            v.m_hw[k - 1] = static_cast<float>(didge ? std::pow(static_cast<double>(k), -0.7) * ((k & 1) ? 1.0 : 0.55)
                                                   : std::pow(static_cast<double>(k), -0.4));
        // Eight eighths per bar: beat one always, three to five more.
        uint8_t mask = 1;
        const int extra = 2 + v.m_rng.below(3) + (type == SfxTypeE::JAW_HARP ? 1 : 0);
        for (int e = 0; e < extra; ++e) mask |= static_cast<uint8_t>(1u << (1 + v.m_rng.below(7)));
        v.m_pattern = mask;
        // The depth rule (Texture.h): 36 dB/octave, Butterworth (three sections with Q 0.518, 0.707, 1.932).
        const float hpHz = type == SfxTypeE::DIDGERIDOO ? 200.0f : 300.0f;
        v.m_hp1.setQ(hpHz, 0.51763809f, sr);
        v.m_hp2.setQ(hpHz, 0.70710678f, sr);
        v.m_hp3.setQ(hpHz, 1.93185165f, sr);
        v.m_f1.setQ(type == SfxTypeE::DIDGERIDOO ? 600.0f : 450.0f, 3.0f, sr);
        v.m_breathBp.setQ(1500.0f, 1.0f, sr);
        v.m_formant = 0.3f;
        v.m_target = 0.3f;
    }
}

void Texture::voiceSample(Voice& v, float& outL, float& outR)
{
    const float sr = static_cast<float>(m_sr);
    const double posD = static_cast<double>(v.m_pos) + v.m_late;
    const double t = posD / m_sr;
    const double lenS = static_cast<double>(v.m_length) / m_sr;
    float s = 0.0f, amp = 1.0f, side = 0.0f;
    double panRate = 0.05;
    if (v.m_type == SfxTypeE::BOWL) {
        // The two partials of each doublet lean to opposite sides, so the bowl's beating also moves
        // between the speakers -- the way a bowl turning in the hand sounds.
        //
        // The rotation and the two sums are two loops, not one (23.09.2026). As one loop, adding into sum[k & 1],
        // Intel's icx vectorised it wrongly at /O2: the bowl came out at 1/260 of its energy on the wrong
        // frequencies (it was right at /O1, at /Od and with /Qvec-, and under MSVC). The sums still add the
        // partials in the same order -- 0, 2, 4, 6 and 1, 3, 5, 7 -- so MSVC's result is the same to the bit.
        for (int k = 0; k < 2 * Modes; ++k) {
            const double re = v.m_re[k] * v.m_c[k] - v.m_im[k] * v.m_s[k];
            const double im = v.m_re[k] * v.m_s[k] + v.m_im[k] * v.m_c[k];
            v.m_re[k] = re;
            v.m_im[k] = im;
        }
        double sum[2] = { 0.0, 0.0 };
        for (int m = 0; m < Modes; ++m) {
            sum[0] += v.m_amp[2 * m] * v.m_im[2 * m];
            sum[1] += v.m_amp[2 * m + 1] * v.m_im[2 * m + 1];
        }
        s = static_cast<float>(sum[0] + sum[1]) * BowlGain;
        side = static_cast<float>(sum[0] - sum[1]) * BowlGain * 0.6f * m_width;
        amp = static_cast<float>(std::min(1.0, t / 0.002));
        panRate = 0.08;
    } else {
        // Where in the bar we are, in eighths, for the accent and pluck pattern.
        const double beat = posD / v.m_spb;
        const int eighth = static_cast<int>(std::floor(beat * 2.0));
        if (eighth != v.m_lastEighth) {
            v.m_lastEighth = eighth;
            const bool hit = ((v.m_pattern >> (eighth & 7)) & 1u) != 0;
            if (hit) {
                v.m_pluck = 1.0f;
                v.m_up = !v.m_up;
            }
            v.m_target = hit ? 1.0f : 0.25f;
        }
        v.m_ph += v.m_f0 / m_sr;
        v.m_ph -= std::floor(v.m_ph);
        const float fadeIn = static_cast<float>(std::min(1.0, t / (v.m_type == SfxTypeE::DIDGERIDOO ? 1.5 : 0.3)));
        const float fadeOut = static_cast<float>(std::clamp((lenS - t) / (v.m_type == SfxTypeE::DIDGERIDOO ? 2.0 : 0.5), 0.0, 1.0));
        if (v.m_type == SfxTypeE::DIDGERIDOO) {
            const float src = harmonics(v.m_ph, v.m_hw, v.m_harmonics);
            // The vocal tract's formant follows the accents with a 40 ms lag: "doo-da-da-doo".
            v.m_formant += (v.m_target - v.m_formant) * static_cast<float>(1.0 - std::exp(-1.0 / (0.04 * m_sr)));
            const float fc = static_cast<float>(350.0 * std::pow(2.0, 2.3 * (0.6 * m_didgeFormant + 0.4 * v.m_formant)));
            v.m_f2.setQ(fc, 4.0f, sr);
            float lp, bp, hp;
            v.m_f2.tick(src, lp, bp, hp);
            const float formantOut = bp * v.m_f2.m_k;
            // Circular breathing: every two bars, over the last eighth, a dip and an inhale.
            const double inBar2 = std::fmod(beat, 8.0);
            const bool breath = inBar2 >= 7.5;
            const float breathAmt = m_didgeBreath;
            const float dip = breath ? 1.0f - 0.7f * breathAmt : 1.0f;
            v.m_breathBp.tick(v.m_rng.bipolar(), lp, bp, hp);
            const float inhale = breath ? breathAmt * 0.6f * bp * v.m_breathBp.m_k * static_cast<float>(std::sin(PiD * (inBar2 - 7.5) / 0.5)) : 0.0f;
            v.m_accent += ((v.m_target > 0.5f ? 1.0f : 0.0f) - v.m_accent) * static_cast<float>(1.0 - std::exp(-1.0 / (0.03 * m_sr)));
            s = (0.25f * src + formantOut) * dip * (0.75f + 0.25f * v.m_accent) * 0.35f + inhale;
            amp = fadeIn * fadeOut;
        } else {
            // Jaw harp: a bright buzz, re-plucked on the pattern and never quite silent in between.
            const float src = harmonics(v.m_ph, v.m_hw, v.m_harmonics);
            v.m_pluck *= static_cast<float>(std::exp(-1.0 / (0.35 * m_sr)));
            // The mouth resonance swings to the other end of its range with every pluck, over 60 ms.
            const float hi = static_cast<float>(900.0 * std::pow(2.0, 1.5 * m_jawSweep));
            const float goal = v.m_up ? hi : 900.0f;
            v.m_formant += (goal - v.m_formant) * static_cast<float>(1.0 - std::exp(-1.0 / (0.06 * m_sr)));
            if (v.m_formant < 100.0f) v.m_formant = goal;
            v.m_f2.setQ(v.m_formant, 10.0f, sr);
            float lp, bp, hp;
            v.m_f1.tick(src, lp, bp, hp);
            const float a1 = bp * v.m_f1.m_k;
            v.m_f2.tick(src, lp, bp, hp);
            const float a2 = bp * v.m_f2.m_k;
            s = (0.1f * src + 0.5f * a1 + 1.2f * a2) * (0.15f + 0.85f * v.m_pluck) * 0.3f;
            amp = fadeIn * fadeOut;
        }
        // The depth rule: 36 dB/octave at 200 or 300 Hz (Texture.h).
        float lp, bp, hp;
        v.m_hp1.tick(s, lp, bp, hp);
        v.m_hp2.tick(hp, lp, bp, hp);
        v.m_hp3.tick(hp, lp, bp, s);
        s *= DroneGain;
    }
    if (v.m_pos >= v.m_length) v.m_on = false;
    v.m_panPh += panRate / m_sr;
    if (v.m_panPh >= 1.0) v.m_panPh -= 1.0;
    const float pan = 0.5f * m_width * static_cast<float>(std::sin(2.0 * PiD * v.m_panPh));
    ++v.m_pos;
    const double theta = (static_cast<double>(clampv(pan, -1.0f, 1.0f)) + 1.0) * PiD / 4.0;
    const float g = amp * v.m_velocity;
    outL = g * (s * static_cast<float>(std::cos(theta) * 1.41421356) + side);
    outR = g * (s * static_cast<float>(std::sin(theta) * 1.41421356) - side);
}

void Texture::process(float* L, float* R, int n)
{
    for (int i = 0; i < n; ++i) { L[i] = 0.0f; R[i] = 0.0f; }
    for (Voice& v : m_voice) {
        if (!v.m_on) continue;
        for (int i = 0; i < n && v.m_on; ++i) {
            float l = 0.0f, r = 0.0f;
            voiceSample(v, l, r);
            L[i] += l;
            R[i] += r;
        }
    }
}

} // namespace phos
