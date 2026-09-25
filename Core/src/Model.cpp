/**
 * @file Model.cpp
 * @brief Reading a `.phosmdl` weight file and running one forward step of it.
 *
 * Two halves. The first walks the file as docs/MODEL_FORMAT.md section 1 describes it: magic,
 * version, text header, then tensor after tensor until the end marker, checking every length
 * against what is left and reporting the field and the byte offset when something does not add up.
 * A weight file is written by another program on another machine, and the only thing worse than
 * refusing it is reading it as garbage.
 *
 * The second is the forward pass of section 4, which is entirely the kernels of ModelKernel.h:
 * layer norm, matrix-vector products with the output rows in the lanes, causal attention over the
 * key/value cache, the feed-forward layer, the output head. The only arithmetic written here is the
 * bookkeeping around them.
 *
 * **The `arch=ssm` seam.** MODEL_FORMAT.md section 4 also specifies a selective state-space block.
 * It is not implemented, and deliberately: the training run of 16.09.2026 measured the SSM at 2.16
 * nats per held-out token against the transformer's 1.49 and exported only the transformer. The
 * loader reads such a header, checks it and refuses it by name. What a later implementation would
 * add is one more branch in step() and eleven more tensors per block -- in.w/b, conv.w/b, xproj.w,
 * dt.w/b, Alog.w, D.w, out.w/b -- plus, in ModelKernel.h, a lane logarithm for the softplus of
 * delta; everything else (the norms, the matrix-vector kernel, the activations, the packing) is
 * already here. The recurrence itself is one first-order scalar scan per channel, run with the
 * lanes over channels and the state index walked in order, which is the same bit-identity shape as
 * the attention.
 */
#include "phos/Model.h"
#include "phos/Harmony.h"   // kNumScales: the rows mode.emb is allowed to have
#include "phos/Util.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace phos {

namespace {

using V = VecF;
constexpr int kLanes = kVecWidth;   ///< lanes of the path this was built for

constexpr size_t kNameBytes = 48;
constexpr size_t kTensorFixed = kNameBytes + 1 + 1 + 2 + 16;   ///< name, dtype, ndim, reserved, dim[4]
constexpr size_t kAlign = 32;                                   ///< payloads start on this boundary

/** @brief Where the model files are looked for (setModelSearchPath). */
std::string& searchPath()
{
    static std::string p;
    return p;
}

/** @brief One tensor as it sits in the file. */
struct Tensor {
    std::string name;
    int dtype = 0;               ///< 0 float32, 1 int8 with one scale per row
    int dim[4] = { 1, 1, 1, 1 };
    const float* scale = nullptr;
    const void* data = nullptr;
    size_t count = 0;
};

uint32_t readU32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint16_t readU16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

/** @brief The value of @p key in a "key=value\n" header, or an empty string. */
std::string headerValue(const std::string& header, const char* key)
{
    const std::string pattern = std::string(key) + "=";
    size_t at = 0;
    while (at < header.size()) {
        const size_t eol = header.find('\n', at);
        const std::string line = header.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        if (line.size() >= pattern.size() && line.compare(0, pattern.size(), pattern) == 0) return line.substr(pattern.size());
        if (eol == std::string::npos) break;
        at = eol + 1;
    }
    return std::string();
}

/** @brief The integer value of @p key in a model file's header, or @p fallback. */
int headerInt(const std::string& header, const char* key, int fallback)
{
    const std::string v = headerValue(header, key);
    return v.empty() ? fallback : std::atoi(v.c_str());
}

/** @brief One value of a tensor, dequantized when it is int8 (one scale per row of dim[0]). */
float value(const Tensor& t, size_t index, int row)
{
    if (t.dtype == 1) return static_cast<float>(static_cast<const int8_t*>(t.data)[index]) * t.scale[row];
    return static_cast<const float*>(t.data)[index];
}

/** @brief Copies @p n values of a one-dimensional tensor into a buffer padded to the lane width. */
void copyPadded(const Tensor& t, int n, std::vector<float>& dst)
{
    dst.assign(static_cast<size_t>(roundUpTo(n, kLanes)), 0.0f);
    for (int i = 0; i < n; ++i) dst[static_cast<size_t>(i)] = value(t, static_cast<size_t>(i), i);
}

/** @brief Packs a rows x cols tensor into panels, dequantizing an int8 tensor on the way. */
void packMatrix(const Tensor& t, int rows, int cols, std::vector<float>& dst)
{
    dst.assign(static_cast<size_t>(roundUpTo(rows, kLanes)) * static_cast<size_t>(cols), 0.0f);
    if (t.dtype == 1) packPanels<V>(static_cast<const int8_t*>(t.data), t.scale, rows, cols, dst.data());
    else packPanels<V>(static_cast<const float*>(t.data), nullptr, rows, cols, dst.data());
}

/** @brief Copies an embedding table into rows padded to the lane width. */
void copyRows(const Tensor& t, int rows, int cols, int stride, std::vector<float>& dst)
{
    dst.assign(static_cast<size_t>(rows) * static_cast<size_t>(stride), 0.0f);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const size_t k = static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c);
            dst[static_cast<size_t>(r) * static_cast<size_t>(stride) + static_cast<size_t>(c)] = value(t, k, r);
        }
}

/** @brief @p v clamped into [0, @p n - 1]. */
int clampRow(int v, int n) { return v < 0 ? 0 : (v >= n ? n - 1 : v); }

} // namespace

void setModelSearchPath(const std::string& directory) { searchPath() = directory; }

bool NeuralModel::load(const char* path, std::string& error)
{
    loaded_ = false;
    error.clear();
    std::vector<uint8_t> file;
    std::string tried = path;
    if (!readFile(path, file)) {
        // A bare name is looked for where the host put its resources, then beside the sources.
        const bool bare = std::strchr(path, '/') == nullptr && std::strchr(path, '\\') == nullptr;
        bool got = false;
        if (bare && !searchPath().empty()) {
            const std::string p = searchPath() + "/" + path;
            tried += ", " + p;
            got = readFile(p.c_str(), file);
        }
#if defined(PHOS_SOURCE_DATA_DIR)
        if (!got && bare) {
            const std::string p = std::string(PHOS_SOURCE_DATA_DIR) + "/" + path;
            tried += ", " + p;
            got = readFile(p.c_str(), file);
        }
#endif
        if (!got) { error = "cannot open the model file (" + tried + ")"; return false; }
    }

    size_t at = 0;
    const size_t n = file.size();
    if (n < 16 || std::memcmp(file.data(), "PHOSMDL1", 8) != 0) return fileError(&error, "magic is not PHOSMDL1", 0);
    at = 8;
    const uint32_t version = readU32(file.data() + at);
    if (version != 1) return fileError(&error, "version is not 1", at);
    at += 4;
    const uint32_t headerLen = readU32(file.data() + at);
    at += 4;
    if (at + headerLen > n) return fileError(&error, "headerLen runs past the end of the file", at - 4);
    const std::string header(reinterpret_cast<const char*>(file.data() + at), headerLen);
    at += headerLen;

    ModelInfo info;
    info.text = header;
    const std::string arch = headerValue(header, "arch");
    if (arch == "ssm") info.arch = ModelArch::Ssm;
    else if (arch == "transformer") info.arch = ModelArch::Transformer;
    else { error = "header arch=\"" + arch + "\" is neither transformer nor ssm"; return false; }
    info.layers = headerInt(header, "layers", 0);
    info.dim = headerInt(header, "dim", 0);
    info.heads = headerInt(header, "heads", 0);
    info.ffn = headerInt(header, "ffn", 0);
    info.state = headerInt(header, "state", 0);
    info.vocab = headerInt(header, "vocab", 0);
    info.ctx = headerInt(header, "ctx", 0);
    info.roles = headerInt(header, "roles", 0);
    info.styles = headerInt(header, "styles", 0);
    info.tokenVersion = headerInt(header, "tokenVersion", 0);
    info.relMin = headerInt(header, "relMin", kCorpusRelMin);
    info.relMax = headerInt(header, "relMax", kCorpusRelMax);
    info.condStep = headerInt(header, "condStep", 0);
    info.condBar = headerInt(header, "condBar", 0);
    info.condGap = headerInt(header, "condGap", 0);
    info.condIdx = headerInt(header, "condIdx", 0);
    info.condBars = headerInt(header, "condBars", 0);
    // Absent in a three-role melodic file, and then the tenth embedding table is not looked for and
    // NoteCond::kick is ignored (docs/MODEL_FORMAT.md, section 3, the bass paragraph).
    info.condKick = headerInt(header, "condKick", 0);
    // Absent in every file written before 18.09.2026, and then the eleventh embedding table is not
    // looked for and the mode argument of begin() is ignored (docs/MODEL_FORMAT.md, section 3).
    info.condMode = headerInt(header, "condMode", 0);
    info.expand = headerInt(header, "expand", 0);
    info.dtRank = headerInt(header, "dtRank", 0);
    info.convK = headerInt(header, "convK", 0);
    info.int8 = headerValue(header, "quant") == "int8";
    const std::string act = headerValue(header, "act");
    if (act == "relu") info.act = ModelAct::Relu;
    else if (act == "silu" || act == "swish") info.act = ModelAct::Silu;
    else if (act == "gelu_tanh" || act == "gelu" || act.empty()) info.act = ModelAct::GeluTanh;
    else { error = "header act=\"" + act + "\" is not one of gelu_tanh, relu, silu"; return false; }
    const std::string eps = headerValue(header, "eps");
    if (!eps.empty()) info.eps = static_cast<float>(std::atof(eps.c_str()));
    const std::string nll = headerValue(header, "nll");
    if (!nll.empty()) info.nll = std::atof(nll.c_str());

    char buf[256];
    if (info.arch == ModelArch::Ssm) {
        error = "arch=ssm: the selective scan is not implemented (the seam is described at the top of "
                "Model.cpp); the transformer export is the model Phase 8 measured as the better one";
        return false;
    }
    if (info.vocab != kCorpusAlphabet || info.relMin != kCorpusRelMin || info.relMax != kCorpusRelMax) {
        std::snprintf(buf, sizeof(buf), "header vocab=%d rel=%d..%d, but the composer's alphabet is %d symbols over %d..%d",
                      info.vocab, info.relMin, info.relMax, kCorpusAlphabet, kCorpusRelMin, kCorpusRelMax);
        error = buf;
        return false;
    }
    if (info.tokenVersion != 1) { error = "header tokenVersion is not 1 (the stage-A alphabet)"; return false; }
    if (info.layers <= 0 || info.dim <= 0 || info.heads <= 0 || info.ffn <= 0 || info.ctx <= 0
        || info.roles <= 0 || info.styles <= 0 || info.condStep <= 0 || info.condBar <= 0
        || info.condGap <= 0 || info.condIdx <= 0 || info.condBars <= 0) {
        error = "header is missing one of layers, dim, heads, ffn, ctx, roles, styles, cond*";
        return false;
    }
    if (info.condKick < 0) { error = "header condKick is negative"; return false; }
    if (info.condMode < 0) { error = "header condMode is negative"; return false; }
    if (info.condMode > 0 && info.condMode != kNumScales) {
        std::snprintf(buf, sizeof(buf), "header condMode=%d, but the composer has %d modes (Harmony.h, "
                                        "kScaleSteps) and passes one of them as a row index",
                      info.condMode, kNumScales);
        error = buf;
        return false;
    }
    if (info.roles > kBassRole && info.condKick <= 0) {
        error = "header says roles=4 (the bass is in) but carries no condKick; docs/MODEL_FORMAT.md "
                "section 3 makes the kick class part of the bass role's conditioning";
        return false;
    }
    if (info.dim % info.heads != 0) { error = "header dim is not a multiple of heads"; return false; }

    // --- the tensors -------------------------------------------------------------------------
    std::vector<Tensor> tensors;
    while (true) {
        if (at + 8 <= n && std::memcmp(file.data() + at, "PHOSEND1", 8) == 0) { at += 8; break; }
        if (at + kTensorFixed > n) return fileError(&error, "a tensor header runs past the end of the file", at);
        const size_t head = at;
        Tensor t;
        const char* nameBytes = reinterpret_cast<const char*>(file.data() + at);
        size_t nameLen = 0;
        while (nameLen < kNameBytes && nameBytes[nameLen] != '\0') ++nameLen;
        t.name.assign(nameBytes, nameLen);
        if (t.name.empty()) return fileError(&error, "tensor name is empty", head);
        at += kNameBytes;
        t.dtype = file[at];
        const int ndim = file[at + 1];
        if (t.dtype > 1) return fileError(&error, "tensor dtype is neither 0 (float32) nor 1 (int8)", at);
        if (ndim < 1 || ndim > 4) return fileError(&error, "tensor ndim is not in 1..4", at + 1);
        if (readU16(file.data() + at + 2) != 0) return fileError(&error, "tensor reserved field is not zero", at + 2);
        at += 4;
        t.count = 1;
        for (int i = 0; i < 4; ++i) {
            t.dim[i] = static_cast<int>(readU32(file.data() + at + static_cast<size_t>(i) * 4));
            if (t.dim[i] <= 0) return fileError(&error, ("tensor " + t.name + " has a dimension that is not positive").c_str(), at + static_cast<size_t>(i) * 4);
            t.count *= static_cast<size_t>(t.dim[i]);
        }
        at += 16;
        if (t.dtype == 1) {
            const size_t bytes = static_cast<size_t>(t.dim[0]) * 4;
            if (at + bytes > n) return fileError(&error, ("tensor " + t.name + ": the int8 scales run past the end").c_str(), at);
            t.scale = reinterpret_cast<const float*>(file.data() + at);
            at += bytes;
        }
        at += (kAlign - (at % kAlign)) % kAlign;
        const size_t bytes = t.count * (t.dtype == 1 ? 1u : 4u);
        if (at + bytes > n) return fileError(&error, ("tensor " + t.name + ": the data runs past the end of the file").c_str(), at);
        t.data = file.data() + at;
        at += bytes;
        info.parameters += t.count;
        tensors.push_back(t);
    }
    if (at != n) return fileError(&error, "trailing bytes after PHOSEND1", at);

    auto find = [&](const std::string& name) -> const Tensor* {
        for (const Tensor& tt : tensors) if (tt.name == name) return &tt;
        return nullptr;
    };
    const Tensor* t = nullptr;
    auto need = [&](const std::string& name, int d0, int d1) {
        const Tensor* got = find(name);
        if (got == nullptr) { error = "tensor " + name + " is missing"; return false; }
        if (got->dim[0] != d0 || got->dim[1] != d1 || got->dim[2] != 1 || got->dim[3] != 1) {
            std::snprintf(buf, sizeof(buf), "tensor %s is %dx%dx%dx%d, the header asks for %dx%d",
                          name.c_str(), got->dim[0], got->dim[1], got->dim[2], got->dim[3], d0, d1);
            error = buf;
            return false;
        }
        t = got;
        return true;
    };

    const int d = info.dim, f = info.ffn, L = info.layers;
    headDim_ = d / info.heads;
    dimP_ = roundUpTo(d, kLanes);
    ffnP_ = roundUpTo(f, kLanes);
    vocabP_ = roundUpTo(info.vocab, kLanes);
    headDimP_ = roundUpTo(headDim_, kLanes);
    ctxP_ = roundUpTo(info.ctx, kLanes);
    qkvP_ = roundUpTo(3 * d, kLanes);

    // The nine embedding tables (MODEL_FORMAT.md section 4), kick.emb as a tenth when the header
    // declares condKick and mode.emb as an eleventh when it declares condMode. Their rows are summed
    // into the input.
    struct Emb { const char* name; int rows; std::vector<float>* dst; };
    const Emb embeddings[] = {
        { "tok.emb",   info.vocab,     &tok_ },
        { "pos.emb",   info.ctx,       &posE_ },
        { "role.emb",  info.roles,     &roleE_ },
        { "style.emb", info.styles,    &styleE_ },
        { "bars.emb",  info.condBars,  &barsE_ },
        { "step.emb",  info.condStep,  &stepE_ },
        { "bar.emb",   info.condBar,   &barE_ },
        { "gap.emb",   info.condGap,   &gapE_ },
        { "idx.emb",   info.condIdx,   &idxE_ },
        { "kick.emb",  info.condKick,  &kickE_ },
        { "mode.emb",  info.condMode,  &modeE_ },
    };
    for (const Emb& e : embeddings) {
        if (e.rows <= 0) { e.dst->clear(); continue; }       // only kick.emb and mode.emb can be absent
        if (!need(e.name, e.rows, d)) return false;
        copyRows(*t, e.rows, d, dimP_, *e.dst);
    }

    blocks_.assign(static_cast<size_t>(L), Block{});
    for (int i = 0; i < L; ++i) {
        Block& b = blocks_[static_cast<size_t>(i)];
        const std::string pre = "blocks." + std::to_string(i) + ".";
        if (!need(pre + "norm1.w", d, 1)) return false; copyPadded(*t, d, b.n1w);
        if (!need(pre + "norm1.b", d, 1)) return false; copyPadded(*t, d, b.n1b);
        if (!need(pre + "attn.qkv.w", 3 * d, d)) return false; packMatrix(*t, 3 * d, d, b.qkv);
        if (!need(pre + "attn.qkv.b", 3 * d, 1)) return false; copyPadded(*t, 3 * d, b.qkvB);
        if (!need(pre + "attn.out.w", d, d)) return false; packMatrix(*t, d, d, b.out);
        if (!need(pre + "attn.out.b", d, 1)) return false; copyPadded(*t, d, b.outB);
        if (!need(pre + "norm2.w", d, 1)) return false; copyPadded(*t, d, b.n2w);
        if (!need(pre + "norm2.b", d, 1)) return false; copyPadded(*t, d, b.n2b);
        if (!need(pre + "ffn.up.w", f, d)) return false; packMatrix(*t, f, d, b.up);
        if (!need(pre + "ffn.up.b", f, 1)) return false; copyPadded(*t, f, b.upB);
        if (!need(pre + "ffn.down.w", d, f)) return false; packMatrix(*t, d, f, b.down);
        if (!need(pre + "ffn.down.b", d, 1)) return false; copyPadded(*t, d, b.downB);
        b.kCache.assign(static_cast<size_t>(info.heads) * static_cast<size_t>(ctxP_) * static_cast<size_t>(headDim_), 0.0f);
        b.vCache.assign(static_cast<size_t>(info.heads) * static_cast<size_t>(info.ctx) * static_cast<size_t>(headDimP_), 0.0f);
    }
    if (!need("norm.w", d, 1)) return false; copyPadded(*t, d, normW_);
    if (!need("norm.b", d, 1)) return false; copyPadded(*t, d, normB_);
    if (!need("head.w", info.vocab, d)) return false; packMatrix(*t, info.vocab, d, head_);
    if (!need("head.b", info.vocab, 1)) return false; copyPadded(*t, info.vocab, headB_);

    x_.assign(static_cast<size_t>(dimP_), 0.0f);
    nx_.assign(static_cast<size_t>(dimP_), 0.0f);
    qkvBuf_.assign(static_cast<size_t>(qkvP_), 0.0f);
    att_.assign(static_cast<size_t>(dimP_), 0.0f);
    ff_.assign(static_cast<size_t>(ffnP_), 0.0f);
    accum_.assign(static_cast<size_t>(headDimP_), 0.0f);
    scores_.assign(static_cast<size_t>(ctxP_), 0.0f);
    logits_.assign(static_cast<size_t>(vocabP_), 0.0f);
    probs_.assign(static_cast<size_t>(info.vocab), 0.0);

    size_t bytes = 0;
    for (const Block& b : blocks_)
        bytes += (b.qkv.size() + b.out.size() + b.up.size() + b.down.size() + b.kCache.size() + b.vCache.size()) * sizeof(float);
    bytes += (tok_.size() + posE_.size() + roleE_.size() + styleE_.size() + barsE_.size() + stepE_.size()
              + barE_.size() + gapE_.size() + idxE_.size() + kickE_.size() + modeE_.size()
              + head_.size()) * sizeof(float);
    info.bytes = bytes;
    info_ = info;
    loaded_ = true;
    pos_ = 0;
    return true;
}

void NeuralModel::begin(int role, int style, int bars, int mode)
{
    role_ = clampRow(role, info_.roles);
    style_ = clampRow(style, info_.styles);
    bars_ = clampRow(bars - 1, info_.condBars);
    // Clamped like every other row index, and simply not read by a file without mode.emb -- so a
    // composer that always passes the section's mode works with both generations of weight file.
    mode_ = info_.condMode > 0 ? clampRow(mode, info_.condMode) : 0;
    pos_ = 0;
    // The key/value cache is not cleared: only the first pos_ entries are ever read (the attention
    // runs over 0..pos_, and the scores past it are pushed below the softmax's maximum in
    // ModelKernel.h), so what a previous line left behind cannot reach the output. The self test
    // measures exactly that (testModelDecode).
}

bool NeuralModel::step(int token, const NoteCond& cond)
{
    if (!loaded_ || token < 0 || token >= info_.vocab) return false;
    // A line longer than the file's ctx is refused rather than guessed at. MODEL_FORMAT.md says what
    // pos.emb does past its last row (it clamps) but nothing about what the attention should then
    // attend to, and the trained model's reference cases stop at ctx, so there is no oracle for an
    // answer either way. It cannot happen in the composer: ctx is 256 and the longest line any maker
    // in Melody.cpp draws is the 32 onsets of a two-bar acid pattern. The caller (sampleMasked) turns
    // a false into a clean failure, and the composer falls back to the Markov model.
    if (pos_ >= info_.ctx) return false;
    const int d = info_.dim;
    // The nine embedding rows -- ten with the bass's kick table, eleven with mode.emb -- summed.
    // Table reads are gathers and stay on the scalar side (Vec.h has no gather on NEON); the adds are lanes.
    const int posRow = pos_;
    const size_t stride = static_cast<size_t>(dimP_);
    std::memcpy(x_.data(), tok_.data() + static_cast<size_t>(token) * stride, stride * sizeof(float));
    laneAdd<V>(x_.data(), posE_.data() + static_cast<size_t>(posRow) * stride, dimP_);
    laneAdd<V>(x_.data(), roleE_.data() + static_cast<size_t>(role_) * stride, dimP_);
    laneAdd<V>(x_.data(), styleE_.data() + static_cast<size_t>(style_) * stride, dimP_);
    laneAdd<V>(x_.data(), barsE_.data() + static_cast<size_t>(bars_) * stride, dimP_);
    laneAdd<V>(x_.data(), stepE_.data() + static_cast<size_t>(clampRow(cond.step, info_.condStep)) * stride, dimP_);
    laneAdd<V>(x_.data(), barE_.data() + static_cast<size_t>(clampRow(cond.bar, info_.condBar)) * stride, dimP_);
    laneAdd<V>(x_.data(), gapE_.data() + static_cast<size_t>(clampRow(cond.gap, info_.condGap)) * stride, dimP_);
    laneAdd<V>(x_.data(), idxE_.data() + static_cast<size_t>(clampRow(cond.idx, info_.condIdx)) * stride, dimP_);
    if (info_.condKick > 0)
        laneAdd<V>(x_.data(), kickE_.data() + static_cast<size_t>(clampRow(cond.kick, info_.condKick)) * stride, dimP_);
    // The mode's row is per line, not per note, so it is read from the state begin() fixed; the add
    // still happens here because the sum is one vector per position (MODEL_FORMAT.md section 4).
    if (info_.condMode > 0)
        laneAdd<V>(x_.data(), modeE_.data() + static_cast<size_t>(mode_) * stride, dimP_);

    forwardTransformer(posRow);
    ++pos_;

    laneLayerNorm<V>(x_.data(), normW_.data(), normB_.data(), info_.eps, d, nx_.data());
    matvecPanel<V>(head_.data(), headB_.data(), nx_.data(), info_.vocab, d, logits_.data());

    // The probabilities the sampler asks for, in double: a draw multiplies them by constraint
    // weights and raises them to 1/temperature, and float would lose the tail of a peaked model.
    double mx = logits_[0];
    for (int c = 1; c < info_.vocab; ++c) if (logits_[static_cast<size_t>(c)] > mx) mx = logits_[static_cast<size_t>(c)];
    double total = 0.0;
    for (int c = 0; c < info_.vocab; ++c) {
        const double e = std::exp(static_cast<double>(logits_[static_cast<size_t>(c)]) - mx);
        probs_[static_cast<size_t>(c)] = e;
        total += e;
    }
    for (int c = 0; c < info_.vocab; ++c) probs_[static_cast<size_t>(c)] /= total;
    return true;
}

void NeuralModel::forwardTransformer(int t)
{
    const int d = info_.dim, f = info_.ffn, hd = headDim_, heads = info_.heads;
    const float attScale = 1.0f / vsqrt(static_cast<float>(hd));
    for (int l = 0; l < info_.layers; ++l) {
        Block& b = blocks_[static_cast<size_t>(l)];
        laneLayerNorm<V>(x_.data(), b.n1w.data(), b.n1b.data(), info_.eps, d, nx_.data());
        matvecPanel<V>(b.qkv.data(), b.qkvB.data(), nx_.data(), 3 * d, d, qkvBuf_.data());
        const float* q = qkvBuf_.data();
        const float* k = qkvBuf_.data() + d;
        const float* v = qkvBuf_.data() + 2 * d;
        for (int h = 0; h < heads; ++h) {
            float* kc = b.kCache.data() + static_cast<size_t>(h) * static_cast<size_t>(ctxP_) * static_cast<size_t>(hd);
            float* vc = b.vCache.data() + static_cast<size_t>(h) * static_cast<size_t>(info_.ctx) * static_cast<size_t>(headDimP_);
            storeKey<V>(kc, t, k + h * hd, hd);
            float* vrow = vc + static_cast<size_t>(t) * static_cast<size_t>(headDimP_);
            for (int i = 0; i < hd; ++i) vrow[i] = v[h * hd + i];
            for (int i = hd; i < headDimP_; ++i) vrow[i] = 0.0f;
            // Scores of this query against every key so far -- the same kernel as a matmul, with the
            // time steps as its output rows -- then scaled, softmaxed, and used to mix the values.
            matvecPanel<V>(kc, nullptr, q + h * hd, t + 1, hd, scores_.data());
            laneScale<V>(scores_.data(), attScale, roundUpTo(t + 1, kLanes));
            laneSoftmax<V>(scores_.data(), t + 1);
            // The head's slice of att_ is hd wide and need not be a whole number of lanes, so the
            // accumulation runs in a padded scratch buffer and is copied back: one head's lanes may
            // not spill into the next head's slice.
            float* acc = accum_.data();
            for (int i = 0; i < headDimP_; ++i) acc[i] = 0.0f;
            for (int tt = 0; tt <= t; ++tt)
                laneAccumulate<V>(acc, vc + static_cast<size_t>(tt) * static_cast<size_t>(headDimP_),
                                  scores_[static_cast<size_t>(tt)], headDimP_);
            float* outSlice = att_.data() + h * hd;
            for (int i = 0; i < hd; ++i) outSlice[i] = acc[i];
        }
        for (int i = d; i < dimP_; ++i) att_[static_cast<size_t>(i)] = 0.0f;
        matvecPanel<V>(b.out.data(), b.outB.data(), att_.data(), d, d, nx_.data());
        laneAdd<V>(x_.data(), nx_.data(), dimP_);

        laneLayerNorm<V>(x_.data(), b.n2w.data(), b.n2b.data(), info_.eps, d, nx_.data());
        matvecPanel<V>(b.up.data(), b.upB.data(), nx_.data(), f, d, ff_.data());
        laneActivate<V>(ff_.data(), ffnP_, info_.act);
        for (int i = f; i < ffnP_; ++i) ff_[static_cast<size_t>(i)] = 0.0f;
        matvecPanel<V>(b.down.data(), b.downB.data(), ff_.data(), d, f, nx_.data());
        laneAdd<V>(x_.data(), nx_.data(), dimP_);
    }
}

NeuralModel* sharedMelodyModel(std::string* note)
{
    static NeuralModel model;
    static std::string message;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string error;
        if (!model.load(kMelodyModelFile, error))
            message = "phosphene: no neural melody model (" + error + "); the composer stays on the Markov model";
    });
    if (note != nullptr) *note = message;
    return model.loaded() ? &model : nullptr;
}

NeuralModel* sharedBassModel(std::string* note)
{
    static NeuralModel model;
    static std::string message;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string error;
        if (!model.load(kBassModelFile, error))
            message = "phosphene: no neural bass model (" + error + "); the composer keeps the pattern families";
        else if (model.info().roles <= kBassRole) {
            message = "phosphene: " + std::string(kBassModelFile) + " has roles=" +
                      std::to_string(model.info().roles) + ", so it carries no bass role; the composer "
                      "keeps the pattern families";
            model = NeuralModel{};
        }
    });
    if (note != nullptr) *note = message;
    return model.loaded() ? &model : nullptr;
}

} // namespace phos
