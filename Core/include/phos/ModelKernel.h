/**
 * @file ModelKernel.h
 * @brief The hot loops of the neural sequence model, written once over the lane types of Vec.h.
 *
 * Phase 8 puts a small transformer where the order-2 Markov model of Phase 5 stood (docs/PLAN.md,
 * 6.9). Its forward pass is five matrix-vector products and an attention per layer; this header is
 * that arithmetic, and nothing else. It is header-only so that the three lane variants of
 * phos_vectest -- AVX2, the NEON shim and the forced scalar path -- can compile it without linking
 * the core.
 *
 * **The bit-identity rule and what it costs.** Tests/vectest.cpp demands that a lane of the vector
 * path equal the scalar computation bit for bit, and a dot product is exactly the operation that
 * breaks that: eight partial sums added at the end are not the same float as one sum accumulated in
 * order. The way out is to choose what the lanes stand for:
 *
 *  - **Lanes are output rows.** matvecPanel() puts W output neurons in the W lanes and walks the
 *    input dimension sequentially, so lane l performs exactly the multiply-add sequence the scalar
 *    path performs for row l. The weights are therefore stored in "panels": rows are grouped by W
 *    and interleaved, w[(g * cols + c) * W + l] being row g*W+l, column c. Packing happens once when
 *    the model is loaded (Model.cpp), never in the hot loop.
 *  - **Elementwise work is lane-parallel** (the exponential, the activation, the normalisation's
 *    affine part, the accumulation of the value vectors): no lane ever reads another lane.
 *  - **Reductions stay scalar.** A layer norm's mean and variance run over `dim` and a softmax's
 *    total over the context length; both are O(dim) beside matmuls that are O(dim^2), so nothing is
 *    lost by summing them in a plain scalar loop, and a plain scalar loop is trivially identical on
 *    every path. sumOrdered() is deliberately not used here.
 *
 * Attention fits the same two shapes: the scores of one head are K (rows = time steps) times q, so
 * the key cache is stored in panels over *time* and scored with matvecPanel(); the weighted sum of
 * the value vectors is an elementwise accumulate over the head dimension, summed over time in the
 * order a scalar loop would.
 *
 * **The exponential.** Softmax needs one, and @c std::exp is out: it is not a single IEEE operation,
 * and its result differs between the desktop's libm and the Quest's. laneExp() is the classic
 * Cephes range reduction -- exp(x) = 2^k * exp(r) with k = round(x log2 e) and |r| <= ln2/2 -- with
 * the polynomial of Moshier's expf and, in place of the usual bit-fiddling on the exponent field,
 * lanePow2i(): 2^k built from the binary digits of k out of the exactly representable constants
 * 2^-1, 2^-2, 2^-4 ... 2^-64. Multiplying by a power of two is exact, so the only error in the
 * result is the polynomial's. The self test measures it against @c std::exp in double
 * (Tests/selftest.cpp, testModelKernel) and the vector tests measure the three paths against each
 * other bit for bit.
 *
 * @note The transformer is the pre-norm decoder of Radford et al., "Language models are unsupervised
 *       multitask learners" (2019); the musical framing is Music Transformer (Huang et al., ICLR
 *       2019) and Compound Word Transformer (Hsiao et al., AAAI 2021). GELU is the tanh form of
 *       Hendrycks and Gimpel, "Gaussian error linear units", arXiv:1606.08415 (2016).
 */
#pragma once
#include "phos/Vec.h"

namespace phos {

/** @brief Rows per panel on the path this translation unit was built for. */
constexpr int kPanelRows = kVecWidth;

/** @brief @p n rounded up to a whole number of panels of @p w rows. */
constexpr int roundUpTo(int n, int w) { return ((n + w - 1) / w) * w; }

/**
 * @brief 2^k for an integer-valued @p k in [-127, 127], exactly.
 *
 * The usual implementation writes k into the exponent field of a float, which needs integer
 * operations that Vec.h does not offer (and would have to be written three times). Instead the
 * seven binary digits of |k| are peeled off with floor and the matching factors out of 2^-1 .. 2^-64
 * are multiplied together. Every intermediate product is itself a power of two between the result
 * and 1, so no multiplication rounds and none can underflow; the positive side is one exact division
 * by that product. The self test checks all 255 values of k against ldexp bit for bit, because
 * "exact" is the only reason this is worth writing out rather than calling a library.
 *
 * @param k integer-valued lanes; anything outside [-127, 127] gives a wrong answer, so laneExp()
 *          clamps its argument before it gets here.
 */
template <class V>
inline V lanePow2i(V k)
{
    // c[i] = 2^-(2^i), all exactly representable.
    static constexpr float c[7] = { 0.5f, 0.25f, 0.0625f, 3.90625e-3f,
                                    1.52587890625e-5f, 2.3283064365386963e-10f, 5.42101086242752217e-20f };
    const V zero = lanes<V>(0.0f), one = lanes<V>(1.0f), half = lanes<V>(0.5f);
    const auto negative = vlt(k, zero);
    V m = vselect(negative, -k, k);
    V bit[7];
    for (int i = 0; i < 7; ++i) {
        const V h = vfloor(m * half);
        bit[i] = m - (h + h);   // m mod 2, exact for integer-valued m
        m = h;
    }
    V f = one;
    for (int i = 6; i >= 0; --i) f = vselect(vgt(bit[i], half), f * lanes<V>(c[i]), f);
    return vselect(negative, f, one / f);
}

/**
 * @brief exp(@p x) for lanes, from single IEEE operations only.
 *
 * Range reduction exp(x) = 2^k * exp(r), k = round(x log2 e), r = x - k C1 - k C2 with ln2 = C1 + C2
 * split the way Moshier does (C1 = 0.693359375 = 355/512, C2 = -2.12194440e-4), then his degree-6
 * minimax polynomial for exp(r) on |r| <= ln2/2. C1 has nine significant bits, so k * C1 and the
 * subtraction from x are both exact and the only rounding before the polynomial is the small
 * correction k * C2 -- though at the accuracy this needs, the nearest single float to ln2 turns out
 * to do as well: measured, the two splittings give the same worst case. The argument is clamped to
 * +-87, where the result is still a normal float; the softmaxes here subtract their maximum first,
 * so the clamp is never active in practice. Measured against std::exp in double: 8.3e-8 relative,
 * seven tenths of an ulp (Tests/selftest.cpp, testModelKernel).
 */
template <class V>
inline V laneExp(V x)
{
    x = vmin(vmax(x, lanes<V>(-87.0f)), lanes<V>(87.0f));
    const V k = vfloor(vfmadd(x, lanes<V>(1.44269504088896341f), lanes<V>(0.5f)));
    V r = vfnmadd(k, lanes<V>(0.693359375f), x);
    r = vfnmadd(k, lanes<V>(-2.12194440e-4f), r);
    V p = lanes<V>(1.9875691500e-4f);
    p = vfmadd(p, r, lanes<V>(1.3981999507e-3f));
    p = vfmadd(p, r, lanes<V>(8.3334519073e-3f));
    p = vfmadd(p, r, lanes<V>(4.1665795894e-2f));
    p = vfmadd(p, r, lanes<V>(1.6666665459e-1f));
    p = vfmadd(p, r, lanes<V>(5.0000001201e-1f));
    p = vfmadd(p * r, r, r);
    p = p + lanes<V>(1.0f);
    return p * lanePow2i(k);
}

/**
 * @brief tanh(@p x) for lanes, as (e^2x - 1) / (e^2x + 1).
 *
 * Clamped to +-9, where tanh is within 3e-8 of +-1 -- half an ulp of 1.0f -- so the clamp changes no
 * float that a float could tell apart.
 */
template <class V>
inline V laneTanh(V x)
{
    const V c = vmin(vmax(x, lanes<V>(-9.0f)), lanes<V>(9.0f));
    const V e = laneExp(c + c);
    return (e - lanes<V>(1.0f)) / (e + lanes<V>(1.0f));
}

/** @brief GELU in the tanh form (Hendrycks and Gimpel 2016): 0.5 x (1 + tanh(sqrt(2/pi)(x + 0.044715 x^3))). */
template <class V>
inline V laneGelu(V x)
{
    const V inner = lanes<V>(0.797884560802865355f) * vfmadd(lanes<V>(0.044715f) * x * x, x, x);
    return lanes<V>(0.5f) * x * (lanes<V>(1.0f) + laneTanh(inner));
}

/** @brief The activations a model header may ask for (key @c act). */
enum class ModelAct : int { GeluTanh = 0, Relu, Silu };

/** @brief Applies @p act to lanes. */
template <class V>
inline V laneAct(V x, ModelAct act)
{
    if (act == ModelAct::Relu) return vmax(x, lanes<V>(0.0f));
    if (act == ModelAct::Silu) return x / (lanes<V>(1.0f) + laneExp(-x));
    return laneGelu(x);
}

/**
 * @brief out = bias + W x, with the lanes holding W output rows at a time.
 *
 * @param panels weights in panel layout: element (g * cols + c) * W + l is row g*W+l, column c.
 *               Rows beyond @p rows must be present (zero) so the last panel is whole.
 * @param bias   one value per row, padded to a whole number of panels; may be null for no bias
 * @param x      @p cols inputs
 * @param rows   output rows actually wanted
 * @param cols   input dimension
 * @param out    receives roundUpTo(rows, W) values; the padding entries are meaningless
 *
 * Lane l of panel g executes bias[g*W+l], then cols fused multiply-adds in column order -- the same
 * sequence, in the same order, as the scalar instantiation performs for that row. That is why the
 * three paths agree bit for bit.
 */
template <class V>
inline void matvecPanel(const float* panels, const float* bias, const float* x, int rows, int cols, float* out)
{
    constexpr int kW = laneWidth<V>();   // lanes of this instantiation
    const int groups = (rows + kW - 1) / kW;
    for (int g = 0; g < groups; ++g) {
        const float* p = panels + static_cast<size_t>(g) * static_cast<size_t>(cols) * kW;
        V acc = bias != nullptr ? loadLanes<V>(bias + static_cast<size_t>(g) * kW) : lanes<V>(0.0f);
        for (int c = 0; c < cols; ++c)
            acc = vfmadd(loadLanes<V>(p + static_cast<size_t>(c) * kW), lanes<V>(x[c]), acc);
        vstore(out + static_cast<size_t>(g) * kW, acc);
    }
}

/**
 * @brief Packs a row-major matrix into the panel layout matvecPanel() reads, folding in row scales.
 * @param src    @p rows x @p cols, row major
 * @param scale  one factor per row (the int8 quantisation scale), or null for 1
 * @param dst    roundUpTo(rows, W) * cols floats, zeroed for the padding rows
 *
 * Called once per matrix when the model is loaded. The scale is folded into the weight rather than
 * applied to the finished sum, which is one multiply fewer per row and -- because the reference
 * vectors are computed from the dequantized weights as well -- the same arithmetic the oracle does.
 */
template <class V, class Src>
inline void packPanels(const Src* src, const float* scale, int rows, int cols, float* dst)
{
    constexpr int kW = laneWidth<V>();   // lanes of this instantiation
    const int groups = (rows + kW - 1) / kW;
    for (int g = 0; g < groups; ++g)
        for (int c = 0; c < cols; ++c)
            for (int l = 0; l < kW; ++l) {
                const int row = g * kW + l;
                float v = 0.0f;
                if (row < rows) {
                    v = static_cast<float>(src[static_cast<size_t>(row) * static_cast<size_t>(cols) + static_cast<size_t>(c)]);
                    if (scale != nullptr) v *= scale[row];
                }
                dst[(static_cast<size_t>(g) * static_cast<size_t>(cols) + static_cast<size_t>(c)) * kW + static_cast<size_t>(l)] = v;
            }
}

/**
 * @brief Layer normalisation: y = (x - mean) / sqrt(var + eps) * w + b.
 *
 * Mean and variance are summed in a scalar loop over @p n -- the reduction that must not be
 * reordered -- and the affine part runs in lanes. Ba, Kiros and Hinton, "Layer normalization",
 * arXiv:1607.06450 (2016); the variance is the biased one (divided by n), as in PyTorch.
 *
 * @param n  the width; @p x, @p w, @p b and @p out are padded to a whole number of lanes
 */
template <class V>
inline void laneLayerNorm(const float* x, const float* w, const float* b, float eps, int n, float* out)
{
    constexpr int kW = laneWidth<V>();   // lanes of this instantiation
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += x[i];
    const float mean = sum / static_cast<float>(n);
    float sq = 0.0f;
    for (int i = 0; i < n; ++i) { const float d = x[i] - mean; sq += d * d; }
    const float inv = 1.0f / vsqrt(sq / static_cast<float>(n) + eps);
    const V vm = lanes<V>(mean), vi = lanes<V>(inv);
    for (int i = 0; i < n; i += kW) {
        const V t = (loadLanes<V>(x + i) - vm) * vi;
        vstore(out + i, vfmadd(t, loadLanes<V>(w + i), loadLanes<V>(b + i)));
    }
}

/**
 * @brief In place: x = exp(x - max(x)), then divided by the total.
 *
 * The maximum and the total are scalar reductions (exact and order-free for the maximum, order-fixed
 * for the total); the exponentials run in lanes. Subtracting the maximum first is the standard guard
 * against overflow and is what the reference implementation does, so the two agree term by term.
 *
 * @param x   @p n values, padded to a whole number of lanes; the padding is set to the lowest value
 *            first so that it cannot contribute
 * @param n   how many entries count
 */
template <class V>
inline void laneSoftmax(float* x, int n)
{
    constexpr int kW = laneWidth<V>();   // lanes of this instantiation
    float mx = x[0];
    for (int i = 1; i < n; ++i) if (x[i] > mx) mx = x[i];
    const int padded = roundUpTo(n, kW);
    // The padding is pushed so far below the maximum that its exponential is the smallest normal
    // float; it is never summed and never read, but this keeps whatever stood there out of sight.
    for (int i = n; i < padded; ++i) x[i] = mx - 1000.0f;
    const V vm = lanes<V>(mx);
    for (int i = 0; i < padded; i += kW) vstore(x + i, laneExp(loadLanes<V>(x + i) - vm));
    float total = 0.0f;
    for (int i = 0; i < n; ++i) total += x[i];
    const V vt = lanes<V>(1.0f / total);
    for (int i = 0; i < padded; i += kW) vstore(x + i, loadLanes<V>(x + i) * vt);
}

/** @brief dst += a * src over @p n lanes-worth of values (@p n a multiple of the lane width). */
template <class V>
inline void laneAccumulate(float* dst, const float* src, float a, int n)
{
    constexpr int kW = laneWidth<V>();   // lanes of this instantiation
    const V va = lanes<V>(a);
    for (int i = 0; i < n; i += kW) vstore(dst + i, vfmadd(va, loadLanes<V>(src + i), loadLanes<V>(dst + i)));
}

/** @brief x *= a over @p n values (@p n a multiple of the lane width). */
template <class V>
inline void laneScale(float* x, float a, int n)
{
    constexpr int kW = laneWidth<V>();   // lanes of this instantiation
    const V va = lanes<V>(a);
    for (int i = 0; i < n; i += kW) vstore(x + i, loadLanes<V>(x + i) * va);
}

/** @brief dst += src over @p n values (@p n a multiple of the lane width). */
template <class V>
inline void laneAdd(float* dst, const float* src, int n)
{
    constexpr int kW = laneWidth<V>();   // lanes of this instantiation
    for (int i = 0; i < n; i += kW) vstore(dst + i, loadLanes<V>(dst + i) + loadLanes<V>(src + i));
}

/** @brief Applies @p act to @p n values in place (@p n a multiple of the lane width). */
template <class V>
inline void laneActivate(float* x, int n, ModelAct act)
{
    constexpr int kW = laneWidth<V>();   // lanes of this instantiation
    for (int i = 0; i < n; i += kW) vstore(x + i, laneAct(loadLanes<V>(x + i), act));
}

/**
 * @brief Writes one key vector into the time-panelled key cache.
 * @param cache  panels over time: element (g * headDim + d) * W + l is time g*W+l, dimension d
 * @param t      the time step
 * @param k      @p headDim values
 *
 * W scalar stores per dimension. The cache is laid out this way so that scoring a query against all
 * keys is exactly matvecPanel() with the time steps as output rows.
 */
template <class V>
inline void storeKey(float* cache, int t, const float* k, int headDim)
{
    constexpr int kW = laneWidth<V>();   // lanes of this instantiation
    const int g = t / kW, l = t % kW;
    float* p = cache + static_cast<size_t>(g) * static_cast<size_t>(headDim) * kW + static_cast<size_t>(l);
    for (int d = 0; d < headDim; ++d) p[static_cast<size_t>(d) * kW] = k[d];
}

} // namespace phos
