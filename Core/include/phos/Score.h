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
#include "phos/Params.h"   // PolyInstance: the polyphonic parts follow its order
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace phos {

/**
 * @brief The generators a note can belong to.
 *
 * Texture (the shamanic bed, Texture.h) and Vocal (speech phrases and synthetic voices, Vocal.h) carry
 * the effect type as their pitch exactly like Part::Sfx does (SfxBaseNote + SfxType); sfxTypePart()
 * (Sfx.h) says which of the three parts a type belongs to.
 *
 * 19.09.2026, round "voices": the polyphonic parts stand in the groups of PolyInstance (Params.h) --
 * lead and counter-lead, arp and stab, pad and drone -- and the parts after them moved. A part is never
 * stored as a number (the MIDI export names its tracks with PartNames, and the import matches names),
 * so nothing saved depends on these values; the sort order of notes at equal beats does, and with it
 * every render, which the round accepted.
 */
enum class PartE : uint8_t { KICK = 0, BASS, PERC, ACID, LEAD, COUNTER, ARP, STAB, PAD, DRONE, SFX, TEXTURE, VOCAL,
                            FIELD,   ///< 27.09.2026: the field recordings' sampler (FieldPlayer.h). Appended.
                            COUNT };
constexpr int NumParts = static_cast<int>(PartE::COUNT);   ///< number of parts
extern const char* const PartNames[NumParts];              ///< "Kick", "Bass", ...

/** @brief The part a polyphonic instance plays (the two enums run side by side from Lead). */
constexpr PartE polyPart(PolyInstanceE i) { return static_cast<PartE>(static_cast<int>(PartE::LEAD) + polyIndex(i)); }
/** @brief The polyphonic instance of a part, or -1 when the part is not played by a Poly. */
constexpr int polyOfPart(PartE p)
{
    const int k = static_cast<int>(p) - static_cast<int>(PartE::LEAD);
    return k >= 0 && k < PolyInstances ? k : -1;
}
static_assert(polyPart(PolyInstanceE::DRONE) == PartE::DRONE && polyPart(PolyInstanceE::COUNTER) == PartE::COUNTER,
              "Part and PolyInstance must list the polyphonic voices in the same order");

/** @brief Flags of a note event. */
enum NoteFlagE : uint8_t {
    NOTE_ACCENT = 1,   ///< accented step (acid, percussion)
    NOTE_SLIDE  = 2,   ///< glide into the next note (acid; the lead since 22.09.2026, on flagged notes only)
    NOTE_SHORT  = 4,   ///< played at half its written length: the lead's staccato gate (22.09.2026)
};

/** @brief One note of the score. */
struct NoteEvent {
    double  m_beat = 0.0;       ///< start in beats from the start of the set
    float   m_length = 0.25f;   ///< duration in beats
    PartE    m_part = PartE::KICK; ///< generator
    uint16_t m_lane = 0;        ///< lane within the part (percussion lane, layer; an effect's bank preset since 23.09.2026, up to 512)
    uint8_t m_pitch = 36;       ///< MIDI note number
    uint8_t m_velocity = 100;   ///< 1..127
    uint8_t m_flags = 0;        ///< NoteFlag bits
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
    /** @brief What a control event does to its parameter. */
    enum class KindE : uint8_t {
        OFFSET,        ///< ramp the normalised offset to `value` over `length` beats (raised cosine)
        OVERRIDE,      ///< play `value` instead of the knob (discrete parameters); value < 0 clears it
        /**
         * @brief The first sounding bass slot of the coming beat, in beats after that beat's kick
         *        (`param` is not read; a value <= 0 hands the engine back its own derivation).
         *
         * Two computations in the engine need the instant of the first bass note after a kick and
         * have to agree with the composer exactly: how long the kick may ring before it
         * (`Kick::constrainTail`) and the kick's phase at that instant (`Kick::setPhaseTarget` and
         * the bass's start phase). Until 16.09.2026 the engine derived that instant from the bass
         * pattern family, carried over an `Override` on `compose.bass_pattern` -- which works only
         * while every bar is one of five families with a fixed first slot. A bar drawn from the
         * corpus onset model has no number in `BassPatterns`, so the number itself travels instead
         * of the family it used to be looked up from. One event per beat, at the beat: that is the
         * latest instant at which it is still in force for that beat's kick (controls are dispatched
         * before notes at equal beats) and the earliest at which it is not still the previous beat's.
         */
        BASS_SLOT,
        /**
         * @brief Play `value` -- absolute, in the parameter's own unit -- in place of the knob of a continuous
         *        parameter; an offset still rides on top. A negative `length` clears it (26.09.2026; a NaN would be
         *        the clearer word, but every control's value stays finite -- testKnobFuzz checks that).
         *
         * The composer's sound for a track is a preset (SoundPresets.h, bankPresets) since 26.09.2026, and the user
         * wanted it absolute: "Stellt der Composer auch die Preset-Werte auf Absolutwerte ... ein?" A preset's
         * continuous knobs arrive as this at the track's start, its discrete ones as Override; the section rides stay
         * offsets. An own sound (mix.*_own) plays the knobs and none of it.
         */
        BASE,
    };
    double  m_beat = 0.0;       ///< when the change starts
    float   m_length = 0.0f;    ///< ramp length in beats; 0 = immediately
    float   m_value = 0.0f;     ///< target offset or override value
    int16_t m_param = 0;        ///< global parameter id
    KindE    m_kind = KindE::OFFSET; ///< what `value` means
};

/** @brief Key signature change (for the MIDI export). */
struct KeyChange {
    double m_beat = 0.0;   ///< position in beats
    int    m_root = 6;     ///< pitch class of the minor key
};

/**
 * @brief Section types of a track's form (Form.h).
 *
 * The first six are the categories the score has always carried; Pdb and Cut were appended in Phase 5
 * for the two sub-bar categories of Grosz et al. 2025 -- the pre-drop break (the last bar of a buildup)
 * and the cut (the first beats of a breakdown) -- so that the indices of the older types, and with them
 * the MIDI markers, did not move.
 */
enum class SectionTypeE : uint8_t { INTRO = 0, GROOVE, BUILD, DROP, BREAK, OUTRO, PDB, CUT, COUNT };
extern const char* const SectionNames[static_cast<int>(SectionTypeE::COUNT)];   ///< display names

/** @brief Start of a section. */
struct SectionMark {
    double      m_beat = 0.0;                ///< start in beats
    SectionTypeE m_type = SectionTypeE::GROOVE; ///< kind of section
    float       m_energy = 0.5f;             ///< 0..1 on the energy arc
    int         m_track = 0;                 ///< index of the track within the set
};

/** @brief A complete or partial score. */
struct Score {
    TempoMap m_tempo;                        ///< tempo over beats
    int m_keyRoot = 6;                       ///< pitch class of the key, 0 = C
    int m_scale = 1;                         ///< index into ScaleNames
    std::vector<NoteEvent> m_notes;          ///< all notes, kept sorted by sort()
    std::vector<SectionMark> m_sections;     ///< section starts, sorted by beat
    std::vector<KeyChange> m_keyChanges;     ///< key changes after the start (keyRoot holds at beat 0)

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
        : m_size(capacityPow2), m_buf(std::make_unique<T[]>(static_cast<size_t>(capacityPow2))) {}
    /** @brief Producer side; false if full. */
    bool push(const T& e)
    {
        const int w = m_write.load(std::memory_order_relaxed);
        const int next = (w + 1) & (m_size - 1);
        if (next == m_read.load(std::memory_order_acquire)) return false;
        m_buf[static_cast<size_t>(w)] = e;
        m_write.store(next, std::memory_order_release);
        return true;
    }
    /** @brief Consumer side: the oldest event without removing it; false if empty. */
    bool peek(T& e) const
    {
        const int r = m_read.load(std::memory_order_relaxed);
        if (r == m_write.load(std::memory_order_acquire)) return false;
        e = m_buf[static_cast<size_t>(r)];
        return true;
    }
    /** @brief Consumer side: removes the oldest event; false if empty. */
    bool pop(T& e)
    {
        if (!peek(e)) return false;
        m_read.store((m_read.load(std::memory_order_relaxed) + 1) & (m_size - 1), std::memory_order_release);
        return true;
    }
    /** @brief Free slots as seen from the producer. */
    int space() const
    {
        const int w = m_write.load(std::memory_order_acquire), r = m_read.load(std::memory_order_acquire);
        return (r - w - 1 + m_size) & (m_size - 1);
    }
    /** @brief Consumer side: drops everything. */
    void clear() { m_read.store(m_write.load(std::memory_order_acquire), std::memory_order_release); }
private:
    int m_size;   ///< capacity
    std::unique_ptr<T[]> m_buf;   ///< the ring
    std::atomic<int> m_read{ 0 };   ///< the consumer's position
    std::atomic<int> m_write{ 0 };   ///< the producer's position
};

} // namespace phos
