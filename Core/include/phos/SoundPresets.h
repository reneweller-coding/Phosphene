/**
 * @file SoundPresets.h
 * @brief Sound presets for every synth, in groups (23.09.2026, round "Presets").
 *
 * The generator already makes a new sound for every synth in every track: a *recipe* -- for a polyphonic voice
 * an oscillator, a wavetable, a filter, a second oscillator and five perceptual directions (brightness,
 * softness, thickness, space, motion); for the kick and the bass five directions each; for the acid a point
 * between its three voicings -- turned into knob values by the same loadings the composer uses
 * (Composer::voiceRecipeOffsets, recipeOffsets, acidVoicingOffsets). The factory presets are those recipes laid
 * out on purpose instead of drawn: each voice's oscillators from its own palette with a set of characters
 * ("Bright", "Soft", "Thick", ...), every wavetable of the voice's lanes grouped by the library's families, the
 * kick per engine, the bass by character, the acid by voicing. So a preset sounds like something the generator
 * could have played on that voice, and the list grows with the wavetable pack without a line changing.
 *
 * A preset is the text of the module's knobs that differ from their defaults (ParamStore::parseText form), so a
 * user preset is the same thing saved from the knobs as they stand (moduleText).
 */
#pragma once

#include "phos/Params.h"

#include <string>
#include <vector>

namespace phos {

/** @brief One preset: where it is listed and what it sets. */
struct SoundPreset {
    std::string group;   ///< the submenu ("FM", "Wavetables: Ambient", "Resonant", "Liquid", ...)
    std::string name;    ///< the entry
    std::string text;    ///< `key=value` lines of the module's knobs that differ from their defaults
};

/**
 * @brief The factory presets of a synth: Module::Kick, Module::Bass, Module::Acid, or Module::Poly with the
 *        instance (PolyInstance order). Built once per synth, thread-safe; empty for any other module.
 */
const std::vector<SoundPreset>& factoryPresets(Module module, int instance = 0);

/**
 * @brief Whether a preset leaves knob @p k of @p module alone: the level, the ducking and, on a polyphonic voice,
 *        the high pass and the trance gate -- where the mix and the form put the synth, not what it sounds like.
 */
bool presetLeaves(Module module, int k);

/** @brief Puts the module's knobs to their defaults and applies @p text (a preset's); presetLeaves() knobs stay. */
bool applySoundPreset(ParamStore& params, Module module, int instance, const std::string& text);

/** @brief The module's knobs that differ from their defaults, as `key=value` lines (a user preset); presetLeaves() knobs are left out. */
std::string moduleText(const ParamStore& params, Module module, int instance = 0);

} // namespace phos
