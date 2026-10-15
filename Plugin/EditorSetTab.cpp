/**
 * @file EditorSetTab.cpp
 * @brief The Set tab -- seed, clock, the composer's knobs, loudness, the track list -- and the Export tab (01.10.2026):
 *        the recording, the MIDI, the set's file and the score cues.
 *
 * Everything here is what the composer's knobs alone cannot say: which set is playing (the seed),
 * whether it is playing at all, how loud it is, what the plan looks like, and how to get the plan
 * out as MIDI or as text. The knobs themselves are the ordinary generated groups -- the Set tab is
 * the only page that adds controls of its own, and it adds them through the same layout engine, in
 * cell units, rather than by placing them.
 */
#include "PluginEditor.h"
#include "phos/Composer.h"
#include <cmath>

using namespace phos;
using namespace phosui;

namespace {
/** @brief A button that is really a switch (Follow host, Mute). */
std::unique_ptr<juce::TextButton> makeToggle(const juce::String& caption)
{
    auto b = std::make_unique<juce::TextButton>(caption);
    b->setClickingTogglesState(true);
    return b;
}

/**
 * @brief One line about one part's pitch source: what is really loaded, not what the knob asks for.
 *
 * @param part      "Melody" or "Bass", as the two knobs above are named
 * @param fallback  what the part plays without a weight file ("Markov", "Pattern")
 * @param learned   whether the model really loaded in this process
 * @param nll       the held-out nats per token the file declares, for a loaded model
 * @param source    true when it loaded from somewhere this build was not installed into
 */
juce::String modelLine(const char* part, const char* fallback, bool learned, double nll, bool source)
{
    juce::String s;
    s << part << ":  ";
    if (!learned) return s + juce::String("not installed -- ") + fallback;
    s << "learned, " << juce::String(nll, 2) << " nats";
    if (source) s << "  (from the build tree, not from an installed copy)";
    return s;
}

/** @brief The melodic parts a track has, by name, in the order of the voices' groups (Form.h, MelodyPart). */
juce::String melodicParts(const TrackPlan& plan)
{
    static const char* const Names[MelodyParts] = { "acid", "lead", "counter", "arp", "stab", "pad", "drone" };
    juce::String s;
    for (int k = 0; k < MelodyParts; ++k) if (plan.m_melody.m_present[k]) s << "  " << Names[k];
    return s;
}
} // namespace

void PhospheneEditor::buildSetPage()
{
    auto page = std::make_unique<ControlPage>();
    const juce::Colour tint = partColour(0);

    // ---------------------------------------------------------------- seed and clock
    // Play, stop and mute are in the header since 01.10.2026 (the frame's), the recording and the files on the Export
    // tab, as every generator has them. Here what only this page says: the seed to type, the host's clock, and where the
    // transport stands.
    const int gt = page->addGroup("Seed and Clock", tint, 12);
    {
        auto seed = std::make_unique<juce::TextEditor>();
        seed->setText(juce::String(m_proc.seed()), false);
        seed->setJustification(juce::Justification::centred);
        seed->onReturnKey = [this] {
            const juce::int64 v = m_seedEditor->getText().getLargeIntValue();
            m_proc.undoable("Seed", [this, v] { m_proc.setSeed(static_cast<uint64_t>(juce::jmax<juce::int64>(1, v))); });
        };
        seed->onFocusLost = [this] { m_seedEditor->setText(juce::String(m_proc.seed()), false); };
        m_seedEditor = seed.get();
        page->addControl(gt, std::move(seed), "Seed", 2);

        auto dice = std::make_unique<juce::TextButton>("Randomize seed");
        dice->onClick = [this] {
            m_proc.undoable("Randomize seed", [this] { m_proc.randomiseSeed(); });
            m_seedEditor->setText(juce::String(m_proc.seed()), false);
            m_trackRows.clear();
            m_rowsSeed = 0;
        };
        page->addControl(gt, std::move(dice), "", 3, true);

        auto follow = makeToggle("Follow host");
        follow->setToggleState(m_proc.followsHost(), juce::dontSendNotification);
        follow->onClick = [this] { m_proc.setFollowHost(m_followButton->getToggleState()); };
        m_followButton = follow.get();
        page->addControl(gt, std::move(follow), "", 2, true);

        auto status = std::make_unique<juce::Label>(juce::String(), "stopped");
        status->setJustificationType(juce::Justification::centredLeft);
        status->setColour(juce::Label::textColourId, Dim);
        m_statusLabel = status.get();
        page->addControl(gt, std::move(status), "", 5, true);
    }

    // ---------------------------------------------------------------- the composer's knobs
    // 23.09.2026, at the user's word: grouped by what the knobs decide. Until then the groups were slices of the table
    // in the order the rounds had appended to it, so the key and the scale stood between the tempo and the bass
    // pattern, the style fourteen knobs further down under "Form", and "Style Tempo" -- which replaces the tempo and
    // its range -- in a different group from both. Style, key, scale and tempo are where the set *starts*: the style
    // walks with Style Mix and the key and tempo walk from track to track, which the names now say.
    const int cb = m_proc.params().base(ModuleE::COMPOSE);
    auto ids = [cb](std::initializer_list<int> ks) { std::vector<int> v; for (int k : ks) v.push_back(cb + k); return v; };
    using namespace compose;
    const std::vector<std::pair<const char*, std::vector<int>>> groups = {
        { "Start & Style", ids({ STYLE, STYLE_MIX, STYLE_TEMPO, BPM, TEMPO_RANGE, KEY, SCALE }) },
        { "Set Journey",   ids({ compose::ARC, SET_MINUTES, TRACK_BARS, TRACK_VARIATION, SOUND_VARIATION, SOUND_PRESETS }) },
        { "Kick & Bass",   ids({ KICK_PATTERN, BASS_PATTERN, BASS_GATE, BASS_VARIATION, BASS_REGISTER, BASS_FOLLOWS_CHORDS, BASS_MODEL, BASS_RHYTHM }) },
        { "Rhythm",        ids({ PERC_DENSITY, PERC_VARIATION, SWING, GATE_CHANCE }) },
        { "Melody",        ids({ ACID_AMOUNT, ARP_AMOUNT, MELODY_VARIATION, MELODY_TEMPERATURE, SQUELCH_CHANCE, MODAL_INTERCHANGE, MELODY_MODEL, PITCH_ENTROPY }) },
        { "Lead",          ids({ LEAD_AMOUNT, LEAD_DENSITY, compose::COUNTER_MODE }) },
        { "Voices",        ids({ COUNTER_AMOUNT, STAB_AMOUNT, PAD_AMOUNT, DRONE_AMOUNT, SFX_AMOUNT, VOICE_DENSITY, BED_DENSITY, FIELD_DENSITY }) },
        { "Levels",        ids({ LEVEL_MATCH, PRESENCE_MATCH, AUDIBILITY_MATCH }) },
    };
    static const int Columns[] = { 5, 3, 5, 4, 5, 5, 4, 3 };
    size_t claimed = 0;
    for (size_t i = 0; i < groups.size(); ++i) {
        page->addParamsGroup(m_proc, groups[i].first, tint, Columns[i], groups[i].second);
        claimed += groups[i].second.size();
    }
    // Every compose knob in exactly one group: a knob appended to the table must be placed here by hand now.
    jassert(claimed == static_cast<size_t>(compose::Count));
    juce::ignoreUnused(claimed);

    // ---------------------------------------------------------------- what Phase 8 really is here
    // The two knobs above say what is *asked for*; these two lines say what the process *has*. They
    // are wanted because the failure they describe is silent: `melody.phosmdl` or `bass.phosmdl` not
    // installed means the composer draws from the Markov model and the pattern families instead,
    // sounds perfectly healthy, and reports it on stderr -- which a plug-in inside a DAW never shows
    // anyone. The chooser entries are marked "(missing)" as well (EditorLayout.cpp), but a mark
    // inside a closed combo box is only seen by somebody already looking; a line on the page is seen
    // by somebody who is not.
    //
    // Text and not a lamp, because the useful part is the *reason*: which of the two files, and
    // whether the one that loaded came from an installation or from this machine's source tree,
    // which is the case that works here and nowhere else. It is built once and never refreshed:
    // both models are loaded once per process, before the first engine, and cannot change afterwards
    // (PluginProcessor.cpp, installSearchPaths()).
    const PhospheneProcessor::LearnedModels m = m_proc.learnedModels();
    const int gn = page->addGroup("Pitch models", tint, 6);
    {
        struct Line { const char* m_part; const char* m_fallback; bool m_learned; double m_nll; juce::String m_note; };
        const Line lines[2] = { { "Melody", "Markov",  m.m_melody, m.m_melodyNll, m.m_melodyNote },
                                { "Bass",   "Pattern", m.m_bass,   m.m_bassNll,   m.m_bassNote } };
        for (const Line& l : lines) {
            auto lab = std::make_unique<juce::Label>(juce::String(),
                                                     modelLine(l.m_part, l.m_fallback, l.m_learned, l.m_nll, m.m_fromSourceTree));
            lab->setJustificationType(juce::Justification::centredLeft);
            // The palette's own "over" colour, the one the loudness meter uses when a reading is not
            // what it should be: a fallback is not an error, but it is not the product either.
            lab->setColour(juce::Label::textColourId, l.m_learned ? Dim : Red);
            // The whole truth, for anyone who wants it: the directory the plugin looked in, or the
            // core's own message naming every path it tried.
            lab->setTooltip(l.m_learned ? (m.m_directory.isEmpty() ? juce::String("found outside this installation")
                                                              : m.m_directory)
                                      : l.m_note);
            page->addControl(gn, std::move(lab), "", 6, true);
        }
    }

    // ---------------------------------------------------------------- meters, plan, export
    const int gm = page->addGroup("Loudness", tint, 3);
    {
        auto meter = std::make_unique<LoudnessDisplay>();
        m_loudness = meter.get();
        page->addControl(gm, std::move(meter), "", 3, true, 2);
    }
    const int gp = page->addGroup("Plan", tint, 5);
    {
        auto list = std::make_unique<TrackDisplay>();
        list->m_onJump = [this](int bar) { m_proc.seekToBar(bar); };
        m_tracks = list.get();
        page->addControl(gp, std::move(list), "", 7, true, 3);
    }
    // 20.09.2026, round "dialogue". Two things the rule asks for, in one place where they can be seen:
    // a visible way back to the shipped calibration, and -- when a state older than version 3 was
    // loaded -- the offer that replaces silently applying it (PluginProcessor.h, StateVersion).
    const int gd = page->addGroup("Factory defaults", tint, 4);
    {
        auto reset = std::make_unique<juce::TextButton>("Reset to factory defaults");
        reset->setTooltip("Puts every knob back to the value this build ships with. The set itself -- seed, "
                          "locks and rerolls -- is left alone.");
        reset->onClick = [this] {
            m_proc.undoable("Reset to factory defaults", [this] { m_proc.resetToFactoryDefaults(); });
            if (m_legacyNote != nullptr) m_legacyNote->setText(juce::String(), juce::dontSendNotification);
            if (m_legacyButton != nullptr) m_legacyButton->setVisible(false);
        };
        page->addControl(gd, std::move(reset), "", 2, true);

        auto adopt = std::make_unique<juce::TextButton>("Load the saved knobs anyway");
        adopt->setTooltip("Applies the knob values of the older session that was loaded. They were saved "
                          "before this build's calibration and will replace it.");
        adopt->onClick = [this] {
            m_proc.undoable("Load the saved knobs", [this] { m_proc.adoptLegacyState(); });
            if (m_legacyNote != nullptr) m_legacyNote->setText("The older session's knobs are in.", juce::dontSendNotification);
            if (m_legacyButton != nullptr) m_legacyButton->setVisible(false);
        };
        m_legacyButton = adopt.get();
        m_legacyButton->setVisible(false);
        page->addControl(gd, std::move(adopt), "", 2, true);

        auto note = std::make_unique<juce::Label>();
        note->setJustificationType(juce::Justification::centredLeft);
        note->setMinimumHorizontalScale(1.0f);
        m_legacyNote = note.get();
        page->addControl(gd, std::move(note), "", 4, true);
    }
    m_pages[0] = std::move(page);
}

void PhospheneEditor::refreshSetPage()
{
    if (m_loudness == nullptr) return;
    const ParamStore& p = m_proc.params();
    m_loudness->update(m_proc.engine().meter(), m_proc.engine().compReduction(), m_proc.engine().limiterReduction(),
                      p.get(p.base(ModuleE::MASTER) + master::TARGET_LUFS));

    const TransportView t = m_proc.transport();
    if (m_followButton != nullptr) m_followButton->setToggleState(m_proc.followsHost(), juce::dontSendNotification);
    if (m_statusLabel != nullptr) {
        juce::String s;
        if (t.m_restarting) s << "planning the next bars...   ";
        s << (t.m_hostSync ? "host transport" : "own clock") << "   bar " << juce::String(t.m_bar + 1)
          << "   beat " << juce::String(t.m_musicalBeat, 2) << "   " << juce::String(t.m_bpm, 1) << " BPM";
        if (m_proc.isRecording()) s << "   recording " << juce::String(m_proc.recordedSeconds(), 1) << " s";
        m_statusLabel->setText(s, juce::dontSendNotification);
    }
    // The older-state offer (20.09.2026). It appears only while a state of version 1 or 2 is held back,
    // and it names the version, so the user can see why their session came up on the defaults.
    if (m_legacyButton != nullptr && m_legacyNote != nullptr) {
        const bool held = m_proc.pendingLegacyState().isNotEmpty();
        if (held != m_legacyButton->isVisible()) {
            m_legacyButton->setVisible(held);
            m_legacyNote->setText(held ? "This session was saved by an older build (state version "
                                            + juce::String(m_proc.lastStateVersion())
                                            + "). Such a state stores every knob of its own session, including the ones "
                                              "later calibration changed, so it was not applied: you are hearing this "
                                              "build's defaults."
                                      : juce::String(),
                                 juce::dontSendNotification);
        }
    }

    // The plan as the composer has published it. Reading it is a copy under a short lock and never
    // waits for a probe render; the list grows as the composer's spare time plans the next track.
    if (m_tracks == nullptr) return;
    m_trackRows.clear();
    for (int i = 0; i < 24; ++i) {
        TrackPlan plan;
        if (!m_proc.tryReadTrack(i, plan)) break;
        TrackDisplay::Row row;
        row.m_index = plan.m_index;
        row.m_bar = plan.m_firstBar;
        row.m_bars = plan.m_bars;
        row.m_bpm = plan.m_bpm;
        row.m_text = juce::String(plan.m_index + 1) + "   bar " + juce::String(plan.m_firstBar + 1)
                 + "   " + juce::String(plan.m_bars) + " bars   " + juce::String(plan.m_bpm, 1) + " BPM   "
                 + KeyNames[plan.m_key] + " " + ScaleNames[plan.m_scale]
                 + "   bass " + BassPatternNames[plan.m_primaryPattern]
                 + "   " + juce::String(plan.m_perc.m_layers) + " perc"
                 + melodicParts(plan)
                 + "   " + juce::String(plan.m_gainDb, 1) + " dB";
        m_trackRows.push_back(row);
    }
    if (m_trackRows.size() != m_rowsSeed) { m_rowsSeed = m_trackRows.size(); m_tracks->setRows(m_trackRows); }
    m_tracks->setPosition(m_proc.transport().m_bar);
}

// ==================================================================== the Export tab (01.10.2026)

void PhospheneEditor::buildExportPage()
{
    // What leaves the instrument, in one place as every generator has it: the recording of the output, the score as
    // MIDI, the set as a file (and back), the score cues over OSC.
    auto page = std::make_unique<ControlPage>();
    const juce::Colour tint = partColour(TAB_EXPORT);
    const int gr = page->addGroup("Record", tint, 6);
    {
        auto rec = std::make_unique<frame::IconButton>(frame::IconButton::IconE::RECORD, "Records the output into a WAV file until you press again");
        rec->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffb03030));
        rec->onClick = [this] {
            if (m_proc.isRecording()) { m_proc.stopRecording(); return; }
            m_chooser = std::make_unique<juce::FileChooser>("Record the output", juce::File(), "*.wav");
            m_chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      const juce::File f = fc.getResult();
                                      if (f != juce::File()) m_proc.startRecording(f.withFileExtension("wav"));
                                  });
        };
        m_recordButton = rec.get();
        page->addControl(gr, std::move(rec), "", 1, true);
        auto note = std::make_unique<juce::Label>(juce::String(), "the output as it plays, 32-bit float");
        note->setColour(juce::Label::textColourId, Dim);
        page->addControl(gr, std::move(note), "", 3, true);
    }
    const int ge = page->addGroup("Score and Set", tint, 8);
    {
        auto bars = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        bars->setRange(4.0, 1024.0, 4.0);
        bars->setValue(64.0, juce::dontSendNotification);
        m_exportBars = bars.get();
        page->addControl(ge, std::move(bars), "Bars", 1);

        auto midi = std::make_unique<juce::TextButton>("Export MIDI...");
        midi->setTooltip("Writes the score of that many bars from the start as a MIDI file, a channel per part (Ctrl+E)");
        midi->onClick = [this] {
            m_chooser = std::make_unique<juce::FileChooser>("Write the score", juce::File(), "*.mid");
            m_chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      const juce::File f = fc.getResult();
                                      if (f != juce::File()) m_proc.exportMidi(f.withFileExtension("mid"), static_cast<int>(m_exportBars->getValue()));
                                  });
        };
        m_exportMidi = midi.get();
        page->addControl(ge, std::move(midi), "", 3, true);

        auto set = std::make_unique<frame::IconButton>(frame::IconButton::IconE::SAVE, "Writes the set -- seed, locks, rerolls and every changed knob -- as a .phosset (Ctrl+S)");
        set->onClick = [this] {
            m_chooser = std::make_unique<juce::FileChooser>("Write the set", juce::File(), "*.phosset");
            m_chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      const juce::File f = fc.getResult();
                                      if (f != juce::File()) m_proc.exportSet(f.withFileExtension("phosset"));
                                  });
        };
        m_exportSet = set.get();
        page->addControl(ge, std::move(set), "", 1, true);

        auto load = std::make_unique<frame::IconButton>(frame::IconButton::IconE::OPEN, "Reads a .phosset and plays it (Ctrl+O)");
        load->onClick = [this] {
            m_chooser = std::make_unique<juce::FileChooser>("Read a set", juce::File(), "*.phosset");
            m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      const juce::File f = fc.getResult();
                                      bool ok = false;
                                      if (f != juce::File()) m_proc.undoable("Load set", [this, f, &ok] { ok = m_proc.importSet(f); });
                                      if (!ok) return;
                                      if (m_seedEditor != nullptr) m_seedEditor->setText(juce::String(m_proc.seed()), false);
                                      m_trackRows.clear();
                                      m_rowsSeed = 0;
                                  });
        };
        m_loadSet = load.get();
        page->addControl(ge, std::move(load), "", 1, true);
    }
    page->addModuleGroup(m_proc, ModuleE::CUE, 0, "Score Cues (OSC)", tint, 4);   // 23.09.2026, on the Mixer tab until 01.10.2026
    m_pages[static_cast<size_t>(TAB_EXPORT)] = std::move(page);
}

void PhospheneEditor::refreshExportPage()
{
    if (m_recordButton != nullptr) {
        m_recordButton->setToggleState(m_proc.isRecording(), juce::dontSendNotification);
        m_recordButton->show(frame::IconButton::IconE::RECORD, m_proc.isRecording() ? "Stop recording" : "Records the output into a WAV file until you press again");
    }
}
