/**
 * @file vectest.cpp
 * @brief Lane paths against the scalar reference, bit for bit.
 *
 * Built once per vector path (Tests/CMakeLists.txt): AVX2, NEON through the x86 shim, and scalar.
 * Every lane of every vector operation, and of the ladder and the half-band filters run as lane
 * templates, must equal the float instantiation exactly -- not within a tolerance. If this ever
 * needs a tolerance, an operation has crept in that is not a single IEEE operation per lane.
 */
#include "phos/DiodeLadder.h"
#include "phos/Halfband.h"
#include "phos/Ladder.h"
#include "phos/Perc.h"
#include "phos/Poly.h"
#include "phos/Vec.h"
#include "TestSupport.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>

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
            // A different oscillator and filter every so often, the same on both.
            p.parseText(fmt("lead.osc=%d lead.resonance=%.2f lead.fm_index=%d lead.wave=0.5", (block / 250) % 3, 0.1 + 0.07 * (block / 250 % 10), block / 250 % 7).c_str());
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
    check(bad == 0 && energy > 1.0, "56 oscillator slots and 16 voice channels identical to scalar (supersaw, VA, FM)", fmt("%d differing samples, energy %.1f", bad, energy));

    // Cost, not a check: eight voices of a sustained supersaw, ten seconds.
    auto time = [&](bool vec) {
        auto e = std::make_unique<Poly>();
        e->prepare(48000.0);
        ParamStore q;
        q.parseText("lead.amp_sustain=1 lead.delay_send=0.3");
        std::vector<float> v(static_cast<size_t>(poly::Count));
        q.readModule(Module::Poly, 0, v.data());
        e->update(v.data(), 145.0);
        for (int k = 0; k < kPolyVoices; ++k) e->noteOn(60 + 3 * k, 1.0f, 4.0, 1 << 30, 0.0);
        std::vector<float> L(32), R(32);
        const auto t0 = std::chrono::steady_clock::now();
        for (int block = 0; block < 15000; ++block) {
            if (vec) e->processWith<VecF>(L.data(), R.data(), 32);
            else e->processWith<float>(L.data(), R.data(), 32);
        }
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    const double ts = time(false), tv = time(true);
    std::printf("         cost of 10 s, eight supersaw voices: scalar %.1f %% of a core, %s %.1f %% (x%.2f)\n",
                ts * 10.0, kVecPathName, tv * 10.0, ts / tv);
}

} // namespace

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
    return finish();
}
