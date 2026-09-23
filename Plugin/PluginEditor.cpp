/**
 * @file PluginEditor.cpp
 * @brief The editor's frame: header, tab bar, lane bar, pages and the screenshot mode.
 */
#include "PluginEditor.h"
#include "phos/Composer.h"
#include <cmath>
#include <map>

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
    // 23.09.2026: appended in earlier rounds and on no page until the host test's reachability check found them.
    { "Bite & Sub", bass::Bite, 7, 4 },
};
/** @brief One percussion lane's table. */
const Slice kPercSlices[] = {
    { "Lane", perc::Active, 3, 5 }, { "Source", perc::Pitch, 8, 4 }, { "Noise", perc::Noise, 4, 4 },
    { "Shape", perc::Decay, 6, 4 }, { "Output", perc::Level, 6, 4 },
    { "Motion", perc::PanDepth, 3, 3 },   // 23.09.2026, see the bass
};
/** @brief The acid voice's table. */
const Slice kAcidSlices[] = {
    { "Voice", acid::Wave, 10, 5 }, { "Squelch", acid::Squelch, 6, 4 },
    { "Delay", acid::DelaySend, 6, 4 }, { "Sends & Level", acid::RoomSend, 4, 4 },
    { "Colour & Hall", acid::Disperse, 3, 3 },   // 23.09.2026, see the bass
};
/** @brief A polyphonic engine's table (lead, counter-lead, arp, stab, pad, drone). */
const Slice kPolySlices[] = {
    { "Oscillator", poly::Osc, 15, 5 }, { "Filter", poly::Cutoff, 7, 4 }, { "Amplitude", poly::AmpAttack, 6, 4 },
    { "Delay", poly::DelaySend, 6, 4 }, { "Sends", poly::RoomSend, 3, 3 },
    { "Trance Gate", poly::Gate, 7, 4 }, { "Level", poly::Level, 1, 2 },
    // Appended parameters: the disperser, the drift and (19.09.2026) the filter response.
    { "Colour", poly::Disperse, 4, 4 },
    // 20.09.2026, round "dialogue": the portamento and the place in the image, and the voice's own
    // comb / flanger / phaser. Every parameter has to stand on a tab or the manual generator refuses
    // to print (Tools/manual), which is exactly how a forgotten append is caught.
    { "Glide & Image", poly::Glide, 2, 2 },
    { "Modulation", poly::Mod, 5, 5 },
    // 23.09.2026, see the bass: the gated hall, the second oscillator and the voice LFO (the four after it).
    { "Gated Hall", poly::HallGate, 1, 2 }, { "Second Oscillator", poly::Osc2, 4, 4 }, { "LFO", poly::Osc2Detune + 1, 4, 4 },
};
/** @brief The send effects. */
const Slice kFxSlices[] = {
    { "Room", fx::RoomSize, 3, 3 }, { "Hall", fx::HallSize, 4, 4 }, { "Returns", fx::LowCut, 5, 5 },
};
/** @brief The mixer and the master. */
// The channels up to the SFX strip, then the sidechain, then the two strips of 19.09.2026 (texture, vocal).
const Slice kMixSlices[] = { { "Channels", mix::KickMute, mix::PercRoom + 2, 6 }, { "Sidechain", mix::DuckAttack, 3, 3 },
                             { "Bed & Voices", mix::TextureMute, 4, 4 } };
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
    // The limiter's ceiling is -1 dBTP, so exactly -1.0 is the target and not an overshoot.
    bar(area.removeFromTop(rowH), "Peak", reading_.truePeak, green, -0.9f);
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
    // Over the DJ overlap (Form.h, kDjOverlap) two rows contain the play head: the outgoing track, which
    // owns the bar (kick, bass, key), and the incoming one, whose intro is mixing in. The first is marked
    // as playing; the second more faintly, and it says so.
    bool ownerSeen = false;
    for (const Row& row : rows_) {
        if (area.getHeight() < rowH) break;
        juce::Rectangle<int> line = area.removeFromTop(rowH);
        const bool inside = bar_ >= row.bar && bar_ < row.bar + row.bars;
        const bool here = inside && !ownerSeen;
        const bool mixingIn = inside && ownerSeen;
        ownerSeen = ownerSeen || inside;
        if (mixingIn) {
            g.setColour(accent.withAlpha(0.07f));
            g.fillRoundedRectangle(line.toFloat(), 3.0f);
            g.setColour(accent.withAlpha(0.7f));
            g.setFont(phosui::body(9.5f));
            g.drawText("mixing in, " + juce::String(juce::jmax(0, row.bar + kDjOverlap - bar_)) + " bars to the hand-over",
                       line.reduced(6, 0), juce::Justification::centredRight, false);
        }
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

// ==================================================================== PatternDisplay

void PatternDisplay::update(std::vector<NoteEvent> notes, int firstBar, int bars, double beat)
{
    notes_ = std::move(notes);
    firstBar_ = firstBar;
    bars_ = juce::jmax(1, bars);
    beat_ = beat;
    repaint();
}

void PatternDisplay::paint(juce::Graphics& g)
{
    const juce::Rectangle<int> r = getLocalBounds().reduced(4, 3);
    g.setColour(bg0.withAlpha(0.6f));
    g.fillRoundedRectangle(r.toFloat(), 5.0f);
    juce::Rectangle<int> area = r.reduced(8, 6);
    g.setColour(dim);
    g.setFont(title(10.0f));
    g.drawText(juce::String(kPartNames[static_cast<int>(part_)]).toUpperCase() + "   bars "
                   + juce::String(firstBar_ + 1) + " to " + juce::String(firstBar_ + bars_),
               area.removeFromTop(13), juce::Justification::topLeft, false);
    const juce::Rectangle<float> plot = area.toFloat();
    if (plot.getHeight() < 12.0f) return;

    const double from = static_cast<double>(firstBar_) * kBeatsPerBar;
    const double span = static_cast<double>(bars_) * kBeatsPerBar;
    auto xOf = [&](double beat) { return plot.getX() + static_cast<float>((beat - from) / span) * plot.getWidth(); };

    // The grid: a hairline every sixteenth, brighter on beats, brightest on bars.
    for (int s = 0; s <= bars_ * kBeatsPerBar * 4; ++s) {
        const double beat = from + s * 0.25;
        const bool bar = s % (kBeatsPerBar * 4) == 0, beatLine = s % 4 == 0;
        g.setColour(bar ? edge.brighter(0.5f) : (beatLine ? edge : edge.withAlpha(0.35f)));
        g.drawVerticalLine(juce::roundToInt(xOf(beat)), plot.getY(), plot.getBottom());
    }

    if (notes_.empty()) {
        g.setColour(faint);
        g.setFont(phosui::body(11.0f));
        g.drawText("nothing composed here yet", plot, juce::Justification::centred, false);
    } else if (part_ == Part::Perc) {
        // Twelve lanes, named by the role each lane plays; the lane whose knobs are on screen is lit.
        const float rowH = plot.getHeight() / kPercLanes;
        for (int lane = 0; lane < kPercLanes; ++lane) {
            const juce::Rectangle<float> row(plot.getX(), plot.getY() + lane * rowH, plot.getWidth(), rowH);
            if (lane == lane_) {
                g.setColour(accent.withAlpha(0.10f));
                g.fillRect(row);
            }
            g.setColour(faint.withAlpha(0.6f));
            g.setFont(phosui::body(juce::jlimit(7.0f, 9.5f, rowH * 0.7f)));
            g.drawText(juce::String(lane + 1), row.withWidth(14.0f), juce::Justification::centredRight, false);
        }
        for (const NoteEvent& e : notes_) {
            if (e.part != Part::Perc || e.lane >= kPercLanes) continue;
            const juce::Rectangle<float> row(xOf(e.beat), plot.getY() + e.lane * rowH + 1.5f,
                                             juce::jmax(3.0f, xOf(e.beat + 0.22) - xOf(e.beat)), rowH - 3.0f);
            g.setColour((e.lane == lane_ ? accent : partColour(3)).withAlpha(0.35f + 0.65f * e.velocity / 127.0f));
            g.fillRoundedRectangle(row, 1.5f);
        }
    } else {
        // A piano roll over whatever pitches this part uses, with two semitones of air.
        int lo = 127, hi = 0;
        for (const NoteEvent& e : notes_) if (e.part == part_) { lo = juce::jmin(lo, static_cast<int>(e.pitch)); hi = juce::jmax(hi, static_cast<int>(e.pitch)); }
        if (lo > hi) {
            g.setColour(faint);
            g.setFont(phosui::body(11.0f));
            g.drawText("this part is silent here", plot, juce::Justification::centred, false);
            return;
        }
        lo -= 2;
        hi += 2;
        const float rowH = plot.getHeight() / juce::jmax(1, hi - lo + 1);
        // The black keys as darker stripes, so the pitches can be read off.
        for (int p = lo; p <= hi; ++p) {
            static const bool black[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
            if (!black[((p % 12) + 12) % 12]) continue;
            g.setColour(edge.withAlpha(0.25f));
            g.fillRect(plot.getX(), plot.getBottom() - (p - lo + 1) * rowH, plot.getWidth(), rowH);
        }
        for (const NoteEvent& e : notes_) {
            if (e.part != part_) continue;
            const float y = plot.getBottom() - (e.pitch - lo + 1) * rowH;
            const juce::Rectangle<float> box(xOf(e.beat), y + 0.5f,
                                             juce::jmax(3.0f, xOf(e.beat + e.length) - xOf(e.beat) - 1.0f),
                                             juce::jmax(2.0f, rowH - 1.0f));
            const juce::Colour c = partColour(static_cast<int>(part_) + 1);
            g.setColour((e.flags & kNoteAccent) ? c.brighter(0.5f) : c.withAlpha(0.45f + 0.55f * e.velocity / 127.0f));
            g.fillRoundedRectangle(box, 1.5f);
            // A slide is drawn as a line into the next note, which is what it does.
            if (e.flags & kNoteSlide) {
                g.setColour(c.withAlpha(0.7f));
                g.drawLine(box.getRight(), box.getCentreY(), box.getRight() + 5.0f, box.getCentreY(), 1.2f);
            }
        }
    }

    // Where the engine is. It sits behind the composed bars by the horizon the rings are kept at,
    // so most of the time the line stands at the very left of the window the display is showing.
    if (beat_ >= from && beat_ <= from + span) {
        g.setColour(accent.withAlpha(0.85f));
        g.drawVerticalLine(juce::roundToInt(xOf(beat_)), plot.getY(), plot.getBottom());
    }
}

// ==================================================================== PhospheneEditor

const juce::StringArray& PhospheneEditor::tabNames()
{
    static const juce::StringArray names{ "Set", "Arrange", "Kick", "Bass", "Percussion", "Acid",
                                          "Lead", "Counter", "Arp", "Stab", "Pad", "Drone", "SFX / FX", "Mixer / Master", "Perform", "Gallery" };
    jassert(names.size() == TabCount);
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
    // Undo and redo (23.09.2026): every knob gesture, seed, lock, reroll, loaded set and factory reset is a step.
    undoButton_.onClick = [this] { proc_.undo(); };
    redoButton_.onClick = [this] { proc_.redo(); };
    content_.addAndMakeVisible(undoButton_);
    content_.addAndMakeVisible(redoButton_);
    setWantsKeyboardFocus(true);
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

namespace {
/** @brief Which part a generator tab shows in its pattern preview; Count = none. */
Part partOfTab(int tab)
{
    switch (tab) {
    case TabKick: return Part::Kick;
    case TabBass: return Part::Bass;
    case TabPerc: return Part::Perc;
    case TabAcid: return Part::Acid;
    case TabLead:    return Part::Lead;
    case TabCounter: return Part::Counter;
    case TabArp:     return Part::Arp;
    case TabStab:    return Part::Stab;
    case TabPad:     return Part::Pad;
    case TabDrone:   return Part::Drone;
    case TabFx:   return Part::Sfx;
    default: return Part::Count;
    }
}
} // namespace

void PhospheneEditor::buildPages()
{
    pages_.resize(static_cast<size_t>(tabNames().size()));
    patterns_.assign(static_cast<size_t>(tabNames().size()), nullptr);
    for (int t = 0; t < tabNames().size(); ++t) {
        if (t == TabPerc) continue;   // percussion: one page per lane, built below
        auto page = std::make_unique<ControlPage>();
        const juce::Colour tint = partColour(t);
        switch (t) {
        case TabSet: break;       // built in EditorSetTab.cpp, after this loop
        case TabArrange: break;   // built in EditorArrange.cpp
        case TabPerform: break;   // built in EditorPerform.cpp
        case TabGallery: break;   // built in EditorGallery.cpp
        case TabKick: addSoundGroup(*page, Module::Kick, 0, 0, tint); addSlices(*page, proc_, Module::Kick, 0, kKickSlices, tint); break;
        case TabBass: addSoundGroup(*page, Module::Bass, 0, 1, tint); addSlices(*page, proc_, Module::Bass, 0, kBassSlices, tint); break;
        case TabAcid: addSoundGroup(*page, Module::Acid, 0, 2, tint); addSlices(*page, proc_, Module::Acid, 0, kAcidSlices, tint); break;
        case TabLead: case TabCounter: case TabArp: case TabStab: case TabPad: case TabDrone:
            // The six voice pages share one table; the tab order is the instance order (PluginEditor.h).
            addSoundGroup(*page, Module::Poly, t - TabLead, 3 + t - TabLead, tint);
            addSlices(*page, proc_, Module::Poly, t - TabLead, kPolySlices, tint);
            break;
        case TabFx:
            page->addModuleGroup(proc_, Module::Sfx, 0, "Effect Generator", tint, 5);
            addSlices(*page, proc_, Module::Fx, 0, kFxSlices, tint);
            // 23.09.2026: the modules of round "fx-psychedelia" had no page (see the bass slices).
            page->addModuleGroup(proc_, Module::PsyFx, 0, "Psy FX", tint, 4);
            page->addModuleGroup(proc_, Module::Texture, 0, "Shamanic Bed", tint, 5);
            page->addModuleGroup(proc_, Module::Vocal, 0, "Voices", tint, 5);
            break;
        case TabMix:
            addSlices(*page, proc_, Module::Mix, 0, kMixSlices, tint);
            addSlices(*page, proc_, Module::Master, 0, kMasterSlices, tint);
            page->addModuleGroup(proc_, Module::Cue, 0, "Score Cues (OSC)", tint, 4);   // 23.09.2026, see the bass slices
            break;
        default: break;
        }
        // Every generator page ends with the notes it is shaping, so the knobs and the pattern are
        // on the same screen (PLAN 8.1).
        if (partOfTab(t) != Part::Count) {
            auto roll = std::make_unique<PatternDisplay>();
            roll->setPart(partOfTab(t));
            patterns_[static_cast<size_t>(t)] = roll.get();
            const int g = page->addGroup("Pattern", tint, 12);
            page->addControl(g, std::move(roll), "", 12, true, 2);
        }
        pages_[static_cast<size_t>(t)] = std::move(page);
    }
    percPages_.resize(kPercLanes);
    percPatterns_.assign(kPercLanes, nullptr);
    for (int lane = 0; lane < kPercLanes; ++lane) {
        auto page = std::make_unique<ControlPage>();
        addSlices(*page, proc_, Module::Perc, lane, kPercSlices, partColour(3));
        // The whole kit on every lane's page: the twelve lanes are one pattern, and only the lit
        // row moves as the lane is changed.
        auto roll = std::make_unique<PatternDisplay>();
        roll->setPart(Part::Perc, lane);
        percPatterns_[static_cast<size_t>(lane)] = roll.get();
        const int g = page->addGroup("Kit pattern", partColour(3), 12);
        page->addControl(g, std::move(roll), "", 12, true, 3);
        percPages_[static_cast<size_t>(lane)] = std::move(page);
    }
    buildSetPage();
    buildArrangePage();
    buildPerformPage();
    buildGalleryPage();
}

void PhospheneEditor::fillPresetBox(PresetBox& pb)
{
    pb.presets = factoryPresets(pb.module, pb.instance);
    for (SoundPreset& u : proc_.userPresets(pb.module, pb.instance)) pb.presets.push_back(std::move(u));
    pb.box->clear(juce::dontSendNotification);
    // A submenu per group, in the order the groups first appear (the factory's order: oscillators, then the
    // wavetable families; the user's last).
    std::vector<std::string> order;
    std::map<std::string, juce::PopupMenu> menus;
    for (size_t i = 0; i < pb.presets.size(); ++i) {
        const SoundPreset& p = pb.presets[i];
        if (menus.find(p.group) == menus.end()) order.push_back(p.group);
        menus[p.group].addItem(static_cast<int>(i) + 1, p.name);
    }
    for (const std::string& g : order) pb.box->getRootMenu()->addSubMenu(g, menus[g]);
    pb.box->setTextWhenNothingSelected(juce::String(pb.presets.size()) + " presets");
}

void PhospheneEditor::addSoundGroup(ControlPage& page, Module module, int instance, int owner, juce::Colour tint)
{
    // The own-sound switch is a mixer parameter; its group is this one, so it stands next to what it protects.
    // Three cells wide, so the page's first group still stands beside it: the switch and "Save..." on the first
    // row, the chooser across the second.
    const int g = page.addModuleGroup(proc_, Module::Mix, 0, "Sound", tint, 3, mix::KickOwn + owner, 1);
    auto pb = std::make_unique<PresetBox>();
    pb->module = module;
    pb->instance = instance;
    auto box = std::make_unique<juce::ComboBox>();
    box->setTooltip("Presets in groups. Choosing one sets the synth's knobs and switches Own Sound on, so the "
                    "generator's per-track sound design leaves it as it is. Level, ducking and (on a voice) the high "
                    "pass and the trance gate stay where they are. Undo takes it back.");
    pb->box = box.get();
    PresetBox* raw = pb.get();
    box->onChange = [this, raw] {
        const int id = raw->box->getSelectedId();
        if (id <= 0 || id > static_cast<int>(raw->presets.size())) return;
        proc_.applyPreset(raw->module, raw->instance, raw->presets[static_cast<size_t>(id - 1)]);
    };
    fillPresetBox(*pb);

    auto save = std::make_unique<juce::TextButton>("Save...");
    save->setTooltip("Saves this synth's knobs as a preset of your own (group \"User\")");
    save->onClick = [this, raw] {
        presetNameDialog_ = std::make_unique<juce::AlertWindow>("Save preset", "A name for this sound:", juce::MessageBoxIconType::NoIcon, this);
        presetNameDialog_->addTextEditor("name", raw->box->getSelectedId() > 0 ? raw->box->getText() : juce::String("My sound"));
        presetNameDialog_->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        presetNameDialog_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        presetNameDialog_->enterModalState(true, juce::ModalCallbackFunction::create([this, raw](int result) {
            const juce::String name = presetNameDialog_ != nullptr ? presetNameDialog_->getTextEditorContents("name") : juce::String();
            if (result == 1 && proc_.saveUserPreset(raw->module, raw->instance, name) != juce::File()) {
                fillPresetBox(*raw);
                for (size_t i = 0; i < raw->presets.size(); ++i)
                    if (raw->presets[i].group == "User" && juce::String(raw->presets[i].name) == juce::File::createLegalFileName(name.trim()))
                        raw->box->setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
            }
        }), false);
    };
    page.addControl(g, std::move(save), "", 2, true);
    page.addControl(g, std::move(box), "Preset", 3);
    presetBoxes_.push_back(std::move(pb));
}

juce::StringArray PhospheneEditor::parametersOnPages() const
{
    const ParamStore& p = proc_.params();
    juce::StringArray seen;
    auto collect = [&](const phosui::ControlPage* page) {
        if (page == nullptr) return;
        for (int g = 0; g < page->groupCount(); ++g)
            for (int id : page->groupParams(g)) seen.addIfNotAlreadyThere(juce::String(p.key(id)));
    };
    for (auto& page : pages_) collect(page.get());
    for (auto& page : percPages_) collect(page.get());
    return seen;
}

ControlPage* PhospheneEditor::activePage() const
{
    if (tab_ == TabPerc) return percPages_[static_cast<size_t>(juce::jlimit(0, kPercLanes - 1, percLane_))].get();
    return tab_ >= 0 && tab_ < static_cast<int>(pages_.size()) ? pages_[static_cast<size_t>(tab_)].get() : nullptr;
}

void PhospheneEditor::setTab(int index)
{
    tab_ = juce::jlimit(0, tabNames().size() - 1, index);
    for (int i = 0; i < tabButtons_.size(); ++i) tabButtons_[i]->setToggleState(i == tab_, juce::dontSendNotification);
    for (int i = 0; i < laneButtons_.size(); ++i) laneButtons_[i]->setVisible(tab_ == TabPerc);
    for (int i = 0; i < laneButtons_.size(); ++i) laneButtons_[i]->setToggleState(i == percLane_, juce::dontSendNotification);
    viewport_.setViewedComponent(activePage(), false);
    layoutContent();
    // Feed the page that has just opened before it is seen for the first time, so a screenshot of
    // a tab shows what it has to say rather than an empty frame waiting for the next tick.
    if (tab_ == TabSet) refreshSetPage();
    else if (tab_ == TabArrange) { arrangeDirty_ = true; refreshArrangePage(); }
    else if (tab_ == TabPerform) refreshPerformPage();
    else if (tab_ == TabGallery && gallery_ != nullptr && gallery_->rowCount() == 0) refreshGalleryPage();
    refreshPattern();
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
    // Undo and redo sit in the header, after the title and the mute note (paintContent keeps that space).
    undoButton_.setBounds(516, 18, 72, 28);
    redoButton_.setBounds(594, 18, 72, 28);
    juce::Rectangle<int> tabs = r.removeFromTop(34).reduced(10, 4);
    const int tw = tabs.getWidth() / juce::jmax(1, tabButtons_.size());
    for (auto* b : tabButtons_) b->setBounds(tabs.removeFromLeft(tw).reduced(2, 0));
    if (tab_ == TabPerc) {
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

void PhospheneEditor::refreshPattern()
{
    // Only the roll that is on screen is fed; the others are filled the moment their tab opens,
    // which is also what makes a screenshot of a tab show its pattern without waiting for a tick.
    PatternDisplay* roll = tab_ == TabPerc ? percPatterns_[static_cast<size_t>(juce::jlimit(0, kPercLanes - 1, percLane_))]
                                     : (tab_ < static_cast<int>(patterns_.size()) ? patterns_[static_cast<size_t>(tab_)] : nullptr);
    if (roll == nullptr) return;
    const TransportView t = proc_.transport();
    constexpr int bars = 4;
    // The window starts at the bar the engine is in, rounded down to a group of four, so the
    // picture does not slide sideways at every tick.
    const int firstBar = juce::jmax(0, (t.bar / bars) * bars);
    if (proc_.readPattern(firstBar, bars, patternNotes_)) roll->update(patternNotes_, firstBar, bars, t.musicalBeat);
    else roll->update({}, firstBar, bars, t.musicalBeat);
}

bool PhospheneEditor::keyPressed(const juce::KeyPress& key)
{
    const juce::ModifierKeys m = key.getModifiers();
    if (m.isCommandDown() && key.getKeyCode() == 'Z') { if (m.isShiftDown()) proc_.redo(); else proc_.undo(); return true; }
    if (m.isCommandDown() && key.getKeyCode() == 'Y') { proc_.redo(); return true; }
    return false;
}

void PhospheneEditor::timerCallback()
{
    undoButton_.setEnabled(proc_.canUndo());
    redoButton_.setEnabled(proc_.canRedo());
    undoButton_.setTooltip(proc_.canUndo() ? "Undo " + proc_.undoName() + " (Ctrl+Z)" : juce::String("Nothing to undo"));
    redoButton_.setTooltip(proc_.canRedo() ? "Redo " + proc_.redoName() + " (Ctrl+Y)" : juce::String("Nothing to redo"));
    // Only the page that is on screen is fed. The arrange timeline in particular draws a whole set,
    // and a set that is not being looked at costs nothing at all this way.
    if (tab_ == TabSet) refreshSetPage();
    else if (tab_ == TabArrange) refreshArrangePage();
    else if (tab_ == TabPerform) refreshPerformPage();
    else if (tab_ == TabGallery && gallery_ != nullptr && gallery_->rowCount() == 0) refreshGalleryPage();
    refreshPattern();
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

namespace {
/** @brief The file name a tab's picture gets, from the tab's own name. */
juce::String shotName(int index)
{
    return "tab-" + juce::String(index) + "-"
         + PhospheneEditor::tabNames()[index].toLowerCase().replace(" / ", "-").replace(" ", "-") + ".png";
}
/** @brief The name of a curve, for the manual. */
const char* curveName(Curve c)
{
    switch (c) {
    case Curve::Linear: return "linear";
    case Curve::Log: return "log";
    case Curve::Int: return "int";
    case Curve::Choice: return "choice";
    default: return "toggle";
    }
}
} // namespace

bool PhospheneEditor::writeManual(const juce::File& dir)
{
    if (!dir.createDirectory()) return false;
    const ParamStore& p = proc_.params();

    // ---------------------------------------------------------------- the pictures
    for (int i = 0; i < tabNames().size(); ++i) {
        setTab(i);
        setSize(designW_, designH_);
        if (!writeScreenshot(dir.getChildFile(shotName(i)))) return false;
    }

    // ---------------------------------------------------------------- every parameter
    juce::Array<juce::var> params;
    for (int i = 0; i < p.count(); ++i) {
        const ParamDesc& d = p.desc(i);
        auto* o = new juce::DynamicObject();
        const juce::String key(p.key(i));
        o->setProperty("key", key);
        o->setProperty("module", key.upToFirstOccurrenceOf(".", false, false));
        o->setProperty("name", juce::String(d.name));
        o->setProperty("unit", juce::String(d.unit));
        o->setProperty("min", d.minValue);
        o->setProperty("max", d.maxValue);
        o->setProperty("default", p.defaultValue(i));
        o->setProperty("curve", juce::String(curveName(d.curve)));
        if (d.curve == Curve::Choice && d.choices != nullptr) {
            juce::Array<juce::var> choices;
            for (int c = 0; c <= static_cast<int>(d.maxValue); ++c) choices.add(juce::String(d.choices[c]));
            o->setProperty("choices", choices);
        }
        params.add(juce::var(o));
    }

    // ---------------------------------------------------------------- the tabs, as they were built
    auto groupsOf = [&p](const phosui::ControlPage* page) {
        juce::Array<juce::var> out;
        if (page == nullptr) return out;
        for (int g = 0; g < page->groupCount(); ++g) {
            auto* o = new juce::DynamicObject();
            o->setProperty("title", page->groupTitle(g));
            juce::Array<juce::var> keys;
            for (int id : page->groupParams(g)) keys.add(juce::String(p.key(id)));
            o->setProperty("params", keys);
            out.add(juce::var(o));
        }
        return out;
    };
    juce::Array<juce::var> tabs;
    for (int i = 0; i < tabNames().size(); ++i) {
        auto* o = new juce::DynamicObject();
        o->setProperty("index", i);
        o->setProperty("name", tabNames()[i]);
        o->setProperty("image", shotName(i));
        // The percussion tab is twelve pages behind one bar; its table is the same twelve times, so
        // the manual prints lane 1 and says so.
        o->setProperty("groups", groupsOf(i == TabPerc ? percPages_[0].get() : pages_[static_cast<size_t>(i)].get()));
        if (i == TabPerc) o->setProperty("lanes", kPercLanes);
        tabs.add(juce::var(o));
    }

    // Which parameters are on a page somewhere, all twelve percussion lanes included. The generator
    // compares this with the full list; what is in neither is a parameter the editor cannot reach.
    juce::Array<juce::var> shown;
    for (const juce::String& s : parametersOnPages()) shown.add(s);

    // ---------------------------------------------------------------- the macros
    juce::Array<juce::var> macros;
    for (int i = 0; i < kNumMacros; ++i) {
        auto* o = new juce::DynamicObject();
        o->setProperty("name", juce::String(kMacroNames[i]));
        o->setProperty("help", juce::String(kMacroHelp[i]));
        o->setProperty("momentary", macroIsMomentary(static_cast<Macro>(i)));
        juce::Array<juce::var> moves;
        PhospheneProcessor::MacroTarget t[PhospheneProcessor::kMaxMacroTargets];
        const int n = proc_.macroTargets(static_cast<Macro>(i), 1.0f, t);
        for (int k = 0; k < n; ++k) {
            auto* m = new juce::DynamicObject();
            m->setProperty("key", juce::String(p.key(t[k].param)));
            m->setProperty("value", t[k].value);
            m->setProperty("absolute", t[k].absolute);
            moves.add(juce::var(m));
        }
        o->setProperty("moves", moves);
        macros.add(juce::var(o));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty("version", juce::String(JucePlugin_VersionString));
    root->setProperty("name", juce::String(JucePlugin_Name));
    root->setProperty("designWidth", designW_);
    root->setProperty("designHeight", designH_);
    root->setProperty("params", params);
    root->setProperty("tabs", tabs);
    root->setProperty("shown", shown);
    root->setProperty("macros", macros);
    return dir.getChildFile("manual.json").replaceWithText(juce::JSON::toString(juce::var(root), false));
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
    const juce::String man = juce::SystemStats::getEnvironmentVariable("PHOS_MANUAL", "");
    if (one.isEmpty() && all.isEmpty() && man.isEmpty()) return;
    // At design size: the pixels of the picture are then the layout's own measurements.
    setSize(designW_, designH_);
    // Playing, and silent: PHOS_SHOT implies PHOS_MUTE, and the pictures are meant to show the
    // meters and the pattern rolls doing something rather than an instrument that has not started.
    proc_.play();
    // A moment for the composer to plan the first tracks, so the Set tab's list is not "planning..."
    // and the arrange timeline has a set to draw. A track's first plan costs a probe render of about
    // two seconds, so a picture of a nine-track set needs `PHOS_SHOT_WAIT=25`.
    const double wait = juce::jlimit(1.0, 600.0, juce::SystemStats::getEnvironmentVariable("PHOS_SHOT_WAIT", "6").getDoubleValue());
    juce::Timer::callAfterDelay(static_cast<int>(wait * 1000.0),
                                [safe = juce::Component::SafePointer<PhospheneEditor>(this), one, all, man] {
        if (safe == nullptr) return;
        if (all.isNotEmpty()) {
            const juce::File dir(all);
            dir.createDirectory();
            for (int i = 0; i < tabNames().size(); ++i) {
                safe->setTab(i);
                safe->setSize(safe->designW_, safe->designH_);
                safe->writeScreenshot(dir.getChildFile(shotName(i)));
            }
        }
        if (man.isNotEmpty()) safe->writeManual(juce::File(man));
        if (one.isNotEmpty()) safe->writeScreenshot(juce::File(one));
        juce::JUCEApplicationBase::quit();
    });
}
