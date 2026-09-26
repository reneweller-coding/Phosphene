/**
 * @file PhospheneLookAndFeel.cpp
 * @brief Implementation of the editor's palette, knob, toggle and combo box.
 */
#include "PhospheneLookAndFeel.h"
#include <cmath>

namespace phosui {

juce::Colour partColour(int index)
{
    // In tab order (PluginEditor.h): Set, Arrange | Kick, Bass, Percussion | Acid | Lead, Counter, Arp, Stab |
    // Pad, Drone, SFX / FX | Mixer, Perform, Gallery. See the palette's note in the header.
    static const juce::uint32 tabs[] = {
        0xffb9b3d6, 0xffc4bde4,                           // the set: lavender
        0xffff2e97, 0xffff4fa8, 0xffff6fbd,               // the low end: hot magenta
        0xffb8ff3c,                                       // the acid: acid green
        0xff22e4ff, 0xff3cc8ff, 0xff22f0d0, 0xff62b4ff,   // the lines: UV cyan
        0xff9b6bff, 0xff7d5cff, 0xffc46bff,               // the space: electric violet
        0xffb9b3d6, 0xffc4bde4, 0xffaea8cc,               // mixer, perform, gallery: lavender
    };
    const int n = static_cast<int>(sizeof(tabs) / sizeof(tabs[0]));
    return juce::Colour(tabs[((index % n) + n) % n]);
}

juce::Colour partColourOf(int part)
{
    // Score.h's parts run Kick .. Sfx in the tabs' order from TabKick (2); the bed and the voices live on the SFX tab.
    return partColour(part <= 10 ? part + 2 : 12);
}

juce::Colour cycleColour(int i)
{
    static const juce::uint32 c[] = { 0xffff2e97, 0xff22e4ff, 0xff9b6bff, 0xffb8ff3c, 0xffff6fbd, 0xff62b4ff };
    return juce::Colour(c[((i % 6) + 6) % 6]);
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
    // The live ring (26.09.2026; phosui::showLive): where the parameter plays away from the knob -- a ride, a lift, a
    // correction -- a thin bright arc on the ring from the knob to the played value, and a dot where it plays.
    const juce::var live = s.getProperties()["live"];
    if (!live.isVoid()) {
        const float lp = static_cast<float>(juce::jlimit(0.0, 1.0, s.valueToProportionOfLength(static_cast<double>(live))));
        if (std::fabs(lp - pos) > 0.004f) {
            const float la = startAngle + lp * (endAngle - startAngle);
            arc(g, c, r, juce::jmin(angle, la), juce::jmax(angle, la), thickness * 0.42f, text.withAlpha(0.85f), false);
            const float dr = juce::jmax(2.0f, thickness * 0.45f);
            g.setColour(text);
            g.fillEllipse(juce::Rectangle<float>(2.0f * dr, 2.0f * dr).withCentre(c.getPointOnCircumference(r, la)));
        }
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
    g.setFont(phosui::body(juce::jlimit(8.5f, 15.0f, side * 0.18f)));   // a large knob reads its value larger
    g.drawFittedText(v, area.reduced(side * 0.13f, side * 0.34f).toNearestInt(), juce::Justification::centred, 1, 0.45f);
}

void PhospheneLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float pos, float minPos,
                                            float maxPos, juce::Slider::SliderStyle style, juce::Slider& s)
{
    juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, pos, minPos, maxPos, style, s);
    // The live ring's form on a fader (26.09.2026): a bright tick where the level plays, joined to the cap by a line.
    const juce::var live = s.getProperties()["live"];
    if (live.isVoid() || style != juce::Slider::LinearVertical) return;
    const float lp = static_cast<float>(s.getPositionOfValue(juce::jlimit(s.getMinimum(), s.getMaximum(), static_cast<double>(live))));
    if (std::fabs(lp - pos) < 1.5f) return;
    const float cx = static_cast<float>(x) + 0.5f * static_cast<float>(width);
    g.setColour(phosui::text.withAlpha(0.85f));
    g.drawLine(cx, juce::jmin(pos, lp), cx, juce::jmax(pos, lp), 2.0f);
    g.fillRoundedRectangle(juce::Rectangle<float>(14.0f, 3.0f).withCentre({ cx, lp }), 1.5f);
}

void PhospheneLookAndFeel::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    using namespace phosui;
    const juce::Rectangle<float> r = b.getLocalBounds().toFloat().reduced(1.0f);
    const bool on = b.getToggleState();
    const juce::Colour lit = b.findColour(juce::ToggleButton::tickColourId);   // the page's colour (EditorLayout.cpp)
    g.setColour(on ? lit.withAlpha(0.28f) : group.brighter(highlighted ? 0.12f : 0.04f));
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(on ? lit : edge);
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
    // A tab carries its page's family colour (PluginEditor.cpp): a stripe along its foot, its frame when open.
    const juce::var stripe = b.getProperties()["stripe"];
    const juce::Colour own = stripe.isVoid() ? accent : juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(stripe)));
    const bool open = b.getToggleState();
    g.setColour(open && !stripe.isVoid() ? own.withAlpha(0.22f) : background.brighter(down ? 0.22f : (highlighted ? 0.12f : 0.0f)));
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(open ? own : edge);
    g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f);
    if (!stripe.isVoid()) {
        g.setColour(own.withAlpha(open ? 1.0f : 0.55f));
        g.fillRoundedRectangle(r.withTop(r.getBottom() - 2.5f).reduced(5.0f, 0.0f), 1.2f);
    }
}

juce::Font PhospheneLookAndFeel::getLabelFont(juce::Label& l) { return phosui::body(juce::jlimit(9.0f, 13.0f, l.getHeight() * 0.72f)); }
juce::Font PhospheneLookAndFeel::getComboBoxFont(juce::ComboBox& b) { return phosui::body(juce::jlimit(9.0f, 12.0f, b.getHeight() * 0.52f)); }
juce::Font PhospheneLookAndFeel::getTextButtonFont(juce::TextButton&, int h) { return phosui::body(juce::jlimit(9.0f, 13.0f, h * 0.5f)); }
juce::Font PhospheneLookAndFeel::getPopupMenuFont() { return phosui::body(13.0f); }
