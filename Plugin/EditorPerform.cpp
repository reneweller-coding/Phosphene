/**
 * @file EditorPerform.cpp
 * @brief The Perform tab: four macros, and what each of them is doing right now.
 *
 * PLAN 8.1 asks for "live macros (filter sweep, FX throw, break now, drop now, stutter)". Four are
 * built, and the reason there are four rather than a wall of them is that a macro has to be worth a
 * name: something a player reaches for without looking, and something the manual can describe in one
 * line. Each one names the handful of parameters it moves (PhospheneProcessor::macroTargets), and
 * the page prints them under the control -- so what the manual says, what the tooltip says and what
 * the plugin does are the same list.
 *
 * Two of them are values that are held (a knob), two are pressed (a button). A drop-out is a press
 * because it means "one bar, from here"; a stutter is held because it means "while I hold this".
 */
#include "PluginEditor.h"
#include "phos/Rating.h"
#include <algorithm>
#include <cmath>
#include <ctime>

using namespace phos;
using namespace phosui;

void PhospheneEditor::buildPerformPage()
{
    auto page = std::make_unique<phosui::ControlPage>();
    const juce::Colour tint = partColour(TabPerform);

    const int gm = page->addGroup("Macros", tint, 16);
    for (int i = 0; i < kNumMacros; ++i) {
        const Macro m = static_cast<Macro>(i);
        if (macroIsMomentary(m)) {
            auto b = std::make_unique<juce::TextButton>(kMacroNames[i]);
            b->setTooltip(kMacroHelp[i]);
            if (m == Macro::Stutter) {
                // Held: it starts when the mouse goes down and ends when it comes up.
                b->setTriggeredOnMouseDown(true);
                juce::TextButton* raw = b.get();
                b->onStateChange = [this, raw] { proc_.setMacro(Macro::Stutter, raw->isDown() ? 1.0f : 0.0f); };
            } else {
                b->onClick = [this, m] { proc_.setMacro(m, 1.0f); };
            }
            macroButton_[i] = b.get();
            page->enableMidiLearn(proc_, *b, proc_.params().count() + i);   // a macro is a MIDI target like any knob
            page->addControl(gm, std::move(b), "", 4, true);
        } else {
            auto s = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
            const bool bipolar = m == Macro::FilterSweep;
            s->setRange(bipolar ? -1.0 : 0.0, 1.0, 0.0);
            s->setNumDecimalPlacesToDisplay(2);   // the ring holds a number, not a float's every digit
            s->setValue(0.0, juce::dontSendNotification);
            s->setDoubleClickReturnValue(true, 0.0);   // back to neutral, which is what a macro needs most
            if (bipolar) s->getProperties().set("bipolar", true);
            s->setTooltip(kMacroHelp[i]);
            juce::Slider* raw = s.get();
            s->onValueChange = [this, m, raw] { proc_.setMacro(m, static_cast<float>(raw->getValue())); };
            macroSlider_[i] = s.get();
            page->enableMidiLearn(proc_, *s, proc_.params().count() + i);
            page->addControl(gm, std::move(s), kMacroNames[i], 4);
        }
    }

    // What each macro moves, generated from the same table the manual prints: four lines, one per
    // macro, so the page cannot drift from what the processor does.
    const int gw = page->addGroup("What they move", tint, 16);
    {
        juce::String all;
        for (int i = 0; i < kNumMacros; ++i) {
            PhospheneProcessor::MacroTarget t[PhospheneProcessor::kMaxMacroTargets];
            const int n = proc_.macroTargets(static_cast<Macro>(i), 1.0f, t);
            if (i > 0) all << "\n";
            all << kMacroNames[i] << ":  ";
            for (int k = 0; k < n; ++k) {
                if (k > 0) all << ", ";
                all << juce::String(proc_.params().key(t[k].param));
                if (!t[k].absolute) all << " " << (t[k].value >= 0 ? "+" : "") << juce::String(t[k].value, 2);
            }
            if (n == 0) all << "nothing";
        }
        auto label = std::make_unique<juce::Label>(juce::String(), all);
        label->setJustificationType(juce::Justification::topLeft);
        label->setColour(juce::Label::textColourId, dim);
        label->setMinimumHorizontalScale(0.7f);
        page->addControl(gw, std::move(label), "", 16, true);
    }

    const int gs = page->addGroup("State", tint, 16);
    {
        auto note = std::make_unique<juce::Label>(juce::String(), "neutral");
        note->setJustificationType(juce::Justification::centredLeft);
        note->setColour(juce::Label::textColourId, dim);
        macroNote_ = note.get();
        page->addControl(gs, std::move(note), "", 16, true);
    }

    // MIDI (23.09.2026, phos/MidiMap.h): every knob, switch and chooser in the plugin -- and the four macros above --
    // takes a controller. Right click, "MIDI Learn", turn the hardware knob. This group says what is being learned
    // and what is bound, and forgets everything at once.
    // The keyboard (23.09.2026, Engine::liveNoteOn): notes coming in play the chosen voice with its sound as its page
    // has it. Replace leaves out the generator's notes of that voice, Layer plays over them.
    const int gk = page->addModuleGroup(proc_, Module::Mix, 0, "Keyboard", tint, 16, mix::KeyboardPart, 2);
    {
        auto note = std::make_unique<juce::Label>(juce::String(),
            "Notes from a MIDI keyboard play this voice, with the sound its page has. By channel: 1 acid, 2 lead, "
            "3 counter, 4 arp, 5 stab, 6 pad, 7 drone. Plays while the set runs.");
        note->setJustificationType(juce::Justification::topLeft);
        note->setColour(juce::Label::textColourId, dim);
        note->setMinimumHorizontalScale(0.7f);
        page->addControl(gk, std::move(note), "", 12, true);
    }
    const int gx = page->addGroup("MIDI", tint, 16);
    {
        auto note = std::make_unique<juce::Label>(juce::String(), "no controller learned");
        note->setJustificationType(juce::Justification::topLeft);
        note->setColour(juce::Label::textColourId, dim);
        note->setMinimumHorizontalScale(0.7f);
        midiNote_ = note.get();
        page->addControl(gx, std::move(note), "", 12, true);
        auto clear = std::make_unique<juce::TextButton>("Forget all");
        clear->setTooltip("Releases every controller from every parameter");
        clear->onClick = [this] { proc_.midiMap().clear(); };
        page->addControl(gx, std::move(clear), "", 4, true);
    }

    // Rating what is playing (23.09.2026, phos/Rating.h): "good here" and "bad here" write one line each --
    // seed, track, bar, section, style, the note -- into ratings.tsv in the user's application data folder,
    // so that a listening session ends in bars instead of in prose. Tools/ratings.py reads the file back.
    const int gr = page->addGroup("Rate what you hear", tint, 16);
    {
        auto note = std::make_unique<juce::TextEditor>();
        note->setTextToShowWhenEmpty("note (optional): what is good or wrong here", dim);
        note->setTooltip("Written with the next verdict and then cleared.");
        ratingNote_ = note.get();
        page->addControl(gr, std::move(note), "", 8, true);
        static const char* const kLabels[2] = { "Good here", "Bad here" };
        static const char* const kHelp[2] = { "Writes 'good' for the bar that is playing now into ratings.tsv",
                                              "Writes 'bad' for the bar that is playing now into ratings.tsv" };
        for (int i = 0; i < 2; ++i) {
            auto b = std::make_unique<juce::TextButton>(kLabels[i]);
            b->setTooltip(kHelp[i]);
            const int verdict = i == 0 ? 1 : -1;
            b->onClick = [this, verdict] { rateNow(verdict); };
            ratingButton_[i] = b.get();
            page->addControl(gr, std::move(b), "", 4, true);
        }
        auto last = std::make_unique<juce::Label>(juce::String(), "nothing rated yet");
        last->setJustificationType(juce::Justification::centredLeft);
        last->setColour(juce::Label::textColourId, dim);
        last->setMinimumHorizontalScale(0.7f);
        ratingLast_ = last.get();
        page->addControl(gr, std::move(last), "", 16, true);

        // Learning from the verdicts (23.09.2026, phos/Preferences.h): the fit is shown before it is applied.
        auto learn = std::make_unique<juce::TextButton>("Learn from my ratings...");
        learn->setTooltip("Counts your good and bad verdicts per decision (style, form, lead archetype, arp, counter, harmony) "
                          "and lets the composer choose what you liked more often");
        learn->onClick = [this] {
            int verdicts = 0;
            const Preferences prefs = proc_.fitFromRatings(&verdicts);
            juce::String text;
            if (prefs.empty()) text << "Nothing to learn yet: " << verdicts << " verdicts with their decisions. Rate a few bars first (Good here / Bad here).";
            else text << verdicts << " verdicts give " << static_cast<int>(prefs.size()) << " weights (positive: chosen more often):\n\n" << juce::String(prefs.toText()).fromFirstOccurrenceOf("\n", false, false);
            galleryAsk_ = std::make_unique<juce::AlertWindow>("Learn from my ratings", text, juce::MessageBoxIconType::NoIcon);
            if (!prefs.empty()) galleryAsk_->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
            galleryAsk_->addButton(prefs.empty() ? "OK" : "Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            galleryAsk_->enterModalState(true, juce::ModalCallbackFunction::create([this, prefs](int result) {
                if (result == 1) proc_.applyPreferences(prefs);
                if (prefsNote_ != nullptr) prefsNote_->setText(proc_.preferencesSummary(), juce::dontSendNotification);
                juce::MessageManager::callAsync([safe = juce::Component::SafePointer<PhospheneEditor>(this)] { if (safe != nullptr) safe->galleryAsk_.reset(); });
            }), false);
        };
        page->addControl(gr, std::move(learn), "", 4, true);
        auto forget = std::make_unique<juce::TextButton>("Forget learned preferences...");
        forget->setTooltip("Puts preferences.txt aside (as preferences.txt.old) and lets the styles choose on their own again");
        forget->onClick = [this] {
            galleryAsk_ = std::make_unique<juce::AlertWindow>("Forget learned preferences",
                                                              "The composer goes back to the styles' own choices. The file is kept as preferences.txt.old.",
                                                              juce::MessageBoxIconType::NoIcon);
            galleryAsk_->addButton("Forget", 1, juce::KeyPress(juce::KeyPress::returnKey));
            galleryAsk_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            galleryAsk_->enterModalState(true, juce::ModalCallbackFunction::create([this](int result) {
                if (result == 1) proc_.forgetPreferences();
                if (prefsNote_ != nullptr) prefsNote_->setText(proc_.preferencesSummary(), juce::dontSendNotification);
                juce::MessageManager::callAsync([safe = juce::Component::SafePointer<PhospheneEditor>(this)] { if (safe != nullptr) safe->galleryAsk_.reset(); });
            }), false);
        };
        page->addControl(gr, std::move(forget), "", 4, true);
        auto pn = std::make_unique<juce::Label>(juce::String(), proc_.preferencesSummary());
        pn->setJustificationType(juce::Justification::centredLeft);
        pn->setColour(juce::Label::textColourId, dim);
        pn->setMinimumHorizontalScale(0.6f);
        prefsNote_ = pn.get();
        page->addControl(gr, std::move(pn), "", 8, true);
    }
    pages_[static_cast<size_t>(TabPerform)] = std::move(page);
}

void PhospheneEditor::rateNow(int verdict)
{
    const TransportView tv = proc_.transport();
    RatingEntry e;
    e.seed = proc_.seed();
    e.track = tv.track + 1;
    e.bar = tv.bar + 1;
    e.barInTrack = tv.barInTrack + 1;
    e.verdict = verdict;
    e.source = "plugin";
    TrackPlan plan;
    if (proc_.tryReadTrack(tv.track, plan) && plan.form.count > 0) {
        const int si = sectionOfBar(plan.form, tv.barInTrack);
        e.section = kSectionNames[static_cast<int>(plan.form.section[si].type)];
        e.style = kStyleNames[std::clamp(plan.style, 0, kNumStyles - 1)];
    }
    if (ratingNote_ != nullptr) e.note = ratingNote_->getText().toStdString();
    // What this verdict teaches: the composer's decisions at the bar (phos/Preferences.h, decisionFeatures).
    if (plan.form.count > 0) e.features = decisionFeatures(plan, tv.barInTrack);
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &local);
        e.time = buf;
    }
    const juce::File file = proc_.ratingsFile();
    const bool ok = appendRating(file.getFullPathName().toRawUTF8(), e);
    if (ratingNote_ != nullptr && ok) ratingNote_->clear();
    if (ratingLast_ != nullptr) {
        juce::String s;
        if (ok) s << (verdict > 0 ? "good" : "bad") << ": track " << e.track << ", bar " << e.barInTrack << " (" << e.section << ", " << e.style
                  << ")  ->  " << file.getFullPathName();
        else s << "could not write " << file.getFullPathName();
        ratingLast_->setText(s, juce::dontSendNotification);
    }
}

void PhospheneEditor::refreshPerformPage()
{
    if (macroNote_ == nullptr) return;
    // A momentary macro lets go by itself (a drop-out on the next bar line), so the page reports
    // what the processor holds rather than what the mouse last did.
    juce::String s;
    for (int i = 0; i < kNumMacros; ++i) {
        const float v = proc_.macroValue(static_cast<Macro>(i));
        if (macroSlider_[i] != nullptr && std::fabs(static_cast<float>(macroSlider_[i]->getValue()) - v) > 1.0e-4f)
            macroSlider_[i]->setValue(v, juce::dontSendNotification);
        if (macroButton_[i] != nullptr) {
            macroButton_[i]->setToggleState(v > 0.0f, juce::dontSendNotification);
            // 22.09.2026: a drop-out means "one bar, from here", so with the transport standing
            // still there is no bar line to let go on and the processor clears the macro in the same
            // call that sets it (PluginProcessor.cpp, serviceMacros). Pressing it then did exactly
            // nothing and said nothing -- a button that looks alive and is not. It now greys out
            // while nothing is playing, so the page says why instead of swallowing the press.
            if (static_cast<Macro>(i) == Macro::DropOut)
                macroButton_[i]->setEnabled(proc_.transport().playing);
        }
        if (v == 0.0f) continue;
        if (s.isNotEmpty()) s << "   -   ";
        s << kMacroNames[i] << " " << juce::String(v, 2);
    }
    macroNote_->setText(s.isEmpty() ? juce::String("neutral: every knob is where you left it") : s,
                        juce::dontSendNotification);

    // MIDI learn: what is armed, and the bindings (redrawn only when the table changed).
    if (midiNote_ != nullptr) {
        const phos::MidiMap& map = proc_.midiMap();
        const int armed = map.armed();
        const uint32_t rev = map.revision() * 2u + (armed >= 0 ? 1u : 0u);
        if (rev != midiShown_ || armed != midiArmedShown_) {
            midiShown_ = rev;
            midiArmedShown_ = armed;
            juce::String t;
            if (armed >= 0) t << "Learning " << proc_.midiTargetName(armed) << ": move a knob or fader on your controller.\n";
            const std::vector<phos::MidiBinding> b = map.bindings();
            if (b.empty() && armed < 0) t << "No controller learned. Right-click any knob, switch or chooser and choose MIDI Learn.";
            for (size_t k = 0; k < b.size(); ++k) {
                if (k > 0) t << ",  ";
                t << "CC " << b[k].cc << "/" << (b[k].channel + 1) << " " << proc_.midiTargetName(b[k].target);
            }
            midiNote_->setText(t, juce::dontSendNotification);
        }
    }
}
