/**
 * @file Midi.h
 * @brief Standard MIDI File export of a score, and a reader for round-trip tests and imports.
 *
 * Format 1, 960 ticks per quarter note: a 1/16 is 240 ticks and a 1/16 triplet 160, so every grid
 * Phosphene composes on is exact. Track 0 is the conductor track (name, 4/4, key signature, tempo
 * map, section markers); every part with notes gets its own track and channel. Tempo ramps are
 * written as one tempo event per beat whose value makes that beat last exactly as long as the ramp
 * does, so the MIDI file stays in time with the audio render to the sample at every beat.
 *
 * Channel layout (0-based): Kick 9 (GM drums, note 36), Bass 0, Acid 1, Lead 2, Arp 3, Pad 4,
 * Sfx 5, Perc 9 (shares the drum channel).
 * @author Rene Weller
 */
#pragma once
#include "phos/Score.h"
#include <cstdint>
#include <string>
#include <vector>

namespace phos {

constexpr int MidiPpq = 960;   ///< ticks per quarter note in exported files

/**
 * @brief MIDI channel (0-based) a part is written to.
 * @param[in] part  the part
 * @return its MIDI channel, 0 .. 15
 */
int midiChannelOf(PartE part);

/**
 * @brief Encodes a score as a Standard MIDI File (format 1).
 * @param[in] score  the score; notes need not be sorted
 * @return the file's bytes
 */
std::vector<uint8_t> encodeMidi(const Score& score);

/**
 * @brief Writes encodeMidi() to a file.
 * @param[in] score  the score
 * @param[in] path   the file's path
 * @return false if the file cannot be written
 */
bool writeMidiFile(const Score& score, const char* path);

/** @brief One track of a decoded file. */
struct MidiTrackData {
    std::string m_name;                 ///< track name meta event, may be empty
    std::vector<NoteEvent> m_notes;     ///< notes; part is taken from the track name if it matches PartNames
    int m_channel = -1;                 ///< channel of the first note, -1 if none
};

/** @brief Contents of a decoded file. */
struct MidiFileData {
    int m_format = 1;                                    ///< SMF format 0 or 1
    int m_ppq = 480;                                     ///< ticks per quarter note
    std::vector<TempoPoint> m_tempos;                    ///< tempo events (beat, bpm), held steps
    std::vector<std::pair<double, std::string>> m_markers; ///< marker meta events (beat, text)
    int m_keySharps = 0;                                 ///< first key signature: sharps (+) or flats (-)
    bool m_keyMinor = false;                             ///< first key signature's mode
    std::vector<std::pair<double, int>> m_keys;          ///< every key signature (beat, sharps)
    std::vector<MidiTrackData> m_tracks;                 ///< all tracks in file order
};

/**
 * @brief Decodes a Standard MIDI File (format 0 or 1, metrical time).
 * @param[in]  data   file bytes
 * @param[in]  size   number of bytes
 * @param[out] out    receives the contents
 * @param[out] error  receives a message on failure, may be null
 * @return false on malformed or unsupported data
 */
bool decodeMidi(const uint8_t* data, size_t size, MidiFileData& out, std::string* error = nullptr);

/**
 * @brief Reads and decodes a file.
 * @param[in]  path   the file's path
 * @param[out] out    the decoded file
 * @param[out] error  receives the failure, or null
 * @return false if it cannot be read
 */
bool readMidiFile(const char* path, MidiFileData& out, std::string* error = nullptr);

/**
 * @brief Key signature sharps/flats for a minor key with root pitch class @p root (A minor = 0).
 * @param[in] root  the root's pitch class
 * @return the sharps, negative for flats
 */
int minorKeySharps(int root);

} // namespace phos
