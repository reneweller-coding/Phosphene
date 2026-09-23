/**
 * @file PluginEditor.h
 * @brief The editor: a header, one tab per generator, and pages built from the parameter tables.
 *
 * The tabs follow PLAN 8.1: Set, Arrange, Kick, Bass, Percussion (twelve lanes behind a lane bar),
 * Acid, Lead, Counter, Arp, Stab, Pad, Drone, SFX + FX, Mixer + Master, Perform. None of the pages knows a coordinate; each is a
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
#include "PhospheneLookAndFeel.h"
#include "PluginProcessor.h"
#include <memory>
#include <vector>

/**
 * @brief The tabs, by name rather than by number.
 *
 * They were plain indices until the Arrange and Perform tabs arrived in the middle of the row and
 * every `tab_ == 3` in the file meant something else.
 */
enum Tab : int {
    TabSet = 0, TabArrange, TabKick, TabBass, TabPerc, TabAcid,
    // 19.09.2026: the polyphonic voices in their groups (Params.h, PolyInstance), each beside its partner.
    TabLead, TabCounter, TabArp, TabStab, TabPad, TabDrone,
    TabFx, TabMix, TabPerform,
    TabGallery,   ///< 23.09.2026: saved sets with their form, loaded with a click (EditorGallery.cpp)
    TabCount
};

/**
 * @brief The editor's fixed-size body.
 *
 * Everything is laid out once in a design space of PhospheneEditor::designW_ by designH_ and the
 * whole body is then scaled to the window. Nothing reflows when the window is dragged -- it only
 * gets bigger or smaller -- which is what makes a picture taken at design size the truth about the
 * layout, and what keeps the measurement in one place.
 */
class EditorContent final : public juce::Component {
public:
    std::function<void(juce::Graphics&)> onPaint;   ///< the editor draws the frame
    std::function<void()> onResized;                ///< the editor places its children
    void paint(juce::Graphics& g) override { if (onPaint) onPaint(g); }
    void resized() override { if (onResized) onResized(); }
};

/** @brief The bar meters of the master output, fed from phos::Engine::meter(). */
class LoudnessDisplay final : public juce::Component {
public:
    /** @brief Draws the reading it was last given. */
    void paint(juce::Graphics&) override;
    /** @brief New numbers from the audio thread's meter (message thread). */
    void update(const phos::LoudnessReading& r, float compDb, float limitDb, float target);

private:
    phos::LoudnessReading reading_;   ///< what the engine's meter last said
    float comp_ = 0.0f;               ///< gain reduction of the bus compressor, dB
    float limit_ = 0.0f;              ///< gain reduction of the limiter, dB
    float target_ = -8.0f;            ///< master.target_lufs, drawn as the mark to hit
};

/** @brief The set as a list of tracks: key, tempo, patterns and where the play head is. */
class TrackDisplay final : public juce::Component {
public:
    /** @brief One line of the list. */
    struct Row {
        int bar = 0, bars = 0, index = 0;
        double bpm = 145.0;
        juce::String text;
    };
    void paint(juce::Graphics&) override;
    void resized() override {}
    void mouseDown(const juce::MouseEvent&) override;
    /** @brief Replaces the list (message thread). */
    void setRows(std::vector<Row> rows) { rows_ = std::move(rows); repaint(); }
    /** @brief Moves the play head marker. */
    void setPosition(int bar) { if (bar != bar_) { bar_ = bar; repaint(); } }
    /** @brief Called with the first bar of the track that was clicked. */
    std::function<void(int)> onJump;

private:
    std::vector<Row> rows_;   ///< the tracks the composer has published
    int bar_ = 0;             ///< where the play head stands, in bars from the start of the set
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
    void setPart(phos::Part part, int highlightLane = -1) { part_ = part; lane_ = highlightLane; }
    /** @brief New notes and where the play head stands, in bars and beats (message thread). */
    void update(std::vector<phos::NoteEvent> notes, int firstBar, int bars, double beat);
    void paint(juce::Graphics&) override;

private:
    std::vector<phos::NoteEvent> notes_;    ///< the window's notes, in musical beats
    phos::Part part_ = phos::Part::Kick;    ///< which part is drawn
    int lane_ = -1;                         ///< percussion: the lane whose knobs are on screen
    int firstBar_ = 0;                      ///< first bar of the window
    int bars_ = 4;                          ///< how many bars it shows
    double beat_ = 0.0;                     ///< where the engine stands, for the play head
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
        juce::File file;              ///< the .phosset
        phos::GalleryEntry entry;     ///< what its gallery lines say
        phos::RatingCount rating;     ///< the listener's verdicts for its seed
    };
    static constexpr int kRowH = 60;  ///< pixels per row
    /** @brief Replaces the list and sizes the component to it. */
    void setRows(std::vector<Row> rows);
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    /** @brief Called with the file of the row that was clicked. */
    std::function<void(const juce::File&)> onOpen;
    int rowCount() const { return static_cast<int>(rows_.size()); }

private:
    std::vector<Row> rows_;   ///< newest first
    int hover_ = -1;          ///< the row under the mouse
};

/** @brief The Phosphene editor. */
class PhospheneEditor final : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit PhospheneEditor(PhospheneProcessor&);
    ~PhospheneEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    /** @brief Names of the tabs, in order. */
    static const juce::StringArray& tabNames();
    /** @brief Opens a tab. */
    void setTab(int index);
    /** @brief The tab that is open. */
    int tab() const { return tab_; }
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
     * That is the bug of the last round (four new parameters that no group claimed) made impossible
     * to miss.
     * @return false if the folder cannot be written
     */
    bool writeManual(const juce::File& dir);

private:
    void buildPages();
    void buildSetPage();          // EditorSetTab.cpp
    void refreshSetPage();        // EditorSetTab.cpp: meters, transport, track list
    void buildArrangePage();      // EditorArrange.cpp
    void refreshArrangePage();    // EditorArrange.cpp: the timeline and the play head
    void buildPerformPage();      // EditorPerform.cpp
    void refreshPerformPage();    // EditorPerform.cpp: what the macros are doing
    void buildGalleryPage();      // EditorGallery.cpp
    void refreshGalleryPage();    // EditorGallery.cpp: rescans the folder
    void refreshPattern();        // the pattern roll of the tab that is open
    void timerCallback() override;
    /** @brief Ctrl+Z undoes, Ctrl+Y and Ctrl+Shift+Z redo (23.09.2026). */
    bool keyPressed(const juce::KeyPress& key) override;
    juce::TextButton undoButton_{ "Undo" }, redoButton_{ "Redo" };   ///< in the header (23.09.2026)
    phosui::ControlPage* activePage() const;
    void layoutContent();
    void paintContent(juce::Graphics&);
    void runScreenshotMode();

    PhospheneProcessor& proc_;
    PhospheneLookAndFeel lnf_;
    EditorContent content_;
    juce::TooltipWindow tooltips_{ this, 700 };

    std::vector<std::unique_ptr<phosui::ControlPage>> pages_;   ///< one per tab; the percussion tab's is a stand-in
    std::vector<std::unique_ptr<phosui::ControlPage>> percPages_;
    /** @brief The pattern preview of each tab (null for the Set tab, which has the plan instead). */
    std::vector<PatternDisplay*> patterns_;
    std::vector<PatternDisplay*> percPatterns_;   ///< one per lane page, each lighting its own lane
    std::vector<phos::NoteEvent> patternNotes_;   ///< scratch for the timer's read
    juce::OwnedArray<juce::TextButton> tabButtons_, laneButtons_;
    juce::Viewport viewport_;
    int tab_ = 0, percLane_ = 0;
    int designW_ = 1290, designH_ = 860;

    // ---- the Set tab's own controls (owned by the Set page, referenced here)
    juce::Label*      seedLabel_ = nullptr;
    juce::TextEditor* seedEditor_ = nullptr;
    juce::TextButton* playButton_ = nullptr;
    juce::TextButton* stopButton_ = nullptr;
    juce::TextButton* recordButton_ = nullptr;
    juce::TextButton* followButton_ = nullptr;
    juce::TextButton* muteButton_ = nullptr;
    juce::Label*      statusLabel_ = nullptr;
    LoudnessDisplay*  loudness_ = nullptr;
    TrackDisplay*     tracks_ = nullptr;
    juce::Slider*     exportBars_ = nullptr;
    /** @name The factory-defaults group of the Set tab (20.09.2026, round "dialogue") @{ */
    juce::TextButton* legacyButton_ = nullptr;   ///< "Load the saved knobs anyway", shown only while one is held
    juce::Label*      legacyNote_ = nullptr;     ///< why the session came up on the defaults
    /** @} */
    std::unique_ptr<juce::FileChooser> chooser_;
    /** @brief The track list as the composer has published it so far. */
    std::vector<TrackDisplay::Row> trackRows_;
    size_t rowsSeed_ = 0;   ///< how many rows the display was last given, so it repaints only when it grows

    // ---- the Arrange tab
    ArrangeDisplay* arrange_ = nullptr;      ///< the timeline
    juce::Label*    arrangeNote_ = nullptr;  ///< where the play head is, in words
    bool arrangeDirty_ = true;               ///< a lock or a reroll: read the plans again
    unsigned arrangeTicks_ = 0;              ///< the timeline reads the plans once a second, the head every tick
    uint64_t arrangeHash_ = 0;               ///< what the timeline is drawing, so an unchanged plan is not redrawn

    // ---- the Perform tab
    juce::Slider*     macroSlider_[kNumMacros] = {};   ///< the macros that are held at a value
    juce::TextButton* macroButton_[kNumMacros] = {};   ///< the macros that are pressed
    juce::Label*      macroNote_ = nullptr;            ///< what they are doing now
    /** @name Rating what is playing (23.09.2026; phos/Rating.h)
     *  @{ */
    juce::Label*      midiNote_ = nullptr;             ///< MIDI learn: what is armed, what is bound (23.09.2026)
    // ---- the Gallery tab (23.09.2026)
    GalleryDisplay*   gallery_ = nullptr;              ///< the list
    juce::Viewport*   galleryView_ = nullptr;          ///< scrolls it
    juce::Label*      galleryNote_ = nullptr;          ///< where the sets are, what was saved
    std::unique_ptr<juce::AlertWindow> galleryAsk_;    ///< the name dialog of "Save to gallery..."
    uint32_t          midiShown_ = 0xFFFFFFFFu;        ///< the map's revision the note shows
    int               midiArmedShown_ = -2;            ///< the armed target the note shows
    juce::TextEditor* ratingNote_ = nullptr;           ///< the note that goes with the next verdict
    juce::Label*      ratingLast_ = nullptr;           ///< what the last press wrote, and where
    juce::Label*      prefsNote_ = nullptr;            ///< the learned preferences in force (23.09.2026)
    juce::TextButton* ratingButton_[2] = {};           ///< good, bad
    /** @brief Writes a verdict for the bar that is playing (+1 good, -1 bad) into the ratings file. */
    void rateNow(int verdict);
    /** @} */

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhospheneEditor)
};
