/**
 * @file Poly.cpp
 * @brief Polyphonic engine: voice allocation, supersaw coefficients, rendering.
 */
#include "phos/Poly.h"
#include "phos/Params.h"
#include "phos/Util.h"
#include "phos/WaveTableFile.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {

constexpr double kPiD = 3.141592653589793;

/** @brief Pan position of each unison oscillator: low and high partners on opposite sides. */
constexpr double kUnisonPan[kPolyUnison] = { -1.0, 0.67, -0.33, 0.0, 0.33, -0.67, 1.0 };

/**
 * @brief Frequency factor of a drift of @p cents, as 1 + cents ln2 / 1200.
 *
 * The first two terms of 2^(cents/1200). At the parameter's ceiling of 8 cents the walk practically
 * never leaves +-5 standard deviations, that is +-40 cents, where the truncation error of the
 * expansion is 2.7e-4 -- 0.46 cents. At the default of 1 cent and the +-4 cents a walk actually
 * covers the error is 5.4e-6, that is 0.009 cents, three orders under the smallest pitch difference
 * anyone hears. A pow() per slot per note would be correct to the last bit and would also be fine
 * here; the expansion is kept because it is the same arithmetic on every platform and no library's
 * pow() is.
 */
inline double driftFactor(double cents) { return 1.0 + cents * (0.6931471805599453 / 1200.0); }

/**
 * @brief The share of the unison detune the second oscillator's pair plays (24.09.2026).
 *
 * The pair sits on the outermost kept slots, which in a supersaw carry the widest detune (Szabo's outer
 * offset, +-11 % of the curve) -- right for the two quietest of seven saws, wrong for a second oscillator
 * that carries up to half the voice's power in two copies. Seed 5's first pad (a supersaw partner in
 * unison) played every chord tone with a loud pair a third of a semitone either side: 15 %
 * of its peak power more than a quarter tone off the grid, after every FM pad was already back on it.
 * A quarter keeps the pair's stereo beat and puts it within the few cents osc2_detune means (seed 5: +7 .. +15
 * cents, and no first pad of seeds 1..24 with more than a tenth of its power off the grid).
 */
constexpr double kOsc2Spread = 0.25;

/**
 * @brief The FM ratio a voice plays: poly.fm_ratio to the nearest half, never under 0.5.
 *
 * A two-operator pair puts its sidebands at f_c (1 +- k r). With r = p/q in lowest terms every one of
 * them is a multiple of f_c / q, so the spectrum is harmonic; with any other r it is not, and that is
 * the bell and the struck metal (Chowning, JAES 21(7), 1973 -- his bell is 1 : 1.4). A half keeps q at 1 or 2:
 * the note's own harmonic series, or the one an octave under it.
 *
 * 24.09.2026, the user on the first track's pad: "klingen einfach nur schraeg und nicht, wie irgend
 * etwas, was ich jemals in einem Psytrance-Song gehoert habe". Measured over the first track of seeds
 * 1..24 (all Full-On, F# Phrygian): in 9 of 24 more than a tenth of the pad's peak power lay more
 * than a quarter tone off the grid, and every one of the FM pads was among them. Seed 1's pad played
 * at 1.91 -- the knob's 2.0 plus its recipe's thickness offset (ComposerControls.cpp, kVoiceLoadings) -- so
 * each chord tone carried partials at 0.91 and 2.91 times itself, a quarter tone off every note of
 * the key, 7 dB under the chord. Every pitched voice plays chords or lines in a key; the metallic FM
 * of the set lives in the percussion (the rim, the zap: perc.fm_ratio is not rounded).
 */
inline double harmonicFmRatio(double knob) { return std::max(0.5, std::round(knob * 2.0) * 0.5); }

} // namespace

double Poly::detuneCurve(double x)
{
    // Szabo (2010), the eleventh-degree fit to the JP-8000's detune, highest power first.
    static constexpr double c[12] = { 10028.7312891634, -50818.8652045924, 111363.4808729368, -138150.6761080548,
                                      106649.6679158292, -53046.9642751875, 17019.9518580080, -3425.0836591318,
                                      404.2703938388, -24.1878824391, 0.6717417634, 0.0030115596 };
    const double t = std::clamp(x, 0.0, 1.0);
    double y = 0.0;
    for (double k : c) y = y * t + k;
    return std::max(0.0, y);
}

void Poly::mixGains(double x, double& center, double& side)
{
    const double t = std::clamp(x, 0.0, 1.0);
    center = -0.55366 * t + 0.99785;
    side = -0.73764 * t * t + 1.2841 * t + 0.044372;
}

double Poly::dynamicDetune(double knob, double amount, double lengthBeats)
{
    // 0 at a sixteenth or shorter, 1 from a beat on, linear in log2 of the length in between.
    const double s = std::clamp(std::log2(std::max(lengthBeats, 1e-6) / 0.25) / 2.0, 0.0, 1.0);
    return knob * (1.0 - std::clamp(amount, 0.0, 1.0) * (1.0 - s));
}

void Poly::prepare(double sampleRate)
{
    sr_ = sampleRate;
    for (Envelope& e : amp_) e.setSampleRate(sr_);
    ampTimes_.setSampleRate(sr_);
    delay_.prepare(sr_);
    slotL_.assign(static_cast<size_t>(kPolyBlock * kPolySlots), 0.0f);
    slotR_.assign(static_cast<size_t>(kPolyBlock * kPolySlots), 0.0f);
    chanIn_.assign(static_cast<size_t>(kPolyBlock * kPolyLanes), 0.0f);
    chanAmp_.assign(static_cast<size_t>(kPolyBlock * kPolyLanes), 0.0f);
    chanOut_.assign(static_cast<size_t>(kPolyBlock * kPolyLanes), 0.0f);
    sendBuf_.assign(static_cast<size_t>(kPolyBlock), 0.0f);
    wtRow_.assign(static_cast<size_t>(kPolyBlock * kPolySlots), 0.0f);
    // Every table this engine can be pointed at is built here, never on the audio thread: a build
    // is ten inverse FFTs per frame. loadWaveTableLibrary() is idempotent, so the three Poly
    // instances share the one load; a missing file is not an error, the built-in tables are the
    // fallback and waveTable() hands one out in its place.
    for (int i = 0; i < kNumBuiltinWaveTables; ++i) builtinWaveTable(i);
    loadWaveTableLibrary();
    table_ = &waveTable(0);
    sawTable_ = &builtinWaveTable(kClassicTable);
    reset();
}

void Poly::reset()
{
    slots_ = PolySlots{};
    ch_ = PolyChannels{};
    for (int s = 0; s < kPolySlots; ++s) { slots_.dt[s] = 0.001f; slots_.inv[s] = 1000.0f; slots_.idxDecay[s] = 1.0f; slots_.pw[s] = 0.5f; wtPh_[s] = 0.0; wtDt_[s] = 0.0; wtLevel_[s] = 0; slotSaw_[s] = false; slotHzMul_[s] = 1.0; slotSpread_[s] = 1.0; }
    for (int v = 0; v < kPolyVoices; ++v) {
        amp_[v].kill();
        fenv_[v] = 0.0f;
        accent_[v] = 1.0f;
        gate_[v] = 0;
        age_[v] = 0;
        pitch_[v] = 60;
        hpHz_[v] = 220.0f;
        posEnv_[v] = 0.0f;
        lfoPh_[v] = 0.0;
        sawVoice_[v] = false;
        glidePitch_[v] = glideTarget_[v] = 60.0;
        glideY_[v] = glideScale_[v] = glideFmRatio_[v] = 0.0;
        voiceCoefs(v);
    }
    lastPitch_ = -1.0;
    lfo2Ph_ = 0.0;
    lfo2Value_ = 0.0f;
    newest_ = 0;
    counter_ = 0;
    pos_ = 0;
    tableReads_ = 0;
    seedPhases(0x504F4C59ull);
    for (float& d : driftSlot_) d = 0.0f;
    for (float& d : driftVoice_) d = 0.0f;
    dispL_.reset();
    dispR_.reset();
    delay_.reset();
}

void Poly::advanceDrift()
{
    for (int s = 0; s < kPolySlots; ++s) driftSlot_[s] += (driftRng_.bipolar() - driftSlot_[s]) * driftAlpha_;
    for (int v = 0; v < kPolyVoices; ++v) driftVoice_[v] += (driftRng_.bipolar() - driftVoice_[v]) * driftAlpha_;
}

void Poly::writeSlotPitch(int voice)
{
    const int uFirst = (kPolyUnison - unisonLimit_) / 2, uLast = uFirst + unisonLimit_;
    // The LFO's pitch destination rides here, on the same 16-sample grid the portamento steps on:
    // a factor on the note's frequency, so it reaches the second oscillator's interval as well.
    const double vib = lfo2Pitch_ > 0.0f
        ? std::pow(2.0, static_cast<double>(lfo2Pitch_) * static_cast<double>(lfo2Value_) / 1200.0) : 1.0;
    const double f0 = midiToHz(glidePitch_[voice]) * vib;
    const double y = glideY_[voice], scale = glideScale_[voice], fmRatio = glideFmRatio_[voice];
    for (int u = uFirst; u < uLast; ++u) {
        const int s = voice * kPolyUnison + u;
        const double hz = f0 * slotHzMul_[s] * (1.0 + kSupersawOffsets[u] * slotSpread_[s] * y * scale) * driftFactorHeld_[s];
        const double dt = std::min(hz / sr_, 0.45);
        slots_.dt[s] = static_cast<float>(dt);
        slots_.inv[s] = static_cast<float>(1.0 / dt);
        slots_.mdt[s] = static_cast<float>(std::min(hz * fmRatio / sr_, 0.45));
        wtDt_[s] = hz / sr_;
        wtLevel_[s] = waveLevelFor(hz, sr_, -1);
    }
}

void Poly::advanceGlide()
{
    for (int v = 0; v < kPolyVoices; ++v) {
        if (!amp_[v].isActive()) continue;
        const double d = glideTarget_[v] - glidePitch_[v];
        if (d != 0.0) {
            // Within a thousandth of a semitone the bend has arrived: snapping there keeps the pitch of
            // a long held note exactly the note's, so nothing wobbles under a sustained pad or drone.
            glidePitch_[v] = std::abs(d) < 1.0e-3 ? glideTarget_[v] : glidePitch_[v] + d * glideAlpha_;
            writeSlotPitch(v);
        }
    }
    // The instance's last sounding pitch, for the next note to start from -- and nothing to start from
    // once every voice has fallen silent, so the first note after a rest arrives without a bend.
    lastPitch_ = -1.0;
    for (int v = 0; v < kPolyVoices; ++v) if (amp_[v].isActive()) { lastPitch_ = glidePitch_[newest_]; break; }
}

void Poly::update(const float* v, double bpm)
{
    std::copy(v, v + poly::Count, values_);
    fDecay_ = static_cast<float>(std::exp(std::log(1.0e-3) / (v[poly::FilterDecay] * 0.001 * sr_)));
    // Once, then copied: the same numbers setTimes would give each voice, without eight times three exp().
    ampTimes_.setTimes(v[poly::AmpAttack] * 0.001f, v[poly::AmpDecay] * 0.001f, v[poly::AmpSustain], std::max(0.005f, v[poly::AmpRelease] * 0.001f));
    refreshEnvelopeTimes();
    // waveTable() addresses the built-in tables and the library with one index, and hands back a
    // built-in when a library table's file is missing -- so this stays a pointer swap with no
    // branch on the audio thread and no chance of a null table.
    table_ = &waveTable(static_cast<int>(std::lround(v[poly::Table])));
    posDecay_ = static_cast<float>(std::exp(std::log(1.0e-3) / (std::max(1.0f, v[poly::PosDecay]) * 0.001 * sr_)));
    bpm_ = bpm;
    lfoInc_ = static_cast<float>(bpm / 60.0 / (std::max(0.05f, v[poly::PosLfoBeats]) * sr_));
    // The voice LFO (22.09.2026). Free-running for the instance and tempo-synced, so a held chord
    // breathes as one body and the breathing stays on the bar however long the note is.
    lfo2Inc_ = static_cast<float>(bpm / 60.0 / (std::max(0.05f, v[poly::LfoBeats]) * sr_));
    lfo2Cut_ = v[poly::LfoCutoff];
    lfo2Pitch_ = v[poly::LfoPitch];
    lfo2Amp_ = std::clamp(v[poly::LfoAmp], 0.0f, 1.0f);
    // Thermal drift (Poly.h). The walks step once per kPolyBlock samples, so their corner frequency
    // is kDriftHz against that grid rate; driftNorm_ turns the process's standing deviation,
    // sqrt(alpha / ((2 - alpha) * 3)) for a uniform excitation, into the drift depth in cents.
    const double gridRate = sr_ / static_cast<double>(kPolyBlock);
    driftAlpha_ = static_cast<float>(1.0 - std::exp(-2.0 * kPiD * kDriftHz / gridRate));
    const double sd = std::sqrt(static_cast<double>(driftAlpha_) / ((2.0 - driftAlpha_) * 3.0));
    drift_ = v[poly::Drift];
    driftNorm_ = static_cast<float>(sd > 0.0 ? drift_ / sd : 0.0);
    disperse_.set(static_cast<int>(std::lround(v[poly::Disperse])), v[poly::DisperseFreq], sr_);
    send_ = v[poly::DelaySend];
    level_ = dbToGain(v[poly::Level]);
    // Portamento (Poly.h): a one-pole slew on the pitch, stepped once per kPolyBlock samples like the
    // drift walks. The parameter is the time constant, so after glide ms a step has covered 1 - 1/e of
    // its interval and after 0.693 glide exactly half of it -- which is what the self test measures.
    glideMs_ = std::max(0.0f, v[poly::Glide]);
    const double gridPeriod = static_cast<double>(kPolyBlock) / sr_;
    glideAlpha_ = glideMs_ > 0.0f ? static_cast<float>(1.0 - std::exp(-gridPeriod / (glideMs_ * 0.001))) : 0.0f;
    // Pan, constant power. At the centre cos(pi/4) * sqrt(2) is 1 to the last bit of a double, so a
    // voice that does not pan multiplies by exactly 1.0f and renders as it would without a pan.
    const double pan = std::clamp(static_cast<double>(v[poly::Pan]), -1.0, 1.0);
    const double theta = (pan + 1.0) * kPiD / 4.0;
    panL_ = static_cast<float>(std::cos(theta) * std::sqrt(2.0));
    panR_ = static_cast<float>(std::sin(theta) * std::sqrt(2.0));
    const int dl = std::clamp(static_cast<int>(std::lround(v[poly::DelayLeft])), 0, kNumDelayTimes - 1);
    const int dr = std::clamp(static_cast<int>(std::lround(v[poly::DelayRight])), 0, kNumDelayTimes - 1);
    delay_.set(kDelayBeats[dl], kDelayBeats[dr], bpm, v[poly::DelayFeedback], v[poly::DelayHighPass], v[poly::DelayLowPass]);
}

bool Poly::tableStale() const
{
    return table_ != &waveTable(static_cast<int>(std::lround(values_[poly::Table])));
}

void Poly::voiceCoefs(int voice)
{
    // High pass: fixed for the note. The low pass follows its envelope in renderSegment().
    for (int c = 0; c < 2; ++c)
        svfCoefs(hpHz_[voice], std::sqrt(2.0), sr_, ch_.c1[voice * 2 + c], ch_.c2[voice * 2 + c], ch_.c3[voice * 2 + c]);
}

int Poly::activeVoices() const
{
    int n = 0;
    for (const Envelope& e : amp_) n += e.isActive() ? 1 : 0;
    return n;
}

void Poly::noteOn(int pitch, float velocity, double lengthBeats, int gateSamples, double late, bool accent, bool slide)
{
    const float* v = values_;
    // A free voice, else the one that started longest ago.
    int voice = -1;
    for (int i = 0; i < kPolyVoices && voice < 0; ++i) if (!amp_[i].isActive()) voice = i;
    if (voice < 0) {
        voice = 0;
        for (int i = 1; i < kPolyVoices; ++i) if (age_[i] < age_[voice]) voice = i;
    }
    const bool wasIdle = !amp_[voice].isActive();
    age_[voice] = ++counter_;
    pitch_[voice] = pitch;
    vel_[voice] = clampv(velocity, 0.0f, 1.0f);
    gate_[voice] = std::max(1, gateSamples);
    // The accent (22.09.2026): the filter envelope reaches half again as far for this note, read by
    // lowPassCoefs on every block of the note. 1 for every unaccented note, so nothing else moves.
    accent_[voice] = accent ? 1.5f : 1.0f;

    const int osc = std::clamp(static_cast<int>(std::lround(v[poly::Osc])), 0, static_cast<int>(PolyOsc::Count) - 1);
    const double x = dynamicDetune(v[poly::Detune], v[poly::DynamicDetune], lengthBeats);
    const double y = detuneCurve(x);
    double center = 1.0, side = 0.0;
    mixGains(v[poly::Mix], center, side);
    // Portamento (Poly.h): the note's oscillators start at the pitch this instance was last sounding
    // at and slew to the note's own. Without a glide time, without a note before, or on a repeated
    // pitch there is nothing to slew and the oscillators start where they always did -- glidePitch_ is
    // then bit for bit the note's pitch and every number below is the number without a glide.
    glideTarget_[voice] = static_cast<double>(pitch);
    // `slide` (22.09.2026): a note the composer did not flag starts on its own pitch even with a glide
    // time set -- the lead slides where a tension degree resolves, not on every step.
    const bool bend = slide && glideMs_ > 0.0f && lastPitch_ >= 0.0 && lastPitch_ != static_cast<double>(pitch);
    glidePitch_[voice] = bend ? lastPitch_ : static_cast<double>(pitch);
    glideY_[voice] = y;
    newest_ = voice;
    const double f0 = midiToHz(glidePitch_[voice]);
    // Everything that is a property of the *note* rather than of the moment reads the note's own pitch:
    // the FM index headroom, the tracking high pass and the filter's key tracking. A glide bends the
    // oscillators, not the patch.
    const double f0Note = midiToHz(pitch);
    const double velGain = 1.0 - v[poly::VelSens] + v[poly::VelSens] * vel_[voice];
    const double width = v[poly::Width];
    const double wave = v[poly::Wave];
    // Frequency modulation is band-limited by Carson's rule instead of by oversampling: the significant
    // sidebands of a two-operator pair reach f_c + (I + 1) f_m, so an index above
    // ((0.45 sr) - f_c) / f_m - 1 folds its outer sidebands back as aliasing. Measured on 16.09.2026 at
    // C6 with ratio 3.5: index 10 left aliasing 15.7 dB under the carrier, index 2.5 (the knob's
    // default) 103.8 dB under it; at ratio 7.3 the aliasing was 6 dB *above* the carrier. Two-times
    // oversampling with the half-band only moved that to -39 dB and costs a second oscillator pass,
    // while clamping the index costs nothing and is where the classic instruments draw the line as well
    // (Chowning, "The synthesis of complex audio spectra by means of frequency modulation", JAES 21(7),
    // 1973, section 3, on the bandwidth of the modulated spectrum).
    const double fmRatio = harmonicFmRatio(v[poly::FmRatio]);
    const double fmRoom = std::max(0.0, (0.45 * sr_ - f0Note) / (f0Note * fmRatio) - 1.0);
    const double fmI = std::min(static_cast<double>(v[poly::FmIndex]), fmRoom);
    const float idxDecay = static_cast<float>(std::exp(-3.0 / (std::max(1.0, v[poly::FmDecay] * 0.001 * sr_))));

    double gains[kPolyUnison] = {}, detuneScale = 1.0;
    switch (static_cast<PolyOsc>(osc)) {
    case PolyOsc::Supersaw:
    case PolyOsc::Wavetable:
        for (int u = 0; u < kPolyUnison; ++u) gains[u] = u == 3 ? center : side;
        break;
    case PolyOsc::Va:
        gains[2] = gains[4] = 0.5 * side;
        gains[3] = center;
        break;
    case PolyOsc::Fm:
        gains[2] = gains[4] = 0.35 * side;
        gains[3] = center;
        detuneScale = 0.5;
        break;
    default: break;
    }
    // The unison limit of the quality level (Quality.h). Only the middle unisonLimit_ of the seven
    // oscillators are set up; the rest get gain 0, dt 0 and no source weight, so their slots produce
    // exactly zero and the render loop can leave them out (renderSegment). The power that normalises
    // the mix is then the power of the kept oscillators alone, which is the same renormalisation the
    // level needs: the voice stays as loud as the full unison. At the default limit uFirst is 0 and
    // uLast is kPolyUnison, and every loop below is the loop over all seven it always was.
    const int uFirst = (kPolyUnison - unisonLimit_) / 2, uLast = uFirst + unisonLimit_;

    // The second oscillator (22.09.2026, round "Klangfarben"). A voice is already seven unison slots,
    // each with its own frequency and its own four source weights, so a second oscillator needs no
    // second render pass: it is the outermost pair of those slots given another source, another
    // interval and a share of the power. For the VA and the FM those two slots carry nothing anyway
    // (the switch above leaves gains[0] and gains[6] at zero), so there it is free in the literal
    // sense; for the supersaw and the wavetable it trades the two furthest-detuned copies of the
    // first oscillator for a second one, which is the trade a two-oscillator synth is.
    //
    // The interval is the point rather than the mix: an octave under the note turns a pad into a
    // body, a fifth into an organ, a unison with a few cents of detune into a slow beat -- three
    // sounds a single oscillator cannot make whatever its table is.
    //
    // The pair is the outermost *kept* slots, not slots 0 and 6: under the Quest's unison limit only
    // the middle three are set up at all, and a second oscillator parked on a slot the limit throws
    // away would be inaudible there while still taking its share of the first one's power. It needs
    // three kept slots to exist, so that at least one is left for the first oscillator.
    const int osc2 = std::clamp(static_cast<int>(std::lround(v[poly::Osc2])), 0, static_cast<int>(PolyOsc2::Count) - 1);
    const double mix2 = osc2 == 0 ? 0.0 : std::clamp(static_cast<double>(v[poly::Osc2Mix]), 0.0, 1.0);
    const bool two = mix2 > 0.0 && unisonLimit_ >= 3;
    bool bSlot[kPolyUnison] = {};
    if (two) {
        // Equal-power between the two: the first oscillator keeps 1 - mix2 of the power, the pair
        // splits mix2. The `norm` below then brings the whole voice back to the level it had.
        const double keep = std::sqrt(1.0 - mix2), give = std::sqrt(0.5 * mix2);
        for (int u = 0; u < kPolyUnison; ++u) gains[u] *= keep;
        for (const int u : { uFirst, uLast - 1 }) { bSlot[u] = true; gains[u] = give; }
    }
    const int oscB = two ? osc2 - 1 : 0;   // PolyOsc2 counts Off first; PolyOsc does not
    const int interval = std::clamp(static_cast<int>(std::lround(v[poly::Osc2Interval])),
                                    0, static_cast<int>(PolyOsc2Interval::Count) - 1);
    const double hzMul2 = two ? std::pow(2.0, (static_cast<double>(kOsc2IntervalSemis[interval])
                                               + static_cast<double>(v[poly::Osc2Detune]) / 100.0) / 12.0) : 1.0;

    double power = 0.0;
    for (int u = uFirst; u < uLast; ++u) power += gains[u] * gains[u];
    const double norm = power > 0.0 ? 1.0 / std::sqrt(power) : 0.0;

    // The supersaw reads the mipmapped saw of the Classic table; only the VA blends a PolyBLEP ramp
    // into a pulse. A table frame is normalised to another RMS than the ramp, hence kSawTableGain.
    const bool sup = osc == static_cast<int>(PolyOsc::Supersaw);
    const bool supB = oscB == static_cast<int>(PolyOsc::Supersaw);
    // The fast path (renderSegment: one Classic saw frame, no position, no LFO) needs *every* slot of
    // the voice to be the supersaw. With a second oscillator of another kind the voice takes the
    // general path, where each slot says for itself which table it reads (slotSaw_).
    sawVoice_[voice] = sup && (!two || supB);
    // kSawTableGain is per slot since 22.09.2026: with two oscillators one slot may read the Classic
    // saw frame while its neighbour reads the voice's table, and the two are normalised to different
    // RMS values.
    glideScale_[voice] = detuneScale;
    glideFmRatio_[voice] = fmRatio;

    for (int u = 0; u < kPolyUnison; ++u) {
        const int s = voice * kPolyUnison + u;
        // Szabo: a new random phase for every oscillator at every note. Both draws happen for all
        // seven slots even when the limit plays three, so the oscillators that are kept start on the
        // same phases they would have on the desktop level and the two levels stay comparable.
        const float wtRand = phaseRng_.uniform(), phRand = phaseRng_.uniform();
        if (u < uFirst || u >= uLast) {
            slots_.dt[s] = slots_.inv[s] = slots_.mdt[s] = 0.0f;
            slots_.ph[s] = slots_.mph[s] = 0.0f;
            slots_.idx[s] = slots_.idxFloor[s] = 0.0f;
            slots_.idxDecay[s] = 1.0f;
            slots_.pw[s] = 0.5f;
            slots_.wSaw[s] = slots_.wPulse[s] = slots_.wFm[s] = slots_.wWt[s] = 0.0f;
            slots_.gL[s] = slots_.gR[s] = 0.0f;
            wtPh_[s] = wtDt_[s] = 0.0;
            wtLevel_[s] = 0;
            slotSaw_[s] = false;
            slotHzMul_[s] = 1.0;
            slotSpread_[s] = 1.0;
            continue;
        }
        // Thermal drift: each unison slot has its own walk, so the seven saws of a supersaw age
        // against each other. The walk is read once here and held for the note (Poly.h) -- held in
        // driftFactorHeld_ since 20.09.2026, because a gliding note rewrites its frequencies later and
        // must keep reading the walk it started with.
        driftFactorHeld_[s] = driftFactor(slotDrift(s));
        // Which of the two oscillators this slot is, and at which pitch (22.09.2026).
        const int slotOsc = bSlot[u] ? oscB : osc;
        slotHzMul_[s] = bSlot[u] ? hzMul2 : 1.0;
        slotSpread_[s] = bSlot[u] ? kOsc2Spread : 1.0;
        const double hz = f0 * slotHzMul_[s] * (1.0 + kSupersawOffsets[u] * slotSpread_[s] * y * detuneScale) * driftFactorHeld_[s];
        const double dt = std::min(hz / sr_, 0.45);
        const double mdt = std::min(hz * fmRatio / sr_, 0.45);
        slots_.dt[s] = static_cast<float>(dt);
        slots_.inv[s] = static_cast<float>(1.0 / dt);
        slots_.mdt[s] = static_cast<float>(mdt);
        slots_.pw[s] = v[poly::PulseWidth];
        const bool slotSup = slotOsc == static_cast<int>(PolyOsc::Supersaw);
        const bool fm = slotOsc == static_cast<int>(PolyOsc::Fm);
        const bool wt = slotOsc == static_cast<int>(PolyOsc::Wavetable) || slotSup;
        slotSaw_[s] = slotSup;
        slots_.wWt[s] = wt ? 1.0f : 0.0f;
        wtDt_[s] = hz / sr_;
        wtLevel_[s] = waveLevelFor(hz, sr_, -1);
        {
            const double wp = static_cast<double>(wtRand) + wtDt_[s] * late;
            wtPh_[s] = wp - std::floor(wp);
        }
        slots_.wSaw[s] = (fm || wt) ? 0.0f : static_cast<float>(slotOsc == static_cast<int>(PolyOsc::Va) ? 1.0 - wave : 1.0);
        slots_.wPulse[s] = slotOsc == static_cast<int>(PolyOsc::Va) ? static_cast<float>(wave) : 0.0f;
        slots_.wFm[s] = fm ? 1.0f : 0.0f;
        slots_.idxFloor[s] = static_cast<float>(0.3 * fmI);
        slots_.idxDecay[s] = idxDecay;
        const double theta = (std::clamp(width * kUnisonPan[u], -1.0, 1.0) + 1.0) * kPiD / 4.0;
        const double g = gains[u] * norm * velGain * (slotSup ? static_cast<double>(kSawTableGain) : 1.0);
        slots_.gL[s] = static_cast<float>(g * std::cos(theta) * std::sqrt(2.0));
        slots_.gR[s] = static_cast<float>(g * std::sin(theta) * std::sqrt(2.0));
        // `late` samples have already passed since the note ideally started.
        double ph = static_cast<double>(phRand) + dt * late;
        slots_.ph[s] = static_cast<float>(ph - std::floor(ph));
        const double mp = mdt * late;
        slots_.mph[s] = static_cast<float>(mp - std::floor(mp));
        slots_.idx[s] = static_cast<float>(0.7 * fmI * std::pow(static_cast<double>(idxDecay), late));
    }
    hpHz_[voice] = std::max(v[poly::HpFloor], static_cast<float>(v[poly::HpTrack] * f0Note));
    if (wasIdle) {
        for (int c = 0; c < 2; ++c) {
            const int l = voice * 2 + c;
            ch_.ic1[l] = ch_.ic2[l] = ch_.ha1[l] = ch_.ha2[l] = ch_.hb1[l] = ch_.hb2[l] = 0.0f;
        }
    }
    voiceCoefs(voice);
    fenv_[voice] = static_cast<float>(std::pow(static_cast<double>(fDecay_), late));
    posEnv_[voice] = static_cast<float>(std::pow(static_cast<double>(posDecay_), late));
    lfoPh_[voice] = 0.0;
    // The voice's own walk: the filter moves by a quarter of the pitch deviation and the attack by
    // one per cent per cent, both held for the note (Poly.h). update() has already given every voice
    // the written envelope times; this replaces them for this voice only, so a voice with drift 0
    // keeps exactly the times update() set.
    const double vDrift = voiceDrift(voice);
    driftOct_[voice] = static_cast<float>(vDrift * kDriftCutoffRatio / 1200.0);
    if (drift_ > 0.0f) {
        const float attack = v[poly::AmpAttack] * 0.001f * static_cast<float>(1.0 + vDrift * kDriftAttackRatio);
        amp_[voice].setTimes(std::max(1.0e-5f, attack), v[poly::AmpDecay] * 0.001f, v[poly::AmpSustain],
                             std::max(0.005f, v[poly::AmpRelease] * 0.001f));
    }
    lowPassCoefs(voice, 2.0 - 1.9 * std::clamp(static_cast<double>(v[poly::Resonance]), 0.0, 1.0));
    amp_[voice].noteOn();
    amp_[voice].advanceAttack(late);
}

template <class V>
void Poly::renderSegment(float* L, float* R, int n)
{
    const int width = laneWidth<V>();
    const float* v = values_;
    const double damping = 2.0 - 1.9 * std::clamp(static_cast<double>(v[poly::Resonance]), 0.0, 1.0);
    // The drift walks step on the absolute grid, for every slot and voice whether it sounds or not:
    // a walk that only advanced while a note was held would depend on the note history and would no
    // longer be a function of the sample index. processWith() cuts every segment at the grid, so this
    // runs exactly once per kPolyBlock samples however the host splits its blocks.
    if (drift_ > 0.0f && pos_ % kPolyBlock == 0) advanceDrift();
    // The portamento steps on the same grid and for the same reason (Poly.h): a bend that advanced with
    // the host's block boundaries would not be a function of the sample index. At glide 0 nothing here
    // runs and no slot coefficient is touched.
    if (glideMs_ > 0.0f && pos_ % kPolyBlock == 0) advanceGlide();
    // The voice LFO (22.09.2026). It steps on the same absolute grid and for the same reason: a
    // modulation that advanced with the host's block boundaries would not be a function of the sample
    // index, and the offline render is the determinism oracle. One sine per 16 samples for the whole
    // instance, whose value the cutoff (lowPassCoefs), the pitch (writeSlotPitch) and the level (the
    // amplitude loop below) all read -- which is why "kostet praktisch nichts" is literally true here.
    const bool lfoOn = lfo2Cut_ > 0.0f || lfo2Pitch_ > 0.0f || lfo2Amp_ > 0.0f;
    if (lfoOn && pos_ % kPolyBlock == 0) {
        lfo2Value_ = static_cast<float>(std::sin(2.0 * kPiD * lfo2Ph_));
        lfo2Ph_ += static_cast<double>(lfo2Inc_) * kPolyBlock;
        lfo2Ph_ -= std::floor(lfo2Ph_);
        // The pitch destination has to reach the slots, which only writeSlotPitch touches.
        if (lfo2Pitch_ > 0.0f)
            for (int voice = 0; voice < kPolyVoices; ++voice) if (amp_[voice].isActive()) writeSlotPitch(voice);
    }
    // Tremolo: a gain between 1 - depth and 1, so the LFO can only take level away and a voice never
    // gets louder than the level match measured it at.
    const float lfoGain = lfo2Amp_ > 0.0f
        ? 1.0f - lfo2Amp_ * 0.5f * (1.0f - lfo2Value_) : 1.0f;
    bool voiceOn[kPolyVoices] = {};
    for (int voice = 0; voice < kPolyVoices; ++voice) {
        voiceOn[voice] = amp_[voice].isActive();
        if (!voiceOn[voice]) {
            for (int i = 0; i < n; ++i) chanAmp_[static_cast<size_t>(i * kPolyLanes + voice * 2)] = chanAmp_[static_cast<size_t>(i * kPolyLanes + voice * 2 + 1)] = 0.0f;
            continue;
        }
        // Low pass from the envelope, renewed on the absolute 16-sample grid only: where the host cuts
        // its blocks must not decide when a coefficient changes.
        if (pos_ % kPolyBlock == 0) lowPassCoefs(voice, damping);
        for (int i = 0; i < n; ++i) {
            if (gate_[voice] > 0 && --gate_[voice] == 0) amp_[voice].noteOff();
            const float a = amp_[voice].process() * lfoGain;
            chanAmp_[static_cast<size_t>(i * kPolyLanes + voice * 2)] = a;
            chanAmp_[static_cast<size_t>(i * kPolyLanes + voice * 2 + 1)] = a;
            fenv_[voice] *= fDecay_;
        }
    }
    // Wavetable rows, on the scalar side (a table read is a gather): position from the knob, the
    // voice's envelope and its LFO, one position per voice per sample. This is the most expensive
    // scalar work of the engine, so the unison limit of the quality level is worth the most here:
    // only the middle unisonLimit_ slots of a voice are read, the rest are stores of zero.
    const int uFirst = (kPolyUnison - unisonLimit_) / 2, uLast = uFirst + unisonLimit_;
    for (int voice = 0; voice < kPolyVoices; ++voice) {
        const int s0 = voice * kPolyUnison;
        // The source weights are read from the slots inside the limit: noteOn() clears the weights of
        // the slots outside it, so slot 0 of a limited voice says nothing about its oscillator type.
        // Any of them, not just the first: with a second oscillator (22.09.2026) the outer pair can
        // read tables while the middle does not, or the other way round.
        bool wt = false;
        for (int u = uFirst; u < uLast && !wt; ++u) wt = slots_.wWt[s0 + u] != 0.0f;
        wt = wt && voiceOn[voice];
        if (!wt) {
            for (int i = 0; i < n; ++i)
                for (int u = 0; u < kPolyUnison; ++u) wtRow_[static_cast<size_t>(i * kPolySlots + s0 + u)] = 0.0f;
            continue;
        }
        // The silent slots share their lane group with kept ones, so their row must hold a number
        // rather than what the last note left there -- one store against a mipmap choice and a
        // Catmull-Rom read.
        if (unisonLimit_ < kPolyUnison) {
            for (int i = 0; i < n; ++i) {
                for (int u = 0; u < uFirst; ++u) wtRow_[static_cast<size_t>(i * kPolySlots + s0 + u)] = 0.0f;
                for (int u = uLast; u < kPolyUnison; ++u) wtRow_[static_cast<size_t>(i * kPolySlots + s0 + u)] = 0.0f;
            }
        }
        tableReads_ += static_cast<uint64_t>(n) * static_cast<uint64_t>(unisonLimit_);
        if (sawVoice_[voice]) {
            // Supersaw: one frame, no position, no LFO -- a single Catmull-Rom read per slot.
            for (int i = 0; i < n; ++i) {
                for (int u = uFirst; u < uLast; ++u) {
                    const int s = s0 + u;
                    wtRow_[static_cast<size_t>(i * kPolySlots + s)] = sawTable_->sample(wtLevel_[s], kClassicSawFrame, wtPh_[s]);
                    wtPh_[s] += wtDt_[s];
                    if (wtPh_[s] >= 1.0) wtPh_[s] -= 1.0;
                }
            }
            continue;
        }
        for (int i = 0; i < n; ++i) {
            const float lfo = static_cast<float>(std::sin(2.0 * kPiD * lfoPh_[voice]));
            const float pos = v[poly::Position] + v[poly::PosEnv] * posEnv_[voice] + v[poly::PosLfoDepth] * lfo;
            posEnv_[voice] *= posDecay_;
            lfoPh_[voice] += lfoInc_;
            if (lfoPh_[voice] >= 1.0) lfoPh_[voice] -= 1.0;
            for (int u = uFirst; u < uLast; ++u) {
                const int s = s0 + u;
                // Per slot since 22.09.2026: a second oscillator may be the supersaw while the first
                // is a wavetable, and the supersaw reads one frame of the Classic table whatever the
                // Table parameter says (the DSP round's aliasing figures depend on that frame).
                wtRow_[static_cast<size_t>(i * kPolySlots + s)] =
                    slotSaw_[s] ? sawTable_->sample(wtLevel_[s], kClassicSawFrame, wtPh_[s])
                                : table_->sampleAt(wtLevel_[s], pos, wtPh_[s]);
                wtPh_[s] += wtDt_[s];
                if (wtPh_[s] >= 1.0) wtPh_[s] -= 1.0;
            }
        }
    }
    // Groups of eight slots and eight channels run when any of their voices sounds: the same decision
    // on every vector path. A slot outside the unison limit is silent whatever its voice does, so it
    // no longer keeps its group alive. With seven slots per voice and the middle three kept, every
    // one of the seven groups still holds kept slots of one or two voices, so the limit frees a group
    // only when those voices are silent -- a denser layout (voice x unisonLimit_) would free four of
    // the seven outright, but it moves the lane assignment and with it the bit-identity of the vector
    // tests (docs/rounds/2026-09.md).
    for (int g = 0; g < kPolySlots / 8; ++g) {
        bool on = false, blep = false, fm = false, wt = false;
        for (int s = g * 8; s < g * 8 + 8; ++s) {
            const int u = s % kPolyUnison;
            on = on || (voiceOn[s / kPolyUnison] && u >= uFirst && u < uLast);
            blep = blep || slots_.wSaw[s] != 0.0f || slots_.wPulse[s] != 0.0f;
            fm = fm || slots_.wFm[s] != 0.0f;
            wt = wt || slots_.wWt[s] != 0.0f;
        }
        if (on) {
            for (int s = g * 8; s < g * 8 + 8; s += width) polySlotKernel<V>(slots_, s, n, wtRow_.data(), blep, fm, wt, slotL_.data(), slotR_.data());
        } else {
            for (int i = 0; i < n; ++i)
                for (int s = g * 8; s < g * 8 + 8; ++s) slotL_[static_cast<size_t>(i * kPolySlots + s)] = slotR_[static_cast<size_t>(i * kPolySlots + s)] = 0.0f;
        }
    }
    for (int voice = 0; voice < kPolyVoices; ++voice) {
        for (int i = 0; i < n; ++i) {
            float sl = 0.0f, sr = 0.0f;
            for (int u = uFirst; u < uLast; ++u) {   // the slots outside the limit are exactly zero
                sl += slotL_[static_cast<size_t>(i * kPolySlots + voice * kPolyUnison + u)];
                sr += slotR_[static_cast<size_t>(i * kPolySlots + voice * kPolyUnison + u)];
            }
            chanIn_[static_cast<size_t>(i * kPolyLanes + voice * 2)] = sl;
            chanIn_[static_cast<size_t>(i * kPolyLanes + voice * 2 + 1)] = sr;
        }
    }
    for (int g = 0; g < kPolyLanes / 8; ++g) {
        bool on = false;
        for (int l = g * 8; l < g * 8 + 8; ++l) on = on || voiceOn[l / 2];
        if (on) {
            for (int l = g * 8; l < g * 8 + 8; l += width) polyChannelKernel<V>(ch_, l, n, chanIn_.data(), chanAmp_.data(), chanOut_.data());
        } else {
            for (int i = 0; i < n; ++i)
                for (int l = g * 8; l < g * 8 + 8; ++l) chanOut_[static_cast<size_t>(i * kPolyLanes + l)] = 0.0f;
        }
    }
    for (int i = 0; i < n; ++i) {
        float l = 0.0f, r = 0.0f;
        for (int voice = 0; voice < kPolyVoices; ++voice) {
            l += chanOut_[static_cast<size_t>(i * kPolyLanes + voice * 2)];
            r += chanOut_[static_cast<size_t>(i * kPolyLanes + voice * 2 + 1)];
        }
        if (disperse_.stages > 0) {
            l = dispL_.tick(l, disperse_.c, disperse_.d, disperse_.stages);
            r = dispR_.tick(r, disperse_.c, disperse_.d, disperse_.stages);
        }
        // The delay send is taken from the *unpanned* sum, so a voice's place in the image and its
        // delay's own left/right times stay independent (Poly.h). At the centre panL_ and panR_ are
        // exactly 1 and both lines are the lines without a pan.
        sendBuf_[static_cast<size_t>(i)] = 0.5f * (l * level_ + r * level_) * send_;
        L[i] = l * level_ * panL_;
        R[i] = r * level_ * panR_;
    }
    delay_.process(sendBuf_.data(), L, R, n);
    pos_ += static_cast<uint64_t>(n);
}

void Poly::lowPassCoefs(int voice, double damping)
{
    const float* v = values_;
    const double oct = v[poly::EnvAmount] * fenv_[voice] * accent_[voice] + v[poly::KeyTrack] * (pitch_[voice] - 60) / 12.0
                     + static_cast<double>(driftOct_[voice])
                     + static_cast<double>(lfo2Cut_) * static_cast<double>(lfo2Value_);
    const double fc = std::min(static_cast<double>(v[poly::Cutoff]) * std::pow(2.0, oct), 0.45 * sr_);
    svfCoefs(fc, damping, sr_, ch_.a1[voice * 2], ch_.a2[voice * 2], ch_.a3[voice * 2]);
    ch_.a1[voice * 2 + 1] = ch_.a1[voice * 2];
    ch_.a2[voice * 2 + 1] = ch_.a2[voice * 2];
    ch_.a3[voice * 2 + 1] = ch_.a3[voice * 2];
    // The response (PolyKernel.h, PolyChannels::m0): all zero is the low pass.
    const float k = static_cast<float>(damping);
    float m0 = 0.0f, m1 = 0.0f, m2 = 0.0f;
    switch (static_cast<PolyFilter>(std::clamp(static_cast<int>(std::lround(v[poly::FilterType])), 0, static_cast<int>(PolyFilter::Count) - 1))) {
    case PolyFilter::BandPass: m1 = k; m2 = -1.0f; break;
    case PolyFilter::HighPass: m0 = 1.0f; m1 = -k; m2 = -2.0f; break;
    case PolyFilter::Notch:    m0 = 1.0f; m1 = -k; m2 = -1.0f; break;
    default: break;
    }
    for (int c = 0; c < 2; ++c) { ch_.m0[voice * 2 + c] = m0; ch_.m1[voice * 2 + c] = m1; ch_.m2[voice * 2 + c] = m2; }
}

template <class V>
void Poly::processWith(float* L, float* R, int n)
{
    int done = 0;
    while (done < n) {
        const int m = std::min(static_cast<int>(kPolyBlock - pos_ % kPolyBlock), n - done);
        renderSegment<V>(L + done, R + done, m);
        done += m;
    }
}

template void Poly::processWith<float>(float*, float*, int);
#if PHOS_VEC_PATH != 0
template void Poly::processWith<VecF>(float*, float*, int);
#endif

void Poly::process(float* L, float* R, int n)
{
    processWith<VecF>(L, R, n);
}

} // namespace phos
