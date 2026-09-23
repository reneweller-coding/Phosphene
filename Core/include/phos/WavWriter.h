/**
 * @file WavWriter.h
 * @brief Streaming WAV writer for renders and stems.
 *
 * Writes as it goes, so a two-hour set does not have to sit in memory. 32-bit float or 24-bit PCM.
 * A RIFF file cannot exceed 4 GiB; beyond that the writer switches the header to RF64 (EBU Tech
 * 3306) on close, which every current editor reads.
 */
#pragma once
#include <cstdint>
#include <cstdio>

namespace phos {

/** @brief Sample format of a written file. */
enum class WavFormat { Float32, Pcm24 };

/** @brief Streaming stereo or mono WAV writer. */
class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter() { close(); }
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    /**
     * @brief Creates the file and writes a provisional header.
     * @param path       file name
     * @param sampleRate frames per second
     * @param channels   1 or 2
     * @param format     sample format
     * @return false if the file cannot be created
     */
    bool open(const char* path, int sampleRate, int channels, WavFormat format);
    /** @brief Appends @p n frames; @p R is ignored for mono and may be null. */
    bool write(const float* L, const float* R, int n);
    /** @brief Finalises the header and closes the file. */
    bool close();
    /** @brief Frames written so far. */
    uint64_t frames() const { return frames_; }
    /**
     * @brief TPDF dither for 24-bit files (23.09.2026; on by default, ignored for float).
     *
     * Rounding a float mix to 24 bits without dither turns the rounding error into distortion that follows
     * the signal -- inaudible in a loud drop, but a reverb tail or a fade into the next track ends in
     * truncation grit instead of noise. Two uniform draws of one step each (triangular PDF, the textbook
     * choice: the error's first two moments stop depending on the signal) go in before the rounding. The
     * noise sits at about -141 dBFS. It is reproducible -- the generator is seeded at open(), so the same
     * render writes the same file -- and a sample that is exactly 0 stays 0, so digital silence stays silent.
     * Call before open().
     */
    void setDither(bool on) { dither_ = on; }

private:
    bool writeHeader(bool final);
    /** @brief The dither's uniform draw in [0, 1) (xorshift64*). */
    double uniform()
    {
        rng_ ^= rng_ >> 12; rng_ ^= rng_ << 25; rng_ ^= rng_ >> 27;
        return static_cast<double>((rng_ * 2685821657736338717ull) >> 11) * (1.0 / 9007199254740992.0);
    }
    bool dither_ = true;
    uint64_t rng_ = 0x9E3779B97F4A7C15ull;
    FILE* f_ = nullptr;
    int sampleRate_ = 48000, channels_ = 2;
    WavFormat format_ = WavFormat::Float32;
    uint64_t frames_ = 0;
    uint32_t clipped_ = 0;
};

} // namespace phos
