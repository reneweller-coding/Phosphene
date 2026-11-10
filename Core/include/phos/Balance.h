/**
 * @file Balance.h
 * @brief The master's tonal balance (21.10.2026): the share of six bands brought into the range of the genre's
 *        references -- gently, slowly, and only where there is something to raise.
 *
 * Measured on the user's own collection (Family/tools/mix_audit.py, the third-octave curve of 308 Ultimae, 228 Robert
 * Rich and 75 Polymorph recordings), a master of that kind keeps most of its power between 56 and 140 Hz (a median of
 * -2.4 dB of the whole at Ultimae), a quiet sub under it (-26 dB), and presence and air well above what a dark drone
 * leaves (-15 and -21 dB). A preset instrument measured the same way strays far: Empyrean's demos had no sub at all
 * (-72 .. -80 dB) and mids at -2 dB, Noctuary's bank (256 presets) its presence at -25 dB and its air at -55 dB.
 *
 * **Analysis**, on the input: both channels through six bands (under 56 Hz, 56-141, 141-449, 449-1425, 1425-5700, over
 * 5700 -- the references' third octaves folded), their power gathered over 100 ms and gated as ImageMatch's (only
 * blocks within 8 dB of the loudest of the last minute count), into averages over about eight seconds. A band's share
 * is its power over all six's.
 *
 * **Aim:** a band above its range is brought down to the range's top, one below it up to its bottom -- if it holds more
 * than -50 dB of the whole (an equaliser cannot raise what is not there: that is the Foundation's work), and at most
 * 4 dB either way. Inside the range nothing moves. As the shares depend on each other (lowering the loudest band
 * raises every other's share), the aims are found by a few rounds of moving each band to its range's nearest edge
 * through the equaliser's exact response at the band's centre. The trims glide at half a dB a second, times Amount.
 *
 * **The equaliser** is a low shelf, four bells and a high shelf on both channels (ToneEq). Feed-forward from the
 * input, so nothing in the loop can swing. Amount 0 with every trim back at 0 dB is a true bypass.
 * @note Copied from Noctuary `Core/include/ambient/Balance.h` (21.10.2026, the family's canonical copy); namespace phos.
 * @author Rene Weller
 */
#pragma once
#include "phos/ToneEq.h"
#include <array>

namespace phos
{

/** @brief A band meter: two high passes and two low passes, the trapezoidal SVF's low and high outputs. */
struct BandMeter
{
    float m_g[4] = {};     ///< the four stages' g
    /** @name The stages' factors, worked out by prepare() (Phosphene, 10.11.2026)
     *  run() divided for each of them in every stage of every sample: 48 divisions a sample through the six bands of
     *  two channels, 3.7 % of a set's render. The same numbers, once. (Not yet in Noctuary's canonical copy.)
     *  @{ */
    float m_a1[4] = {};    ///< 1 / (1 + g (g + k))
    float m_a2[4] = {};    ///< g a1
    float m_a3[4] = {};    ///< g a2
    /** @} */
    float m_k = 1.414f;    ///< their damping (Butterworth)
    float m_s1[4] = {};    ///< their first integrators
    float m_s2[4] = {};    ///< their second
    bool  m_high = true;   ///< the band has a lower edge (the lowest band has none)
    bool  m_low = true;    ///< and an upper edge

    /**
     * @brief Sets the band's edges and clears the memory.
     * @param[in] lowHz      its lower edge (0: none)
     * @param[in] highHz     its upper edge (0: none)
     * @param[in] sampleRate the sample rate in Hz
     */
    void prepare( float lowHz, float highHz, double sampleRate );

    /** @brief Clears the memory. */
    void clear();

    /**
     * @brief One sample through the meter.
     * @param[in] x  the sample
     * @return the band's sample
     */
    float run( float x );
};

/** @brief The tonal balance in six bands brought into a genre's range. */
class BalanceMatch
{
public:
    static constexpr int Bands = 6;   ///< under 56 Hz, 56-141, 141-449, 449-1425, 1425-5700, over 5700

    /** @brief What it has learnt (22.10.2026): enough to begin a piece where a pass over the whole of it ended. */
    struct Learnt
    {
        std::array<double, Bands> m_avg{};       ///< the gated averages
        std::array<float, Bands>  m_trim{};      ///< the trims, dB
        double                    m_loudest = 0.0;   ///< the loudest the gate compares with
        double                    m_short = 0.0;     ///< the loudness over the gate's seconds
        bool                      m_heard = false;   ///< anything was averaged at all
    };

    /**
     * @brief Sets the rate and clears everything.
     * @param[in] sampleRate  the sample rate in Hz
     */
    void prepare( double sampleRate );

    /** @brief Clears the averages and returns every trim to 0 dB at once. */
    void reset();

    /**
     * @brief The genre's range per band: its share of the whole in dB (its references' first and third quartile).
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
     * @brief How fast it listens and moves (21.10.2026). The defaults (8 s, half a dB a second, 8 dB, 10 dB a minute)
     *        suit an instrument played live; a composed piece wants its balance learnt on its loudest part and held
     *        through the breakdowns -- no equaliser chasing the arrangement (30 s, 0.1 dB a second, 3 dB, 1 dB a
     *        minute: in Parhelion's energy test the drop stood 3.9 LU over the breakdown, with the defaults 2.1).
     * @param[in] averageSeconds     the averages' time
     * @param[in] glideDbPerSecond   how fast a trim moves
     * @param[in] gateDb             blocks within this of the loudest count
     * @param[in] fallDbPerMinute    how fast the loudest is forgotten
     * @param[in] gateSeconds        what the gate compares: 0 each 100 ms block (the default), else the loudness over
     *                               that many seconds -- so a dance track's loud passages count, not its kick hits
     *                               (22.10.2026: gated on 100 ms blocks, Parhelion's loudest blocks were its kicks, and
     *                               their mids and highs read 3 to 5 dB under the passage's)
     */
    void setTiming( float averageSeconds, float glideDbPerSecond, float gateDb, float fallDbPerMinute,
                    float gateSeconds = 0.0f );

    /**
     * @brief The sub band's lower edge (22.10.2026). The references' third octaves begin at 40 Hz (35.6 Hz its lower
     *        edge): what lies under it is not in their curves, and a kick's fundamental and its sub under 35 Hz read
     *        as a sub 15 dB over theirs. 0, the default, counts everything under 56 Hz (Noctuary's and Empyrean's
     *        banks are levelled so); the generators set 35.6.
     * @param[in] hz  the edge (0: none)
     */
    void setLowEdge( float hz );

    /**
     * @brief Whether the trims keep the whole's power where it was (22.10.2026). The shares do not change when every
     *        band moves alike, so without this the trims may all drift up or down together -- in Parhelion by about
     *        3 dB, which its Leveler then took back before the master, and every stem stood 3 to 4 LU lower against
     *        the mix. Off by default: Noctuary's and Empyrean's banks are levelled with it off.
     * @param[in] neutral  true: a common offset after every round of the aims holds the whole's power
     */
    void setNeutral( bool neutral ) { m_neutral = neutral; }

    /**
     * @return what it has learnt so far
     */
    Learnt learnt() const;

    /**
     * @brief Begins where @p learnt left off (22.10.2026): a composed piece rendered once ahead -- its levelling --
     *        plays with its balance from the first bar, as the other generators' tone trims measured before the
     *        track; without it the trims start flat and take a minute to arrive. Nothing that was not heard is taken.
     * @param[in] learnt  learnt() at the end of the pass
     */
    void prime( const Learnt &learnt );

    /**
     * @brief Processes @p n samples in place.
     * @param[in,out] L  the left channel
     * @param[in,out] R  the right channel
     * @param[in]     n  the number of samples
     */
    void process( float *L, float *R, int n );

    /**
     * @param[in] band  the band
     * @return its trim now, dB
     */
    float trim( int band ) const { return m_trim[static_cast<size_t>( band )]; }

    /**
     * @param[in] band  the band
     * @return its share of the whole as measured, dB (-120: not yet heard)
     */
    float share( int band ) const;

private:
    /** @brief A block's end: the gate, the averages, the aims and a step of the trims towards them. */
    void block();

    /**
     * @brief The trims that bring every band with something in it into its range, as far as Amount and the limits go.
     * @param[out] aim  a trim per band, dB
     */
    void aims( float *aim ) const;

    double                    m_rate = 48000.0;       ///< the sample rate
    float                     m_amount = 0.0f;        ///< how much of the way (0 .. 1)
    std::array<float, Bands>  m_lo{};                 ///< the range's lower edges, dB of the whole
    std::array<float, Bands>  m_hi{};                 ///< its upper edges
    std::array<BandMeter, Bands> m_left{};            ///< the left channel's band meters
    std::array<BandMeter, Bands> m_right{};           ///< the right's
    std::array<double, Bands> m_block{};              ///< this block's power per band, both channels
    std::array<double, Bands> m_avg{};                ///< the gated averages
    std::array<float, Bands>  m_trim{};               ///< the trims now, dB
    double                    m_loudest = 0.0;        ///< the loudest block's power of the last minute (decaying)
    int                       m_count = 0;            ///< samples in this block so far
    int                       m_blockLength = 4800;   ///< samples a block (100 ms)
    float                     m_avgCoef = 0.0f;       ///< the averages' step per gated block (about 8 s)
    float                     m_glide = 0.05f;        ///< the trims' step per block, dB (half a dB a second)
    float                     m_avgSeconds = 8.0f;    ///< the averages' time (setTiming)
    float                     m_glidePerSecond = 0.5f;   ///< the trims' speed, dB a second
    float                     m_gateDb = 8.0f;        ///< blocks within this of the loudest count
    float                     m_fallPerMinute = 10.0f;   ///< how fast the loudest is forgotten, dB a minute
    double                    m_fall = 1.0;           ///< the loudest's factor per block (falling)
    bool                      m_heard = false;        ///< a gated block has been averaged
    bool                      m_neutral = false;      ///< the trims keep the whole's power (setNeutral)
    float                     m_gateSeconds = 0.0f;   ///< the gate's measure: 0 a block, else seconds (setTiming)
    float                     m_shortCoef = 1.0f;     ///< that measure's step per block
    double                    m_short = 0.0;          ///< the loudness over m_gateSeconds (the blocks' power, followed)
    float                     m_lowEdge = 0.0f;       ///< the sub band's lower edge, Hz (setLowEdge)
    ToneEq                    m_eq;                   ///< the shelves and bells
    ToneEq::State             m_state;                ///< their memory
};

/**
 * @brief The floor (21.10.2026): the Foundation's two voices set so that the music stands where Ultimae's does -- the
 *        bass band (56-141 Hz) at a share of the whole, and a quiet sub under it.
 *
 * Measured as played, Noctuary's built-ins kept a median 95 % of their power under 150 Hz, most of it under 56 Hz
 * (the Foundation an octave or two under the music, about 20 dB over it), and a quarter of them under 8 % -- no floor
 * at all; Ultimae's masters keep their floor between 56 and 141 Hz (-2.4 dB of the whole) and the sub 26 dB under the
 * whole. So the Foundation as it is (the first voice) is joined by a second on its pitch class in the bass (56 .. 112
 * Hz: octaves above a deep Foundation, below a high one); and both are set by what the rest of the music holds.
 *
 * Feed-forward: what is measured is the rest (everything but the floor) and each voice's own power per band at a level
 * of one; the levels that bring the whole's two shares to their aims follow in closed form -- two equations, two
 * unknowns, no voice below silence. Gated and averaged as BalanceMatch (100 ms blocks, the blocks within 8 dB of the
 * loudest of the last minute, about four seconds). The levels glide at 2.5 dB a second, decided at a block's end and
 * ramped over the next, so they change at the same samples whatever the host's block size.
 */
class FloorMatch
{
public:
    static constexpr int Voices = 2;   ///< the Foundation as it is, and the one above it in the bass

    /**
     * @brief Sets the rate and clears everything.
     * @param[in] sampleRate  the sample rate in Hz
     */
    void prepare( double sampleRate );

    /** @brief Clears the averages and returns both levels to where they begin. */
    void reset();

    /**
     * @brief The aims: the whole's share in the bass band and under it.
     * @param[in] bassDb  the bass band's share (56-141 Hz), dB of the whole
     * @param[in] subDb   the sub's (under 56 Hz)
     */
    void setAims( float bassDb, float subDb );

    /**
     * @brief Measures the rest and writes the floor: both voices at their levels.
     * @param[in]  restL  the rest of the music, left
     * @param[in]  restR  and right
     * @param[in]  aL     the first voice at a level of one, left
     * @param[in]  aR     and right
     * @param[in]  bL     the second, left
     * @param[in]  bR     and right
     * @param[out] outL   the floor, left
     * @param[out] outR   and right
     * @param[in]  n      the number of samples
     */
    void process( const float *restL, const float *restR, const float *aL, const float *aR, const float *bL,
                  const float *bR, float *outL, float *outR, int n );

    /**
     * @param[in] voice  0 the Foundation as it is, 1 the one above it
     * @return its level now, dB against one
     */
    float levelDb( int voice ) const { return m_db[static_cast<size_t>( voice )]; }

private:
    /** @brief A block's end: the gate, the averages, the aims and the next block's ramp. */
    void block();

    /**
     * @brief The power gains that bring the whole's shares to their aims, from the averages.
     * @param[out] x  the first voice's power gain (negative: no aim, keep the level)
     * @param[out] y  the second's
     */
    void solve( double &x, double &y ) const;

    double                       m_rate = 48000.0;       ///< the sample rate
    double                       m_bassAim = 0.35;       ///< the bass band's share aimed at (power ratio)
    double                       m_subAim = 0.002;       ///< the sub's
    BandMeter                    m_restMeter[2][2];      ///< the rest's meters: [sub, bass][left, right]
    BandMeter                    m_voiceMeter[Voices][2][2];   ///< each voice's: [voice][sub, bass][left, right]
    std::array<double, 3>        m_blockRest{};          ///< this block's rest: sub, bass, all
    std::array<double, 3>        m_avgRest{};            ///< and its gated averages
    double                       m_blockVoice[Voices][3] = {};   ///< this block's voices at one: sub, bass, all
    double                       m_avgVoice[Voices][3] = {};     ///< and their gated averages
    std::array<float, Voices>    m_db{};                 ///< the levels now, dB
    std::array<float, Voices>    m_from{};               ///< this block's levels at its start (linear)
    std::array<float, Voices>    m_to{};                 ///< and at its end
    double                       m_loudest = 0.0;        ///< the rest's loudest block of the last minute (decaying)
    int                          m_count = 0;            ///< samples in this block so far
    int                          m_blockLength = 4800;   ///< samples a block (100 ms)
    float                        m_avgCoef = 0.0f;       ///< the averages' step per gated block (about 4 s)
    bool                         m_heard = false;        ///< a gated block has been averaged
};

}   // namespace phos
