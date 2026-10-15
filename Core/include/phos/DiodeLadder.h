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
constexpr double DiodeLadderSelfOsc = 17.0;

template <class VT>
/** @brief The four-pole diode ladder (the 303's filter), one instance per SIMD lane type @p V. */
struct DiodeLadderT {
    VT m_s[4];     ///< trapezoidal integrator states
    VT m_d[4];     ///< saturated differences of the previous sample (gain estimates)
    VT m_u;        ///< input of the previous sample (gain estimate of the input stage)

    /** @brief Clears all states. */
    void reset()
    {
        for (int i = 0; i < 4; ++i) { m_s[i] = lanes<VT>(0.0f); m_d[i] = lanes<VT>(0.0f); }
        m_u = lanes<VT>(0.0f);
    }

    /** @brief Local gain of the algebraic sigmoid: sigma(v)/v = 1/sqrt(1 + v^2). */
    static inline VT gainOf(VT v)
    {
        const VT one = lanes<VT>(1.0f);
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
    inline VT tick(VT x, VT g, VT k, VT comp)
    {
        const VT one = lanes<VT>(1.0f), half = lanes<VT>(0.5f), two = lanes<VT>(2.0f);
        const VT t0 = gainOf(m_u), t1 = gainOf(m_d[0]), t2 = gainOf(m_d[1]), t3 = gainOf(m_d[2]), t4 = gainOf(m_d[3]);
        // One-pole stages in instantaneous-response form: y = G in + S.
        const VT gt1 = g * t1, gt2 = g * t2, gt3 = g * t3, gt4 = g * t4;
        const VT e1 = one / (one + gt1), e2 = one / (one + gt2), e3 = one / (one + gt3), e4 = one / (one + gt4);
        const VT G1 = gt1 * e1, G2 = half * gt2 * e2, G3 = half * gt3 * e3, G4 = half * gt4 * e4;
        const VT S1 = m_s[0] * e1, S2 = m_s[1] * e2, S3 = m_s[2] * e3, S4 = m_s[3] * e4;
        // Nested solution, innermost first.
        const VT i34 = one / (one - G3 * G4);
        const VT g23 = G3 * i34, s23 = vfmadd(G3, S4, S3) * i34;
        const VT i23 = one / (one - G2 * g23);
        const VT g12 = G2 * i23, s12 = vfmadd(G2, s23, S2) * i23;
        const VT i12 = one / (one - G1 * g12);
        const VT g01 = G1 * i12, s01 = vfmadd(G1, s12, S1) * i12;
        const VT g24 = G4 * g23, s24 = vfmadd(G4, s23, S4);
        const VT g14 = g24 * g12, s14 = vfmadd(g24, s12, s24);
        const VT g04 = g14 * g01, s04 = vfmadd(g14, s01, s14);
        // Outer loop: u = xin - k (g04 t0 u + s04).
        const VT xin = x * vfmadd(comp, k, one);
        const VT un = (xin - k * s04) / vfmadd(k * g04, t0, one);
        const VT us = t0 * un;
        const VT y1 = vfmadd(g01, us, s01);
        const VT y2 = vfmadd(g12, y1, s12);
        const VT y3 = vfmadd(g23, y2, s23);
        const VT y4 = vfmadd(G4, y3, S4);
        m_s[0] = two * y1 - m_s[0];
        m_s[1] = two * y2 - m_s[1];
        m_s[2] = two * y3 - m_s[2];
        m_s[3] = two * y4 - m_s[3];
        m_d[0] = (us + y2) - y1;
        m_d[1] = half * (y1 + y3) - y2;
        m_d[2] = half * (y2 + y4) - y3;
        m_d[3] = half * y3 - y4;
        m_u = un;
        return y4;
    }
};

} // namespace phos
