/**
 * @file PolyKernel.h
 * @brief The lane kernels of the polyphonic engine: 56 unison oscillators and 16 voice filter channels.
 *
 * **Oscillator slots.** Eight voices of seven unison oscillators each are 56 slots, seven AVX2
 * registers or fourteen NEON registers. Every slot runs the same arithmetic -- a PolyBLEP sawtooth, a
 * PolyBLEP pulse and a phase-modulated sine -- mixed by per-slot weights, so a supersaw, a VA voice and
 * an FM voice differ only in their weights and frequencies:
 *  - Saw and pulse follow Oscillator.h (Valimaki and Huovilainen 2007) with the same phase convention
 *    and the inverted pulse; the two-sample residuals are written branch-free with masks, as in the
 *    percussion kernel.
 *  - The sine is sin(2 pi (t + I m / (2 pi))), with the modulator m = sin(2 pi t_m) and an index I that
 *    decays from its start towards a floor (two-operator FM in the phase-modulation form of Chowning,
 *    "The synthesis of complex audio spectra by means of frequency modulation", JAES 1973). A sine of
 *    an arbitrary phase has no single IEEE operation, so it is computed from the phase folded into a
 *    quarter period, |theta| <= pi/2, by the Taylor series to theta^11; the truncation error is below
 *    6e-8 (-144 dB). Only multiplies, adds, floors and comparisons: lanes stay bit-identical.
 *
 * **Voice channels.** Each voice has a left and a right channel, 16 lanes: a trapezoidal state-variable
 * filter with resonance -- a low pass unless poly.filter_type chooses its band-pass, high-pass or notch
 * output (19.09.2026) -- then two Butterworth high-pass sections (24 dB/octave) at the voice's
 * key-tracked high-pass frequency, then the amplitude envelope.
 */
#pragma once
#include "phos/Vec.h"

namespace phos {

constexpr int kPolyVoices = 8;                              ///< voices per engine
constexpr int kPolyUnison = 7;                              ///< oscillators per voice
constexpr int kPolySlots = kPolyVoices * kPolyUnison;       ///< 56 oscillator slots
constexpr int kPolyLanes = kPolyVoices * 2;                 ///< 16 filter channels
constexpr int kPolyBlock = 16;                              ///< longest segment with constant coefficients

/** @brief Oscillator slot state and coefficients (structure of arrays). */
struct PolySlots {
    alignas(32) float ph[kPolySlots] = {};       ///< carrier phase [0, 1)
    alignas(32) float mph[kPolySlots] = {};      ///< modulator phase [0, 1)
    alignas(32) float idx[kPolySlots] = {};      ///< decaying part of the FM index
    alignas(32) float dt[kPolySlots] = {};       ///< carrier phase step
    alignas(32) float inv[kPolySlots] = {};      ///< 1 / dt
    alignas(32) float mdt[kPolySlots] = {};      ///< modulator phase step
    alignas(32) float idxDecay[kPolySlots] = {}; ///< FM index factor per sample
    alignas(32) float idxFloor[kPolySlots] = {}; ///< FM index that remains
    alignas(32) float pw[kPolySlots] = {};       ///< pulse width
    alignas(32) float wSaw[kPolySlots] = {}, wPulse[kPolySlots] = {}, wFm[kPolySlots] = {}, wWt[kPolySlots] = {};   ///< source weights
    alignas(32) float gL[kPolySlots] = {}, gR[kPolySlots] = {};   ///< slot gain and pan
};

/** @brief Voice channel state and coefficients. */
struct PolyChannels {
    alignas(32) float ic1[kPolyLanes] = {}, ic2[kPolyLanes] = {};   ///< low pass
    alignas(32) float ha1[kPolyLanes] = {}, ha2[kPolyLanes] = {};   ///< high pass, first section
    alignas(32) float hb1[kPolyLanes] = {}, hb2[kPolyLanes] = {};   ///< high pass, second section
    alignas(32) float a1[kPolyLanes] = {}, a2[kPolyLanes] = {}, a3[kPolyLanes] = {};   ///< low-pass coefficients
    alignas(32) float c1[kPolyLanes] = {}, c2[kPolyLanes] = {}, c3[kPolyLanes] = {};   ///< high-pass coefficients (damping sqrt 2)
    /**
     * @name The filter's response (poly.filter_type, 19.09.2026)
     * The output is v2 + (m0 x + m1 v1 + m2 v2), the textbook mixing of a state-variable filter's
     * outputs (band = v1, low = v2, high = x - k band - low, notch = low + high): low pass (0, 0, 0),
     * band pass normalised to unity at the centre (0, k, -1), high pass (1, -k, -2), notch (1, -k, -1),
     * with k the damping. Zero is the low pass the kernel always had: v2 + 0 is v2.
     * @{ */
    alignas(32) float m0[kPolyLanes] = {}, m1[kPolyLanes] = {}, m2[kPolyLanes] = {};
    /** @} */
};

/** @brief sin(2 pi p) for any p, from the Taylor series on the folded phase (see the file comment). */
template <class V>
inline V laneSin01(V p)
{
    const V half = lanes<V>(0.5f), quarter = lanes<V>(0.25f);
    V q = p - vfloor(p + half);                                 // [-0.5, 0.5)
    q = vselect(vgt(q, quarter), half - q, q);                  // sin(pi - x) = sin x
    q = vselect(vlt(q, -quarter), -half - q, q);
    const V th = lanes<V>(6.28318530717958648f) * q;
    const V t2 = th * th, one = lanes<V>(1.0f);
    V s = one - t2 * lanes<V>(1.0f / 110.0f);
    s = one - t2 * lanes<V>(1.0f / 72.0f) * s;
    s = one - t2 * lanes<V>(1.0f / 42.0f) * s;
    s = one - t2 * lanes<V>(1.0f / 20.0f) * s;
    s = one - t2 * lanes<V>(1.0f / 6.0f) * s;
    return th * s;
}

/**
 * @brief Renders @p n samples of the slots [slot, slot + laneWidth<V>()).
 * @param wt        per sample and slot: the wavetable oscillator's sample, read on the scalar side
 *                  (a table read is a gather, which NEON does not have; see Poly.cpp)
 * @param blep,fm,useWt whether this group of slots weighs the source in at all. Decided per group of
 *                  eight slots, so every vector width runs the same arithmetic on every lane (as in
 *                  the percussion kernel). A source weighed in with zero contributes exactly zero, so
 *                  leaving it out changes nothing but the time it takes -- and it takes a lot: the two
 *                  folded Taylor sines of the frequency modulation are the most expensive part of a
 *                  slot, and a supersaw, a VA voice and a pad never use them.
 * @param outL,outR per sample and slot (index i * kPolySlots + slot)
 */
template <class V>
void polySlotKernel(PolySlots& s, int slot, int n, const float* wt, bool blep, bool fm, bool useWt, float* outL, float* outR)
{
    auto at = [slot](const float* a) { return loadLanes<V>(a + slot); };
    const V zero = lanes<V>(0.0f), one = lanes<V>(1.0f), two = lanes<V>(2.0f);
    const V invTwoPi = lanes<V>(0.159154943f);
    V ph = at(s.ph), mph = at(s.mph), idx = at(s.idx);
    const V dt = at(s.dt), inv = at(s.inv), mdt = at(s.mdt), idxDecay = at(s.idxDecay), idxFloor = at(s.idxFloor);
    const V pw = at(s.pw), wSaw = at(s.wSaw), wPulse = at(s.wPulse), wFm = at(s.wFm), wWt = at(s.wWt), gL = at(s.gL), gR = at(s.gR);
    auto blepAt = [&](V t) {
        const V x1 = t * inv;
        V r = vselect(vlt(t, dt), x1 + x1 - x1 * x1 - one, zero);
        const V x2 = (t - one) * inv;
        return r + vselect(vgt(t, one - dt), x2 * x2 + x2 + x2 + one, zero);
    };
    for (int i = 0; i < n; ++i) {
        const V t = ph;
        const int row = i * kPolySlots + slot;
        V sawTerm = zero, pulseTerm = zero, fmTerm = zero, wtTerm = zero;
        if (blep) {
            const V bt = blepAt(t);
            sawTerm = wSaw * (two * t - one - bt);
            V t2 = t + one - pw;
            t2 = vselect(vge(t2, one), t2 - one, t2);
            pulseTerm = wPulse * -(vselect(vlt(t, pw), one, -one) + bt - blepAt(t2));
        }
        if (fm) {
            const V m = laneSin01(mph);
            fmTerm = wFm * laneSin01(t + (idxFloor + idx) * m * invTwoPi);
        }
        if (useWt) wtTerm = wWt * loadLanes<V>(wt + row);
        const V out = sawTerm + pulseTerm + fmTerm + wtTerm;
        vstore(outL + row, out * gL);
        vstore(outR + row, out * gR);
        V np = t + dt;
        ph = vselect(vge(np, one), np - one, np);
        np = mph + mdt;
        mph = vselect(vge(np, one), np - one, np);
        idx = idx * idxDecay;
    }
    vstore(s.ph + slot, ph);
    vstore(s.mph + slot, mph);
    vstore(s.idx + slot, idx);
}

/**
 * @brief Filters @p n samples of the channels [lane, lane + laneWidth<V>()).
 * @param in   per sample and lane (index i * kPolyLanes + lane): summed oscillators
 * @param amp  per sample and lane: amplitude envelope
 * @param out  per sample and lane
 */
template <class V>
void polyChannelKernel(PolyChannels& c, int lane, int n, const float* in, const float* amp, float* out)
{
    auto at = [lane](const float* a) { return loadLanes<V>(a + lane); };
    const V two = lanes<V>(2.0f), sqrt2 = lanes<V>(1.41421356f);
    V ic1 = at(c.ic1), ic2 = at(c.ic2), ha1 = at(c.ha1), ha2 = at(c.ha2), hb1 = at(c.hb1), hb2 = at(c.hb2);
    const V a1 = at(c.a1), a2 = at(c.a2), a3 = at(c.a3), c1 = at(c.c1), c2 = at(c.c2), c3 = at(c.c3);
    const V m0 = at(c.m0), m1 = at(c.m1), m2 = at(c.m2);
    for (int i = 0; i < n; ++i) {
        const int row = i * kPolyLanes + lane;
        const V x = loadLanes<V>(in + row);
        V v3 = x - ic2;
        V v1 = a1 * ic1 + a2 * v3;
        V v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = two * v1 - ic1;
        ic2 = two * v2 - ic2;
        V y = v2 + (m0 * x + m1 * v1 + m2 * v2);   // the response (see PolyChannels::m0)
        v3 = y - ha2;
        v1 = c1 * ha1 + c2 * v3;
        v2 = ha2 + c2 * ha1 + c3 * v3;
        ha1 = two * v1 - ha1;
        ha2 = two * v2 - ha2;
        y = y - sqrt2 * v1 - v2;
        v3 = y - hb2;
        v1 = c1 * hb1 + c2 * v3;
        v2 = hb2 + c2 * hb1 + c3 * v3;
        hb1 = two * v1 - hb1;
        hb2 = two * v2 - hb2;
        y = y - sqrt2 * v1 - v2;
        vstore(out + row, y * loadLanes<V>(amp + row));
    }
    auto put = [lane](float* a, V v) { vstore(a + lane, v); };
    put(c.ic1, ic1); put(c.ic2, ic2); put(c.ha1, ha1); put(c.ha2, ha2); put(c.hb1, hb1); put(c.hb2, hb2);
}

} // namespace phos
