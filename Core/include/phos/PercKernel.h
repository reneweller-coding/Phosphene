/**
 * @file PercKernel.h
 * @brief The percussion voice, written once as a lane template: twelve lanes side by side.
 *
 * Every lane of the kit runs the same arithmetic. What makes a closed hat, a clap or a conga is
 * which sources are weighted in and how the filters and envelopes are set, never a different code
 * path -- so eight lanes (AVX2) or four (NEON) are one register, and the float instantiation is the
 * bit-exact scalar reference (Tests/vectest.cpp).
 *
 * **Sources**, mixed by per-lane weights:
 *  - Tone: a sine whose frequency follows a pitch envelope and, for FM, a modulator:
 *    w(t) = w0 (1 + A e_p(t)) (1 + I e_p(t) sin(phi_m)). The carrier is a rotating phasor turned by w
 *    every sample. cos w and sin w come from their Taylor series to the w^6 and w^7 terms (error below
 *    5e-4 at the clamp |w| <= 1.2, i.e. 9 kHz at 48 kHz), and one Newton step of 1/sqrt keeps the
 *    phasor on the unit circle, so a sweeping or modulated tone neither grows nor fades by accident.
 *    Only multiplies and adds: the lanes stay bit-identical to the scalar path.
 *  - Modal: four damped rotating phasors at the mode ratios of an ideal membrane (Bessel zeros
 *    2.405, 3.832, 5.136, 5.520 -> 1, 1.593, 2.136, 2.295), a free-free bar (1, 2.756, 5.404, 8.933)
 *    or a nearly harmonic loaded membrane (Raman's tabla, 1, 2, 3, 4); Fletcher and Rossing, "The
 *    Physics of Musical Instruments", ch. 3 and 18. Each mode decays at its own rate, the higher ones
 *    faster. A strike adds energy to the phasors, so a roll sums the way a drum does.
 *  - Metal: six square waves at the TR-808 cymbal oscillator frequencies (205.3, 304.4, 369.6, 522.7,
 *    540 and 800 Hz, scaled), band-limited by PolyBLEP written branch-free with masks (Werner, Abel
 *    and Smith, "The TR-808 cymbal: a physically-informed, circuit-bendable, digital model", ICMC 2014).
 *  - Noise: white noise from a per-lane generator, with an envelope that can restart several times
 *    (the bursts of a clap) before its tail.
 *
 * **Then:** a trapezoidal state-variable filter (low, band or high pass by output weights), a
 * 24 dB/octave low cut that never goes below 150 Hz (under 140 Hz only kick and bass may play: their
 * phase lock depends on it), an algebraic-sigmoid drive, a choke gain (an open hat silenced by the
 * closed hat) and constant-power panning.
 *
 * **The auto-pan (16.09.2026).** The panning gains are no longer constants. Each lane carries a
 * rotating phasor at the tempo-synchronous rate of Perc.h, and the pair (gL, gR) is *rotated* by an
 * angle d(t) = panA * cos(phase) + panB rather than recomputed. Rotation is the right operation: a
 * constant-power pan law is the point (cos theta, sin theta) on the unit circle, so moving the
 * position is turning that point, and the sum of the two channel powers -- which every calibration
 * figure of the mix round sums -- is exactly invariant under a rotation. cos d and sin d come from
 * the same Taylor series the carrier uses, followed by one Newton step of 1/sqrt on the pair, so
 * cos^2 + sin^2 is unity to float precision and the invariance survives the arithmetic and not only
 * the algebra. At d = 0 the series yields exactly (1, 0) and the rotation is the identity in every
 * bit, which is why a kit with the auto-pan off renders bit-identically to the state before it.
 *
 * Subnormal floats are prevented by the caller's DenormalGuard, not by clamps in the loop.
 */
#pragma once
#include "phos/Vec.h"

namespace phos {

constexpr int kPercLaneSlots = 16;   ///< lane arrays hold 16 entries (12 lanes, padded to the widest register)
constexpr int kPercModes = 4;        ///< modes per lane
constexpr int kPercMetalOsc = 6;     ///< square oscillators per lane

/** @brief The evolving state of every lane (structure of arrays). */
struct PercState {
    alignas(32) float cr[kPercLaneSlots] = {};   ///< the carrier phasor's real part
    alignas(32) float ci[kPercLaneSlots] = {};   ///< the carrier phasor's imaginary part
    alignas(32) float mr[kPercLaneSlots] = {};   ///< the modulator phasor's real part
    alignas(32) float mi[kPercLaneSlots] = {};   ///< the modulator phasor's imaginary part
    alignas(32) float envA[kPercLaneSlots] = {};                          ///< tone and metal amplitude
    alignas(32) float envP[kPercLaneSlots] = {};                          ///< pitch and FM-index envelope
    alignas(32) float envN[kPercLaneSlots] = {};                          ///< noise amplitude
    alignas(32) float choke[kPercLaneSlots] = {};                         ///< choke gain
    alignas(32) float ic1[kPercLaneSlots] = {};   ///< the main filter's first state
    alignas(32) float ic2[kPercLaneSlots] = {};   ///< the main filter's second state
    alignas(32) float la1[kPercLaneSlots] = {};   ///< the low cut's first section, first state
    alignas(32) float la2[kPercLaneSlots] = {};   ///< the low cut's first section, second state
    alignas(32) float lb1[kPercLaneSlots] = {};   ///< the low cut's second section, first state
    alignas(32) float lb2[kPercLaneSlots] = {};   ///< the low cut's second section, second state
    alignas(32) float zr[kPercModes][kPercLaneSlots] = {};   ///< the mode phasors' real parts
    alignas(32) float zi[kPercModes][kPercLaneSlots] = {};   ///< the mode phasors' imaginary parts
    alignas(32) float mt[kPercMetalOsc][kPercLaneSlots] = {};             ///< metal oscillator phases
    alignas(32) float pr[kPercLaneSlots] = {};   ///< the auto-pan phasor's real part
    alignas(32) float pi[kPercLaneSlots] = {};   ///< the auto-pan phasor's imaginary part (16.09.2026)
};

/** @brief Per-lane coefficients, constant within a rendered segment. */
struct PercCoefs {
    alignas(32) float w0[kPercLaneSlots] = {};       ///< carrier step at the end of the pitch envelope, radians
    alignas(32) float pAmt[kPercLaneSlots] = {};     ///< pitch envelope depth: start frequency / end - 1
    alignas(32) float dP[kPercLaneSlots] = {};       ///< pitch envelope factor per sample
    alignas(32) float fmIdx[kPercLaneSlots] = {};    ///< FM index at the start
    alignas(32) float mc[kPercLaneSlots] = {};   ///< the modulator's rotation: cosine
    alignas(32) float ms[kPercLaneSlots] = {};   ///< the modulator's rotation: sine
    alignas(32) float dA[kPercLaneSlots] = {};       ///< amplitude factor per sample
    alignas(32) float wTone[kPercLaneSlots] = {};   ///< the tone's weight in the sum
    alignas(32) float wModal[kPercLaneSlots] = {};   ///< the modes' weight
    alignas(32) float wMetal[kPercLaneSlots] = {};   ///< the metal's weight
    alignas(32) float wNoise[kPercLaneSlots] = {};   ///< the noise's weight
    alignas(32) float modeC[kPercModes][kPercLaneSlots] = {};   ///< the damped mode rotations: radius times cosine
    alignas(32) float modeS[kPercModes][kPercLaneSlots] = {};   ///< the damped mode rotations: radius times sine
    alignas(32) float a1[kPercLaneSlots] = {};   ///< the main filter's coefficient a1 (trapezoidal SVF)
    alignas(32) float a2[kPercLaneSlots] = {};   ///< ... a2
    alignas(32) float a3[kPercLaneSlots] = {};   ///< ... a3
    alignas(32) float k[kPercLaneSlots] = {};   ///< ... its damping
    alignas(32) float mLp[kPercLaneSlots] = {};   ///< the main filter's low-pass weight
    alignas(32) float mBp[kPercLaneSlots] = {};   ///< its band-pass weight
    alignas(32) float mHp[kPercLaneSlots] = {};   ///< its high-pass weight
    alignas(32) float c1[kPercLaneSlots] = {};   ///< the low cut's coefficient c1
    alignas(32) float c2[kPercLaneSlots] = {};   ///< ... c2
    alignas(32) float c3[kPercLaneSlots] = {};   ///< ... c3 (low cut, damping sqrt 2)
    alignas(32) float drvG[kPercLaneSlots] = {};   ///< the drive's gain into the clip
    alignas(32) float drvN[kPercLaneSlots] = {};   ///< the gain that keeps the level after it
    alignas(32) float chokeD[kPercLaneSlots] = {};   ///< the choke's factor per sample (1: not choked)
    alignas(32) float gL[kPercLaneSlots] = {};   ///< the pan's gain, left
    alignas(32) float gR[kPercLaneSlots] = {};   ///< the pan's gain, right
    alignas(32) float dt[kPercMetalOsc][kPercLaneSlots] = {};   ///< per metal oscillator: its phase step
    alignas(32) float inv[kPercMetalOsc][kPercLaneSlots] = {};   ///< ... and the inverse, for its PolyBLEP
    /**
     * @name The auto-pan (16.09.2026)
     * panC and panS turn the lane's LFO phasor by one sample of its period; the angle the pan gains
     * are rotated by is @c panA * pr + panB, so panA is the swing and panB the standing offset that
     * puts the lane's own position (perc.pan) at the middle of the swing. Both are zero when the lane
     * does not move, and then the rotation below is the exact identity.
     * @{ */
    alignas(32) float panC[kPercLaneSlots] = {};   ///< the auto-pan rotation's cosine
    alignas(32) float panS[kPercLaneSlots] = {};   ///< its sine
    alignas(32) float panA[kPercLaneSlots] = {};   ///< the swing
    alignas(32) float panB[kPercLaneSlots] = {};   ///< the standing offset
    /** @} */
};

/**
 * @brief Renders @p n samples of the lanes [lane, lane + laneWidth<V>()).
 * @param s      states (updated)
 * @param c      coefficients
 * @param lane   first lane of the register
 * @param n      samples
 * @param noise  per sample and lane (index i * 16 + lane): white noise
 * @param reset  per sample and lane: value the noise envelope restarts at, 0 = none
 * @param dNoise per sample and lane: noise envelope factor
 * @param tone,modal,metal whether this lane group uses the source at all (decided per group of 8
 *               lanes so every vector width runs the same arithmetic on every lane)
 * @param pan    whether *any* lane of the kit moves in the panorama. It is one decision for the whole
 *               kit rather than per group of eight, so that every lane's pan phasor advances in step
 *               whatever the other lanes are set to: a phasor that stopped while its group had no
 *               moving lane would make the phase depend on the history of the knobs.
 * @param outL,outR per sample and lane outputs
 */
template <class V>
void percKernel(PercState& s, const PercCoefs& c, int lane, int n, const float* noise, const float* reset,
                const float* dNoise, bool tone, bool modal, bool metal, bool pan, float* outL, float* outR)
{
    auto at = [lane](const float* a) { return loadLanes<V>(a + lane); };
    const V zero = lanes<V>(0.0f), one = lanes<V>(1.0f), two = lanes<V>(2.0f), half = lanes<V>(0.5f), threeHalves = lanes<V>(1.5f);
    const V sqrt2 = lanes<V>(1.41421356f), sixth = lanes<V>(1.0f / 6.0f);
    const V t2 = lanes<V>(0.5f), t24 = lanes<V>(1.0f / 24.0f), t720 = lanes<V>(1.0f / 720.0f);
    const V t6 = lanes<V>(1.0f / 6.0f), t120 = lanes<V>(1.0f / 120.0f), t5040 = lanes<V>(1.0f / 5040.0f);
    const V wMax = lanes<V>(1.2f), wMin = lanes<V>(-1.2f);

    V cr = at(s.cr), ci = at(s.ci), mr = at(s.mr), mi = at(s.mi);
    V envA = at(s.envA), envP = at(s.envP), envN = at(s.envN), choke = at(s.choke);
    V ic1 = at(s.ic1), ic2 = at(s.ic2), la1 = at(s.la1), la2 = at(s.la2), lb1 = at(s.lb1), lb2 = at(s.lb2);
    V zr[kPercModes], zi[kPercModes], mC[kPercModes], mS[kPercModes];
    for (int m = 0; m < kPercModes; ++m) { zr[m] = at(s.zr[m]); zi[m] = at(s.zi[m]); mC[m] = at(c.modeC[m]); mS[m] = at(c.modeS[m]); }
    V mt[kPercMetalOsc], dt[kPercMetalOsc], iv[kPercMetalOsc];
    for (int j = 0; j < kPercMetalOsc; ++j) { mt[j] = at(s.mt[j]); dt[j] = at(c.dt[j]); iv[j] = at(c.inv[j]); }
    const V w0 = at(c.w0), pAmt = at(c.pAmt), dP = at(c.dP), fmIdx = at(c.fmIdx), mc = at(c.mc), ms = at(c.ms), dA = at(c.dA);
    const V wTone = at(c.wTone), wModal = at(c.wModal), wMetal = at(c.wMetal), wNoise = at(c.wNoise);
    const V a1 = at(c.a1), a2 = at(c.a2), a3 = at(c.a3), kk = at(c.k), mLp = at(c.mLp), mBp = at(c.mBp), mHp = at(c.mHp);
    const V c1 = at(c.c1), c2 = at(c.c2), c3 = at(c.c3), drvG = at(c.drvG), drvN = at(c.drvN), chokeD = at(c.chokeD);
    const V gL = at(c.gL), gR = at(c.gR);
    V pr = at(s.pr), pi = at(s.pi);
    const V panC = at(c.panC), panS = at(c.panS), panA = at(c.panA), panB = at(c.panB);

    for (int i = 0; i < n; ++i) {
        const int row = i * kPercLaneSlots + lane;
        V src = zero;
        if (tone) {
            // Modulator: exact rotation, renormalised.
            const V nmr = mr * mc - mi * ms, nmi = mr * ms + mi * mc;
            V fix = threeHalves - half * (nmr * nmr + nmi * nmi);
            mr = nmr * fix;
            mi = nmi * fix;
            // Carrier step: base, pitch envelope, frequency modulation; clamped.
            V w = w0 * (one + pAmt * envP) * (one + fmIdx * envP * mi);
            w = vmin(vmax(w, wMin), wMax);
            const V q = w * w;
            const V cs = one - q * (t2 - q * (t24 - q * t720));
            const V sn = w * (one - q * (t6 - q * (t120 - q * t5040)));
            const V ncr = cr * cs - ci * sn, nci = cr * sn + ci * cs;
            fix = threeHalves - half * (ncr * ncr + nci * nci);
            cr = ncr * fix;
            ci = nci * fix;
            src = src + wTone * ci * envA;
            envP = envP * dP;
        }
        if (modal) {
            V sum = zero;
            for (int m = 0; m < kPercModes; ++m) {
                const V nr = zr[m] * mC[m] - zi[m] * mS[m];
                zi[m] = zr[m] * mS[m] + zi[m] * mC[m];
                zr[m] = nr;
                sum = sum + zi[m];
            }
            src = src + wModal * sum;
        }
        if (metal) {
            V acc = zero;
            for (int j = 0; j < kPercMetalOsc; ++j) {
                const V t = mt[j], d = dt[j], inv = iv[j];
                V sq = vselect(vlt(t, half), one, -one);
                const V x1 = t * inv;
                sq = sq + vselect(vlt(t, d), x1 + x1 - x1 * x1 - one, zero);
                const V x2 = (t - one) * inv;
                sq = sq + vselect(vgt(t, one - d), x2 * x2 + x2 + x2 + one, zero);
                V u = t + half;
                u = vselect(vge(u, one), u - one, u);
                const V x3 = u * inv;
                sq = sq - vselect(vlt(u, d), x3 + x3 - x3 * x3 - one, zero);
                const V x4 = (u - one) * inv;
                sq = sq - vselect(vgt(u, one - d), x4 * x4 + x4 + x4 + one, zero);
                acc = acc + sq;
                const V nt = t + d;
                mt[j] = vselect(vge(nt, one), nt - one, nt);
            }
            src = src + wMetal * acc * sixth * envA;
        }
        const V rst = loadLanes<V>(reset + row);
        envN = vselect(vgt(rst, zero), rst, envN);
        src = src + wNoise * loadLanes<V>(noise + row) * envN;
        envN = envN * loadLanes<V>(dNoise + row);
        envA = envA * dA;

        // Main filter (trapezoidal SVF), output chosen by weights.
        V v3 = src - ic2;
        V v1 = a1 * ic1 + a2 * v3;
        V v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = two * v1 - ic1;
        ic2 = two * v2 - ic2;
        V y = mLp * v2 + mBp * kk * v1 + mHp * (src - kk * v1 - v2);
        // Low cut: two Butterworth high-pass sections.
        v3 = y - la2;
        v1 = c1 * la1 + c2 * v3;
        v2 = la2 + c2 * la1 + c3 * v3;
        la1 = two * v1 - la1;
        la2 = two * v2 - la2;
        y = y - sqrt2 * v1 - v2;
        v3 = y - lb2;
        v1 = c1 * lb1 + c2 * v3;
        v2 = lb2 + c2 * lb1 + c3 * v3;
        lb1 = two * v1 - lb1;
        lb2 = two * v2 - lb2;
        y = y - sqrt2 * v1 - v2;
        // Drive: algebraic sigmoid, unity gain for small signals.
        const V yg = y * drvG;
        y = yg / vsqrt(one + yg * yg) * drvN;
        y = y * choke;
        choke = choke * chokeD;
        // The auto-pan: turn (gL, gR) on the unit circle by d = panA * cos(phase) + panB. The phasor
        // is renormalised the same way the carrier is, and so is the (cos d, sin d) pair, which is
        // what makes the two channel powers sum to a constant to float precision.
        V gl = gL, gr = gR;
        if (pan) {
            const V npr = pr * panC - pi * panS, npi = pr * panS + pi * panC;
            V fix = threeHalves - half * (npr * npr + npi * npi);
            pr = npr * fix;
            pi = npi * fix;
            const V d = panA * pr + panB;
            const V q = d * d;
            V cs = one - q * (t2 - q * (t24 - q * t720));
            V sn = d * (one - q * (t6 - q * (t120 - q * t5040)));
            fix = threeHalves - half * (cs * cs + sn * sn);
            cs = cs * fix;
            sn = sn * fix;
            gl = gL * cs - gR * sn;
            gr = gR * cs + gL * sn;
        }
        vstore(outL + row, y * gl);
        vstore(outR + row, y * gr);
    }

    auto put = [lane](float* a, V v) { vstore(a + lane, v); };
    put(s.pr, pr); put(s.pi, pi);
    put(s.cr, cr); put(s.ci, ci); put(s.mr, mr); put(s.mi, mi);
    put(s.envA, envA); put(s.envP, envP); put(s.envN, envN); put(s.choke, choke);
    put(s.ic1, ic1); put(s.ic2, ic2); put(s.la1, la1); put(s.la2, la2); put(s.lb1, lb1); put(s.lb2, lb2);
    for (int m = 0; m < kPercModes; ++m) { put(s.zr[m], zr[m]); put(s.zi[m], zi[m]); }
    for (int j = 0; j < kPercMetalOsc; ++j) put(s.mt[j], mt[j]);
}

} // namespace phos
