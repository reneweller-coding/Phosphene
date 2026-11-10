/**
 * @file FieldPresets.h
 * @brief The Field track's factory presets (27.09.2026): one per recording of the shipped library and a group of
 *        scenes of two, from Tools/field_presets.py (FieldPresetData.inl).
 *
 * The user: "Sollten wir für die vorhandenen Samples dann Presets schreiben, die einfach in den Sampler geladen werden
 * können?" A preset names its recordings by file and by their place in the shipped library: the file where the
 * library has it (a user folder beside the shipped one moves the numbers), the shipped place where it cannot say --
 * so the composer's score is the same on a machine without the recordings, and the plan snapshots with it.
 * Loaded by hand (the Field tab's preset menu) it sets the knobs; the composer plays one per track when layer A's
 * category is Auto, drawn by the track's style (fieldPresetFor), as bases and overrides that leave the knobs alone.
 * @author Rene Weller
 */
#pragma once
#include "phos/Params.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace phos
{

/** @brief One Field preset. */
struct FieldPreset
{
    const char *m_group;        ///< the menu's group (the category, or "Scenes")
    const char *m_name;         ///< the entry
    int         m_categoryA;    ///< layer A's category (FieldCategorySlugs index)
    const char *m_fileA;        ///< layer A's recording (the file's stem)
    int         m_variationA;   ///< its variation in the shipped library
    int         m_categoryB;    ///< layer B's category, -1 for no layer B
    const char *m_fileB;        ///< layer B's recording
    int         m_variationB;   ///< its variation in the shipped library
    const char *m_text;         ///< `key=value` lines of the Field module's knobs that differ from their defaults
    float       m_style[5];     ///< its weight in each style (StyleId order)
};

/**
 * @brief The factory presets, in menu order.
 * @return the presets
 */
const std::vector<FieldPreset> &fieldPresets();
/**
 * @brief The variation of @p category whose file is @p stem in the current library, -1 if it has none.
 * @param[in] category  the category
 * @param[in] stem      the file's stem
 * @return the variation, -1 for none
 */
int fieldVariationOf( int category, const std::string &stem );
/**
 * @brief Whether every recording of @p preset is in the current library.
 * @param[in] preset  the preset
 * @return true when every recording is there
 */
bool fieldPresetAvailable( const FieldPreset &preset );
/**
 * @brief Every Field knob the preset puts where: the defaults, its text, its recordings' categories and variations.
 * @param[in] p       the knobs
 * @param[in] preset  the preset
 * @return (index in the Field module, value) for every knob of the module but the strip's own (none today)
 */
std::vector<std::pair<int, float>> fieldPresetValues( const ParamStore &p, const FieldPreset &preset );
/**
 * @brief How a preset is played in a key so that a tone its recordings carry is in it (10.11.2026; FieldLibrary.h,
 *        fieldToneOf).
 *
 * The whole semitones go into the note (FieldPlayer transposes both layers by the note's distance from middle C), the
 * rest into each layer's fine tune -- not into the layers' pitch knobs: those are steps, a step reaches the engine as
 * an override, and an override under zero means "none" (Engine.cpp), so a transposition downwards never arrived.
 */
struct FieldTuning
{
    int   m_semis = 0;      ///< the note's distance from middle C, semitones
    float m_fineA = 0.0f;   ///< layer A's fine tune on top of the preset's, cents
    float m_fineB = 0.0f;   ///< layer B's
};
/**
 * @brief The tuning of @p preset in the key @p keyRoot: the tone of layer A (else of layer B) on the key's root, fourth
 *        or fifth, whichever is nearest; where both layers carry one, B takes the target its fine tune reaches best.
 * @param[in] preset   the preset
 * @param[in] keyRoot  the key (its pitch class is taken)
 * @return the tuning; all zero for recordings without a tone
 */
FieldTuning fieldPresetTuning( const FieldPreset &preset, int keyRoot );
/**
 * @brief The variation a preset's layer plays: the file's in the library if it is there, else the shipped one.
 * @param[in] category  the category
 * @param[in] file      the file
 * @param[in] shipped   the shipped variation
 * @return the variation
 */
int fieldPresetVariation( int category, const char *file, int shipped );
/**
 * @brief A preset for a track in style @p style (StyleId), drawn by @p seed with the presets' style weights -- over all
 *        of them, whether their files are there or not, so the score does not depend on the machine.
 * @param[in] style  the style
 * @param[in] seed   the seed
 * @return the preset's index
 */
int fieldPresetFor( int style, uint64_t seed );

}   // namespace phos
