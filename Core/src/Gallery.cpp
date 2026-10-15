/**
 * @file Gallery.cpp
 * @brief The set gallery's comment lines (Gallery.h).
 * @author Rene Weller
 */
#include "phos/Gallery.h"
#include "phos/Composer.h"
#include "phos/Form.h"
#include "phos/Params.h"
#include "phos/Rating.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace phos
{

namespace
{

/**
 * @brief The letter of a section in the form string.
 * @param[in] s  the section
 * @return its letter
 */
char sectionLetter( const Section &s )
{
    switch ( s.m_type )
    {
    case SectionTypeE::INTRO: return 'I';
    case SectionTypeE::GROOVE: return 'G';
    case SectionTypeE::BUILD: return 'B';
    case SectionTypeE::DROP: return s.m_climax ? 'C' : 'D';
    case SectionTypeE::BREAK: return 'K';
    case SectionTypeE::OUTRO: return 'O';
    default: return 'X';
    }
}

/**
 * @brief Tabs and line breaks out of a field, and the separator of the track line.
 * @param[in] s  the field
 * @return the field cleaned
 */
std::string clean( const std::string &s )
{
    std::string out = s;
    for ( char &c : out )
        if ( c == '\n' || c == '\r' || c == '|' ) c = ' ';
    return out;
}

}   // namespace

GalleryTrack galleryTrackOf( const TrackPlan &p )
{
    GalleryTrack t;
    t.m_style = StyleNames[std::clamp( p.m_style, 0, NumStyles - 1 )];
    t.m_key = KeyNames[( ( p.m_key % 12 ) + 12 ) % 12];
    t.m_scale = ScaleNames[std::clamp( p.m_scale, 0, NumScales - 1 )];
    t.m_bpm = p.m_bpm;
    t.m_bars = p.m_bars;
    for ( int s = 0; s < p.m_form.m_count; ++s )
    {
        if ( !t.m_form.empty() ) t.m_form += ' ';
        t.m_form += sectionLetter( p.m_form.m_section[s] );
        t.m_form += std::to_string( p.m_form.m_section[s].m_bars );
    }
    return t;
}

std::string galleryComment( const GalleryEntry &e )
{
    std::string s = "# gallery.name=" + clean( e.m_name ) + "\n# gallery.saved=" + clean( e.m_saved ) + "\n";
    for ( const GalleryTrack &t : e.m_tracks )
    {
        char bpm[32];
        std::snprintf( bpm, sizeof( bpm ), "%.1f", t.m_bpm );
        s += "# gallery.track=" + clean( t.m_style ) + "|" + clean( t.m_key ) + "|" + clean( t.m_scale ) + "|" + bpm +
             "|" + std::to_string( t.m_bars ) + "|" + clean( t.m_form ) + "\n";
    }
    return s;
}

std::string withGalleryComment( const std::string &setText, const GalleryEntry &e )
{
    const size_t nl = setText.find( '\n' );
    if ( nl == std::string::npos ) return setText + "\n" + galleryComment( e );
    return setText.substr( 0, nl + 1 ) + galleryComment( e ) + setText.substr( nl + 1 );
}

bool readGalleryEntry( std::string_view text, GalleryEntry &out )
{
    GalleryEntry       e;
    bool               haveSeed = false;
    std::istringstream in{ std::string( text ) };
    std::string        line;
    while ( std::getline( in, line ) )
    {
        if ( !line.empty() && line.back() == '\r' ) line.pop_back();
        if ( line.rfind( "seed=", 0 ) == 0 )
        {
            e.m_seed = std::strtoull( line.c_str() + 5, nullptr, 10 );
            haveSeed = true;
            continue;
        }
        if ( line.rfind( "# gallery.name=", 0 ) == 0 )
        {
            e.m_name = line.substr( 15 );
            continue;
        }
        if ( line.rfind( "# gallery.saved=", 0 ) == 0 )
        {
            e.m_saved = line.substr( 16 );
            continue;
        }
        if ( line.rfind( "# gallery.track=", 0 ) == 0 )
        {
            std::vector<std::string> f;
            std::string              cell;
            std::istringstream       cells( line.substr( 16 ) );
            while ( std::getline( cells, cell, '|' ) ) f.push_back( cell );
            if ( f.size() < 6 ) continue;
            GalleryTrack t;
            t.m_style = f[0];
            t.m_key = f[1];
            t.m_scale = f[2];
            t.m_bpm = std::atof( f[3].c_str() );
            t.m_bars = std::atoi( f[4].c_str() );
            t.m_form = f[5];
            e.m_tracks.push_back( t );
        }
    }
    if ( !haveSeed ) return false;
    out = e;
    return true;
}

std::map<uint64_t, RatingCount> ratingsBySeed( const char *path )
{
    std::map<uint64_t, RatingCount> out;
    std::ifstream                   in( path );
    std::string                     line;
    while ( std::getline( in, line ) )
    {
        RatingEntry r;
        if ( !parseRating( line, r ) ) continue;
        RatingCount &c = out[r.m_seed];
        if ( r.m_verdict > 0 )
            ++c.m_good;
        else if ( r.m_verdict < 0 )
            ++c.m_bad;
        else
            ++c.m_notes;
    }
    return out;
}

}   // namespace phos
