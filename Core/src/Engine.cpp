/**
 * @file Engine.cpp
 * @brief Engine implementation.
 * @author Rene Weller
 */
#include "phos/Engine.h"
#include "phos/Patterns.h"
#include "phos/WaveTableFile.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace phos
{

const char *const StemNames[NumStems] = { "Kick", "Bass",  "Perc", "Acid",    "Lead",  "Counter", "Arp",    "Stab",
                                          "Pad",  "Drone", "Sfx",  "Texture", "Vocal", "Field",   "Returns" };

namespace
{
constexpr double PiD = 3.141592653589793;   ///< pi
/**
 * @brief Whether a curve takes whole steps (Int, Choice, Toggle).
 * @param[in] c  the curve
 * @return true for whole steps
 */
bool isDiscreteCurve( CurveE c )
{
    return c == CurveE::INT || c == CurveE::CHOICE || c == CurveE::TOGGLE;
}

// The gated hall's bar-line window (20.09.2026, round "reverb"; Reverb::barGate()). Fixed, not exposed
// as knobs: the brief names a result, not a set of controls. 8 ms is fast enough to read as a hard cut
// (a raised-cosine taper, so it never clicks) yet the measured floor is already 40+ dB down well inside
// it (Tests/selftest.cpp, testGatedReverb); 40 ms hold, 15 ms reopen -- 63 ms of the bar silenced out of
// 1.655 s at 145 BPM, so most of the bar still rings.
constexpr float HallGateCloseMs = 8.0f;   ///< the gated hall's close from the bar line, ms
constexpr float HallGateHoldMs = 40.0f;   ///< ... its hold at the floor, ms
constexpr float HallGateOpenMs = 15.0f;   ///< ... its reopening, ms

/**
 * @brief Clip with a quadratic knee: unity below 0.7 T, flat at T beyond 1.3 T, and a parabola in
 *        between that meets both with matching slope. Unlike tanh it leaves everything under the knee
 *        untouched, so it rounds the kick's transients without compressing the body of the mix.
 * @param[in] x  the input sample
 * @param[in] T  the ceiling
 * @return the output sample
 */
inline float kneeClip( float x, float T )
{
    constexpr float k = 0.7f;
    const float     a = std::fabs( x );
    if ( a <= k * T ) return x;
    const float y = a >= ( 2.0f - k ) * T
                        ? T
                        : T - ( a - ( 2.0f - k ) * T ) * ( a - ( 2.0f - k ) * T ) / ( 4.0f * ( 1.0f - k ) * T );
    return x < 0.0f ? -y : y;
}
}   // namespace

Engine::Engine() : m_notes( 16384 ), m_controls( 16384 ), m_immediate( 256 )
{
    retainFieldLoader();
    m_var = std::make_unique<Variation[]>( static_cast<size_t>( m_params.count() ) );
    m_eff.assign( static_cast<size_t>( m_params.count() ), 0.0f );
    m_raw.assign( static_cast<size_t>( m_params.count() ), 0.0f );
    m_rawKnob.assign( static_cast<size_t>( m_params.count() ), 0.0f );
    m_rawDirty.assign( static_cast<size_t>( m_params.count() ), 1 );
    // Which synth each parameter belongs to, for the own-sound switches (mix.*_own, 23.09.2026): -1 for none, and
    // for the parameters that are the arrangement's rather than the sound's -- the pad's and drone's high pass
    // (the sub foundation, rule 20) and the trance gate -- so an own sound still sits where the form puts it.
    m_ownOf.assign( static_cast<size_t>( m_params.count() ), -1 );
    auto mark = [&]( ModuleE m, int instance, int owner )
    {
        const int b = m_params.base( m, instance );
        for ( int k = 0; k < ParamStore::moduleCount( m ); ++k )
            m_ownOf[static_cast<size_t>( b + k )] = static_cast<int8_t>( owner );
    };
    mark( ModuleE::KICK, 0, 0 );
    mark( ModuleE::BASS, 0, 1 );
    mark( ModuleE::ACID, 0, 2 );
    for ( int v = 0; v < PolyInstances; ++v )
    {
        mark( ModuleE::POLY, v, 3 + v );
        const int b = m_params.base( ModuleE::POLY, v );
        for ( int k : { static_cast<int>( poly::HP_FLOOR ), static_cast<int>( poly::HP_TRACK ),
                        static_cast<int>( poly::GATE ), static_cast<int>( poly::GATE_PATTERN ),
                        static_cast<int>( poly::GATE_DEPTH ), static_cast<int>( poly::GATE_DUTY ),
                        static_cast<int>( poly::GATE_ATTACK ), static_cast<int>( poly::GATE_RELEASE ),
                        static_cast<int>( poly::GATE_TONE ), static_cast<int>( poly::LEVEL ) } )
            m_ownOf[static_cast<size_t>( b + k )] = -1;
    }
    for ( int &t : m_liveTarget ) t = -1;
}

int Engine::keyboardTarget( int channel ) const
{
    const int mb = m_params.base( ModuleE::MIX );
    const int part = static_cast<int>( std::lround( m_params.get( mb + mix::KEYBOARD_PART ) ) );
    if ( part <= 0 ) return -1;
    if ( part == 9 ) return 7;    // the bass (01.10.2026)
    if ( part == 10 ) return 8;   // the kit
    if ( part == 8 )
    {   // by channel: 1 acid, 2 lead ... 7 drone, 8 bass, 10 the kit (General MIDI's drums)
        if ( channel == 9 ) return 8;
        return channel >= 0 && channel < 8 ? channel : -1;
    }
    return std::min( part, 7 ) - 1;   // 0 acid, 1 .. 6 the polyphonic voices
}

void Engine::liveNoteOn( int pitch, int velocity, int channel, int part )
{
    if ( pitch < 0 || pitch > 127 ) return;
    // A note that names its voice (the split, 02.10.2026) goes there; mix.keyboard_part's values, By channel excepted.
    const int target = part == 9 ? 7 : part == 10 ? 8 : part >= 1 && part <= 7 ? part - 1 : keyboardTarget( channel );
    if ( target < 0 ) return;
    const float   vel = static_cast<float>( std::clamp( velocity, 1, 127 ) ) / 127.0f;
    constexpr int Held = 1 << 30;   // a gate that does not run out: the key's release ends the note
    if ( target == 8 )
    {
        // The kit (01.10.2026): C1 the kick, with the ducks a kick starts; C#1 .. C2 the twelve lanes, at their own pitch.
        if ( pitch == 36 )
        {
            m_kick.trigger( vel, 0.0 );
            m_bass.duck( 0.0 );
            for ( Ducker &d : m_duck ) d.trigger( 0.0 );
            m_returnDuck.trigger( 0.0 );
            m_subDuck.trigger( 0.0 );
        }
        else if ( pitch > 36 && pitch <= 36 + PercLanes )
        {
            m_perc.trigger( pitch - 37, vel, 0, 0.0 );
        }
        m_livePlayed |= 1u << target;
        return;   // one-shots: no release to wait for
    }
    if ( m_liveTarget[pitch] >= 0 ) liveNoteOff( pitch, channel );
    if ( target == 7 )
        m_bass.noteOn( pitch, vel, Held, 0.0, m_bassPhase );   // the bass (01.10.2026)
    else if ( target == 0 )
        m_acid.noteOn( pitch, vel, velocity > 110, true, Held, 0.0 );   // held keys slide, as a 303's legato does
    else
        m_poly[target - 1].noteOnLimited( pitch, vel, 1.0, Held, 0.0, false, true );
    m_liveTarget[pitch] = target;
    m_livePlayed |= 1u << target;
}

void Engine::liveNoteOff( int pitch, int /*channel*/ )
{
    if ( pitch < 0 || pitch > 127 || m_liveTarget[pitch] < 0 ) return;
    const int target = m_liveTarget[pitch];
    if ( target == 7 )
        m_bass.release();
    else if ( target == 0 )
        m_acid.noteOff( pitch );
    else
        m_poly[target - 1].noteOff( pitch );
    m_liveTarget[pitch] = -1;
}

void Engine::previewSfx( int choice, int preset )
{
    if ( choice < 0 || choice >= sfx::NumPresetChoices ) return;
    const double bps = m_beatsPerSample > 0.0 ? m_beatsPerSample : 145.0 / ( 60.0 * m_sr );
    const int    n = static_cast<int>( std::lround( static_cast<double>( PresetPreviewBeats[choice] ) / bps ) );
    m_sfx.trigger( PresetChoiceType[choice], std::max( 1, n ), 0.9f, 0.0, preset );
}

Engine::~Engine()
{
    releaseFieldLoader();
}

int Engine::takeMeters( float *peak, double *sumSq )
{
    for ( int k = 0; k < NumParts; ++k )
    {
        peak[k] = m_meterPeak[k];
        sumSq[k] = m_meterSum[k];
        m_meterPeak[k] = 0.0f;
        m_meterSum[k] = 0.0;
    }
    const int n = m_meterCount;
    m_meterCount = 0;
    return n;
}

namespace
{

/** @brief The five cues of distance, and a voice's plane (25.09.2026, the Dark-Ambient addon's table). */
struct DistanceCue
{
    float m_gainDb;      ///< the level, dB
    float m_lowPassHz;   ///< the low pass, Hz
    float m_wet;         ///< the plate send
    float m_width;       ///< the width
};

/**
 * @brief The cues at distance @p d (0 near .. 1 far): dB linear, Hz logarithmic, the rest linear between 0, 0.5, 1.
 * @param[in] d  the distance, 0..1
 * @return the cues
 */
DistanceCue distanceCue( float d )
{
    static const DistanceCue k[3] = { { 0.0f, 20000.0f, 0.05f, 0.15f },
                                      { -6.0f, 12000.0f, 0.20f, 0.50f },
                                      { -15.0f, 6000.0f, 0.60f, 1.0f } };
    const float              x = clampv( d, 0.0f, 1.0f ) * 2.0f;
    const int                i = std::min( 1, static_cast<int>( x ) );
    const float              u = x - static_cast<float>( i );
    const DistanceCue       &a = k[i];
    const DistanceCue       &b = k[i + 1];
    return { a.m_gainDb + u * ( b.m_gainDb - a.m_gainDb ), a.m_lowPassHz * std::pow( b.m_lowPassHz / a.m_lowPassHz, u ),
             a.m_wet + u * ( b.m_wet - a.m_wet ), a.m_width + u * ( b.m_width - a.m_width ) };
}

/** @brief Each voice's own plane, which its poly.distance default is (DefaultPoly): lead front, pad and drone back. */
constexpr float DistancePlane[PolyInstances] = { 0.0f, 0.5f, 0.5f, 0.5f, 1.0f, 1.0f };

/** @brief The free slow movement's periods, in beats: 13, 21 and 34 s at 145 BPM -- ratios of the golden mean. */
constexpr double SlowBeats[3] = { 31.4166, 50.8325, 82.2491 };

}   // namespace

void Engine::liveAllOff()
{
    for ( int k = 0; k < 128; ++k ) liveNoteOff( k, 0 );
    m_livePlayed = 0;
}

bool Engine::generatedSilenced( PartE part ) const
{
    if ( !m_live ) return false;   // a render or an export plays what was composed (01.10.2026)
    const int mb = m_params.base( ModuleE::MIX );
    if ( m_params.get( mb + mix::COMPOSER ) < 0.5f ) return true;   // the composer off: only what is played
    const int kbPart = static_cast<int>( std::lround( m_params.get( mb + mix::KEYBOARD_PART ) ) );
    if ( kbPart <= 0 || static_cast<int>( std::lround( m_params.get( mb + mix::KEYBOARD_MODE ) ) ) != 0 )
        return false;   // off, or Layer
    int target = -1;
    if ( part == PartE::ACID )
        target = 0;
    else if ( part == PartE::BASS )
        target = 7;
    else if ( part == PartE::KICK || part == PartE::PERC )
        target = 8;
    else if ( part == PartE::LEAD || part == PartE::COUNTER || part == PartE::ARP || part == PartE::STAB ||
              part == PartE::PAD || part == PartE::DRONE )
        target = 1 + polyOfPart( part );
    if ( target < 0 ) return false;
    if ( kbPart == 8 )
        return ( m_livePlayed >> target ) & 1u;   // by channel: a voice is the player's from its first played note
    if ( kbPart == 9 ) return target == 7;
    if ( kbPart == 10 ) return target == 8;
    return std::min( kbPart, 7 ) - 1 == target;
}

void Engine::prepare( double sampleRate, int /*maxBlockSize*/, const Quality &quality )
{
    m_sr = sampleRate;
    m_quality = quality;
    m_kickBuf.assign( static_cast<size_t>( Chunk ), 0.0f );
    m_bassBuf.assign( static_cast<size_t>( Chunk ), 0.0f );
    m_percL.assign( static_cast<size_t>( Chunk ), 0.0f );
    m_percR.assign( static_cast<size_t>( Chunk ), 0.0f );
    m_acidL.assign( static_cast<size_t>( Chunk ), 0.0f );
    m_acidR.assign( static_cast<size_t>( Chunk ), 0.0f );
    // The library's frame limit is read when the library is built, and that happens in the first
    // Poly::prepare() below -- for the whole process, whichever engine gets there first. The
    // desktop level asks for every frame (0), which is what the limit already is, so a desktop
    // build is untouched by this line; a Quest build has to get here before the first load, which
    // is why it sits in prepare() and not in the audio path.
    setWaveTableFrameLimit( m_quality.m_waveTableFrames );
    for ( int i = 0; i < PolyInstances; ++i )
    {
        m_polyL[i].assign( static_cast<size_t>( Chunk ), 0.0f );
        m_polyR[i].assign( static_cast<size_t>( Chunk ), 0.0f );
        m_poly[i].prepare( m_sr );
        m_poly[i].setQuality( m_quality.m_polyUnison[i], m_quality.m_polyVoices[i] );
    }
    // Whatever the parameters name right now -- a preset, a `--set`, a restored plugin state -- is
    // expanded here, on this thread. Anything the composer decides later it asks for itself.
    ensureVoiceTables();
    for ( auto *b : { &m_sfxL,        &m_sfxR,         &m_sfxWetL,      &m_sfxWetR,  &m_roomInL,  &m_roomInR,
                      &m_hallInL,     &m_hallInR,      &m_roomOutL,     &m_roomOutR, &m_hallOutL, &m_hallOutR,
                      &m_texL,        &m_texR,         &m_vocL,         &m_vocR,     &m_vocThrow, &m_subBuf,
                      &m_throwIn,     &m_sendL,        &m_sendR,        &m_bedSendL, &m_bedSendR, &m_hallGateInL,
                      &m_hallGateInR, &m_hallGateOutL, &m_hallGateOutR, &m_fieldL,   &m_fieldR } )
        b->assign( static_cast<size_t>( Chunk ), 0.0f );
    m_acid.prepare( m_sr );
    m_acid.setOversampling( m_quality.m_acidOversampling );
    m_sfx.prepare( m_sr );
    m_texture.prepare( m_sr );
    m_vocal.prepare( m_sr );   // loads the voice pack on the first call (Vocal.h); never on the audio thread
    // The field library (FieldLibrary.h): scanned here, never on the audio thread; the NASA shots asked for at once, so
    // that the first one the score calls is in memory (a shot that is not is left out, FieldShot::trigger).
    scanFieldLibrary();
    for ( int i = 0; i < fieldShotCount(); ++i ) requestFieldClip( fieldShotIndex( i ) );
    m_field.prepare( m_sr );
    m_shot.prepare( m_sr );
    m_sfxFx.prepare( m_sr );
    m_sendFx.prepare( m_sr );
    m_bedFx.prepare( m_sr );
    for ( int i = 0; i < PolyInstances; ++i )
    {
        m_polyFlanger[i].prepare( m_sr );
        m_polyPhaser[i].prepare( m_sr );
    }
    m_throw.prepare( m_sr );
    m_stutter.prepare( m_sr );
    m_subDuck.prepare( m_sr );
    for ( Ducker &d : m_duck ) d.prepare( m_sr );
    m_returnDuck.prepare( m_sr );
    for ( TranceGate &g : m_gate ) g.prepare( m_sr );
    m_room.prepare( m_sr );
    m_hall.prepare( m_sr );
    m_plate.prepare( m_sr );
    m_monLp1.setQ( 80.0f, 0.5412f, static_cast<float>( m_sr ) );   // Butterworth, fourth order: Q 0.5412 and 1.3066
    m_monLp2.setQ( 80.0f, 1.3066f, static_cast<float>( m_sr ) );
    for ( auto *v : { &m_plateInL, &m_plateInR, &m_plateOutL, &m_plateOutR } )
        v->assign( static_cast<size_t>( Chunk ), 0.0f );
    m_leadDuck.prepare( m_sr );
    m_padBandDuck.prepare( m_sr );
    m_padBandL.setQ( 1200.0f, 0.7f, static_cast<float>( m_sr ) );
    m_padBandR.setQ( 1200.0f, 0.7f, static_cast<float>( m_sr ) );
    m_hallGate.prepare( m_sr );
    // The gated hall's fixed recipe (20.09.2026, round "reverb"; docs/rounds/2026-09.md has the measurements).
    // Big hall, 4 s decay (the brief's "3-5 s"), no pre-delay so the gate's timing is exact from the
    // very first sample; the same return band as the plain hall's own default. Duck: -50 dBFS is a
    // floor well under any real send, so any voice that is actually sounding trips it -- 15 ms in
    // (fast enough to catch the attack), 300 ms out, the "opens as the voice's envelope releases" time
    // constant the brief asks to report; 0.7 depth pulls the send about 10.5 dB down while it holds.
    m_hallGate.set( 1.8f, 4.0f, 0.35f, 0.0f, 200.0f, 9000.0f );
    m_hallGate.setDuck( 0.7f, -50.0f, 0.015f, 0.30f );
    m_comp.prepare( m_sr );
    m_bandLimitL.prepare( m_sr );
    m_bandLimitR.prepare( m_sr );
    m_limiter.prepare( m_sr, 1.5f );
    {
        const HalfbandDesign d = designHalfband( 96.0, 0.1 );
        m_clipUpL.setup( d );
        m_clipUpR.setup( d );
        m_clipDownL.setup( d );
        m_clipDownR.setup( d );
    }
    m_meter.prepare( m_sr );
    m_kick.prepare( m_sr );
    m_bass.prepare( m_sr );
    m_bass.setOversampling( m_quality.m_bassOversampling );
    m_perc.prepare( m_sr );
    reset();
}

void Engine::ensureVoiceTables()
{
    int want[PolyInstances];
    int n = 0;
    for ( int i = 0; i < PolyInstances; ++i )
    {
        const int t =
            static_cast<int>( std::lround( m_params.get( m_params.base( ModuleE::POLY, i ) + poly::TABLE ) ) );
        if ( t >= NumBuiltinWaveTables && t < NumWaveTables ) want[n++] = t;
    }
    if ( n > 0 ) ensureWaveTables( want, n );
}

void Engine::reset()
{
    NoteEvent e;
    while ( m_notes.pop( e ) )
    {
    }
    ControlEvent c;
    while ( m_controls.pop( c ) )
    {
    }
    while ( m_immediate.pop( c ) )
    {
    }
    for ( int i = 0; i < m_params.count(); ++i ) m_var[static_cast<size_t>( i )] = Variation{};
    m_rawValid = false;   // every offset and override is gone: recompute every value
    m_paramsPending = false;
    for ( int &t : m_liveTarget ) t = -1;   // the voices are cleared below; no played note is held any more
    m_livePlayed = 0;
    m_samples = 0;
    m_chunkBeat = 0.0;
    m_chunkPos = 0;
    m_slotBeats = -1.0;
    m_beatsPerSample = 0.0;
    m_beatNow.store( 0.0, std::memory_order_relaxed );
    m_kick.reset();
    m_bass.reset();
    m_perc.reset();
    m_acid.reset();
    for ( Poly &p : m_poly ) p.reset();
    // Distinct, fixed phase streams per instance: a render is the same every time.
    for ( int i = 0; i < PolyInstances; ++i )
        m_poly[i].seedPhases( 0x9E3779B97F4A7C15ull * static_cast<uint64_t>( i + 1 ) );
    m_clipL.reset();
    m_clipR.reset();
    m_sfx.reset();
    m_texture.reset();
    m_vocal.reset();
    m_field.reset();
    m_shot.reset();
    m_sfxFx.reset();
    m_sendFx.reset();
    m_bedFx.reset();
    for ( int i = 0; i < PolyInstances; ++i )
    {
        m_polyFlanger[i].reset();
        m_polyPhaser[i].reset();
    }
    m_throw.reset();
    m_stutter.reset();
    m_subDuck.reset();
    m_sfxShift = Motion{};
    m_sfxFlange = Motion{};
    m_sendMotion = Motion{};
    for ( Ducker &d : m_duck ) d.reset();
    m_returnDuck.reset();
    for ( TranceGate &g : m_gate ) g.reset();
    m_room.reset();
    m_hall.reset();
    m_plate.reset();
    m_leadDuck.reset();
    m_padBandDuck.reset();
    m_padBandL.m_ic1 = m_padBandL.m_ic2 = 0.0f;
    m_padBandR.m_ic1 = m_padBandR.m_ic2 = 0.0f;
    for ( int k = 0; k < PolyInstances; ++k )
        m_distLpL[k].m_ic1 = m_distLpL[k].m_ic2 = m_distLpR[k].m_ic1 = m_distLpR[k].m_ic2 = 0.0f;
    m_monLp1.m_ic1 = m_monLp1.m_ic2 = m_monLp2.m_ic1 = m_monLp2.m_ic2 = 0.0f;
    m_hallGate.reset();
    m_comp.reset();
    m_sideHp1.reset();
    m_sideHp2.reset();
    m_bandLimitL.reset();
    m_bandLimitR.reset();
    m_limiter.reset();
    m_clipUpL.reset();
    m_clipUpR.reset();
    m_clipDownL.reset();
    m_clipDownR.reset();
    m_meter.reset();
}

float Engine::effective( int id ) const
{
    return id >= 0 && id < static_cast<int>( m_eff.size() ) ? m_eff[static_cast<size_t>( id )] : 0.0f;
}

void Engine::playedValues( float *out ) const
{
    const ParamStore &p = m_params;
    for ( int id = 0; id < p.count(); ++id ) out[id] = m_eff[static_cast<size_t>( id )];
    auto add = [&]( int id, float delta )
    {
        if ( id < 0 ) return;
        const ParamDesc &d = p.desc( id );
        out[id] = std::clamp( out[id] + delta, d.m_minValue, d.m_maxValue );
    };
    auto mul = [&]( int id, float factor )
    {
        if ( id < 0 ) return;
        const ParamDesc &d = p.desc( id );
        out[id] = std::clamp( out[id] * factor, d.m_minValue, d.m_maxValue );
    };
    auto sum = []( const float *s, ModDestE d ) { return s[static_cast<int>( d )]; };
    // The same arithmetic as the synths (Poly.cpp applyModulation and lowPassCoefs, Bass.cpp, Acid.cpp): the cutoff in
    // octaves, level and FM index as factors, the rest added.
    auto show = [&]( const float *s, int cutoff, int res, int mode, int pw, int fm, int pos, int level, int pan )
    {
        if ( s == nullptr ) return;
        mul( cutoff, std::pow( 2.0f, sum( s, ModDestE::CUTOFF ) ) );
        add( res, sum( s, ModDestE::RESONANCE ) );
        add( mode, sum( s, ModDestE::FILTER_MODE ) );
        add( pw, sum( s, ModDestE::PULSE_WIDTH ) );
        mul( fm, std::clamp( 1.0f + sum( s, ModDestE::FM_INDEX ), 0.0f, 2.0f ) );
        add( pos, sum( s, ModDestE::TABLE_POS ) );
        if ( level >= 0 )
            add( level, 20.0f * std::log10(
                                    std::max( 1.0e-4f, std::clamp( 1.0f + sum( s, ModDestE::LEVEL ), 0.0f, 2.0f ) ) ) );
        add( pan, sum( s, ModDestE::PAN ) );
    };
    for ( int i = 0; i < PolyInstances; ++i )
    {
        const int b = p.base( ModuleE::POLY, i );
        show( m_poly[i].displayModulation(), b + poly::CUTOFF, b + poly::RESONANCE, b + poly::FILTER_MODE,
              b + poly::PULSE_WIDTH, b + poly::FM_INDEX, b + poly::POSITION, b + poly::LEVEL, b + poly::PAN );
    }
    const int bb = p.base( ModuleE::BASS ), ab = p.base( ModuleE::ACID );
    show( m_bass.displayModulation(), bb + bass::CUTOFF, bb + bass::RESONANCE, bb + bass::FILTER_MODE,
          bb + bass::PULSE_WIDTH, -1, -1, bb + bass::LEVEL, -1 );
    show( m_acid.displayModulation(), ab + acid::CUTOFF, ab + acid::RESONANCE, -1, -1, -1, -1, ab + acid::LEVEL, -1 );
}

void Engine::advanceRamps()
{
    for ( int i = 0; i < m_params.count(); ++i )
    {
        Variation &v = m_var[static_cast<size_t>( i )];
        if ( v.m_length <= 0.0 ) continue;
        const double x = ( m_chunkBeat - v.m_start ) / v.m_length;
        m_rawDirty[static_cast<size_t>( i )] = 1;
        if ( x >= 1.0 )
        {
            v.m_offset = v.m_to;
            v.m_length = 0.0;
            continue;
        }
        const double s = x <= 0.0 ? 0.0 : 0.5 - 0.5 * std::cos( PiD * x );   // raised cosine: C1 at both ends
        v.m_offset = v.m_from + static_cast<float>( s ) * ( v.m_to - v.m_from );
    }
}

void Engine::dispatchControl( const ControlEvent &e )
{
    if ( e.m_kind == ControlEvent::KindE::BASS_SLOT )
    {
        // The composer says where the first bass note of this beat is (Score.h). It arrives at the
        // beat itself, before that beat's kick, because controls are dispatched before notes at
        // equal beats -- so Kick::constrain and setPhaseTarget below see the gap this kick really
        // has, and m_bassPhase is the kick's phase at the instant the note really starts.
        m_slotBeats = static_cast<double>( e.m_value );
        applyParams();
        return;
    }
    if ( e.m_param < 0 || e.m_param >= m_params.count() ) return;
    m_rawDirty[static_cast<size_t>( e.m_param )] = 1;
    Variation &v = m_var[static_cast<size_t>( e.m_param )];
    if ( e.m_kind == ControlEvent::KindE::OVERRIDE )
    {
        v.m_override = e.m_value;
    }
    else if ( e.m_kind == ControlEvent::KindE::BASE )
    {
        // A negative length clears it (Score.h); a value is held inside the knob's range, as the knob itself is.
        const ParamDesc &d = m_params.desc( e.m_param );
        v.m_base = e.m_length < 0.0f ? std::numeric_limits<float>::quiet_NaN()
                                     : std::clamp( e.m_value, d.m_minValue, d.m_maxValue );
    }
    else if ( e.m_length <= 0.0f )
    {
        v.m_offset = e.m_value;
        v.m_length = 0.0;
    }
    else
    {
        v.m_from = v.m_offset;
        v.m_to = e.m_value;
        v.m_start = e.m_beat;
        v.m_length = e.m_length;
    }
    // At a chunk's first sample the pass also reads the tempo, which times every later event of the chunk, so it
    // runs now, as it always did. Anywhere else the events of one instant are applied together, once
    // (flushParams(), before the next note, render or chunk): a track start is hundreds of events at one beat.
    if ( m_chunkPos == 0 || m_beatsPerSample <= 0.0 )
        applyParams();
    else
        m_paramsPending = true;
}

double Engine::firstSlotSeconds() const
{
    // The slot the composer sent for this beat, or -- while none has been sent -- the one the bass
    // pattern family implies, which is what this function always returned (Engine.h, m_slotBeats).
    const double beats = m_slotBeats > 0.0 ? m_slotBeats : firstBassSlot( m_pattern );
    return beats / ( m_beatsPerSample * m_sr );
}

void Engine::applyParams()
{
    const ParamStore &p = m_params;
    m_paramsPending = false;
    // The own-sound switches, read once (23.09.2026): kick, bass, acid, then the six polyphonic voices. A switch
    // that moved changes every value it governs, so those are recomputed however their knobs stand.
    bool own[9];
    bool ownMoved[9];
    for ( int k = 0; k < 9; ++k )
    {
        own[k] = p.get( p.base( ModuleE::MIX ) + mix::KICK_OWN + k ) >= 0.5f;
        ownMoved[k] = !m_rawValid || own[k] != m_ownSeen[k];
        m_ownSeen[k] = own[k];
    }
    // Effective values: knob + normalised offset for continuous parameters, override for discrete. Only where
    // something they are made of moved (Engine.h, m_raw); the rest keep last pass's value, which is the same number.
    // A polyphonic voice whose values all stayed is not updated either (below): Poly::update only derives
    // coefficients from its inputs, so the same inputs are the same state.
    const bool full = !m_rawValid;
    const int  polyFirst = p.base( ModuleE::POLY, 0 ), polyCount = ParamStore::moduleCount( ModuleE::POLY );
    bool       polyMoved[PolyInstances] = {};
    auto       store = [&]( int i, float value )
    {
        const size_t si = static_cast<size_t>( i );
        // Bit for bit, not ==: -0.0f == 0.0f, and keeping the old zero's sign changed the render.
        if ( !full && std::memcmp( &value, &m_raw[si], sizeof value ) == 0 ) return;
        m_raw[si] = value;
        if ( i >= polyFirst && i < polyFirst + PolyInstances * polyCount )
            polyMoved[( i - polyFirst ) / polyCount] = true;
    };
    for ( int i = 0; i < p.count(); ++i )
    {
        const size_t si = static_cast<size_t>( i );
        const float  knob = p.get( i );
        const int    owner = m_ownOf[si];
        if ( m_rawValid && !m_rawDirty[si] && knob == m_rawKnob[si] && ( owner < 0 || !ownMoved[owner] ) ) continue;
        m_rawDirty[si] = 0;
        m_rawKnob[si] = knob;
        const Variation &v = m_var[si];
        float            value = knob;
        // An own sound (mix.*_own, 23.09.2026): the synth plays its knobs, whatever the composer's recipes say.
        if ( owner >= 0 && own[owner] )
        {
            store( i, knob );
            continue;
        }
        if ( isDiscreteCurve( p.desc( i ).m_curve ) )
        {
            if ( v.m_override >= 0.0f ) value = v.m_override;
        }
        else
        {
            // A base value (a preset of the composer's, 26.09.2026) stands in for the knob; the offset rides on it.
            const float basis = std::isnan( v.m_base ) ? knob : v.m_base;
            value = v.m_offset != 0.0f ? p.fromNormalised( i, p.toNormalised( i, basis ) + v.m_offset ) : basis;
        }
        store( i, value );
    }
    m_rawValid = true;
    std::copy( m_raw.begin(), m_raw.end(), m_eff.begin() );   // Kick::constrain below edits the kick's copy in place

    const int cb = p.base( ModuleE::COMPOSE );
    m_keyRoot = static_cast<int>( std::lround( m_eff[static_cast<size_t>( cb + compose::KEY )] ) );
    m_pattern = static_cast<int>( std::lround( m_eff[static_cast<size_t>( cb + compose::BASS_PATTERN )] ) );
    m_scale = static_cast<int>( std::lround( m_eff[static_cast<size_t>( cb + compose::SCALE )] ) );
    for ( int l = 0; l < PercLanes; ++l )
        m_perc.update( l, m_eff.data() + p.base( ModuleE::PERC, l ), m_keyRoot, m_scale );
    // The tempo is read at chunk starts only: a control event inside a chunk must not change the
    // beats per sample the rest of that chunk is timed with.
    if ( m_chunkPos == 0 || m_beatsPerSample <= 0.0 )
    {
        const double bpm = m_useTempoMap ? m_tempo.bpmAt( m_chunkBeat )
                                         : static_cast<double>( m_eff[static_cast<size_t>( cb + compose::BPM )] );
        m_beatsPerSample = bpm / 60.0 / m_sr;
    }

    float       *kv = m_eff.data() + p.base( ModuleE::KICK );
    float       *bv = m_eff.data() + p.base( ModuleE::BASS );
    const double slot = firstSlotSeconds();
    Kick::constrain( kv, slot, m_keyRoot );
    m_kick.update( kv, m_keyRoot );
    m_bass.update( bv );
    const double bpmNow = m_beatsPerSample * 60.0 * m_sr;
    // The kit's auto-pan counts its period in bars (perc.pan_bars), so it needs the bar length. The
    // tempo here is the one this chunk is timed with (read at chunk starts only, above), so the pan
    // period follows a tempo ramp between tracks without ever changing inside a chunk.
    m_perc.setTempo( bpmNow );
    m_acid.update( m_eff.data() + p.base( ModuleE::ACID ), bpmNow );
    for ( int i = 0; i < PolyInstances; ++i )
    {
        if ( !full && !polyMoved[i] && m_polyBpm[i] == bpmNow && !m_poly[i].tableStale() )
        {
            m_poly[i].refreshEnvelopeTimes();   // the one thing update() does even with the same inputs (Poly.h)
            continue;
        }
        m_poly[i].update( m_eff.data() + p.base( ModuleE::POLY, i ), bpmNow );
        m_polyBpm[i] = bpmNow;
    }

    // The kick lock: either the kick's tail meets the bass's own start phase, or the bass starts
    // where the kick's phase is when the first bass note begins.
    m_lockMode = static_cast<int>( std::lround( bv[bass::KICK_LOCK] ) );
    if ( m_lockMode == static_cast<int>( KickLockE::KICK_FOLLOWS_BASS ) )
    {
        m_kick.setPhaseTarget( slot, m_bass.knobPhase() );
        m_bassPhase = m_bass.knobPhase();
    }
    else
    {
        m_kick.setPhaseTarget( 0.0, 0.0 );
        if ( m_lockMode == static_cast<int>( KickLockE::BASS_FOLLOWS_KICK ) )
        {
            const double ph = m_kick.outputPhaseAt( slot );
            m_bassPhase = ph - std::floor( ph );
        }
        else
        {
            m_bassPhase = m_bass.knobPhase();
        }
    }

    const int mb = p.base( ModuleE::MIX );
    m_kickMute = m_eff[static_cast<size_t>( mb + mix::KICK_MUTE )] >= 0.5f;
    m_bassMute = m_eff[static_cast<size_t>( mb + mix::BASS_MUTE )] >= 0.5f;
    m_percMute = m_eff[static_cast<size_t>( mb + mix::PERC_MUTE )] >= 0.5f;
    m_percGain = m_percMute ? 0.0f : dbToGain( m_eff[static_cast<size_t>( mb + mix::PERC_LEVEL )] );
    auto e = [&]( int id ) { return m_eff[static_cast<size_t>( id )]; };
    auto partGain = [&]( int mute, int level ) { return e( mb + mute ) >= 0.5f ? 0.0f : dbToGain( e( mb + level ) ); };
    const int ab = p.base( ModuleE::ACID ), sb = p.base( ModuleE::SFX ), fb = p.base( ModuleE::FX );
    m_stripGain[STRIP_PERC] = m_percGain;
    m_stripGain[STRIP_ACID] = partGain( mix::ACID_MUTE, mix::ACID_LEVEL );
    for ( int k = 0; k < PolyInstances; ++k )
        m_stripGain[STRIP_LEAD + k] = partGain( mix::polyMute( static_cast<PolyInstanceE>( k ) ),
                                                mix::polyLevel( static_cast<PolyInstanceE>( k ) ) );
    m_stripGain[STRIP_SFX] = partGain( mix::SFX_MUTE, mix::SFX_LEVEL );
    const float duckA = e( mb + mix::DUCK_ATTACK ), duckH = e( mb + mix::DUCK_HOLD ),
                duckR = e( mb + mix::DUCK_RELEASE );
    // 25.09.2026, the mix guide's matrix: the lines come back faster than the pads and the bed.
    const float duckRLines = e( mb + mix::DUCK_RELEASE_LINES );
    m_stripRoom[STRIP_PERC] = e( mb + mix::PERC_ROOM );
    m_stripHall[STRIP_PERC] = e( mb + mix::PERC_HALL );
    m_duck[STRIP_PERC].set( 0.0f, duckA, duckH, duckR );
    m_stripRoom[STRIP_ACID] = e( ab + acid::ROOM_SEND );
    m_stripHall[STRIP_ACID] = e( ab + acid::HALL_SEND );
    m_duck[STRIP_ACID].set( e( ab + acid::DUCK ), duckA, duckH, duckRLines );
    m_stripPlate[STRIP_ACID] = e( ab + acid::PLATE_SEND );
    m_hallGateOn[STRIP_ACID] = e( ab + acid::HALL_GATE ) >= 0.5f;
    m_stripRoom[STRIP_SFX] = e( sb + sfx::ROOM_SEND );
    m_stripHall[STRIP_SFX] = e( sb + sfx::HALL_SEND );
    m_duck[STRIP_SFX].set( e( sb + sfx::DUCK ), duckA, duckH, duckR );
    m_sfx.update( m_eff.data() + sb, m_keyRoot );
    m_sfx.setScale( m_scale );
    // The psychedelic layer (Engine.h): the bed's and the voices' strips, their sends, the modulation
    // chains, the delay throw and the sub drop's own duck.
    {
        const int tb = p.base( ModuleE::TEXTURE ), vb = p.base( ModuleE::VOCAL ), xb = p.base( ModuleE::PSY_FX );
        m_stripGain[STRIP_TEXTURE] = partGain( mix::TEXTURE_MUTE, mix::TEXTURE_LEVEL );
        m_stripGain[STRIP_VOCAL] = partGain( mix::VOCAL_MUTE, mix::VOCAL_LEVEL );
        m_stripRoom[STRIP_TEXTURE] = e( tb + texture::ROOM_SEND );
        m_stripHall[STRIP_TEXTURE] = e( tb + texture::HALL_SEND );
        m_stripFx[STRIP_TEXTURE] = e( tb + texture::FX_SEND );
        m_duck[STRIP_TEXTURE].set( e( tb + texture::DUCK ), duckA, duckH, duckR );
        m_stripRoom[STRIP_VOCAL] = 0.0f;
        m_stripHall[STRIP_VOCAL] = e( vb + vocal::HALL_SEND );
        m_stripFx[STRIP_VOCAL] = e( vb + vocal::FX_SEND );
        m_duck[STRIP_VOCAL].set( e( vb + vocal::DUCK ), duckA, duckH, duckR );
        m_texture.update( m_eff.data() + tb, m_keyRoot );
        m_vocal.update( m_eff.data() + vb, m_keyRoot );
        // The Field track's strip (27.09.2026): sends and duck on the sampler's own knobs, like the bed's.
        const int flb = p.base( ModuleE::FIELD );
        m_stripGain[STRIP_FIELD] = partGain( mix::FIELD_MUTE, mix::FIELD_LEVEL );
        m_stripRoom[STRIP_FIELD] = e( flb + field::ROOM_SEND );
        m_stripPlate[STRIP_FIELD] = e( flb + field::PLATE_SEND );
        m_stripHall[STRIP_FIELD] = e( flb + field::HALL_SEND );
        m_stripFx[STRIP_FIELD] = 0.0f;
        m_duck[STRIP_FIELD].set( e( flb + field::DUCK ), duckA, duckH, duckR );
        m_field.update( m_eff.data() + flb );
        m_sfxFx.update( m_eff.data() + xb );
        m_sendFx.update( m_eff.data() + xb );
        m_bedFx.update( m_eff.data() + xb );
        m_sendReturn = dbToGain( e( xb + psyfx::RETURN ) );
        m_motion = e( xb + psyfx::MOTION );
        m_throwSend = e( vb + vocal::THROW_SEND );
        const int tIdx =
            std::clamp( static_cast<int>( std::lround( e( vb + vocal::THROW_BEATS ) ) ), 0, NumDelayTimes - 1 );
        // The throw: the knob's time on the left, the next shorter one on the right (a dotted eighth
        // against a quarter at the default), so the echoes of a word bounce across the field; a band of
        // 300 Hz .. 3.5 kHz inside the loop thins them out like a radio instead of piling up body.
        m_throw.set( DelayBeats[tIdx], DelayBeats[std::max( 0, tIdx - 1 )],
                     m_beatsPerSample > 0.0 ? m_beatsPerSample * 60.0 * m_sr : 145.0, e( vb + vocal::THROW_FEEDBACK ),
                     300.0f, 3500.0f );
        // The kick's hold on the sub drop: the kit's duck times, 40 ms more hold for the kick's body.
        m_subDuck.set( e( sb + sfx::SUB_DUCK ), duckA, duckH + 40.0f, duckR );
    }
    const double beatSeconds = m_beatsPerSample > 0.0 ? 1.0 / ( m_beatsPerSample * m_sr ) : 60.0 / 145.0;
    // The gated hall's bar-line window (Reverb::barGate() takes beats, not seconds, for the same reason
    // poly.gate_attack/_release do above: it stays the same fraction of a bar under a tempo ramp).
    m_hallGateCloseBeats = static_cast<double>( HallGateCloseMs ) * 0.001 / beatSeconds;
    m_hallGateHoldBeats = static_cast<double>( HallGateHoldMs ) * 0.001 / beatSeconds;
    m_hallGateOpenBeats = static_cast<double>( HallGateOpenMs ) * 0.001 / beatSeconds;
    for ( int k = 0; k < PolyInstances; ++k )
    {
        const int pb = p.base( ModuleE::POLY, k );
        const int strip = STRIP_LEAD + k;
        m_stripRoom[strip] = e( pb + poly::ROOM_SEND );
        m_stripHall[strip] = e( pb + poly::HALL_SEND );
        m_stripPlate[strip] = e( pb + poly::PLATE_SEND );   // the distance's wet share is added after the table below
        const bool line = strip <= STRIP_STAB;              // lead, counter, arp, stab: the lines' release
        m_duck[strip].set( e( pb + poly::DUCK ), duckA, duckH, line ? duckRLines : duckR );
        m_hallGateOn[strip] = e( pb + poly::HALL_GATE ) >= 0.5f;
        m_gateOn[k] = e( pb + poly::GATE ) >= 0.5f;
        m_gatePattern[k] = static_cast<int>( std::lround( e( pb + poly::GATE_PATTERN ) ) );
        m_gateDepth[k] = e( pb + poly::GATE_DEPTH );
        m_gateDuty[k] = e( pb + poly::GATE_DUTY );
        m_gateTone[k] = e( pb + poly::GATE_TONE );
        m_gateAttack[k] = e( pb + poly::GATE_ATTACK ) * 0.001 / beatSeconds;
        m_gateRelease[k] = e( pb + poly::GATE_RELEASE ) * 0.001 / beatSeconds;
        // 20.09.2026, round "dialogue": the voice's own modulation insert (PsyFx.h). A comb is the
        // flanger with its sweep stopped, so both share one object; the type decides which of the two
        // runs and whether either runs at all. Both are tempo-synchronised by construction (their LFO
        // reads the absolute beat), so the period stays a period in bars however the tempo ramps.
        m_polyMod[k] = static_cast<int>( std::lround( e( pb + poly::MOD ) ) );
        const float mix = e( pb + poly::MOD_MIX );
        const bool  comb = m_polyMod[k] == static_cast<int>( PolyModE::COMB );
        m_polyFlanger[k].set( e( pb + poly::MOD_BEATS ), comb ? 0.0f : e( pb + poly::MOD_DEPTH ),
                              e( pb + poly::MOD_FEEDBACK ),
                              m_polyMod[k] == static_cast<int>( PolyModE::FLANGER ) || comb ? mix : 0.0f );
        m_polyPhaser[k].set( e( pb + poly::MOD_BEATS ), e( pb + poly::MOD_DEPTH ), e( pb + poly::MOD_FEEDBACK ),
                             m_polyMod[k] == static_cast<int>( PolyModE::PHASER ) ? mix : 0.0f );
    }
    // Send effects: the hall's pre-delay follows the tempo.
    // 25.09.2026, the Dark-Ambient addon: every room with its own send filter; the pre-delays are distance cues in
    // milliseconds (front long, back short), the hall's the one that stays in beats (its default is ~5 ms).
    m_room.set( e( fb + fx::ROOM_SIZE ), e( fb + fx::ROOM_DECAY ), e( fb + fx::ROOM_DAMPING ),
                static_cast<float>( e( fb + fx::ROOM_PRE_DELAY ) * 0.001 * m_sr ), e( fb + fx::ROOM_LOW_CUT ),
                e( fb + fx::ROOM_HIGH_CUT ) );
    m_hall.set( e( fb + fx::HALL_SIZE ), e( fb + fx::HALL_DECAY ), e( fb + fx::HALL_DAMPING ),
                static_cast<float>( e( fb + fx::HALL_PRE_DELAY ) * beatSeconds * m_sr ), e( fb + fx::LOW_CUT ),
                e( fb + fx::HIGH_CUT ) );
    m_roomReturn = dbToGain( e( fb + fx::ROOM_RETURN ) );
    m_hallReturn = dbToGain( e( fb + fx::HALL_RETURN ) );
    m_returnDuck.set( e( fb + fx::RETURN_DUCK ), duckA, duckH, e( fb + fx::RETURN_DUCK_RELEASE ) );
    m_plate.set( e( fb + fx::PLATE_SIZE ), e( fb + fx::PLATE_DECAY ), e( fb + fx::PLATE_DAMPING ),
                 static_cast<float>( e( fb + fx::PLATE_PRE_DELAY ) * 0.001 * m_sr ), e( fb + fx::PLATE_LOW_CUT ),
                 e( fb + fx::PLATE_HIGH_CUT ) );
    m_plateReturn = dbToGain( e( fb + fx::PLATE_RETURN ) );
    m_plateToHall = e( fb + fx::PLATE_TO_HALL );
    m_stripPlate[STRIP_SFX] = e( sb + sfx::PLATE_SEND );
    for ( int k = 0; k < PolyInstances; ++k )
    {
        const int         pb = p.base( ModuleE::POLY, k );
        const DistanceCue at = distanceCue( e( pb + poly::DISTANCE ) ), plane = distanceCue( DistancePlane[k] );
        m_distGain[k] = dbToGain( at.m_gainDb - plane.m_gainDb );
        m_distWet[k] = at.m_wet - plane.m_wet;
        // The width cue as a side gain against the plane's, at most doubled or halved: a lead far back is wider, but
        // six times its own side (0.15 against 1) would make it a different sound, not a farther one.
        m_distWidth[k] = clampv( at.m_width / plane.m_width, 0.5f, 2.0f );
        m_distLpOn[k] = at.m_lowPassHz < plane.m_lowPassHz * 0.999f;
        if ( m_distLpOn[k] )
        {
            m_distLpL[k].setQ( at.m_lowPassHz, 0.7071f, static_cast<float>( m_sr ) );
            m_distLpR[k].copyCoefficients( m_distLpL[k] );
        }
        m_slowWidth[k] = e( pb + poly::SLOW_MOD );
    }
    m_monitor = static_cast<int>( std::lround( e( p.base( ModuleE::MASTER ) + master::MONITOR ) ) );
    m_counterDuck = e( mb + mix::COUNTER_DUCK );
    m_leadDuck.set( m_counterDuck, 2.0f, 20.0f, 50.0f );
    m_padBandCut = dbToGain( -e( mb + mix::PAD_LEAD_DUCK ) );

    const int ms = p.base( ModuleE::MASTER );
    m_masterGain = dbToGain( e( ms + master::GAIN ) + e( mb + mix::TRACK_GAIN ) );
    m_ceiling = dbToGain( e( ms + master::CEILING ) );
    m_clip = e( ms + master::CLIP ) >= 0.5f;
    m_comp.set( e( ms + master::COMP_THRESHOLD ), e( ms + master::COMP_RATIO ), e( ms + master::COMP_KNEE ),
                e( ms + master::COMP_ATTACK ), e( ms + master::COMP_RELEASE ) );
    m_sideHp1.setQ( e( ms + master::MONO_BASS ), 0.70710678f, static_cast<float>( m_sr ) );
    m_sideHp2.copyCoefficients( m_sideHp1 );
    const bool lim = e( ms + master::LIMITER ) >= 0.5f;
    if ( lim != m_limiterOn ) m_limiter.reset();
    m_limiterOn = lim;
    m_limiter.set( e( ms + master::CEILING ), e( ms + master::LIMITER_RELEASE ) );
    m_clipperOn = e( ms + master::CLIPPER ) >= 0.5f;
    m_clipperT = dbToGain( e( ms + master::CEILING ) + e( ms + master::CLIPPER_THRESHOLD ) );
}

int Engine::jamPitch( PartE part, int pitch ) const
{
    switch ( part )
    {
    case PartE::BASS:
    case PartE::ACID:
    case PartE::LEAD:
    case PartE::COUNTER:
    case PartE::ARP:
    case PartE::STAB:
    case PartE::PAD:
    case PartE::DRONE: return std::clamp( pitch + m_jamTranspose, 0, 127 );
    default: return pitch;
    }
}

void Engine::dispatch( const NoteEvent &in, double late )
{
    flushParams();   // a note reads what the controls before it at this instant set
    // A voice the keyboard plays in Replace mode leaves the composer's notes out (23.09.2026, liveNoteOn).
    if ( generatedSilenced( in.m_part ) ) return;
    if ( jamSilenced( in.m_part ) ) return;   // the family jam: the leader's break (02.10.2026)
    NoteEvent e = in;                         // and its root: the melodic voices moved to it
    e.m_pitch = static_cast<decltype( e.m_pitch )>( jamPitch( in.m_part, in.m_pitch ) );
    const float vel = static_cast<float>( e.m_velocity ) / 127.0f;
    switch ( e.m_part )
    {
    case PartE::KICK:
        m_kick.trigger( vel, late );
        m_bass.duck( late );
        for ( Ducker &d : m_duck ) d.trigger( late );
        m_returnDuck.trigger( late );
        m_subDuck.trigger( late );
        break;
    case PartE::BASS:
    {
        const double samples = static_cast<double>( e.m_length ) / m_beatsPerSample;
        m_bass.noteOn( e.m_pitch, vel, std::max( 1, static_cast<int>( std::lround( samples ) ) ), late, m_bassPhase );
        break;
    }
    case PartE::PERC:
    {
        const int lane = e.m_lane < PercLanes ? e.m_lane : 0;
        const int shift = static_cast<int>( e.m_pitch ) - PercRoleNote[static_cast<int>( m_perc.role( lane ) )];
        m_perc.trigger( lane, vel, shift, late );
        break;
    }
    case PartE::ACID:
    {
        const double samples = static_cast<double>( e.m_length ) / m_beatsPerSample;
        m_acid.noteOn( e.m_pitch, vel, ( e.m_flags & NOTE_ACCENT ) != 0, ( e.m_flags & NOTE_SLIDE ) != 0,
                       std::max( 1, static_cast<int>( std::lround( samples ) ) ), late );
        break;
    }
    case PartE::LEAD:
    case PartE::COUNTER:
    case PartE::ARP:
    case PartE::STAB:
    case PartE::PAD:
    case PartE::DRONE:
    {
        const double samples = static_cast<double>( e.m_length ) / m_beatsPerSample;
        const int    inst = polyOfPart( e.m_part );
        if ( e.m_part == PartE::LEAD )
        {
            // The two ducks the lead drives (Engine.h): the counter's, and the pad's band for as long as the note sounds.
            m_leadDuck.trigger( late );
            m_padBandDuck.set( 1.0f, 5.0f, static_cast<float>( samples * 1000.0 / m_sr ), 80.0f );
            m_padBandDuck.trigger( late );
        }
        // noteOnLimited applies the quality level's unison and voice limits (Quality.h, Poly.h); at
        // the desktop level it is noteOn() itself.
        // 22.09.2026 (round "Lead"): the note's accent and slide flags. Every voice but the lead slides on
        // every note where its glide time says so, as before; the lead only where the composer flagged it.
        m_poly[inst].noteOnLimited(
            e.m_pitch, vel, e.m_length, std::max( 1, static_cast<int>( std::lround( samples ) ) ), late,
            ( e.m_flags & NOTE_ACCENT ) != 0, e.m_part != PartE::LEAD || ( e.m_flags & NOTE_SLIDE ) != 0 );
        break;
    }
    case PartE::SFX:
    case PartE::TEXTURE:
    case PartE::VOCAL:
    {
        // Routed by type, not by the note's part (Sfx.h, sfxTypePart): the composer writes every effect
        // as a Part::Sfx note, a MIDI file may carry the bed and the voices on tracks of their own.
        const int type = static_cast<int>( e.m_pitch ) - SfxBaseNote;
        if ( type < 0 || type >= NumSfxTypes ) break;
        const SfxTypeE t = static_cast<SfxTypeE>( type );
        const double   samples = static_cast<double>( e.m_length ) / m_beatsPerSample;
        const int      n = std::max( 1, static_cast<int>( std::lround( samples ) ) );
        const double   spb = 1.0 / m_beatsPerSample;
        // The event's own seed: its position on the sixteenth grid and its type. Everything a texture or
        // a voice draws hangs off it, so a bar rendered alone sounds like the same bar in the set. Since
        // 19.09.2026 the composer also sends a variant drawn from the track's seed in the lane (Form.h,
        // SfxEvent::variant), so which phrase speaks is the seed's decision; a note without one (lane 0:
        // an older MIDI file, a hand-made test event) keeps the position-only seed it always had.
        const uint64_t where = static_cast<uint64_t>( std::llround( e.m_beat * 16.0 ) );
        const uint64_t pick = e.m_lane != 0 ? mixSeed( where ^ ( static_cast<uint64_t>( e.m_lane ) << 40 ),
                                                       static_cast<uint64_t>( type ) + 0x5641ull )
                                            : mixSeed( where, static_cast<uint64_t>( type ) + 0x5053ull );
        switch ( sfxTypePart( t ) )
        {
        case PartE::TEXTURE: m_texture.trigger( t, n, vel, late, spb, pick ); break;
        case PartE::VOCAL:
        {
            m_vocal.trigger( t, n, vel, late, spb, pick );
            // The send chain detunes each phrase by its own few hertz, up or down, and opens the flanger.
            Rng r;
            r.seed( pick ^ 0x4D4F54ull );
            const float hz = ( 8.0f + 17.0f * r.uniform() ) * ( r.below( 2 ) == 0 ? 1.0f : -1.0f );
            m_sendMotion = Motion{ e.m_beat, std::max( 1.0, static_cast<double>( e.m_length ) ), hz, 0.3f };
            break;
        }
        default:
            if ( t == SfxTypeE::SPACE_SHOT )
            {
                m_shot.trigger( vel, late, pick );   // a NASA recording, whole (FieldPlayer.h)
                break;
            }
            if ( t == SfxTypeE::STUTTER )
            {
                // A sixteenth repeated, or a thirty-second for a stutter of half a beat or less.
                const int slice = static_cast<int>( std::lround( spb * ( e.m_length <= 0.5f ? 0.125 : 0.25 ) ) );
                m_stutter.trigger( n, slice );
                break;
            }
            m_sfx.trigger( t, n, vel, late,
                           e.m_lane );   // the lane carries the bank preset (23.09.2026, SfxEvent::variant)
            // The event half of the SFX chain's automation: a riser drags the shifter up with it, a
            // downlifter down, the reverse effects lift it a little, and a sweep opens the flanger.
            // The shift and the flanger are separate strands, so that the sweep that falls into a drop
            // opens the flanger without taking the riser's climb away from its last two bars.
            switch ( t )
            {
            case SfxTypeE::RISER:
                m_sfxShift = Motion{ e.m_beat, e.m_length, 150.0f, 0.0f };
                m_sfxFlange = Motion{ e.m_beat, e.m_length, 0.0f, 0.2f };
                break;
            case SfxTypeE::DOWNLIFTER:
                m_sfxShift = Motion{ e.m_beat, e.m_length, -120.0f, 0.0f };
                m_sfxFlange = Motion{ e.m_beat, e.m_length, 0.0f, 0.2f };
                break;
            case SfxTypeE::REVERSE_SWELL:
                m_sfxShift = Motion{ e.m_beat, e.m_length, 40.0f, 0.0f };
                m_sfxFlange = Motion{ e.m_beat, e.m_length, 0.0f, 0.3f };
                break;
            case SfxTypeE::REVERSE_CRASH:
                m_sfxShift = Motion{ e.m_beat, e.m_length, 80.0f, 0.0f };
                m_sfxFlange = Motion{ e.m_beat, e.m_length, 0.0f, 0.3f };
                break;
            case SfxTypeE::SWEEP: m_sfxFlange = Motion{ e.m_beat, e.m_length, 0.0f, 0.5f }; break;
            default: break;
            }
            applyMotion();
            break;
        }
        break;
    }
    case PartE::FIELD:
    {
        // The field recordings (27.09.2026): the note's length is the event's, its pitch against middle C transposes it.
        const double   samples = static_cast<double>( e.m_length ) / m_beatsPerSample;
        const uint64_t where = static_cast<uint64_t>( std::llround( e.m_beat * 16.0 ) );
        const uint64_t pick = mixSeed( where ^ ( static_cast<uint64_t>( e.m_lane ) << 40 ), 0x4649454C44ull );
        m_field.trigger( std::max( 1, static_cast<int>( std::lround( samples ) ) ), vel, late,
                         static_cast<int>( e.m_pitch ) - 60, e.m_beat, pick );
        break;
    }
    default: break;
    }
}

void Engine::applyMotion()
{
    // The event half of the chains' automation (Engine.h): evaluated from the chunk's beat, so it is a
    // function of the score alone. The shift ramps with the event (a riser's shift climbs to its end),
    // the flanger opens and closes on a half sine; once the event is over both are back at zero.
    auto eval = [&]( const Motion &m, float &hz, float &flange )
    {
        hz = 0.0f;
        flange = 0.0f;
        if ( m.m_length <= 0.0 ) return;
        const double x = ( m_chunkBeat - m.m_start ) / m.m_length;
        if ( x < 0.0 || x >= 1.0 ) return;
        hz = static_cast<float>( m.m_shiftHz * x ) * m_motion;
        flange = static_cast<float>( m.m_flange * std::sin( PiD * x ) ) * m_motion;
    };
    float hz, fl, unused;
    eval( m_sfxShift, hz, unused );
    eval( m_sfxFlange, unused, fl );
    m_sfxFx.setMotion( hz, fl );
    // A phrase's detune holds while it speaks and fades over its last quarter.
    eval( m_sendMotion, hz, fl );
    if ( m_sendMotion.m_length > 0.0 && m_sendMotion.m_shiftHz != 0.0f )
    {
        const double x = ( m_chunkBeat - m_sendMotion.m_start ) / m_sendMotion.m_length;
        hz = ( x >= 0.0 && x < 1.0 )
                 ? m_sendMotion.m_shiftHz * m_motion * static_cast<float>( std::min( 1.0, 4.0 * ( 1.0 - x ) ) )
                 : 0.0f;
    }
    m_sendFx.setMotion( hz, fl );
    m_bedFx.setMotion( 0.0f, fl );   // the flanger's opening, not the shift (Engine.h, m_bedFx)
}

void Engine::renderSegment( float *L, float *R, int offset, int count )
{
    flushParams();
    if ( count <= 0 ) return;
    m_kick.process( m_kickBuf.data(), count );
    m_bass.process( m_bassBuf.data(), count );
    m_perc.process( m_percL.data(), m_percR.data(), count );
    m_acid.process( m_acidL.data(), m_acidR.data(), count );
    for ( int k = 0; k < PolyInstances; ++k ) m_poly[k].process( m_polyL[k].data(), m_polyR[k].data(), count );
    // The voice's modulation insert, between the voice and its strip (Engine.h, m_polyFlanger). Its LFO
    // reads the absolute beat of each sample, so a bar rendered alone moves exactly as the same bar
    // inside a whole render and the host's block boundaries decide nothing.
    {
        const double beatAt0 = m_chunkBeat + static_cast<double>( m_chunkPos ) * m_beatsPerSample;
        for ( int k = 0; k < PolyInstances; ++k )
        {
            if ( m_polyMod[k] == static_cast<int>( PolyModE::OFF ) ) continue;
            const bool ph = m_polyMod[k] == static_cast<int>( PolyModE::PHASER );
            for ( int i = 0; i < count; ++i )
            {
                const double beat = beatAt0 + static_cast<double>( i ) * m_beatsPerSample;
                if ( ph )
                    m_polyPhaser[k].tick( m_polyL[k][static_cast<size_t>( i )], m_polyR[k][static_cast<size_t>( i )],
                                          beat );
                else
                    m_polyFlanger[k].tick( m_polyL[k][static_cast<size_t>( i )], m_polyR[k][static_cast<size_t>( i )],
                                           beat );
            }
        }
    }
    m_sfx.processSplit( m_sfxL.data(), m_sfxR.data(), m_subBuf.data(), m_sfxWetL.data(), m_sfxWetR.data(), count );
    m_texture.process( m_texL.data(), m_texR.data(), count );
    m_vocal.process( m_vocL.data(), m_vocR.data(), m_vocThrow.data(), count );
    const double beat0 = m_chunkBeat + static_cast<double>( m_chunkPos ) * m_beatsPerSample;
    m_field.process( m_fieldL.data(), m_fieldR.data(), count, beat0, m_beatsPerSample );
    m_shot.process( m_sfxL.data(), m_sfxR.data(), count );   // before the SFX strip's insert, like every effect
    // The SFX strip's insert: flanger, phaser, frequency shifter (PsyFx.h).
    m_sfxFx.process( m_sfxL.data(), m_sfxR.data(), count, beat0, m_beatsPerSample );
    const float  kg = m_kickMute ? 0.0f : 1.0f, bg = m_bassMute ? 0.0f : 1.0f;
    const float *srcL[STRIP_COUNT] = {};
    const float *srcR[STRIP_COUNT] = {};
    srcL[STRIP_PERC] = m_percL.data();
    srcR[STRIP_PERC] = m_percR.data();
    srcL[STRIP_ACID] = m_acidL.data();
    srcR[STRIP_ACID] = m_acidR.data();
    for ( int k = 0; k < PolyInstances; ++k )
    {
        srcL[STRIP_LEAD + k] = m_polyL[k].data();
        srcR[STRIP_LEAD + k] = m_polyR[k].data();
    }
    srcL[STRIP_SFX] = m_sfxL.data();
    srcR[STRIP_SFX] = m_sfxR.data();
    srcL[STRIP_TEXTURE] = m_texL.data();
    srcR[STRIP_TEXTURE] = m_texR.data();
    srcL[STRIP_VOCAL] = m_vocL.data();
    srcR[STRIP_VOCAL] = m_vocR.data();
    srcL[STRIP_FIELD] = m_fieldL.data();
    srcR[STRIP_FIELD] = m_fieldR.data();
    float *outL = L + offset;
    float *outR = R + offset;
    for ( int i = 0; i < count; ++i )
    {
        const size_t si = static_cast<size_t>( i );
        // Kick and bass are mono and centred, and so is the sub drop, under its own duck; everything
        // else brings the stereo.
        const float  sub = m_stripGain[STRIP_SFX] * m_subDuck.next() * m_subBuf[si];
        const float  mono = kg * m_kickBuf[si] + bg * m_bassBuf[si] + sub;
        const size_t oi =
            static_cast<size_t>( offset + i );   // this sample's index in the caller's buffers (and the stem tap's)
        if ( m_tap != nullptr )
        {
            // The stems (StemTap): the mono parts here, the strips in the loop below, the returns after it.
            const float k = m_masterGain * kg * m_kickBuf[si], bs = m_masterGain * bg * m_bassBuf[si];
            m_tap->m_L[static_cast<int>( PartE::KICK )][oi] = k;
            m_tap->m_R[static_cast<int>( PartE::KICK )][oi] = k;
            m_tap->m_L[static_cast<int>( PartE::BASS )][oi] = bs;
            m_tap->m_R[static_cast<int>( PartE::BASS )][oi] = bs;
        }
        if ( m_metering )
        {
            const float k = m_masterGain * kg * m_kickBuf[si], bs = m_masterGain * bg * m_bassBuf[si];
            meterAdd( static_cast<int>( PartE::KICK ), k, k );
            meterAdd( static_cast<int>( PartE::BASS ), bs, bs );
            ++m_meterCount;
        }
        float l = mono, r = mono, rl = 0.0f, rr = 0.0f, hl = 0.0f, hr = 0.0f, fl = 0.0f, fr = 0.0f, bl = 0.0f,
              br = 0.0f;
        float pl = 0.0f, pr = 0.0f;   // the plate's input
        if ( m_chunkPos + i == 0 )
        {
            // The free slow movement (poly.slow_mod): a sine on the absolute beat with a period of 82 beats (34 s at
            // 145 BPM), renewed on the engine's absolute 32-sample grid -- never where the host happens to cut its
            // blocks -- so a bar rendered alone, or at another block size, moves as the same bar in the set.
            const double b = m_chunkBeat;
            m_slowSide = static_cast<float>( std::sin( 2.0 * PiD * b / SlowBeats[2] ) );
        }
        const float  underLead = m_leadDuck.next(),
                     band = 1.0f - m_padBandDuck.next();   // once per sample; band: 0 .. 1 engaged
        float        hgl = 0.0f, hgr = 0.0f;               // the gated hall's input: the strips whose hall_gate is on
        float        ml = 0.0f, mr = 0.0f;   // the melodic bus a stutter may replace (acid, lead, counter, arp, stab)
        const double beat = m_chunkBeat + static_cast<double>( m_chunkPos + i ) * m_beatsPerSample;
        for ( int s = 0; s < STRIP_COUNT; ++s )
        {
            float sl = srcL[s][si], sr = srcR[s][si];
            if ( s >= STRIP_LEAD && s <= STRIP_DRONE )
            {
                const int k = s - STRIP_LEAD;
                if ( m_gateOn[k] )
                {
                    const float o =
                        TranceGate::open( beat, m_gatePattern[k], m_gateDuty[k], m_gateAttack[k], m_gateRelease[k] );
                    m_gate[k].apply( sl, sr, o, m_gateDepth[k], m_gateTone[k] );
                }
            }
            float g = m_stripGain[s] * m_duck[s].next();
            if ( s == STRIP_COUNTER ) g *= underLead;
            sl *= g;
            sr *= g;
            if ( s >= STRIP_LEAD && s <= STRIP_DRONE )
            {
                // Distance (Engine.h): level, low pass and width against the voice's plane; the wet share is in
                // the plate send below. The free slow movement of width (poly.slow_mod) rides the side gain.
                const int k = s - STRIP_LEAD;
                sl *= m_distGain[k];
                sr *= m_distGain[k];
                if ( m_distLpOn[k] )
                {
                    sl = m_distLpL[k].lp( sl );
                    sr = m_distLpR[k].lp( sr );
                }
                float w = m_distWidth[k];
                if ( m_slowWidth[k] > 0.0f ) w *= 1.0f + 0.25f * m_slowWidth[k] * m_slowSide;
                if ( w != 1.0f )
                {
                    const float m = 0.5f * ( sl + sr ), sd = 0.5f * ( sl - sr ) * w;
                    sl = m + sd;
                    sr = m - sd;
                }
            }
            if ( s == STRIP_PAD )
            {
                // The pad's presence band under the lead: the bandpass share, scaled towards m_padBandCut as far as
                // the lead's duck has opened, taken out of the signal (a bell cut of up to pad_lead_duck dB).
                float lp, bp, hp;
                m_padBandL.tick( sl, lp, bp, hp );
                const float cutL = ( 1.0f - m_padBandCut ) * band * ( bp / 0.7f );
                m_padBandR.tick( sr, lp, bp, hp );
                const float cutR = ( 1.0f - m_padBandCut ) * band * ( bp / 0.7f );
                sl -= cutL;
                sr -= cutR;
            }
            if ( m_tap != nullptr )
            {
                // Strip s is part s + 2 (Perc .. Vocal follow Kick and Bass in both enums); the sub drop is the effects'.
                const int   part = s + static_cast<int>( PartE::PERC );
                const float extra = s == STRIP_SFX ? sub : 0.0f;
                m_tap->m_L[part][oi] = m_masterGain * ( sl + extra );
                m_tap->m_R[part][oi] = m_masterGain * ( sr + extra );
            }
            if ( m_metering )
            {
                const float extra = s == STRIP_SFX ? sub : 0.0f;
                meterAdd( s + static_cast<int>( PartE::PERC ), m_masterGain * ( sl + extra ),
                          m_masterGain * ( sr + extra ) );
            }
            if ( s >= STRIP_ACID && s <= STRIP_STAB )
            {
                ml += sl;
                mr += sr;
            }
            else
            {
                l += sl;
                r += sr;
            }
            rl += m_stripRoom[s] * sl;
            rr += m_stripRoom[s] * sr;
            const float plateSend = s >= STRIP_LEAD && s <= STRIP_DRONE
                                        ? std::max( 0.0f, m_stripPlate[s] + m_distWet[s - STRIP_LEAD] )
                                        : m_stripPlate[s];
            pl += plateSend * sl;
            pr += plateSend * sr;
            // hall_gate on: this strip's hall send feeds the gated bus instead of the plain hall (never
            // both -- a voice picks one hall or the other).
            if ( m_hallGateOn[s] )
            {
                hgl += m_stripHall[s] * sl;
                hgr += m_stripHall[s] * sr;
            }
            else
            {
                hl += m_stripHall[s] * sl;
                hr += m_stripHall[s] * sr;
            }
            if ( s == STRIP_TEXTURE )
            {
                bl += m_stripFx[s] * sl;
                br += m_stripFx[s] * sr;
            }
            else
            {
                fl += m_stripFx[s] * sl;
                fr += m_stripFx[s] * sr;
            }
            if ( s == STRIP_VOCAL ) m_throwIn[si] = 0.5f * ( sl + sr ) * m_vocThrow[si] * m_throwSend;
        }
        // The glitch: while a stutter runs, the melodic bus is its repeat (the sends above keep the
        // live signal, so the reverb tails run on underneath).
        m_stutter.tick( ml, mr );
        outL[i] = l + ml;
        outR[i] = r + mr;
        // The returns' stem holds the dry sum for now; after the returns are added it becomes their share.
        if ( m_tap != nullptr )
        {
            m_tap->m_L[NumParts][oi] = outL[i];
            m_tap->m_R[NumParts][oi] = outR[i];
        }
        m_roomInL[si] = rl;
        m_roomInR[si] = rr;
        m_plateInL[si] = pl;
        m_plateInR[si] = pr;
        // The wandering SFX voices' growing reverb-send trajectory (Sfx.h, sfx.wander) joins the plain
        // hall directly here -- it already left the dry mix in Sfx::processSplit(), so it is not counted
        // again through m_stripHall[StripSfx] above, and it bypasses the gated hall (Engine.h has why).
        m_hallInL[si] = hl + m_sfxWetL[si];
        m_hallInR[si] = hr + m_sfxWetR[si];
        m_hallGateInL[si] = hgl;
        m_hallGateInR[si] = hgr;
        m_sendL[si] = fl;
        m_sendR[si] = fr;
        m_bedSendL[si] = bl;
        m_bedSendR[si] = br;
    }
    m_room.process( m_roomInL.data(), m_roomInR.data(), m_roomOutL.data(), m_roomOutR.data(), count );
    // The plate first: its output feeds the far room (fx.plate_to_hall), the addon's serial B -> C.
    m_plate.process( m_plateInL.data(), m_plateInR.data(), m_plateOutL.data(), m_plateOutR.data(), count );
    if ( m_plateToHall > 0.0f )
        for ( int i = 0; i < count; ++i )
        {
            m_hallInL[static_cast<size_t>( i )] += m_plateToHall * m_plateOutL[static_cast<size_t>( i )];
            m_hallInR[static_cast<size_t>( i )] += m_plateToHall * m_plateOutR[static_cast<size_t>( i )];
        }
    m_hall.process( m_hallInL.data(), m_hallInR.data(), m_hallOutL.data(), m_hallOutR.data(), count );
    // The gated hall: ducked by its own send (Reverb.h, processDucked), then cut hard on the absolute
    // bar line (Reverb::barGate, a function of the beat alone -- the same reason TranceGate::open above
    // is called per sample instead of once per block: a bar rendered alone must close at the same
    // instant as the same bar inside a whole set).
    m_hallGate.processDucked( m_hallGateInL.data(), m_hallGateInR.data(), m_hallGateOutL.data(), m_hallGateOutR.data(),
                              count );
    for ( int i = 0; i < count; ++i )
    {
        const double beat = m_chunkBeat + static_cast<double>( m_chunkPos + i ) * m_beatsPerSample;
        const float  barCut =
            Reverb::barGate( beat, m_hallGateCloseBeats, m_hallGateHoldBeats, m_hallGateOpenBeats, 0.0f );
        m_hallGateOutL[static_cast<size_t>( i )] *= barCut;
        m_hallGateOutR[static_cast<size_t>( i )] *= barCut;
    }
    // The modulation send of the bed and the voices, and the voices' delay throw.
    m_sendFx.process( m_sendL.data(), m_sendR.data(), count, beat0, m_beatsPerSample );
    m_bedFx.process( m_bedSendL.data(), m_bedSendR.data(), count, beat0, m_beatsPerSample );
    for ( int i = 0; i < count; ++i )
    {
        outL[i] += m_sendReturn * ( m_sendL[static_cast<size_t>( i )] + m_bedSendL[static_cast<size_t>( i )] );
        outR[i] += m_sendReturn * ( m_sendR[static_cast<size_t>( i )] + m_bedSendR[static_cast<size_t>( i )] );
    }
    m_throw.process( m_throwIn.data(), outL, outR, count );
    for ( int i = 0; i < count; ++i )
    {
        const size_t si = static_cast<size_t>( i );
        const float  d = m_returnDuck.next();
        // The gated hall shares the plain hall's return trim (fx.hall_return): it is the same hall
        // knob-wise, only gated for the voices routed into it. It ducks under the kick with everyone
        // else's returns too (m_returnDuck), on top of its own gate.
        outL[i] = ( outL[i] + d * ( m_roomReturn * m_roomOutL[si] + m_plateReturn * m_plateOutL[si] +
                                    m_hallReturn * ( m_hallOutL[si] + m_hallGateOutL[si] ) ) ) *
                  m_masterGain;
        outR[i] = ( outR[i] + d * ( m_roomReturn * m_roomOutR[si] + m_plateReturn * m_plateOutR[si] +
                                    m_hallReturn * ( m_hallOutR[si] + m_hallGateOutR[si] ) ) ) *
                  m_masterGain;
        if ( m_tap != nullptr )
        {
            const size_t oi = static_cast<size_t>( offset + i );
            m_tap->m_L[NumParts][oi] = outL[i] - m_masterGain * m_tap->m_L[NumParts][oi];
            m_tap->m_R[NumParts][oi] = outR[i] - m_masterGain * m_tap->m_R[NumParts][oi];
        }
    }
    // Master: bus compressor, mono bass, limiter, safety clip, meter.
    m_comp.process( outL, outR, count );
    for ( int i = 0; i < count; ++i )
    {
        const float m = 0.5f * ( outL[i] + outR[i] );
        float       s = 0.5f * ( outL[i] - outR[i] );
        float       lp, bp, hp;
        m_sideHp1.tick( s, lp, bp, hp );
        m_sideHp2.tick( hp, lp, bp, s );
        outL[i] = m + s;
        outR[i] = m - s;
    }
    if ( m_clipperOn )
    {
        // Soft clip at twice the rate: the kick's transients are rounded here instead of pulling the
        // limiter's gain down for the whole mix.
        const float T = m_clipperT;
        for ( int i = 0; i < count; ++i )
        {
            float a, b;
            m_clipUpL.process( outL[i], a, b );
            outL[i] = m_clipDownL.process( kneeClip( a, T ), kneeClip( b, T ) );
            m_clipUpR.process( outR[i], a, b );
            outR[i] = m_clipDownR.process( kneeClip( a, T ), kneeClip( b, T ) );
        }
    }
    // The upper end of the programme, before the limiter reads it. Everything above 20 kHz is cost
    // without a listener: it throws the true-peak estimate off by 0.9 dB, it folds down when anything
    // resamples the render to 44.1 kHz, and the clipper above turns it into difference tones inside
    // the mix. See Dsp.h (BandLimit) for the measurements behind the corner.
    for ( int i = 0; i < count; ++i )
    {
        outL[i] = m_bandLimitL.process( outL[i] );
        outR[i] = m_bandLimitR.process( outR[i] );
    }
    if ( m_limiterOn ) m_limiter.process( outL, outR, count );
    if ( m_clip )
    {
        const float invCeil = 1.0f / m_ceiling;
        for ( int i = 0; i < count; ++i )
        {
            if ( m_limiterOn )
            {
                // Only a safety net: the limiter already keeps the true peak at the ceiling. A plain clip,
                // not an antiderivative one -- that would average neighbouring samples even below the ceiling.
                outL[i] = clampv( outL[i], -m_ceiling, m_ceiling );
                outR[i] = clampv( outR[i], -m_ceiling, m_ceiling );
            }
            else
            {
                outL[i] = m_ceiling * m_clipL( outL[i] * invCeil );
                outR[i] = m_ceiling * m_clipR( outR[i] * invCeil );
            }
        }
    }
    m_meter.process( outL, outR, count );
    // The monitor (master.monitor, the addon): what the listener hears, after the meter.
    if ( m_monitor != 0 )
        for ( int i = 0; i < count; ++i )
        {
            const float m = 0.5f * ( outL[i] + outR[i] ), sd = 0.5f * ( outL[i] - outR[i] );
            const float y = m_monitor == 1 ? m : ( m_monitor == 2 ? m_monLp2.lp( m_monLp1.lp( m ) ) : sd );
            outL[i] = outR[i] = y;
        }
    m_chunkPos += count;
    m_samples += static_cast<uint64_t>( count );
}
void Engine::process( float *L, float *R, int n )
{
    const DenormalGuard guard;
    int                 done = 0;
    while ( done < n )
    {
        if ( m_chunkPos == 0 )
        {
            // Out-of-band corrections first (pushImmediate): due now, whatever the queue holds ahead of them.
            ControlEvent now;
            while ( m_immediate.pop( now ) )
            {
                if ( now.m_beat < m_chunkBeat ) now.m_beat = m_chunkBeat;
                dispatchControl( now );
            }
            advanceRamps();
            applyParams();
            applyMotion();
            // The free slow movement of the voices' cutoff and colour (poly.slow_mod, the addon): two sines on the
            // absolute beat, periods of 31 and 51 beats (13 and 21 s at 145 BPM).
            for ( int k = 0; k < PolyInstances; ++k )
                m_poly[k].setSlow( static_cast<float>( std::sin( 2.0 * PiD * m_chunkBeat / SlowBeats[0] ) ),
                                   static_cast<float>( std::sin( 2.0 * PiD * m_chunkBeat / SlowBeats[1] + 2.1 ) ) );
            // The voices' own modulation reads the set's beat (26.09.2026, Modulation.h: the synced LFOs).
            for ( int k = 0; k < PolyInstances; ++k ) m_poly[k].setClock( m_chunkBeat, m_beatsPerSample );
            m_bass.setClock( m_chunkBeat, m_beatsPerSample );
            m_acid.setClock( m_chunkBeat, m_beatsPerSample );
        }
        flushParams();
        int left = std::min( n - done, Chunk - m_chunkPos );

        for ( ;; )
        {
            NoteEvent    ne;
            ControlEvent ce;
            const bool   hasNote = m_notes.peek( ne ), hasCtl = m_controls.peek( ce );
            if ( ( !hasNote && !hasCtl ) || left <= 0 ) break;
            // Controls first at equal beats.
            const bool   ctl = hasCtl && ( !hasNote || ce.m_beat <= ne.m_beat );
            const double beat = ctl ? ce.m_beat : ne.m_beat;
            const double rel = ( beat - m_chunkBeat ) / m_beatsPerSample;
            int          m = rel <= 0.0 ? 0 : static_cast<int>( std::ceil( rel - 1e-7 ) );
            if ( m < m_chunkPos ) m = m_chunkPos;
            if ( m >= m_chunkPos + left ) break;
            const int before = m - m_chunkPos;
            renderSegment( L, R, done, before );
            done += before;
            left -= before;
            // How far past the ideal instant this sample lies; late events count as on time.
            double late = static_cast<double>( m ) - rel;
            if ( late < 0.0 || late >= 1.0 ) late = 0.0;
            if ( ctl )
            {
                m_controls.pop( ce );
                dispatchControl( ce );
            }
            else
            {
                m_notes.pop( ne );
                dispatch( ne, late );
            }
        }
        renderSegment( L, R, done, left );
        done += left;

        if ( m_chunkPos >= Chunk )
        {
            m_chunkBeat += Chunk * m_beatsPerSample;
            m_chunkPos = 0;
        }
        m_beatNow.store( m_chunkBeat + m_chunkPos * m_beatsPerSample, std::memory_order_relaxed );
    }
}

}   // namespace phos
