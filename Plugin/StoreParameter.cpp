/**
 * @file StoreParameter.cpp
 * @brief The host's view of one engine parameter (PluginProcessor.h, StoreParameter).
 */
#include "PluginProcessor.h"
#include <cmath>

using namespace phos;

namespace {

/** @brief A value formatted the way phos::ParamStore::format would, but for an arbitrary value. */
juce::String formatValue(const ParamDesc& d, float v)
{
    if (d.curve == Curve::Choice && d.choices != nullptr) {
        const int i = juce::jlimit(0, static_cast<int>(d.maxValue), static_cast<int>(std::lround(v)));
        return d.choices[i];
    }
    if (d.curve == Curve::Toggle) return v >= 0.5f ? "On" : "Off";
    if (d.curve == Curve::Int) return juce::String(static_cast<int>(std::lround(v)));
    const float a = std::fabs(v);
    return juce::String(v, a >= 100.0f ? 0 : (a >= 10.0f ? 1 : 2));
}

} // namespace

// ==================================================================== StoreParameter

StoreParameter::StoreParameter(ParamStore& store, int id, const juce::String& name)
    : juce::RangedAudioParameter(juce::ParameterID(juce::String(store.key(id)), 1), name,
                                 juce::AudioProcessorParameterWithIDAttributes().withLabel(store.desc(id).unit)),
      store_(store), id_(id), name_(name)
{
    const ParamDesc& d = store.desc(id);
    // The mapping is the store's own, so a knob in the plugin and the same knob in phos_render
    // stand at the same place: normalised 0..1 through ParamStore::fromNormalised/toNormalised
    // (logarithmic where the descriptor says so, rounded where it is discrete).
    ParamStore* s = &store;
    range_ = juce::NormalisableRange<float>(
        d.minValue, d.maxValue,
        [s, id](float, float, float n) { return s->fromNormalised(id, n); },
        [s, id](float, float, float v) { return s->toNormalised(id, v); },
        [s, id](float lo, float hi, float v) {
            const ParamDesc& dd = s->desc(id);
            const float c = juce::jlimit(lo, hi, v);
            return (dd.curve == Curve::Linear || dd.curve == Curve::Log) ? c : std::round(c);
        });
}

float StoreParameter::getValue() const { return store_.toNormalised(id_, store_.get(id_)); }

void StoreParameter::setValue(float newValue) { store_.setNormalised(id_, newValue); }

float StoreParameter::getDefaultValue() const { return store_.toNormalised(id_, store_.defaultValue(id_)); }

juce::String StoreParameter::getName(int maximumStringLength) const { return name_.substring(0, maximumStringLength); }

juce::String StoreParameter::getLabel() const { return store_.desc(id_).unit; }

int StoreParameter::getNumSteps() const
{
    const ParamDesc& d = store_.desc(id_);
    if (d.curve == Curve::Toggle) return 2;
    if (d.curve == Curve::Choice || d.curve == Curve::Int)
        return static_cast<int>(std::lround(d.maxValue - d.minValue)) + 1;
    return juce::AudioProcessor::getDefaultNumParameterSteps();
}

bool StoreParameter::isDiscrete() const
{
    const Curve c = store_.desc(id_).curve;
    return c == Curve::Int || c == Curve::Choice || c == Curve::Toggle;
}

bool StoreParameter::isBoolean() const { return store_.desc(id_).curve == Curve::Toggle; }

juce::String StoreParameter::getText(float normalisedValue, int maximumStringLength) const
{
    const juce::String s = formatValue(store_.desc(id_), store_.fromNormalised(id_, normalisedValue));
    return maximumStringLength > 0 ? s.substring(0, maximumStringLength) : s;
}

float StoreParameter::getValueForText(const juce::String& text) const
{
    const ParamDesc& d = store_.desc(id_);
    if (d.curve == Curve::Choice && d.choices != nullptr) {
        for (int i = 0; i <= static_cast<int>(d.maxValue); ++i)
            if (text.trim().equalsIgnoreCase(d.choices[i])) return store_.toNormalised(id_, static_cast<float>(i));
    }
    if (d.curve == Curve::Toggle) {
        const juce::String t = text.trim().toLowerCase();
        if (t == "on" || t == "true" || t == "yes") return 1.0f;
        if (t == "off" || t == "false" || t == "no") return 0.0f;
    }
    return store_.toNormalised(id_, static_cast<float>(text.getDoubleValue()));
}
