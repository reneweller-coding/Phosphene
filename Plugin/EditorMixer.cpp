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

constexpr float kTopDb = 6.0f;       ///< the meter's top
constexpr float kFloorDb = -60.0f;   ///< and its floor
constexpr double kHoldSeconds = 1.5;
constexpr double kFallDbPerSecond = 20.0;
constexpr double kRmsRelease = 0.3;  ///< seconds for the RMS bar to fall by 1/e of its distance

float toDb(float linear) { return linear > 1.0e-5f ? 20.0f * std::log10(linear) : -100.0f; }

/** @brief What one strip binds: its fader, its mute and its knobs (key, name under the knob). */
struct StripSpec {
    const char* name;
    int tab;   ///< the page whose colour it takes
    const char* level;
    const char* mute;
    std::vector<std::pair<const char*, const char*>> knobs;
};

/** @brief The strips in phos::Part order. The kick and the bass have no strip level of their own: their fader is the synth's. */
std::vector<StripSpec> stripSpecs()
{
    static const char* const kPoly[] = { "lead", "counter", "arp", "stab", "pad", "drone" };
    static const char* const kPolyName[] = { "Lead", "Counter", "Arp", "Stab", "Pad", "Drone" };
    // mix.<v>_level, mix.<v>_mute, <v>.pan, <v>.room_send, <v>.hall_send, <v>.delay_send, <v>.duck for the six
    // voices; built once and never touched again, so the specs may point into it.
    static std::vector<std::string> keys;
    if (keys.empty()) {
        keys.reserve(6 * 7);
        for (const char* v : kPoly) {
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
    out.push_back({ "Kick", TabKick, "kick.level", "mix.kick_mute", {} });
    out.push_back({ "Bass", TabBass, "bass.level", "mix.bass_mute", {} });
    out.push_back({ "Perc", TabPerc, "mix.perc_level", "mix.perc_mute", { { "mix.perc_room", "Room" }, { "mix.perc_hall", "Hall" } } });
    out.push_back({ "Acid", TabAcid, "mix.acid_level", "mix.acid_mute",
                    { { "acid.room_send", "Room" }, { "acid.hall_send", "Hall" }, { "acid.delay_send", "Delay" }, { "acid.duck", "Duck" } } });
    for (int i = 0; i < 6; ++i) {
        const std::string* k = &keys[static_cast<size_t>(7 * i)];
        out.push_back({ kPolyName[i], TabLead + i, k[0].c_str(), k[1].c_str(),
                        { { k[2].c_str(), "Pan" }, { k[3].c_str(), "Room" }, { k[4].c_str(), "Hall" }, { k[5].c_str(), "Delay" },
                          { k[6].c_str(), "Duck" } } });
    }
    out.push_back({ "SFX", TabFx, "mix.sfx_level", "mix.sfx_mute", { { "sfx.room_send", "Room" }, { "sfx.hall_send", "Hall" }, { "sfx.duck", "Duck" } } });
    out.push_back({ "Bed", TabFx, "mix.texture_level", "mix.texture_mute",
                    { { "texture.room_send", "Room" }, { "texture.hall_send", "Hall" }, { "texture.fx_send", "FX" }, { "texture.duck", "Duck" } } });
    out.push_back({ "Voices", TabFx, "mix.vocal_level", "mix.vocal_mute",
                    { { "vocal.hall_send", "Hall" }, { "vocal.fx_send", "FX" }, { "vocal.throw_send", "Throw" }, { "vocal.duck", "Duck" } } });
    return out;
}

} // namespace

// ==================================================================== MixerStrip

MixerStrip::MixerStrip(PhospheneProcessor& proc, const juce::String& name, juce::Colour colour, const char* levelKey,
                       const char* muteKey, const std::vector<std::pair<const char*, const char*>>& knobKeys)
    : name_(name), colour_(colour)
{
    const ParamStore& p = proc.params();
    const int levelId = p.find(levelKey), muteId = p.find(muteKey);
    fader_ = std::make_unique<juce::Slider>(juce::Slider::LinearVertical, juce::Slider::NoTextBox);
    fader_->setColour(juce::Slider::trackColourId, colour_.withAlpha(0.8f));
    fader_->setColour(juce::Slider::thumbColourId, text);
    fader_->setColour(juce::Slider::backgroundColourId, bg0);
    fader_->setTextValueSuffix(" dB");
    fader_->setTooltip(juce::String(levelKey));
    fader_->setPopupDisplayEnabled(true, true, nullptr);
    addAndMakeVisible(*fader_);
    if (StoreParameter* sp = levelId >= 0 ? proc.parameterFor(levelId) : nullptr) {
        faderLink_ = std::make_unique<juce::SliderParameterAttachment>(*sp, *fader_);
        params_.push_back(levelId);
        controls_.emplace_back(fader_.get(), levelId);
    }
    mute_ = std::make_unique<juce::TextButton>("M");
    mute_->setClickingTogglesState(true);
    mute_->setColour(juce::TextButton::buttonOnColourId, red.withAlpha(0.85f));
    mute_->setTooltip("Mute (" + juce::String(muteKey) + ")");
    addAndMakeVisible(*mute_);
    if (StoreParameter* sp = muteId >= 0 ? proc.parameterFor(muteId) : nullptr) {
        muteLink_ = std::make_unique<juce::ButtonParameterAttachment>(*sp, *mute_);
        params_.push_back(muteId);
        controls_.emplace_back(mute_.get(), muteId);
    }
    for (const auto& [key, label] : knobKeys) {
        const int id = p.find(key);
        StoreParameter* sp = id >= 0 ? proc.parameterFor(id) : nullptr;
        if (sp == nullptr) continue;
        auto k = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        const ParamDesc& d = p.desc(id);
        if (d.minValue < -1.0e-6f && d.maxValue > 1.0e-6f) k->getProperties().set("bipolar", true);
        k->setColour(juce::Slider::rotarySliderFillColourId, colour_);   // the channel's family colour (26.09.2026)
        k->setTooltip(juce::String(key));
        k->setPopupDisplayEnabled(true, true, nullptr);
        addAndMakeVisible(*k);
        knobLinks_.push_back(std::make_unique<juce::SliderParameterAttachment>(*sp, *k));
        auto l = std::make_unique<juce::Label>(juce::String(), label);
        l->setJustificationType(juce::Justification::centred);
        l->setColour(juce::Label::textColourId, dim);
        l->setFont(body(10.0f));
        l->setInterceptsMouseClicks(false, false);
        addAndMakeVisible(*l);
        params_.push_back(id);
        controls_.emplace_back(k.get(), id);
        knobs_.push_back(std::move(k));
        knobNames_.push_back(std::move(l));
    }
}

void MixerStrip::meter(float peak, float rms, double seconds)
{
    const float pDb = toDb(peak), rDb = toDb(rms);
    // The RMS bar rises at once and falls with kRmsRelease; the peak line holds, then falls.
    const float a = static_cast<float>(std::exp(-seconds / kRmsRelease));
    const float fallen = rDb >= rmsDb_ ? rDb : juce::jmax(rDb, kFloorDb - 1.0f + (rmsDb_ - (kFloorDb - 1.0f)) * a);
    rmsDb_ = fallen;
    if (pDb >= holdDb_) { holdDb_ = pDb; holdAge_ = 0.0; }
    else {
        holdAge_ += seconds;
        if (holdAge_ > kHoldSeconds) holdDb_ = juce::jmax(pDb, holdDb_ - static_cast<float>(kFallDbPerSecond * seconds));
    }
    repaint(meterArea_.expanded(2).withBottom(getHeight()));
}

void MixerStrip::resized()
{
    auto r = getLocalBounds().reduced(3);
    r.removeFromTop(18);   // the name
    // The knobs two to a row, so that a strip of five stays short enough for the fader to have room.
    const int cols = 2, kw = r.getWidth() / cols, kh = 30, lh = 11;
    const int rows = juce::jmax(knobRows(), knobRowsShown_);
    auto knobArea = r.removeFromTop(rows * (kh + lh));
    for (size_t i = 0; i < knobs_.size(); ++i) {
        const int row = static_cast<int>(i) / cols, col = static_cast<int>(i) % cols;
        // An odd last knob stands in the middle.
        const bool alone = static_cast<int>(i) == static_cast<int>(knobs_.size()) - 1 && knobs_.size() % 2 == 1;
        const int x = knobArea.getX() + (alone ? (knobArea.getWidth() - kw) / 2 : col * kw);
        const int y = knobArea.getY() + row * (kh + lh);
        knobs_[i]->setBounds(x, y, kw, kh);
        knobNames_[i]->setBounds(x - 4, y + kh - 1, kw + 8, lh);
    }
    r.removeFromTop(4);
    mute_->setBounds(r.removeFromTop(20).reduced(6, 0));
    r.removeFromTop(4);
    r.removeFromBottom(15);   // the peak readout
    const int half = r.getWidth() / 2;
    fader_->setBounds(r.removeFromLeft(half));
    meterArea_ = r.reduced(4, 6);
}

void MixerStrip::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(group.brighter(0.04f));
    g.fillRoundedRectangle(b, 4.0f);
    g.setColour(colour_);
    g.fillRoundedRectangle(b.withHeight(3.0f), 1.5f);
    g.setFont(title(13.0f));
    g.drawFittedText(name_, getLocalBounds().withHeight(20).reduced(2, 0), juce::Justification::centred, 1);

    // The meter: a dB scale from kFloorDb to kTopDb, the RMS bar, the held peak, 0 dBFS marked.
    const auto m = meterArea_.toFloat();
    g.setColour(bg0);
    g.fillRect(m);
    auto yOf = [&](float db) {
        const float t = (juce::jlimit(kFloorDb, kTopDb, db) - kFloorDb) / (kTopDb - kFloorDb);
        return m.getBottom() - t * m.getHeight();
    };
    const float top = yOf(rmsDb_);
    if (rmsDb_ > kFloorDb) {
        const float y6 = yOf(-6.0f), y0 = yOf(0.0f);
        g.setColour(green.withAlpha(0.85f));
        g.fillRect(juce::Rectangle<float>(m.getX(), juce::jmax(top, y6), m.getWidth(), m.getBottom() - juce::jmax(top, y6)));
        if (top < y6) {
            g.setColour(warm.withAlpha(0.9f));
            g.fillRect(juce::Rectangle<float>(m.getX(), juce::jmax(top, y0), m.getWidth(), y6 - juce::jmax(top, y0)));
        }
        if (top < y0) {
            g.setColour(red);
            g.fillRect(juce::Rectangle<float>(m.getX(), top, m.getWidth(), y0 - top));
        }
    }
    if (holdDb_ > kFloorDb) {
        g.setColour(holdDb_ > 0.0f ? red : text);
        g.fillRect(m.getX(), yOf(holdDb_) - 1.0f, m.getWidth(), 2.0f);
    }
    g.setColour(faint);
    for (float db : { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f }) g.fillRect(m.getRight() + 1.0f, yOf(db), 3.0f, 1.0f);
    g.setColour(edge);
    g.drawRect(m, 1.0f);
    // The held peak in figures under the meter.
    g.setColour(holdDb_ > 0.0f ? red : dim);
    g.setFont(body(10.5f));
    const juce::String readout = holdDb_ > -99.0f ? juce::String(holdDb_, 1) : juce::String("-inf");
    g.drawFittedText(readout, getLocalBounds().removeFromBottom(17).reduced(2, 1), juce::Justification::centred, 1);
}

// ==================================================================== MixerConsole

MixerConsole::MixerConsole(PhospheneProcessor& proc) : proc_(proc)
{
    for (const StripSpec& s : stripSpecs()) {
        auto strip = std::make_unique<MixerStrip>(proc, s.name, partColour(s.tab), s.level, s.mute, s.knobs);
        addAndMakeVisible(*strip);
        strips_.push_back(std::move(strip));
    }
    lastPoll_ = juce::Time::getMillisecondCounterHiRes() * 0.001;
    startTimerHz(30);
}

MixerConsole::~MixerConsole() { stopTimer(); }

std::vector<int> MixerConsole::params() const
{
    std::vector<int> out;
    for (const auto& s : strips_) out.insert(out.end(), s->params().begin(), s->params().end());
    return out;
}

void MixerConsole::timerCallback()
{
    float peak[kNumParts], rms[kNumParts];
    proc_.takeChannelMeters(peak, rms);
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const double dt = juce::jlimit(0.001, 0.5, now - lastPoll_);
    lastPoll_ = now;
    // The strips follow the readings whether the page is on screen or not (a repaint of a hidden strip costs
    // nothing), so a page that comes into view -- or into a screenshot -- shows the level of now.
    for (size_t i = 0; i < strips_.size() && i < static_cast<size_t>(kNumParts); ++i) strips_[i]->meter(peak[i], rms[i], dt);
}

void MixerConsole::resized()
{
    const int n = static_cast<int>(strips_.size());
    if (n == 0) return;
    const int gap = 4, w = (getWidth() - gap * (n - 1)) / n;
    int rows = 0;
    for (const auto& s : strips_) rows = juce::jmax(rows, s->knobRows());
    for (int i = 0; i < n; ++i) {
        strips_[static_cast<size_t>(i)]->setKnobRows(rows);
        strips_[static_cast<size_t>(i)]->setBounds(i * (w + gap), 0, w, getHeight());
    }
}

void MixerConsole::paint(juce::Graphics&) {}

} // namespace phosui
