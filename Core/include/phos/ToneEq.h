/**
 * @file ToneEq.h
 * @brief The tone trims' equaliser (20.10.2026): a track brought toward its style's tonal balance, measured on the
 *        references, by a few broad bands -- a low shelf, bells, a high shelf.
 *
 * Where a mastering engineer reaches for a broad equaliser after hearing the loudest part of a track, the composer
 * (Composer::matchTone; Totality's Leveler there) measures that part before it plays (score-aware: the trims are known when the track begins and hold for all of it,
 * no equaliser chasing the arrangement) and sets a gain per band: a share of the distance between the part's band
 * levels and the references' median for the style, at most a few dB. This class is only the equaliser.
 *
 * The bands are Simper's trapezoidal state-variable shelves and bells (Cytomic, "Solving the continuous SVF equations
 * using trapezoidal integration and equivalent currents", 2013): stable when their gains move, which they do slowly,
 * a block at a time. The coefficients live here and the states apart (State), so that one set of coefficients runs
 * the mix and every stem alike -- the filters are linear, the stems still sum to the mix.
 * @note Copied from Totality `Core/include/tot/fx/ToneEq.h` (20.10.2026); namespace phos.
 * @author Rene Weller
 */
#pragma once
#include <algorithm>
#include <cmath>

namespace phos
{

/** @brief What a band of the equaliser is. */
enum class ToneBandE
{
    LOW_SHELF,
    BELL,
    HIGH_SHELF
};

/** @brief A band: its kind, its frequency (a shelf's corner, a bell's centre) and its Q. */
struct ToneBand
{
    ToneBandE m_kind = ToneBandE::BELL;   ///< a shelf or a bell
    float     m_hz = 1000.0f;             ///< a shelf's corner, a bell's centre
    float     m_q = 0.7071f;              ///< its Q
};

/** @brief A few broad shelves and bells, their gains set a block at a time; the states kept by the caller. */
class ToneEq
{
public:
    static constexpr int MaxBands = 6;   ///< the most bands

    /** @brief The filters' memory for one stereo signal. */
    struct State
    {
        float m_ic1[MaxBands][2] = {};   ///< the first integrator per band and channel
        float m_ic2[MaxBands][2] = {};   ///< the second

        /** @brief Clears the memory. */
        void reset() { *this = State{}; }
    };

    /**
     * @brief Sets the rate and the bands, every gain at 0 dB.
     * @param[in] sampleRate  the sample rate in Hz
     * @param[in] bands       the bands
     * @param[in] count       how many (at most MaxBands)
     */
    void prepare( double sampleRate, const ToneBand *bands, int count )
    {
        m_rate = sampleRate;
        m_count = std::clamp( count, 0, MaxBands );
        for ( int b = 0; b < m_count; ++b ) m_bands[b] = bands[b];
        const float zero[MaxBands] = {};
        setGains( zero );
    }

    /**
     * @brief Sets every band's gain.
     * @param[in] db  a gain per band in dB
     */
    void setGains( const float *db )
    {
        m_flat = true;
        for ( int b = 0; b < m_count; ++b )
        {
            const ToneBand &band = m_bands[b];
            const float     A = std::pow( 10.0f, db[b] / 40.0f );
            const double    hz = std::min( static_cast<double>( band.m_hz ), 0.45 * m_rate );
            const float     t = static_cast<float>( std::tan( 3.14159265358979 * hz / m_rate ) );
            float           g = t, k = 1.0f / band.m_q;
            Coef           &c = m_coef[b];
            switch ( band.m_kind )
            {
            case ToneBandE::LOW_SHELF:
                g = t / std::sqrt( A );
                c.m_m0 = 1.0f;
                c.m_m1 = k * ( A - 1.0f );
                c.m_m2 = A * A - 1.0f;
                break;
            case ToneBandE::HIGH_SHELF:
                g = t * std::sqrt( A );
                c.m_m0 = A * A;
                c.m_m1 = k * ( 1.0f - A ) * A;
                c.m_m2 = 1.0f - A * A;
                break;
            case ToneBandE::BELL:
                k = 1.0f / ( band.m_q * A );
                c.m_m0 = 1.0f;
                c.m_m1 = k * ( A * A - 1.0f );
                c.m_m2 = 0.0f;
                break;
            }
            c.m_a1 = 1.0f / ( 1.0f + g * ( g + k ) );
            c.m_a2 = g * c.m_a1;
            c.m_a3 = g * c.m_a2;
            c.m_on = std::fabs( db[b] ) > 0.005f;
            m_flat = m_flat && !c.m_on;
        }
    }

    /** @return true when every gain is 0 dB (process then leaves the signal alone) */
    bool flat() const { return m_flat; }

    /**
     * @brief The power gain the bands would have at @p hz with the gains @p db (the coefficients set are left alone):
     *        the analogue prototypes at the prewarped frequency, which is what the trapezoidal filters are exactly.
     * @param[in] db  a gain per band in dB
     * @param[in] hz  the frequency
     * @return |H|^2 there
     */
    double powerGain( const float *db, double hz ) const
    {
        const double W = std::tan( 3.14159265358979 * std::min( hz, 0.499 * m_rate ) / m_rate );
        double       out = 1.0;
        for ( int b = 0; b < m_count; ++b )
        {
            const ToneBand &band = m_bands[b];
            const double    A = std::pow( 10.0, db[b] / 40.0 );
            const double    hzBand = std::min( static_cast<double>( band.m_hz ), 0.45 * m_rate );
            const double    t = std::tan( 3.14159265358979 * hzBand / m_rate );
            double          g = t, k = 1.0 / band.m_q, nr, ni;   // the numerator at s = jw, w = W / g
            switch ( band.m_kind )
            {
            case ToneBandE::LOW_SHELF:
                g = t / std::sqrt( A );
                break;
            case ToneBandE::HIGH_SHELF:
                g = t * std::sqrt( A );
                break;
            case ToneBandE::BELL:
                k = 1.0 / ( band.m_q * A );
                break;
            }
            const double w = W / g, w2 = w * w;
            switch ( band.m_kind )
            {
            case ToneBandE::LOW_SHELF:   // (s^2 + k A s + A^2) / (s^2 + k s + 1)
                nr = A * A - w2;
                ni = k * A * w;
                break;
            case ToneBandE::HIGH_SHELF:   // (A^2 s^2 + k A s + 1) / (s^2 + k s + 1)
                nr = 1.0 - A * A * w2;
                ni = k * A * w;
                break;
            default:   // the bell: (s^2 + k A^2 s + 1) / (s^2 + k s + 1)
                nr = 1.0 - w2;
                ni = k * A * A * w;
                break;
            }
            const double dr = 1.0 - w2, di = k * w;
            out *= ( nr * nr + ni * ni ) / ( dr * dr + di * di );
        }
        return out;
    }

    /**
     * @brief Processes @p n samples in place through the bands, with the memory @p s.
     * @param[in,out] s  the signal's filter memory
     * @param[in,out] L  the left channel
     * @param[in,out] R  the right channel
     * @param[in]     n  the number of samples
     */
    void process( State &s, float *L, float *R, int n ) const
    {
        if ( m_flat ) return;
        for ( int b = 0; b < m_count; ++b )
        {
            const Coef &c = m_coef[b];
            if ( !c.m_on ) continue;
            float *ch[2] = { L, R };
            for ( int q = 0; q < 2; ++q )
            {
                float  ic1 = s.m_ic1[b][q], ic2 = s.m_ic2[b][q];
                float *x = ch[q];
                if ( x == nullptr ) continue;
                for ( int i = 0; i < n; ++i )
                {
                    const float v0 = x[i], v3 = v0 - ic2;
                    const float v1 = c.m_a1 * ic1 + c.m_a2 * v3;
                    const float v2 = ic2 + c.m_a2 * ic1 + c.m_a3 * v3;
                    ic1 = 2.0f * v1 - ic1;
                    ic2 = 2.0f * v2 - ic2;
                    x[i] = c.m_m0 * v0 + c.m_m1 * v1 + c.m_m2 * v2;
                }
                s.m_ic1[b][q] = ic1;
                s.m_ic2[b][q] = ic2;
            }
        }
    }

private:
    /** @brief A band's coefficients (Simper: a1 .. a3 the integration, m0 .. m2 the output's mix). */
    struct Coef
    {
        float m_a1 = 1.0f;    ///< the integration's first coefficient
        float m_a2 = 0.0f;    ///< its second
        float m_a3 = 0.0f;    ///< its third
        float m_m0 = 1.0f;    ///< the input's share of the output
        float m_m1 = 0.0f;    ///< the band's share
        float m_m2 = 0.0f;    ///< the low pass's share
        bool  m_on = false;   ///< the band is away from 0 dB
    };
    double   m_rate = 48000.0;    ///< the sample rate
    int      m_count = 0;         ///< how many bands
    ToneBand m_bands[MaxBands];   ///< the bands
    Coef     m_coef[MaxBands];    ///< their coefficients
    bool     m_flat = true;       ///< every band at 0 dB
};

}   // namespace phos
