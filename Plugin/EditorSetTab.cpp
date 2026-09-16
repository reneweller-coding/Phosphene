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

    // ---------------------------------------------------------------- meters, plan, export
    const int gm = page->addGroup("Loudness", tint, 4);
    {
        auto meter = std::make_unique<LoudnessDisplay>();
        loudness_ = meter.get();
        page->addControl(gm, std::move(meter), "", 4, true, 2);
    }
    const int gp = page->addGroup("Plan", tint, 7);
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
                 + (plan.melody.present[0] ? "  acid" : "") + (plan.melody.present[1] ? "  lead" : "")
                 + (plan.melody.present[2] ? "  arp" : "")
                 + "   " + juce::String(plan.gainDb, 1) + " dB";
        trackRows_.push_back(row);
    }
    if (trackRows_.size() != rowsSeed_) { rowsSeed_ = trackRows_.size(); tracks_->setRows(trackRows_); }
    tracks_->setPosition(proc_.transport().bar);
}
