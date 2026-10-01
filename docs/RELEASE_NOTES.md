# Phosphene release notes

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
