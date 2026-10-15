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
 * @author Rene Weller
 */
#pragma once
#include "phos/Vec.h"

namespace phos
{

constexpr int PercLaneSlots = 16;   ///< lane arrays hold 16 entries (12 lanes, padded to the widest register)
constexpr int PercModes = 4;        ///< modes per lane
constexpr int PercMetalOsc = 6;     ///< square oscillators per lane

/** @brief The evolving state of every lane (structure of arrays). */
struct PercState
{
    alignas( 32 ) float m_cr[PercLaneSlots] = {};                 ///< the carrier phasor's real part
    alignas( 32 ) float m_ci[PercLaneSlots] = {};                 ///< the carrier phasor's imaginary part
    alignas( 32 ) float m_mr[PercLaneSlots] = {};                 ///< the modulator phasor's real part
    alignas( 32 ) float m_mi[PercLaneSlots] = {};                 ///< the modulator phasor's imaginary part
    alignas( 32 ) float m_envA[PercLaneSlots] = {};               ///< tone and metal amplitude
    alignas( 32 ) float m_envP[PercLaneSlots] = {};               ///< pitch and FM-index envelope
    alignas( 32 ) float m_envN[PercLaneSlots] = {};               ///< noise amplitude
    alignas( 32 ) float m_choke[PercLaneSlots] = {};              ///< choke gain
    alignas( 32 ) float m_ic1[PercLaneSlots] = {};                ///< the main filter's first state
    alignas( 32 ) float m_ic2[PercLaneSlots] = {};                ///< the main filter's second state
    alignas( 32 ) float m_la1[PercLaneSlots] = {};                ///< the low cut's first section, first state
    alignas( 32 ) float m_la2[PercLaneSlots] = {};                ///< the low cut's first section, second state
    alignas( 32 ) float m_lb1[PercLaneSlots] = {};                ///< the low cut's second section, first state
    alignas( 32 ) float m_lb2[PercLaneSlots] = {};                ///< the low cut's second section, second state
    alignas( 32 ) float m_zr[PercModes][PercLaneSlots] = {};      ///< the mode phasors' real parts
    alignas( 32 ) float m_zi[PercModes][PercLaneSlots] = {};      ///< the mode phasors' imaginary parts
    alignas( 32 ) float m_mt[PercMetalOsc][PercLaneSlots] = {};   ///< metal oscillator phases
    alignas( 32 ) float m_pr[PercLaneSlots] = {};                 ///< the auto-pan phasor's real part
    alignas( 32 ) float m_pi[PercLaneSlots] = {};                 ///< the auto-pan phasor's imaginary part (16.09.2026)
};

/** @brief Per-lane coefficients, constant within a rendered segment. */
struct PercCoefs
{
    alignas( 32 ) float m_w0[PercLaneSlots] = {};       ///< carrier step at the end of the pitch envelope, radians
    alignas( 32 ) float m_pAmt[PercLaneSlots] = {};     ///< pitch envelope depth: start frequency / end - 1
    alignas( 32 ) float m_dP[PercLaneSlots] = {};       ///< pitch envelope factor per sample
    alignas( 32 ) float m_fmIdx[PercLaneSlots] = {};    ///< FM index at the start
    alignas( 32 ) float m_mc[PercLaneSlots] = {};       ///< the modulator's rotation: cosine
    alignas( 32 ) float m_ms[PercLaneSlots] = {};       ///< the modulator's rotation: sine
    alignas( 32 ) float m_dA[PercLaneSlots] = {};       ///< amplitude factor per sample
    alignas( 32 ) float m_wTone[PercLaneSlots] = {};    ///< the tone's weight in the sum
    alignas( 32 ) float m_wModal[PercLaneSlots] = {};   ///< the modes' weight
    alignas( 32 ) float m_wMetal[PercLaneSlots] = {};   ///< the metal's weight
    alignas( 32 ) float m_wNoise[PercLaneSlots] = {};   ///< the noise's weight
    alignas( 32 ) float m_modeC[PercModes][PercLaneSlots] = {};   ///< the damped mode rotations: radius times cosine
    alignas( 32 ) float m_modeS[PercModes][PercLaneSlots] = {};   ///< the damped mode rotations: radius times sine
    alignas( 32 ) float m_a1[PercLaneSlots] = {};       ///< the main filter's coefficient a1 (trapezoidal SVF)
    alignas( 32 ) float m_a2[PercLaneSlots] = {};       ///< ... a2
    alignas( 32 ) float m_a3[PercLaneSlots] = {};       ///< ... a3
    alignas( 32 ) float m_k[PercLaneSlots] = {};        ///< ... its damping
    alignas( 32 ) float m_mLp[PercLaneSlots] = {};      ///< the main filter's low-pass weight
    alignas( 32 ) float m_mBp[PercLaneSlots] = {};      ///< its band-pass weight
    alignas( 32 ) float m_mHp[PercLaneSlots] = {};      ///< its high-pass weight
    alignas( 32 ) float m_c1[PercLaneSlots] = {};       ///< the low cut's coefficient c1
    alignas( 32 ) float m_c2[PercLaneSlots] = {};       ///< ... c2
    alignas( 32 ) float m_c3[PercLaneSlots] = {};       ///< ... c3 (low cut, damping sqrt 2)
    alignas( 32 ) float m_drvG[PercLaneSlots] = {};     ///< the drive's gain into the clip
    alignas( 32 ) float m_drvN[PercLaneSlots] = {};     ///< the gain that keeps the level after it
    alignas( 32 ) float m_chokeD[PercLaneSlots] = {};   ///< the choke's factor per sample (1: not choked)
    alignas( 32 ) float m_gL[PercLaneSlots] = {};       ///< the pan's gain, left
    alignas( 32 ) float m_gR[PercLaneSlots] = {};       ///< the pan's gain, right
    alignas( 32 ) float m_dt[PercMetalOsc][PercLaneSlots] = {};    ///< per metal oscillator: its phase step
    alignas( 32 ) float m_inv[PercMetalOsc][PercLaneSlots] = {};   ///< ... and the inverse, for its PolyBLEP
    /**
     * @name The auto-pan (16.09.2026)
     * panC and panS turn the lane's LFO phasor by one sample of its period; the angle the pan gains
     * are rotated by is @c panA * pr + panB, so panA is the swing and panB the standing offset that
     * puts the lane's own position (perc.pan) at the middle of the swing. Both are zero when the lane
     * does not move, and then the rotation below is the exact identity.
     * @{ */
    alignas( 32 ) float m_panC[PercLaneSlots] = {};   ///< the auto-pan rotation's cosine
    alignas( 32 ) float m_panS[PercLaneSlots] = {};   ///< its sine
    alignas( 32 ) float m_panA[PercLaneSlots] = {};   ///< the swing
    alignas( 32 ) float m_panB[PercLaneSlots] = {};   ///< the standing offset
    /** @} */
};

/**
 * @brief Renders @p n samples of the lanes [lane, lane + laneWidth\<V\>()).
 * @param[in,out] s       states (updated)
 * @param[in]     c       coefficients
 * @param[in]     lane    first lane of the register
 * @param[in]     n       samples
 * @param[in]     noise   per sample and lane (index i * 16 + lane): white noise
 * @param[in]     reset   per sample and lane: value the noise envelope restarts at, 0 = none
 * @param[in]     dNoise  per sample and lane: noise envelope factor
 * @param[in]     tone    whether this lane group uses the source at all (decided per group of 8 lanes so every vector
 *                        width runs the same arithmetic on every lane)
 * @param[in]     modal   whether this lane group uses the source at all (decided per group of 8 lanes so every vector
 *                        width runs the same arithmetic on every lane)
 * @param[in]     metal   whether this lane group uses the source at all (decided per group of 8 lanes so every vector
 *                        width runs the same arithmetic on every lane)
 * @param[in]     pan     whether *any* lane of the kit moves in the panorama. It is one decision for the whole kit
 *                        rather than per group of eight, so that every lane's pan phasor advances in step whatever the
 *                        other lanes are set to: a phasor that stopped while its group had no moving lane would make
 *                        the phase depend on the history of the knobs.
 * @param[in,out] outL    per sample and lane outputs
 * @param[in,out] outR    per sample and lane outputs
 */
template <class VT>
void percKernel( PercState &s, const PercCoefs &c, int lane, int n, const float *noise, const float *reset,
                 const float *dNoise, bool tone, bool modal, bool metal, bool pan, float *outL, float *outR )
{
    auto     at = [lane]( const float *a ) { return loadLanes<VT>( a + lane ); };
    const VT zero = lanes<VT>( 0.0f ), one = lanes<VT>( 1.0f ), two = lanes<VT>( 2.0f ), half = lanes<VT>( 0.5f ),
             threeHalves = lanes<VT>( 1.5f );
    const VT sqrt2 = lanes<VT>( 1.41421356f ), sixth = lanes<VT>( 1.0f / 6.0f );
    const VT t2 = lanes<VT>( 0.5f ), t24 = lanes<VT>( 1.0f / 24.0f ), t720 = lanes<VT>( 1.0f / 720.0f );
    const VT t6 = lanes<VT>( 1.0f / 6.0f ), t120 = lanes<VT>( 1.0f / 120.0f ), t5040 = lanes<VT>( 1.0f / 5040.0f );
    const VT wMax = lanes<VT>( 1.2f ), wMin = lanes<VT>( -1.2f );

    VT cr = at( s.m_cr ), ci = at( s.m_ci ), mr = at( s.m_mr ), mi = at( s.m_mi );
    VT envA = at( s.m_envA ), envP = at( s.m_envP ), envN = at( s.m_envN ), choke = at( s.m_choke );
    VT ic1 = at( s.m_ic1 ), ic2 = at( s.m_ic2 ), la1 = at( s.m_la1 ), la2 = at( s.m_la2 ), lb1 = at( s.m_lb1 ),
       lb2 = at( s.m_lb2 );
    VT zr[PercModes], zi[PercModes], mC[PercModes], mS[PercModes];
    for ( int m = 0; m < PercModes; ++m )
    {
        zr[m] = at( s.m_zr[m] );
        zi[m] = at( s.m_zi[m] );
        mC[m] = at( c.m_modeC[m] );
        mS[m] = at( c.m_modeS[m] );
    }
    VT mt[PercMetalOsc], dt[PercMetalOsc], iv[PercMetalOsc];
    for ( int j = 0; j < PercMetalOsc; ++j )
    {
        mt[j] = at( s.m_mt[j] );
        dt[j] = at( c.m_dt[j] );
        iv[j] = at( c.m_inv[j] );
    }
    const VT w0 = at( c.m_w0 ), pAmt = at( c.m_pAmt ), dP = at( c.m_dP ), fmIdx = at( c.m_fmIdx ), mc = at( c.m_mc ),
             ms = at( c.m_ms ), dA = at( c.m_dA );
    const VT wTone = at( c.m_wTone ), wModal = at( c.m_wModal ), wMetal = at( c.m_wMetal ), wNoise = at( c.m_wNoise );
    const VT a1 = at( c.m_a1 ), a2 = at( c.m_a2 ), a3 = at( c.m_a3 ), kk = at( c.m_k ), mLp = at( c.m_mLp ),
             mBp = at( c.m_mBp ), mHp = at( c.m_mHp );
    const VT c1 = at( c.m_c1 ), c2 = at( c.m_c2 ), c3 = at( c.m_c3 ), drvG = at( c.m_drvG ), drvN = at( c.m_drvN ),
             chokeD = at( c.m_chokeD );
    const VT gL = at( c.m_gL ), gR = at( c.m_gR );
    VT       pr = at( s.m_pr ), pi = at( s.m_pi );
    const VT panC = at( c.m_panC ), panS = at( c.m_panS ), panA = at( c.m_panA ), panB = at( c.m_panB );

    for ( int i = 0; i < n; ++i )
    {
        const int row = i * PercLaneSlots + lane;
        VT        src = zero;
        if ( tone )
        {
            // Modulator: exact rotation, renormalised.
            const VT nmr = mr * mc - mi * ms, nmi = mr * ms + mi * mc;
            VT       fix = threeHalves - half * ( nmr * nmr + nmi * nmi );
            mr = nmr * fix;
            mi = nmi * fix;
            // Carrier step: base, pitch envelope, frequency modulation; clamped.
            VT w = w0 * ( one + pAmt * envP ) * ( one + fmIdx * envP * mi );
            w = vmin( vmax( w, wMin ), wMax );
            const VT q = w * w;
            const VT cs = one - q * ( t2 - q * ( t24 - q * t720 ) );
            const VT sn = w * ( one - q * ( t6 - q * ( t120 - q * t5040 ) ) );
            const VT ncr = cr * cs - ci * sn, nci = cr * sn + ci * cs;
            fix = threeHalves - half * ( ncr * ncr + nci * nci );
            cr = ncr * fix;
            ci = nci * fix;
            src = src + wTone * ci * envA;
            envP = envP * dP;
        }
        if ( modal )
        {
            VT sum = zero;
            for ( int m = 0; m < PercModes; ++m )
            {
                const VT nr = zr[m] * mC[m] - zi[m] * mS[m];
                zi[m] = zr[m] * mS[m] + zi[m] * mC[m];
                zr[m] = nr;
                sum = sum + zi[m];
            }
            src = src + wModal * sum;
        }
        if ( metal )
        {
            VT acc = zero;
            for ( int j = 0; j < PercMetalOsc; ++j )
            {
                const VT t = mt[j], d = dt[j], inv = iv[j];
                VT       sq = vselect( vlt( t, half ), one, -one );
                const VT x1 = t * inv;
                sq = sq + vselect( vlt( t, d ), x1 + x1 - x1 * x1 - one, zero );
                const VT x2 = ( t - one ) * inv;
                sq = sq + vselect( vgt( t, one - d ), x2 * x2 + x2 + x2 + one, zero );
                VT u = t + half;
                u = vselect( vge( u, one ), u - one, u );
                const VT x3 = u * inv;
                sq = sq - vselect( vlt( u, d ), x3 + x3 - x3 * x3 - one, zero );
                const VT x4 = ( u - one ) * inv;
                sq = sq - vselect( vgt( u, one - d ), x4 * x4 + x4 + x4 + one, zero );
                acc = acc + sq;
                const VT nt = t + d;
                mt[j] = vselect( vge( nt, one ), nt - one, nt );
            }
            src = src + wMetal * acc * sixth * envA;
        }
        const VT rst = loadLanes<VT>( reset + row );
        envN = vselect( vgt( rst, zero ), rst, envN );
        src = src + wNoise * loadLanes<VT>( noise + row ) * envN;
        envN = envN * loadLanes<VT>( dNoise + row );
        envA = envA * dA;

        // Main filter (trapezoidal SVF), output chosen by weights.
        VT v3 = src - ic2;
        VT v1 = a1 * ic1 + a2 * v3;
        VT v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = two * v1 - ic1;
        ic2 = two * v2 - ic2;
        VT y = mLp * v2 + mBp * kk * v1 + mHp * ( src - kk * v1 - v2 );
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
        const VT yg = y * drvG;
        y = yg / vsqrt( one + yg * yg ) * drvN;
        y = y * choke;
        choke = choke * chokeD;
        // The auto-pan: turn (gL, gR) on the unit circle by d = panA * cos(phase) + panB. The phasor
        // is renormalised the same way the carrier is, and so is the (cos d, sin d) pair, which is
        // what makes the two channel powers sum to a constant to float precision.
        VT gl = gL, gr = gR;
        if ( pan )
        {
            const VT npr = pr * panC - pi * panS, npi = pr * panS + pi * panC;
            VT       fix = threeHalves - half * ( npr * npr + npi * npi );
            pr = npr * fix;
            pi = npi * fix;
            const VT d = panA * pr + panB;
            const VT q = d * d;
            VT       cs = one - q * ( t2 - q * ( t24 - q * t720 ) );
            VT       sn = d * ( one - q * ( t6 - q * ( t120 - q * t5040 ) ) );
            fix = threeHalves - half * ( cs * cs + sn * sn );
            cs = cs * fix;
            sn = sn * fix;
            gl = gL * cs - gR * sn;
            gr = gR * cs + gL * sn;
        }
        vstore( outL + row, y * gl );
        vstore( outR + row, y * gr );
    }

    auto put = [lane]( float *a, VT v ) { vstore( a + lane, v ); };
    put( s.m_pr, pr );
    put( s.m_pi, pi );
    put( s.m_cr, cr );
    put( s.m_ci, ci );
    put( s.m_mr, mr );
    put( s.m_mi, mi );
    put( s.m_envA, envA );
    put( s.m_envP, envP );
    put( s.m_envN, envN );
    put( s.m_choke, choke );
    put( s.m_ic1, ic1 );
    put( s.m_ic2, ic2 );
    put( s.m_la1, la1 );
    put( s.m_la2, la2 );
    put( s.m_lb1, lb1 );
    put( s.m_lb2, lb2 );
    for ( int m = 0; m < PercModes; ++m )
    {
        put( s.m_zr[m], zr[m] );
        put( s.m_zi[m], zi[m] );
    }
    for ( int j = 0; j < PercMetalOsc; ++j ) put( s.m_mt[j], mt[j] );
}

}   // namespace phos
