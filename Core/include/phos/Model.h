/**
 * @file Model.h
 * @brief Stage B of the pattern generator: a small neural sequence model over the stage-A alphabet.
 *
 * Phase 5 draws melodies from a Witten-Bell order-2 Markov model of the local MIDI corpus under
 * exact constrained sampling (Corpus.h, Pachet and Roy 2011). Phase 8 keeps the alphabet, the
 * constraint sets and the composer around them and replaces only the predictive model by a small
 * network trained on the same corpus (docs/PLAN.md, 6.9, stage B). This file is the inference half:
 * the reader for the weight file the trainer writes, the architecture it describes, and one forward
 * step given everything drawn so far.
 *
 * **The contract is docs/MODEL_FORMAT.md**, written by the training side; this header implements it
 * and does not extend it. Two architectures are allowed there:
 *
 *  - `arch=transformer`: a pre-LayerNorm decoder (Radford et al., "Language models are unsupervised
 *    multitask learners", 2019), causal attention over at most `ctx` positions, GELU in the tanh
 *    form (Hendrycks and Gimpel, arXiv:1606.08415, 2016). Musical precedents: Music Transformer
 *    (Huang et al., ICLR 2019), Compound Word Transformer (Hsiao et al., AAAI 2021).
 *  - `arch=ssm`: a selective state-space block with a real diagonal A (Gu and Dao, "Mamba:
 *    linear-time sequence modeling with selective state spaces", 2023). **Not implemented**, on
 *    purpose: the training run of 16.09.2026 measured it at a held-out NLL of 2.16 nats per token
 *    against the transformer's 1.49 and exported only the transformer, so the second forward pass
 *    would be dead code. The loader reads and checks an `arch=ssm` header and then refuses it by
 *    name, and Model.cpp says what would have to be written; the seam is the only thing that exists.
 *
 * **What the model sees.** A line is the sequence of note onsets of one pattern. At position t the
 * *token* is the symbol of the previous note (12, the interval 0, before the first), and the
 * *conditioning* describes the note being predicted: role, style, the pattern's length in bars, and
 * per note its step in the bar, its bar, the gap to the next note and a bucket of its index. The
 * composer knows every one of those before it draws a pitch, because all three makers in Melody.cpp
 * draw the rhythm first. Nine embedding rows are summed into the input; the output is a
 * distribution over the 37 symbols and nothing else.
 *
 * **Threading.** A model is loaded once, on the composer's thread, and is read-only afterwards
 * except for its own scratch buffers -- so one NeuralModel serves one thread. The audio thread never
 * touches it. Nothing allocates after load().
 *
 * **int8** weights are dequantized to float32 when they are packed, so the file is small and the
 * arithmetic is float. That costs four bytes per weight in memory instead of one; the trained model
 * of 16.09.2026 is 1.5 MB on disk and 6.4 MB packed.
 */
#pragma once
#include "phos/Corpus.h"
#include "phos/ModelKernel.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace phos {

/** @brief The architectures docs/MODEL_FORMAT.md allows. */
enum class ModelArch : int { Transformer = 0, Ssm };

/** @brief What the text header of a weight file says (docs/MODEL_FORMAT.md, section 2). */
struct ModelInfo {
    ModelArch arch = ModelArch::Transformer;   ///< @c arch
    int layers = 0;          ///< @c layers: blocks
    int dim = 0;             ///< @c dim: width of the residual stream
    int heads = 0;           ///< @c heads (0 for an SSM)
    int ffn = 0;             ///< @c ffn: feed-forward hidden width (0 for an SSM)
    int state = 0;           ///< @c state: SSM state size per inner channel
    int vocab = 0;           ///< @c vocab: must be kCorpusAlphabet
    int ctx = 0;             ///< @c ctx: rows of pos.emb; later positions clamp to the last row
    int roles = 0;           ///< @c roles: 3
    int styles = 0;          ///< @c styles: 6 (0 = unknown, then StyleId + 1)
    int tokenVersion = 0;    ///< @c tokenVersion: 1 is the stage-A alphabet
    int relMin = 0;          ///< @c relMin: must be kCorpusRelMin
    int relMax = 0;          ///< @c relMax: must be kCorpusRelMax
    int condStep = 0;        ///< @c condStep: rows of step.emb (16)
    int condBar = 0;         ///< @c condBar: rows of bar.emb (8)
    int condGap = 0;         ///< @c condGap: rows of gap.emb (10)
    int condIdx = 0;         ///< @c condIdx: rows of idx.emb (8)
    int condBars = 0;        ///< @c condBars: rows of bars.emb (8)
    int expand = 0;          ///< @c expand: SSM inner expansion factor
    int dtRank = 0;          ///< @c dtRank: SSM rank of the delta projection
    int convK = 0;           ///< @c convK: SSM depthwise kernel width
    bool int8 = false;       ///< @c quant=int8 (per tensor the dtype byte still decides)
    ModelAct act = ModelAct::GeluTanh;   ///< @c act
    float eps = 1e-5f;       ///< @c eps: the norm's epsilon
    double nll = 0.0;        ///< @c nll: held-out negative log likelihood per token, in nats
    std::string text;        ///< the header as it stood in the file
    size_t parameters = 0;   ///< weights read from the file
    size_t bytes = 0;        ///< memory the packed weights and the caches take
};

/**
 * @brief What the composer knows about the note it is about to draw (MODEL_FORMAT.md, section 3).
 *
 * All four come out of the onset list the maker has already drawn, before any pitch exists.
 */
struct NoteCond {
    int step = 0;   ///< step in the bar, 0..15
    int bar = 0;    ///< bar in the pattern, 0..7
    int gap = 0;    ///< 0 for the last note, else the distance to the next capped at 8, 9 above that
    int idx = 0;    ///< bucket of the note's index in the line, 0..7
};

/** @brief The index bucket of note number @p t (MODEL_FORMAT.md, section 3). */
inline int noteIndexBucket(int t)
{
    if (t < 4) return t;
    if (t < 6) return 4;
    if (t < 10) return 5;
    if (t < 16) return 6;
    return 7;
}

/** @brief The gap code between a note at @p step and the next onset (or none). */
inline int noteGapCode(int step, int nextStep, bool hasNext)
{
    if (!hasNext) return 0;
    const int d = nextStep - step;
    if (d > 8) return 9;
    return d < 1 ? 1 : d;
}

/**
 * @brief A loaded model and one line's worth of state.
 *
 * Bit-identical between AVX2, NEON and the scalar path: every matrix-vector product runs with the
 * output rows in the lanes, every reduction in a scalar loop (ModelKernel.h).
 */
class NeuralModel {
public:
    /**
     * @brief Reads a weight file and packs it for this machine's lane width.
     * @param path  the `.phosmdl` file; a bare name is also looked for in setModelSearchPath()
     * @param error receives a message naming the field and the byte offset when the file is not
     *              what it claims to be
     * @return false on any failure; the model is then unusable and loaded() stays false
     */
    bool load(const char* path, std::string& error);

    /** @brief Whether a model is loaded. */
    bool loaded() const { return loaded_; }
    /** @brief What the header said, plus the measured sizes. */
    const ModelInfo& info() const { return info_; }
    /** @brief Symbols in the alphabet (kCorpusAlphabet for tokenVersion 1). */
    int alphabet() const { return info_.vocab; }
    /** @brief The symbol that starts a line: the interval 0, as MODEL_FORMAT.md section 3 defines it. */
    static int startToken() { return -kCorpusRelMin; }

    /**
     * @brief Starts a line.
     * @param role  0 acid, 1 lead, 2 arp
     * @param style 0 unknown, else StyleId + 1 -- the trained corpus carries no style label, so the
     *              composer passes 0 (MODEL_FORMAT.md, the warning in section 3)
     * @param bars  the pattern's length in bars, 1..8 (stored as bars - 1)
     * Every argument is clamped into the range the file declares.
     */
    void begin(int role, int style, int bars);

    /**
     * @brief Computes the distribution of the next note.
     * @param token the previous note's symbol, startToken() before the first
     * @param cond  what is known about the note being predicted
     * @return false when the model is not loaded, the token is not a symbol, or the line has
     *         already reached the file's @c ctx positions -- see Model.cpp for why a longer line is
     *         refused instead of guessed at. The longest line the composer draws is 32 notes and
     *         the trained model's ctx is 256, so it does not arise.
     */
    bool step(int token, const NoteCond& cond);

    /** @brief How many notes have been predicted since begin(). */
    int length() const { return pos_; }
    /** @brief Logits of the next symbol; valid after a step(). */
    const float* logits() const { return logits_.data(); }
    /** @brief The softmax of logits(), as a probability of symbol @p c. */
    double prob(int c) const { return probs_[static_cast<size_t>(c)]; }

private:
    void forwardTransformer(int t);

    ModelInfo info_;
    bool loaded_ = false;
    int role_ = 0, style_ = 0, bars_ = 0, pos_ = 0;
    int headDim_ = 0;
    int dimP_ = 0, ffnP_ = 0, vocabP_ = 0, headDimP_ = 0, ctxP_ = 0, qkvP_ = 0;

    /** @brief Weights of one block, already packed into panels (empty for the other architecture). */
    struct Block {
        std::vector<float> n1w, n1b, n2w, n2b;                      ///< the norms
        std::vector<float> qkv, qkvB, out, outB, up, upB, down, downB;   ///< the transformer
        std::vector<float> kCache, vCache;                          ///< keys in time panels, values row major
    };
    std::vector<Block> blocks_;
    /** @brief The nine embedding tables, in the order of MODEL_FORMAT.md section 4. */
    std::vector<float> tok_, posE_, roleE_, styleE_, barsE_, stepE_, barE_, gapE_, idxE_;
    std::vector<float> normW_, normB_, head_, headB_;
    std::vector<float> x_, nx_, qkvBuf_, att_, ff_, accum_, scores_, logits_;
    std::vector<double> probs_;
};

/**
 * @brief Where load() looks when it is given a bare file name.
 *
 * The plugin and the Quest app set this to their own resource directory once at start-up. Empty by
 * default, in which case the working directory and the compiled-in source data directory are tried.
 */
void setModelSearchPath(const std::string& directory);

/** @brief The file name of the melody model inside the search path. */
constexpr const char* kMelodyModelFile = "melody.phosmdl";

/**
 * @brief The melody model, loaded once on first use.
 * @param note receives a line for the log when the model could not be loaded (empty otherwise)
 * @return the model, or null when there is none -- the composer then stays on the Markov model.
 *         Not const: a draw writes the model's cache. One instance, one thread.
 *
 * Loaded lazily, on whichever thread composes first; in the plugin that is the composer thread,
 * never the audio thread. Never unloaded, because a set may start a new track at any time.
 */
NeuralModel* sharedMelodyModel(std::string* note = nullptr);

/** @brief Drives one model over one line; what sampleMasked() talks to. */
struct NeuralStepper {
    NeuralModel* model = nullptr;             ///< the model (mutable: the cache is per line)
    int role = 0;                             ///< 0 acid, 1 lead, 2 arp
    int style = 0;                            ///< 0 unknown (the trained corpus has no style label)
    int bars = 1;                             ///< the pattern's length in bars
    const std::vector<NoteCond>* cond = nullptr;   ///< one entry per position, from the onset list
    int previous = 0;                         ///< the symbol drawn last

    int alphabet() const { return model->alphabet(); }
    /** @brief Resets; an order-2 context's older symbol means nothing to a long-context model. */
    void begin(int, int start1) { model->begin(role, style, bars); previous = start1; }
    bool advance(int i) { return model->step(previous, (*cond)[static_cast<size_t>(i)]); }
    void observe(int symbol) { previous = symbol; }
    double prob(int c) const { return model->prob(c); }
};

/** @brief What one masked draw had to do to succeed. */
struct MaskedDrawStats {
    int retries = 0;        ///< draws thrown away because a position had no probability mass left
    bool fellBack = false;  ///< every retry failed; the symbols came from the constraint weights alone
};

/** @brief A position counts as dead when the model gives its whole allowed set less than this. */
constexpr double kMaskedMassFloor = 1e-6;
/** @brief How often a draw is restarted before the constraint weights decide alone. */
constexpr int kMaskedRetries = 4;

/**
 * @brief Draws a sequence under per-position allowed sets from a model with unbounded context.
 *
 * **What this is, and what it is not.** Stage A samples *exactly* from the model conditioned on the
 * constraints: with an order-2 chain the probability of being able to finish from every state can be
 * computed backwards, and the forward draw is weighted by it (Pachet and Roy, "Markov constraints",
 * Constraints 16(2), 2011). That recursion needs a finite state, and a transformer's state is the
 * whole prefix, so it is not available here -- MODEL_FORMAT.md section 6 says the same and asks for
 * masked left-to-right sampling. At each position the model's distribution is restricted to the
 * allowed symbols, weighted, renormalised and drawn from. That is the sampler the self test of stage
 * A deliberately measures as *wrong*: it has no way of knowing that a symbol it likes now leads into
 * a position where the model has little to say. With unary constraints it can never paint itself
 * into a corner -- every position's allowed set is fixed in advance and non-empty -- so it always
 * produces a sequence; what it loses is that the sequence is drawn from the right distribution. The
 * self test measures how far off that is (testModelDecode), and docs/PLAN.md says so in as many
 * words.
 *
 * The one failure it can have is numerical: a model confident enough that its whole allowed set
 * underflows. Then the draw is restarted -- the random stream has moved on, so a restart is not a
 * repeat -- up to kMaskedRetries times, and after that the constraint weights alone decide.
 *
 * @tparam Stepper  int alphabet() const; void begin(int start2, int start1); bool advance(int i);
 *                  void observe(int symbol); double prob(int) const
 * @param  m        the model, conditioned and ready
 * @param  allowed  allowed[i][s] = 0 forbids symbol s at position i, any other value is its relative
 *                  weight (the energy arc's colour weights; Melody.h), exactly as in stage A
 * @param  start2,start1 the context before the first position; a stage-B model uses only start1
 * @param  temperature   the model's probabilities are raised to 1/temperature, as in stage A
 * @param  uniform  source of uniform numbers in [0, 1)
 * @param  out      receives the symbols
 * @param  stats    receives what the draw cost, or null
 * @return false only when a position has an empty allowed set or the model refuses a step
 */
template <class Stepper, class Uniform>
bool sampleMasked(Stepper& m, const std::vector<std::vector<uint8_t>>& allowed, int start2, int start1,
                  double temperature, Uniform&& uniform, std::vector<int>& out, MaskedDrawStats* stats = nullptr)
{
    const int A = m.alphabet();
    const int n = static_cast<int>(allowed.size());
    out.clear();
    if (stats != nullptr) *stats = MaskedDrawStats{};
    if (n == 0) return true;
    for (int i = 0; i < n; ++i) {
        bool any = false;
        for (int c = 0; c < A; ++c) if (allowed[static_cast<size_t>(i)][static_cast<size_t>(c)]) any = true;
        if (!any) return false;
    }
    const double expo = temperature > 0.0 ? 1.0 / temperature : 1.0;
    std::vector<double> w(static_cast<size_t>(A));
    for (int attempt = 0; attempt <= kMaskedRetries; ++attempt) {
        m.begin(start2, start1);
        out.clear();
        bool ok = true;
        for (int i = 0; i < n && ok; ++i) {
            if (!m.advance(i)) return false;
            const std::vector<uint8_t>& mask = allowed[static_cast<size_t>(i)];
            double total = 0.0, mass = 0.0;
            for (int c = 0; c < A; ++c) {
                w[static_cast<size_t>(c)] = 0.0;
                if (mask[static_cast<size_t>(c)] == 0) continue;
                const double p = m.prob(c);
                mass += p;
                const double v = (expo == 1.0 ? p : std::pow(p, expo)) * mask[static_cast<size_t>(c)];
                w[static_cast<size_t>(c)] = v;
                total += v;
            }
            if (mass < kMaskedMassFloor || total <= 0.0) {
                ok = false;
                if (stats != nullptr) ++stats->retries;
                break;
            }
            double r = uniform() * total;
            int chosen = -1;
            for (int c = 0; c < A; ++c) {
                if (w[static_cast<size_t>(c)] <= 0.0) continue;
                chosen = c;   // the last symbol with weight takes what rounding leaves over
                if (r < w[static_cast<size_t>(c)]) break;
                r -= w[static_cast<size_t>(c)];
            }
            out.push_back(chosen);
            m.observe(chosen);
        }
        if (ok) return true;
    }
    // Every attempt died: draw from the constraint weights alone, which is still inside the
    // constraints and still deterministic, and say so.
    out.clear();
    for (int i = 0; i < n; ++i) {
        const std::vector<uint8_t>& mask = allowed[static_cast<size_t>(i)];
        double total = 0.0;
        for (int c = 0; c < A; ++c) total += mask[static_cast<size_t>(c)];
        double r = uniform() * total;
        int chosen = -1;
        for (int c = 0; c < A; ++c) {
            if (mask[static_cast<size_t>(c)] == 0) continue;
            chosen = c;
            if (r < mask[static_cast<size_t>(c)]) break;
            r -= mask[static_cast<size_t>(c)];
        }
        out.push_back(chosen);
    }
    if (stats != nullptr) stats->fellBack = true;
    return true;
}

} // namespace phos
