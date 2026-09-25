/**
 * @file vectest.cpp
 * @brief Lane paths against the scalar reference, bit for bit.
 *
 * Built once per vector path (Tests/CMakeLists.txt): AVX2, NEON through the x86 shim, and scalar.
 * Every lane of every vector operation, and of the ladder and the half-band filters run as lane
 * templates, must equal the float instantiation exactly -- not within a tolerance. If this ever
 * needs a tolerance, an operation has crept in that is not a single IEEE operation per lane.
 */
#include "phos/Acid.h"
#include "phos/Bass.h"
#include "phos/DiodeLadder.h"
#include "phos/Kick.h"
#include "phos/Halfband.h"
#include "phos/Ladder.h"
#include "phos/Model.h"
#include "phos/Perc.h"
#include "phos/Poly.h"
#include "phos/Vec.h"
#include "TestSupport.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace phos;
using namespace phostest;

namespace {

constexpr int W = kVecWidth;

/** @brief Deterministic test values covering signs, tiny and large magnitudes. */
float testValue(uint32_t i)
{
    uint32_t h = i * 2654435761u ^ 0x9E3779B9u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    const float u = static_cast<float>(h) / 4294967296.0f;
    switch (i % 5) {
    case 0:  return u * 2.0f - 1.0f;
    case 1:  return (u * 2.0f - 1.0f) * 1.0e-6f;
    case 2:  return (u * 2.0f - 1.0f) * 1000.0f;
    case 3:  return u + 0.5f;
    default: return -(u + 0.25f);
    }
}

bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

/** @brief Vector test: vector operations, lane by lane. */
void testOps()
{
    section("vector operations, lane by lane");
    int bad = 0, total = 0;
    float a[8], b[8], c[8];
    for (uint32_t round = 0; round < 2000; ++round) {
        for (int i = 0; i < 8; ++i) { a[i] = testValue(round * 8 + i); b[i] = testValue(round * 8 + i + 11); c[i] = testValue(round * 8 + i + 23); }
        const VecF va = loadLanes<VecF>(a), vb = loadLanes<VecF>(b), vc = loadLanes<VecF>(c);
        const VecF r[] = {
            va + vb, va - vb, va * vb, va / vb, vfmadd(va, vb, vc), vfnmadd(va, vb, vc),
            vmin(va, vb), vmax(va, vb), vsqrt(vabs(va)), vabs(va), vfloor(va), vselect(vlt(va, vb), vc, va),
        };
        for (int l = 0; l < W; ++l) {
            const float x = a[l], y = b[l], z = c[l];
            const float s[] = {
                x + y, x - y, x * y, x / y, vfmadd(x, y, z), vfnmadd(x, y, z),
                vmin(x, y), vmax(x, y), vsqrt(vabs(x)), vabs(x), vfloor(x), vselect(vlt(x, y), z, x),
            };
            for (size_t k = 0; k < sizeof(s) / sizeof(s[0]); ++k) {
                ++total;
                if (!sameBits(laneOf(r[k], l), s[k])) ++bad;
            }
        }
    }
    check(bad == 0, "12 operations identical to scalar", fmt("%d of %d lanes differ", bad, total));
}

/** @brief Vector test: ladder lanes against the scalar ladder. */
void testLadder()
{
    section("ladder lanes against the scalar ladder");
    LadderT<VecF> vl;
    LadderT<float> sl[8];
    vl.reset();
    for (auto& s : sl) s.reset();
    int bad = 0;
    float maxAbs = 0.0f;
    float x[8], g[8], k[8], comp[8];
    for (uint32_t n = 0; n < 20000; ++n) {
        for (int l = 0; l < 8; ++l) {
            // A different saw, cutoff sweep and resonance per lane; loud enough to saturate.
            const float ph = static_cast<float>((n * (l + 3)) % 97) / 97.0f;
            x[l] = (2.0f * ph - 1.0f) * (0.5f + static_cast<float>(l));
            g[l] = 0.01f + 0.6f * (0.5f + 0.5f * std::sin(0.001f * static_cast<float>(n) * static_cast<float>(l + 1)));
            k[l] = static_cast<float>(l) * 0.55f;
            comp[l] = 0.5f;
        }
        const VecF y = vl.tick(loadLanes<VecF>(x), loadLanes<VecF>(g), loadLanes<VecF>(k), loadLanes<VecF>(comp));
        for (int l = 0; l < W; ++l) {
            const float ys = sl[l].tick(x[l], g[l], k[l], comp[l]);
            if (!sameBits(laneOf(y, l), ys)) ++bad;
            maxAbs = std::max(maxAbs, std::fabs(ys));
        }
    }
    check(bad == 0, "ladder output identical to scalar", fmt("%d differing samples", bad));
    check(std::isfinite(maxAbs) && maxAbs < 50.0f, "ladder bounded under drive and resonance", fmt("max |y| = %.3f", static_cast<double>(maxAbs)));
}

/** @brief Vector test: half-band lanes against scalar. */
void testHalfband()
{
    section("half-band lanes against scalar");
    const HalfbandDesign d = designHalfband(96.0, 0.1);
    HalfbandDown<VecF> vd;
    HalfbandUp<VecF> vu;
    HalfbandDown<float> sd[8];
    HalfbandUp<float> su[8];
    vd.setup(d); vu.setup(d);
    for (auto& s : sd) s.setup(d);
    for (auto& s : su) s.setup(d);
    int bad = 0;
    float a[8], b[8];
    for (uint32_t n = 0; n < 5000; ++n) {
        for (int l = 0; l < 8; ++l) { a[l] = testValue(n * 8 + l); b[l] = testValue(n * 8 + l + 5); }
        const VecF yd = vd.process(loadLanes<VecF>(a), loadLanes<VecF>(b));
        VecF u0, u1;
        vu.process(loadLanes<VecF>(a), u0, u1);
        for (int l = 0; l < W; ++l) {
            float s0, s1;
            su[l].process(a[l], s0, s1);
            if (!sameBits(laneOf(yd, l), sd[l].process(a[l], b[l]))) ++bad;
            if (!sameBits(laneOf(u0, l), s0) || !sameBits(laneOf(u1, l), s1)) ++bad;
        }
    }
    check(bad == 0, "decimator and interpolator identical to scalar", fmt("%d differing samples", bad));
}

/** @brief Vector test: percussion kit lanes against the scalar kit. */
void testPerc()
{
    section("percussion kit lanes against the scalar kit");
    const DenormalGuard guard;
    ParamStore p;
    // The default kit, with every lane pushed somewhere else so all engines and filters differ.
    p.parseText("perc1.drive=0.7 perc3.fm_index=3 perc3.engine=FM perc4.resonance=0.9 perc6.pitch_amount=6 "
                "perc9.mode_set=Bar perc10.filter=Band Pass perc11.fm_index=8 perc12.engine=Modal perc5.bursts=6");
    auto a = std::make_unique<PercKit>(), b = std::make_unique<PercKit>();
    a->prepare(48000.0);
    b->prepare(48000.0);
    for (int l = 0; l < kPercLanes; ++l) {
        std::vector<float> v(static_cast<size_t>(perc::Count));
        p.readModule(Module::Perc, l, v.data());
        a->update(l, v.data(), 6, 1);
        b->update(l, v.data(), 6, 1);
    }
    int bad = 0;
    double energy = 0.0;
    std::vector<float> aL(64), aR(64), bL(64), bR(64);
    for (int block = 0; block < 1500; ++block) {
        // Hits on changing lanes, with velocities, shifts and sub-sample offsets.
        if (block % 7 == 0) {
            const int lane = (block / 7) % kPercLanes;
            const float vel = 0.4f + 0.05f * static_cast<float>(block % 13);
            const int shift = (block % 5) - 2;
            const double late = static_cast<double>(block % 10) / 10.0;
            a->trigger(lane, vel, shift, late);
            b->trigger(lane, vel, shift, late);
        }
        const int n = 17 + block % 48;
        a->processWith<float>(aL.data(), aR.data(), n);
        b->processWith<VecF>(bL.data(), bR.data(), n);
        for (int i = 0; i < n; ++i) {
            if (!sameBits(aL[static_cast<size_t>(i)], bL[static_cast<size_t>(i)]) || !sameBits(aR[static_cast<size_t>(i)], bR[static_cast<size_t>(i)])) ++bad;
            energy += static_cast<double>(aL[static_cast<size_t>(i)]) * aL[static_cast<size_t>(i)];
        }
    }
    check(bad == 0 && energy > 1.0, "twelve lanes identical to the scalar kit", fmt("%d differing samples, energy %.1f", bad, energy));

    // Cost, not a check: ten seconds of all twelve lanes kept busy, scalar against this path.
    auto time = [&](bool vec) {
        auto k = std::make_unique<PercKit>();
        k->prepare(48000.0);
        for (int l = 0; l < kPercLanes; ++l) {
            std::vector<float> v(static_cast<size_t>(perc::Count));
            p.readModule(Module::Perc, l, v.data());
            k->update(l, v.data(), 6, 1);
        }
        std::vector<float> L(32), R(32);
        const auto t0 = std::chrono::steady_clock::now();
        for (int block = 0; block < 15000; ++block) {
            if (block % 50 == 0) for (int l = 0; l < kPercLanes; ++l) k->trigger(l, 1.0f, 0, 0.0);
            if (vec) k->processWith<VecF>(L.data(), R.data(), 32);
            else k->processWith<float>(L.data(), R.data(), 32);
        }
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    const double ts = time(false), tv = time(true);
    std::printf("         cost of 10 s, all lanes busy: scalar %.1f %% of a core, %s %.1f %% (x%.2f)\n",
                ts * 10.0, kVecPathName, tv * 10.0, ts / tv);
}

/** @brief Vector test: diode ladder lanes against the scalar diode ladder. */
void testDiodeLadder()
{
    section("diode ladder lanes against the scalar diode ladder");
    DiodeLadderT<VecF> vl;
    DiodeLadderT<float> sl[8];
    vl.reset();
    for (auto& s : sl) s.reset();
    int bad = 0;
    float maxAbs = 0.0f;
    float x[8], g[8], k[8], comp[8];
    for (uint32_t n = 0; n < 20000; ++n) {
        for (int l = 0; l < 8; ++l) {
            const float ph = static_cast<float>((n * (l + 5)) % 89) / 89.0f;
            x[l] = (2.0f * ph - 1.0f) * (0.5f + static_cast<float>(l));
            g[l] = 0.02f + 1.2f * (0.5f + 0.5f * std::sin(0.0007f * static_cast<float>(n) * static_cast<float>(l + 1)));
            k[l] = static_cast<float>(l) * 2.4f;
            comp[l] = 0.3f;
        }
        const VecF y = vl.tick(loadLanes<VecF>(x), loadLanes<VecF>(g), loadLanes<VecF>(k), loadLanes<VecF>(comp));
        for (int l = 0; l < W; ++l) {
            const float ys = sl[l].tick(x[l], g[l], k[l], comp[l]);
            if (!sameBits(laneOf(y, l), ys)) ++bad;
            maxAbs = std::max(maxAbs, std::fabs(ys));
        }
    }
    check(bad == 0, "diode ladder output identical to scalar", fmt("%d differing samples", bad));
    check(std::isfinite(maxAbs) && maxAbs < 50.0f, "diode ladder bounded under drive and resonance", fmt("max |y| = %.3f", static_cast<double>(maxAbs)));
}

/** @brief Vector test: polyphonic engine lanes against the scalar engine. */
void testPoly()
{
    section("polyphonic engine lanes against the scalar engine");
    const DenormalGuard guard;
    ParamStore p;
    auto a = std::make_unique<Poly>(), b = std::make_unique<Poly>();
    a->prepare(48000.0);
    b->prepare(48000.0);
    int bad = 0;
    double energy = 0.0;
    std::vector<float> aL(64), aR(64), bL(64), bR(64);
    for (int block = 0; block < 3000; ++block) {
        if (block % 250 == 0) {
            // A different oscillator and filter every so often, the same on both. Thermal drift and
            // the disperser are switched on here on purpose (16.09.2026): the drift moves the phase
            // step of every one of the 56 slots and the disperser sits on the summed output, and both
            // are computed on the scalar side precisely so that the lane paths cannot diverge -- a run
            // with them off would never notice if one day they did.
            p.parseText(fmt("lead.osc=%d lead.table=%d lead.pos_env=0.4 lead.pos_lfo_beats=0.5 lead.resonance=%.2f lead.fm_index=%d "
                            "lead.wave=0.5 lead.drift=4 lead.disperse=%d lead.disperse_freq=900",
                            (block / 250) % 4, (block / 250) % 6, 0.1 + 0.07 * (block / 250 % 10), block / 250 % 7,
                            1 + (block / 250) % 8).c_str());
            std::vector<float> v(static_cast<size_t>(poly::Count));
            p.readModule(Module::Poly, 0, v.data());
            a->update(v.data(), 145.0);
            b->update(v.data(), 145.0);
        }
        if (block % 9 == 0) {
            const int pitch = 55 + (block * 7) % 30;
            const double late = static_cast<double>(block % 10) / 10.0;
            a->noteOn(pitch, 0.8f, 0.25 * (1 + block % 8), 2000 + block % 5000, late);
            b->noteOn(pitch, 0.8f, 0.25 * (1 + block % 8), 2000 + block % 5000, late);
        }
        const int n = 5 + block % 60;
        a->processWith<float>(aL.data(), aR.data(), n);
        b->processWith<VecF>(bL.data(), bR.data(), n);
        for (int i = 0; i < n; ++i) {
            if (!sameBits(aL[static_cast<size_t>(i)], bL[static_cast<size_t>(i)]) || !sameBits(aR[static_cast<size_t>(i)], bR[static_cast<size_t>(i)])) ++bad;
            energy += static_cast<double>(aL[static_cast<size_t>(i)]) * aL[static_cast<size_t>(i)];
        }
    }
    check(bad == 0 && energy > 1.0, "56 oscillator slots and 16 voice channels identical to scalar (supersaw, VA, FM, wavetable, with drift and disperser)", fmt("%d differing samples, energy %.1f", bad, energy));

    // Cost, not a check: eight voices held for ten seconds, once as the lead and once as the pad,
    // each at the full unison of the desktop level and at the three oscillators of the Quest level
    // (Quality.h). The limited runs go through setQuality() and noteOnLimited(), which is the path
    // Engine::dispatch takes; the voice limit stays at eight so that only the unison differs.
    auto time = [&](const char* settings, PolyInstance inst, bool vec, int unison) {
        auto e = std::make_unique<Poly>();
        e->prepare(48000.0);
        e->setQuality(unison, kPolyVoices);
        ParamStore q;
        q.parseText(settings);
        std::vector<float> v(static_cast<size_t>(poly::Count));
        q.readModule(Module::Poly, static_cast<int>(inst), v.data());
        e->update(v.data(), 145.0);
        for (int k = 0; k < kPolyVoices; ++k) e->noteOnLimited(60 + 3 * k, 1.0f, 4.0, 1 << 30, 0.0);
        std::vector<float> L(32), R(32);
        const auto t0 = std::chrono::steady_clock::now();
        for (int block = 0; block < 15000; ++block) {
            if (vec) e->processWith<VecF>(L.data(), R.data(), 32);
            else e->processWith<float>(L.data(), R.data(), 32);
        }
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    const char* const kLead = "lead.amp_sustain=1 lead.delay_send=0.3";
    const char* const kPad = "pad.amp_sustain=1 pad.amp_attack=1";
    const double ts = time(kLead, PolyInstance::Lead, false, kPolyUnison), tv = time(kLead, PolyInstance::Lead, true, kPolyUnison);
    std::printf("         cost of 10 s, eight supersaw voices: scalar %.1f %% of a core, %s %.1f %% (x%.2f)\n",
                ts * 10.0, kVecPathName, tv * 10.0, ts / tv);
    const double ts3 = time(kLead, PolyInstance::Lead, false, 3), tv3 = time(kLead, PolyInstance::Lead, true, 3);
    std::printf("         cost of 10 s, eight supersaw voices, unison 3 (quest): scalar %.1f %% of a core, %s %.1f %% (x%.2f); %s %+.0f %% against unison 7\n",
                ts3 * 10.0, kVecPathName, tv3 * 10.0, ts3 / tv3, kVecPathName, (tv3 / tv - 1.0) * 100.0);
    const double ps = time(kPad, PolyInstance::Pad, false, kPolyUnison), pv = time(kPad, PolyInstance::Pad, true, kPolyUnison);
    std::printf("         cost of 10 s, eight wavetable pad voices: scalar %.1f %% of a core, %s %.1f %% (x%.2f)\n",
                ps * 10.0, kVecPathName, pv * 10.0, ps / pv);
    const double ps3 = time(kPad, PolyInstance::Pad, false, 3), pv3 = time(kPad, PolyInstance::Pad, true, 3);
    std::printf("         cost of 10 s, eight wavetable pad voices, unison 3 (quest): scalar %.1f %% of a core, %s %.1f %% (x%.2f); %s %+.0f %% against unison 7\n",
                ps3 * 10.0, kVecPathName, pv3 * 10.0, ps3 / pv3, kVecPathName, (pv3 / pv - 1.0) * 100.0);
}

/**
 * @brief The neural model's kernels: the lane instantiation against the scalar one, bit for bit.
 *
 * Every kernel of ModelKernel.h is a template over the lane type, so both instantiations exist in
 * this one executable and can be compared directly -- and because the weights are packed for the
 * lane width the kernel will be run with, the comparison is of the whole contract (pack, then
 * multiply) rather than of one loop. A matrix-vector product is where bit-identity is easiest to
 * lose: the lanes hold output rows and the input dimension is walked in order, so lane l does what
 * the scalar path does for row l, in the same order. If a partial-sum reduction ever creeps in, this
 * is the check that fails.
 */
void testModelKernels()
{
    section("the model's lane kernels against their scalar instantiation");
    constexpr int kRows = 53, kCols = 71;   // neither a multiple of 8 nor of 4

    // The exponential, tanh, GELU: elementwise, so they must agree on every lane.
    int bad = 0, total = 0;
    float in[8], outV[8];
    for (uint32_t round = 0; round < 4000; ++round) {
        for (int i = 0; i < 8; ++i) in[i] = testValue(round * 8 + i) * (round % 3 == 0 ? 30.0f : 1.0f);
        const VecF v = loadLanes<VecF>(in);
        const VecF r[] = { laneExp(v), laneTanh(v), laneGelu(v), lanePow2i(vfloor(v)), laneAct(v, ModelAct::Relu), laneAct(v, ModelAct::Silu) };
        for (int l = 0; l < W; ++l) {
            const float x = in[l];
            const float s[] = { laneExp(x), laneTanh(x), laneGelu(x), lanePow2i(vfloor(x)), laneAct(x, ModelAct::Relu), laneAct(x, ModelAct::Silu) };
            for (size_t k = 0; k < sizeof(s) / sizeof(s[0]); ++k) {
                ++total;
                if (!sameBits(laneOf(r[k], l), s[k])) ++bad;
            }
        }
    }
    check(bad == 0, "exp, tanh, GELU, 2^k, ReLU and SiLU identical to scalar", fmt("%d of %d lanes differ", bad, total));
    (void)outV;

    // The matrix-vector product with the output rows in the lanes, packed each way.
    std::vector<float> w(static_cast<size_t>(kRows) * kCols), b(static_cast<size_t>(roundUpTo(kRows, 8))), x(kCols);
    for (size_t i = 0; i < w.size(); ++i) w[i] = testValue(static_cast<uint32_t>(i) + 7);
    for (size_t i = 0; i < b.size(); ++i) b[i] = testValue(static_cast<uint32_t>(i) + 101);
    for (size_t i = 0; i < x.size(); ++i) x[i] = testValue(static_cast<uint32_t>(i) + 303);
    std::vector<float> panelV(static_cast<size_t>(roundUpTo(kRows, W)) * kCols, 0.0f), panelS(static_cast<size_t>(kRows) * kCols, 0.0f);
    std::vector<float> yv(static_cast<size_t>(roundUpTo(kRows, W)), 0.0f), ys(static_cast<size_t>(kRows), 0.0f);
    packPanels<VecF>(w.data(), nullptr, kRows, kCols, panelV.data());
    packPanels<float>(w.data(), nullptr, kRows, kCols, panelS.data());
    matvecPanel<VecF>(panelV.data(), b.data(), x.data(), kRows, kCols, yv.data());
    matvecPanel<float>(panelS.data(), b.data(), x.data(), kRows, kCols, ys.data());
    int mv = 0;
    for (int i = 0; i < kRows; ++i) if (!sameBits(yv[static_cast<size_t>(i)], ys[static_cast<size_t>(i)])) ++mv;
    check(mv == 0, "matvecPanel identical to scalar for every output row", fmt("%d of %d rows differ", mv, kRows));

    // The same with int8 weights and one scale per row, which is how a quantized model is packed.
    std::vector<int8_t> q(static_cast<size_t>(kRows) * kCols);
    std::vector<float> scale(static_cast<size_t>(kRows));
    for (size_t i = 0; i < q.size(); ++i) q[i] = static_cast<int8_t>(static_cast<int>(testValue(static_cast<uint32_t>(i) + 55) * 90.0f) % 127);
    for (int i = 0; i < kRows; ++i) scale[static_cast<size_t>(i)] = 1e-3f * (1.0f + static_cast<float>(i));
    packPanels<VecF>(q.data(), scale.data(), kRows, kCols, panelV.data());
    packPanels<float>(q.data(), scale.data(), kRows, kCols, panelS.data());
    matvecPanel<VecF>(panelV.data(), b.data(), x.data(), kRows, kCols, yv.data());
    matvecPanel<float>(panelS.data(), b.data(), x.data(), kRows, kCols, ys.data());
    int mq = 0;
    for (int i = 0; i < kRows; ++i) if (!sameBits(yv[static_cast<size_t>(i)], ys[static_cast<size_t>(i)])) ++mq;
    check(mq == 0, "int8 weights with per-row scales identical to scalar", fmt("%d of %d rows differ", mq, kRows));

    // Layer norm, softmax, the accumulate and the scale.
    const int n = 40, nP = roundUpTo(n, 8);
    std::vector<float> xs(static_cast<size_t>(nP)), gw(static_cast<size_t>(nP)), gb(static_cast<size_t>(nP));
    std::vector<float> lv(static_cast<size_t>(nP), 0.0f), ls(static_cast<size_t>(nP), 0.0f);
    for (int i = 0; i < nP; ++i) { xs[static_cast<size_t>(i)] = testValue(static_cast<uint32_t>(i) + 11); gw[static_cast<size_t>(i)] = 1.0f + 0.1f * testValue(static_cast<uint32_t>(i)); gb[static_cast<size_t>(i)] = testValue(static_cast<uint32_t>(i) + 3); }
    laneLayerNorm<VecF>(xs.data(), gw.data(), gb.data(), 1e-5f, n, lv.data());
    laneLayerNorm<float>(xs.data(), gw.data(), gb.data(), 1e-5f, n, ls.data());
    int ln = 0;
    for (int i = 0; i < n; ++i) if (!sameBits(lv[static_cast<size_t>(i)], ls[static_cast<size_t>(i)])) ++ln;

    std::vector<float> sv(xs), ss(xs);
    laneSoftmax<VecF>(sv.data(), n);
    laneSoftmax<float>(ss.data(), n);
    int sm = 0;
    for (int i = 0; i < n; ++i) if (!sameBits(sv[static_cast<size_t>(i)], ss[static_cast<size_t>(i)])) ++sm;

    std::vector<float> av(static_cast<size_t>(nP), 0.25f), as(static_cast<size_t>(nP), 0.25f);
    for (int t = 0; t < 7; ++t) {
        laneAccumulate<VecF>(av.data(), xs.data(), 0.1f * static_cast<float>(t + 1), nP);
        laneAccumulate<float>(as.data(), xs.data(), 0.1f * static_cast<float>(t + 1), nP);
    }
    laneScale<VecF>(av.data(), 0.3f, nP);
    laneScale<float>(as.data(), 0.3f, nP);
    int ac = 0;
    for (int i = 0; i < nP; ++i) if (!sameBits(av[static_cast<size_t>(i)], as[static_cast<size_t>(i)])) ++ac;
    check(ln == 0 && sm == 0 && ac == 0, "layer norm, softmax, accumulate and scale identical to scalar",
          fmt("%d norm, %d softmax, %d accumulate entries differ", ln, sm, ac));

    // Attention: the key cache written in time panels, then scored by the same matmul kernel.
    const int headDim = 24, steps = 37;
    std::vector<float> kv(static_cast<size_t>(roundUpTo(steps, W)) * headDim, 0.0f), ksc(static_cast<size_t>(steps) * headDim, 0.0f);
    std::vector<float> key(static_cast<size_t>(headDim)), qq(static_cast<size_t>(headDim));
    for (int i = 0; i < headDim; ++i) qq[static_cast<size_t>(i)] = testValue(static_cast<uint32_t>(i) + 91);
    for (int t = 0; t < steps; ++t) {
        for (int i = 0; i < headDim; ++i) key[static_cast<size_t>(i)] = testValue(static_cast<uint32_t>(t * headDim + i) + 17);
        storeKey<VecF>(kv.data(), t, key.data(), headDim);
        storeKey<float>(ksc.data(), t, key.data(), headDim);
    }
    std::vector<float> scv(static_cast<size_t>(roundUpTo(steps, W)), 0.0f), scs(static_cast<size_t>(steps), 0.0f);
    matvecPanel<VecF>(kv.data(), nullptr, qq.data(), steps, headDim, scv.data());
    matvecPanel<float>(ksc.data(), nullptr, qq.data(), steps, headDim, scs.data());
    int at = 0;
    for (int t = 0; t < steps; ++t) if (!sameBits(scv[static_cast<size_t>(t)], scs[static_cast<size_t>(t)])) ++at;
    check(at == 0, "attention scores over the time-panelled key cache identical to scalar", fmt("%d of %d steps differ", at, steps));
}

/**
 * @brief The whole forward pass on this path: against the reference vectors, and as bits.
 *
 * NeuralModel is compiled for one lane type, so the three builds of this test cannot compare it
 * against each other inside one process. Each compares itself against the trainer's float64
 * reference instead, and prints its logits as bits so that the three outputs can also be diffed.
 * The cost line is the other half of what this is for: the scalar number is what the Quest's NEON
 * path will resemble. PHOS_MODEL_BENCH points at a larger model than the one in the repository.
 */
void testModelForwardFile(const std::string& file, const char* label, bool bench)
{
    NeuralModel model;
    std::string error;
    if (!model.load(file.c_str(), error)) { check(false, fmt("%s: loads the model", label).c_str(), error); return; }

    // The last case of the reference file (docs/MODEL_FORMAT.md section 5): the conditioning, the
    // tokens and what PyTorch computed. Read with the smallest parser that can read it. `kick` is
    // present only in a four-role file's cases and stays empty otherwise; `mode` only in a file
    // whose header carries condMode, and -1 stands for "the case records none".
    int role = 0, style = 0, bars = 0, modeOf = -1;
    std::vector<int> tok, stepOf, barOf, gapOf, idxOf, kickOf;
    std::vector<double> want;
    {
        std::ifstream in(file + ".ref.txt");
        std::string line;
        auto ints = [](std::istringstream& v, std::vector<int>& dst) { dst.clear(); int x; while (v >> x) dst.push_back(x); };
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t eq = line.find('=');
            if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
            const std::string key = line.substr(0, eq);
            std::istringstream vals(line.substr(eq + 1));
            if (key == "role") vals >> role;
            else if (key == "style") vals >> style;
            else if (key == "bars") vals >> bars;
            else if (key == "mode") vals >> modeOf;   // only in a file with condMode
            else if (key == "tok") ints(vals, tok);
            else if (key == "step") ints(vals, stepOf);
            else if (key == "bar") ints(vals, barOf);
            else if (key == "gap") ints(vals, gapOf);
            else if (key == "idx") ints(vals, idxOf);
            else if (key == "kick") ints(vals, kickOf);
            else if (key == "logits") { want.clear(); double x; while (vals >> x) want.push_back(x); }
        }
    }
    const size_t n = tok.size();
    if (n == 0 || want.empty() || stepOf.size() != n || barOf.size() != n || gapOf.size() != n || idxOf.size() != n
        || (!kickOf.empty() && kickOf.size() != n)) {
        check(false, fmt("%s: reads the reference vectors", label).c_str(), file + ".ref.txt");
        return;
    }
    model.begin(role, style, bars + 1, modeOf < 0 ? 0 : modeOf);
    for (size_t i = 0; i < n; ++i) {
        NoteCond c;
        c.step = stepOf[i];
        c.bar = barOf[i];
        c.gap = gapOf[i];
        c.idx = idxOf[i];
        c.kick = kickOf.empty() ? 0 : kickOf[i];
        if (!model.step(tok[i], c)) { check(false, fmt("%s: feeds the reference context", label).c_str(), fmt("stopped at position %zu", i)); return; }
    }
    double worst = 0.0;
    std::string bits;
    for (size_t c = 0; c < want.size(); ++c) {
        worst = std::max(worst, std::fabs(static_cast<double>(model.logits()[c]) - want[c]));
        uint32_t u;
        const float v = model.logits()[c];
        std::memcpy(&u, &v, 4);
        bits += fmt("%08x", u);
    }
    check(worst < 1e-3, fmt("%s: the whole forward pass matches PyTorch on this path", label).c_str(),
          fmt("%zu positions, largest logit error %.2e (the format allows 1e-3)", n, worst));
    // Printed as bits as well: the three builds of this test are diffed against each other, and the
    // line has to be character for character the same.
    std::printf("         %s logits after %zu positions: %s\n", label, n, bits.c_str());

    // The eleventh embedding table (condMode) is one more laneAdd into the same accumulator, so it
    // has to be bit-identical across the paths for the same reason every other one is -- and it has
    // to *reach* the output, which a wrong row index or a table read past its rows would not. Both
    // are checked here: every row of mode.emb is fed through the same context, the logits are
    // printed as bits for the cross-path diff, and no two rows may leave the same output.
    if (model.info().condMode > 0) {
        std::vector<std::string> perMode;
        for (int md = 0; md < model.info().condMode; ++md) {
            model.begin(role, style, bars + 1, md);
            bool fine = true;
            for (size_t i = 0; i < n && fine; ++i) {
                NoteCond c;
                c.step = stepOf[i];
                c.bar = barOf[i];
                c.gap = gapOf[i];
                c.idx = idxOf[i];
                c.kick = kickOf.empty() ? 0 : kickOf[i];
                fine = model.step(tok[i], c);
            }
            std::string b;
            for (size_t c = 0; c < want.size(); ++c) {
                uint32_t u;
                const float v = model.logits()[c];
                std::memcpy(&u, &v, 4);
                b += fmt("%08x", u);
            }
            perMode.push_back(b);
            std::printf("         %s mode %d logits: %s\n", label, md, b.c_str());
        }
        int same = 0;
        for (size_t i = 0; i < perMode.size(); ++i)
            for (size_t j = i + 1; j < perMode.size(); ++j)
                if (perMode[i] == perMode[j]) ++same;
        check(same == 0, fmt("%s: every row of mode.emb reaches the output on this path", label).c_str(),
              fmt("%zu rows fed through the same context, %d pairs of them with identical logits",
                  perMode.size(), same));
    }
    if (!bench) return;

    const auto t0 = std::chrono::steady_clock::now();
    const int lines = 40, notes = 16;
    for (int d = 0; d < lines; ++d) {
        model.begin(d % 3, 0, 1 + d % 8);
        for (int i = 0; i < notes; ++i) {
            NoteCond c;
            c.step = i % 16;
            c.gap = 1 + i % 8;
            c.idx = noteIndexBucket(i);
            model.step((i * 7 + d) % model.alphabet(), c);
        }
    }
    const double us = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1e6;
    std::printf("         cost %s, %zu parameters: %.2f us per symbol, %.2f ms per 16-note line\n",
                kVecPathName, model.info().parameters, us / (lines * notes), us / lines / 1000.0);
}

/** @brief Vector test: the model's forward pass, on this path, against the trainer's reference vectors. */
void testModelForward()
{
    section("the model's forward pass, on this path, against the trainer's reference vectors");
    const char* bench = std::getenv("PHOS_MODEL_BENCH");
    testModelForwardFile(bench != nullptr ? std::string(bench) : std::string(PHOS_SOURCE_DATA_DIR "/melody.phosmdl"),
                         "melody", true);
    // The bass file is the same architecture with a fourth role and a tenth embedding table, so its
    // extra laneAdd has to be bit-identical across the paths for the same reason every other one is.
    if (bench == nullptr) testModelForwardFile(PHOS_SOURCE_DATA_DIR "/bass.phosmdl", "bass", false);
}

/**
 * @brief Cost of the parts that are not lane templates, for the plan's table.
 *
 * Kick, bass and acid have one voice each, so they are not written over a lane type and the scalar
 * and NEON variants of this executable do not compile them (Tests/CMakeLists.txt gives those
 * variants a fixed source list). They are measured only in the variant that links the core, which is
 * the AVX2 build on x86-64. `phos_render --solo <part>` cannot serve here: --solo is a mute in the
 * mixer, so every engine still renders and every part measures the whole engine.
 */
#if !defined(PHOS_FORCE_SCALAR) && !defined(PHOS_NEON_SHIM)
/** @brief Vector test: cost of kick, bass and acid (no check, a measurement). */
void testParts()
{
    section("cost of kick, bass and acid (no check, a measurement)");
    const DenormalGuard guard;
    ParamStore p;
    const int kb = p.base(Module::Kick), bb = p.base(Module::Bass), ab = p.base(Module::Acid);
    std::vector<float> kv(static_cast<size_t>(kick::Count)), bv(static_cast<size_t>(bass::Count)), av(static_cast<size_t>(acid::Count));
    for (int i = 0; i < kick::Count; ++i) kv[static_cast<size_t>(i)] = p.get(kb + i);
    for (int i = 0; i < bass::Count; ++i) bv[static_cast<size_t>(i)] = p.get(bb + i);
    for (int i = 0; i < acid::Count; ++i) av[static_cast<size_t>(i)] = p.get(ab + i);

    // 145 BPM: a kick every beat (12414 samples), bass and acid on every sixteenth.
    const int beat = 19862, sixteenth = beat / 4;
    Kick kick;
    Bass bass;
    kick.prepare(48000.0);
    bass.prepare(48000.0);
    kick.update(kv.data(), 6);
    bass.update(bv.data());
    std::vector<float> kb2(32), bb2(32);
    auto t0 = std::chrono::steady_clock::now();
    for (int block = 0; block < 15000; ++block) {
        const int n0 = block * 32;
        if (n0 % beat < 32) { kick.trigger(1.0f, 0.0); bass.duck(0.0); }
        if (n0 % sixteenth < 32) bass.noteOn(30 + (block % 5), 0.9f, sixteenth - 200, 0.0, 0.0);
        kick.process(kb2.data(), 32);
        bass.process(bb2.data(), 32);
    }
    const double tkb = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    Acid acid;
    acid.prepare(48000.0);
    acid.update(av.data(), 145.0);
    std::vector<float> aL(32), aR(32);
    t0 = std::chrono::steady_clock::now();
    for (int block = 0; block < 15000; ++block) {
        const int n0 = block * 32;
        if (n0 % sixteenth < 32) acid.noteOn(45 + (block % 12), 0.9f, block % 4 == 0, block % 3 == 0, sixteenth - 100, 0.0);
        acid.process(aL.data(), aR.data(), 32);
    }
    const double ta = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("         cost of 10 s, kick + rolling bass: %s %.2f %% of a core\n", kVecPathName, tkb * 10.0);
    std::printf("         cost of 10 s, acid on every sixteenth: %s %.2f %% of a core\n", kVecPathName, ta * 10.0);
}
#endif

} // namespace

/** @brief Runs every comparison of the vector path against the scalar one. */
int main()
{
    std::printf("phos_vectest: path %s, %d lanes\n", kVecPathName, W);
#if defined(PHOS_EXPECT_PATH)
    check(std::strcmp(kVecPathName, PHOS_EXPECT_PATH) == 0, "built for the expected path", fmt("expected %s, got %s", PHOS_EXPECT_PATH, kVecPathName));
#endif
    testOps();
    testLadder();
    testHalfband();
    testPerc();
    testDiodeLadder();
    testPoly();
    testModelKernels();
    testModelForward();
#if !defined(PHOS_FORCE_SCALAR) && !defined(PHOS_NEON_SHIM)
    testParts();
#endif
    return finish();
}
