/**
 * @file Ducker.h
 * @brief Event-driven sidechain ducking.
 *
 * The kick's triggers are known from the score, so ducking does not need to listen to the kick's
 * audio: the gain curve starts on the trigger, sample-accurate and deterministic, with no detector
 * lag and no dependence on the kick's level. The curve is attack (linear, from wherever it is),
 * hold, and a raised-cosine release back to unity -- continuous in value everywhere, and in slope
 * at both ends of the release.
 * @author Rene Weller
 */
#pragma once
#include "phos/Dsp.h"

namespace phos
{

/** @brief Gain envelope triggered by events. */
class Ducker
{
public:
    /**
     * @brief Sets the sample rate.
     * @param[in] sr  the sample rate in Hz
     */
    void prepare( double sr )
    {
        m_sr = sr;
        reset();
    }
    /** @brief Back to unity, idle. */
    void reset()
    {
        m_amount = 0.0f;
        m_stage = StageE::IDLE;
    }
    /**
     * @brief Shape of the duck.
     * @param[in] depth      0..1, how far the gain falls (1 = silence)
     * @param[in] attackMs   time to full depth
     * @param[in] holdMs     time held at full depth
     * @param[in] releaseMs  time back to unity
     */
    void set( float depth, float attackMs, float holdMs, float releaseMs )
    {
        m_depth = clampv( depth, 0.0f, 1.0f );
        m_attack = std::max( 1, static_cast<int>( attackMs * 0.001 * m_sr ) );
        m_hold = std::max( 0, static_cast<int>( holdMs * 0.001 * m_sr ) );
        m_release = std::max( 1, static_cast<int>( releaseMs * 0.001 * m_sr ) );
    }
    /**
     * @brief Starts a duck.
     * @param[in] late  how many samples ago the trigger ideally happened (0 <= late < 1): the curve is evaluated that
     *                  much further along, so it keeps the same shape relative to the kick whatever the kick's position
     *                  between samples
     */
    void trigger( double late = 0.0 )
    {
        m_stage = StageE::ATTACK;
        m_pos = 0;
        m_from = m_amount;
        m_late = static_cast<float>( late );
    }
    /**
     * @brief Next gain factor.
     * @return the next gain factor
     */
    inline float next()
    {
        switch ( m_stage )
        {
        case StageE::ATTACK:
            m_amount = m_from + ( 1.0f - m_from ) * std::min( 1.0f, ( static_cast<float>( m_pos + 1 ) + m_late ) /
                                                                        static_cast<float>( m_attack ) );
            if ( ++m_pos >= m_attack )
            {
                m_stage = StageE::HOLD;
                m_pos = 0;
                m_amount = 1.0f;
            }
            break;
        case StageE::HOLD:
            if ( ++m_pos >= m_hold )
            {
                m_stage = StageE::RELEASE;
                m_pos = 0;
            }
            break;
        case StageE::RELEASE:
            m_amount = 0.5f + 0.5f * std::cos( Pi * std::min( 1.0f, ( static_cast<float>( m_pos + 1 ) + m_late ) /
                                                                        static_cast<float>( m_release ) ) );
            if ( ++m_pos >= m_release )
            {
                m_stage = StageE::IDLE;
                m_amount = 0.0f;
            }
            break;
        default: break;
        }
        return 1.0f - m_depth * m_amount;
    }

private:
    /** @brief The curve's stages. */
    enum class StageE
    {
        IDLE,
        ATTACK,
        HOLD,
        RELEASE
    };
    double m_sr = 48000.0;           ///< sample rate
    float  m_depth = 0.5f;           ///< the depth, 0..1
    float  m_amount = 0.0f;          ///< the current amount (0..1, scaled by the depth)
    float  m_from = 0.0f;            ///< where the attack started
    float  m_late = 0.0f;            ///< the trigger's sub-sample offset
    int    m_attack = 48;            ///< the attack's length, samples
    int    m_hold = 0;               ///< the hold's length, samples
    int    m_release = 2880;         ///< the release's length, samples
    int    m_pos = 0;                ///< samples into the stage
    StageE m_stage = StageE::IDLE;   ///< where it is
};

}   // namespace phos
