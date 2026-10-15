/**
 * @file Score.cpp
 * @brief Score helpers.
 * @author Rene Weller
 */
#include "phos/Score.h"
#include <algorithm>

namespace phos {

const char* const PartNames[NumParts] = { "Kick", "Bass", "Perc", "Acid", "Lead", "Counter", "Arp", "Stab", "Pad", "Drone",
                                            "Sfx", "Texture", "Vocal", "Field" };
const char* const SectionNames[static_cast<int>(SectionTypeE::COUNT)] = { "Intro", "Groove", "Build", "Drop", "Break", "Outro", "PDB", "Cut" };

bool noteLess(const NoteEvent& a, const NoteEvent& b)
{
    if (a.m_beat != b.m_beat) return a.m_beat < b.m_beat;
    if (a.m_part != b.m_part) return a.m_part < b.m_part;
    if (a.m_lane != b.m_lane) return a.m_lane < b.m_lane;
    return a.m_pitch < b.m_pitch;
}

void Score::sort()
{
    std::stable_sort(m_notes.begin(), m_notes.end(), noteLess);
    std::stable_sort(m_sections.begin(), m_sections.end(), [](const SectionMark& a, const SectionMark& b) { return a.m_beat < b.m_beat; });
}

double Score::endBeat() const
{
    double e = 0.0;
    for (const NoteEvent& n : m_notes) e = std::max(e, n.m_beat + static_cast<double>(n.m_length));
    return e;
}

} // namespace phos
