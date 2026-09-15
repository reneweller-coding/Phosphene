/**
 * @file Quality.h
 * @brief Quality levels: the same engine on a desktop core and on the Quest's Snapdragon XR2.
 *
 * The Quest 2 runs a Kryo 585 (one prime and three big Cortex-A77 cores at about 2 GHz, four small
 * A55) with 128-bit NEON, so a lane register holds four floats instead of the eight of AVX2, and the
 * clock is roughly half a desktop core's. Audio gets one big core and the composer a small one, and
 * the budget for a full drop arrangement is 30 % of that big core at 48 kHz with 256-sample blocks
 * (docs/PLAN.md, section 9). The engine therefore has two levels, chosen once at Engine::prepare():
 *
 * | Setting | Desktop | Quest | Why |
 * |---|---|---|---|
 * | Bass oversampling | 2x | 2x | The bass is the one voice whose ladder is swept by a fast envelope over a fundamental that the kick is phase-locked to; its alias products land in the band the whole track is built on. It keeps its half-band. |
 * | Acid oversampling | 2x | 1x | The acid sits above D3 and is high-passed at 150 Hz; its aliasing folds into a band that the percussion and the leads already occupy. Halving its ladder saves the most of any single switch. |
 * | Unison oscillators per voice | 7 | 3 | Szabo's supersaw keeps its centre and its inner detuned pair (offsets -0.0195 and +0.0199); the wide pair and the outer pair fall away. The remaining gains are renormalised by incoherent power, so the level does not jump. |
 * | Pad voices | 8 | 4 | Pads play four-voice voicings (Melody.h), so four voices is what the part actually needs; the fifth to eighth exist for overlapping chord changes. Lead and arp stay at 8 -- they are monophonic lines whose voices only overlap during releases. |
 *
 * **What is not here.** The plan also lists "reverb mode Classic" and "convolver off". Phosphene's
 * send effects are one FDN with no mode switch (Reverb.h) and there is no convolver at all, so both
 * are already met and no switch is needed.
 *
 * **Defaults do not change anything.** A default-constructed Quality is the desktop level, and
 * Engine::prepare() takes it as its default argument, so every existing caller, the self test and the
 * vector tests render exactly what they rendered before. The lane kernels are untouched by all of
 * this: bit-identity between the scalar path and NEON does not depend on the level.
 */
#pragma once
#include "phos/Params.h"
#include "phos/PolyKernel.h"
#include <cstring>

namespace phos {

/** @brief What the engine may spend per part. */
struct Quality {
    /** @brief The two levels. */
    enum class Level : int { Desktop = 0, Quest };

    Level level = Level::Desktop;   ///< which level this is (for displays and reports)
    int bassOversampling = 2;       ///< ladder rate of the bass, 1 or 2 times the sample rate
    int acidOversampling = 2;       ///< ladder rate of the acid, 1 or 2 times the sample rate
    int polyUnison[kPolyInstances] = { kPolyUnison, kPolyUnison, kPolyUnison };   ///< oscillators per voice, per Poly instance
    int polyVoices[kPolyInstances] = { kPolyVoices, kPolyVoices, kPolyVoices };   ///< voices that may sound, per Poly instance

    /** @brief Everything at full: what the engine did before quality levels existed. */
    static Quality desktop() { return Quality{}; }

    /** @brief The Quest level of the table in the file comment. */
    static Quality quest()
    {
        Quality q;
        q.level = Level::Quest;
        q.bassOversampling = 2;
        q.acidOversampling = 1;
        for (int i = 0; i < kPolyInstances; ++i) q.polyUnison[i] = 3;
        q.polyVoices[static_cast<int>(PolyInstance::Pad)] = 4;
        return q;
    }

    /** @brief Display name of the level. */
    const char* name() const { return level == Level::Quest ? "quest" : "desktop"; }

    /**
     * @brief Looks a level up by name ("desktop" or "quest").
     * @param text name, case-sensitive
     * @param out  receives the level when the name is known
     * @return false when the name is neither of the two
     */
    static bool fromName(const char* text, Quality& out)
    {
        if (text == nullptr) return false;
        if (std::strcmp(text, "desktop") == 0) { out = desktop(); return true; }
        if (std::strcmp(text, "quest") == 0) { out = quest(); return true; }
        return false;
    }
};

} // namespace phos
