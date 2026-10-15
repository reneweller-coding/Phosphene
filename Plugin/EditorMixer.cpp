/**
 * @file EditorMixer.cpp
 * @brief The mixer page's console (EditorMixer.h).
 */
#include "EditorMixer.h"
#include "PhospheneLookAndFeel.h"
#include "PluginEditor.h"
#include <cmath>

using namespace phos;

namespace phosui {

namespace {

constexpr float TopDb = 6.0f;       ///< the meter's top
constexpr float FloorDb = -60.0f;   ///< and its floor
constexpr double HoldSeconds = 1.5;   ///< how long the held peak stays, s
constexpr double FallDbPerSecond = 20.0;   ///< how fast it falls then, dB per second
constexpr double RmsRelease = 0.3;  ///< seconds for the RMS bar to fall by 1/e of its distance

/** @brief A linear level in dB, -100 for silence. */
float toDb(float linear) { return linear > 1.0e-5f ? 20.0f * std::log10(linear) : -100.0f; }

/** @brief What one strip binds: its fader, its mute and its knobs (key, name under the knob). */
struct StripSpec {
    const char* m_name;   ///< the strip's name
    int m_tab;   ///< the page whose colour it takes
    const char* m_level;   ///< its fader's key
    const char* m_mute;   ///< its mute's key
    std::vector<std::pair<const char*, const char*>> m_knobs;   ///< its knobs: key, name under the knob
};

/** @brief The strips in phos::Part order. The kick and the bass have no strip level of their own: their fader is the synth's. */
std::vector<StripSpec> stripSpecs()
{
    static const char* const Poly[] = { "lead", "counter", "arp", "stab", "pad", "drone" };
    static const char* const PolyName[] = { "Lead", "Counter", "Arp", "Stab", "Pad", "Drone" };
    // mix.<v>_level, mix.<v>_mute, <v>.pan, <v>.room_send, <v>.hall_send, <v>.delay_send, <v>.duck for the six
    // voices; built once and never touched again, so the specs may point into it.
    static std::vector<std::string> keys;
    if (keys.empty()) {
        keys.reserve(6 * 7);
        for (const char* v : Poly) {
            const std::string s(v);
            keys.push_back("mix." + s + "_level");
            keys.push_back("mix." + s + "_mute");
            keys.push_back(s + ".pan");
            keys.push_back(s + ".room_send");
            keys.push_back(s + ".hall_send");
            keys.push_back(s + ".delay_send");
            keys.push_back(s + ".duck");
        }
    }
    std::vector<StripSpec> out;
    out.push_back({ "Kick", TAB_KICK, "kick.level", "mix.kick_mute", {} });
    out.push_back({ "Bass", TAB_BASS, "bass.level", "mix.bass_mute", {} });
    out.push_back({ "Perc", TAB_PERC, "mix.perc_level", "mix.perc_mute", { { "mix.perc_room", "Room" }, { "mix.perc_hall", "Hall" } } });
    out.push_back({ "Acid", TAB_ACID, "mix.acid_level", "mix.acid_mute",
                    { { "acid.room_send", "Room" }, { "acid.hall_send", "Hall" }, { "acid.delay_send", "Delay" }, { "acid.duck", "Duck" } } });
    for (int i = 0; i < 6; ++i) {
        const std::string* k = &keys[static_cast<size_t>(7 * i)];
        out.push_back({ PolyName[i], TAB_LEAD + i, k[0].c_str(), k[1].c_str(),
                        { { k[2].c_str(), "Pan" }, { k[3].c_str(), "Room" }, { k[4].c_str(), "Hall" }, { k[5].c_str(), "Delay" },
                          { k[6].c_str(), "Duck" } } });
    }
    out.push_back({ "SFX", TAB_FX, "mix.sfx_level", "mix.sfx_mute", { { "sfx.room_send", "Room" }, { "sfx.hall_send", "Hall" }, { "sfx.duck", "Duck" } } });
    out.push_back({ "Bed", TAB_FX, "mix.texture_level", "mix.texture_mute",
                    { { "texture.room_send", "Room" }, { "texture.hall_send", "Hall" }, { "texture.fx_send", "FX" }, { "texture.duck", "Duck" } } });
    out.push_back({ "Voices", TAB_FX, "mix.vocal_level", "mix.vocal_mute",
                    { { "vocal.hall_send", "Hall" }, { "vocal.fx_send", "FX" }, { "vocal.throw_send", "Throw" }, { "vocal.duck", "Duck" } } });
    out.push_back({ "Field", TAB_FIELD, "mix.field_level", "mix.field_mute",
                    { { "field.pan", "Pan" }, { "field.room_send", "Room" }, { "field.hall_send", "Hall" }, { "field.duck", "Duck" } } });
    return out;
}

} // namespace

// ==================================================================== MixerStrip

MixerStrip::MixerStrip(PhospheneProcessor& proc, const juce::String& name, juce::Colour colour, const char* levelKey,
                       const char* muteKey, const std::vector<std::pair<const char*, const char*>>& knobKeys)
    : m_name(name), m_colour(colour)
{
    const ParamStore& p = proc.params();
    const int levelId = p.find(levelKey), muteId = p.find(muteKey);
    m_fader = std::make_unique<juce::Slider>(juce::Slider::LinearVertical, juce::Slider::NoTextBox);
    m_fader->setColour(juce::Slider::trackColourId, m_colour.withAlpha(0.8f));
    m_fader->setColour(juce::Slider::thumbColourId, Text);
    m_fader->setColour(juce::Slider::backgroundColourId, Bg0);
    m_fader->setTextValueSuffix(" dB");
    m_fader->setTooltip(juce::String(levelKey));
    m_fader->setPopupDisplayEnabled(true, true, nullptr);
    addAndMakeVisible(*m_fader);
    if (StoreParameter* sp = levelId >= 0 ? proc.parameterFor(levelId) : nullptr) {
        m_faderLink = std::make_unique<juce::SliderParameterAttachment>(*sp, *m_fader);
        m_params.push_back(levelId);
        m_controls.emplace_back(m_fader.get(), levelId);
    }
    m_mute = std::make_unique<juce::TextButton>("M");
    m_mute->setClickingTogglesState(true);
    m_mute->setColour(juce::TextButton::buttonOnColourId, Red.withAlpha(0.85f));
    m_mute->setTooltip("Mute (" + juce::String(muteKey) + ")");
    addAndMakeVisible(*m_mute);
    if (StoreParameter* sp = muteId >= 0 ? proc.parameterFor(muteId) : nullptr) {
        m_muteLink = std::make_unique<juce::ButtonParameterAttachment>(*sp, *m_mute);
        m_params.push_back(muteId);
        m_controls.emplace_back(m_mute.get(), muteId);
    }
    for (const auto& [key, label] : knobKeys) {
        const int id = p.find(key);
        StoreParameter* sp = id >= 0 ? proc.parameterFor(id) : nullptr;
        if (sp == nullptr) continue;
        auto k = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        const ParamDesc& d = p.desc(id);
        if (d.m_minValue < -1.0e-6f && d.m_maxValue > 1.0e-6f) k->getProperties().set("bipolar", true);
        k->setColour(juce::Slider::rotarySliderFillColourId, m_colour);   // the channel's family colour (26.09.2026)
        k->setTooltip(juce::String(key));
        k->setPopupDisplayEnabled(true, true, nullptr);
        addAndMakeVisible(*k);
        m_knobLinks.push_back(std::make_unique<juce::SliderParameterAttachment>(*sp, *k));
        auto l = std::make_unique<juce::Label>(juce::String(), label);
        l->setJustificationType(juce::Justification::centred);
        l->setColour(juce::Label::textColourId, Dim);
        l->setFont(body(10.0f));
        l->setInterceptsMouseClicks(false, false);
        addAndMakeVisible(*l);
        m_params.push_back(id);
        m_controls.emplace_back(k.get(), id);
        m_knobs.push_back(std::move(k));
        m_knobNames.push_back(std::move(l));
    }
}

void MixerStrip::meter(float peak, float rms, double seconds)
{
    const float pDb = toDb(peak), rDb = toDb(rms);
    // The RMS bar rises at once and falls with RmsRelease; the peak line holds, then falls.
    const float a = static_cast<float>(std::exp(-seconds / RmsRelease));
    const float fallen = rDb >= m_rmsDb ? rDb : juce::jmax(rDb, FloorDb - 1.0f + (m_rmsDb - (FloorDb - 1.0f)) * a);
    m_rmsDb = fallen;
    if (pDb >= m_holdDb) { m_holdDb = pDb; m_holdAge = 0.0; }
    else {
        m_holdAge += seconds;
        if (m_holdAge > HoldSeconds) m_holdDb = juce::jmax(pDb, m_holdDb - static_cast<float>(FallDbPerSecond * seconds));
    }
    repaint(m_meterArea.expanded(2).withBottom(getHeight()));
}

void MixerStrip::resized()
{
    auto r = getLocalBounds().reduced(3);
    r.removeFromTop(18);   // the name
    // The knobs two to a row, so that a strip of five stays short enough for the fader to have room.
    const int cols = 2, kw = r.getWidth() / cols, kh = 30, lh = 11;
    const int rows = juce::jmax(knobRows(), m_knobRowsShown);
    auto knobArea = r.removeFromTop(rows * (kh + lh));
    for (size_t i = 0; i < m_knobs.size(); ++i) {
        const int row = static_cast<int>(i) / cols, col = static_cast<int>(i) % cols;
        // An odd last knob stands in the middle.
        const bool alone = static_cast<int>(i) == static_cast<int>(m_knobs.size()) - 1 && m_knobs.size() % 2 == 1;
        const int x = knobArea.getX() + (alone ? (knobArea.getWidth() - kw) / 2 : col * kw);
        const int y = knobArea.getY() + row * (kh + lh);
        m_knobs[i]->setBounds(x, y, kw, kh);
        m_knobNames[i]->setBounds(x - 4, y + kh - 1, kw + 8, lh);
    }
    r.removeFromTop(4);
    m_mute->setBounds(r.removeFromTop(20).reduced(6, 0));
    r.removeFromTop(4);
    r.removeFromBottom(15);   // the peak readout
    const int half = r.getWidth() / 2;
    m_fader->setBounds(r.removeFromLeft(half));
    m_meterArea = r.reduced(4, 6);
}

void MixerStrip::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(GroupColour.brighter(0.04f));
    g.fillRoundedRectangle(b, 4.0f);
    g.setColour(m_colour);
    g.fillRoundedRectangle(b.withHeight(3.0f), 1.5f);
    g.setFont(title(13.0f));
    g.drawFittedText(m_name, getLocalBounds().withHeight(20).reduced(2, 0), juce::Justification::centred, 1);

    // The meter: a dB scale from FloorDb to TopDb, the RMS bar, the held peak, 0 dBFS marked.
    const auto m = m_meterArea.toFloat();
    g.setColour(Bg0);
    g.fillRect(m);
    auto yOf = [&](float db) {
        const float t = (juce::jlimit(FloorDb, TopDb, db) - FloorDb) / (TopDb - FloorDb);
        return m.getBottom() - t * m.getHeight();
    };
    const float top = yOf(m_rmsDb);
    if (m_rmsDb > FloorDb) {
        const float y6 = yOf(-6.0f), y0 = yOf(0.0f);
        g.setColour(Green.withAlpha(0.85f));
        g.fillRect(juce::Rectangle<float>(m.getX(), juce::jmax(top, y6), m.getWidth(), m.getBottom() - juce::jmax(top, y6)));
        if (top < y6) {
            g.setColour(Warm.withAlpha(0.9f));
            g.fillRect(juce::Rectangle<float>(m.getX(), juce::jmax(top, y0), m.getWidth(), y6 - juce::jmax(top, y0)));
        }
        if (top < y0) {
            g.setColour(Red);
            g.fillRect(juce::Rectangle<float>(m.getX(), top, m.getWidth(), y0 - top));
        }
    }
    if (m_holdDb > FloorDb) {
        g.setColour(m_holdDb > 0.0f ? Red : Text);
        g.fillRect(m.getX(), yOf(m_holdDb) - 1.0f, m.getWidth(), 2.0f);
    }
    g.setColour(Faint);
    for (float db : { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f }) g.fillRect(m.getRight() + 1.0f, yOf(db), 3.0f, 1.0f);
    g.setColour(Edge);
    g.drawRect(m, 1.0f);
    // The held peak in figures under the meter.
    g.setColour(m_holdDb > 0.0f ? Red : Dim);
    g.setFont(body(10.5f));
    const juce::String readout = m_holdDb > -99.0f ? juce::String(m_holdDb, 1) : juce::String("-inf");
    g.drawFittedText(readout, getLocalBounds().removeFromBottom(17).reduced(2, 1), juce::Justification::centred, 1);
}

// ==================================================================== MixerConsole

MixerConsole::MixerConsole(PhospheneProcessor& proc) : m_proc(proc)
{
    for (const StripSpec& s : stripSpecs()) {
        auto strip = std::make_unique<MixerStrip>(proc, s.m_name, partColour(s.m_tab), s.m_level, s.m_mute, s.m_knobs);
        addAndMakeVisible(*strip);
        m_strips.push_back(std::move(strip));
    }
    m_lastPoll = juce::Time::getMillisecondCounterHiRes() * 0.001;
    startTimerHz(30);
}

MixerConsole::~MixerConsole() { stopTimer(); }

std::vector<int> MixerConsole::params() const
{
    std::vector<int> out;
    for (const auto& s : m_strips) out.insert(out.end(), s->params().begin(), s->params().end());
    return out;
}

void MixerConsole::timerCallback()
{
    float peak[NumParts], rms[NumParts];
    m_proc.takeChannelMeters(peak, rms);
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const double dt = juce::jlimit(0.001, 0.5, now - m_lastPoll);
    m_lastPoll = now;
    // The strips follow the readings whether the page is on screen or not (a repaint of a hidden strip costs
    // nothing), so a page that comes into view -- or into a screenshot -- shows the level of now.
    for (size_t i = 0; i < m_strips.size() && i < static_cast<size_t>(NumParts); ++i) m_strips[i]->meter(peak[i], rms[i], dt);
    // The live ring on the faders and the strip knobs (26.09.2026, the user: "Stellt der Composer auch ... die Mixer-Werte
    // ein?"): a fader stays where the user put it, and the level the level match plays it at shows beside it.
    for (auto& s : m_strips)
        for (const auto& [comp, id] : s->controls())
            if (auto* sl = dynamic_cast<juce::Slider*>(comp)) showLive(*sl, m_proc.playedValue(id));
}

void MixerConsole::resized()
{
    const int n = static_cast<int>(m_strips.size());
    if (n == 0) return;
    const int gap = 4, w = (getWidth() - gap * (n - 1)) / n;
    int rows = 0;
    for (const auto& s : m_strips) rows = juce::jmax(rows, s->knobRows());
    for (int i = 0; i < n; ++i) {
        m_strips[static_cast<size_t>(i)]->setKnobRows(rows);
        m_strips[static_cast<size_t>(i)]->setBounds(i * (w + gap), 0, w, getHeight());
    }
}

void MixerConsole::paint(juce::Graphics&) {}

} // namespace phosui
