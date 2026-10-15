/**
 * @file EditorGallery.cpp
 * @brief The Gallery tab (23.09.2026): saved sets, each with its name, seed, tracks, the listener's verdicts and
 *        a strip of its form; a click loads one.
 *
 * Sets are ordinary `.phosset` files in the gallery folder (PhospheneProcessor::galleryFolder) with the gallery
 * lines of phos/Gallery.h after their header. "Save to gallery..." asks for a name and writes the set as it
 * stands -- seed, knobs, locks and rerolls -- together with the tracks the editor has seen planned, which is what
 * the strip draws. Loading goes through the processor's undo (a loaded set is one step back), and the verdicts
 * next to each set are counted from the ratings file of the Perform tab, by seed.
 * @author Rene Weller
 */
#include "PluginEditor.h"
#include <algorithm>

using namespace phos;
using namespace phosui;

namespace {

/**
 * @brief The colour of a section letter in the form strip.
 * @param[in] c  the section letter
 * @return its colour
 */
juce::Colour sectionColour(char c)
{
    switch (c) {
    case 'I': return Faint;
    case 'G': return juce::Colour::fromHSV(0.33f, 0.45f, 0.75f, 1.0f);
    case 'B': return juce::Colour::fromHSV(0.14f, 0.60f, 0.90f, 1.0f);
    case 'D': return Accent;
    case 'C': return Warm;
    case 'K': return juce::Colour::fromHSV(0.76f, 0.45f, 0.85f, 1.0f);
    case 'O': return Faint.darker(0.3f);
    default: return Edge;
    }
}

} // namespace

// ==================================================================== GalleryDisplay

void GalleryDisplay::setRows(std::vector<Row> rows)
{
    m_rows = std::move(rows);
    m_hover = -1;
    setSize(getWidth(), juce::jmax(RowH, static_cast<int>(m_rows.size()) * RowH));
    repaint();
}

void GalleryDisplay::paint(juce::Graphics& g)
{
    if (m_rows.empty()) {
        g.setColour(Dim);
        g.setFont(body(13.0f));
        g.drawText("No sets saved yet. \"Save to gallery...\" keeps the set that is playing, with its seed, knobs, locks and rerolls.",
                   getLocalBounds().reduced(12, 0), juce::Justification::centredLeft, true);
        return;
    }
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const Row& r = m_rows[i];
        juce::Rectangle<int> row(0, static_cast<int>(i) * RowH, getWidth(), RowH);
        if (static_cast<int>(i) == m_hover) { g.setColour(GroupColour.brighter(0.08f)); g.fillRect(row); }
        g.setColour(Edge);
        g.drawHorizontalLine(row.getBottom() - 1, 0.0f, static_cast<float>(getWidth()));
        juce::Rectangle<int> left = row.reduced(12, 6).removeFromLeft(380);
        const juce::String name = r.m_entry.m_name.empty() ? r.m_file.getFileNameWithoutExtension() : juce::String(r.m_entry.m_name);
        g.setColour(Text);
        g.setFont(title(13.5f));
        g.drawText(name, left.removeFromTop(18), juce::Justification::centredLeft, true);
        double minutes = 0.0;
        juce::StringArray styles;
        for (const GalleryTrack& t : r.m_entry.m_tracks) {
            if (t.m_bpm > 0.0) minutes += t.m_bars * 4.0 / t.m_bpm;
            styles.addIfNotAlreadyThere(juce::String(t.m_style));
        }
        juce::String info;
        info << "seed " << juce::String(static_cast<juce::int64>(r.m_entry.m_seed));
        if (!r.m_entry.m_tracks.empty()) info << "  ·  " << static_cast<int>(r.m_entry.m_tracks.size()) << " tracks, " << juce::roundToInt(minutes) << " min  ·  " << styles.joinIntoString(", ");
        g.setColour(Dim);
        g.setFont(body(11.5f));
        g.drawText(info, left.removeFromTop(16), juce::Justification::centredLeft, true);
        juce::String second = juce::String(r.m_entry.m_saved).replaceCharacter('T', ' ');
        if (r.m_rating.m_good + r.m_rating.m_bad + r.m_rating.m_notes > 0)
            second << "   ·   good " << r.m_rating.m_good << ", bad " << r.m_rating.m_bad << (r.m_rating.m_notes > 0 ? ", notes " + juce::String(r.m_rating.m_notes) : juce::String());
        g.drawText(second, left.removeFromTop(16), juce::Justification::centredLeft, true);

        // The strip: every track's sections in order, as wide as their bars, over the width that is left.
        juce::Rectangle<int> strip = row.reduced(12, 16).withTrimmedLeft(400);
        int total = 0;
        for (const GalleryTrack& t : r.m_entry.m_tracks) total += t.m_bars;
        if (total <= 0 || strip.getWidth() < 20) {
            g.setColour(Faint);
            g.drawText("(no form saved)", strip, juce::Justification::centredLeft, false);
            continue;
        }
        const float scale = (strip.getWidth() - 3.0f * static_cast<float>(r.m_entry.m_tracks.size())) / static_cast<float>(total);
        float x = static_cast<float>(strip.getX());
        for (const GalleryTrack& t : r.m_entry.m_tracks) {
            const juce::StringArray parts = juce::StringArray::fromTokens(juce::String(t.m_form), " ", "");
            for (const juce::String& p : parts) {
                if (p.isEmpty()) continue;
                const float w = scale * static_cast<float>(p.substring(1).getIntValue());
                g.setColour(sectionColour(static_cast<char>(p[0])));
                g.fillRect(juce::Rectangle<float>(x, static_cast<float>(strip.getY()), juce::jmax(1.0f, w - 0.5f), static_cast<float>(strip.getHeight())));
                x += w;
            }
            x += 3.0f;   // a gap between tracks
        }
    }
}

void GalleryDisplay::mouseDown(const juce::MouseEvent& e)
{
    const int i = e.y / RowH;
    if (i >= 0 && i < static_cast<int>(m_rows.size()) && m_onOpen) m_onOpen(m_rows[static_cast<size_t>(i)].m_file);
}

void GalleryDisplay::mouseMove(const juce::MouseEvent& e)
{
    const int i = e.y / RowH;
    if (i != m_hover) { m_hover = i; repaint(); }
}

void GalleryDisplay::mouseExit(const juce::MouseEvent&)
{
    m_hover = -1;
    repaint();
}

// ==================================================================== the page

void PhospheneEditor::buildGalleryPage()
{
    auto page = std::make_unique<ControlPage>();
    const juce::Colour tint = partColour(TAB_GALLERY);

    const int ga = page->addGroup("Gallery", tint, 16);
    {
        auto save = std::make_unique<juce::TextButton>("Save to gallery...");
        save->setTooltip("Keeps the set as it is now: seed, knobs, locks, rerolls, and the form of its tracks");
        save->onClick = [this] {
            m_galleryAsk = std::make_unique<juce::AlertWindow>("Save to gallery", "A name for this set", juce::MessageBoxIconType::NoIcon);
            m_galleryAsk->addTextEditor("name", "seed " + juce::String(static_cast<juce::int64>(m_proc.seed())));
            m_galleryAsk->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
            m_galleryAsk->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            m_galleryAsk->enterModalState(true, juce::ModalCallbackFunction::create([this](int result) {
                if (result == 1 && m_galleryAsk != nullptr) {
                    const juce::File f = m_proc.saveToGallery(m_galleryAsk->getTextEditorContents("name"));
                    if (m_galleryNote != nullptr)
                        m_galleryNote->setText(f.existsAsFile() ? "Saved: " + f.getFullPathName() : juce::String("Could not write into ") + m_proc.galleryFolder().getFullPathName(),
                                              juce::dontSendNotification);
                    refreshGalleryPage();
                }
                // Not deleted inside its own callback: the modal manager is still holding it.
                juce::MessageManager::callAsync([safe = juce::Component::SafePointer<PhospheneEditor>(this)] { if (safe != nullptr) safe->m_galleryAsk.reset(); });
            }), false);
        };
        page->addControl(ga, std::move(save), "", 4, true);

        auto open = std::make_unique<juce::TextButton>("Open folder...");
        open->setTooltip("Shows the gallery folder: sets can be copied in and out as files");
        open->onClick = [this] { m_proc.galleryFolder().createDirectory(); m_proc.galleryFolder().startAsProcess(); };
        page->addControl(ga, std::move(open), "", 3, true);

        auto note = std::make_unique<juce::Label>(juce::String(), m_proc.galleryFolder().getFullPathName());
        note->setJustificationType(juce::Justification::centredLeft);
        note->setColour(juce::Label::textColourId, Dim);
        note->setMinimumHorizontalScale(0.6f);
        m_galleryNote = note.get();
        page->addControl(ga, std::move(note), "", 9, true);
    }

    const int gl = page->addGroup("Saved sets (click to load)", tint, 16);
    {
        auto view = std::make_unique<juce::Viewport>();
        view->setScrollBarsShown(true, false);
        auto* list = new GalleryDisplay();
        list->m_onOpen = [this](const juce::File& f) {
            bool ok = false;
            m_proc.undoable("Load " + f.getFileNameWithoutExtension(), [this, f, &ok] { ok = m_proc.importSet(f); });
            if (m_galleryNote != nullptr)
                m_galleryNote->setText((ok ? "Loaded: " : "Could not read: ") + f.getFileName(), juce::dontSendNotification);
            m_trackRows.clear();
            m_rowsSeed = 0;
        };
        view->setViewedComponent(list, true);
        m_gallery = list;
        m_galleryView = view.get();
        page->addControl(gl, std::move(view), "", 16, true, 8);
    }
    m_pages[static_cast<size_t>(TAB_GALLERY)] = std::move(page);
}

void PhospheneEditor::refreshGalleryPage()
{
    if (m_gallery == nullptr) return;
    const std::map<uint64_t, RatingCount> ratings = ratingsBySeed(m_proc.ratingsFile().getFullPathName().toRawUTF8());
    juce::Array<juce::File> files = m_proc.galleryFolder().findChildFiles(juce::File::findFiles, false, "*.phosset");
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
    std::vector<GalleryDisplay::Row> rows;
    for (const juce::File& f : files) {
        GalleryDisplay::Row r;
        r.m_file = f;
        if (!readGalleryEntry(f.loadFileAsString().toStdString(), r.m_entry)) continue;
        const auto it = ratings.find(r.m_entry.m_seed);
        if (it != ratings.end()) r.m_rating = it->second;
        rows.push_back(std::move(r));
    }
    const int w = m_galleryView != nullptr ? juce::jmax(200, m_galleryView->getMaximumVisibleWidth()) : 800;
    m_gallery->setSize(w, m_gallery->getHeight());
    m_gallery->setRows(std::move(rows));
}
