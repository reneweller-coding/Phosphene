/**
 * @file FieldPresets.cpp
 * @brief The Field track's factory presets (FieldPresets.h).
 * @author Rene Weller
 */
#include "phos/FieldPresets.h"
#include "phos/Dsp.h"
#include "phos/FieldLibrary.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace phos
{

namespace
{
/** @brief The presets of the Field track (FieldPresetData.inl). */
const FieldPreset Rows[] = {
#include "FieldPresetData.inl"
};
}   // namespace

const std::vector<FieldPreset> &fieldPresets()
{
    static const std::vector<FieldPreset> presets( std::begin( Rows ), std::end( Rows ) );
    return presets;
}

int fieldVariationOf( int category, const std::string &stem )
{
    const int n = fieldClipCount( category );
    for ( int v = 0; v < n; ++v )
        if ( fieldClipName( fieldClipIndex( category, v ) ) == stem ) return v;
    return -1;
}

bool fieldPresetAvailable( const FieldPreset &preset )
{
    return fieldVariationOf( preset.m_categoryA, preset.m_fileA ) >= 0 &&
           ( preset.m_categoryB < 0 || fieldVariationOf( preset.m_categoryB, preset.m_fileB ) >= 0 );
}

std::vector<std::pair<int, float>> fieldPresetValues( const ParamStore &p, const FieldPreset &preset )
{
    const int          b = p.base( ModuleE::FIELD );
    std::vector<float> v( static_cast<size_t>( field::COUNT ) );
    for ( int i = 0; i < field::COUNT; ++i ) v[static_cast<size_t>( i )] = p.defaultValue( b + i );
    std::istringstream in( preset.m_text );
    std::string        line;
    while ( std::getline( in, line ) )
    {
        const size_t eq = line.find( '=' );
        if ( eq == std::string::npos ) continue;
        const int id = p.find( "field." + line.substr( 0, eq ) );
        if ( id < b || id >= b + field::COUNT ) continue;
        v[static_cast<size_t>( id - b )] = std::strtof( line.c_str() + eq + 1, nullptr );
    }
    v[field::A_CATEGORY] = static_cast<float>( preset.m_categoryA + 1 );
    v[field::A_VARIATION] =
        static_cast<float>( fieldPresetVariation( preset.m_categoryA, preset.m_fileA, preset.m_variationA ) );
    v[field::B_ON] = preset.m_categoryB >= 0 ? 1.0f : 0.0f;
    if ( preset.m_categoryB >= 0 )
    {
        v[field::B_CATEGORY] = static_cast<float>( preset.m_categoryB + 1 );
        v[field::B_VARIATION] =
            static_cast<float>( fieldPresetVariation( preset.m_categoryB, preset.m_fileB, preset.m_variationB ) );
    }
    std::vector<std::pair<int, float>> out;
    out.reserve( static_cast<size_t>( field::COUNT ) );
    for ( int i = 0; i < field::COUNT; ++i ) out.emplace_back( i, v[static_cast<size_t>( i )] );
    return out;
}

FieldTuning fieldPresetTuning( const FieldPreset &preset, int keyRoot )
{
    FieldTuning t;
    const float hzA = fieldToneOf( preset.m_fileA );
    const float hzB = preset.m_categoryB >= 0 ? fieldToneOf( preset.m_fileB ) : 0.0f;
    if ( hzA > 0.0f )
    {
        const double shift = fieldToneShift( hzA, keyRoot );
        t.m_semis = static_cast<int>( std::lround( shift ) );
        t.m_fineA = static_cast<float>( 100.0 * ( shift - t.m_semis ) );
    }
    if ( hzB > 0.0f )
    {
        if ( hzA <= 0.0f ) t.m_semis = static_cast<int>( std::lround( fieldToneShift( hzB, keyRoot ) ) );
        // Transposed with the note already: of root, fourth and fifth the one B's fine tune comes nearest to.
        const double note = 69.0 + 12.0 * std::log2( static_cast<double>( hzB ) / 440.0 );
        double       best = 1e9;
        for ( const int degree : { 0, 5, 7 } )
        {
            double d = std::fmod( static_cast<double>( keyRoot + degree ) - note, 12.0 );
            if ( d < -6.0 ) d += 12.0;
            if ( d >= 6.0 ) d -= 12.0;
            if ( std::fabs( d - t.m_semis ) < std::fabs( best ) ) best = d - t.m_semis;
        }
        t.m_fineB = static_cast<float>( std::clamp( 100.0 * best, -100.0, 100.0 ) );
    }
    return t;
}

int fieldPresetVariation( int category, const char *file, int shipped )
{
    const int v = fieldVariationOf( category, file );
    return v >= 0 ? v : shipped;
}

int fieldPresetFor( int style, uint64_t seed )
{
    const std::vector<FieldPreset> &all = fieldPresets();
    const int                       s = std::clamp( style, 0, 4 );
    double                          total = 0.0;
    std::vector<double>             w( all.size(), 0.0 );
    for ( size_t i = 0; i < all.size(); ++i )
    {
        // Squared: a style's own places come up far more often than the ones that only fit it.
        w[i] = static_cast<double>( all[i].m_style[s] ) * all[i].m_style[s];
        // A recording with tones of its own that no shift puts into a key is not the composer's to draw (10.11.2026,
        // FieldLibrary.h, fieldToneOf): a Webb sonification is a piece in a key of its own. The menu keeps it.
        if ( fieldToneOf( all[i].m_fileA ) == FieldToneMusic ||
             ( all[i].m_categoryB >= 0 && fieldToneOf( all[i].m_fileB ) == FieldToneMusic ) )
            w[i] = 0.0;
        total += w[i];
    }
    if ( total <= 0.0 ) return -1;
    Rng r;
    r.seed( seed );
    double x = static_cast<double>( r.uniform() ) * total;
    for ( size_t i = 0; i < all.size(); ++i )
    {
        if ( w[i] <= 0.0 ) continue;
        if ( x < w[i] ) return static_cast<int>( i );
        x -= w[i];
    }
    for ( size_t i = all.size(); i-- > 0; )
        if ( w[i] > 0.0 ) return static_cast<int>( i );
    return -1;
}

}   // namespace phos
