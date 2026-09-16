/**
 * @file PluginEditor.cpp
 * @brief The editor's frame: header, tab bar, lane bar, pages and the screenshot mode.
 */
#include "PluginEditor.h"
#include "phos/Composer.h"
#include <cmath>

using namespace phos;
using namespace phosui;

namespace {

/** @brief One titled slice of a module's descriptor table. */
struct Slice {
    const char* title;   ///< the group's title
    int first;           ///< first index in the module table
    int count;           ///< how many, -1 = to the end
    int columns;         ///< cell units per row
};

/** @brief The kick's table, grouped by what the parameters do. */
const Slice kKickSlices[] = {
    { "Pitch", kick::Engine, 7, 5 }, { "Amplitude", kick::AmpAttack, 3, 3 },
    { "Drive", kick::Drive, 2, 3 }, { "Click", kick::ClickLevel, 3, 3 }, { "Output", kick::Tone, 3, 3 },
};
/** @brief The bass's table. */
const Slice kBassSlices[] = {
    { "Oscillator", bass::Wave, 8, 5 }, { "Filter", bass::Cutoff, 7, 4 },
    { "Amplitude", bass::AmpAttack, 4, 4 }, { "Duck & Level", bass::DuckDepth, 4, 4 },
};
/** @brief One percussion lane's table. */
const Slice kPercSlices[] = {
    { "Lane", perc::Active, 3, 5 }, { "Source", perc::Pitch, 8, 4 }, { "Noise", perc::Noise, 4, 4 },
    { "Shape", perc::Decay, 6, 4 }, { "Output", perc::Level, 6, 4 },
};
/** @brief The acid voice's table. */
const Slice kAcidSlices[] = {
    { "Voice", acid::Wave, 10, 5 }, { "Squelch", acid::Squelch, 6, 4 },
    { "Delay", acid::DelaySend, 6, 4 }, { "Sends & Level", acid::RoomSend, 4, 4 },
};
/** @brief A polyphonic engine's table (lead, arp, pad). */
const Slice kPolySlices[] = {
    { "Oscillator", poly::Osc, 15, 5 }, { "Filter", poly::Cutoff, 7, 4 }, { "Amplitude", poly::AmpAttack, 6, 4 },
    { "Delay", poly::DelaySend, 6, 4 }, { "Sends", poly::RoomSend, 3, 3 },
    { "Trance Gate", poly::Gate, 7, 4 }, { "Level", poly::Level, 1, 2 },
};
/** @brief The send effects. */
const Slice kFxSlices[] = {
    { "Room", fx::RoomSize, 3, 3 }, { "Hall", fx::HallSize, 4, 4 }, { "Returns", fx::LowCut, 5, 5 },
};
/** @brief The mixer and the master. */
const Slice kMixSlices[] = { { "Channels", mix::KickMute, 17, 6 }, { "Sidechain", mix::DuckAttack, 3, 3 } };
const Slice kMasterSlices[] = {
    { "Gain", master::Gain, 3, 3 }, { "Compressor", master::CompThreshold, 5, 5 }, { "Output", master::MonoBass, 7, 4 },
};

template <size_t N>
void addSlices(ControlPage& page, PhospheneProcessor& proc, Module m, int instance, const Slice (&slices)[N], juce::Colour tint)
{
    for (const Slice& s : slices) page.addModuleGroup(proc, m, instance, s.title, tint, s.columns, s.first, s.count);
}

} // namespace

// ==================================================================== LoudnessDisplay

void LoudnessDisplay::update(const LoudnessReading& r, float compDb, float limitDb, float target)
{
    reading_ = r;
    comp_ = compDb;
    limit_ = limitDb;
    target_ = target;
    repaint();
}

void LoudnessDisplay::paint(juce::Graphics& g)
{
    const juce::Rectangle<int> r = getLocalBounds().reduced(4, 3);
    g.setColour(bg0.withAlpha(0.6f));
    g.fillRoundedRectangle(r.toFloat(), 5.0f);
    juce::Rectangle<int> area = r.reduced(7, 5);
    g.setColour(dim);
    g.setFont(title(10.0f));
    g.drawText("MASTER", area.removeFromTop(13), juce::Justification::topLeft, false);
    const int rowH = juce::jmax(9, (area.getHeight() - 14) / 3);

    // -50 .. 0 LUFS / dBTP on one scale, so the bars can be read against each other.
    auto bar = [&](juce::Rectangle<int> band, const juce::String& name, float value, juce::Colour colour, float over) {
        juce::Rectangle<int> row = band.reduced(0, 2);
        g.setColour(faint);
        g.setFont(phosui::body(9.5f));
        g.drawText(name, row.removeFromLeft(38), juce::Justification::centredLeft, false);
        const juce::Rectangle<int> num = row.removeFromRight(50);
        const float t = juce::jlimit(0.0f, 1.0f, (value + 50.0f) / 50.0f);
        g.setColour(edge);
        g.fillRoundedRectangle(row.toFloat(), 2.5f);
        g.setColour(value > over ? red : colour);
        g.fillRoundedRectangle(row.withWidth(juce::roundToInt(row.getWidth() * t)).toFloat(), 2.5f);
        g.setColour(text);
        g.drawText(value <= -99.0f ? juce::String("--") : juce::String(value, 1), num, juce::Justification::centredRight, false);
    };
    bar(area.removeFromTop(rowH), "Short", reading_.shortTerm, green, target_ + 3.0f);
    bar(area.removeFromTop(rowH), "Integr.", reading_.integrated, accent, target_ + 2.0f);
    bar(area.removeFromTop(rowH), "Peak", reading_.truePeak, green, -1.0f);
    g.setColour(faint);
    g.setFont(phosui::body(9.5f));
    g.drawText("comp " + juce::String(comp_, 1) + " dB   limit " + juce::String(limit_, 1) + " dB   target "
                   + juce::String(target_, 1) + " LUFS",
               area.removeFromTop(13), juce::Justification::centredLeft, false);
}

// ==================================================================== TrackDisplay

void TrackDisplay::paint(juce::Graphics& g)
{
    const juce::Rectangle<int> r = getLocalBounds().reduced(4, 3);
    g.setColour(bg0.withAlpha(0.6f));
    g.fillRoundedRectangle(r.toFloat(), 5.0f);
    juce::Rectangle<int> area = r.reduced(7, 5);
    g.setColour(dim);
    g.setFont(title(10.0f));
    g.drawText("TRACKS", area.removeFromTop(14), juce::Justification::topLeft, false);
    if (rows_.empty()) {
        g.setColour(faint);
        g.setFont(phosui::body(11.5f));
        g.drawText("planning...", area, juce::Justification::centred, false);
        return;
    }
    const int rowH = juce::jlimit(13, 20, area.getHeight() / juce::jmax(1, static_cast<int>(rows_.size())));
    for (const Row& row : rows_) {
        if (area.getHeight() < rowH) break;
        juce::Rectangle<int> line = area.removeFromTop(rowH);
        const bool here = bar_ >= row.bar && bar_ < row.bar + row.bars;
        if (here) {
            g.setColour(accent.withAlpha(0.18f));
            g.fillRoundedRectangle(line.toFloat(), 3.0f);
            // How far into the track the play head stands.
            const float t = row.bars > 0 ? juce::jlimit(0.0f, 1.0f, static_cast<float>(bar_ - row.bar) / row.bars) : 0.0f;
            g.setColour(accent);
            g.fillRect(line.getX() + juce::roundToInt(t * line.getWidth()), line.getY(), 2, line.getHeight());
        }
        g.setColour(here ? text : dim);
        g.setFont(phosui::body(11.5f));
        g.drawText(row.text, line.reduced(4, 0), juce::Justification::centredLeft, false);
    }
}

void TrackDisplay::mouseDown(const juce::MouseEvent& e)
{
    if (rows_.empty() || !onJump) return;
    const juce::Rectangle<int> area = getLocalBounds().reduced(11, 8).withTrimmedTop(14);
    const int rowH = juce::jlimit(13, 20, area.getHeight() / juce::jmax(1, static_cast<int>(rows_.size())));
    const int index = (e.y - area.getY()) / juce::jmax(1, rowH);
    if (index >= 0 && index < static_cast<int>(rows_.size())) onJump(rows_[static_cast<size_t>(index)].bar);
}

// ==================================================================== PhospheneEditor

const juce::StringArray& PhospheneEditor::tabNames()
{
    static const juce::StringArray names{ "Set", "Kick", "Bass", "Percussion", "Acid",
                                          "Lead", "Arp", "Pad", "SFX / FX", "Mixer / Master" };
    return names;
}

PhospheneEditor::PhospheneEditor(PhospheneProcessor& p) : juce::AudioProcessorEditor(&p), proc_(p)
{
    setLookAndFeel(&lnf_);
    content_.onPaint = [this](juce::Graphics& g) { paintContent(g); };
    content_.onResized = [this] { layoutContent(); };
    addAndMakeVisible(content_);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setViewedComponent(nullptr, false);
    content_.addAndMakeVisible(viewport_);

    for (int i = 0; i < tabNames().size(); ++i) {
        auto* b = tabButtons_.add(new juce::TextButton(tabNames()[i]));
        b->setClickingTogglesState(false);
        b->onClick = [this, i] { setTab(i); };
        content_.addAndMakeVisible(b);
    }
    for (int i = 0; i < kPercLanes; ++i) {
        auto* b = laneButtons_.add(new juce::TextButton(juce::String(i + 1)));
        b->onClick = [this, i] { setPercLane(i); };
        content_.addChildComponent(b);
    }
    buildPages();

    // The design size is the size at which no page has to scroll; the window opens scaled to fit
    // the screen, and dragging its corner only zooms (the aspect ratio is fixed).
    int tallest = 0, widest = 0;
    for (auto& page : pages_) if (page != nullptr) { tallest = juce::jmax(tallest, page->layout(designW_ - 30)); widest = juce::jmax(widest, page->minimumWidth()); }
    for (auto& page : percPages_) if (page != nullptr) { tallest = juce::jmax(tallest, page->layout(designW_ - 30)); widest = juce::jmax(widest, page->minimumWidth()); }
    designW_ = juce::jmax(designW_, widest + 30);
    designH_ = juce::jlimit(680, 1120, tallest + 64 + 34 + 30 + 16);

    setResizable(true, true);
    if (auto* con = getConstrainer()) {
        con->setFixedAspectRatio(static_cast<double>(designW_) / designH_);
        con->setSizeLimits(designW_ / 2, designH_ / 2, designW_ * 2, designH_ * 2);
    }
    float fit = 1.0f;
    if (auto* screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
        const juce::Rectangle<float> area = screen->userBounds;
        fit = juce::jlimit(0.4f, 1.0f, juce::jmin(area.getWidth() * 0.94f / designW_, area.getHeight() * 0.90f / designH_));
    }
    setSize(juce::roundToInt(designW_ * fit), juce::roundToInt(designH_ * fit));

    const int startTab = juce::SystemStats::getEnvironmentVariable("PHOS_TAB", "0").getIntValue();
    setTab(juce::jlimit(0, tabNames().size() - 1, startTab));
    runScreenshotMode();
    startTimerHz(12);
}

PhospheneEditor::~PhospheneEditor()
{
    viewport_.setViewedComponent(nullptr, false);
    setLookAndFeel(nullptr);
}

void PhospheneEditor::buildPages()
{
    pages_.resize(static_cast<size_t>(tabNames().size()));
    for (int t = 0; t < tabNames().size(); ++t) {
        if (t == 3) continue;   // percussion: one page per lane, built below
        auto page = std::make_unique<ControlPage>();
        const juce::Colour tint = partColour(t);
        switch (t) {
        case 0: break;   // the Set page is built in EditorSetTab.cpp, after this loop
        case 1: addSlices(*page, proc_, Module::Kick, 0, kKickSlices, tint); break;
        case 2: addSlices(*page, proc_, Module::Bass, 0, kBassSlices, tint); break;
        case 4: addSlices(*page, proc_, Module::Acid, 0, kAcidSlices, tint); break;
        case 5: addSlices(*page, proc_, Module::Poly, static_cast<int>(PolyInstance::Lead), kPolySlices, tint); break;
        case 6: addSlices(*page, proc_, Module::Poly, static_cast<int>(PolyInstance::Arp), kPolySlices, tint); break;
        case 7: addSlices(*page, proc_, Module::Poly, static_cast<int>(PolyInstance::Pad), kPolySlices, tint); break;
        case 8:
            page->addModuleGroup(proc_, Module::Sfx, 0, "Effect Generator", tint, 5);
            addSlices(*page, proc_, Module::Fx, 0, kFxSlices, tint);
            break;
        case 9:
            addSlices(*page, proc_, Module::Mix, 0, kMixSlices, tint);
            addSlices(*page, proc_, Module::Master, 0, kMasterSlices, tint);
            break;
        default: break;
        }
        pages_[static_cast<size_t>(t)] = std::move(page);
    }
    percPages_.resize(kPercLanes);
    for (int lane = 0; lane < kPercLanes; ++lane) {
        auto page = std::make_unique<ControlPage>();
        addSlices(*page, proc_, Module::Perc, lane, kPercSlices, partColour(3));
        percPages_[static_cast<size_t>(lane)] = std::move(page);
    }
    buildSetPage();
}

ControlPage* PhospheneEditor::activePage() const
{
    if (tab_ == 3) return percPages_[static_cast<size_t>(juce::jlimit(0, kPercLanes - 1, percLane_))].get();
    return tab_ >= 0 && tab_ < static_cast<int>(pages_.size()) ? pages_[static_cast<size_t>(tab_)].get() : nullptr;
}

void PhospheneEditor::setTab(int index)
{
    tab_ = juce::jlimit(0, tabNames().size() - 1, index);
    for (int i = 0; i < tabButtons_.size(); ++i) tabButtons_[i]->setToggleState(i == tab_, juce::dontSendNotification);
    for (int i = 0; i < laneButtons_.size(); ++i) laneButtons_[i]->setVisible(tab_ == 3);
    for (int i = 0; i < laneButtons_.size(); ++i) laneButtons_[i]->setToggleState(i == percLane_, juce::dontSendNotification);
    viewport_.setViewedComponent(activePage(), false);
    layoutContent();
    content_.repaint();
}

void PhospheneEditor::setPercLane(int lane)
{
    percLane_ = juce::jlimit(0, kPercLanes - 1, lane);
    for (int i = 0; i < laneButtons_.size(); ++i) laneButtons_[i]->setToggleState(i == percLane_, juce::dontSendNotification);
    viewport_.setViewedComponent(activePage(), false);
    layoutContent();
    content_.repaint();
}

void PhospheneEditor::resized()
{
    // The body is drawn in design coordinates and scaled to the window, so a page never reflows:
    // dragging the corner is a zoom. That is what makes a picture taken at design size the truth
    // about the layout.
    const float scale = juce::jmax(0.25f, static_cast<float>(getWidth()) / designW_);
    content_.setTransform(juce::AffineTransform::scale(scale));
    content_.setBounds(0, 0, designW_, juce::roundToInt(getHeight() / scale));
}

void PhospheneEditor::layoutContent()
{
    juce::Rectangle<int> r = content_.getLocalBounds();
    r.removeFromTop(64);                                  // header, painted
    juce::Rectangle<int> tabs = r.removeFromTop(34).reduced(10, 4);
    const int tw = tabs.getWidth() / juce::jmax(1, tabButtons_.size());
    for (auto* b : tabButtons_) b->setBounds(tabs.removeFromLeft(tw).reduced(2, 0));
    if (tab_ == 3) {
        juce::Rectangle<int> lanes = r.removeFromTop(30).reduced(12, 4);
        const int lw = juce::jmin(52, lanes.getWidth() / juce::jmax(1, laneButtons_.size()));
        for (auto* b : laneButtons_) b->setBounds(lanes.removeFromLeft(lw).reduced(2, 0));
    }
    viewport_.setBounds(r.reduced(8, 4));
    if (auto* page = activePage()) {
        const int first = page->layout(viewport_.getWidth());
        if (first > viewport_.getHeight()) page->layout(viewport_.getWidth() - 12);   // room for the scrollbar
    }
}

void PhospheneEditor::paint(juce::Graphics& g) { g.fillAll(bg0); }

void PhospheneEditor::paintContent(juce::Graphics& g)
{
    g.fillAll(bg0);
    juce::Rectangle<int> header(0, 0, designW_, 64);
    g.setColour(card);
    g.fillRect(header);
    g.setColour(edge);
    g.drawHorizontalLine(63, 0.0f, static_cast<float>(designW_));

    juce::Rectangle<int> h = header.reduced(16, 8);
    g.setColour(accent);
    g.setFont(title(21.0f));
    g.drawText("PHOSPHENE", h.removeFromLeft(170), juce::Justification::centredLeft, false);
    g.setColour(faint);
    g.setFont(body(11.0f));
    g.drawText("psytrance set generator", h.removeFromLeft(150), juce::Justification::centredLeft, false);
    if (proc_.muted()) {
        g.setColour(red);
        g.setFont(title(11.0f));
        g.drawText(proc_.muteForced() ? "MUTED (PHOS_MUTE)" : "MUTED", h.removeFromLeft(160), juce::Justification::centredLeft, false);
    }

    const TransportView t = proc_.transport();
    juce::String state = t.restarting ? "planning" : (t.playing ? "playing" : "stopped");
    juce::String info;
    info << "seed " << juce::String(proc_.seed()) << "      bar " << juce::String(t.bar + 1)
         << "      " << juce::String(t.bpm, 1) << " BPM      " << (t.hostSync ? "host clock" : "own clock")
         << "      " << state;
    g.setColour(text);
    g.setFont(body(12.5f));
    g.drawText(info, h, juce::Justification::centredRight, false);
    // The page's background, so a tab reads as a sheet lying on the window.
    g.setColour(card.darker(0.25f));
    g.fillRoundedRectangle(viewport_.getBounds().toFloat(), 8.0f);
}

void PhospheneEditor::timerCallback()
{
    refreshSetPage();
    content_.repaint(0, 0, designW_, 64);
}

// ---------------------------------------------------------------- screenshots

bool PhospheneEditor::writeScreenshot(const juce::File& file)
{
    const juce::Image img = createComponentSnapshot(getLocalBounds(), true, 1.0f);
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    auto out = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
    if (out == nullptr) return false;
    juce::PNGImageFormat png;
    return png.writeImageToStream(img, *out);
}

void PhospheneEditor::runScreenshotMode()
{
    // PHOS_PLAY=<seconds> [+ PHOS_RECORD=<file.wav>]: start the standalone, play for a while,
    // optionally record what it played, and quit. The dev aid for hearing the live path -- the
    // composer on its own thread, blocks arriving from a real device -- without a hand on the mouse.
    const double playFor = juce::SystemStats::getEnvironmentVariable("PHOS_PLAY", "0").getDoubleValue();
    if (playFor > 0.0) {
        const juce::String wav = juce::SystemStats::getEnvironmentVariable("PHOS_RECORD", "");
        if (wav.isNotEmpty()) proc_.startRecording(juce::File(wav));
        proc_.play();
        juce::Timer::callAfterDelay(static_cast<int>(playFor * 1000.0),
                                    [safe = juce::Component::SafePointer<PhospheneEditor>(this)] {
                                        if (safe != nullptr) { safe->proc_.stopRecording(); safe->proc_.stop(); }
                                        juce::JUCEApplicationBase::quit();
                                    });
        return;
    }
    const juce::String one = juce::SystemStats::getEnvironmentVariable("PHOS_SHOT", "");
    const juce::String all = juce::SystemStats::getEnvironmentVariable("PHOS_SHOT_ALL", "");
    if (one.isEmpty() && all.isEmpty()) return;
    // At design size: the pixels of the picture are then the layout's own measurements.
    setSize(designW_, designH_);
    // A moment for the composer to plan the first tracks, so the Set tab's list is not "planning...".
    juce::Timer::callAfterDelay(2500, [safe = juce::Component::SafePointer<PhospheneEditor>(this), one, all] {
        if (safe == nullptr) return;
        if (all.isNotEmpty()) {
            const juce::File dir(all);
            dir.createDirectory();
            for (int i = 0; i < tabNames().size(); ++i) {
                safe->setTab(i);
                safe->setSize(safe->designW_, safe->designH_);
                const juce::String name = tabNames()[i].toLowerCase().replace(" / ", "-").replace(" ", "-");
                safe->writeScreenshot(dir.getChildFile("tab-" + juce::String(i) + "-" + name + ".png"));
            }
        }
        if (one.isNotEmpty()) safe->writeScreenshot(juce::File(one));
        juce::JUCEApplicationBase::quit();
    });
}
