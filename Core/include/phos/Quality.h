/**
 * @file Quality.h
 * @brief Quality levels: the same engine on a desktop core and on the Quest's Snapdragon XR2.
 *
 * The Quest 2 runs a Kryo 585 (one prime and three big Cortex-A77 cores at about 2 GHz, four small
 * A55) with 128-bit NEON, so a lane register holds four floats instead of the eight of AVX2, and the
 * clock is roughly half a desktop core's. Audio gets one big core and the composer a small one, and
 * the budget for a full drop arrangement is 30 % of that big core at 48 kHz with 256-sample blocks
 * (docs/rounds/2026-09.md, section 9). The engine therefore has two levels, chosen once at Engine::prepare():
 *
 * | Setting | Desktop | Quest | Why |
 * |---|---|---|---|
 * | Bass oversampling | 2x | 2x | The bass is the one voice whose ladder is swept by a fast envelope over a fundamental that the kick is phase-locked to; its alias products land in the band the whole track is built on. It keeps its half-band. |
 * | Acid oversampling | 2x | 1x | The acid sits above D3 and is high-passed at 150 Hz; its aliasing folds into a band that the percussion and the leads already occupy. Halving its ladder saves the most of any single switch. |
 * | Unison oscillators per voice | 7 | 3 | Szabo's supersaw keeps its centre and its inner detuned pair (offsets -0.0195 and +0.0199); the wide pair and the outer pair fall away. The remaining gains are renormalised by incoherent power, so the level does not jump. |
 * | Pad voices | 8 | 4 | Pads play four-voice voicings (Melody.h), so four voices is what the part actually needs; the fifth to eighth exist for overlapping chord changes. Lead and arp stay at 8 -- they are monophonic lines whose voices only overlap during releases. |
 * | Drone voices | 8 | 6 | (19.09.2026) At most three held notes, and the next three cross-fading in at a boundary. Counter-lead and stab stay at 8. |
 * | Wavetable frames | 64 (all) | 32 | The shipped library is the engine's largest block of memory: 741 frames at 33 KB each are 23.3 MB once the ten mip levels are expanded, and 111 ms to build. Halving the frames halves both. |
 *
 * **Why 32 frames and not 16.** This is the one setting that costs no arithmetic at all, only
 * *grain*: the position knob crossfades between two neighbouring frames, so thinning makes every
 * step of the morph bigger. The selection of 16.09.2026 threw out tables that step instead of
 * gliding, and the measure it used is the one the self test repeats (`testWaveTableQuality`): the
 * median total variation between the power spectra of neighbouring frames. Over the five pad tables
 * the worst of those is 0.074 with all 64 frames, 0.173 at 32 and 0.261 at 16 -- against the 0.49
 * that marked a bank as a shaker. 32 frames buy the whole saving while the pad still glides; 16
 * would spend half the remaining distance to the rejected range for memory the Quest budget does not
 * ask for. `directness` (travel / path) is not harmed by thinning at all: both ends of the table are
 * kept, so `travel` is unchanged and `path` can only shrink.
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
    int polyUnison[kPolyInstances] = {};   ///< oscillators per voice, per Poly instance (set by the constructor)
    int polyVoices[kPolyInstances] = {};   ///< voices that may sound, per Poly instance (set by the constructor)
    int waveTableFrames = 0;        ///< frames a library table is built with; 0 = every frame (setWaveTableFrameLimit)

    /** @brief The desktop level: every instance at full unison and every voice (a loop, so a new instance cannot be missed). */
    Quality()
    {
        for (int i = 0; i < kPolyInstances; ++i) { polyUnison[i] = kPolyUnison; polyVoices[i] = kPolyVoices; }
    }

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
        q.polyVoices[polyIndex(PolyInstance::Pad)] = 4;
        // 19.09.2026: the drone holds at most three notes (root, fifth, octave) and cross-fades into the
        // next held chord at a boundary (Melody.h), so six voices cover the overlap; the stab keeps all
        // eight, because its short chords of four overlap their own releases on close hits.
        q.polyVoices[polyIndex(PolyInstance::Drone)] = 6;
        q.waveTableFrames = 32;
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
