/**
 * @file selftest.cpp
 * @brief phos_selftest: measured checks of every building block and of the engine as a whole.
 *
 * Each check measures a property against a value derived independently of the code under test --
 * the analytic response of a filter, the closed-form tempo integral, the pattern definition --
 * rather than against a recording of what the code once produced.
 */
#include "phos/Acid.h"
#include "phos/Audibility.h"
#include "phos/Composer.h"
#include "phos/Corpus.h"
#include "phos/Cue.h"
#include "phos/DiodeLadder.h"
#include "phos/Dynamics.h"
#include "phos/Dsp.h"
#include "phos/Engine.h"
#include "phos/FieldLibrary.h"
#include "phos/Halfband.h"
#include "phos/Harmony.h"
#include "phos/Ladder.h"
#include "phos/Loudness.h"
#include "phos/Midi.h"
#include "phos/MidiMap.h"
#include "phos/Model.h"
#include "phos/Oscillator.h"
#include "phos/Patterns.h"
#include "phos/Melody.h"
#include "phos/Perc.h"
#include "phos/Poly.h"
#include "phos/Preferences.h"
#include "phos/Probe.h"
#include "phos/Rating.h"
#include "phos/Reverb.h"
#include "phos/Rhythm.h"
#include "phos/Form.h"
#include "phos/Gallery.h"
#include "phos/SetFile.h"
#include "phos/Sfx.h"
#include "phos/SoundPresets.h"
#include "phos/TranceGate.h"
#include "phos/WaveTable.h"
#include "phos/WaveTableFile.h"
#include "phos/WavWriter.h"
#include "TestSupport.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <tuple>

using namespace phos;
using namespace phostest;

#include "SelfTests.h"

// Round "speed" (20.09.2026): the probe scheduler's opt-in, and the two sections of Tests/selftest_probe.cpp
// (a file of their own so that they do not collide with the rounds that work in this one). phos/Probe.h is
// included at the top of this file (round "test-speed-rest", 20.09.2026): several sections now use
// phos::probe::runAll themselves, so the include had to be visible before their definitions, not just here.
void testProbeSchedule();
void testProbeCache();

/**
 * @brief Runs the self-test sections: all of them, the named ones, or none but their names.
 *
 * The `run("name", fn)` lines below are the one table of sections. Nothing else lists them:
 * Tests/selftest_tests.cmake asks this binary for the table (`--list`) every time ctest starts and
 * registers each section as a test of its own (`selftest.<name>`), so a section added here is in
 * ctest the moment it is built, and one that is removed cannot linger as a stale name.
 *
 * Selection, in order of precedence:
 * - `--list` prints the table's names, one per line, and runs nothing.
 * - `--only a[,b...]` (also `--only=a,b`) runs exactly the named sections -- whole names, because
 *   the substring rule below runs every section whose name occurs *inside* the string: asking for
 *   `testAcidColour` also runs testAcid, `testMidiKeys` also testMidi, `testWaveTableLibrary` also
 *   testWaveTable. A ctest test must measure one section, not two. A name that is not in the table counts
 *   as a failed check, so a typo or a renamed section fails loudly instead of passing with nothing
 *   checked. `PHOS_ONLY` is ignored then. A section split into parts is named `group.part`; the
 *   group's name alone selects all its parts (`--only testVoices`), a whole part name one of them.
 * - `PHOS_ONLY=a[,b...]` keeps its old meaning for work by hand: every section whose name -- or,
 *   for a part, whose group's name -- occurs in the string.
 * - nothing: every section in table order, in this one process (ctest's `selftest`, label `full`).
 *
 * Each section that runs ends with a line `== <name>: <seconds> s`, the wall time of that section
 * alone, which is what the ctest costs and the quick/slow labels are measured from.
 *
 * @param argc argument count
 * @param argv `--list`, `--only <names>`
 * @return 0 when every check passed, 1 when one failed, 2 on a bad command line
 */

/** @brief Runs the self test: every test, or those named by --only; --list prints the run table. */
int main(int argc, char** argv)
{
    // A development program: the composer's probes run in parallel, and from the probe cache when
    // PHOS_PROBE_CACHE names a directory (ctest does; phos/Probe.h). Neither changes a number.
    phos::probe::configureFromEnvironment();
    // The Field track's recordings are loaded when the composer plans a track (FieldLibrary.h). In a plugin that is
    // asked of the loader thread, and a recording may arrive a moment into its first note; the tests compare renders
    // bit for bit (with and without the cue tap, at any block size), so here, as in phos_render, the composer waits.
    phos::setFieldPreloadBlocking(true);
    // Unbuffered: under ctest stdout is a pipe and fully buffered, so a crash took every line the
    // section had printed with it. On 19.09.2026 selftest.testVoices died with an access violation
    // after 249 s and ctest recorded no output at all; the check that ran last is the first clue.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool listOnly = false;          // --list: print the table, run nothing
    bool haveOnly = false;          // --only was given
    std::vector<std::string> wanted;  // --only's names, each once
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        std::string names;
        if (a == "--list") { listOnly = true; continue; }
        if (a == "--only" && i + 1 < argc) names = argv[++i];
        else if (a.rfind("--only=", 0) == 0) names = a.substr(7);
        else {
            std::fprintf(stderr, "phos_selftest: unknown argument '%s'\nusage: phos_selftest [--list] [--only name[,name...]]\n", argv[i]);
            return 2;
        }
        haveOnly = true;
        for (size_t p = 0; p <= names.size();) {
            const size_t q = std::min(names.find(',', p), names.size());
            const std::string n = names.substr(p, q - p);
            if (!n.empty() && std::find(wanted.begin(), wanted.end(), n) == wanted.end()) wanted.push_back(n);
            p = q + 1;
        }
    }
    if (haveOnly && wanted.empty()) { std::fprintf(stderr, "phos_selftest: --only names no section\n"); return 2; }
    if (!listOnly) std::printf("phos_selftest (vector path %s)\n", kVecPathName);
    // PHOS_ONLY=testName[,testName...] runs only those tests (for work on one building block).
    const char* only = (listOnly || haveOnly) ? nullptr : std::getenv("PHOS_ONLY");
    // Every --only name is charged as a failed check up front and discharged when its section is
    // reached; what is left at finish() is a name the table does not have.
    std::vector<bool> reached(wanted.size(), false);
    if (listOnly) { wanted.clear(); haveOnly = false; }
    tally().failed += static_cast<int>(wanted.size());
    if (haveOnly) std::printf("only %zu section(s); a name not in the run table stays counted as a failed check\n", wanted.size());
    // A section split into parts (19.09.2026, round "test-split") is a group `name.part`: `--only name`
    // runs all its parts, `--only name.part` one of them. Still whole names -- testMidi is not a group
    // of testMidiKeys, because only the text before the dot is the group.
    auto groupOf = [](const std::string& n) { return n.substr(0, n.find('.')); };
    auto pick = [&](const char* name) {
        for (size_t k = 0; k < wanted.size(); ++k)
            if (wanted[k] == name || wanted[k] == groupOf(name)) {
                if (!reached[k]) { reached[k] = true; --tally().failed; }
                return true;
            }
        return false;
    };
    auto run = [&](const char* name, void (*fn)()) {
        if (listOnly) { std::printf("%s\n", name); return; }
        // PHOS_ONLY keeps its substring rule, applied to the group as well: PHOS_ONLY=testVoices still runs
        // every part of testVoices, as it ran the whole section before the split -- but where the group's
        // name is followed by a dot it names one part (PHOS_ONLY=testVoices.sound), not the group.
        auto groupNamed = [&](const std::string& g) {
            for (const char* at = std::strstr(only, g.c_str()); at != nullptr; at = std::strstr(at + 1, g.c_str()))
                if (at[g.size()] != '.') return true;
            return false;
        };
        if (haveOnly ? !pick(name)
                     : (only != nullptr && std::strstr(only, name) == nullptr && !groupNamed(groupOf(name)))) return;
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        std::printf("== %s: %.1f s\n", name, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        std::fflush(stdout);
    };
    // The measurement bench prints tables and checks nothing; it reproduces the numbers of the
    // DSP quality round of 16.09.2026 (docs/rounds/2026-09.md) and runs only when it is named by itself.
    // Neither it nor the probe audit is in the table, so neither is a ctest test of its own.
    auto optIn = [&](const char* name) { return listOnly ? false : haveOnly ? pick(name) : (only != nullptr && std::strstr(only, name) != nullptr); };
    if (optIn("testMeasure")) testMeasure();
    if (optIn("testProbeAudit")) testProbeAudit();
    run("testSampler", testSampler);
    run("testModelKernel", testModelKernel);
    run("testModelFile", testModelFile);
    run("testModelDecode", testModelDecode);
    run("testMelodyModelWiring", testMelodyModelWiring);
    run("testDiodeLadder", testDiodeLadder);
    run("testAcid", testAcid);
    run("testPoly", testPoly);
    run("testAcidColour", testAcidColour);
    run("testMelody.score", testMelodyScore);
    run("testMelody.variety", testMelodyVariety);
    run("testMelody.depthRender", testMelodyDepthRender);
    run("testMelody.blockSize", testMelodyBlockSize);
    run("testMelody.midi", testMelodyMidi);
    run("testWaveTable", testWaveTable);
    run("testWaveTableLibrary", testWaveTableLibrary);
    run("testLoaderThreadSafety", testLoaderThreadSafety);
    run("testWaveTableQuality", testWaveTableQuality);
    run("testPads", testPads);
    run("testGateAndDuck", testGateAndDuck);
    run("testReverb", testReverb);
    run("testGatedReverb", testGatedReverb);
    run("testDynamics", testDynamics);
    run("testSfx", testSfx);
    run("testWanderingFx", testWanderingFx);
    run("testMaster", testMaster);
    run("testSfxLevel", testSfxLevel);
    run("testPsychedelia", testPsychedelia);
    run("testMixBalance", testMixBalance);
    run("testBandLimit", testBandLimit);
    run("testStereoWidth", testStereoWidth);
    run("testModalInterchange.bass", testModalInterchangeBass);
    run("testModalInterchange.modes", testModalInterchangeModes);
    run("testModalInterchange.presenceOn1", testModalInterchangePresenceOn1);
    run("testModalInterchange.presenceOn2", testModalInterchangePresenceOn2);
    run("testModalInterchange.presenceOff1", testModalInterchangePresenceOff1);
    run("testModalInterchange.presenceOff2", testModalInterchangePresenceOff2);
    run("testModalInterchange.presenceArcOn1", testModalInterchangePresenceArcOn1);
    run("testModalInterchange.presenceArcOn2", testModalInterchangePresenceArcOn2);
    run("testModalInterchange.presenceArcOff1", testModalInterchangePresenceArcOff1);
    run("testModalInterchange.presenceArcOff2", testModalInterchangePresenceArcOff2);
    run("testModalInterchange.newTone", testModalInterchangeNewTone);
    run("testModeColour", testModeColour);
    run("testTensionCurve", testTensionCurve);
    run("testMotifOperators", testMotifOperators);
    run("testArpPatterns", testArpPatterns);
    run("testGenreRules.rules", testGenreRulesRules);
    run("testGenreRules.padNoFlat9", testGenreRulesPadNoFlat9);
    run("testGenreRules.heldNoFlat9", testGenreRulesHeldNoFlat9);
    run("testGenreRules.bassAndDrone", testGenreRulesBassAndDrone);
    run("testGenreRules.counterAgainstLead", testGenreRulesCounterAgainstLead);
    run("testSfxToneIntervals", testSfxToneIntervals);
    run("testMixGuide.matrix", testMixGuideMatrix);
    run("testMixGuide.leadDucks", testMixGuideLeadDucks);
    run("testMixGuide.planes", testMixGuidePlanes);
    run("testMixGuide.distance", testMixGuideDistance);
    run("testMixGuide.phase", testMixGuidePhase);
    run("testFilterModels.levels", testFilterModelsLevels);
    run("testModulation.blockSize", testModulationBlockSize);
    run("testModulation.sync", testModulationSync);
    run("testModulation.filterAdsr", testModulationFilterAdsr);
    run("testModulation.display", testModulationDisplay);
    run("testPresetBank", testPresetBank);
    run("testField.loop", testFieldLoop);
    run("testField.engine", testFieldEngine);
    run("testField.form", testFieldForm);
    run("testField.presets", testFieldPresets);
    run("testField.noLibrary", testFieldNoLibrary);
    run("testGenreRules.listeningSeed", testGenreRulesListeningSeed);
    run("testGenreRules.arpGate", testGenreRulesArpGate);
    run("testFoundation.score", testFoundationScore);
    run("testFoundation.render", testFoundationRender);
    run("testVoices.score", testVoicesScore);
    run("testVoices.droneRender", testVoicesDroneRender);
    run("testBed.audible", testBedAudible);
    run("testVoices.sound", testVoicesSound);
    run("testVoices.counterSoundListening", testVoicesCounterSoundListening);
    run("testVoices.counterSound77", testVoicesCounterSound77);
    run("testVoices.counterSound2026", testVoicesCounterSound2026);
    run("testVoices.acidRide", testVoicesAcidRide);
    run("testDialogue.score", testDialogueScore);
    run("testDialogue.glide", testDialogueGlide);
    run("testDialogue.sound", testDialogueSound);
    run("testDialogue.levels", testDialogueLevels);
    run("testForm", testForm);
    run("testSetArc", testSetArc);
    run("testRatings", testRatings);
    run("testMidiMap", testMidiMap);
    run("testGallery", testGallery);
    run("testSoloTrack", testSoloTrack);
    run("testPreferences", testPreferences);
    run("testKnobFuzz", testKnobFuzz);
    run("testStems", testStems);
    run("testSoundPresets", testSoundPresets);
    run("testDeferredPlan", testDeferredPlan);
    run("testPlanCacheLive", testPlanCacheLive);
    run("testKeyboard", testKeyboard);
    run("testAudibility", testAudibility);
    run("testAudibilityMatch", testAudibilityMatch);
    run("testArrangeDynamics", testArrangeDynamics);
    run("testSectionRules", testSectionRules);
    run("testCuration", testCuration);
    run("testTransitions", testTransitions);
    run("testArrangement", testArrangement);
    run("testClimax", testClimax);
    run("testPresence", testPresence);
    run("testCues", testCues);
    run("testParams", testParams);
    run("testTempo", testTempo);
    run("testHalfband", testHalfband);
    run("testOscillator", testOscillator);
    run("testLadder", testLadder);
    run("testKick", testKick);
    run("testBass", testBass);
    run("testComposer", testComposer);
    run("testBassModel", testBassModel);
    run("testVariety.plans", testVarietyPlans);
    run("testVariety.levelMatch", testVarietyLevelMatch);
    run("testVariety.recipes", testVarietyRecipes);
    run("testEngine", testEngine);
    run("testKickReference", testKickReference);
    run("testBassBite", testBassBite);
    run("testKickBody", testKickBody);
    run("testAcidVoicing.corners", testAcidVoicingCorners);
    run("testAcidVoicing.night", testAcidVoicingNight);
    run("testAcidVoicing.engine", testAcidVoicingEngine);
    run("testRecipeSpread", testRecipeSpread);
    run("testPercTempo", testPercTempo);
    run("testPhaseLock.lock", testPhaseLockLock);
    run("testPhaseLock.onsets", testPhaseLockOnsets);
    run("testBassRhythm", testBassRhythm);
    run("testPercKit", testPercKit);
    run("testRhythm", testRhythm);
    run("testMidi", testMidi);
    run("testMidiKeys", testMidiKeys);
    run("testWav", testWav);
    run("testLoudness", testLoudness);
    run("testProbeSchedule", testProbeSchedule);
    run("testProbeCache", testProbeCache);
    return finish();
}
