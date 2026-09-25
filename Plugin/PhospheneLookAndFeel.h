/**
 * @file PhospheneLookAndFeel.h
 * @brief The plugin's palette and the knob it draws.
 *
 * One knob has to carry a name and a value in 76 by 84 pixels, because a set generator puts six
 * hundred of them on ten pages. The value is therefore drawn inside the knob's ring rather than in a
 * text box under it, and the name goes under the knob (EditorLayout.cpp lays that out). Parameters
 * that span zero get an arc growing out of the top centre, which reads as "how far from neutral"
 * instead of "how full".
 */
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

/** @brief Colours shared by the whole editor. */
namespace phosui {
const juce::Colour bg0     { 0xff10121a };   ///< window background
const juce::Colour card    { 0xff181b26 };   ///< a page's background
const juce::Colour group   { 0xff1f2331 };   ///< a group box
const juce::Colour edge    { 0xff2c3244 };   ///< hairlines
const juce::Colour text    { 0xffe4e8f4 };   ///< body text
const juce::Colour dim     { 0xff97a0ba };   ///< labels
const juce::Colour faint   { 0xff5d6683 };   ///< axes, disabled
const juce::Colour accent  { 0xff5ad1ff };   ///< the plugin's colour
const juce::Colour warm    { 0xffff9a4d };   ///< kick and bass
const juce::Colour green   { 0xff67e8a0 };   ///< meters in range
const juce::Colour red     { 0xffff6b6b };   ///< meters over
/** @brief The colour a generator's page is tinted with. */
juce::Colour partColour(int index);
/** @brief The editor's fonts. */
juce::Font title(float height);
juce::Font body(float height);   ///< @copydoc title
} // namespace phosui

/** @brief Look and feel of the Phosphene editor. */
class PhospheneLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    /** @brief Sets the editor's colours. */
    PhospheneLookAndFeel();
    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float pos,
                          float startAngle, float endAngle, juce::Slider&) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawComboBox(juce::Graphics&, int width, int height, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& background, bool highlighted, bool down) override;
    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    juce::Font getPopupMenuFont() override;
};
