/**
 * @file Midi.cpp
 * @brief Standard MIDI File encoder and decoder.
 * @author Rene Weller
 */
#include "phos/Midi.h"
#include "phos/Sfx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace phos
{

namespace
{

/** @brief A raw timed MIDI message before delta encoding. */
struct RawEvent
{
    int64_t              m_tick;    ///< when, in ticks
    int                  m_order;   ///< tie-break at equal ticks: meta 0, note-off 1, note-on 2
    std::vector<uint8_t> m_bytes;   ///< status and data, or FF type len data for meta
};

/**
 * @brief Appends @p v as a MIDI variable-length quantity.
 * @param[in,out] out  the bytes, appended to
 * @param[in]     v    the value
 */
void putVlq( std::vector<uint8_t> &out, uint32_t v )
{
    uint8_t tmp[5];
    int     n = 0;
    tmp[n++] = static_cast<uint8_t>( v & 0x7F );
    while ( ( v >>= 7 ) != 0 ) tmp[n++] = static_cast<uint8_t>( 0x80 | ( v & 0x7F ) );
    while ( n > 0 ) out.push_back( tmp[--n] );
}

/**
 * @brief Appends @p v big-endian, four bytes.
 * @param[in,out] out  the bytes, appended to
 * @param[in]     v    the value
 */
void put32( std::vector<uint8_t> &out, uint32_t v )
{
    out.push_back( static_cast<uint8_t>( v >> 24 ) );
    out.push_back( static_cast<uint8_t>( v >> 16 ) );
    out.push_back( static_cast<uint8_t>( v >> 8 ) );
    out.push_back( static_cast<uint8_t>( v ) );
}

/**
 * @brief Appends @p v big-endian, two bytes.
 * @param[in,out] out  the bytes, appended to
 * @param[in]     v    the value
 */
void put16( std::vector<uint8_t> &out, uint16_t v )
{
    out.push_back( static_cast<uint8_t>( v >> 8 ) );
    out.push_back( static_cast<uint8_t>( v ) );
}

/**
 * @brief A meta event of @p type with @p payload at @p tick.
 * @param[in] tick     the tick
 * @param[in] type     the meta type
 * @param[in] payload  the payload
 * @return the event
 */
RawEvent meta( int64_t tick, uint8_t type, const std::vector<uint8_t> &payload )
{
    RawEvent e{ tick, 0, {} };
    e.m_bytes.push_back( 0xFF );
    e.m_bytes.push_back( type );
    putVlq( e.m_bytes, static_cast<uint32_t>( payload.size() ) );
    e.m_bytes.insert( e.m_bytes.end(), payload.begin(), payload.end() );
    return e;
}

/**
 * @brief A text meta event (track name, marker, ...) at @p tick.
 * @param[in] tick  the tick
 * @param[in] type  the meta type
 * @param[in] s     the text
 * @return the event
 */
RawEvent textMeta( int64_t tick, uint8_t type, const std::string &s )
{
    return meta( tick, type, std::vector<uint8_t>( s.begin(), s.end() ) );
}

/**
 * @brief A set-tempo meta event for @p bpm at @p tick.
 * @param[in] tick  the tick
 * @param[in] bpm   the tempo
 * @return the event
 */
RawEvent tempoMeta( int64_t tick, double bpm )
{
    const uint32_t mpq = static_cast<uint32_t>( std::lround( 60000000.0 / bpm ) );
    return meta( tick, 0x51,
                 { static_cast<uint8_t>( mpq >> 16 ), static_cast<uint8_t>( mpq >> 8 ), static_cast<uint8_t>( mpq ) } );
}

/**
 * @brief Beat @p beat in ticks (MidiPpq per beat).
 * @param[in] beat  the position in beats
 * @return the position in ticks
 */
int64_t toTick( double beat )
{
    return static_cast<int64_t>( std::llround( beat * MidiPpq ) );
}

/**
 * @brief Sorts and delta-encodes events into an MTrk chunk.
 * @param[in,out] file    the file's bytes, appended to
 * @param[in,out] events  the events, sorted in place
 */
void emitTrack( std::vector<uint8_t> &file, std::vector<RawEvent> &events )
{
    std::stable_sort( events.begin(), events.end(), []( const RawEvent &a, const RawEvent &b )
                      { return a.m_tick != b.m_tick ? a.m_tick < b.m_tick : a.m_order < b.m_order; } );
    std::vector<uint8_t> body;
    int64_t              last = 0;
    for ( const RawEvent &e : events )
    {
        const int64_t t = e.m_tick < 0 ? 0 : e.m_tick;
        putVlq( body, static_cast<uint32_t>( t - last ) );
        last = t;
        body.insert( body.end(), e.m_bytes.begin(), e.m_bytes.end() );
    }
    putVlq( body, 0 );
    body.push_back( 0xFF );
    body.push_back( 0x2F );
    body.push_back( 0x00 );
    file.insert( file.end(), { 'M', 'T', 'r', 'k' } );
    put32( file, static_cast<uint32_t>( body.size() ) );
    file.insert( file.end(), body.begin(), body.end() );
}

}   // namespace

int midiChannelOf( PartE part )
{
    switch ( part )
    {
    case PartE::KICK:
    case PartE::PERC: return 9;
    case PartE::BASS: return 0;
    case PartE::ACID: return 1;
    // 19.09.2026, round "voices": the six polyphonic voices on channels 3 to 8 in the order of their
    // groups (Params.h, PolyInstance), then the effects; channel 10 (index 9) stays the drums'.
    case PartE::LEAD: return 2;
    case PartE::COUNTER: return 3;
    case PartE::ARP: return 4;
    case PartE::STAB: return 5;
    case PartE::PAD: return 6;
    case PartE::DRONE: return 7;
    case PartE::SFX: return 8;
    case PartE::TEXTURE: return 10;   // the shamanic bed and the voices on their own channels
    case PartE::VOCAL: return 11;
    case PartE::FIELD: return 12;   // 27.09.2026: the field recordings
    default: return 15;
    }
}

int minorKeySharps( int root )
{
    // Minor keys around the circle of fifths from A minor, choosing the spelling with at most six
    // accidentals: C -3, C# 4, D -1, D# 6, E 1, F -4, F# 3, G -2, G# 5, A 0, A# -5 (Bb), B 2.
    static const int Sharps[12] = { -3, 4, -1, 6, 1, -4, 3, -2, 5, 0, -5, 2 };
    return Sharps[( ( root % 12 ) + 12 ) % 12];
}

std::vector<uint8_t> encodeMidi( const Score &score )
{
    std::vector<std::vector<RawEvent>> tracks;

    // Conductor track.
    {
        std::vector<RawEvent> ev;
        ev.push_back( textMeta( 0, 0x03, "Phosphene" ) );
        ev.push_back( meta( 0, 0x58, { 4, 2, 24, 8 } ) );
        ev.push_back(
            meta( 0, 0x59, { static_cast<uint8_t>( static_cast<int8_t>( minorKeySharps( score.m_keyRoot ) ) ), 1 } ) );
        for ( const KeyChange &k : score.m_keyChanges )
            ev.push_back( meta( toTick( k.m_beat ), 0x59,
                                { static_cast<uint8_t>( static_cast<int8_t>( minorKeySharps( k.m_root ) ) ), 1 } ) );
        const auto  &pts = score.m_tempo.points();
        const double end = std::max( score.endBeat(), pts.back().m_beat ) + 1.0;
        for ( size_t i = 0; i < pts.size(); ++i )
        {
            const TempoPoint &p = pts[i];
            if ( !p.m_rampToNext || i + 1 >= pts.size() )
            {
                ev.push_back( tempoMeta( toTick( p.m_beat ), p.m_bpm ) );
                continue;
            }
            // One held tempo per beat of the ramp, chosen so the beat's duration is exact.
            const double stop = std::min( pts[i + 1].m_beat, end );
            for ( double b = p.m_beat; b < stop - 1e-9; b += 1.0 )
            {
                const double nb = std::min( b + 1.0, stop );
                const double dt = score.m_tempo.secondsAt( nb ) - score.m_tempo.secondsAt( b );
                ev.push_back( tempoMeta( toTick( b ), 60.0 * ( nb - b ) / dt ) );
            }
        }
        for ( const SectionMark &s : score.m_sections )
        {
            char buf[64];
            std::snprintf( buf, sizeof( buf ), "%s (track %d)", SectionNames[static_cast<int>( s.m_type )],
                           s.m_track + 1 );
            ev.push_back( textMeta( toTick( s.m_beat ), 0x06, buf ) );
        }
        tracks.push_back( std::move( ev ) );
    }

    // One track per part that has notes. Effect notes go to the track of the part their type belongs
    // to (Sfx.h, routedPart): the composer writes the shamanic bed and the voices as Part::Sfx notes,
    // and a producer opening the file wants them on tracks of their own.
    for ( int p = 0; p < NumParts; ++p )
    {
        std::vector<RawEvent> ev;
        const uint8_t         ch = static_cast<uint8_t>( midiChannelOf( static_cast<PartE>( p ) ) );
        bool                  portamento = false;
        for ( const NoteEvent &n : score.m_notes )
        {
            if ( static_cast<int>( routedPart( n ) ) != p ) continue;
            const int64_t on = toTick( n.m_beat );
            const int64_t off = std::max( on + 1, toTick( n.m_beat + static_cast<double>( n.m_length ) ) );
            // Slides: the notes overlap already; CC 65 (portamento) is on from the first sliding note
            // until the note a slide ends in has finished.
            if ( n.m_flags & NOTE_SLIDE )
            {
                if ( !portamento ) ev.push_back( RawEvent{ on, 1, { static_cast<uint8_t>( 0xB0 | ch ), 65, 127 } } );
                portamento = true;
            }
            else if ( portamento )
            {
                ev.push_back( RawEvent{ off, 1, { static_cast<uint8_t>( 0xB0 | ch ), 65, 0 } } );
                portamento = false;
            }
            ev.push_back( RawEvent{
                on, 2, { static_cast<uint8_t>( 0x90 | ch ), n.m_pitch, std::max<uint8_t>( 1, n.m_velocity ) } } );
            ev.push_back( RawEvent{ off, 1, { static_cast<uint8_t>( 0x80 | ch ), n.m_pitch, 0 } } );
        }
        if ( ev.empty() ) continue;
        ev.push_back( textMeta( 0, 0x03, PartNames[p] ) );
        tracks.push_back( std::move( ev ) );
    }

    std::vector<uint8_t> file;
    file.insert( file.end(), { 'M', 'T', 'h', 'd' } );
    put32( file, 6 );
    put16( file, 1 );
    put16( file, static_cast<uint16_t>( tracks.size() ) );
    put16( file, static_cast<uint16_t>( MidiPpq ) );
    for ( auto &t : tracks ) emitTrack( file, t );
    return file;
}

bool writeMidiFile( const Score &score, const char *path )
{
    const std::vector<uint8_t> bytes = encodeMidi( score );
    FILE                      *f = std::fopen( path, "wb" );
    if ( f == nullptr ) return false;
    const bool ok = std::fwrite( bytes.data(), 1, bytes.size(), f ) == bytes.size();
    return std::fclose( f ) == 0 && ok;
}

bool decodeMidi( const uint8_t *d, size_t size, MidiFileData &out, std::string *error )
{
    auto fail = [&]( const char *msg )
    {
        if ( error ) *error = msg;
        return false;
    };
    out = MidiFileData{};
    if ( size < 14 || std::memcmp( d, "MThd", 4 ) != 0 ) return fail( "not a MIDI file" );
    auto rd32 = [&]( size_t p )
    {
        return ( uint32_t( d[p] ) << 24 ) | ( uint32_t( d[p + 1] ) << 16 ) | ( uint32_t( d[p + 2] ) << 8 ) |
               uint32_t( d[p + 3] );
    };
    auto           rd16 = [&]( size_t p ) { return static_cast<uint16_t>( ( d[p] << 8 ) | d[p + 1] ); };
    const uint32_t hlen = rd32( 4 );
    if ( hlen < 6 || 8 + static_cast<size_t>( hlen ) > size ) return fail( "bad header" );
    out.m_format = rd16( 8 );
    const int      ntracks = rd16( 10 );
    const uint16_t div = rd16( 12 );
    if ( div & 0x8000 ) return fail( "SMPTE time division is not supported" );
    out.m_ppq = div == 0 ? 480 : div;
    size_t pos = 8 + hlen;

    for ( int t = 0; t < ntracks; ++t )
    {
        if ( pos + 8 > size ) return fail( "truncated track header" );
        if ( std::memcmp( d + pos, "MTrk", 4 ) != 0 )
        {   // unknown chunk: skip
            pos += 8 + rd32( pos + 4 );
            --t;
            continue;
        }
        const size_t len = rd32( pos + 4 );
        size_t       p = pos + 8;
        const size_t end = p + len;
        if ( end > size ) return fail( "truncated track" );
        pos = end;

        MidiTrackData track;
        struct Open
        {
            int64_t m_tick;
            uint8_t m_vel;
            bool    m_used;
        };
        Open    open[16][128] = {};
        int64_t tick = 0;
        uint8_t running = 0;
        auto    vlq = [&]( uint32_t &v )
        {
            v = 0;
            for ( int i = 0; i < 4; ++i )
            {
                if ( p >= end ) return false;
                const uint8_t b = d[p++];
                v = ( v << 7 ) | ( b & 0x7F );
                if ( !( b & 0x80 ) ) return true;
            }
            return false;
        };
        std::vector<NoteEvent> notes;
        while ( p < end )
        {
            uint32_t delta = 0;
            if ( !vlq( delta ) ) return fail( "bad delta time" );
            tick += delta;
            if ( p >= end ) return fail( "truncated event" );
            uint8_t st = d[p];
            if ( st == 0xFF )
            {
                if ( p + 2 > end ) return fail( "truncated meta" );
                const uint8_t type = d[p + 1];
                p += 2;
                uint32_t mlen = 0;
                if ( !vlq( mlen ) || p + mlen > end ) return fail( "bad meta length" );
                const double beat = static_cast<double>( tick ) / out.m_ppq;
                if ( type == 0x03 && track.m_name.empty() )
                    track.m_name.assign( reinterpret_cast<const char *>( d + p ), mlen );
                else if ( type == 0x06 )
                    out.m_markers.emplace_back( beat, std::string( reinterpret_cast<const char *>( d + p ), mlen ) );
                else if ( type == 0x51 && mlen == 3 )
                {
                    const uint32_t mpq = ( uint32_t( d[p] ) << 16 ) | ( uint32_t( d[p + 1] ) << 8 ) | d[p + 2];
                    if ( mpq > 0 ) out.m_tempos.push_back( TempoPoint{ beat, 60000000.0 / mpq, false } );
                }
                else if ( type == 0x59 && mlen == 2 )
                {
                    if ( out.m_keys.empty() )
                    {
                        out.m_keySharps = static_cast<int8_t>( d[p] );
                        out.m_keyMinor = d[p + 1] != 0;
                    }
                    out.m_keys.emplace_back( beat, static_cast<int>( static_cast<int8_t>( d[p] ) ) );
                }
                p += mlen;
                continue;
            }
            if ( st == 0xF0 || st == 0xF7 )
            {
                ++p;
                uint32_t slen = 0;
                if ( !vlq( slen ) || p + slen > end ) return fail( "bad sysex length" );
                p += slen;
                continue;
            }
            if ( st & 0x80 )
            {
                running = st;
                ++p;
            }
            else if ( running == 0 )
                return fail( "data byte without status" );
            st = running;
            const uint8_t hi = st & 0xF0, ch = st & 0x0F;
            const int     nData = ( hi == 0xC0 || hi == 0xD0 ) ? 1 : 2;
            if ( p + static_cast<size_t>( nData ) > end ) return fail( "truncated channel message" );
            const uint8_t a = d[p], b = nData > 1 ? d[p + 1] : 0;
            p += static_cast<size_t>( nData );
            if ( hi == 0x90 && b > 0 )
            {
                if ( track.m_channel < 0 ) track.m_channel = ch;
                open[ch][a & 0x7F] = Open{ tick, b, true };
            }
            else if ( hi == 0x80 || ( hi == 0x90 && b == 0 ) )
            {
                Open &o = open[ch][a & 0x7F];
                if ( o.m_used )
                {
                    NoteEvent n;
                    n.m_beat = static_cast<double>( o.m_tick ) / out.m_ppq;
                    n.m_length = static_cast<float>( static_cast<double>( tick - o.m_tick ) / out.m_ppq );
                    n.m_pitch = a & 0x7F;
                    n.m_velocity = o.m_vel;
                    n.m_lane = ch;
                    notes.push_back( n );
                    o.m_used = false;
                }
            }
        }
        PartE part = PartE::COUNT;
        for ( int k = 0; k < NumParts; ++k )
            if ( track.m_name == PartNames[k] ) part = static_cast<PartE>( k );
        for ( NoteEvent &n : notes )
        {
            n.m_part = part == PartE::COUNT ? PartE::LEAD : part;
            n.m_lane = 0;
        }
        std::stable_sort( notes.begin(), notes.end(), noteLess );
        track.m_notes = std::move( notes );
        out.m_tracks.push_back( std::move( track ) );
    }
    std::stable_sort( out.m_tempos.begin(), out.m_tempos.end(),
                      []( const TempoPoint &x, const TempoPoint &y ) { return x.m_beat < y.m_beat; } );
    return true;
}

bool readMidiFile( const char *path, MidiFileData &out, std::string *error )
{
    FILE *f = std::fopen( path, "rb" );
    if ( f == nullptr )
    {
        if ( error ) *error = "cannot open file";
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t              buf[65536];
    size_t               n;
    while ( ( n = std::fread( buf, 1, sizeof( buf ), f ) ) > 0 ) bytes.insert( bytes.end(), buf, buf + n );
    std::fclose( f );
    return decodeMidi( bytes.data(), bytes.size(), out, error );
}

}   // namespace phos
