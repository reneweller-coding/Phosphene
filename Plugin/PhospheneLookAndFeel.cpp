/**
 * @file PhospheneLookAndFeel.cpp
 * @brief Implementation of the editor's palette, knob, toggle and combo box.
 */
#include "PhospheneLookAndFeel.h"
#include <cmath>

namespace phosui {

juce::Colour partColour(int index)
{
    // One hue per generator page, walked around the wheel so that neighbouring tabs differ; the
    // saturation and brightness stay put, so no page shouts louder than another.
    static const float hues[] = { 0.55f, 0.06f, 0.10f, 0.33f, 0.78f, 0.62f, 0.47f, 0.88f, 0.16f, 0.71f };
    const int n = static_cast<int>(sizeof(hues) / sizeof(hues[0]));
    return juce::Colour::fromHSV(hues[((index % n) + n) % n], 0.55f, 0.92f, 1.0f);
}

juce::Font title(float height)
{
    return juce::Font(juce::FontOptions(height, juce::Font::bold)).withExtraKerningFactor(0.12f);
}

juce::Font body(float height)
{
    return juce::Font(juce::FontOptions(height));
}

} // namespace phosui

namespace {

/** @brief An arc with a soft wide copy underneath, so a lit value glows instead of being outlined. */
void arc(juce::Graphics& g, juce::Point<float> c, float radius, float from, float to,
         float thickness, juce::Colour colour, bool glow)
{
    if (std::fabs(to - from) < 1.0e-4f) return;
    juce::Path p;
    p.addCentredArc(c.x, c.y, radius, radius, 0.0f, from, to, true);
    if (glow) {
        g.setColour(colour.withMultipliedAlpha(0.18f));
        g.strokePath(p, juce::PathStrokeType(thickness * 2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    g.setColour(colour);
    g.strokePath(p, juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

} // namespace

PhospheneLookAndFeel::PhospheneLookAndFeel()
{
    using namespace phosui;
    setColour(juce::ResizableWindow::backgroundColourId, bg0);
    setColour(juce::Label::textColourId, text);
    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::rotarySliderFillColourId, accent);
    setColour(juce::Slider::rotarySliderOutlineColourId, edge);
    setColour(juce::Slider::thumbColourId, text);
    setColour(juce::Slider::textBoxTextColourId, text);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ComboBox::backgroundColourId, card.brighter(0.12f));
    setColour(juce::ComboBox::textColourId, text);
    setColour(juce::ComboBox::outlineColourId, edge);
    setColour(juce::ComboBox::arrowColourId, dim);
    setColour(juce::PopupMenu::backgroundColourId, card.brighter(0.05f));
    setColour(juce::PopupMenu::textColourId, text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha(0.22f));
    setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour(juce::TextButton::buttonColourId, card.brighter(0.12f));
    setColour(juce::TextButton::buttonOnColourId, accent.withAlpha(0.32f));
    setColour(juce::TextButton::textColourOffId, text);
    setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    setColour(juce::ToggleButton::textColourId, text);
    setColour(juce::ToggleButton::tickColourId, accent);
    setColour(juce::TextEditor::backgroundColourId, bg0);
    setColour(juce::TextEditor::textColourId, text);
    setColour(juce::TextEditor::outlineColourId, edge);
    setColour(juce::TextEditor::focusedOutlineColourId, accent.withAlpha(0.6f));
    setColour(juce::TextEditor::highlightColourId, accent.withAlpha(0.3f));
    setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ScrollBar::thumbColourId, faint);
    setColour(juce::AlertWindow::backgroundColourId, card);
    setColour(juce::AlertWindow::textColourId, text);
    setColour(juce::AlertWindow::outlineColourId, edge);
}

void PhospheneLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                            float pos, float startAngle, float endAngle, juce::Slider& s)
{
    using namespace phosui;
    const juce::Rectangle<float> area(static_cast<float>(x), static_cast<float>(y),
                                      static_cast<float>(width), static_cast<float>(height));
    const float side = juce::jmin(area.getWidth(), area.getHeight());
    const juce::Point<float> c = area.getCentre();
    const float r = side * 0.5f - 2.5f;
    if (r < 5.0f) return;
    const float thickness = juce::jmax(2.5f, r * 0.20f);
    const juce::Colour fill = s.findColour(juce::Slider::rotarySliderFillColourId);

    g.setColour(group.brighter(0.05f));
    g.fillEllipse(juce::Rectangle<float>(side - 2.0f * thickness, side - 2.0f * thickness).withCentre(c));
    arc(g, c, r, startAngle, endAngle, thickness, edge, false);

    const float angle = startAngle + pos * (endAngle - startAngle);
    const bool bipolar = s.getProperties()["bipolar"];
    if (bipolar) {
        const float mid = 0.5f * (startAngle + endAngle);
        arc(g, c, r, juce::jmin(mid, angle), juce::jmax(mid, angle), thickness, fill, true);
    } else {
        arc(g, c, r, startAngle, angle, thickness, fill, true);
    }
    // The pointer: short, bright, and inside the ring, so the value text stays readable.
    juce::Path pointer;
    pointer.addRectangle(-1.0f, -r + 1.0f, 2.0f, thickness * 1.5f);
    g.setColour(text);
    g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(c.x, c.y));

    // The value lives inside the ring, which is what buys a knob this size in a wall of six hundred.
    // It is fitted rather than clipped, so "10000 Hz" shrinks instead of turning into "10000 H".
    const juce::String v = s.getTextFromValue(s.getValue()).trim();
    g.setColour(text.withAlpha(s.isEnabled() ? 0.92f : 0.4f));
    g.setFont(phosui::body(juce::jlimit(8.5f, 11.5f, side * 0.20f)));
    g.drawFittedText(v, area.reduced(side * 0.13f, side * 0.34f).toNearestInt(), juce::Justification::centred, 1, 0.45f);
}

void PhospheneLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    using namespace phosui;
    const juce::Rectangle<float> r = b.getLocalBounds().toFloat().reduced(1.0f);
    const bool on = b.getToggleState();
    g.setColour(on ? accent.withAlpha(0.30f) : group.brighter(highlighted ? 0.12f : 0.04f));
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(on ? accent : edge);
    g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f);
    g.setColour(on ? juce::Colours::white : dim);
    g.setFont(phosui::body(juce::jlimit(9.0f, 12.0f, r.getHeight() * 0.5f)));
    g.drawText(b.getButtonText().isEmpty() ? (on ? "On" : "Off") : b.getButtonText(), r, juce::Justification::centred, false);
}

void PhospheneLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool,
                                        int, int, int, int, juce::ComboBox& box)
{
    using namespace phosui;
    const juce::Rectangle<float> r(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle(r.reduced(1.0f), 5.0f);
    g.setColour(edge);
    g.drawRoundedRectangle(r.reduced(1.5f), 5.0f, 1.0f);
    juce::Path tri;
    const float cx = r.getRight() - 11.0f, cy = r.getCentreY();
    tri.addTriangle(cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f, cx, cy + 3.0f);
    g.setColour(dim);
    g.fillPath(tri);
}

void PhospheneLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& background,
                                                bool highlighted, bool down)
{
    using namespace phosui;
    const juce::Rectangle<float> r = b.getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(background.brighter(down ? 0.22f : (highlighted ? 0.12f : 0.0f)));
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(b.getToggleState() ? accent : edge);
    g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f);
}

juce::Font PhospheneLookAndFeel::getLabelFont(juce::Label& l) { return phosui::body(juce::jlimit(9.0f, 13.0f, l.getHeight() * 0.72f)); }
juce::Font PhospheneLookAndFeel::getComboBoxFont(juce::ComboBox& b) { return phosui::body(juce::jlimit(9.0f, 12.0f, b.getHeight() * 0.52f)); }
juce::Font PhospheneLookAndFeel::getTextButtonFont(juce::TextButton&, int h) { return phosui::body(juce::jlimit(9.0f, 13.0f, h * 0.5f)); }
juce::Font PhospheneLookAndFeel::getPopupMenuFont() { return phosui::body(13.0f); }
