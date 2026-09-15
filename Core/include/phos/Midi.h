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
 */
#pragma once
#include "phos/Score.h"
#include <cstdint>
#include <string>
#include <vector>

namespace phos {

constexpr int kMidiPpq = 960;   ///< ticks per quarter note in exported files

/** @brief MIDI channel (0-based) a part is written to. */
int midiChannelOf(Part part);

/**
 * @brief Encodes a score as a Standard MIDI File (format 1).
 * @param score the score; notes need not be sorted
 * @return the file's bytes
 */
std::vector<uint8_t> encodeMidi(const Score& score);

/**
 * @brief Writes encodeMidi() to a file.
 * @return false if the file cannot be written
 */
bool writeMidiFile(const Score& score, const char* path);

/** @brief One track of a decoded file. */
struct MidiTrackData {
    std::string name;                 ///< track name meta event, may be empty
    std::vector<NoteEvent> notes;     ///< notes; part is taken from the track name if it matches kPartNames
    int channel = -1;                 ///< channel of the first note, -1 if none
};

/** @brief Contents of a decoded file. */
struct MidiFileData {
    int format = 1;                                    ///< SMF format 0 or 1
    int ppq = 480;                                     ///< ticks per quarter note
    std::vector<TempoPoint> tempos;                    ///< tempo events (beat, bpm), held steps
    std::vector<std::pair<double, std::string>> markers; ///< marker meta events (beat, text)
    int keySharps = 0;                                 ///< key signature: sharps (+) or flats (-)
    bool keyMinor = false;                             ///< key signature mode
    std::vector<MidiTrackData> tracks;                 ///< all tracks in file order
};

/**
 * @brief Decodes a Standard MIDI File (format 0 or 1, metrical time).
 * @param data  file bytes
 * @param size  number of bytes
 * @param out   receives the contents
 * @param error receives a message on failure, may be null
 * @return false on malformed or unsupported data
 */
bool decodeMidi(const uint8_t* data, size_t size, MidiFileData& out, std::string* error = nullptr);

/** @brief Reads and decodes a file. */
bool readMidiFile(const char* path, MidiFileData& out, std::string* error = nullptr);

/** @brief Key signature sharps/flats for a minor key with root pitch class @p root (A minor = 0). */
int minorKeySharps(int root);

} // namespace phos
