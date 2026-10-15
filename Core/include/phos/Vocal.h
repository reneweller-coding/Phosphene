/**
 * @file Vocal.h
 * @brief The voices of the psychedelic layer: spoken phrases from the shipped voice pack, and two
 *        synthetic voices -- a formant choir voice and "alien" chatter (19.09.2026, round
 *        "fx-psychedelia").
 *
 * **The voice pack (`voices.phosvx`).** Short spoken phrases cut from public-domain and free-to-reuse
 * recordings of the AmbientSynth archive (NASA mission audio, the Library of Congress's Citizen DJ
 * packs); which ones, why, and their credits are in `Core/data/CREDITS-voices.md`, and the packer is
 * `Tools/voice_pack.py`. A phrase is stored at 16 kHz as 4-bit IMA ADPCM, a quarter of 16-bit PCM:
 * speech is band limited to 350 .. 3200 Hz by its treatment anyway, so 16 kHz loses nothing that is
 * kept, and the ADPCM hiss sits under the saturation and the delay throws. Every phrase carries two
 * marks the packer took from a word-level transcript: where its **last word** starts (the delay
 * throw catches that word) and where its **first word** lies (the chop repeats it).
 *
 * **Byte layout** (little endian), normative for `Tools/voice_pack.py`:
 * @code
 *   "PHOSVX01"  u32 count  u32 sampleRate
 *   count x { u8 category  u8 nameLength  name[nameLength]
 *             u32 samples  u32 throwAt  u32 chopStart  u32 chopLength
 *             i16 predictor  u8 stepIndex  u8 reserved  u32 bytes  data[bytes] (low nibble first) }
 *   "PHOSVXE1"
 * @endcode
 *
 * **One voice at a time.** "Never two phrases at once" (the brief): the generator is monophonic. A new
 * event takes over and the one that was sounding fades out over 10 ms -- the placement (Form.cpp) keeps
 * them apart anyway, so this is the rule's guard, not its mechanism.
 *
 * **The synthetic voices.** A glottal source -- a band-limited saw through a one-pole low pass, which
 * gives the glottal flow's -12 dB/octave tilt above the corner -- through three formant band passes on
 * the vowel formants of Peterson and Barney ("Control methods used in a study of the vowels", J. Acoust.
 * Soc. Am. 24(2), 1952; the male averages). The *formant voice* glides through three vowels and a
 * fifth or a fourth in pitch over the event, with vibrato: a chant. The *alien chatter* fires syllables
 * of 45 .. 90 ms, each on a random vowel with the formants scaled up by a third (a smaller vocal tract)
 * and a random pitch, and ring-modulates the result with a sine of 400 .. 1100 Hz.
 *
 * **Treatment** (all four): a band pass 250 Hz .. 4.5 kHz (the speech pocket of the user's rule is
 * 400 Hz .. 2 kHz; the edges keep the consonants), saturation, a pitch shift by resampling (the pick
 * chooses 0, down by vocal.pitch, down by half of it, or up by half), and a *throw weight* per sample
 * that the engine feeds into a tempo delay: 0 while the phrase speaks, 1 from its last word on.
 */
#pragma once
#include "phos/Dsp.h"
#include "phos/Oscillator.h"
#include "phos/Sfx.h"
#include <cstdint>
#include <string>
#include <vector>

namespace phos {

/** @brief One phrase of the voice pack, decoded. */
struct VoicePhrase {
    std::string m_name;             ///< the source file's stem (CREDITS-voices.md lists every one)
    int m_category = 0;             ///< 0 = space (NASA), 1 = spoken (Library of Congress)
    int m_sampleRate = 16000;       ///< rate of @c samples
    std::vector<float> m_samples;   ///< decoded, -1..1
    uint32_t m_throwAt = 0;         ///< first sample of the last word
    uint32_t m_chopStart = 0;   ///< the first word: its first sample
    uint32_t m_chopLength = 0;   ///< ... its length, samples
};

/**
 * @brief Where a bare pack name is looked for before waveTableSearchPath(): for a host that unpacks
 *        its resources one by one (the Quest app, which may take a pushed wavetable pack from one
 *        directory and the shipped voice pack from another). Call it before the first Engine::prepare().
 */
void setVoicePackSearchPath(const std::string& directory);

/**
 * @brief Loads the voice pack once (idempotent, genuinely thread-safe -- see loadWaveTableLibrary()'s
 *        note, same call-once gate). Looks for the bare name in the working directory, then in
 *        setVoicePackSearchPath()'s directory, then in waveTableSearchPath() (a host that set that
 *        one ships the voice pack beside the wavetables), then in PHOS_SOURCE_DATA_DIR.
 * @param path  the file; nullptr means "voices.phosvx"
 * @param error receives why nothing was loaded, may be null
 * @return phrases loaded (0 when the pack is missing: the spoken types then fall silent, the synthetic
 *         voices still play)
 * @warning Never on the audio thread.
 */
int loadVoicePack(const char* path = nullptr, std::string* error = nullptr);
/** @brief Forgets the pack (tests). */
void resetVoicePack();
/** @brief Phrases loaded. */
int voicePhraseCount();
/** @brief A loaded phrase (index < voicePhraseCount()). */
const VoicePhrase& voicePhrase(int index);
/**
 * @brief Decodes 4-bit IMA ADPCM (low nibble first), exposed for the self test.
 * @param data      the nibbles
 * @param samples   how many samples to decode
 * @param predictor initial predictor
 * @param stepIndex initial step index (0..88)
 * @param out       receives @p samples values in -32768..32767
 */
void decodeImaAdpcm(const uint8_t* data, size_t samples, int predictor, int stepIndex, std::vector<int16_t>& out);

/** @brief The voice generator (part Vocal). */
class Vocal {
public:
    /** @brief Prepares for @p sampleRate (loads the voice pack on the first call). */
    void prepare(double sampleRate);
    /** @brief Silences every voice. */
    void reset();
    /** @brief Reads the effective parameters (indexed by vocal::) and the key. */
    void update(const float* v, int keyRoot);
    /**
     * @brief Starts a voice; the one sounding fades out.
     * @param type           FormantVoice, AlienChatter, SpokenWord or VoiceChop
     * @param samples        the event's length
     * @param velocity       0..1
     * @param late           sub-sample onset
     * @param samplesPerBeat the tempo (the chop's sixteenths)
     * @param pick           the event's own seed: which phrase, which vowels, which pitch
     */
    void trigger(SfxTypeE type, int samples, float velocity, double late, double samplesPerBeat, uint64_t pick);
    /**
     * @brief Renders @p n samples into @p L, @p R (replaced) and the throw weight into @p throwOut
     *        (replaced; 0..1 times the voice's own level, for the engine's delay throw).
     */
    void process(float* L, float* R, float* throwOut, int n);
    /** @brief Voices sounding (0, 1, or 2 during a handover). */
    int active() const;

private:
    /** @brief One sounding voice. */
    struct Voice {
        SfxTypeE m_type = SfxTypeE::SPOKEN_WORD;   ///< which kind of voice
        bool m_on = false;   ///< sounding
        long long m_pos = 0;   ///< samples played
        long long m_length = 1;   ///< the event's length, samples
        double m_late = 0.0;   ///< the sub-sample onset
        double m_spb = 20000.0;   ///< samples per beat
        float m_velocity = 1.0f;   ///< the level
        float m_fade = 1.0f;   ///< the handover's fade
        float m_pan = 0.0f;   ///< the place, -1 .. 1
        bool m_fading = false;   ///< fading out under a new voice
        int m_phrase = -1;   ///< the voice pack phrase it plays (spoken word, chop)
        double m_read = 0.0;   ///< the read position in the phrase
        double m_rate = 1.0 / 3.0;   ///< its step per sample
        Rng m_rng;   ///< the event's own random stream
        VaOscillator m_osc;   ///< the sung and chattered voices' source
        Svf m_formant[3];   ///< three formants
        Svf m_hp1;   ///< the high pass: first section
        Svf m_hp2;   ///< ... second
        Svf m_lp;   ///< the low pass
        float m_tilt = 0.0f;   ///< the source's spectral tilt state
        float m_throwW = 0.0f;   ///< the weight for the delay throw
        double m_vibPh = 0.0;   ///< the vibrato's phase
        double m_ringPh = 0.0;   ///< the ring modulator's phase
        double m_ringHz = 700.0;   ///< the ring modulator's frequency, Hz
        double m_pitchHz = 150.0;   ///< the sung frequency, Hz
        double m_glide = 0.0;   ///< the glide, semitones
        int m_vowel[3] = { 0, 4, 7 };   ///< the vowels a sung phrase moves through
        long long m_sylEnd = 0;   ///< the chatter's syllable: where it ends
        long long m_sylLen = 1;   ///< ... how long it is
        float m_sylF[3] = { 700, 1100, 2400 };   ///< its formant frequencies
    };
    /** @brief One sample of voice @p v; its weight for the delay throw goes to @p throwW. */
    float voiceSample(Voice& v, float& throwW);

    double m_sr = 48000.0;   ///< sample rate
    Voice m_voice[2];   ///< the voice and the one fading out
    int m_keyRoot = 6;   ///< the key's pitch class, for the sung voice
    float m_pitch = 3.0f;   ///< vocal.pitch
    float m_drive = 0.35f;   ///< vocal.drive
    float m_width = 0.5f;   ///< vocal.width
};

} // namespace phos
