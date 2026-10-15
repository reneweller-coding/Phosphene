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
 * **The mode (`condMode`, 18.09.2026).** An eleventh table, optional exactly as the bass's
 * `kick.emb` is: a file without `condMode` has no `mode.emb`, the argument of begin() is ignored, and
 * the file is the model it was before. The row is the mode of the line -- one value per line, like
 * the role -- and the composer has it because the section's mode is fixed by the form before any
 * pitch is drawn (Form.h, modal interchange). The *training* label has to be estimated from the
 * notes (Tools/train/mode.py), because the corpus is transposed to a common tonic and no file says
 * which minor it is; how much of that label is noise is measured there and in
 * docs/MODEL_FORMAT.md section 3.
 *
 * **Threading.** A model is loaded once, on the composer's thread, and is read-only afterwards
 * except for its own scratch buffers -- so one NeuralModel serves one thread. The audio thread never
 * touches it. Nothing allocates after load().
 *
 * **int8** weights are dequantized to float32 when they are packed, so the file is small and the
 * arithmetic is float. That costs four bytes per weight in memory instead of one; the trained model
 * of 16.09.2026 is 1.5 MB on disk and 6.4 MB packed.
 * @author Rene Weller
 */
#pragma once
#include "phos/Corpus.h"
#include "phos/ModelKernel.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace phos
{

/** @brief The architectures docs/MODEL_FORMAT.md allows. */
enum class ModelArchE : int
{
    TRANSFORMER = 0,
    SSM
};

/** @brief What the text header of a weight file says (docs/MODEL_FORMAT.md, section 2). */
struct ModelInfo
{
    ModelArchE  m_arch = ModelArchE::TRANSFORMER;   ///< @c arch
    int         m_layers = 0;                       ///< @c layers: blocks
    int         m_dim = 0;                          ///< @c dim: width of the residual stream
    int         m_heads = 0;                        ///< @c heads (0 for an SSM)
    int         m_ffn = 0;                          ///< @c ffn: feed-forward hidden width (0 for an SSM)
    int         m_state = 0;                        ///< @c state: SSM state size per inner channel
    int         m_vocab = 0;                        ///< @c vocab: must be CorpusAlphabet
    int         m_ctx = 0;                          ///< @c ctx: rows of pos.emb; later positions clamp to the last row
    int         m_roles = 0;                        ///< @c roles: 3
    int         m_styles = 0;                       ///< @c styles: 6 (0 = unknown, then StyleId + 1)
    int         m_tokenVersion = 0;                 ///< @c tokenVersion: 1 is the stage-A alphabet
    int         m_relMin = 0;                       ///< @c relMin: must be CorpusRelMin
    int         m_relMax = 0;                       ///< @c relMax: must be CorpusRelMax
    int         m_condStep = 0;                     ///< @c condStep: rows of step.emb (16)
    int         m_condBar = 0;                      ///< @c condBar: rows of bar.emb (8)
    int         m_condGap = 0;                      ///< @c condGap: rows of gap.emb (10)
    int         m_condIdx = 0;                      ///< @c condIdx: rows of idx.emb (8)
    int         m_condBars = 0;                     ///< @c condBars: rows of bars.emb (8)
    int         m_condKick = 0;   ///< @c condKick: rows of kick.emb (3), or 0 in a file without the bass role
    int         m_condMode = 0;   ///< @c condMode: rows of mode.emb (6), or 0 in a file trained mode-blind
    int         m_expand = 0;     ///< @c expand: SSM inner expansion factor
    int         m_dtRank = 0;     ///< @c dtRank: SSM rank of the delta projection
    int         m_convK = 0;      ///< @c convK: SSM depthwise kernel width
    bool        m_int8 = false;   ///< @c quant=int8 (per tensor the dtype byte still decides)
    ModelActE   m_act = ModelActE::GELU_TANH;   ///< @c act
    float       m_eps = 1e-5f;                  ///< @c eps: the norm's epsilon
    double      m_nll = 0.0;                    ///< @c nll: held-out negative log likelihood per token, in nats
    std::string m_text;                         ///< the header as it stood in the file
    size_t      m_parameters = 0;               ///< weights read from the file
    size_t      m_bytes = 0;                    ///< memory the packed weights and the caches take
};

/**
 * @brief What the composer knows about the note it is about to draw (MODEL_FORMAT.md, section 3).
 *
 * All four come out of the onset list the maker has already drawn, before any pitch exists.
 */
struct NoteCond
{
    int m_step = 0;   ///< step in the bar, 0..15
    int m_bar = 0;    ///< bar in the pattern, 0..7
    int m_gap = 0;    ///< 0 for the last note, else the distance to the next capped at 8, 9 above that
    int m_idx = 0;    ///< bucket of the note's index in the line, 0..7
    int m_kick = 0;   ///< kick class of the step (kickClass()); read only by a file with @c condKick
};

/**
 * @brief Where a note sits relative to the kick: 0 on it, 1 the sixteenth after it, 2 the rest.
 *
 * The fifth conditioning input. A psytrance bass is defined by
 * its interlock with a four-on-the-floor kick -- it plays in the gaps and leans on the sixteenth
 * right after the beat (Solberg and Dibben, "Peak experiences with electronic dance music", Music
 * Perception 36(4), 2019). Measured on the corpus before it was built in: a bass onset falls on one
 * of the four kick steps in 22.5 % of the bars against 62.4 % of the steps between them.
 *
 * Three classes and not sixteen distances, because @c step already carries the exact position; what
 * this adds is the relation to the kick, which survives the bars in which `Composer` takes kicks away
 * (a breakdown, the pre-drop bar) and which a model can therefore generalise over.
 *
 * **What it was measured to be worth: nothing, so far.** The same model trained with and without this
 * table scores 0.4323 against 0.4293 nats on the same held-out split -- a paired bootstrap over lines
 * puts the difference at -0.0030 nats, 95 % interval [-0.0208, +0.0150]. With a kick on every beat
 * the class is a *function of* @c step, so it carries nothing the model was missing -- and class 0
 * never even occurs in this composer, because none of the five pattern families of Patterns.h puts
 * a bass note on the beat at all. It is kept
 * because it costs 576 parameters, because the self test proves it reaches the output, and because a
 * corpus that really contains a kick could fill it without a format change (docs/MODEL_FORMAT.md 7).
 * @param[in] step  the sixteenth
 * @return 0 on the kick, 1 the sixteenth after, 2 the rest
 */
inline int kickClass( int step )
{
    const int s = ( ( step % 16 ) + 16 ) % 16;
    if ( s % 4 == 0 ) return 0;
    if ( s % 4 == 1 ) return 1;
    return 2;
}

/**
 * @brief The index bucket of note number @p t (MODEL_FORMAT.md, section 3).
 * @param[in] t  the note number
 * @return its bucket
 */
inline int noteIndexBucket( int t )
{
    if ( t < 4 ) return t;
    if ( t < 6 ) return 4;
    if ( t < 10 ) return 5;
    if ( t < 16 ) return 6;
    return 7;
}

/**
 * @brief The gap code between a note at @p step and the next onset (or none).
 * @param[in] step      the note's sixteenth
 * @param[in] nextStep  the next onset's sixteenth
 * @param[in] hasNext   true when there is a next onset
 * @return the gap code
 */
inline int noteGapCode( int step, int nextStep, bool hasNext )
{
    if ( !hasNext ) return 0;
    const int d = nextStep - step;
    if ( d > 8 ) return 9;
    return d < 1 ? 1 : d;
}

/**
 * @brief A loaded model and one line's worth of state.
 *
 * Bit-identical between AVX2, NEON and the scalar path: every matrix-vector product runs with the
 * output rows in the lanes, every reduction in a scalar loop (ModelKernel.h).
 */
class NeuralModel
{
public:
    /**
     * @brief Reads a weight file and packs it for this machine's lane width.
     * @param[in]  path   the `.phosmdl` file; a bare name is also looked for in setModelSearchPath()
     * @param[out] error  receives a message naming the field and the byte offset when the file is not what it claims to
     *                    be
     * @return false on any failure; the model is then unusable and loaded() stays false
     */
    bool load( const char *path, std::string &error );

    /**
     * @brief Whether a model is loaded.
     * @return true when a model is loaded
     */
    bool loaded() const { return m_loaded; }
    /**
     * @brief What the header said, plus the measured sizes.
     * @return the model's facts
     */
    const ModelInfo &info() const { return m_info; }
    /**
     * @brief Symbols in the alphabet (CorpusAlphabet for tokenVersion 1).
     * @return the number of symbols
     */
    int alphabet() const { return m_info.m_vocab; }
    /**
     * @brief The symbol that starts a line: the interval 0, as MODEL_FORMAT.md section 3 defines it.
     * @return the start symbol
     */
    static int startToken() { return -CorpusRelMin; }

    /**
     * @brief Starts a line.
     * @param[in] role   0 acid, 1 lead, 2 arp, 3 bass (only in a file whose header says @c roles=4)
     * @param[in] style  0 unknown, else StyleId + 1 -- the trained corpus carries no style label, so the composer
     *                   passes 0 (MODEL_FORMAT.md, the warning in section 3)
     * @param[in] bars   the pattern's length in bars, 1..8 (stored as bars - 1)
     * @param[in] mode   the line's mode, 0..5 in the order of @c phos::ScaleSteps; read only by a file whose header
     *                   carries @c condMode, ignored (and harmless) by every older file Every argument is clamped into
     *                   the range the file declares.
     */
    void begin( int role, int style, int bars, int mode = 0 );

    /**
     * @brief Computes the distribution of the next note.
     * @param[in] token  the previous note's symbol, startToken() before the first
     * @param[in] cond   what is known about the note being predicted
     * @return false when the model is not loaded, the token is not a symbol, or the line has already reached the file's
     *         @c ctx positions -- see Model.cpp for why a longer line is refused instead of guessed at. The longest
     *         line the composer draws is 32 notes and the trained model's ctx is 256, so it does not arise.
     */
    bool step( int token, const NoteCond &cond );

    /**
     * @brief How many notes have been predicted since begin().
     * @return the notes predicted
     */
    int length() const { return m_pos; }
    /**
     * @brief Logits of the next symbol; valid after a step().
     * @return the logits
     */
    const float *logits() const { return m_logits.data(); }
    /**
     * @brief The softmax of logits(), as a probability of symbol @p c.
     * @param[in] c  the symbol
     * @return its probability
     */
    double prob( int c ) const { return m_probs[static_cast<size_t>( c )]; }

private:
    /**
     * @brief Runs the transformer's forward pass for position @p t and fills the logits.
     * @param[in] t  the position
     */
    void forwardTransformer( int t );

    ModelInfo m_info;             ///< the file's header: architecture and sizes
    bool      m_loaded = false;   ///< a model file was read
    int       m_role = 0;         ///< the line's role (a conditioning row)
    int       m_style = 0;        ///< its style
    int       m_bars = 0;         ///< its bars
    int       m_mode = 0;         ///< its mode
    int       m_pos = 0;          ///< the position in the line
    int       m_headDim = 0;      ///< dimensions per attention head
    int       m_dimP = 0;         ///< the residual width, padded to the lane width
    int       m_ffnP = 0;         ///< the feed-forward width, padded
    int       m_vocabP = 0;       ///< the alphabet, padded
    int       m_headDimP = 0;     ///< a head's width, padded
    int       m_ctxP = 0;         ///< the context, padded
    int       m_qkvP = 0;         ///< the qkv width, padded

    /** @brief Weights of one block, already packed into panels (empty for the other architecture). */
    struct Block
    {
        std::vector<float> m_n1w;      ///< the first norm's weights
        std::vector<float> m_n1b;      ///< ... its biases
        std::vector<float> m_n2w;      ///< the second norm's weights
        std::vector<float> m_n2b;      ///< ... its biases
        std::vector<float> m_qkv;      ///< the query, key and value projection
        std::vector<float> m_qkvB;     ///< ... its biases
        std::vector<float> m_out;      ///< the attention's output projection
        std::vector<float> m_outB;     ///< ... its biases
        std::vector<float> m_up;       ///< the feed-forward's up projection
        std::vector<float> m_upB;      ///< ... its biases
        std::vector<float> m_down;     ///< the feed-forward's down projection
        std::vector<float> m_downB;    ///< ... its biases
        std::vector<float> m_kCache;   ///< the keys so far, in time panels
        std::vector<float> m_vCache;   ///< the values so far, row major
    };
    std::vector<Block> m_blocks;   ///< the layers
    /** @brief The nine embedding tables, in the order of MODEL_FORMAT.md section 4, plus the two
     *         optional ones (kick.emb, mode.emb) that are empty in a file that does not carry them. */
    std::vector<float>  m_tok;      ///< the token embedding
    std::vector<float>  m_posE;     ///< the position embedding
    std::vector<float>  m_roleE;    ///< the role embedding
    std::vector<float>  m_styleE;   ///< the style embedding
    std::vector<float>  m_barsE;    ///< the bars embedding
    std::vector<float>  m_stepE;    ///< the step embedding
    std::vector<float>  m_barE;     ///< the bar embedding
    std::vector<float>  m_gapE;     ///< the gap embedding
    std::vector<float>  m_idxE;     ///< the index embedding
    std::vector<float>  m_kickE;    ///< the kick embedding (empty without condKick)
    std::vector<float>  m_modeE;    ///< the mode embedding (empty without condMode)
    std::vector<float>  m_normW;    ///< the final norm's weights
    std::vector<float>  m_normB;    ///< ... its biases
    std::vector<float>  m_head;     ///< the output head
    std::vector<float>  m_headB;    ///< the head's biases
    std::vector<float>  m_x;        ///< the residual stream
    std::vector<float>  m_nx;       ///< the normed stream
    std::vector<float>  m_qkvBuf;   ///< the queries, keys and values of the position
    std::vector<float>  m_att;      ///< the attention's output
    std::vector<float>  m_ff;       ///< the feed-forward's hidden layer
    std::vector<float>  m_accum;    ///< the accumulators of a product
    std::vector<float>  m_scores;   ///< the attention scores
    std::vector<float>  m_logits;   ///< the logits
    std::vector<double> m_probs;    ///< softmax of the logits
};

/**
 * @brief Where load() looks when it is given a bare file name.
 *
 * The plugin and the Quest app set this to their own resource directory once at start-up. Empty by
 * default, in which case the working directory and the compiled-in source data directory are tried.
 * @param[in] directory  the directory
 */
void setModelSearchPath( const std::string &directory );

/** @brief The file name of the melody model inside the search path. */
constexpr const char *MelodyModelFile = "melody.phosmdl";

/** @brief The file name of the bass model inside the search path. */
constexpr const char *BassModelFile = "bass.phosmdl";

/** @brief The role index of the bass in a four-role file (acid, lead, arp, bass). */
constexpr int BassRole = 3;

/**
 * @brief The melody model, loaded once on first use.
 * @param[out] note  receives a line for the log when the model could not be loaded (empty otherwise)
 * @return the model, or null when there is none -- the composer then stays on the Markov model. Not const: a draw
 *         writes the model's cache. One instance, one thread. Loaded lazily, on whichever thread composes first; in the
 *         plugin that is the composer thread, never the audio thread. Never unloaded, because a set may start a new
 *         track at any time.
 */
NeuralModel *sharedMelodyModel( std::string *note = nullptr );

/**
 * @brief The bass model, loaded once on first use (Core/data/bass.phosmdl).
 * @param[out] note  receives a line for the log when the model could not be loaded (empty otherwise)
 * @return the model, or null when there is none -- the composer then keeps the pattern families. A second instance and
 *         not the melody model's, although both files may carry all four roles: one NeuralModel holds one line's
 *         key/value cache, and the bass and the melodic parts are drawn in the same composing pass. Loaded lazily on
 *         whichever thread composes first -- the composer thread in the plugin, never the audio thread -- and never
 *         unloaded.
 */
NeuralModel *sharedBassModel( std::string *note = nullptr );

/** @brief Drives one model over one line; what sampleMasked() talks to. */
struct NeuralStepper
{
    NeuralModel                 *m_model = nullptr;   ///< the model (mutable: the cache is per line)
    int                          m_role = 0;          ///< 0 acid, 1 lead, 2 arp, 3 bass
    int                          m_style = 0;         ///< 0 unknown (the trained corpus has no style label)
    int                          m_bars = 1;          ///< the pattern's length in bars
    const std::vector<NoteCond> *m_cond = nullptr;    ///< one entry per position, from the onset list
    int                          m_previous = 0;      ///< the symbol drawn last
    int                          m_mode = 0;          ///< the section's mode (ScaleSteps); ignored by a file
                                                      ///< without @c condMode. **Last on purpose**: the bass
                                                      ///< draw in Composer.cpp initialises this aggregate
                                                      ///< positionally and does not name the mode.

    /**
     * @brief The model's alphabet.
     * @return the number of symbols
     */
    int alphabet() const { return m_model->alphabet(); }
    /**
     * @brief Resets; an order-2 context's older symbol means nothing to a long-context model.
     * @param[in] start1  the line's first symbol
     */
    void begin( int, int start1 )
    {
        m_model->begin( m_role, m_style, m_bars, m_mode );
        m_previous = start1;
    }
    /**
     * @brief Computes the distribution at position @p i of the line; false where the model cannot.
     * @param[in] i  the position in the line
     * @return false where the model cannot
     */
    bool advance( int i ) { return m_model->step( m_previous, ( *m_cond )[static_cast<size_t>( i )] ); }
    /**
     * @brief Takes @p symbol as drawn.
     * @param[in] symbol  the symbol drawn
     */
    void observe( int symbol ) { m_previous = symbol; }
    /**
     * @brief The probability of symbol @p c.
     * @param[in] c  the symbol
     * @return its probability
     */
    double prob( int c ) const { return m_model->prob( c ); }
};

/** @brief What one masked draw had to do to succeed. */
struct MaskedDrawStats
{
    int  m_retries = 0;        ///< draws thrown away because a position had no probability mass left
    bool m_fellBack = false;   ///< every retry failed; the symbols came from the constraint weights alone
};

/** @brief A position counts as dead when the model gives its whole allowed set less than this. */
constexpr double MaskedMassFloor = 1e-6;
/** @brief How often a draw is restarted before the constraint weights decide alone. */
constexpr int MaskedRetries = 4;

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
 * self test measures how far off that is (testModelDecode), and docs/rounds/2026-09.md says so in as many
 * words.
 *
 * The one failure it can have is numerical: a model confident enough that its whole allowed set
 * underflows. Then the draw is restarted -- the random stream has moved on, so a restart is not a
 * repeat -- up to MaskedRetries times, and after that the constraint weights alone decide.
 * @param[in,out] m            the model, conditioned and ready
 * @param[in]     allowed      allowed[i][s] = 0 forbids symbol s at position i, any other value is its relative weight
 *                             (the energy arc's colour weights; Melody.h), exactly as in stage A
 * @param[in]     start2       the context before the first position; a stage-B model uses only start1
 * @param[in]     start1       the context before the first position; a stage-B model uses only start1
 * @param[in]     temperature  the model's probabilities are raised to 1/temperature, as in stage A
 * @param[in]     uniform      source of uniform numbers in [0, 1)
 * @param[out]    out          receives the symbols
 * @param[out]    stats        receives what the draw cost, or null
 * @return false only when a position has an empty allowed set or the model refuses a step
 * @tparam Stepper  int alphabet() const; void begin(int start2, int start1); bool advance(int i); void observe(int
 *         symbol); double prob(int) const
 */
template <class StepperT, class UniformT>
bool sampleMasked( StepperT &m, const std::vector<std::vector<uint8_t>> &allowed, int start2, int start1,
                   double temperature, UniformT &&uniform, std::vector<int> &out, MaskedDrawStats *stats = nullptr )
{
    const int A = m.alphabet();
    const int n = static_cast<int>( allowed.size() );
    out.clear();
    if ( stats != nullptr ) *stats = MaskedDrawStats{};
    if ( n == 0 ) return true;
    for ( int i = 0; i < n; ++i )
    {
        bool any = false;
        for ( int c = 0; c < A; ++c )
            if ( allowed[static_cast<size_t>( i )][static_cast<size_t>( c )] ) any = true;
        if ( !any ) return false;
    }
    const double        expo = temperature > 0.0 ? 1.0 / temperature : 1.0;
    std::vector<double> w( static_cast<size_t>( A ) );
    for ( int attempt = 0; attempt <= MaskedRetries; ++attempt )
    {
        m.begin( start2, start1 );
        out.clear();
        bool ok = true;
        for ( int i = 0; i < n && ok; ++i )
        {
            if ( !m.advance( i ) ) return false;
            const std::vector<uint8_t> &mask = allowed[static_cast<size_t>( i )];
            double                      total = 0.0, mass = 0.0;
            for ( int c = 0; c < A; ++c )
            {
                w[static_cast<size_t>( c )] = 0.0;
                if ( mask[static_cast<size_t>( c )] == 0 ) continue;
                const double p = m.prob( c );
                mass += p;
                const double v = ( expo == 1.0 ? p : std::pow( p, expo ) ) * mask[static_cast<size_t>( c )];
                w[static_cast<size_t>( c )] = v;
                total += v;
            }
            if ( mass < MaskedMassFloor || total <= 0.0 )
            {
                ok = false;
                if ( stats != nullptr ) ++stats->m_retries;
                break;
            }
            double r = uniform() * total;
            int    chosen = -1;
            for ( int c = 0; c < A; ++c )
            {
                if ( w[static_cast<size_t>( c )] <= 0.0 ) continue;
                chosen = c;   // the last symbol with weight takes what rounding leaves over
                if ( r < w[static_cast<size_t>( c )] ) break;
                r -= w[static_cast<size_t>( c )];
            }
            out.push_back( chosen );
            m.observe( chosen );
        }
        if ( ok ) return true;
    }
    // Every attempt died: draw from the constraint weights alone, which is still inside the
    // constraints and still deterministic, and say so.
    out.clear();
    for ( int i = 0; i < n; ++i )
    {
        const std::vector<uint8_t> &mask = allowed[static_cast<size_t>( i )];
        double                      total = 0.0;
        for ( int c = 0; c < A; ++c ) total += mask[static_cast<size_t>( c )];
        double r = uniform() * total;
        int    chosen = -1;
        for ( int c = 0; c < A; ++c )
        {
            if ( mask[static_cast<size_t>( c )] == 0 ) continue;
            chosen = c;
            if ( r < mask[static_cast<size_t>( c )] ) break;
            r -= mask[static_cast<size_t>( c )];
        }
        out.push_back( chosen );
    }
    if ( stats != nullptr ) stats->m_fellBack = true;
    return true;
}

}   // namespace phos
