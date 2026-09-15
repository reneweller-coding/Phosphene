/**
 * @file WaveTable.h
 * @brief The classic wavetable: a stack of single cycles, each stored at ten octave-spaced
 *        resolutions so that no note aliases, read with a phase accumulator and Catmull-Rom.
 *
 * A table is up to 64 single cycles. What keeps a high note from aliasing is the classic remedy: every
 * frame is stored at ten resolutions, each an octave poorer than the one before (512 harmonics in 4096
 * samples, 256 in 2048, down to one sine in 32), and a note reads the richest copy whose highest
 * harmonic still lies below Nyquist. Each copy holds eight samples per cycle of its highest harmonic:
 * at four, the interpolation's own images of the top partials were audible (-41 dB in Noctuary's
 * measurement). The copies are made from each frame's Fourier coefficients, so a level is the same
 * waveform with its upper octave of harmonics taken away, phases intact.
 *
 * **Tables without files.** Phosphene uses no samples, and the Quest has no room for a library of
 * table files. The built-in tables are written as spectra in code at start-up (WaveTable.cpp) in the
 * Virus, Microwave and Serum pad idiom the plan asks for: Classic (sine to pulse), Vocal (a saw through
 * the formants of five vowels, a to u), Glass (sparse, brightening partials), PWM (pulse width 50 % to
 * 5 %), Sync (a hard-synced saw, slave ratio 1 to 8) and Formant Saw (a saw with a resonant peak moving
 * up the harmonics).
 *
 * @note Adapted from Noctuary `Core/include/ambient/CycleTable.h` at b60a2fe (15.09.2026): the level
 *       scheme, the guards, the Catmull-Rom read, the level hysteresis and buildFromHarmonics() are
 *       unchanged; file reading and cycle-length detection are left behind, the frame count is capped
 *       at 64 and the built-in tables are new.
 */
#pragma once
#include <complex>
#include <cstddef>
#include <vector>

namespace phos {

constexpr int kNumWaveTables = 6;   ///< built-in tables (names in Params.cpp)
constexpr int kClassicTable  = 0;   ///< built-in table 0: sine, triangle, saw, square, narrow pulse
constexpr int kClassicSawFrame = 2; ///< the sawtooth frame of the Classic table (the supersaw reads it)

/** @brief One table: frames at ten levels. */
struct WaveTable {
    static constexpr int kStoreLen  = 4096;   ///< samples per stored cycle at the finest level
    static constexpr int kMaxFrames = 64;     ///< frames per table
    static constexpr int kLevels    = 10;     ///< 4096 .. 32 samples, 512 .. 1 harmonics
    static constexpr int kGuard     = 3;      ///< one sample before each cycle, two after it
    static constexpr float kTargetRms = 0.35355339f;   ///< RMS of the loudest frame

    int frames = 0;                   ///< frames in the table
    int offset[kLevels] = {};         ///< start of each level in data
    std::vector<float> data;          ///< all levels, all frames

    /** @brief Samples per cycle at a level. */
    static int levelLength(int level)    { const int n = kStoreLen >> level; return n < 32 ? 32 : n; }
    /** @brief Harmonics kept at a level. */
    static int levelHarmonics(int level) { const int h = (kStoreLen / 8) >> level; return h < 1 ? 1 : h; }

    /** @brief The stored cycle of frame @p f at a level; readable from index -1 to levelLength + 1. */
    const float* cycle(int level, int f) const
    {
        return data.data() + static_cast<size_t>(offset[level])
             + static_cast<size_t>(f) * static_cast<size_t>(levelLength(level) + kGuard) + 1;
    }
    /** @brief One sample of frame @p f at a level and a phase in [0, 1): Catmull-Rom through four samples. */
    float sample(int level, int f, double phase) const
    {
        const int len = levelLength(level);
        const double x = phase * static_cast<double>(len);
        int i = static_cast<int>(x);
        if (i >= len) i = len - 1;
        if (i < 0) i = 0;
        const float t = static_cast<float>(x - static_cast<double>(i));
        const float* c = cycle(level, f);
        const float y0 = c[i - 1], y1 = c[i], y2 = c[i + 1], y3 = c[i + 2];
        const float a = 0.5f * (y2 - y0);
        const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float d = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((d * t + b) * t + a) * t + y1;
    }
    /**
     * @brief A position between frames: frame floor(p (frames-1)) blended into the next.
     * @param position 0..1 across the table
     */
    float sampleAt(int level, float position, double phase) const
    {
        const float p = (position < 0.0f ? 0.0f : (position > 1.0f ? 1.0f : position)) * static_cast<float>(frames - 1);
        int f = static_cast<int>(p);
        if (f >= frames - 1) f = frames - 2 < 0 ? 0 : frames - 2;
        const float t = frames > 1 ? p - static_cast<float>(f) : 0.0f;
        const float a = sample(level, f, phase);
        return frames > 1 ? a + t * (sample(level, f + 1, phase) - a) : a;
    }
    /**
     * @brief Builds from Fourier coefficients, one vector per frame: element h-1 is harmonic h as the
     *        complex amplitude of a cosine, |c| cos(2 pi h t + arg c).
     */
    bool buildFromHarmonics(const std::vector<std::vector<std::complex<double>>>& frameCoefficients);
};

/**
 * @brief Gain that lifts a stored frame to the loudness of the ramp 2t - 1.
 *
 * buildFromHarmonics() normalises every frame to @c kTargetRms = 1/(2 sqrt 2); the ramp has RMS
 * 1/sqrt 3. The supersaw reads the saw frame instead of generating a PolyBLEP ramp and multiplies by
 * this, so the change of oscillator does not change the level of the lead: 2 sqrt(2/3).
 */
inline constexpr float kSawTableGain = 1.63299316f;

/**
 * @brief The level a cycle at @p hz reads: the richest whose highest harmonic lies below Nyquist.
 *
 * It moves to a richer level only with ten per cent to spare, so a pitch hovering on a boundary does
 * not flip between two levels. @p current < 0 means there is no previous level.
 */
int waveLevelFor(double hz, double sampleRate, int current);

/** @brief A built-in table (0 .. kNumWaveTables-1), built on first use -- in prepare(), never on the audio thread. */
const WaveTable& builtinWaveTable(int index);

} // namespace phos
