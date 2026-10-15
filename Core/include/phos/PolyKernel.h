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
 * @author Rene Weller
 */
#pragma once
#include "phos/Filters.h"
#include "phos/Vec.h"

namespace phos {

constexpr int PolyVoices = 8;                              ///< voices per engine
constexpr int PolyUnison = 7;                              ///< oscillators per voice
constexpr int PolySlotCount = PolyVoices * PolyUnison;       ///< 56 oscillator slots
constexpr int PolyLanes = PolyVoices * 2;                 ///< 16 filter channels
constexpr int PolyBlock = 16;                              ///< longest segment with constant coefficients

/** @brief Oscillator slot state and coefficients (structure of arrays). */
struct PolySlots {
    alignas(32) float m_ph[PolySlotCount] = {};       ///< carrier phase [0, 1)
    alignas(32) float m_mph[PolySlotCount] = {};      ///< modulator phase [0, 1)
    alignas(32) float m_idx[PolySlotCount] = {};      ///< decaying part of the FM index
    alignas(32) float m_dt[PolySlotCount] = {};       ///< carrier phase step
    alignas(32) float m_inv[PolySlotCount] = {};      ///< 1 / dt
    alignas(32) float m_mdt[PolySlotCount] = {};      ///< modulator phase step
    alignas(32) float m_idxDecay[PolySlotCount] = {}; ///< FM index factor per sample
    alignas(32) float m_idxFloor[PolySlotCount] = {}; ///< FM index that remains
    alignas(32) float m_pw[PolySlotCount] = {};       ///< pulse width
    alignas(32) float m_wSaw[PolySlotCount] = {};   ///< the saw's weight in the source
    alignas(32) float m_wPulse[PolySlotCount] = {};   ///< the pulse's weight
    alignas(32) float m_wFm[PolySlotCount] = {};   ///< the FM's weight
    alignas(32) float m_wWt[PolySlotCount] = {};   ///< the wavetable's weight
    alignas(32) float m_gL[PolySlotCount] = {};   ///< the slot's gain and pan, left
    alignas(32) float m_gR[PolySlotCount] = {};   ///< ... right
};

/** @brief Voice channel state and coefficients. */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)   // padded to its 32-byte lanes on purpose
#endif
struct PolyChannels {
    alignas(32) float m_ic1[PolyLanes] = {};   ///< the low pass's first state
    alignas(32) float m_ic2[PolyLanes] = {};   ///< ... its second
    alignas(32) float m_ha1[PolyLanes] = {};   ///< the high pass's first section: first state
    alignas(32) float m_ha2[PolyLanes] = {};   ///< ... second state
    alignas(32) float m_hb1[PolyLanes] = {};   ///< the high pass's second section: first state
    alignas(32) float m_hb2[PolyLanes] = {};   ///< ... second state
    alignas(32) float m_a1[PolyLanes] = {};   ///< the low pass's coefficient a1
    alignas(32) float m_a2[PolyLanes] = {};   ///< ... a2
    alignas(32) float m_a3[PolyLanes] = {};   ///< ... a3
    alignas(32) float m_c1[PolyLanes] = {};   ///< the high pass's coefficient c1
    alignas(32) float m_c2[PolyLanes] = {};   ///< ... c2
    alignas(32) float m_c3[PolyLanes] = {};   ///< ... c3 (damping sqrt 2)
    /**
     * @name The filter's response (poly.filter_type, 19.09.2026)
     * The output is v2 + (m0 x + m1 v1 + m2 v2), the textbook mixing of a state-variable filter's
     * outputs (band = v1, low = v2, high = x - k band - low, notch = low + high): low pass (0, 0, 0),
     * band pass normalised to unity at the centre (0, k, -1), high pass (1, -k, -2), notch (1, -k, -1),
     * with k the damping. Zero is the low pass the kernel always had: v2 + 0 is v2.
     * @{ */
    alignas(32) float m_m0[PolyLanes] = {};   ///< the response: the weight of x
    alignas(32) float m_m1[PolyLanes] = {};   ///< ... of v1
    alignas(32) float m_m2[PolyLanes] = {};   ///< ... of v2
    /** @} */
    /**
     * @name The filter models (poly.filter_model, 26.09.2026; Filters.h)
     * `model` 0 is the state-variable filter above; 1 .. 9 are FilterModel Moog .. Wasp, run in place of it on the
     * same lanes: four node voltages and four trapezoidal states per lane, the cutoff as g = tan(pi fc / fs), the
     * resonance as the model's feedback k, and the pass band's makeup gain, all written per voice on the 16-sample grid.
     * @{ */
    int m_model = 0;                                                     ///< poly.filter_model
    alignas(32) float m_fmd[PolyLanes] = {};                            ///< poly.filter_mode per lane (SEM morph, Polivoks), modulated per voice
    alignas(32) float m_xw[5][PolyLanes] = {};                          ///< the Xpander's pole mix per lane (poleMix of the mode)
    alignas(32) float m_fv[4][PolyLanes] = {}, m_fs[4][PolyLanes] = {};  ///< node voltages and trapezoidal states
    alignas(32) float m_fg[PolyLanes] = {}, m_fk[PolyLanes] = {}, m_fmk[PolyLanes] = {};   ///< g (at twice the rate), feedback, makeup
    alignas(32) float m_fxp[PolyLanes] = {};                                               ///< the last input sample (the half-way point of 2x)
    /** @} */
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/**
 * @brief sin(2 pi p) for any p, from the Taylor series on the folded phase (see the file comment).
 * @param[in] p  the phase in turns
 * @return the sine
 */
template <class VT>
inline VT laneSin01(VT p)
{
    const VT half = lanes<VT>(0.5f), quarter = lanes<VT>(0.25f);
    VT q = p - vfloor(p + half);                                 // [-0.5, 0.5)
    q = vselect(vgt(q, quarter), half - q, q);                  // sin(pi - x) = sin x
    q = vselect(vlt(q, -quarter), -half - q, q);
    const VT th = lanes<VT>(6.28318530717958648f) * q;
    const VT t2 = th * th, one = lanes<VT>(1.0f);
    VT s = one - t2 * lanes<VT>(1.0f / 110.0f);
    s = one - t2 * lanes<VT>(1.0f / 72.0f) * s;
    s = one - t2 * lanes<VT>(1.0f / 42.0f) * s;
    s = one - t2 * lanes<VT>(1.0f / 20.0f) * s;
    s = one - t2 * lanes<VT>(1.0f / 6.0f) * s;
    return th * s;
}

/**
 * @brief Renders @p n samples of the slots [slot, slot + laneWidth\<V\>()).
 * @param[in,out] s      the slots' oscillator state
 * @param[in]     slot   the first slot of this call
 * @param[in]     n      samples to render
 * @param[in]     wt     per sample and slot: the wavetable oscillator's sample, read on the scalar side (a table read
 *                       is a gather, which NEON does not have; see Poly.cpp)
 * @param[in]     blep   whether this group of slots weighs the source in at all. Decided per group of eight slots, so
 *                       every vector width runs the same arithmetic on every lane (as in the percussion kernel). A
 *                       source weighed in with zero contributes exactly zero, so leaving it out changes nothing but the
 *                       time it takes -- and it takes a lot: the two folded Taylor sines of the frequency modulation
 *                       are the most expensive part of a slot, and a supersaw, a VA voice and a pad never use them.
 * @param[in]     fm     whether this group of slots weighs the source in at all. Decided per group of eight slots, so
 *                       every vector width runs the same arithmetic on every lane (as in the percussion kernel). A
 *                       source weighed in with zero contributes exactly zero, so leaving it out changes nothing but the
 *                       time it takes -- and it takes a lot: the two folded Taylor sines of the frequency modulation
 *                       are the most expensive part of a slot, and a supersaw, a VA voice and a pad never use them.
 * @param[in]     useWt  whether this group of slots weighs the source in at all. Decided per group of eight slots, so
 *                       every vector width runs the same arithmetic on every lane (as in the percussion kernel). A
 *                       source weighed in with zero contributes exactly zero, so leaving it out changes nothing but the
 *                       time it takes -- and it takes a lot: the two folded Taylor sines of the frequency modulation
 *                       are the most expensive part of a slot, and a supersaw, a VA voice and a pad never use them.
 * @param[in,out] outL   per sample and slot (index i * PolySlotCount + slot)
 * @param[in,out] outR   per sample and slot (index i * PolySlotCount + slot)
 */
template <class VT>
void polySlotKernel(PolySlots& s, int slot, int n, const float* wt, bool blep, bool fm, bool useWt, float* outL, float* outR)
{
    auto at = [slot](const float* a) { return loadLanes<VT>(a + slot); };
    const VT zero = lanes<VT>(0.0f), one = lanes<VT>(1.0f), two = lanes<VT>(2.0f);
    const VT invTwoPi = lanes<VT>(0.159154943f);
    VT ph = at(s.m_ph), mph = at(s.m_mph), idx = at(s.m_idx);
    const VT dt = at(s.m_dt), inv = at(s.m_inv), mdt = at(s.m_mdt), idxDecay = at(s.m_idxDecay), idxFloor = at(s.m_idxFloor);
    const VT pw = at(s.m_pw), wSaw = at(s.m_wSaw), wPulse = at(s.m_wPulse), wFm = at(s.m_wFm), wWt = at(s.m_wWt), gL = at(s.m_gL), gR = at(s.m_gR);
    auto blepAt = [&](VT t) {
        const VT x1 = t * inv;
        VT r = vselect(vlt(t, dt), x1 + x1 - x1 * x1 - one, zero);
        const VT x2 = (t - one) * inv;
        return r + vselect(vgt(t, one - dt), x2 * x2 + x2 + x2 + one, zero);
    };
    for (int i = 0; i < n; ++i) {
        const VT t = ph;
        const int row = i * PolySlotCount + slot;
        VT sawTerm = zero, pulseTerm = zero, fmTerm = zero, wtTerm = zero;
        if (blep) {
            const VT bt = blepAt(t);
            sawTerm = wSaw * (two * t - one - bt);
            VT t2 = t + one - pw;
            t2 = vselect(vge(t2, one), t2 - one, t2);
            pulseTerm = wPulse * -(vselect(vlt(t, pw), one, -one) + bt - blepAt(t2));
        }
        if (fm) {
            const VT m = laneSin01(mph);
            fmTerm = wFm * laneSin01(t + (idxFloor + idx) * m * invTwoPi);
        }
        if (useWt) wtTerm = wWt * loadLanes<VT>(wt + row);
        const VT out = sawTerm + pulseTerm + fmTerm + wtTerm;
        vstore(outL + row, out * gL);
        vstore(outR + row, out * gR);
        VT np = t + dt;
        ph = vselect(vge(np, one), np - one, np);
        np = mph + mdt;
        mph = vselect(vge(np, one), np - one, np);
        idx = idx * idxDecay;
    }
    vstore(s.m_ph + slot, ph);
    vstore(s.m_mph + slot, mph);
    vstore(s.m_idx + slot, idx);
}

/**
 * @brief Filters @p n samples of the channels [lane, lane + laneWidth\<V\>()).
 * @param[in] in   per sample and lane (index i * PolyLanes + lane): summed oscillators
 * @param[in] amp  per sample and lane: amplitude envelope
 * @param[in,out] out  per sample and lane
 * @param[in,out] c    the channels' filter and gain state
 * @param[in] lane the first channel of this call
 * @param[in] n    samples to render
 */
template <class VT>
void polyModelKernel(PolyChannels& c, int lane, int n, const float* in, const float* amp, float* out);

template <class VT>
/**
 * @brief The voices' channels [lane, lane + width): @p in through the high and low pass (or the model) times @p amp into @p
 *        out, @p n samples.
 * @param[in,out] c     the channels' state
 * @param[in]     lane  the first lane
 * @param[in]     n     the samples
 * @param[in]     in    the input samples
 * @param[in]     amp   the gains
 * @param[out]    out   the output samples
 */
void polyChannelKernel(PolyChannels& c, int lane, int n, const float* in, const float* amp, float* out)
{
    if (c.m_model != 0) { polyModelKernel<VT>(c, lane, n, in, amp, out); return; }
    auto at = [lane](const float* a) { return loadLanes<VT>(a + lane); };
    const VT two = lanes<VT>(2.0f), sqrt2 = lanes<VT>(1.41421356f);
    VT ic1 = at(c.m_ic1), ic2 = at(c.m_ic2), ha1 = at(c.m_ha1), ha2 = at(c.m_ha2), hb1 = at(c.m_hb1), hb2 = at(c.m_hb2);
    const VT a1 = at(c.m_a1), a2 = at(c.m_a2), a3 = at(c.m_a3), c1 = at(c.m_c1), c2 = at(c.m_c2), c3 = at(c.m_c3);
    const VT m0 = at(c.m_m0), m1 = at(c.m_m1), m2 = at(c.m_m2);
    for (int i = 0; i < n; ++i) {
        const int row = i * PolyLanes + lane;
        const VT x = loadLanes<VT>(in + row);
        VT v3 = x - ic2;
        VT v1 = a1 * ic1 + a2 * v3;
        VT v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = two * v1 - ic1;
        ic2 = two * v2 - ic2;
        VT y = v2 + (m0 * x + m1 * v1 + m2 * v2);   // the response (see PolyChannels::m0)
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
        vstore(out + row, y * loadLanes<VT>(amp + row));
    }
    auto put = [lane](float* a, VT v) { vstore(a + lane, v); };
    put(c.m_ic1, ic1); put(c.m_ic2, ic2); put(c.m_ha1, ha1); put(c.m_ha2, ha2); put(c.m_hb1, hb1); put(c.m_hb2, hb2);
}

/**
 * @brief polyChannelKernel with one of the filter models (PolyChannels::model > 0): the model, its makeup gain, then the
 *        same two high-pass sections and the amplitude envelope. One sample loop per model, so the switch is decided
 *        once per call and every lane of every vector path runs the same arithmetic.
 */
template <class VT>
void polyModelKernel(PolyChannels& c, int lane, int n, const float* in, const float* amp, float* out)
{
    auto at = [lane](const float* a) { return loadLanes<VT>(a + lane); };
    const VT two = lanes<VT>(2.0f), sqrt2 = lanes<VT>(1.41421356f);
    VT v[4], s[4];
    for (int j = 0; j < 4; ++j) { v[j] = at(c.m_fv[j]); s[j] = at(c.m_fs[j]); }
    VT ha1 = at(c.m_ha1), ha2 = at(c.m_ha2), hb1 = at(c.m_hb1), hb2 = at(c.m_hb2);
    const VT c1 = at(c.m_c1), c2 = at(c.m_c2), c3 = at(c.m_c3);
    const VT g = at(c.m_fg), k = at(c.m_fk), mk = at(c.m_fmk);
    VT xp = at(c.m_fxp);
    const FilterModelE m = static_cast<FilterModelE>(c.m_model - 1);
    const VT morph = at(c.m_fmd);
    // Twice the rate, as in the BerlinSchoolGenerator whose tuning the models carry (26.09.2026): at the plain rate
    // the Polivoks' slew-limited integrators and the Wasp's inverters screamed (a centroid of 9 kHz on the lead) and
    // the nonlinear stages folded their harmonics back. The input is interpolated half-way, the two outputs averaged.
    const VT half = lanes<VT>(0.5f);
    auto run = [&](auto&& filt) {
        for (int i = 0; i < n; ++i) {
            const int row = i * PolyLanes + lane;
            const VT x = loadLanes<VT>(in + row);
            const VT y1 = filt(half * (xp + x));
            const VT y2 = filt(x);
            xp = x;
            VT y = half * (y1 + y2) * mk;
            VT v3 = y - ha2;
            VT v1 = c1 * ha1 + c2 * v3;
            VT v2 = ha2 + c2 * ha1 + c3 * v3;
            ha1 = two * v1 - ha1;
            ha2 = two * v2 - ha2;
            y = y - sqrt2 * v1 - v2;
            v3 = y - hb2;
            v1 = c1 * hb1 + c2 * v3;
            v2 = hb2 + c2 * hb1 + c3 * v3;
            hb1 = two * v1 - hb1;
            hb2 = two * v2 - hb2;
            y = y - sqrt2 * v1 - v2;
            vstore(out + row, y * loadLanes<VT>(amp + row));
        }
    };
    switch (m) {
    case FilterModelE::MOOG: run([&](VT x) { return ladderMoog<VT>(v, s, x, g, k); }); break;
    case FilterModelE::PROPHET: case FilterModelE::JUNO: {
        const float d = FilterVoicing::otaDrive(m), r = FilterVoicing::otaRes(m);
        run([&](VT x) { return otaCascade<VT>(v, s, x, g, k, d, r); });
        break;
    }
    case FilterModelE::XPANDER: {
        const float d = FilterVoicing::otaDrive(m), r = FilterVoicing::otaRes(m);
        const VT w0 = at(c.m_xw[0]), w1 = at(c.m_xw[1]), w2 = at(c.m_xw[2]), w3 = at(c.m_xw[3]), w4 = at(c.m_xw[4]);
        const VT rv = lanes<VT>(r), invR = lanes<VT>(1.0f / r);
        run([&](VT x) {
            otaCascade<VT>(v, s, x, g, k, d, r);
            const VT a0 = x - k * ftanh<VT>(rv * v[3]) * invR;
            return w0 * a0 + w1 * v[0] + w2 * v[1] + w3 * v[2] + w4 * v[3];
        });
        break;
    }
    case FilterModelE::SEM: case FilterModelE::WASP: {
        const float L = FilterVoicing::svfRange(m), a = FilterVoicing::svfAsym(m);
        run([&](VT x) { return svfNonlinear<VT>(v, s, x, g, k, L, a, morph); });
        break;
    }
    case FilterModelE::POLIVOKS: {
        const float L = FilterVoicing::svfRange(m);
        run([&](VT x) { return svfNonlinear<VT>(v, s, x, g, k, L, 0.0f, morph, true); });
        break;
    }
    case FilterModelE::DIODE: {
        const VT gd = g * lanes<VT>(0.70710678f);
        run([&](VT x) { return diodeLadder<VT>(v, s, x, gd, k); });
        break;
    }
    case FilterModelE::KORG35: run([&](VT x) { return korg35<VT>(v, s, x, g, k); }); break;
    default: run([&](VT x) { return x; }); break;
    }
    for (int j = 0; j < 4; ++j) { vstore(c.m_fv[j] + lane, v[j]); vstore(c.m_fs[j] + lane, s[j]); }
    vstore(c.m_fxp + lane, xp);
    auto put = [lane](float* a, VT x) { vstore(a + lane, x); };
    put(c.m_ha1, ha1); put(c.m_ha2, ha2); put(c.m_hb1, hb1); put(c.m_hb2, hb2);
}

} // namespace phos
