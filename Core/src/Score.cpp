/**
 * @file Score.cpp
 * @brief Score helpers.
 */
#include "phos/Score.h"
#include <algorithm>

namespace phos {

const char* const kPartNames[kNumParts] = { "Kick", "Bass", "Perc", "Acid", "Lead", "Arp", "Pad", "Sfx" };
const char* const kSectionNames[static_cast<int>(SectionType::Count)] = { "Intro", "Groove", "Build", "Drop", "Break", "Outro", "PDB", "Cut" };

bool noteLess(const NoteEvent& a, const NoteEvent& b)
{
    if (a.beat != b.beat) return a.beat < b.beat;
    if (a.part != b.part) return a.part < b.part;
    if (a.lane != b.lane) return a.lane < b.lane;
    return a.pitch < b.pitch;
}

void Score::sort()
{
    std::stable_sort(notes.begin(), notes.end(), noteLess);
    std::stable_sort(sections.begin(), sections.end(), [](const SectionMark& a, const SectionMark& b) { return a.beat < b.beat; });
}

double Score::endBeat() const
{
    double e = 0.0;
    for (const NoteEvent& n : notes) e = std::max(e, n.beat + static_cast<double>(n.length));
    return e;
}

} // namespace phos
