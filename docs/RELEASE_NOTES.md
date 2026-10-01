# Phosphene release notes

## Next: the family's panel (01.10.2026, not yet released)

**One panel for the family.** Phosphene shares its panel with Ephemeris, Totality and Parhelion now (Plugin/Frame.h,
the same file in each): the header's two rows, the overview, the tabs in groups with small tabs (Set, Arrange, Low End,
Percussion, Acid, Synths, Effects, Mixer, Perform, Export, Gallery), a tab of its own for Export, Undo, Redo, Help and
the settings as icons, the same keys and the same right click on every control (MIDI learn, forget, the default
value). The controllers every generator listens to: 74 the filter sweep, 11 the gate depth, 64 the stutter. The
headset's controls only while a headset sends (port 9101); the Quest app plays the desktop's Phosphene in bridge mode,
in the grammar every generator's headset shares, with the plugin's four macros.

**One layout for the repositories.** Every instrument of the family builds the same way now: `build.ps1` (msvc, icx,
release, quest) on the presets of `CMakePresets.json`, the build trees under `build\<preset>`, everything that can be
started -- the standalone, the VST3, the renderer -- flat in `bin\msvc` and `bin\icx` (and the Quest APK in
`bin\quest`), the release in `dist\`, local data, renders and logs in `work\` (`cmake/Family.cmake`).

**No page scrolls.** The window opens at 1280 x 860, as every generator of the family's. A tab of several modules has a
small tab for each, and a page that is still taller than the window shows its groups in sections, one at a time --
the sound and the modulation apart, cut further where needed (Filter, LFO, Matrix ...) --, switched at its top right.
The overview above the tabs can be folded away in the settings.

The notes of the releases before (1.0.0 to 1.2.0) are on their release pages on GitHub.
