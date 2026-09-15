/**
 * @file Score.h
 * @brief The score ("Partitur"): the symbolic result of composition.
 *
 * The composer writes a score; the audio thread only plays events from it; the MIDI export and the
 * offline render read the same data. Positions are in beats (quarter notes) from the start of the
 * set, so a score is independent of tempo and sample rate -- the tempo map turns beats into time.
 */
#pragma once
#include "phos/Clock.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace phos {

/** @brief The generators a note can belong to. */
enum class Part : uint8_t { Kick = 0, Bass, Perc, Acid, Lead, Arp, Pad, Sfx, Count };
constexpr int kNumParts = static_cast<int>(Part::Count);   ///< number of parts
extern const char* const kPartNames[kNumParts];              ///< "Kick", "Bass", ...

/** @brief Flags of a note event. */
enum NoteFlag : uint8_t {
    kNoteAccent = 1,   ///< accented step (acid, percussion)
    kNoteSlide  = 2,   ///< glide into the next note (acid)
};

/** @brief One note of the score. */
struct NoteEvent {
    double  beat = 0.0;       ///< start in beats from the start of the set
    float   length = 0.25f;   ///< duration in beats
    Part    part = Part::Kick; ///< generator
    uint8_t lane = 0;         ///< lane within the part (percussion lane, layer)
    uint8_t pitch = 36;       ///< MIDI note number
    uint8_t velocity = 100;   ///< 1..127
    uint8_t flags = 0;        ///< NoteFlag bits
};

/** @brief Sort order of events: beat, then part, then lane, then pitch. */
bool noteLess(const NoteEvent& a, const NoteEvent& b);

/**
 * @brief A change of the sound, written by the composer: how a track or a passage departs from the
 *        knobs.
 *
 * The composer never writes parameters. It writes these, and the engine combines them with the
 * knobs: a continuous parameter plays at knob + offset, both in the knob's normalised 0..1 domain,
 * so a variation keeps its musical size whatever the knob's range or curve; a discrete parameter
 * (a choice, a switch) plays the override while one is set. With every offset at zero and no
 * override, the instrument sounds exactly as the knobs say.
 */
struct ControlEvent {
    enum class Kind : uint8_t {
        Offset,        ///< ramp the normalised offset to `value` over `length` beats (raised cosine)
        Override,      ///< play `value` instead of the knob (discrete parameters); value < 0 clears it
    };
    double  beat = 0.0;       ///< when the change starts
    float   length = 0.0f;    ///< ramp length in beats; 0 = immediately
    float   value = 0.0f;     ///< target offset or override value
    int16_t param = 0;        ///< global parameter id
    Kind    kind = Kind::Offset; ///< what `value` means
};

/** @brief Key signature change (for the MIDI export). */
struct KeyChange {
    double beat = 0.0;   ///< position in beats
    int    root = 6;     ///< pitch class of the minor key
};

/**
 * @brief Section types of a track's form (Form.h).
 *
 * The first six are the categories the score has always carried; Pdb and Cut were appended in Phase 5
 * for the two sub-bar categories of Grosz et al. 2025 -- the pre-drop break (the last bar of a buildup)
 * and the cut (the first beats of a breakdown) -- so that the indices of the older types, and with them
 * the MIDI markers, did not move.
 */
enum class SectionType : uint8_t { Intro = 0, Groove, Build, Drop, Break, Outro, Pdb, Cut, Count };
extern const char* const kSectionNames[static_cast<int>(SectionType::Count)];   ///< display names

/** @brief Start of a section. */
struct SectionMark {
    double      beat = 0.0;                ///< start in beats
    SectionType type = SectionType::Groove; ///< kind of section
    float       energy = 0.5f;             ///< 0..1 on the energy arc
    int         track = 0;                 ///< index of the track within the set
};

/** @brief A complete or partial score. */
struct Score {
    TempoMap tempo;                        ///< tempo over beats
    int keyRoot = 6;                       ///< pitch class of the key, 0 = C
    int scale = 1;                         ///< index into kScaleNames
    std::vector<NoteEvent> notes;          ///< all notes, kept sorted by sort()
    std::vector<SectionMark> sections;     ///< section starts, sorted by beat
    std::vector<KeyChange> keyChanges;     ///< key changes after the start (keyRoot holds at beat 0)

    /** @brief Sorts notes and sections. */
    void sort();
    /** @brief Beat after the last note ends. */
    double endBeat() const;
};

/**
 * @brief Single-producer single-consumer ring of events, lock-free.
 *
 * The composer thread pushes, the audio thread pops. The buffer is allocated once at construction.
 * @tparam T trivially copyable event type
 */
template <class T>
class EventRing {
public:
    /** @brief @p capacityPow2 must be a power of two; one slot stays empty. */
    explicit EventRing(int capacityPow2 = 8192)
        : size_(capacityPow2), buf_(std::make_unique<T[]>(static_cast<size_t>(capacityPow2))) {}
    /** @brief Producer side; false if full. */
    bool push(const T& e)
    {
        const int w = write_.load(std::memory_order_relaxed);
        const int next = (w + 1) & (size_ - 1);
        if (next == read_.load(std::memory_order_acquire)) return false;
        buf_[static_cast<size_t>(w)] = e;
        write_.store(next, std::memory_order_release);
        return true;
    }
    /** @brief Consumer side: the oldest event without removing it; false if empty. */
    bool peek(T& e) const
    {
        const int r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        e = buf_[static_cast<size_t>(r)];
        return true;
    }
    /** @brief Consumer side: removes the oldest event; false if empty. */
    bool pop(T& e)
    {
        if (!peek(e)) return false;
        read_.store((read_.load(std::memory_order_relaxed) + 1) & (size_ - 1), std::memory_order_release);
        return true;
    }
    /** @brief Free slots as seen from the producer. */
    int space() const
    {
        const int w = write_.load(std::memory_order_acquire), r = read_.load(std::memory_order_acquire);
        return (r - w - 1 + size_) & (size_ - 1);
    }
    /** @brief Consumer side: drops everything. */
    void clear() { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }
private:
    int size_;
    std::unique_ptr<T[]> buf_;
    std::atomic<int> read_{ 0 }, write_{ 0 };
};

} // namespace phos
