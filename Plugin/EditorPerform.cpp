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
#include <cmath>

using namespace phos;
using namespace phosui;

void PhospheneEditor::buildPerformPage()
{
    auto page = std::make_unique<phosui::ControlPage>();
    const juce::Colour tint = partColour(6);

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
    pages_[static_cast<size_t>(TabPerform)] = std::move(page);
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
}
