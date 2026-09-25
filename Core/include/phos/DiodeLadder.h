/**
 * @file DiodeLadder.h
 * @brief Four-pole diode ladder low pass (the acid filter), zero-delay feedback, nonlinear, as a lane
 *        template.
 *
 * **Model.** In a diode ladder the rungs are not buffered from each other as in the transistor ladder:
 * each capacitor sees its neighbours on both sides. With the stage outputs y1..y4 and u = x - k y4,
 * Zavalishin ("The Art of VA Filter Design", rev. 2.1.2, 2020, section 5.10, eq. 5.18) writes
 * @code
 *   y1' = wc ((u + y2)   - y1)
 *   y2' = wc ((y1 + y3)/2 - y2)
 *   y3' = wc ((y2 + y4)/2 - y3)
 *   y4' = wc ( y3/2      - y4)
 * @endcode
 * with the transfer function (eq. 5.29)
 * @code
 *   H(s) = 1 / (8 (1+s)^4 - 8 (1+s)^2 + 1 + k)       (s normalised by wc)
 * @endcode
 * whose denominator without k is the Chebyshev polynomial T4(1+s). Its properties, all checked in the
 * self test against the implementation: DC gain 1/(1+k); the resonance peak at wc/sqrt(2); and
 * self-oscillation from k = 17, the value that solves Im(8(1+s)^4 - 8(1+s)^2 + 1) = 0 at s = j/sqrt(2).
 * Because the peak lies at wc/sqrt(2), the cutoff given to this filter is the peak frequency, and the
 * integrator gain is g = sqrt(2) tan(pi fc / fs) (prewarped at the peak).
 *
 * **Zero-delay solution.** The coupled stages are solved from the inside out as Zavalishin shows
 * (eqs. after Fig. 5.53): stage i is y_i = G_i in_i + S_i; the innermost pair gives y3 = g23 y2 + s23,
 * then y2 = g12 y1 + s12, then y1 = g01 u + s01, and chaining these gives the ladder's instantaneous
 * response y4 = g04 u + s04, from which the outer feedback solves in closed form. Every denominator
 * is positive for any g > 0 (Zavalishin shows g04 < 1), so the loop never becomes instantaneously
 * unstable.
 *
 * **Nonlinearity.** Each stage integrates a saturated difference, y_i' = wc sigma(in_i - y_i), and the
 * input stage sees sigma(u), with the algebraic sigmoid sigma(v) = v / sqrt(1 + v^2) as in the bass
 * ladder (Ladder.h). The cheap method of Voipio (2012) replaces sigma(v) by its local gain
 * sigma(v)/v taken at the previous sample; with those gains the system is linear and the closed form
 * above still applies. At small levels the gains are 1 and the filter is exactly the linear model.
 *
 * @tparam V lane type (float or VecF)
 */
#pragma once
#include "phos/Vec.h"

namespace phos {

/** @brief Feedback at which the linear diode ladder self-oscillates (Zavalishin, section 5.10). */
constexpr double kDiodeLadderSelfOsc = 17.0;

template <class V>
/** @brief The four-pole diode ladder (the 303's filter), one instance per SIMD lane type @p V. */
struct DiodeLadderT {
    V s[4];     ///< trapezoidal integrator states
    V d[4];     ///< saturated differences of the previous sample (gain estimates)
    V u;        ///< input of the previous sample (gain estimate of the input stage)

    /** @brief Clears all states. */
    void reset()
    {
        for (int i = 0; i < 4; ++i) { s[i] = lanes<V>(0.0f); d[i] = lanes<V>(0.0f); }
        u = lanes<V>(0.0f);
    }

    /** @brief Local gain of the algebraic sigmoid: sigma(v)/v = 1/sqrt(1 + v^2). */
    static inline V gainOf(V v)
    {
        const V one = lanes<V>(1.0f);
        return one / vsqrt(vfmadd(v, v, one));
    }

    /**
     * @brief One sample.
     * @param x    input (already scaled by the drive)
     * @param g    integrator gain sqrt(2) tan(pi fc / fs), fc being the resonance peak, per lane
     * @param k    feedback; the linear filter self-oscillates at 17
     * @param comp input gain (1 + comp k), which restores part of the passband lost to the feedback
     * @return the low-pass output y4
     */
    inline V tick(V x, V g, V k, V comp)
    {
        const V one = lanes<V>(1.0f), half = lanes<V>(0.5f), two = lanes<V>(2.0f);
        const V t0 = gainOf(u), t1 = gainOf(d[0]), t2 = gainOf(d[1]), t3 = gainOf(d[2]), t4 = gainOf(d[3]);
        // One-pole stages in instantaneous-response form: y = G in + S.
        const V gt1 = g * t1, gt2 = g * t2, gt3 = g * t3, gt4 = g * t4;
        const V e1 = one / (one + gt1), e2 = one / (one + gt2), e3 = one / (one + gt3), e4 = one / (one + gt4);
        const V G1 = gt1 * e1, G2 = half * gt2 * e2, G3 = half * gt3 * e3, G4 = half * gt4 * e4;
        const V S1 = s[0] * e1, S2 = s[1] * e2, S3 = s[2] * e3, S4 = s[3] * e4;
        // Nested solution, innermost first.
        const V i34 = one / (one - G3 * G4);
        const V g23 = G3 * i34, s23 = vfmadd(G3, S4, S3) * i34;
        const V i23 = one / (one - G2 * g23);
        const V g12 = G2 * i23, s12 = vfmadd(G2, s23, S2) * i23;
        const V i12 = one / (one - G1 * g12);
        const V g01 = G1 * i12, s01 = vfmadd(G1, s12, S1) * i12;
        const V g24 = G4 * g23, s24 = vfmadd(G4, s23, S4);
        const V g14 = g24 * g12, s14 = vfmadd(g24, s12, s24);
        const V g04 = g14 * g01, s04 = vfmadd(g14, s01, s14);
        // Outer loop: u = xin - k (g04 t0 u + s04).
        const V xin = x * vfmadd(comp, k, one);
        const V un = (xin - k * s04) / vfmadd(k * g04, t0, one);
        const V us = t0 * un;
        const V y1 = vfmadd(g01, us, s01);
        const V y2 = vfmadd(g12, y1, s12);
        const V y3 = vfmadd(g23, y2, s23);
        const V y4 = vfmadd(G4, y3, S4);
        s[0] = two * y1 - s[0];
        s[1] = two * y2 - s[1];
        s[2] = two * y3 - s[2];
        s[3] = two * y4 - s[3];
        d[0] = (us + y2) - y1;
        d[1] = half * (y1 + y3) - y2;
        d[2] = half * (y2 + y4) - y3;
        d[3] = half * y3 - y4;
        u = un;
        return y4;
    }
};

} // namespace phos
