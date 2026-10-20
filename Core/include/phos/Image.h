/**
 * @file Image.h
 * @brief The master's image (20.10.2026): the stereo width in five bands brought into the range of the genre's
 *        references, and never wider than the mono fold-down allows.
 *
 * What makes a master like Ultimae's or Robert Rich's sound placed is not one width but a width that follows the
 * frequency: measured on 308 Ultimae recordings (Family/tools/mix_audit.py), the side lies 11 dB under the mid at
 * 63 Hz, 2.5 dB under it from 500 Hz to 2 kHz, and narrows again to 5 dB under it above 8 kHz -- a centred floor, a
 * wide body, highs that stay where they are. A generator's mix does not know its own width: a mono 303 line leaves a
 * trance drop narrow (side 16 dB under the mid), a voice spread by a Haas delay leaves the side above the mid in an
 * octave, and that octave cancels when the mix is played in mono.
 *
 * **Analysis**, on the input: mid and side through five bands (two second-order Butterworth high passes and two low
 * passes each, as the Leveler's band meters), their energy gathered over 100 ms, and gated -- only blocks within
 * 8 dB of the loudest of the last minute count, so a breakdown's tail or a silence does not move the image -- into
 * averages over about eight seconds. The width of a band is its side's energy over its mid's.
 *
 * **Aim:** a band wider than its range is brought to the range's top, a narrower one to its bottom -- if there is a
 * side to raise (more than 40 dB under the mid), and at most 4 dB up; inside the range nothing moves. Whatever the
 * range, no band's side may stand over -0.5 dB of its mid (the guard: what would cancel in mono). The trims glide at
 * half a dB a second, times Amount.
 *
 * **The cut** is a shelf and three bells on the side alone (ToneEq), the mid untouched: the centre -- kick, bass, a
 * lead's fundamental -- stays where it was; only how far the rest reaches out changes. Feed-forward from the input,
 * so nothing in the loop can swing. Amount 0 with every trim back at 0 dB is a true bypass.
 * @note Copied from Totality `Core/include/tot/fx/Image.h` (20.10.2026); namespace phos.
 * @author Rene Weller
 */
#pragma once
#include "phos/ToneEq.h"
#include <array>

namespace phos
{

/** @brief The stereo width per band brought into a genre's range, with a guard against what cancels in mono. */
class ImageMatch
{
public:
    static constexpr int Bands = 5;   ///< under 180 Hz, 180-500, 500-1400, 1400-5600, over 5600

    /**
     * @brief Sets the rate and clears everything.
     * @param[in] sampleRate  the sample rate in Hz
     */
    void prepare( double sampleRate );

    /** @brief Clears the averages and returns every trim to 0 dB at once. */
    void reset();

    /**
     * @brief The genre's range per band: side over mid in dB (its references' first and third quartile).
     * @param[in] lo  the lower edges, a band each
     * @param[in] hi  the upper edges
     */
    void setRanges( const float *lo, const float *hi );

    /**
     * @brief How much of the way it goes.
     * @param[in] amount  0 (off) .. 1
     */
    void set( float amount );

    /**
     * @brief Processes @p n samples in place.
     * @param[in,out] L  the left channel
     * @param[in,out] R  the right channel
     * @param[in]     n  the number of samples
     */
    void process( float *L, float *R, int n );

    /**
     * @param[in] band  the band
     * @return its side's trim now, dB
     */
    float trim( int band ) const { return m_trim[static_cast<size_t>( band )]; }

    /**
     * @param[in] band  the band
     * @return its width as measured, side over mid in dB (-120: not yet heard)
     */
    float width( int band ) const;

private:
    /** @brief A band meter: two high passes and two low passes, the trapezoidal SVF's low and high outputs. */
    struct Meter
    {
        float m_g[4] = {};     ///< the four stages' g
        float m_k = 1.414f;    ///< their damping (Butterworth)
        float m_s1[4] = {};    ///< their first integrators
        float m_s2[4] = {};    ///< their second
        bool  m_high = true;   ///< the band has a lower edge (the lowest band has none)
        bool  m_low = true;    ///< and an upper edge
    };

    /**
     * @brief One sample through a meter.
     * @param[in,out] m  the meter
     * @param[in]     x  the sample
     * @return the band's sample
     */
    static float run( Meter &m, float x );

    /** @brief A block's end: the gate, the averages, the aims and a step of the trims towards them. */
    void block();

    double                       m_rate = 48000.0;    ///< the sample rate
    float                        m_amount = 0.0f;     ///< how much of the way (0 .. 1)
    std::array<float, Bands>     m_lo{};              ///< the range's lower edges, dB
    std::array<float, Bands>     m_hi{};              ///< its upper edges
    std::array<Meter, Bands>     m_mid{};             ///< the mid's band meters
    std::array<Meter, Bands>     m_side{};            ///< the side's
    std::array<double, Bands>    m_blockMid{};        ///< this block's mid energy per band
    std::array<double, Bands>    m_blockSide{};       ///< and side energy
    std::array<double, Bands>    m_avgMid{};          ///< the gated averages of the mid's energy
    std::array<double, Bands>    m_avgSide{};         ///< and of the side's
    std::array<float, Bands>     m_trim{};            ///< the side's trims now, dB
    double                       m_loudest = 0.0;     ///< the loudest block's energy of the last minute (decaying)
    int                          m_count = 0;         ///< samples in this block so far
    int                          m_blockLength = 4800;   ///< samples a block (100 ms)
    float                        m_avgCoef = 0.0f;    ///< the averages' step per gated block (about 8 s)
    float                        m_glide = 0.05f;     ///< the trims' step per block, dB (half a dB a second)
    bool                         m_heard = false;     ///< a gated block has been averaged
    ToneEq                       m_eq;                ///< the side's shelf and bells
    ToneEq::State                m_state;             ///< their memory
};

}   // namespace phos
