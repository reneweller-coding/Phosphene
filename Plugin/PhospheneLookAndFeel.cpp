/**
 * @file PhospheneLookAndFeel.cpp
 * @brief Implementation of the editor's palette, knob, toggle and combo box.
 * @author Rene Weller
 */
#include "PhospheneLookAndFeel.h"
#include "PhospheneHelpData.h"
#include <cmath>

namespace phosui {

juce::Colour partColour(int index)
{
    // In tab order (PluginEditor.h): Set, Arrange | Kick, Bass, Percussion | Acid | Lead, Counter, Arp, Stab |
    // Pad, Drone, SFX / FX | Field | Mixer, Perform, Gallery. See the palette's note in the header.
    static const juce::uint32 tabs[] = {
        0xffb9b3d6, 0xffc4bde4,                           // the set: lavender
        0xffff2e97, 0xffff4fa8, 0xffff6fbd,               // the low end: hot magenta
        0xffb8ff3c,                                       // the acid: acid green
        0xff22e4ff, 0xff3cc8ff, 0xff22f0d0, 0xff62b4ff,   // the lines: UV cyan
        0xff9b6bff, 0xff7d5cff, 0xffc46bff,               // the space: electric violet
        0xffffb347,                                       // the field recordings: amber, the one warm colour (27.09.2026)
        0xffb9b3d6, 0xffc4bde4, 0xffaea8cc,               // mixer, perform, gallery: lavender
    };
    const int n = static_cast<int>(sizeof(tabs) / sizeof(tabs[0]));
    return juce::Colour(tabs[((index % n) + n) % n]);
}

juce::Colour partColourOf(int part)
{
    // Score.h's parts run Kick .. Sfx in the tabs' order from TabKick (2); the bed and the voices live on the SFX tab,
    // the field recordings on a tab of their own after it (13).
    return partColour(part <= 10 ? part + 2 : part == 13 ? 13 : 12);
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

namespace {
/**
 * @brief The frame's families in Phosphene's fluorescent tones (the same hues as in every generator).
 * @param[in] f  the family
 * @return its tone
 */
juce::Colour familyTone(frame::FamilyE f)
{
    switch (f) {
    case frame::FamilyE::SOURCE:   return juce::Colour(0xffffc247);   // gold
    case frame::FamilyE::FILTER:   return juce::Colour(0xffff7a45);   // orange
    case frame::FamilyE::ENVELOPE: return juce::Colour(0xff8dff5a);   // acid green
    case frame::FamilyE::MOTION:   return juce::Colour(0xff22e4ff);   // UV cyan
    default:                      return juce::Colour(0xff8c8dff);   // UV blue
    }
}
} // namespace

juce::Colour groupColour(const juce::String& title, juce::Colour page)
{
    const juce::String t = title.toLowerCase();
    auto has = [&t](std::initializer_list<const char*> words) {
        for (const char* w : words) if (t.contains(w)) return true;
        return false;
    };
    using F = frame::FamilyE;
    // The order matters: "Filter Envelope" is an envelope, "Wavetable Motion" moves.
    if (has({ "envelope", "amp", "punch" })) return familyTone(F::ENVELOPE);
    if (has({ "lfo", "mod", "matrix", "gate", "vibrato", "tremolo", "motion", "stutter", "glide", "psy fx", "voice fx" })) return familyTone(F::MOTION);
    if (has({ "filter", "tone", "eq" })) return familyTone(F::FILTER);
    if (has({ "delay", "space", "reverb", "room", "hall", "send", "output", "level", "sidechain", "image", "compressor", "gain", "monitor" }))
        return familyTone(F::SPACE);
    if (has({ "osc", "wave", "fm", "noise", "click", "body", "engine", "sample", "layer", "sub", "pitch", "fundamental", "harmonic" }))
        return familyTone(F::SOURCE);
    return page;
}

const frame::Skin& skin()
{
    static const frame::Skin s = [] {
        frame::Skin k;
        k.m_name = "Phosphene";
        k.m_bg = Bg0;
        k.m_panel = Card.withAlpha(0.62f);   // the phosphenes show faintly through a page
        k.m_group = GroupColour;
        k.m_raised = Card.brighter(0.12f);
        k.m_edge = Edge;
        k.m_ink = Text;
        k.m_dim = Dim;
        k.m_faint = Faint;
        k.m_accent = Accent;
        k.m_onset = Warm;
        k.m_good = Green;
        k.m_bad = Red;
        for (int f = 0; f < 5; ++f) k.m_families[f] = familyTone(static_cast<frame::FamilyE>(f));
        k.m_decks[0] = juce::Colour(0xffff2e97);
        k.m_decks[1] = juce::Colour(0xff22e4ff);
        k.m_decks[2] = juce::Colour(0xff9b6bff);
        k.m_radius = 5.0f;
        k.m_tracking = 0.3f;
        k.m_titleBold = true;
        k.m_glow = true;
        k.m_valueInKnob = true;            // six hundred knobs on ten pages: the value inside the ring
        k.m_backdropData = PhospheneHelpData::backdrop_jpg;
        k.m_backdropSize = PhospheneHelpData::backdrop_jpgSize;
        k.m_backdropTop = 0.9f;
        k.m_backdropPage = 0.6f;
        k.m_logo = [](juce::Graphics& g, juce::Rectangle<float> r) {   // from the image cache: no image outlives JUCE
            g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
            g.drawImage(juce::ImageCache::getFromMemory(PhospheneHelpData::iconsmall_png, PhospheneHelpData::iconsmall_pngSize), r,
                        juce::RectanglePlacement::centred);
        };
        return k;
    }();
    return s;
}

} // namespace phosui

PhospheneLookAndFeel::PhospheneLookAndFeel() : frame::LookAndFeel(phosui::skin())
{
    using namespace phosui;
    // What the frame does not set, as Phosphene had it.
    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::rotarySliderOutlineColourId, Edge);
    setColour(juce::Slider::textBoxTextColourId, Text);
    setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    setColour(juce::ToggleButton::textColourId, Text);
    setColour(juce::TextEditor::backgroundColourId, Bg0);
}

void PhospheneLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& background,
                                                bool highlighted, bool down)
{
    using namespace phosui;
    // A button with a family stripe (the percussion lanes, the sub-tabs) carries its colour along its foot; any other is the
    // frame's.
    const juce::var stripe = b.getProperties()["stripe"];
    if (stripe.isVoid()) {
        frame::LookAndFeel::drawButtonBackground(g, b, background, highlighted, down);
        return;
    }
    const juce::Rectangle<float> r = b.getLocalBounds().toFloat().reduced(1.0f);
    const juce::Colour own(static_cast<juce::uint32>(static_cast<juce::int64>(stripe)));
    const bool open = b.getToggleState();
    g.setColour(open ? own.withAlpha(0.22f) : background.brighter(down ? 0.22f : (highlighted ? 0.12f : 0.0f)));
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(open ? own : Edge);
    g.drawRoundedRectangle(r.reduced(0.5f), 5.0f, 1.0f);
    g.setColour(own.withAlpha(open ? 1.0f : 0.55f));
    g.fillRoundedRectangle(r.withTop(r.getBottom() - 2.5f).reduced(5.0f, 0.0f), 1.2f);
}

juce::Font PhospheneLookAndFeel::getLabelFont(juce::Label& l) { return phosui::body(juce::jlimit(9.0f, 13.0f, l.getHeight() * 0.72f)); }
juce::Font PhospheneLookAndFeel::getComboBoxFont(juce::ComboBox& b) { return phosui::body(juce::jlimit(9.0f, 12.0f, b.getHeight() * 0.52f)); }
juce::Font PhospheneLookAndFeel::getTextButtonFont(juce::TextButton&, int h) { return phosui::body(juce::jlimit(9.0f, 13.0f, h * 0.5f)); }
juce::Font PhospheneLookAndFeel::getPopupMenuFont() { return phosui::body(13.0f); }
