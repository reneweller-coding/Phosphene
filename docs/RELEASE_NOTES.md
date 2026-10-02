# Phosphene release notes

## 1.5.0 (03.10.2026): the family plays together

**The family jam** (Settings > Family jam: Off, Lead or Follow; in the plugin and in the standalone). The five
instruments -- Totality, Parhelion, Ephemeris, Phosphene and Noctuary -- play as one band on the local network. The
leader's key, the energy of its sections and its breaks and drops go out over UDP multicast; a follower takes the new
root at its next bar line by the shortest way and the mode with its next track, and in Phosphene the bass, the 303,
the lead, the counter, the arp, the stab, the pad and the drone move to the leader's key while the kit and the effects
play as written, and the set's walk holds its key; the Filter Sweep macro closes with a quiet section of the leader's;
and in the leader's breaks the kick and the bass are out until its drop. A major mode of the leader's it leaves be:
psytrance's palette has none. Ableton Link or the DAW's transport gives them the same bars, so that a section lands on
the same bar line everywhere; a leader that falls silent for four seconds leaves its followers to themselves.

**An Audio Unit on the Mac, an LV2 on Linux.** The macOS zip has the Audio Unit beside the standalone and the VST3
(Logic, GarageBand, MainStage), passed by Apple's `auval -strict` on GitHub's runners; the Linux archive has the LV2
(Ardour, Carla, Reaper, Qtractor), read by lilv there.

**Kaleidoscope.** KaleidoscopeEnhanced understands the score cues of all five instruments as they come (since
02.10.2026) -- Totality's blocks and keys, Parhelion's sections, Ephemeris' phases, Phosphene's sections and drops,
Noctuary's bars, keys and scenes -- and cuts its pictures to them, on a drop at once.

**Behind the panel.** A test of the follower's engine (`testJam`): the transposition note for note, the drums as
written, the break without its foundation. The jam's bus (Plugin/Jam.h) is the same file in all five repositories.

## 1.4.0 (02.10.2026): with a DAW, on a Mac, and heard

**MIDI out, written down.** The plugin has always sent its score as MIDI in a DAW, a channel per part; the manual
says so now (Export).

**Outputs of their own.** Besides the main output, a stereo output for each of its stems (the kick, the bass, the percussion, the acid, the lead, the counter, the arp, the stab, the pad, the drone, the effects, the texture, the voice, the field recordings and the returns), off until the
DAW switches them on; each carries its part before the master while the main output plays on.

**Ableton Link** in the standalone (Settings > Ableton Link, off to begin with): with other Link apps in the session
Phosphene takes their tempo, lines its bars up with theirs and starts and stops with them; alone it offers its own tempo.

**The keyboard's options**, each off until chosen (the Keyboard group): Lower Keys Play and Split At divide the keys
between two voices, Scale Lock keeps every key in the track's key and mode, Velocity Curve (As Played, Soft, Hard, Fixed) shapes the
touch. A release always ends the note its press started.

**On a Mac.** Every release gets a build for Apple Silicon (macOS 12 or newer) -- the standalone and the VST3 --,
built and tested on GitHub's runners by the workflow `macos` and attached to the release as `Phosphene-<version>-macOS.zip`.
It is signed ad hoc, not notarized (that takes a paid Apple account; README-macOS.txt in the zip says how to open it),
and it has not yet been played on a real Mac.

**Demos.** The release "demos" holds a track per style as an MP3 and one of them as a video with pictures by
[KaleidoscopeEnhanced](https://github.com/reneweller-coding/KaleidoscopeEnhanced), its cuts placed by the track's own
score cues; `Tools/demo/make_demos.py` renders them all again.

**Behind the panel.** A sound check (`Tools/soundcheck.py`, `ctest -L sound`): five styles, three minutes each, and their stems, measured by loudness
(BS.1770) against `Tests/golden/soundcheck.json`, so a change that makes a style louder, quieter or emptier shows up
before anybody listens. A CI run on every push (GitHub Actions: the build and the tests on Windows, the documentation
check on Linux) that nobody waits for. One release script for the family (`Deploy/publish_release.ps1`: the notes from
this file, the checksums, the tag, the release).

**On Linux** (the same evening, attached to the release afterwards): an archive for x86-64 with the
standalone, the VST3 and the renderer, built by the workflow `linux` on GitHub's Ubuntu 22.04 runners
(GCC 12), its quick tests run there, and tried under WSL: the window, and the sound check against the
Windows renders.

## 1.3.0 (01.10.2026): the family's panel

**One panel for the family.** Phosphene shares its panel with Ephemeris, Totality and Parhelion now (Plugin/Frame.h,
the same file in each): the header's two rows, the overview, the tabs in groups with small tabs (Set, Arrange, Low End,
Percussion, Acid, Synths, Effects, Mixer, Perform, Export, Gallery), a tab of its own for Export, Undo, Redo, Help and
the settings as icons, the same keys and the same right click on every control (MIDI learn, forget, the default
value). The controllers every generator listens to: 74 the filter sweep, 11 the gate depth, 64 the stutter. The
headset's controls only while a headset sends (port 9101); the Quest app plays the desktop's Phosphene in bridge mode,
in the grammar every generator's headset shares, with the plugin's four macros.

**Play it yourself.** The keyboard plays the bass and the kit too (C1 the kick, C#1 to C2 the twelve lanes; by channel
8 the bass, 10 the kit), and Composer (beside Keyboard Plays) off leaves every generated note out, so only what is
played sounds. Replace and the composer switch act in the plugin only: an export and phos_render play what was
composed, whatever a set's keyboard knobs say.

**A loaded set lets go of the macros.** Filter Sweep, Gate Depth, Drop-out and Stutter are performance, not part of
the set; loading a set (or a DAW restoring its project) now puts all four back to neutral. Before, pluginval's state
test failed in about every second run: the drop-out let go by itself while the state was being restored.

**One layout for the repositories.** Every instrument of the family builds the same way now: `build.ps1` (msvc, icx,
release, quest) on the presets of `CMakePresets.json`, the build trees under `build\<preset>`, everything that can be
started -- the standalone, the VST3, the renderer -- flat in `bin\msvc` and `bin\icx` (and the Quest APK in
`bin\quest`), the release in `dist\`, local data, renders and logs in `work\` (`cmake/Family.cmake`).

**Every line explained.** Every class, function, variable, macro and table of the sources -- the core, the plugin,
the Quest app, the tools and the tests -- has its Doxygen comment now, and the test `doccheck` (`cmake/Family.cmake`)
fails as soon as one is missing. The scripts that generate tables write the comments into what they generate.

**No page scrolls.** The window opens at 1280 x 860, as every generator of the family's. A tab of several modules has a
small tab for each, and a page that is still taller than the window shows its groups in sections, one at a time --
the sound and the modulation apart, cut further where needed (Filter, LFO, Matrix ...) --, switched at its top right.
The overview above the tabs can be folded away in the settings.

The notes of the releases before (1.0.0 to 1.2.0) are on their release pages on GitHub.
