/**
 * @file EditorSetTab.cpp
 * @brief The Set tab: seed, transport, loudness, the track list and the exports.
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
    static const char* const kNames[kMelodyParts] = { "acid", "lead", "counter", "arp", "stab", "pad", "drone" };
    juce::String s;
    for (int k = 0; k < kMelodyParts; ++k) if (plan.melody.present[k]) s << "  " << kNames[k];
    return s;
}
} // namespace

void PhospheneEditor::buildSetPage()
{
    auto page = std::make_unique<ControlPage>();
    const juce::Colour tint = partColour(0);

    // ---------------------------------------------------------------- transport and seed
    // Twelve cells wide: the transport is a strip across the top of the page, and the
    // composer's knobs stand in three groups underneath it.
    const int gt = page->addGroup("Transport", tint, 12);
    {
        auto play = std::make_unique<juce::TextButton>("Play");
        play->onClick = [this] { proc_.play(); };
        playButton_ = play.get();
        page->addControl(gt, std::move(play), "", 1, true);

        auto stop = std::make_unique<juce::TextButton>("Stop");
        stop->onClick = [this] { proc_.stop(); };
        stopButton_ = stop.get();
        page->addControl(gt, std::move(stop), "", 1, true);

        auto rec = std::make_unique<juce::TextButton>("Record...");
        rec->onClick = [this] {
            if (proc_.isRecording()) { proc_.stopRecording(); return; }
            chooser_ = std::make_unique<juce::FileChooser>("Record the output", juce::File(), "*.wav");
            chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      const juce::File f = fc.getResult();
                                      if (f != juce::File()) proc_.startRecording(f.withFileExtension("wav"));
                                  });
        };
        recordButton_ = rec.get();
        page->addControl(gt, std::move(rec), "", 2, true);

        auto follow = makeToggle("Follow host");
        follow->setToggleState(proc_.followsHost(), juce::dontSendNotification);
        follow->onClick = [this] { proc_.setFollowHost(followButton_->getToggleState()); };
        followButton_ = follow.get();
        page->addControl(gt, std::move(follow), "", 2, true);

        auto seed = std::make_unique<juce::TextEditor>();
        seed->setText(juce::String(proc_.seed()), false);
        seed->setJustification(juce::Justification::centred);
        seed->onReturnKey = [this] {
            const juce::int64 v = seedEditor_->getText().getLargeIntValue();
            proc_.setSeed(static_cast<uint64_t>(juce::jmax<juce::int64>(1, v)));
        };
        seed->onFocusLost = [this] { seedEditor_->setText(juce::String(proc_.seed()), false); };
        seedEditor_ = seed.get();
        page->addControl(gt, std::move(seed), "Seed", 2);

        auto dice = std::make_unique<juce::TextButton>("Randomize seed");
        dice->onClick = [this] {
            proc_.randomiseSeed();
            seedEditor_->setText(juce::String(proc_.seed()), false);
            trackRows_.clear();
            rowsSeed_ = 0;
        };
        page->addControl(gt, std::move(dice), "", 3, true);

        auto mute = makeToggle("Mute");
        mute->setToggleState(proc_.muted(), juce::dontSendNotification);
        mute->setEnabled(!proc_.muteForced());
        mute->onClick = [this] { proc_.setMuted(muteButton_->getToggleState()); };
        muteButton_ = mute.get();
        page->addControl(gt, std::move(mute), "", 1, true);

        auto status = std::make_unique<juce::Label>(juce::String(), "stopped");
        status->setJustificationType(juce::Justification::centredLeft);
        status->setColour(juce::Label::textColourId, dim);
        statusLabel_ = status.get();
        page->addControl(gt, std::move(status), "", 12, true);
    }

    // ---------------------------------------------------------------- the composer's knobs
    page->addModuleGroup(proc_, Module::Compose, 0, "Set", tint, 5, compose::Bpm, 13);
    page->addModuleGroup(proc_, Module::Compose, 0, "Rhythm", tint, 3, compose::PercDensity, 3);
    page->addModuleGroup(proc_, Module::Compose, 0, "Melody", tint, 5, compose::AcidAmount, 10);
    // The form (Phase 5): style profile, energy arc, whether tracks run at the profile's tempo, set length.
    page->addModuleGroup(proc_, Module::Compose, 0, "Form", tint, 2, compose::Style, compose::PresenceMatch - compose::Style + 1);   // two columns, so the row below still holds four groups
    // 22.09.2026, round "Lead": the two melodic knobs appended behind the form block (Params.h).
    page->addModuleGroup(proc_, Module::Compose, 0, "Lead", tint, 2, compose::LeadDensity, 2);

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
    const PhospheneProcessor::LearnedModels m = proc_.learnedModels();
    const int gn = page->addGroup("Pitch models", tint, 6);
    {
        struct Line { const char* part; const char* fallback; bool learned; double nll; juce::String note; };
        const Line lines[2] = { { "Melody", "Markov",  m.melody, m.melodyNll, m.melodyNote },
                                { "Bass",   "Pattern", m.bass,   m.bassNll,   m.bassNote } };
        for (const Line& l : lines) {
            auto lab = std::make_unique<juce::Label>(juce::String(),
                                                     modelLine(l.part, l.fallback, l.learned, l.nll, m.fromSourceTree));
            lab->setJustificationType(juce::Justification::centredLeft);
            // The palette's own "over" colour, the one the loudness meter uses when a reading is not
            // what it should be: a fallback is not an error, but it is not the product either.
            lab->setColour(juce::Label::textColourId, l.learned ? dim : red);
            // The whole truth, for anyone who wants it: the directory the plugin looked in, or the
            // core's own message naming every path it tried.
            lab->setTooltip(l.learned ? (m.directory.isEmpty() ? juce::String("found outside this installation")
                                                              : m.directory)
                                      : l.note);
            page->addControl(gn, std::move(lab), "", 6, true);
        }
    }

    // ---------------------------------------------------------------- meters, plan, export
    const int gm = page->addGroup("Loudness", tint, 3);
    {
        auto meter = std::make_unique<LoudnessDisplay>();
        loudness_ = meter.get();
        page->addControl(gm, std::move(meter), "", 3, true, 2);
    }
    const int gp = page->addGroup("Plan", tint, 5);
    {
        auto list = std::make_unique<TrackDisplay>();
        list->onJump = [this](int bar) { proc_.seekToBar(bar); };
        tracks_ = list.get();
        page->addControl(gp, std::move(list), "", 7, true, 3);
    }
    const int ge = page->addGroup("Export", tint, 4);
    {
        auto bars = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        bars->setRange(4.0, 1024.0, 4.0);
        bars->setValue(64.0, juce::dontSendNotification);
        exportBars_ = bars.get();
        page->addControl(ge, std::move(bars), "Bars", 1);

        auto midi = std::make_unique<juce::TextButton>("Export MIDI...");
        midi->onClick = [this] {
            chooser_ = std::make_unique<juce::FileChooser>("Write the score", juce::File(), "*.mid");
            chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      const juce::File f = fc.getResult();
                                      if (f != juce::File()) proc_.exportMidi(f.withFileExtension("mid"), static_cast<int>(exportBars_->getValue()));
                                  });
        };
        page->addControl(ge, std::move(midi), "", 3, true);

        auto set = std::make_unique<juce::TextButton>("Export set...");
        set->onClick = [this] {
            chooser_ = std::make_unique<juce::FileChooser>("Write the set", juce::File(), "*.phosset");
            chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      const juce::File f = fc.getResult();
                                      if (f != juce::File()) proc_.exportSet(f.withFileExtension("phosset"));
                                  });
        };
        page->addControl(ge, std::move(set), "", 2, true);

        auto load = std::make_unique<juce::TextButton>("Load set...");
        load->onClick = [this] {
            chooser_ = std::make_unique<juce::FileChooser>("Read a set", juce::File(), "*.phosset");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      const juce::File f = fc.getResult();
                                      if (f == juce::File() || !proc_.importSet(f)) return;
                                      seedEditor_->setText(juce::String(proc_.seed()), false);
                                      trackRows_.clear();
                                      rowsSeed_ = 0;
                                  });
        };
        page->addControl(ge, std::move(load), "", 2, true);
    }

    // 20.09.2026, round "dialogue". Two things the rule asks for, in one place where they can be seen:
    // a visible way back to the shipped calibration, and -- when a state older than version 3 was
    // loaded -- the offer that replaces silently applying it (PluginProcessor.h, kStateVersion).
    const int gd = page->addGroup("Factory defaults", tint, 4);
    {
        auto reset = std::make_unique<juce::TextButton>("Reset to factory defaults");
        reset->setTooltip("Puts every knob back to the value this build ships with. The set itself -- seed, "
                          "locks and rerolls -- is left alone.");
        reset->onClick = [this] {
            proc_.resetToFactoryDefaults();
            if (legacyNote_ != nullptr) legacyNote_->setText(juce::String(), juce::dontSendNotification);
            if (legacyButton_ != nullptr) legacyButton_->setVisible(false);
        };
        page->addControl(gd, std::move(reset), "", 2, true);

        auto adopt = std::make_unique<juce::TextButton>("Load the saved knobs anyway");
        adopt->setTooltip("Applies the knob values of the older session that was loaded. They were saved "
                          "before this build's calibration and will replace it.");
        adopt->onClick = [this] {
            proc_.adoptLegacyState();
            if (legacyNote_ != nullptr) legacyNote_->setText("The older session's knobs are in.", juce::dontSendNotification);
            if (legacyButton_ != nullptr) legacyButton_->setVisible(false);
        };
        legacyButton_ = adopt.get();
        legacyButton_->setVisible(false);
        page->addControl(gd, std::move(adopt), "", 2, true);

        auto note = std::make_unique<juce::Label>();
        note->setJustificationType(juce::Justification::centredLeft);
        note->setMinimumHorizontalScale(1.0f);
        legacyNote_ = note.get();
        page->addControl(gd, std::move(note), "", 4, true);
    }
    pages_[0] = std::move(page);
}

void PhospheneEditor::refreshSetPage()
{
    if (loudness_ == nullptr) return;
    const ParamStore& p = proc_.params();
    loudness_->update(proc_.engine().meter(), proc_.engine().compReduction(), proc_.engine().limiterReduction(),
                      p.get(p.base(Module::Master) + master::TargetLufs));

    const TransportView t = proc_.transport();
    if (playButton_ != nullptr) playButton_->setToggleState(t.playing && !t.hostSync, juce::dontSendNotification);
    if (muteButton_ != nullptr) muteButton_->setToggleState(proc_.muted(), juce::dontSendNotification);
    if (followButton_ != nullptr) followButton_->setToggleState(proc_.followsHost(), juce::dontSendNotification);
    if (statusLabel_ != nullptr) {
        juce::String s;
        if (t.restarting) s << "planning the next bars...   ";
        s << (t.hostSync ? "host transport" : "own clock") << "   bar " << juce::String(t.bar + 1)
          << "   beat " << juce::String(t.musicalBeat, 2) << "   " << juce::String(t.bpm, 1) << " BPM";
        if (proc_.isRecording()) s << "   recording " << juce::String(proc_.recordedSeconds(), 1) << " s";
        statusLabel_->setText(s, juce::dontSendNotification);
    }
    if (recordButton_ != nullptr) recordButton_->setButtonText(proc_.isRecording() ? "Stop recording" : "Record...");
    // The older-state offer (20.09.2026). It appears only while a state of version 1 or 2 is held back,
    // and it names the version, so the user can see why their session came up on the defaults.
    if (legacyButton_ != nullptr && legacyNote_ != nullptr) {
        const bool held = proc_.pendingLegacyState().isNotEmpty();
        if (held != legacyButton_->isVisible()) {
            legacyButton_->setVisible(held);
            legacyNote_->setText(held ? "This session was saved by an older build (state version "
                                            + juce::String(proc_.lastStateVersion())
                                            + "). Such a state stores every knob of its own session, including the ones "
                                              "later calibration changed, so it was not applied: you are hearing this "
                                              "build's defaults."
                                      : juce::String(),
                                 juce::dontSendNotification);
        }
    }

    // The plan as the composer has published it. Reading it is a copy under a short lock and never
    // waits for a probe render; the list grows as the composer's spare time plans the next track.
    if (tracks_ == nullptr) return;
    trackRows_.clear();
    for (int i = 0; i < 24; ++i) {
        TrackPlan plan;
        if (!proc_.tryReadTrack(i, plan)) break;
        TrackDisplay::Row row;
        row.index = plan.index;
        row.bar = plan.firstBar;
        row.bars = plan.bars;
        row.bpm = plan.bpm;
        row.text = juce::String(plan.index + 1) + "   bar " + juce::String(plan.firstBar + 1)
                 + "   " + juce::String(plan.bars) + " bars   " + juce::String(plan.bpm, 1) + " BPM   "
                 + kKeyNames[plan.key] + " " + kScaleNames[plan.scale]
                 + "   bass " + kBassPatternNames[plan.primaryPattern]
                 + "   " + juce::String(plan.perc.layers) + " perc"
                 + melodicParts(plan)
                 + "   " + juce::String(plan.gainDb, 1) + " dB";
        trackRows_.push_back(row);
    }
    if (trackRows_.size() != rowsSeed_) { rowsSeed_ = trackRows_.size(); tracks_->setRows(trackRows_); }
    tracks_->setPosition(proc_.transport().bar);
}
