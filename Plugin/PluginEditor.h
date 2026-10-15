/**
 * @file PluginEditor.h
 * @brief The editor: the shared frame's header (Frame.h, 01.10.2026), the set's overview strip, the tabs in groups, and
 *        pages built from the parameter tables.
 *
 * The header and its keys are the ones every generator has (Frame.h): the start's style, key and scale, the set's length,
 * Compose set, New seed, Play, Mute; the ratings, the status, undo, redo, help and the settings. Under it the set as a
 * strip (the Arrange tab's top, to scale), a click jumps. The tabs follow PLAN 8.1, grouped as in every generator so the
 * row stays short: Set, Arrange, Low End (Kick, Bass), Percussion (twelve lanes behind a lane bar), Acid, Synths (Lead,
 * Counter, Arp, Stab, Pad, Drone), Effects (SFX + FX, Field), Mixer + Master, Perform, Export, Gallery. None of the pages knows a coordinate; each is a
 * phosui::ControlPage that is handed slices of a module's descriptor table and measures itself
 * (EditorLayout.h). The window has a design size -- the size at which every page fits without
 * scrolling -- and is scaled to whatever the screen or the host offers.
 *
 * `PHOS_SHOT=<file.png>` renders the editor at its design size into a PNG and quits; with
 * `PHOS_TAB=<index>` it opens a given tab first, and `PHOS_SHOT_ALL=<folder>` writes one picture per
 * tab. The picture is a component snapshot, never a grab of the screen: a screen grab takes whatever
 * window happens to be in front (the lesson from Noctuary's GUI round 8). `PHOS_PLAY=<seconds>`,
 * with `PHOS_RECORD=<file.wav>`, plays for a while and quits -- the way to hear the live path
 * without a hand on the mouse.
 */
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "EditorArrange.h"
#include "EditorLayout.h"
#include "EditorMixer.h"
#include "EditorScope.h"
#include "PhospheneLookAndFeel.h"
#include "PluginProcessor.h"
#include "UpdateCheck.h"
#include <memory>
#include <vector>

/**
 * @brief The tabs, by name rather than by number.
 *
 * They were plain indices until the Arrange and Perform tabs arrived in the middle of the row and
 * every `m_tab == 3` in the file meant something else.
 */
enum TabE : int {
    TAB_SET = 0, TAB_ARRANGE, TAB_KICK, TAB_BASS, TAB_PERC, TAB_ACID,
    // 19.09.2026: the polyphonic voices in their groups (Params.h, PolyInstance), each beside its partner.
    TAB_LEAD, TAB_COUNTER, TAB_ARP, TAB_STAB, TAB_PAD, TAB_DRONE,
    TAB_FX,
    TAB_FIELD,     ///< 27.09.2026: the field recordings' sampler (FieldPlayer.h)
    TAB_MIX, TAB_PERFORM,
    TAB_EXPORT,    ///< 01.10.2026: the files -- the recording, the MIDI, the set, the score cues -- as every generator has them
    TAB_GALLERY,   ///< 23.09.2026: saved sets with their form, loaded with a click (EditorGallery.cpp)
    TAB_COUNT
};

/**
 * @brief The editor's fixed-size body.
 *
 * Everything is laid out once in a design space of PhospheneEditor::m_designW by m_designH and the
 * whole body is then scaled to the window. Nothing reflows when the window is dragged -- it only
 * gets bigger or smaller -- which is what makes a picture taken at design size the truth about the
 * layout, and what keeps the measurement in one place.
 */
class EditorContent final : public juce::Component {
public:
    std::function<void(juce::Graphics&)> m_onPaint;   ///< the editor draws the frame
    std::function<void()> m_onResized;                ///< the editor places its children
    /** @brief Draws through onPaint. */
    void paint(juce::Graphics& g) override { if (m_onPaint) m_onPaint(g); }
    /** @brief Lays out through onResized. */
    void resized() override { if (m_onResized) m_onResized(); }
};

/** @brief The bar meters of the master output, fed from phos::Engine::meter(). */
class LoudnessDisplay final : public juce::Component {
public:
    /** @brief Draws the reading it was last given. */
    void paint(juce::Graphics&) override;
    /** @brief New numbers from the audio thread's meter (message thread). */
    void update(const phos::LoudnessReading& r, float compDb, float limitDb, float target);

private:
    phos::LoudnessReading m_reading;   ///< what the engine's meter last said
    float m_comp = 0.0f;               ///< gain reduction of the bus compressor, dB
    float m_limit = 0.0f;              ///< gain reduction of the limiter, dB
    float m_target = -8.0f;            ///< master.target_lufs, drawn as the mark to hit
};

/** @brief The set as a list of tracks: key, tempo, patterns and where the play head is. */
class TrackDisplay final : public juce::Component {
public:
    /** @brief One line of the list. */
    struct Row {
        int m_bar = 0;   ///< the track's first bar
        int m_bars = 0;   ///< its length, bars
        int m_index = 0;   ///< its index
        double m_bpm = 145.0;   ///< its tempo
        juce::String m_text;   ///< the line as drawn
    };
    /** @brief The lines, the play head marker on the track that plays. */
    void paint(juce::Graphics&) override;
    /** @brief Nothing to lay out. */
    void resized() override {}
    /** @brief A click jumps to the track clicked (onJump). */
    void mouseDown(const juce::MouseEvent&) override;
    /** @brief Replaces the list (message thread). */
    void setRows(std::vector<Row> rows) { m_rows = std::move(rows); repaint(); }
    /** @brief Moves the play head marker. */
    void setPosition(int bar) { if (bar != m_bar) { m_bar = bar; repaint(); } }
    /** @brief Called with the first bar of the track that was clicked. */
    std::function<void(int)> m_onJump;

private:
    std::vector<Row> m_rows;   ///< the tracks the composer has published
    int m_bar = 0;             ///< where the play head stands, in bars from the start of the set
};

/**
 * @brief The notes of one part over the next bars, as a step grid or a small piano roll.
 *
 * Every generator page carries one, so what the knobs are shaping can be seen as well as heard:
 * percussion as twelve lanes, everything else as pitches over time. The notes come from the
 * conductor's copy of the bars it last composed, which is the score the engine is about to play --
 * so the roll runs a little ahead of the sound, by exactly the horizon the rings are kept at.
 */
class PatternDisplay final : public juce::Component {
public:
    /** @brief Which part to draw; percussion draws its twelve lanes instead of pitches. */
    void setPart(phos::PartE part, int highlightLane = -1) { m_part = part; m_lane = highlightLane; }
    /** @brief New notes and where the play head stands, in bars and beats (message thread). */
    void update(std::vector<phos::NoteEvent> notes, int firstBar, int bars, double beat);
    /** @brief The notes of the window as a piano roll (the lanes for the percussion), the play head. */
    void paint(juce::Graphics&) override;

private:
    std::vector<phos::NoteEvent> m_notes;    ///< the window's notes, in musical beats
    phos::PartE m_part = phos::PartE::KICK;    ///< which part is drawn
    int m_lane = -1;                         ///< percussion: the lane whose knobs are on screen
    int m_firstBar = 0;                      ///< first bar of the window
    int m_bars = 4;                          ///< how many bars it shows
    double m_beat = 0.0;                     ///< where the engine stands, for the play head
};

/**
 * @brief The gallery's list (23.09.2026): one row per saved set, its name, seed, tracks and verdicts, and a strip
 *        that draws the whole set as its sections -- intro, groove, build, drop, breakdown, climax, outro -- in the
 *        part colours. A click loads the set.
 */
class GalleryDisplay final : public juce::Component {
public:
    /** @brief One saved set. */
    struct Row {
        juce::File m_file;              ///< the .phosset
        phos::GalleryEntry m_entry;     ///< what its gallery lines say
        phos::RatingCount m_rating;     ///< the listener's verdicts for its seed
    };
    static constexpr int RowH = 60;  ///< pixels per row
    /** @brief Replaces the list and sizes the component to it. */
    void setRows(std::vector<Row> rows);
    /** @brief The rows: name, seed, tracks, verdicts and the strip of sections. */
    void paint(juce::Graphics&) override;
    /** @brief A click opens the set clicked (onOpen). */
    void mouseDown(const juce::MouseEvent&) override;
    /** @brief Follows the row under the mouse. */
    void mouseMove(const juce::MouseEvent&) override;
    /** @brief No row under the mouse. */
    void mouseExit(const juce::MouseEvent&) override;
    /** @brief Called with the file of the row that was clicked. */
    std::function<void(const juce::File&)> m_onOpen;
    /** @brief How many sets the list has. */
    int rowCount() const { return static_cast<int>(m_rows.size()); }

private:
    std::vector<Row> m_rows;   ///< newest first
    int m_hover = -1;          ///< the row under the mouse
};

/**
 * @brief The signal flow as a picture (26.09.2026; EditorFlow.cpp): the help page's topic "Signal flow" and the
 *        manual's figure (flow.png). A fixed canvas of CanvasW x CanvasH, scaled to fit.
 */
class SignalFlow final : public juce::Component {
public:
    SignalFlow() { setInterceptsMouseClicks(false, false); }
    static constexpr float CanvasW = 1000.0f;   ///< the canvas's width in drawing units
    static constexpr float CanvasH = 486.0f;    ///< its height
    /** @brief Every unit a box in its family's colour, the buses as arrows. */
    void paint(juce::Graphics&) override;
    /** @brief The part of the component the drawing covers (the canvas keeps its proportions). */
    juce::Rectangle<int> drawn() const;
};

/**
 * @brief The help page (23.09.2026): the manual's prose by topic, the parameters of the tab it was opened from,
 *        and the update check (EditorHelp.cpp).
 *
 * The prose is Tools/manual/chapters.txt compiled in, the same text the PDF prints -- one source, so the help
 * and the manual cannot say different things. F1 opens it at the chapter of the tab that is open; Esc, F1 or
 * Help closes it again.
 */
class HelpView final : public juce::Component, private juce::ListBoxModel {
public:
    /** @brief Builds the topics from the manual's chapters. */
    explicit HelpView(PhospheneProcessor& proc);
    /**
     * @brief Shows the topic @p name (a tab's name or a chapter's), with the parameters of @p paramIds listed
     *        under "Parameters on this tab" (the tab the help was opened from).
     */
    void open(const juce::String& name, const juce::String& tabName, std::vector<int> paramIds);
    /** @brief The topics, in list order. */
    const juce::StringArray& topicNames() const { return m_names; }
    /** @brief The text a topic shows (for the tests and the manual). */
    juce::String topicText(int index) const;
    /** @brief The topic on screen. */
    int selectedTopic() const { return m_list.getSelectedRow(); }
    /** @brief The panel's background. */
    void paint(juce::Graphics&) override;
    /** @brief The topics left, the text right, the update controls under them. */
    void resized() override;
    /** @brief Refreshes the update line (the editor's timer). */
    void refreshUpdate();

private:
    /** @brief How many topics there are. */
    int getNumRows() override { return m_names.size(); }
    /** @brief A topic's name in the list, the chosen one in the accent. */
    void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
    /** @brief Shows the topic chosen. */
    void selectedRowsChanged(int row) override;
    PhospheneProcessor& m_proc;   ///< the processor the help reads (parameters, update state)
    juce::StringArray m_names;           ///< topic names
    juce::StringArray m_texts;           ///< their text (the parameter topic is filled by open())
    int m_paramTopic = -1;               ///< index of "Parameters on this tab"
    int m_updateTopic = -1;              ///< index of "Updates"
    int m_flowTopic = -1;                ///< index of "Signal flow"
    SignalFlow m_flow;                   ///< its picture, above the text
    juce::ListBox m_list;   ///< the topics
    juce::TextEditor m_text;   ///< the chosen topic's text
    juce::ToggleButton m_autoCheck{ "Look for updates once a day" };   ///< the daily update check on or off
    juce::TextButton m_checkNow{ "Check now" };   ///< asks for updates now
    juce::TextButton m_openRelease{ "Open the release page" };   ///< opens the release page when there is one
    juce::Label m_updateLine;   ///< what the last check found
    juce::SharedResourcePointer<phosui::UpdateCheck> m_updates;   ///< the process's one update check
};

/** @brief The Phosphene editor. */
class PhospheneEditor final : public juce::AudioProcessorEditor, private juce::Timer, private juce::ChangeListener {
public:
    /** @brief Builds every page from the parameter tables and sizes the window. */
    explicit PhospheneEditor(PhospheneProcessor&);
    ~PhospheneEditor() override;

    /** @brief The background behind the scaled body. */
    void paint(juce::Graphics&) override;
    void resized() override;   ///< scales the body to the window

    /** @brief Names of the tabs, in order. */
    static const juce::StringArray& tabNames();
    /** @brief Names of the groups of tabs, the row on top (01.10.2026), in order. */
    static const juce::StringArray& groupNames();
    /** @brief The group tab @p tab belongs to. */
    static int groupOf(int tab);
    /** @brief The tabs of group @p g, in order. */
    static const std::vector<int>& tabsOf(int g);
    /** @brief Opens a tab. */
    void setTab(int index);
    /** @brief The tab that is open. */
    int tab() const { return m_tab; }
    /** @brief Shows one of the twelve percussion lanes. */
    void setPercLane(int lane);
    /**
     * @brief Renders the editor at design size into a PNG (never a screen grab).
     * @return false if the file cannot be written
     */
    bool writeScreenshot(const juce::File& file);
    /**
     * @brief Writes everything the manual generator needs into @p dir (`PHOS_MANUAL`).
     *
     * One picture per tab, and a `manual.json` holding the parameter tables and -- which is the
     * point -- the groups each tab really built, straight out of the pages. Tools/manual/make_manual.py
     * turns that into the manual and refuses to print one in which a parameter appears on no tab.
     * That makes a parameter that no group claims (it happened on 23.09.2026 with four of them)
     * impossible to miss.
     * @return false if the folder cannot be written
     */
    bool writeManual(const juce::File& dir);
    /**
     * @brief The open tab in full (26.09.2026, for the manual): the header and the whole page, however tall -- a page
     *        taller than the window is cut by the viewport in writeScreenshot().
     */
    bool writeFullPage(const juce::File& file);
    /** @brief The keys of every parameter that stands in a group on some page, percussion lanes included. */
    juce::StringArray parametersOnPages() const;
    /** @brief The help page (tests): null until the editor has built it. */
    HelpView* helpView() const { return m_help.get(); }
    /** @brief Opens or closes the help page (the Help button, F1). */
    void setHelpVisible(bool show) { showHelp(show); }
    /** @brief The mixer page's console (tests). */
    phosui::MixerConsole* mixerConsole() const { return m_mixer; }
    /** @brief The kick's and the bass's scopes (tests). */
    phosui::SynthScope* kickScope() const { return m_kickScope; }
    phosui::SynthScope* bassScope() const { return m_bassScope; }   ///< @copydoc kickScope

private:
    /** @brief Builds the synth, effect and mixer pages (the others have their own files). */
    void buildPages();
    void buildSetPage();   ///< EditorSetTab.cpp
    void refreshSetPage();   ///< EditorSetTab.cpp: meters, transport, track list
    void buildArrangePage();   ///< EditorArrange.cpp
    void refreshArrangePage();   ///< EditorArrange.cpp: the timeline and the play head
    void buildPerformPage();   ///< EditorPerform.cpp
    void refreshPerformPage();   ///< EditorPerform.cpp: what the macros are doing
    void buildGalleryPage();   ///< EditorGallery.cpp
    void refreshGalleryPage();   ///< EditorGallery.cpp: rescans the folder
    void buildExportPage();   ///< EditorSetTab.cpp (01.10.2026)
    void refreshExportPage();   ///< EditorSetTab.cpp: the recording's state
    /** @brief The set's plan as the arrange views draw it, copied out of the published plans (EditorArrange.cpp). */
    ArrangeDisplay::Snapshot planSnapshot() const;
    void refreshOverview();   ///< EditorArrange.cpp: the strip under the header
    void refreshHeader();   ///< the header's transport, status and tools
    void showSettings();   ///< the frame's settings menu
    void changeListenerCallback(juce::ChangeBroadcaster*) override;   ///< the settings changed (the backdrop)
    bool standalone() const;      ///< the editor sits in the standalone's window
    bool fullScreen() const;      ///< and that window fills the screen
    void refreshPattern();   ///< the pattern roll of the tab that is open
    /**
     * @brief The "Sound" group at the head of a synth's page (23.09.2026): the synth's presets in a chooser with a
     *        submenu per group, "Save..." for a user preset, and the own-sound switch (mix.*_own).
     * @param owner the synth's own-sound number: 0 kick, 1 bass, 2 acid, 3 + instance the voices
     */
    void addSoundGroup(phosui::ControlPage& page, phos::ModuleE module, int instance, int owner, juce::Colour tint);
    /** @brief One synth's preset chooser and the presets behind its item ids (id = index + 1). */
    struct PresetBox {
        juce::ComboBox* m_box = nullptr;   ///< the chooser (owned by the page)
        phos::ModuleE m_synth = phos::ModuleE::KICK;    ///< the synth it sets (named so: doxygen reads a member `module` as a C++20 module)
        int m_instance = 0;   ///< its instance
        std::vector<phos::SoundPreset> m_presets;   ///< factory and user presets, item id = index + 1
        phosui::ControlPage* m_page = nullptr;   ///< the page it stands on (15.10.2026: the star under the chooser)
        int m_nameCell = -1;   ///< the chooser's cell on that page
        std::vector<std::pair<int, float>> m_loaded;   ///< the knobs the preset chosen put (store id, real value): the star compares
    };
    /** @brief The knobs preset @p preset puts on instance @p instance of synth @p synth (store id, real value; 15.10.2026). */
    std::vector<std::pair<int, float>> presetTargets(phos::ModuleE synth, int instance, const phos::SoundPreset& preset);
    std::unique_ptr<phos::ParamStore> m_scratch;   ///< a store to work out what a preset would set (presetTargets)
    /** @brief The page of a synth's chooser. */
    static int tabOfSynth(phos::ModuleE synth, int instance);
    /** @brief Fills a chooser from the factory presets and the user's, grouped (after a save, again). */
    void fillPresetBox(PresetBox& pb);
    /**
     * @brief The effects page's "Effect Presets" group (24.09.2026): per family of the effect bank a chooser --
     *        Auto, or one of the family's presets in submenus of 32 -- bound to sfx.preset_*, and a button that
     *        auditions it in the running set.
     */
    void addSfxPresetGroup(phosui::ControlPage& page, juce::Colour tint);
    /** @brief The Field tab's preset menu and library line (27.09.2026; phos/FieldPresets.h). */
    void addFieldPresetGroup(phosui::ControlPage& page, juce::Colour tint);
    /** @brief Writes what the field library holds into m_fieldStatus (after a scan or a new folder). */
    void refreshFieldStatus();
    juce::Label* m_fieldStatus = nullptr;                 ///< the Field tab's library line (owned by its page)
    std::unique_ptr<juce::FileChooser> m_fieldChooser;   ///< the folder dialog while it is open
    std::vector<std::unique_ptr<PresetBox>> m_presetBoxes;   ///< one per synth page
    std::unique_ptr<juce::AlertWindow> m_presetNameDialog;   ///< "Save preset" asks for a name
    /** @brief Follows the processor: the status, the play button, the rerolls, the update link; the screenshots. */
    void timerCallback() override;
    /** @brief Ctrl+Z undoes, Ctrl+Y and Ctrl+Shift+Z redo (23.09.2026); F1 help, Esc closes it, F11 full screen. */
    bool keyPressed(const juce::KeyPress& key) override;
    /** @name The frame's header (01.10.2026): undo, redo, help, the settings, the headset's sign, the ratings
     *  @{ */
    frame::IconButton m_undoIcon{ frame::IconButton::IconE::UNDO, "Undo (Ctrl+Z)" }, m_redoIcon{ frame::IconButton::IconE::REDO, "Redo (Ctrl+Y)" };
    frame::IconButton m_helpIcon{ frame::IconButton::IconE::HELP, "Help (F1)" }, m_settingsIcon{ frame::IconButton::IconE::SETTINGS, "Settings" };
    frame::IconButton m_headsetIcon{ frame::IconButton::IconE::HEADSET, "A headset sends its hands: the Perform tab shows them" };
    frame::IconButton m_likeIcon{ frame::IconButton::IconE::THUMB_UP, "Good here: rates the bar that plays (ratings.tsv)" };
    frame::IconButton m_dislikeIcon{ frame::IconButton::IconE::THUMB_DOWN, "Bad here: rates the bar that plays (ratings.tsv)" };
    juce::Component m_titleSpot;                       ///< where the name is drawn
    juce::Rectangle<float> m_logo;                     ///< where the logo is drawn
    juce::ComboBox m_style, m_key, m_scale;              ///< where the set starts
    std::vector<std::unique_ptr<juce::ComboBoxParameterAttachment>> m_headerLinks;
    juce::Slider m_length;                             ///< compose.set_minutes: the time the arc spans
    std::unique_ptr<juce::SliderParameterAttachment> m_lengthLink;
    juce::Label m_lengthLabel;
    juce::TextButton m_compose{ "Compose set" };   ///< plans the set again
    frame::IconButton m_newSeed{ frame::IconButton::IconE::DICE, "New seed" };   ///< a new seed: a die
    frame::IconButton m_play{ frame::IconButton::IconE::PLAY, "Play (Space)" };   ///< play and stop: a triangle, a square
    frame::IconButton m_mute{ frame::IconButton::IconE::SPEAKER, "Silence the output" };   ///< mutes the output: a speaker, crossed out when muted
    juce::Label m_status, m_where;                      ///< seed, clock and state; the track and the bar
    ArrangeDisplay m_overview;                         ///< the set strip under the header
    uint64_t m_overviewHash = 0;                       ///< what it draws
    unsigned m_overviewTicks = 0;
    juce::OwnedArray<frame::FlatTab> m_groupTabs;      ///< the row of tabs: one per group
    juce::OwnedArray<juce::TextButton> m_subButtons;   ///< the pages of every group of several, shown for the group that is open
    std::vector<int> m_subTab;                         ///< per sub button: its tab
    std::vector<int> m_lastTabOfGroup;                 ///< the page of each group last open
    juce::Rectangle<int> m_tabRow;                     ///< where the row of tabs is (its hairline)
    frame::Backdrop m_backdrop;                        ///< the phosphenes behind the panel
    int m_headerBottom = 0;
    int m_headsetGroup = -1;                           ///< the Perform page's headset group
    bool m_shotHands = false;                          ///< PHOS_SHOT_HEADSET: the headset's hands kept alive
    /** @} */
    /** @name Help, full screen and the update notice (23.09.2026)
     *  @{ */
    juce::TextButton m_updateButton{ "" };
    std::unique_ptr<HelpView> m_help;   ///< the help page, made on first use
    juce::SharedResourcePointer<phosui::UpdateCheck> m_updates;   ///< the process's one update check
    /** @brief Opens or closes the help page, at the chapter of the tab that is open. */
    void showHelp(bool show);
    bool helpShown() const { return m_help != nullptr && m_help->isVisible(); }
    /** @brief The standalone's window fills the screen, or stops filling it (a host owns its own window). */
    void toggleFullScreen();
    /** @brief The standalone's title bar gets a maximise button (as Noctuary's). */
    void parentHierarchyChanged() override;
    /** @} */
    /** @brief The control page on screen, null for a page without knobs. */
    phosui::ControlPage* activePage() const;
    /** @brief Places the header, the tab bar and the page for the current size. */
    void layoutContent();
    /** @brief Paints the header and the page's background. */
    void paintContent(juce::Graphics&);
    /** @brief PHOS_SHOT, PHOS_SHOT_ALL, PHOS_MANUAL: plays, waits, writes the pictures (and the manual) and quits. */
    void runScreenshotMode();

    PhospheneProcessor& m_proc;   ///< the processor this editor shows
    PhospheneLookAndFeel m_lnf;   ///< the editor's look
    EditorContent m_content;   ///< the scaled content: header, tabs and page
    juce::TooltipWindow m_tooltips{ this, 700 };   ///< tooltips after 700 ms

    std::vector<std::unique_ptr<phosui::ControlPage>> m_pages;   ///< one per tab; the percussion tab's is a stand-in
    std::vector<std::unique_ptr<phosui::ControlPage>> m_percPages;   ///< one page per percussion lane
    /** @brief The pattern preview of each tab (null for the Set tab, which has the plan instead). */
    std::vector<PatternDisplay*> m_patterns;
    std::vector<PatternDisplay*> m_percPatterns;   ///< one per lane page, each lighting its own lane
    phosui::MixerConsole* m_mixer = nullptr;       ///< on the Mix page (24.09.2026)
    phosui::SynthScope* m_kickScope = nullptr;     ///< on the Kick page (24.09.2026)
    phosui::SynthScope* m_bassScope = nullptr;     ///< on the Bass page (24.09.2026)
    /** @brief The effect preset choosers' host links; declared after m_pages, so they go before their boxes do. */
    std::vector<std::unique_ptr<juce::ComboBoxParameterAttachment>> m_sfxPresetLinks;
    std::vector<phos::NoteEvent> m_patternNotes;   ///< scratch for the timer's read
    juce::OwnedArray<juce::TextButton> m_laneButtons;   ///< the percussion lane bar
    juce::Viewport m_viewport;   ///< scrolls a page taller than the window
    int m_tab = 0;   ///< the tab on screen
    int m_percLane = 0;   ///< the percussion lane on screen
    int m_designW = 1280;   ///< the design width the content is laid out at before scaling
    int m_designH = 860;   ///< ... and the design height
    bool m_shooting = false;   ///< photographing itself (PHOS_SHOT, PHOS_SHOT_ALL, PHOS_MANUAL): the pictures leave out the mute note

    // ---- the Set tab's own controls (owned by the Set page, referenced here)
    juce::Label*      m_seedLabel = nullptr;   ///< the seed's caption
    juce::TextEditor* m_seedEditor = nullptr;   ///< the seed field
    juce::TextButton* m_playButton = nullptr;   ///< Play
    juce::TextButton* m_stopButton = nullptr;   ///< Stop
    frame::IconButton* m_recordButton = nullptr;   ///< Record (a red disc, lit while it records)
    juce::TextButton* m_followButton = nullptr;   ///< Follow host
    juce::TextButton* m_muteButton = nullptr;   ///< Mute
    juce::Label*      m_statusLabel = nullptr;   ///< clock, bar, beat and tempo
    LoudnessDisplay*  m_loudness = nullptr;   ///< the output meter
    TrackDisplay*     m_tracks = nullptr;   ///< the track list
    juce::Slider*     m_exportBars = nullptr;   ///< bars for the MIDI export
    juce::TextButton* m_exportMidi = nullptr;   ///< the Export tab's buttons (Ctrl+E, Ctrl+S, Ctrl+O)
    juce::Button* m_exportSet = nullptr;        ///< @copydoc m_exportMidi
    juce::Button* m_loadSet = nullptr;          ///< @copydoc m_exportMidi
    /** @name The factory-defaults group of the Set tab (20.09.2026, round "dialogue")
     *  @{ */
    juce::TextButton* m_legacyButton = nullptr;   ///< "Load the saved knobs anyway", shown only while one is held
    juce::Label*      m_legacyNote = nullptr;     ///< why the session came up on the defaults
    /** @} */
    std::unique_ptr<juce::FileChooser> m_chooser;   ///< the file dialog while it is open
    /** @brief The track list as the composer has published it so far. */
    std::vector<TrackDisplay::Row> m_trackRows;
    size_t m_rowsSeed = 0;   ///< how many rows the display was last given, so it repaints only when it grows

    // ---- the Arrange tab
    ArrangeDisplay* m_arrange = nullptr;      ///< the timeline
    juce::Label*    m_arrangeNote = nullptr;  ///< where the play head is, in words
    bool m_arrangeDirty = true;               ///< a lock or a reroll: read the plans again
    unsigned m_arrangeTicks = 0;              ///< the timeline reads the plans once a second, the head every tick
    uint64_t m_arrangeHash = 0;               ///< what the timeline is drawing, so an unchanged plan is not redrawn

    // ---- the Perform tab
    juce::Slider*     m_macroSlider[NumMacros] = {};   ///< the macros that are held at a value
    juce::TextButton* m_macroButton[NumMacros] = {};   ///< the macros that are pressed
    juce::Label*      m_macroNote = nullptr;            ///< what they are doing now
    /** @name Rating what is playing (23.09.2026; phos/Rating.h)
     *  @{ */
    juce::Label*      m_midiNote = nullptr;             ///< MIDI learn: what is armed, what is bound (23.09.2026)
    // ---- the Gallery tab (23.09.2026)
    GalleryDisplay*   m_gallery = nullptr;              ///< the list
    juce::Viewport*   m_galleryView = nullptr;          ///< scrolls it
    juce::Label*      m_galleryNote = nullptr;          ///< where the sets are, what was saved
    std::unique_ptr<juce::AlertWindow> m_galleryAsk;    ///< the name dialog of "Save to gallery..."
    uint32_t          m_midiShown = 0xFFFFFFFFu;        ///< the map's revision the note shows
    int               m_midiArmedShown = -2;            ///< the armed target the note shows
    juce::TextEditor* m_ratingNote = nullptr;           ///< the note that goes with the next verdict
    juce::Label*      m_ratingLast = nullptr;           ///< what the last press wrote, and where
    juce::Label*      m_prefsNote = nullptr;            ///< the learned preferences in force (23.09.2026)
    juce::TextButton* m_ratingButton[2] = {};           ///< good, bad
    /** @brief Writes a verdict for the bar that is playing (+1 good, -1 bad) into the ratings file. */
    void rateNow(int verdict);
    /** @} */

    /** @name The family's ways around the panel (15.10.2026, Frame.h, from Empyrean)
     *  @{ */
    /** @brief The command palette's button (Ctrl+K). */
    frame::IconButton m_search{ frame::IconButton::IconE::SEARCH, "Search (Ctrl+K): any control, preset or action by a few letters" };
    /** @brief Held: the synths play their presets as they were chosen; let go, your sound again. */
    frame::IconButton m_compare{ frame::IconButton::IconE::COMPARE, "Hold to hear the presets as they were chosen; let go for your sound" };
    std::unique_ptr<frame::InfoLine> m_info;            ///< the info line at the bottom: what the control under the mouse is
    std::unique_ptr<frame::CommandPalette> m_palette;   ///< the command palette, over everything while open
    std::unique_ptr<frame::PresetBrowser> m_browser;    ///< the preset browser, over everything while open
    std::unique_ptr<frame::Tour> m_tour;                ///< the first steps
    frame::WindowKeeper m_window;                       ///< the standalone's window back where it was left
    std::unique_ptr<frame::ModulationAtKnob> m_modulation;   ///< the voices' matrix rows on their knobs (every page's)
    int m_tourTicks = 0;                                ///< ticks until the first start's tour (-1: considered)
    bool m_comparing = false;                           ///< A|B is held
    void openPalette();                                ///< the command palette (Ctrl+K)
    std::vector<frame::CommandPalette::Command> paletteCommands();   ///< what the palette can do: actions, pages, controls, presets
    void startTour();                                  ///< the first steps, from the first
    /** @brief The info line's words for store id @p id: its name, its value, its range, what moves it, its key. */
    bool describeControl(int id, juce::String& name, juce::String& text);
    /** @brief Shows the control of store id @p id (its tab, lane, section) and lights it; false when no page has it. */
    bool revealId(int id);
    /** @brief Opens the preset browser on a synth's chooser. */
    void browsePresets(PresetBox& pb);
    /** @brief A|B pressed (the presets as chosen) or let go (the sound as it is). */
    void compare(bool on);
    /** @} */

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhospheneEditor)
};
