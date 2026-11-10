/**
 * @file Balance.cpp
 * @brief The master's tonal balance (see Balance.h).
 * @note Copied from Noctuary `Core/src/Balance.cpp` (21.10.2026); namespace phos.
 * @author Rene Weller
 */
#include "phos/Balance.h"

#include <algorithm>
#include <cmath>

namespace phos
{

namespace
{
constexpr double Pi = 3.14159265358979323846;   ///< pi
/** @brief The bands' edges, Hz: the lower (0: none) and the upper (0: none) -- the references' third octaves folded. */
constexpr float Edges[BalanceMatch::Bands][2] = { { 0.0f, 56.0f },      { 56.0f, 141.0f },   { 141.0f, 449.0f },
                                                  { 449.0f, 1425.0f }, { 1425.0f, 5700.0f }, { 5700.0f, 0.0f } };
/** @brief Where a band's trim is judged: its centre (the sub's and the air's where most of them lie). */
constexpr double Centres[BalanceMatch::Bands] = { 40.0, 89.0, 252.0, 800.0, 2850.0, 10000.0 };
/** @brief The equaliser: a low shelf, four bells, a high shelf -- one per band. */
const ToneBand EqBands[BalanceMatch::Bands] = { { ToneBandE::LOW_SHELF, 63.0f, 0.7071f }, { ToneBandE::BELL, 89.0f, 1.06f },
                                                { ToneBandE::BELL, 252.0f, 0.83f },       { ToneBandE::BELL, 800.0f, 0.83f },
                                                { ToneBandE::BELL, 2850.0f, 0.67f },
                                                { ToneBandE::HIGH_SHELF, 5000.0f, 0.7071f } };
constexpr float UpDb = 4.0f;       ///< the most a band is raised
constexpr float DownDb = -4.0f;    ///< the most it is lowered
constexpr float FloorDb = -50.0f;  ///< a band holding less of the whole is not raised (there is nothing to raise)
constexpr float GateDb = 8.0f;     ///< blocks within this of the loudest count
constexpr int   Rounds = 8;        ///< the aims' rounds (the shares depend on each other)
}   // namespace

void BandMeter::prepare( float lowHz, float highHz, double sampleRate )
{
    *this = BandMeter{};
    m_high = lowHz > 0.0f;
    m_low = highHz > 0.0f;
    for ( int s = 0; s < 4; ++s )
    {
        const float  hz = s < 2 ? lowHz : highHz;
        const double f = std::min( static_cast<double>( hz ), 0.45 * sampleRate );
        m_g[s] = hz > 0.0f ? static_cast<float>( std::tan( Pi * f / sampleRate ) ) : 0.0f;
        m_a1[s] = 1.0f / ( 1.0f + m_g[s] * ( m_g[s] + m_k ) );
        m_a2[s] = m_g[s] * m_a1[s];
        m_a3[s] = m_g[s] * m_a2[s];
    }
}

void BandMeter::clear()
{
    for ( int s = 0; s < 4; ++s ) m_s1[s] = m_s2[s] = 0.0f;
}

float BandMeter::run( float x )
{
    // Two high passes and two low passes, each Simper's trapezoidal SVF with Butterworth damping.
    for ( int s = 0; s < 4; ++s )
    {
        if ( ( s < 2 && !m_high ) || ( s >= 2 && !m_low ) ) continue;
        const float a1 = m_a1[s], a2 = m_a2[s], a3 = m_a3[s];
        const float v3 = x - m_s2[s];
        const float v1 = a1 * m_s1[s] + a2 * v3;
        const float v2 = m_s2[s] + a2 * m_s1[s] + a3 * v3;
        m_s1[s] = 2.0f * v1 - m_s1[s];
        m_s2[s] = 2.0f * v2 - m_s2[s];
        x = s < 2 ? x - m_k * v1 - v2 : v2;
    }
    return x;
}

void BalanceMatch::prepare( double sampleRate )
{
    m_rate = sampleRate;
    for ( int b = 0; b < Bands; ++b )
    {
        const float low = b == 0 ? m_lowEdge : Edges[b][0];
        m_left[static_cast<size_t>( b )].prepare( low, Edges[b][1], sampleRate );
        m_right[static_cast<size_t>( b )].prepare( low, Edges[b][1], sampleRate );
    }
    m_blockLength = std::max( 1, static_cast<int>( 0.1 * sampleRate ) );
    setTiming( m_avgSeconds, m_glidePerSecond, m_gateDb, m_fallPerMinute, m_gateSeconds );
    m_eq.prepare( sampleRate, EqBands, Bands );
    reset();
}

void BalanceMatch::setLowEdge( float hz )
{
    m_lowEdge = std::max( hz, 0.0f );
    m_left[0].prepare( m_lowEdge, Edges[0][1], m_rate );
    m_right[0].prepare( m_lowEdge, Edges[0][1], m_rate );
}

void BalanceMatch::setTiming( float averageSeconds, float glideDbPerSecond, float gateDb, float fallDbPerMinute,
                              float gateSeconds )
{
    m_avgSeconds = std::max( averageSeconds, 0.1f );
    m_glidePerSecond = std::max( glideDbPerSecond, 0.0f );
    m_gateDb = std::max( gateDb, 0.0f );
    m_fallPerMinute = std::max( fallDbPerMinute, 0.0f );
    const double block = static_cast<double>( m_blockLength ) / m_rate;   // seconds a block
    m_avgCoef = static_cast<float>( 1.0 - std::exp( -block / m_avgSeconds ) );
    m_glide = static_cast<float>( m_glidePerSecond * block );
    m_fall = std::pow( 10.0, -m_fallPerMinute * block / 600.0 );
    m_gateSeconds = std::max( gateSeconds, 0.0f );
    m_shortCoef = m_gateSeconds > 0.0f ? static_cast<float>( 1.0 - std::exp( -block / m_gateSeconds ) ) : 1.0f;
}

void BalanceMatch::reset()
{
    for ( BandMeter &m : m_left ) m.clear();
    for ( BandMeter &m : m_right ) m.clear();
    m_block.fill( 0.0 );
    m_avg.fill( 0.0 );
    m_trim.fill( 0.0f );
    m_loudest = 0.0;
    m_short = 0.0;
    m_count = 0;
    m_heard = false;
    m_eq.setGains( m_trim.data() );
    m_state.reset();
}

BalanceMatch::Learnt BalanceMatch::learnt() const
{
    Learnt l;
    l.m_avg = m_avg;
    l.m_trim = m_trim;
    l.m_loudest = m_loudest;
    l.m_short = m_short;
    l.m_heard = m_heard;
    return l;
}

void BalanceMatch::prime( const Learnt &learnt )
{
    if ( !learnt.m_heard ) return;
    m_avg = learnt.m_avg;
    m_trim = learnt.m_trim;
    m_loudest = learnt.m_loudest;
    m_short = learnt.m_short;
    m_heard = true;
    m_eq.setGains( m_trim.data() );
}

void BalanceMatch::setRanges( const float *lo, const float *hi )
{
    for ( int b = 0; b < Bands; ++b )
    {
        m_lo[static_cast<size_t>( b )] = lo[b];
        m_hi[static_cast<size_t>( b )] = hi[b];
    }
}

void BalanceMatch::set( float amount )
{
    m_amount = std::clamp( amount, 0.0f, 1.0f );
}

float BalanceMatch::share( int band ) const
{
    double total = 0.0;
    for ( double p : m_avg ) total += p;
    const size_t b = static_cast<size_t>( band );
    if ( !m_heard || total <= 1e-20 ) return -120.0f;
    return static_cast<float>( 10.0 * std::log10( std::max( m_avg[b], 1e-30 ) / total ) );
}

void BalanceMatch::aims( float *aim ) const
{
    float in[Bands];
    for ( int b = 0; b < Bands; ++b ) in[b] = share( b );
    float t[Bands] = {};
    for ( int r = 0; r < Rounds; ++r )
    {
        // The shares as they would come out with these trims: each band's power through the equaliser at its centre.
        double out[Bands], total = 0.0;
        for ( int b = 0; b < Bands; ++b )
        {
            out[b] = m_avg[static_cast<size_t>( b )] * m_eq.powerGain( t, Centres[b] );
            total += out[b];
        }
        if ( total <= 1e-30 ) break;
        for ( int b = 0; b < Bands; ++b )
        {
            const size_t i = static_cast<size_t>( b );
            const float  s = static_cast<float>( 10.0 * std::log10( std::max( out[b], 1e-30 ) / total ) );
            float        d = 0.0f;
            if ( s > m_hi[i] )
                d = m_hi[i] - s;
            else if ( s < m_lo[i] && in[b] > FloorDb )
                d = m_lo[i] - s;
            t[b] = std::clamp( t[b] + d, DownDb, UpDb );
        }
        if ( m_neutral )
        {   // a common offset that brings the whole's power back to where it was
            double before = 0.0, after = 0.0;
            for ( int b = 0; b < Bands; ++b )
            {
                before += m_avg[static_cast<size_t>( b )];
                after += m_avg[static_cast<size_t>( b )] * m_eq.powerGain( t, Centres[b] );
            }
            if ( before > 1e-30 && after > 1e-30 )
            {
                const float offset = static_cast<float>( -10.0 * std::log10( after / before ) );
                for ( int b = 0; b < Bands; ++b ) t[b] = std::clamp( t[b] + offset, DownDb, UpDb );
            }
        }
    }
    for ( int b = 0; b < Bands; ++b ) aim[b] = std::clamp( t[b] * m_amount, DownDb, UpDb );
}

void BalanceMatch::block()
{
    // The gate: this block counts if it is within GateDb of the loudest of about the last minute.
    double total = 0.0;
    for ( double p : m_block ) total += p;
    // What the gate compares: the block itself, or the loudness over m_gateSeconds (a passage, not a hit).
    double level = total;
    if ( m_gateSeconds > 0.0f )
    {
        m_short = m_short > 0.0 ? m_short + ( total - m_short ) * m_shortCoef : total;
        level = m_short;
    }
    m_loudest = std::max( m_loudest * m_fall, level );   // by default falling 1 dB in 6 s
    if ( total > 1e-12 && level >= m_loudest * std::pow( 10.0, -m_gateDb / 10.0 ) )
    {
        for ( size_t i = 0; i < m_avg.size(); ++i )
            m_avg[i] = m_heard ? m_avg[i] + ( m_block[i] - m_avg[i] ) * m_avgCoef : m_block[i];
        m_heard = true;
    }
    m_block.fill( 0.0 );
    // The aims, and the trims a step towards them.
    float aim[Bands] = {};
    if ( m_amount > 0.0f && m_heard ) aims( aim );
    bool moved = false;
    for ( int b = 0; b < Bands; ++b )
    {
        const size_t i = static_cast<size_t>( b );
        const float  step = std::clamp( aim[b] - m_trim[i], -m_glide, m_glide );
        if ( step != 0.0f )
        {
            m_trim[i] += step;
            moved = true;
        }
    }
    if ( moved ) m_eq.setGains( m_trim.data() );
}

void BalanceMatch::process( float *L, float *R, int n )
{
    if ( m_amount <= 0.0f && m_eq.flat() ) return;   // off, and every trim back at 0 dB
    for ( int done = 0; done < n; )
    {
        // Split at every block's end, so the trims change at the same sample whatever the host's block size.
        const int len = std::min( n - done, m_blockLength - m_count );
        for ( int i = 0; i < len; ++i )
        {
            const float l = L[done + i], r = R[done + i];
            for ( int b = 0; b < Bands; ++b )
            {
                const size_t k = static_cast<size_t>( b );
                const float  bl = m_left[k].run( l ), br = m_right[k].run( r );
                m_block[k] += static_cast<double>( bl ) * bl + static_cast<double>( br ) * br;
            }
        }
        m_eq.process( m_state, L + done, R + done, len );
        done += len;
        m_count += len;
        if ( m_count >= m_blockLength )
        {
            m_count = 0;
            block();
        }
    }
}

namespace
{
constexpr float FloorSubHz = 56.0f;     ///< the sub's upper edge, the bass band's lower
constexpr float FloorBassHz = 141.0f;   ///< the bass band's upper edge
constexpr float FloorStartDb = -12.0f;  ///< where both levels begin, before anything is heard
constexpr float FloorMinDb = -60.0f;    ///< the lowest level (silent there)
constexpr float FloorMaxDb = 6.0f;      ///< the highest
constexpr float FloorGlideDb = 0.25f;   ///< the levels' step per block (2.5 dB a second)
}   // namespace

void FloorMatch::prepare( double sampleRate )
{
    m_rate = sampleRate;
    for ( int c = 0; c < 2; ++c )
    {
        m_restMeter[0][c].prepare( 0.0f, FloorSubHz, sampleRate );
        m_restMeter[1][c].prepare( FloorSubHz, FloorBassHz, sampleRate );
        for ( int v = 0; v < Voices; ++v )
        {
            m_voiceMeter[v][0][c].prepare( 0.0f, FloorSubHz, sampleRate );
            m_voiceMeter[v][1][c].prepare( FloorSubHz, FloorBassHz, sampleRate );
        }
    }
    m_blockLength = std::max( 1, static_cast<int>( 0.1 * sampleRate ) );
    m_avgCoef = static_cast<float>( 1.0 - std::exp( -0.1 / 4.0 ) );
    reset();
}

void FloorMatch::reset()
{
    for ( auto &band : m_restMeter )
        for ( BandMeter &m : band ) m.clear();
    for ( auto &voice : m_voiceMeter )
        for ( auto &band : voice )
            for ( BandMeter &m : band ) m.clear();
    m_blockRest.fill( 0.0 );
    m_avgRest.fill( 0.0 );
    for ( int v = 0; v < Voices; ++v )
        for ( int k = 0; k < 3; ++k ) m_blockVoice[v][k] = m_avgVoice[v][k] = 0.0;
    m_db.fill( FloorStartDb );
    const float start = std::pow( 10.0f, FloorStartDb / 20.0f );
    m_from.fill( start );
    m_to.fill( start );
    m_loudest = 0.0;
    m_count = 0;
    m_heard = false;
}

void FloorMatch::setAims( float bassDb, float subDb )
{
    m_bassAim = std::pow( 10.0, bassDb / 10.0 );
    m_subAim = std::pow( 10.0, subDb / 10.0 );
}

void FloorMatch::solve( double &x, double &y ) const
{
    x = y = -1.0;
    const double R = m_avgRest[2];
    if ( R <= 1e-14 ) return;   // nothing to stand under: keep the levels
    const double d = m_subAim, b = m_bassAim;
    const double r1 = d * R - m_avgRest[0], r2 = b * R - m_avgRest[1];
    // The whole's sub share at d and its bass share at b, with x and y the voices' power gains:
    //   x (As - d At) + y (Bs - d Bt) = d R - Rs,   x (Ab - b At) + y (Bb - b Bt) = b R - Rb.
    const double *A = m_avgVoice[0], *B = m_avgVoice[1];
    const bool    haveA = A[2] > 1e-14, haveB = B[2] > 1e-14;
    // A voice alone serves the band it mostly fills; none at all where the rest holds that band already.
    auto alone = [&]( const double *V ) {
        if ( V[2] <= 1e-14 ) return 0.0;
        const bool   sub = V[0] > V[1];
        const double c = sub ? V[0] - d * V[2] : V[1] - b * V[2];
        const double r = sub ? r1 : r2;
        return c > 1e-30 ? std::max( r / c, 0.0 ) : 0.0;
    };
    if ( haveA && haveB )
    {
        const double a11 = A[0] - d * A[2], a12 = B[0] - d * B[2], a21 = A[1] - b * A[2], a22 = B[1] - b * B[2];
        const double det = a11 * a22 - a12 * a21;
        // Two voices on one octave fill the bands alike: no answer of their own (the first then carries it).
        if ( std::fabs( det ) > 1e-3 * ( std::fabs( a11 * a22 ) + std::fabs( a12 * a21 ) ) )
        {
            x = ( r1 * a22 - a12 * r2 ) / det;
            y = ( a11 * r2 - a21 * r1 ) / det;
            if ( x >= 0.0 && y >= 0.0 ) return;
        }
        if ( x < 0.0 && y >= 0.0 )
        {
            x = 0.0;
            y = alone( B );
            return;
        }
        // The first voice alone where it reaches the sub or the bass at all; else the second.
        if ( A[0] + A[1] > 0.1 * A[2] )
        {
            x = alone( A );
            y = 0.0;
        }
        else
        {
            x = 0.0;
            y = alone( B );
        }
        return;
    }
    x = haveA ? alone( A ) : 0.0;
    y = haveB ? alone( B ) : 0.0;
}

void FloorMatch::block()
{
    // The gate: on the rest, within 8 dB of its loudest block of about the last minute.
    const double total = m_blockRest[2];
    m_loudest = std::max( m_loudest * std::pow( 10.0, -0.1 / 60.0 ), total );
    if ( total > 1e-12 && total >= m_loudest * std::pow( 10.0, -GateDb / 10.0 ) )
    {
        for ( int k = 0; k < 3; ++k )
        {
            const size_t i = static_cast<size_t>( k );
            m_avgRest[i] = m_heard ? m_avgRest[i] + ( m_blockRest[i] - m_avgRest[i] ) * m_avgCoef : m_blockRest[i];
            for ( int v = 0; v < Voices; ++v )
                m_avgVoice[v][k] =
                    m_heard ? m_avgVoice[v][k] + ( m_blockVoice[v][k] - m_avgVoice[v][k] ) * m_avgCoef : m_blockVoice[v][k];
        }
        m_heard = true;
    }
    m_blockRest.fill( 0.0 );
    for ( int v = 0; v < Voices; ++v )
        for ( int k = 0; k < 3; ++k ) m_blockVoice[v][k] = 0.0;
    // The next block's ramp: from where this one ended, a step towards the aims.
    m_from = m_to;
    if ( !m_heard ) return;
    double gains[Voices];
    solve( gains[0], gains[1] );
    for ( int v = 0; v < Voices; ++v )
    {
        const size_t i = static_cast<size_t>( v );
        if ( gains[v] >= 0.0 )
        {
            const float aim = gains[v] > 1e-6
                                  ? std::clamp( static_cast<float>( 10.0 * std::log10( gains[v] ) ), FloorMinDb, FloorMaxDb )
                                  : FloorMinDb;
            m_db[i] += std::clamp( aim - m_db[i], -FloorGlideDb, FloorGlideDb );
        }
        m_to[i] = m_db[i] <= FloorMinDb + 0.01f ? 0.0f : std::pow( 10.0f, m_db[i] / 20.0f );
    }
}

void FloorMatch::process( const float *restL, const float *restR, const float *aL, const float *aR, const float *bL,
                          const float *bR, float *outL, float *outR, int n )
{
    const float *vl[Voices] = { aL, bL };
    const float *vr[Voices] = { aR, bR };
    const float  inv = 1.0f / static_cast<float>( m_blockLength );
    for ( int done = 0; done < n; )
    {
        // Split at every block's end: the levels ramp over a block, the same samples whatever the host's blocks.
        const int len = std::min( n - done, m_blockLength - m_count );
        for ( int j = 0; j < len; ++j )
        {
            const int   i = done + j;
            const float t = static_cast<float>( m_count + j + 1 ) * inv;
            const float l = restL[i], r = restR[i];
            float       sl = m_restMeter[0][0].run( l ), sr = m_restMeter[0][1].run( r );
            m_blockRest[0] += static_cast<double>( sl ) * sl + static_cast<double>( sr ) * sr;
            sl = m_restMeter[1][0].run( l );
            sr = m_restMeter[1][1].run( r );
            m_blockRest[1] += static_cast<double>( sl ) * sl + static_cast<double>( sr ) * sr;
            m_blockRest[2] += static_cast<double>( l ) * l + static_cast<double>( r ) * r;
            float oL = 0.0f, oR = 0.0f;
            for ( int v = 0; v < Voices; ++v )
            {
                const float x = vl[v][i], y = vr[v][i];
                float       ml = m_voiceMeter[v][0][0].run( x ), mr = m_voiceMeter[v][0][1].run( y );
                m_blockVoice[v][0] += static_cast<double>( ml ) * ml + static_cast<double>( mr ) * mr;
                ml = m_voiceMeter[v][1][0].run( x );
                mr = m_voiceMeter[v][1][1].run( y );
                m_blockVoice[v][1] += static_cast<double>( ml ) * ml + static_cast<double>( mr ) * mr;
                m_blockVoice[v][2] += static_cast<double>( x ) * x + static_cast<double>( y ) * y;
                const size_t k = static_cast<size_t>( v );
                const float  g = m_from[k] + ( m_to[k] - m_from[k] ) * t;
                oL += g * x;
                oR += g * y;
            }
            outL[i] = oL;
            outR[i] = oR;
        }
        done += len;
        m_count += len;
        if ( m_count >= m_blockLength )
        {
            m_count = 0;
            block();
        }
    }
}

}   // namespace phos
