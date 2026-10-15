/**
 * @file StoreParameter.cpp
 * @brief The host's view of one engine parameter (PluginProcessor.h, StoreParameter).
 * @author Rene Weller
 */
#include "PluginProcessor.h"
#include <cmath>

using namespace phos;

namespace
{

/**
 * @brief A value formatted the way phos::ParamStore::format would, but for an arbitrary value.
 * @param[in] d  the parameter's description
 * @param[in] v  the value
 * @return the value as text
 */
juce::String formatValue( const ParamDesc &d, float v )
{
    if ( d.m_curve == CurveE::CHOICE && d.m_choices != nullptr )
    {
        const int i = juce::jlimit( 0, static_cast<int>( d.m_maxValue ), static_cast<int>( std::lround( v ) ) );
        return d.m_choices[i];
    }
    if ( d.m_curve == CurveE::TOGGLE ) return v >= 0.5f ? "On" : "Off";
    if ( d.m_curve == CurveE::INT ) return juce::String( static_cast<int>( std::lround( v ) ) );
    const float a = std::fabs( v );
    return juce::String( v, a >= 100.0f ? 0 : ( a >= 10.0f ? 1 : 2 ) );
}

}   // namespace

// ==================================================================== StoreParameter

StoreParameter::StoreParameter( ParamStore &store, int id, const juce::String &name )
    : juce::RangedAudioParameter(
          juce::ParameterID( juce::String( store.key( id ) ), 1 ), name,
          juce::AudioProcessorParameterWithIDAttributes().withLabel( store.desc( id ).m_unit ) ),
      m_store( store ), m_id( id ), m_name( name )
{
    const ParamDesc &d = store.desc( id );
    // The mapping is the store's own, so a knob in the plugin and the same knob in phos_render
    // stand at the same place: normalised 0..1 through ParamStore::fromNormalised/toNormalised
    // (logarithmic where the descriptor says so, rounded where it is discrete).
    ParamStore *s = &store;
    m_range = juce::NormalisableRange<float>(
        d.m_minValue, d.m_maxValue, [s, id]( float, float, float n ) { return s->fromNormalised( id, n ); },
        [s, id]( float, float, float v ) { return s->toNormalised( id, v ); },
        [s, id]( float lo, float hi, float v )
        {
            const ParamDesc &dd = s->desc( id );
            const float      c = juce::jlimit( lo, hi, v );
            return ( dd.m_curve == CurveE::LINEAR || dd.m_curve == CurveE::LOG ) ? c : std::round( c );
        } );
}

float StoreParameter::getValue() const
{
    return m_store.toNormalised( m_id, m_store.get( m_id ) );
}

void StoreParameter::setValue( float newValue )
{
    m_store.setNormalised( m_id, newValue );
}

float StoreParameter::getDefaultValue() const
{
    return m_store.toNormalised( m_id, m_store.defaultValue( m_id ) );
}

juce::String StoreParameter::getName( int maximumStringLength ) const
{
    return m_name.substring( 0, maximumStringLength );
}

juce::String StoreParameter::getLabel() const
{
    return m_store.desc( m_id ).m_unit;
}

int StoreParameter::getNumSteps() const
{
    const ParamDesc &d = m_store.desc( m_id );
    if ( d.m_curve == CurveE::TOGGLE ) return 2;
    if ( d.m_curve == CurveE::CHOICE || d.m_curve == CurveE::INT )
        return static_cast<int>( std::lround( d.m_maxValue - d.m_minValue ) ) + 1;
    return juce::AudioProcessor::getDefaultNumParameterSteps();
}

bool StoreParameter::isDiscrete() const
{
    const CurveE c = m_store.desc( m_id ).m_curve;
    return c == CurveE::INT || c == CurveE::CHOICE || c == CurveE::TOGGLE;
}

bool StoreParameter::isBoolean() const
{
    return m_store.desc( m_id ).m_curve == CurveE::TOGGLE;
}

juce::String StoreParameter::getText( float normalisedValue, int maximumStringLength ) const
{
    const juce::String s = formatValue( m_store.desc( m_id ), m_store.fromNormalised( m_id, normalisedValue ) );
    return maximumStringLength > 0 ? s.substring( 0, maximumStringLength ) : s;
}

float StoreParameter::getValueForText( const juce::String &text ) const
{
    const ParamDesc &d = m_store.desc( m_id );
    if ( d.m_curve == CurveE::CHOICE && d.m_choices != nullptr )
    {
        for ( int i = 0; i <= static_cast<int>( d.m_maxValue ); ++i )
            if ( text.trim().equalsIgnoreCase( d.m_choices[i] ) )
                return m_store.toNormalised( m_id, static_cast<float>( i ) );
    }
    if ( d.m_curve == CurveE::TOGGLE )
    {
        const juce::String t = text.trim().toLowerCase();
        if ( t == "on" || t == "true" || t == "yes" ) return 1.0f;
        if ( t == "off" || t == "false" || t == "no" ) return 0.0f;
    }
    return m_store.toNormalised( m_id, static_cast<float>( text.getDoubleValue() ) );
}
