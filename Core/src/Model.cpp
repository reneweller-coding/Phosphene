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
#include "phos/Harmony.h"   // NumScales: the rows mode.emb is allowed to have
#include "phos/Util.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace phos {

namespace {

using V = VecF;   ///< The vector type of this build.
constexpr int Lanes = VecWidth;   ///< lanes of the path this was built for

constexpr size_t NameBytes = 48;   ///< a tensor's name in the file, bytes
constexpr size_t TensorFixed = NameBytes + 1 + 1 + 2 + 16;   ///< name, dtype, ndim, reserved, dim[4]
constexpr size_t Align = 32;                                   ///< payloads start on this boundary

/** @brief Where the model files are looked for (setModelSearchPath). */
std::string& searchPath()
{
    static std::string p;
    return p;
}

/** @brief One tensor as it sits in the file. */
struct Tensor {
    std::string m_name;   ///< its name
    int m_dtype = 0;               ///< 0 float32, 1 int8 with one scale per row
    int m_dim[4] = { 1, 1, 1, 1 };   ///< its dimensions
    const float* m_scale = nullptr;   ///< the rows' scales (int8), else null
    const void* m_data = nullptr;   ///< its payload
    size_t m_count = 0;   ///< its elements
};

/** @brief A little-endian 32-bit word at @p p. */
uint32_t readU32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
/** @brief A little-endian 16-bit word at @p p. */
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
    if (t.m_dtype == 1) return static_cast<float>(static_cast<const int8_t*>(t.m_data)[index]) * t.m_scale[row];
    return static_cast<const float*>(t.m_data)[index];
}

/** @brief Copies @p n values of a one-dimensional tensor into a buffer padded to the lane width. */
void copyPadded(const Tensor& t, int n, std::vector<float>& dst)
{
    dst.assign(static_cast<size_t>(roundUpTo(n, Lanes)), 0.0f);
    for (int i = 0; i < n; ++i) dst[static_cast<size_t>(i)] = value(t, static_cast<size_t>(i), i);
}

/** @brief Packs a rows x cols tensor into panels, dequantizing an int8 tensor on the way. */
void packMatrix(const Tensor& t, int rows, int cols, std::vector<float>& dst)
{
    dst.assign(static_cast<size_t>(roundUpTo(rows, Lanes)) * static_cast<size_t>(cols), 0.0f);
    if (t.m_dtype == 1) packPanels<V>(static_cast<const int8_t*>(t.m_data), t.m_scale, rows, cols, dst.data());
    else packPanels<V>(static_cast<const float*>(t.m_data), nullptr, rows, cols, dst.data());
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
    m_loaded = false;
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
    info.m_text = header;
    const std::string arch = headerValue(header, "arch");
    if (arch == "ssm") info.m_arch = ModelArchE::SSM;
    else if (arch == "transformer") info.m_arch = ModelArchE::TRANSFORMER;
    else { error = "header arch=\"" + arch + "\" is neither transformer nor ssm"; return false; }
    info.m_layers = headerInt(header, "layers", 0);
    info.m_dim = headerInt(header, "dim", 0);
    info.m_heads = headerInt(header, "heads", 0);
    info.m_ffn = headerInt(header, "ffn", 0);
    info.m_state = headerInt(header, "state", 0);
    info.m_vocab = headerInt(header, "vocab", 0);
    info.m_ctx = headerInt(header, "ctx", 0);
    info.m_roles = headerInt(header, "roles", 0);
    info.m_styles = headerInt(header, "styles", 0);
    info.m_tokenVersion = headerInt(header, "tokenVersion", 0);
    info.m_relMin = headerInt(header, "relMin", CorpusRelMin);
    info.m_relMax = headerInt(header, "relMax", CorpusRelMax);
    info.m_condStep = headerInt(header, "condStep", 0);
    info.m_condBar = headerInt(header, "condBar", 0);
    info.m_condGap = headerInt(header, "condGap", 0);
    info.m_condIdx = headerInt(header, "condIdx", 0);
    info.m_condBars = headerInt(header, "condBars", 0);
    // Absent in a three-role melodic file, and then the tenth embedding table is not looked for and
    // NoteCond::kick is ignored (docs/MODEL_FORMAT.md, section 3, the bass paragraph).
    info.m_condKick = headerInt(header, "condKick", 0);
    // Absent in every file written before 18.09.2026, and then the eleventh embedding table is not
    // looked for and the mode argument of begin() is ignored (docs/MODEL_FORMAT.md, section 3).
    info.m_condMode = headerInt(header, "condMode", 0);
    info.m_expand = headerInt(header, "expand", 0);
    info.m_dtRank = headerInt(header, "dtRank", 0);
    info.m_convK = headerInt(header, "convK", 0);
    info.m_int8 = headerValue(header, "quant") == "int8";
    const std::string act = headerValue(header, "act");
    if (act == "relu") info.m_act = ModelActE::RELU;
    else if (act == "silu" || act == "swish") info.m_act = ModelActE::SILU;
    else if (act == "gelu_tanh" || act == "gelu" || act.empty()) info.m_act = ModelActE::GELU_TANH;
    else { error = "header act=\"" + act + "\" is not one of gelu_tanh, relu, silu"; return false; }
    const std::string eps = headerValue(header, "eps");
    if (!eps.empty()) info.m_eps = static_cast<float>(std::atof(eps.c_str()));
    const std::string nll = headerValue(header, "nll");
    if (!nll.empty()) info.m_nll = std::atof(nll.c_str());

    char buf[256];
    if (info.m_arch == ModelArchE::SSM) {
        error = "arch=ssm: the selective scan is not implemented (the seam is described at the top of "
                "Model.cpp); the transformer export is the model Phase 8 measured as the better one";
        return false;
    }
    if (info.m_vocab != CorpusAlphabet || info.m_relMin != CorpusRelMin || info.m_relMax != CorpusRelMax) {
        std::snprintf(buf, sizeof(buf), "header vocab=%d rel=%d..%d, but the composer's alphabet is %d symbols over %d..%d",
                      info.m_vocab, info.m_relMin, info.m_relMax, CorpusAlphabet, CorpusRelMin, CorpusRelMax);
        error = buf;
        return false;
    }
    if (info.m_tokenVersion != 1) { error = "header tokenVersion is not 1 (the stage-A alphabet)"; return false; }
    if (info.m_layers <= 0 || info.m_dim <= 0 || info.m_heads <= 0 || info.m_ffn <= 0 || info.m_ctx <= 0
        || info.m_roles <= 0 || info.m_styles <= 0 || info.m_condStep <= 0 || info.m_condBar <= 0
        || info.m_condGap <= 0 || info.m_condIdx <= 0 || info.m_condBars <= 0) {
        error = "header is missing one of layers, dim, heads, ffn, ctx, roles, styles, cond*";
        return false;
    }
    if (info.m_condKick < 0) { error = "header condKick is negative"; return false; }
    if (info.m_condMode < 0) { error = "header condMode is negative"; return false; }
    if (info.m_condMode > 0 && info.m_condMode != NumScales) {
        std::snprintf(buf, sizeof(buf), "header condMode=%d, but the composer has %d modes (Harmony.h, "
                                        "ScaleSteps) and passes one of them as a row index",
                      info.m_condMode, NumScales);
        error = buf;
        return false;
    }
    if (info.m_roles > BassRole && info.m_condKick <= 0) {
        error = "header says roles=4 (the bass is in) but carries no condKick; docs/MODEL_FORMAT.md "
                "section 3 makes the kick class part of the bass role's conditioning";
        return false;
    }
    if (info.m_dim % info.m_heads != 0) { error = "header dim is not a multiple of heads"; return false; }

    // --- the tensors -------------------------------------------------------------------------
    std::vector<Tensor> tensors;
    while (true) {
        if (at + 8 <= n && std::memcmp(file.data() + at, "PHOSEND1", 8) == 0) { at += 8; break; }
        if (at + TensorFixed > n) return fileError(&error, "a tensor header runs past the end of the file", at);
        const size_t head = at;
        Tensor t;
        const char* nameBytes = reinterpret_cast<const char*>(file.data() + at);
        size_t nameLen = 0;
        while (nameLen < NameBytes && nameBytes[nameLen] != '\0') ++nameLen;
        t.m_name.assign(nameBytes, nameLen);
        if (t.m_name.empty()) return fileError(&error, "tensor name is empty", head);
        at += NameBytes;
        t.m_dtype = file[at];
        const int ndim = file[at + 1];
        if (t.m_dtype > 1) return fileError(&error, "tensor dtype is neither 0 (float32) nor 1 (int8)", at);
        if (ndim < 1 || ndim > 4) return fileError(&error, "tensor ndim is not in 1..4", at + 1);
        if (readU16(file.data() + at + 2) != 0) return fileError(&error, "tensor reserved field is not zero", at + 2);
        at += 4;
        t.m_count = 1;
        for (int i = 0; i < 4; ++i) {
            t.m_dim[i] = static_cast<int>(readU32(file.data() + at + static_cast<size_t>(i) * 4));
            if (t.m_dim[i] <= 0) return fileError(&error, ("tensor " + t.m_name + " has a dimension that is not positive").c_str(), at + static_cast<size_t>(i) * 4);
            t.m_count *= static_cast<size_t>(t.m_dim[i]);
        }
        at += 16;
        if (t.m_dtype == 1) {
            const size_t bytes = static_cast<size_t>(t.m_dim[0]) * 4;
            if (at + bytes > n) return fileError(&error, ("tensor " + t.m_name + ": the int8 scales run past the end").c_str(), at);
            t.m_scale = reinterpret_cast<const float*>(file.data() + at);
            at += bytes;
        }
        at += (Align - (at % Align)) % Align;
        const size_t bytes = t.m_count * (t.m_dtype == 1 ? 1u : 4u);
        if (at + bytes > n) return fileError(&error, ("tensor " + t.m_name + ": the data runs past the end of the file").c_str(), at);
        t.m_data = file.data() + at;
        at += bytes;
        info.m_parameters += t.m_count;
        tensors.push_back(t);
    }
    if (at != n) return fileError(&error, "trailing bytes after PHOSEND1", at);

    auto find = [&](const std::string& name) -> const Tensor* {
        for (const Tensor& tt : tensors) if (tt.m_name == name) return &tt;
        return nullptr;
    };
    const Tensor* t = nullptr;
    auto need = [&](const std::string& name, int d0, int d1) {
        const Tensor* got = find(name);
        if (got == nullptr) { error = "tensor " + name + " is missing"; return false; }
        if (got->m_dim[0] != d0 || got->m_dim[1] != d1 || got->m_dim[2] != 1 || got->m_dim[3] != 1) {
            std::snprintf(buf, sizeof(buf), "tensor %s is %dx%dx%dx%d, the header asks for %dx%d",
                          name.c_str(), got->m_dim[0], got->m_dim[1], got->m_dim[2], got->m_dim[3], d0, d1);
            error = buf;
            return false;
        }
        t = got;
        return true;
    };

    const int d = info.m_dim, f = info.m_ffn, L = info.m_layers;
    m_headDim = d / info.m_heads;
    m_dimP = roundUpTo(d, Lanes);
    m_ffnP = roundUpTo(f, Lanes);
    m_vocabP = roundUpTo(info.m_vocab, Lanes);
    m_headDimP = roundUpTo(m_headDim, Lanes);
    m_ctxP = roundUpTo(info.m_ctx, Lanes);
    m_qkvP = roundUpTo(3 * d, Lanes);

    // The nine embedding tables (MODEL_FORMAT.md section 4), kick.emb as a tenth when the header
    // declares condKick and mode.emb as an eleventh when it declares condMode. Their rows are summed
    // into the input.
    struct Emb { const char* m_name; int m_rows; std::vector<float>* m_dst; };
    const Emb embeddings[] = {
        { "tok.emb",   info.m_vocab,     &m_tok },
        { "pos.emb",   info.m_ctx,       &m_posE },
        { "role.emb",  info.m_roles,     &m_roleE },
        { "style.emb", info.m_styles,    &m_styleE },
        { "bars.emb",  info.m_condBars,  &m_barsE },
        { "step.emb",  info.m_condStep,  &m_stepE },
        { "bar.emb",   info.m_condBar,   &m_barE },
        { "gap.emb",   info.m_condGap,   &m_gapE },
        { "idx.emb",   info.m_condIdx,   &m_idxE },
        { "kick.emb",  info.m_condKick,  &m_kickE },
        { "mode.emb",  info.m_condMode,  &m_modeE },
    };
    for (const Emb& e : embeddings) {
        if (e.m_rows <= 0) { e.m_dst->clear(); continue; }       // only kick.emb and mode.emb can be absent
        if (!need(e.m_name, e.m_rows, d)) return false;
        copyRows(*t, e.m_rows, d, m_dimP, *e.m_dst);
    }

    m_blocks.assign(static_cast<size_t>(L), Block{});
    for (int i = 0; i < L; ++i) {
        Block& b = m_blocks[static_cast<size_t>(i)];
        const std::string pre = "blocks." + std::to_string(i) + ".";
        if (!need(pre + "norm1.w", d, 1)) return false; copyPadded(*t, d, b.m_n1w);
        if (!need(pre + "norm1.b", d, 1)) return false; copyPadded(*t, d, b.m_n1b);
        if (!need(pre + "attn.qkv.w", 3 * d, d)) return false; packMatrix(*t, 3 * d, d, b.m_qkv);
        if (!need(pre + "attn.qkv.b", 3 * d, 1)) return false; copyPadded(*t, 3 * d, b.m_qkvB);
        if (!need(pre + "attn.out.w", d, d)) return false; packMatrix(*t, d, d, b.m_out);
        if (!need(pre + "attn.out.b", d, 1)) return false; copyPadded(*t, d, b.m_outB);
        if (!need(pre + "norm2.w", d, 1)) return false; copyPadded(*t, d, b.m_n2w);
        if (!need(pre + "norm2.b", d, 1)) return false; copyPadded(*t, d, b.m_n2b);
        if (!need(pre + "ffn.up.w", f, d)) return false; packMatrix(*t, f, d, b.m_up);
        if (!need(pre + "ffn.up.b", f, 1)) return false; copyPadded(*t, f, b.m_upB);
        if (!need(pre + "ffn.down.w", d, f)) return false; packMatrix(*t, d, f, b.m_down);
        if (!need(pre + "ffn.down.b", d, 1)) return false; copyPadded(*t, d, b.m_downB);
        b.m_kCache.assign(static_cast<size_t>(info.m_heads) * static_cast<size_t>(m_ctxP) * static_cast<size_t>(m_headDim), 0.0f);
        b.m_vCache.assign(static_cast<size_t>(info.m_heads) * static_cast<size_t>(info.m_ctx) * static_cast<size_t>(m_headDimP), 0.0f);
    }
    if (!need("norm.w", d, 1)) return false; copyPadded(*t, d, m_normW);
    if (!need("norm.b", d, 1)) return false; copyPadded(*t, d, m_normB);
    if (!need("head.w", info.m_vocab, d)) return false; packMatrix(*t, info.m_vocab, d, m_head);
    if (!need("head.b", info.m_vocab, 1)) return false; copyPadded(*t, info.m_vocab, m_headB);

    m_x.assign(static_cast<size_t>(m_dimP), 0.0f);
    m_nx.assign(static_cast<size_t>(m_dimP), 0.0f);
    m_qkvBuf.assign(static_cast<size_t>(m_qkvP), 0.0f);
    m_att.assign(static_cast<size_t>(m_dimP), 0.0f);
    m_ff.assign(static_cast<size_t>(m_ffnP), 0.0f);
    m_accum.assign(static_cast<size_t>(m_headDimP), 0.0f);
    m_scores.assign(static_cast<size_t>(m_ctxP), 0.0f);
    m_logits.assign(static_cast<size_t>(m_vocabP), 0.0f);
    m_probs.assign(static_cast<size_t>(info.m_vocab), 0.0);

    size_t bytes = 0;
    for (const Block& b : m_blocks)
        bytes += (b.m_qkv.size() + b.m_out.size() + b.m_up.size() + b.m_down.size() + b.m_kCache.size() + b.m_vCache.size()) * sizeof(float);
    bytes += (m_tok.size() + m_posE.size() + m_roleE.size() + m_styleE.size() + m_barsE.size() + m_stepE.size()
              + m_barE.size() + m_gapE.size() + m_idxE.size() + m_kickE.size() + m_modeE.size()
              + m_head.size()) * sizeof(float);
    info.m_bytes = bytes;
    m_info = info;
    m_loaded = true;
    m_pos = 0;
    return true;
}

void NeuralModel::begin(int role, int style, int bars, int mode)
{
    m_role = clampRow(role, m_info.m_roles);
    m_style = clampRow(style, m_info.m_styles);
    m_bars = clampRow(bars - 1, m_info.m_condBars);
    // Clamped like every other row index, and simply not read by a file without mode.emb -- so a
    // composer that always passes the section's mode works with both generations of weight file.
    m_mode = m_info.m_condMode > 0 ? clampRow(mode, m_info.m_condMode) : 0;
    m_pos = 0;
    // The key/value cache is not cleared: only the first m_pos entries are ever read (the attention
    // runs over 0..pos_, and the scores past it are pushed below the softmax's maximum in
    // ModelKernel.h), so what a previous line left behind cannot reach the output. The self test
    // measures exactly that (testModelDecode).
}

bool NeuralModel::step(int token, const NoteCond& cond)
{
    if (!m_loaded || token < 0 || token >= m_info.m_vocab) return false;
    // A line longer than the file's ctx is refused rather than guessed at. MODEL_FORMAT.md says what
    // pos.emb does past its last row (it clamps) but nothing about what the attention should then
    // attend to, and the trained model's reference cases stop at ctx, so there is no oracle for an
    // answer either way. It cannot happen in the composer: ctx is 256 and the longest line any maker
    // in Melody.cpp draws is the 32 onsets of a two-bar acid pattern. The caller (sampleMasked) turns
    // a false into a clean failure, and the composer falls back to the Markov model.
    if (m_pos >= m_info.m_ctx) return false;
    const int d = m_info.m_dim;
    // The nine embedding rows -- ten with the bass's kick table, eleven with mode.emb -- summed.
    // Table reads are gathers and stay on the scalar side (Vec.h has no gather on NEON); the adds are lanes.
    const int posRow = m_pos;
    const size_t stride = static_cast<size_t>(m_dimP);
    std::memcpy(m_x.data(), m_tok.data() + static_cast<size_t>(token) * stride, stride * sizeof(float));
    laneAdd<V>(m_x.data(), m_posE.data() + static_cast<size_t>(posRow) * stride, m_dimP);
    laneAdd<V>(m_x.data(), m_roleE.data() + static_cast<size_t>(m_role) * stride, m_dimP);
    laneAdd<V>(m_x.data(), m_styleE.data() + static_cast<size_t>(m_style) * stride, m_dimP);
    laneAdd<V>(m_x.data(), m_barsE.data() + static_cast<size_t>(m_bars) * stride, m_dimP);
    laneAdd<V>(m_x.data(), m_stepE.data() + static_cast<size_t>(clampRow(cond.m_step, m_info.m_condStep)) * stride, m_dimP);
    laneAdd<V>(m_x.data(), m_barE.data() + static_cast<size_t>(clampRow(cond.m_bar, m_info.m_condBar)) * stride, m_dimP);
    laneAdd<V>(m_x.data(), m_gapE.data() + static_cast<size_t>(clampRow(cond.m_gap, m_info.m_condGap)) * stride, m_dimP);
    laneAdd<V>(m_x.data(), m_idxE.data() + static_cast<size_t>(clampRow(cond.m_idx, m_info.m_condIdx)) * stride, m_dimP);
    if (m_info.m_condKick > 0)
        laneAdd<V>(m_x.data(), m_kickE.data() + static_cast<size_t>(clampRow(cond.m_kick, m_info.m_condKick)) * stride, m_dimP);
    // The mode's row is per line, not per note, so it is read from the state begin() fixed; the add
    // still happens here because the sum is one vector per position (MODEL_FORMAT.md section 4).
    if (m_info.m_condMode > 0)
        laneAdd<V>(m_x.data(), m_modeE.data() + static_cast<size_t>(m_mode) * stride, m_dimP);

    forwardTransformer(posRow);
    ++m_pos;

    laneLayerNorm<V>(m_x.data(), m_normW.data(), m_normB.data(), m_info.m_eps, d, m_nx.data());
    matvecPanel<V>(m_head.data(), m_headB.data(), m_nx.data(), m_info.m_vocab, d, m_logits.data());

    // The probabilities the sampler asks for, in double: a draw multiplies them by constraint
    // weights and raises them to 1/temperature, and float would lose the tail of a peaked model.
    double mx = m_logits[0];
    for (int c = 1; c < m_info.m_vocab; ++c) if (m_logits[static_cast<size_t>(c)] > mx) mx = m_logits[static_cast<size_t>(c)];
    double total = 0.0;
    for (int c = 0; c < m_info.m_vocab; ++c) {
        const double e = std::exp(static_cast<double>(m_logits[static_cast<size_t>(c)]) - mx);
        m_probs[static_cast<size_t>(c)] = e;
        total += e;
    }
    for (int c = 0; c < m_info.m_vocab; ++c) m_probs[static_cast<size_t>(c)] /= total;
    return true;
}

void NeuralModel::forwardTransformer(int t)
{
    const int d = m_info.m_dim, f = m_info.m_ffn, hd = m_headDim, heads = m_info.m_heads;
    const float attScale = 1.0f / vsqrt(static_cast<float>(hd));
    for (int l = 0; l < m_info.m_layers; ++l) {
        Block& b = m_blocks[static_cast<size_t>(l)];
        laneLayerNorm<V>(m_x.data(), b.m_n1w.data(), b.m_n1b.data(), m_info.m_eps, d, m_nx.data());
        matvecPanel<V>(b.m_qkv.data(), b.m_qkvB.data(), m_nx.data(), 3 * d, d, m_qkvBuf.data());
        const float* q = m_qkvBuf.data();
        const float* k = m_qkvBuf.data() + d;
        const float* v = m_qkvBuf.data() + 2 * d;
        for (int h = 0; h < heads; ++h) {
            float* kc = b.m_kCache.data() + static_cast<size_t>(h) * static_cast<size_t>(m_ctxP) * static_cast<size_t>(hd);
            float* vc = b.m_vCache.data() + static_cast<size_t>(h) * static_cast<size_t>(m_info.m_ctx) * static_cast<size_t>(m_headDimP);
            storeKey<V>(kc, t, k + h * hd, hd);
            float* vrow = vc + static_cast<size_t>(t) * static_cast<size_t>(m_headDimP);
            for (int i = 0; i < hd; ++i) vrow[i] = v[h * hd + i];
            for (int i = hd; i < m_headDimP; ++i) vrow[i] = 0.0f;
            // Scores of this query against every key so far -- the same kernel as a matmul, with the
            // time steps as its output rows -- then scaled, softmaxed, and used to mix the values.
            matvecPanel<V>(kc, nullptr, q + h * hd, t + 1, hd, m_scores.data());
            laneScale<V>(m_scores.data(), attScale, roundUpTo(t + 1, Lanes));
            laneSoftmax<V>(m_scores.data(), t + 1);
            // The head's slice of m_att is hd wide and need not be a whole number of lanes, so the
            // accumulation runs in a padded scratch buffer and is copied back: one head's lanes may
            // not spill into the next head's slice.
            float* acc = m_accum.data();
            for (int i = 0; i < m_headDimP; ++i) acc[i] = 0.0f;
            for (int tt = 0; tt <= t; ++tt)
                laneAccumulate<V>(acc, vc + static_cast<size_t>(tt) * static_cast<size_t>(m_headDimP),
                                  m_scores[static_cast<size_t>(tt)], m_headDimP);
            float* outSlice = m_att.data() + h * hd;
            for (int i = 0; i < hd; ++i) outSlice[i] = acc[i];
        }
        for (int i = d; i < m_dimP; ++i) m_att[static_cast<size_t>(i)] = 0.0f;
        matvecPanel<V>(b.m_out.data(), b.m_outB.data(), m_att.data(), d, d, m_nx.data());
        laneAdd<V>(m_x.data(), m_nx.data(), m_dimP);

        laneLayerNorm<V>(m_x.data(), b.m_n2w.data(), b.m_n2b.data(), m_info.m_eps, d, m_nx.data());
        matvecPanel<V>(b.m_up.data(), b.m_upB.data(), m_nx.data(), f, d, m_ff.data());
        laneActivate<V>(m_ff.data(), m_ffnP, m_info.m_act);
        for (int i = f; i < m_ffnP; ++i) m_ff[static_cast<size_t>(i)] = 0.0f;
        matvecPanel<V>(b.m_down.data(), b.m_downB.data(), m_ff.data(), d, f, m_nx.data());
        laneAdd<V>(m_x.data(), m_nx.data(), m_dimP);
    }
}

NeuralModel* sharedMelodyModel(std::string* note)
{
    static NeuralModel model;
    static std::string message;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string error;
        if (!model.load(MelodyModelFile, error))
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
        if (!model.load(BassModelFile, error))
            message = "phosphene: no neural bass model (" + error + "); the composer keeps the pattern families";
        else if (model.info().m_roles <= BassRole) {
            message = "phosphene: " + std::string(BassModelFile) + " has roles=" +
                      std::to_string(model.info().m_roles) + ", so it carries no bass role; the composer "
                      "keeps the pattern families";
            model = NeuralModel{};
        }
    });
    if (note != nullptr) *note = message;
    return model.loaded() ? &model : nullptr;
}

} // namespace phos
