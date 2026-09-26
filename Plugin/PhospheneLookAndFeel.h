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

/**
 * @brief Colours shared by the whole editor: "UV blacklight" (26.09.2026, the user's choice).
 *
 * The user found the colours "etwas zufällig" -- sixteen hues walked around the wheel, one per tab. Modern psytrance
 * dresses its stages the same way almost everywhere: a near-black indigo ground and a few fluorescent colours that
 * light up under UV. So the base is indigo-black, and a page takes the colour of its *family*, not a hue of its own:
 * the low end (kick, bass, percussion) hot magenta, the lines (lead, counter, arp, stab) UV cyan, the acid its own
 * acid green, the space (pad, drone, effects) electric violet, and the pages about the whole set a quiet lavender.
 * Neighbours within a family differ by a small step, so a tab is still recognised by its colour.
 */
namespace phosui {
const juce::Colour bg0     { 0xff0b0a12 };   ///< window background
const juce::Colour card    { 0xff12101b };   ///< a page's background
const juce::Colour group   { 0xff191625 };   ///< a group box
const juce::Colour edge    { 0xff2b2641 };   ///< hairlines
const juce::Colour text    { 0xffeceaf6 };   ///< body text
const juce::Colour dim     { 0xffa39dc2 };   ///< labels
const juce::Colour faint   { 0xff5f5982 };   ///< axes, disabled
const juce::Colour accent  { 0xffa46bff };   ///< the plugin's colour: UV violet
const juce::Colour warm    { 0xffff2e97 };   ///< kick and bass: hot magenta
const juce::Colour green   { 0xff8dff5a };   ///< meters in range
const juce::Colour red     { 0xffff4d6d };   ///< meters over
/** @brief The colour of the tab @p index (PluginEditor.h, Tab): its family's, stepped a little per tab. */
juce::Colour partColour(int index);
/** @brief The colour of the tab that shows @p part (Score.h, Part). */
juce::Colour partColourOf(int part);
/** @brief A colour for the @p i-th of several equal things (the tracks of the Arrange tab): the families in turn. */
juce::Colour cycleColour(int i);
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
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height, float pos, float minPos, float maxPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawComboBox(juce::Graphics&, int width, int height, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& background, bool highlighted, bool down) override;
    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    juce::Font getPopupMenuFont() override;
};
