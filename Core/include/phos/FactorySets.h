/**
 * @file FactorySets.h
 * @brief The factory's sets (17.10.2026): 256 sets to begin a night with, each a `.phosset` with a name, its start
 *        style, its arc (or "Long night") and its length.
 *
 * A factory set is what a player would save after an evening of curating: a seed, the start style, the energy arc,
 * the set's length (compose.set_minutes), and a few of the composer's knobs drawn near their defaults -- the tempo's
 * range, a track's length, how much acid, lead, arp, pad and effects, the percussion, the melodies' temperature, the
 * squelches and the gates. Everything else stays at its default, so the composer picks every track's sounds as always.
 * Loaded in the plugin (the Sets browser), listed by the render app (Phosphene Sets) and by Heliacal.
 * Tools/sets/make_sets.py draws them (FactorySetsData.inc); a set plays exactly its length, which the table carries.
 * @author Rene Weller
 */
#pragma once

namespace phos
{

/** @brief A set of the factory. */
struct FactorySet
{
    const char *m_name;      ///< its name
    const char *m_section;   ///< its start style (StyleNames)
    const char *m_group;     ///< its arc (ArcNames), or "Long night"
    const char *m_note;      ///< a few words: its length, where it starts
    const char *m_text;      ///< the `.phosset` (readSetText)
    double      m_seconds;   ///< how long it plays (compose.set_minutes)
};

/**
 * @brief How many sets the factory has.
 * @return the count
 */
int numFactorySets();

/**
 * @brief A set of the factory.
 * @param[in] index  0 .. numFactorySets() - 1 (clamped)
 * @return the set
 */
const FactorySet &factorySet( int index );

/**
 * @brief A set of the factory by its name.
 * @param[in] name  the name
 * @return its index, -1 when there is none of that name
 */
int findFactorySet( const char *name );

}   // namespace phos
