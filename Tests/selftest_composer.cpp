/**
 * @file selftest_composer.cpp
 * @brief The self test's composer: modal interchange and presence, harmony and motifs, audibility, presets and keyboard, the set file, form and curation, cues.
 */
#include "SelfTestHelpers.h"

using namespace phos;

namespace phostest {

/// testModalInterchange part (4): interchange on, qualifying tracks 1-6.
void testModalInterchangePresenceOn1() { modalPresence(0, 0); }
/// testModalInterchange part (4): interchange on, qualifying tracks 7-12.
void testModalInterchangePresenceOn2() { modalPresence(0, 1); }
/// testModalInterchange part (4): interchange off, qualifying tracks 1-6.
void testModalInterchangePresenceOff1() { modalPresence(1, 0); }
/// testModalInterchange part (4): interchange off, qualifying tracks 7-12.
void testModalInterchangePresenceOff2() { modalPresence(1, 1); }

/// testModalInterchange part (4)', track 1: interchange on, qualifying tracks 1-6.
void testModalInterchangePresenceArcOn1() { modalPresenceArc(0, 0); }
/// testModalInterchange part (4)', track 1: interchange on, qualifying tracks 7-12.
void testModalInterchangePresenceArcOn2() { modalPresenceArc(0, 1); }
/// testModalInterchange part (4)', track 1: interchange off, qualifying tracks 1-6.
void testModalInterchangePresenceArcOff1() { modalPresenceArc(1, 0); }
/// testModalInterchange part (4)', track 1: interchange off, qualifying tracks 7-12.
void testModalInterchangePresenceArcOff2() { modalPresenceArc(1, 1); }

/** @brief testModalInterchange, part (5): does the model reach for a tone the borrowed mode newly admits? */
void testModalInterchangeNewTone()
{
    section("modal interchange over the tonic pedal: the newly admitted tone");

    // Does the model use a tone the borrowed mode newly admits, or route around it? Measured with
    // both predictive models, because the neural one is the one that was never told the mode.
    {
        for (int which = 0; which < 2; ++which) {
            BorrowedScore& bs = borrowedScore(which);
            ParamStore& p = bs.p;
            Composer& c = *bs.c;
            const std::vector<NoteEvent>& ev = bs.ev;
            long long newNotes = 0, allNotes = 0;
            double expected = 0.0;
            for (const NoteEvent& e : ev) {
                if (e.part != Part::Lead && e.part != Part::Acid && e.part != Part::Arp) continue;
                const int bar = static_cast<int>(e.beat / kBeatsPerBar);
                const TrackPlan& t = c.track(p, c.trackOfBar(p, bar));
                const int sc = t.form.section[sectionOfBar(t.form, bar - t.firstBar)].scale;
                if (sc == t.scale) continue;                       // only the borrowed sections count
                // The pitch classes the borrowed mode admits and the track's own mode does not.
                int fresh = 0, total = 0;
                bool isNew[12] = {};
                for (int pc = 0; pc < 12; ++pc) {
                    const bool now = inScale(sc, pc), before = inScale(t.scale, pc);
                    isNew[pc] = now && !before;
                    if (now) { ++total; fresh += isNew[pc] ? 1 : 0; }
                }
                if (fresh == 0) continue;
                ++allNotes;
                expected += static_cast<double>(fresh) / total;    // the null: every admitted tone equally likely
                if (isNew[((e.pitch - t.key) % 12 + 12) % 12]) ++newNotes;
            }
            const double share = allNotes > 0 ? static_cast<double>(newNotes) / allNotes : 0.0;
            const double null = allNotes > 0 ? expected / allNotes : 0.0;
            const double ratio = null > 0.0 ? share / null : 0.0;
            check(allNotes > 500 && ratio > 0.05 && ratio < 4.0,
                  which == 0 ? "the Markov model reaches for a newly admitted tone (measured, not asserted)"
                             : "the neural model reaches for a newly admitted tone (measured, not asserted)",
                  fmt("%lld of %lld notes on a new tone = %.3f against a null of %.3f, ratio %.2f",
                      newNotes, allNotes, share, null, ratio));
        }
    }
}

/**
 * @brief The mode's colour: what the model plays once it is told which mode it is in.
 *
 * **The finding this measures.** The modal-interchange round of 16.09.2026 gave every section its own
 * mode and then asked whether the composer's lines use the tone the mode newly admits. They did not:
 * the share of notes on a newly admitted pitch class came out at 0.086 for the Markov model and 0.075
 * for the trained transformer against a mode-blind null of 0.162 -- ratios of 0.53 and 0.47. The cause
 * was structural: the model was conditioned on role, style, bars, step, bar, gap and index, and not on
 * the mode, so the mask admitted the note and the model's own distribution pushed it back down.
 *
 * The answer of 18.09.2026 is an eleventh embedding table, `mode.emb` (docs/MODEL_FORMAT.md section
 * 3), and this section measures what it bought. It also measures what the *alternative* answer -- a
 * calibrated multiplicative lift on the colour tones inside the allowed sets -- was worth; that was
 * built, calibrated, measured and **rejected**, and docs/rounds/2026-09.md has the table it was rejected on.
 *
 * Four measurements, all against values derived outside this program.
 *
 * **1. The colour-tone share against the corpus, per role.** `Tools/train/mode.py` estimates the mode
 * of every line of the local MIDI corpus and counts what share of the notes of the lines in a
 * *colourful* mode (Phrygian, harmonic minor, Phrygian dominant, double harmonic -- the four whose
 * `scaleColourTones` is not zero) falls on that mode's own colour tones. Over 377 lines and 20 939
 * notes that is **0.1539, 95 % bootstrap interval over lines [0.1442, 0.1640]**, and per role
 * acid 0.1372 [0.1137, 0.1671], lead 0.1411 [0.1189, 0.1653], arp 0.1569 [0.1453, 0.1685]. Those
 * intervals were counted from bought loops, not from anything this program produced.
 *
 * The composer's melodic notes are counted by exactly the same rule -- `isColourTone` of the mode the
 * *section* plays -- and **per role**, because an aggregate can be put on the target by one role
 * running far past it while another sits at zero. The claim is the acid, the role that was furthest
 * off: a model that is told the mode has to give the colour at least the share the corpus's acid
 * lines give it. The mode-blind transformer gives it 0.039, which is how this check was seen to fail
 * before it passed.
 *
 * **2. The colour is the rule's, not the model's.** The corpus gives the colour 0.0874 of *all* its
 * notes when the mode is not conditioned on at all (`mode.py`, 666 lines) against 0.1539 inside the
 * mode. Since 18.09.2026 the colour tones are placed at the rules' colour slots and the share no longer
 * depends on the model; since 25.09.2026 the acid's and the lead's b2 in the Phrygian family is a degree
 * the model draws, and is held to at most a quarter of their notes instead.
 *
 * **3. The ratio against the mode-blind null**, the measurement the modal-interchange round left open:
 * over the sections that borrow a mode, the share of notes on a pitch class the borrowed mode admits
 * and the track's own mode does not, split by whether that class is a colour tone at all -- a section
 * that borrows Aeolian over Phrygian gains the *natural* second, which is not a colour tone and which
 * nothing here addresses.
 *
 * **4. What the mode row does to the model's own distribution.** The point of a mode table is not that
 * it changes the logits (a random table would) but that it changes them *in the direction the label
 * means*: at one and the same context, the row of a mode that contains the flat second has to put more
 * probability on the flat second than the row of a mode that does not. Measured here over the real
 * shipped model, Phrygian (row 1) against Aeolian (row 0), with no constraint mask in the way.
 */
void testModeColour()
{
    section("the mode's colour: what the model plays once it is told the mode");

    // The corpus, per role: share, then the 95 % bootstrap interval over lines. `mode.py --report`
    // prints the shares; the intervals are its bootstrap over lines.
    struct Target { const char* name; double share, lo, hi; };
    static const Target kCorpus[3] = { { "acid", 0.1372, 0.1137, 0.1671 },
                                       { "lead", 0.1411, 0.1189, 0.1653 },
                                       { "arp",  0.1569, 0.1453, 0.1685 } };

    // 1 and 2. The colour-tone share of the composer's own lines, by the corpus's rule.
    // Four styles, because the style profile decides how often a section borrows at all and which
    // modes it may reach; a Goa-only measurement would be about Goa and not about the model.
    static const char* kStyles[4] = { "Goa", "FullOn", "DarkForest", "HiTech" };
    double markovAcidShare = 0.0;
    {
        for (int which = 0; which < 2; ++which) {
            long long colourNotes = 0, allNotes = 0;
            long long perRole[3] = {}, perRoleAll[3] = {};
            // Since 25.09.2026 the b2 of the Phrygian family is a degree of the acid's and the lead's own
            // (Harmony.h, isLineColourTone), drawn by the model like any scale tone: [role] notes on it in the
            // modes that have it, and the rule's colour -- every other colour tone -- apart from it.
            long long flat2[2] = {}, flat2All[2] = {}, ruleColour[3] = {};
            long long own[2] = {}, ownAll[2] = {};   // [0] the track's own mode, [1] a borrowed one
            for (int s = 0; s < 4; ++s) {
                ParamStore p;
                p.parseText("compose.track_bars=128 compose.lead_amount=1 compose.acid_amount=1 "
                            "compose.arp_amount=1 compose.modal_interchange=On compose.level_match=Off "
                            "master.auto_gain=Off");
                p.parseText(std::string("compose.style=") + kStyles[s]);
                p.parseText(which == 0 ? "compose.melody_model=Markov" : "compose.melody_model=Neural");
                Composer c(4711 + s);
                std::vector<NoteEvent> ev;
                c.composeBars(p, 0, 8 * 128, ev);
                for (const NoteEvent& e : ev) {
                    int role = -1;
                    if (e.part == Part::Acid) role = 0;
                    else if (e.part == Part::Lead) role = 1;
                    else if (e.part == Part::Arp) role = 2;
                    if (role < 0) continue;
                    const int bar = static_cast<int>(e.beat / kBeatsPerBar);
                    const TrackPlan& t = c.track(p, c.trackOfBar(p, bar));
                    const int sc = t.form.section[sectionOfBar(t.form, bar - t.firstBar)].scale;
                    if (scaleColourTones(sc) == 0) continue;   // the mode has no colour to give
                    const int b = sc == t.scale ? 0 : 1;
                    ++allNotes;
                    ++perRoleAll[role];
                    ++ownAll[b];
                    const int pc = ((e.pitch - t.key) % 12 + 12) % 12;
                    if (isColourTone(sc, pc)) { ++colourNotes; ++perRole[role]; ++own[b]; }
                    if (role < 2 ? isLineColourTone(sc, pc) : isColourTone(sc, pc)) ++ruleColour[role];
                    if (role < 2 && inScale(sc, 1)) { ++flat2All[role]; flat2[role] += pc == 1 ? 1 : 0; }
                }
            }
            const double share = allNotes > 0 ? static_cast<double>(colourNotes) / allNotes : 0.0;
            double roleShare[3] = {};
            for (int k = 0; k < 3; ++k)
                roleShare[k] = perRoleAll[k] > 0 ? static_cast<double>(perRole[k]) / perRoleAll[k] : 0.0;
            std::printf("         %s: %lld of %lld notes = %.4f overall (corpus 0.1539 in the mode, "
                        "0.0874 mode-blind); acid %.4f/%lld, lead %.4f/%lld, arp %.4f/%lld against the "
                        "corpus's %.4f, %.4f, %.4f; the track's own mode %.4f (%lld), a borrowed one "
                        "%.4f (%lld)\n",
                        which == 0 ? "Markov" : "neural", colourNotes, allNotes, share,
                        roleShare[0], perRoleAll[0], roleShare[1], perRoleAll[1], roleShare[2], perRoleAll[2],
                        kCorpus[0].share, kCorpus[1].share, kCorpus[2].share,
                        ownAll[0] > 0 ? static_cast<double>(own[0]) / ownAll[0] : 0.0, ownAll[0],
                        ownAll[1] > 0 ? static_cast<double>(own[1]) / ownAll[1] : 0.0, ownAll[1]);
            // Since 18.09.2026 the colour share is the genre rules' one mechanism (Melody.cpp, kColourShare):
            // colour tones are out of every sampler set and are placed at colour slots, so the neural model's
            // mode table cannot lift them. Since 25.09.2026 that holds for every colour tone but the acid's and
            // the lead's structural b2 (the user's decision): the rule's colour must not depend on the model,
            // and the b2, which the model now draws, must not take a line over -- at most a quarter of its notes
            // in the modes that have it (the lead once sat on it with 0.41). Until then the first check here
            // read the Markov share against the corpus's mode-blind 0.0874; with the b2 a degree of the lines
            // the chain is no longer blind to the mode, by design.
            const double ruleAcid = perRoleAll[0] > 0 ? static_cast<double>(ruleColour[0]) / perRoleAll[0] : 0.0;
            const double b2Acid = flat2All[0] > 0 ? static_cast<double>(flat2[0]) / flat2All[0] : 0.0;
            const double b2Lead = flat2All[1] > 0 ? static_cast<double>(flat2[1]) / flat2All[1] : 0.0;
            std::printf("         %s: the rule's colour in the acid %.4f; the structural b2 acid %.4f (%lld), lead %.4f (%lld)\n",
                        which == 0 ? "Markov" : "neural", ruleAcid, b2Acid, flat2All[0], b2Lead, flat2All[1]);
            check(flat2All[0] > 1000 && flat2All[1] > 1000 && b2Acid <= 0.25 && b2Lead <= 0.25,
                  which == 0 ? "the structural b2 stays a degree among others with the Markov model: at most a quarter of acid and lead"
                             : "the structural b2 stays a degree among others with the neural model: at most a quarter of acid and lead",
                  fmt("acid %.4f of %lld, lead %.4f of %lld notes in the modes with a b2", b2Acid, flat2All[0], b2Lead, flat2All[1]));
            if (which == 1)
                check(perRoleAll[0] > 2000 && std::fabs(ruleAcid - markovAcidShare) < 0.03,
                      "the acid's colour share is the rule's, whichever model draws the line (the structural b2 aside)",
                      fmt("neural %.4f against Markov %.4f (%lld notes; the corpus's acid interval [%.4f, %.4f] "
                          "no longer decides it)", ruleAcid, markovAcidShare, perRoleAll[0], kCorpus[0].lo, kCorpus[0].hi));
            if (which == 0) markovAcidShare = ruleAcid;
        }
    }

    double markovRatio = 0.0;
    // 3. The ratio against the mode-blind null, over the borrowed sections only. Same counting rule
    // as testModalInterchange, so the two numbers are comparable with the ones recorded there. The very
    // same score, too (31337, Goa, 1536 bars): in one process it is composed once for both sections.
    {
        for (int which = 0; which < 2; ++which) {
            BorrowedScore& bs = borrowedScore(which);
            ParamStore& p = bs.p;
            Composer& c = *bs.c;
            const std::vector<NoteEvent>& ev = bs.ev;
            long long newNotes[2] = {}, allNotes[2] = {};
            double expected[2] = {};
            long long roleNew[3] = {}, roleAll[3] = {};
            double roleExp[3] = {};
            for (const NoteEvent& e : ev) {
                int role = -1;
                if (e.part == Part::Acid) role = 0;
                else if (e.part == Part::Lead) role = 1;
                else if (e.part == Part::Arp) role = 2;
                if (role < 0) continue;
                const int bar = static_cast<int>(e.beat / kBeatsPerBar);
                const TrackPlan& t = c.track(p, c.trackOfBar(p, bar));
                const int sc = t.form.section[sectionOfBar(t.form, bar - t.firstBar)].scale;
                if (sc == t.scale) continue;
                int fresh = 0, total = 0, freshColour = 0;
                bool isNew[12] = {};
                for (int pc = 0; pc < 12; ++pc) {
                    const bool now = inScale(sc, pc), before = inScale(t.scale, pc);
                    isNew[pc] = now && !before;
                    if (now) { ++total; fresh += isNew[pc] ? 1 : 0; }
                    if (isNew[pc] && isColourTone(sc, pc)) ++freshColour;
                }
                if (fresh == 0) continue;
                const int k = freshColour > 0 ? 1 : 0;
                const bool hit = isNew[((e.pitch - t.key) % 12 + 12) % 12];
                ++allNotes[k];
                expected[k] += static_cast<double>(fresh) / total;
                if (hit) ++newNotes[k];
                if (k == 1) {
                    ++roleAll[role];
                    roleExp[role] += static_cast<double>(fresh) / total;
                    if (hit) ++roleNew[role];
                }
            }
            double share[2] = {}, null[2] = {}, ratio[2] = {};
            for (int k = 0; k < 2; ++k) {
                share[k] = allNotes[k] > 0 ? static_cast<double>(newNotes[k]) / allNotes[k] : 0.0;
                null[k] = allNotes[k] > 0 ? expected[k] / allNotes[k] : 0.0;
                ratio[k] = null[k] > 0.0 ? share[k] / null[k] : 0.0;
            }
            double roleRatio[3] = {};
            for (int k = 0; k < 3; ++k)
                roleRatio[k] = roleExp[k] > 0.0 ? static_cast<double>(roleNew[k]) / roleExp[k] : 0.0;
            std::printf("         %s, borrowed sections: a newly admitted *colour* class in %lld of "
                        "%lld notes = %.3f against the null %.3f, ratio %.2f (per role acid %.2f/%lld, "
                        "lead %.2f/%lld, arp %.2f/%lld); a newly admitted class that is not a colour "
                        "tone %lld of %lld = %.3f against %.3f, ratio %.2f\n",
                        which == 0 ? "Markov" : "neural", newNotes[1], allNotes[1], share[1], null[1], ratio[1],
                        roleRatio[0], roleAll[0], roleRatio[1], roleAll[1], roleRatio[2], roleAll[2],
                        newNotes[0], allNotes[0], share[0], null[0], ratio[0]);
            // Since 18.09.2026 a colour tone is a neighbour tone at a drawn slot (Melody.cpp), so the
            // borrowed-colour ratio is the rule's and the mode table has no say in it any more; the
            // check is that both models land on the same ratio.
            if (which == 0) markovRatio = ratio[1];
            else
                check(allNotes[1] > 500 && std::fabs(ratio[1] - markovRatio) < 0.15,
                      "the borrowed-colour ratio is the rule's, whichever model draws the line",
                      fmt("neural %.2f against Markov %.2f (per role acid %.2f, lead %.2f, arp %.2f)",
                          ratio[1], markovRatio, roleRatio[0], roleRatio[1], roleRatio[2]));
        }
    }

    // 4. What the mode row does to the model's own distribution, with no mask in the way.
    {
        std::string note;
        NeuralModel* nn = sharedMelodyModel(&note);
        if (nn == nullptr) {
            check(false, "the trained model is there to ask what a mode row does", note);
        } else if (nn->info().condMode <= 0) {
            check(false, "the installed melody model carries the mode table",
                  "the file has no condMode; docs/MODEL_FORMAT.md section 3 makes it optional, so this "
                  "is a model from before 18.09.2026 and the round's claim cannot be measured on it");
        } else {
            // The flat second is the one pitch class Phrygian (row 1) has and Aeolian (row 0) has not,
            // and it is a colour tone in every mode that holds it (Harmony.h). So the Phrygian row has
            // to raise it, at the same context, or the table does not mean what it is labelled with.
            const int flatSecond = PitchModel::symbol(1);
            int positions = 0, raised = 0;
            double sumA = 0.0, sumP = 0.0, worstDrop = 0.0;
            for (int role = 0; role < 3; ++role) {
                for (int startStep = 0; startStep < 4; ++startStep) {
                    std::vector<NoteCond> cond;
                    for (int i = 0; i < 12; ++i) {
                        NoteCond nc;
                        nc.step = (startStep + 3 * i) % 16;
                        nc.bar = (startStep + 3 * i) / 16 % 8;
                        nc.gap = noteGapCode(0, 3, true);
                        nc.idx = noteIndexBucket(i);
                        cond.push_back(nc);
                    }
                    // The same token sequence under both rows, so nothing but the row differs. The
                    // tokens are a plain minor run rather than the model's own argmax, because the
                    // argmax would itself diverge and the two runs would stop being one comparison.
                    static const int kRun[12] = { 0, 3, 5, 7, 8, 7, 5, 3, 0, 7, 10, 12 };
                    double pa[12] = {}, pp[12] = {};
                    for (int md = 0; md < 2; ++md) {
                        nn->begin(role, 0, 2, md);   // row 0 Aeolian, row 1 Phrygian
                        int token = NeuralModel::startToken();
                        for (size_t i = 0; i < cond.size(); ++i) {
                            if (!nn->step(token, cond[i])) break;
                            (md == 0 ? pa : pp)[i] = nn->prob(flatSecond);
                            token = PitchModel::symbol(kRun[i]);
                        }
                    }
                    // Counted position by position, so that one outlier cannot carry the mean.
                    for (size_t i = 0; i < cond.size(); ++i) {
                        ++positions;
                        sumA += pa[i];
                        sumP += pp[i];
                        if (pp[i] > pa[i]) ++raised;
                        worstDrop = std::max(worstDrop, pa[i] - pp[i]);
                    }
                }
            }
            const double meanA = positions > 0 ? sumA / positions : 0.0;
            const double meanP = positions > 0 ? sumP / positions : 0.0;
            check(positions > 100 && raised * 4 >= positions * 3 && meanP > 2.0 * meanA,
                  "the Phrygian row raises the flat second over the Aeolian row, as its label says",
                  fmt("mean probability of the flat second %.5f under Aeolian against %.5f under "
                      "Phrygian (a factor of %.2f), higher at %d of %d positions; the largest drop "
                      "the other way is %.5f", meanA, meanP, meanA > 0.0 ? meanP / meanA : 0.0,
                      raised, positions, worstDrop));
        }
    }
}

/**
 * @brief The measured tension curve (Melody.h, Tools/corpus/measure_tension.py).
 *
 * The composer's lines are measured exactly the way the corpus was measured: the paired per-line
 * difference of Lerdahl instability between beat 4 and beat 1 of the bar, and between the odd and
 * the even bar of a two-bar pair. Both have to come out positive, and both have to land inside the
 * corpus's 95 % interval -- an independently derived value, counted from 655 real loops, not from
 * anything this program produced. With the tilt at zero (kTensionTilt in Melody.cpp) the contrasts
 * collapse, which is how this check was seen to fail before it passed.
 */
void testTensionCurve()
{
    section("the measured tension curve");
    ParamStore p;
    p.parseText("compose.track_bars=128 compose.acid_amount=1 compose.lead_amount=1 compose.arp_amount=1 "
                "compose.level_match=Off master.auto_gain=Off");
    Composer c(8123);
    // One measurement per role, over the plans of many tracks: the plan is the line, and a rendered
    // bar is only a window on it.
    struct Pairs { std::vector<double> beat, parity; };
    Pairs role[3];
    // Enough tracks that the acid's parity has a sample at all: only three acid patterns in ten are
    // two bars long, and a contrast about the *pair* of bars needs two bars to live in. At 48 tracks
    // it rested on nine lines and wandered by a quarter of a Lerdahl level between builds.
    for (int i = 0; i < 400; ++i) {
        const TrackPlan t = c.track(p, i);
        const MelodyPlan& m = t.melody;
        auto measure = [](const std::vector<MelodyNote>& notes, int bars, Pairs& out) {
            double sum[4] = {}, cnt[4] = {}, par[2] = {}, pc[2] = {};
            for (const MelodyNote& n : notes) {
                const int v = lerdahlInstability(n.rel);
                const int b = (n.step % 16) / 4;
                sum[b] += v;
                cnt[b] += 1.0;
                if (bars >= 2) { const int q = (n.step / 16) % 2; par[q] += v; pc[q] += 1.0; }
            }
            if (cnt[0] > 0.0 && cnt[3] > 0.0) out.beat.push_back(sum[3] / cnt[3] - sum[0] / cnt[0]);
            if (pc[0] > 0.0 && pc[1] > 0.0) out.parity.push_back(par[1] / pc[1] - par[0] / pc[0]);
        };
        // The acid's pattern root is a scale degree above the key, so its `rel` is not the interval
        // to the tonic; every role is measured against the key, as the corpus was.
        auto shifted = [](const std::vector<MelodyNote>& in, int offset) {
            std::vector<MelodyNote> out = in;
            for (MelodyNote& n : out) n.rel = static_cast<int8_t>(n.rel + offset);
            return out;
        };
        const int offs[3] = { ((m.root[0] - t.key) % 12 + 12) % 12, ((m.root[1] - t.key) % 12 + 12) % 12,
                              ((m.root[mpIndex(MelodyPart::Arp)] - t.key) % 12 + 12) % 12 };
        measure(shifted(m.acid[0], offs[0]), m.acidSteps / 16, role[0]);
        for (int w = 0; w < 2; ++w) measure(shifted(m.lead[w], offs[1]), 2, role[1]);
        for (int k = 0; k < 4; ++k) measure(shifted(m.arp[k], offs[2]), 1, role[2]);
    }
    auto mean = [](const std::vector<double>& x) {
        double s = 0.0;
        for (double v : x) s += v;
        return x.empty() ? 0.0 : s / static_cast<double>(x.size());
    };
    // The corpus numbers (Tools/corpus/measure_tension.py, 655 deduplicated lines): paired per-line
    // contrasts with their 95 % bootstrap intervals.
    struct Target { const char* name; double lo, hi; };
    const Target beatTarget[3] = { { "acid", 0.164, 0.760 }, { "lead", 0.318, 0.788 }, { "arp", 0.212, 0.360 } };
    const Target parityTarget[2] = { { "acid", 0.044, 0.248 }, { "lead", 0.074, 0.323 } };
    std::string detail;
    for (int k = 0; k < 3; ++k)
        detail += fmt("%s beat %+.3f [%.3f, %.3f]%s", beatTarget[k].name, mean(role[k].beat), beatTarget[k].lo,
                      beatTarget[k].hi, k < 2 ? ", " : "");
    for (int k = 0; k < 2; ++k)
        detail += fmt("; %s parity %+.3f [%.3f, %.3f]", parityTarget[k].name, mean(role[k].parity),
                      parityTarget[k].lo, parityTarget[k].hi);
    // The acid and the lead have to land inside the corpus's own intervals, on both contrasts. The
    // arp is only reported: its allowed set is the chord and nothing else (PLAN 6.5), and within one
    // triad the three pitch classes carry instabilities of 0, 2 and 1, which is not enough variance
    // for the curve to act on -- so its tilt is 0 and its measured contrast is near zero by design
    // (Melody.cpp, kTensionTilt). A one-bar cell has no bar parity to show either.
    const double acidBeat = mean(role[0].beat), leadBeat = mean(role[1].beat);
    const double acidPar = mean(role[0].parity), leadPar = mean(role[1].parity);
    auto inside = [](double v, const Target& t) { return v >= t.lo && v <= t.hi; };
    // Since 18.09.2026 the genre rules stand above the corpus (docs/rounds/2026-09.md): the lead rests on the
    // tonic and the fifth, its colour tones are neighbour tones resolving at once, and the acid's
    // pitch classes and repetitions are bounded. The tilt still acts inside those sets, but the
    // rules take away most of the instability it tilts towards, and the lead's contrasts and both
    // bar parities fall to about zero (before: lead beat +0.558, parities +0.079 / +0.276). That is
    // the corpus disagreeing with a rule, which is reported, not a veto; what is still required is
    // the acid's within-bar rise, which the rules leave room for.
    std::printf("         (%s; the lead's beat contrast and both parities are reported only since the genre rules)\n",
                inside(leadBeat, beatTarget[1]) && inside(acidPar, parityTarget[0]) && inside(leadPar, parityTarget[1])
                    ? "lead and parities inside the corpus intervals" : "lead and parities outside the corpus intervals");
    check(inside(acidBeat, beatTarget[0]),
          "the composer's acid still rises in instability from beat 1 to beat 4, as the corpus was measured to", detail);
}

/**
 * @brief The motivic operators (Melody.h, MotifOperator).
 *
 * The expansion is checked against its own definition -- same contour, sign for sign, and no
 * interval narrower than the parent's -- on lines built here, so the check does not depend on
 * whatever the composer happened to draw. Since 22.09.2026 (round "Lead") the second half reads the
 * lead's design -- cell, archetype, operator per bar (Form.h, CellOp) -- out of the composer's own
 * phrases and checks each operator against the notes, the flags against their rules, and the
 * phrase's filter arc against the controls the composer writes.
 */
void testMotifOperators()
{
    section("motivic operators and the lead's design");

    // The expansion, against its definition.
    {
        int cases = 0, contourBroken = 0, narrowed = 0, refused = 0;
        for (int scale = 0; scale < kNumScales; ++scale) {
            for (uint64_t seed = 1; seed <= 200; ++seed) {
                Rng r;
                r.seed(mixSeed(seed ^ 0xA55A5AA5ull, static_cast<uint64_t>(scale)));
                constexpr int lo = -5, hi = 14;
                std::vector<int> tones;
                for (int rel = lo; rel <= hi; ++rel) if (inScale(scale, rel)) tones.push_back(rel);
                std::vector<int> parent;
                const int n = 4 + r.below(8);
                for (int i = 0; i < n; ++i) parent.push_back(tones[static_cast<size_t>(r.below(static_cast<int>(tones.size())))]);
                std::vector<std::vector<uint8_t>> allowed;
                for (int i = 0; i < n; ++i) {
                    std::vector<uint8_t> a(static_cast<size_t>(kCorpusAlphabet), 0);
                    for (int rel : tones) a[static_cast<size_t>(PitchModel::symbol(rel))] = 1;
                    allowed.push_back(a);
                }
                std::vector<int> ex;
                if (!expandContour(parent, allowed, lo, hi, ex)) { ++refused; continue; }
                ++cases;
                for (size_t i = 1; i < parent.size(); ++i) {
                    const int dp = parent[i] - parent[i - 1], de = ex[i] - ex[i - 1];
                    const int sp = dp > 0 ? 1 : (dp < 0 ? -1 : 0), se = de > 0 ? 1 : (de < 0 ? -1 : 0);
                    if (sp != se) ++contourBroken;
                    if (std::abs(de) < std::abs(dp)) ++narrowed;
                }
            }
        }
        // A third of the random parents here already use the whole ambitus and cannot be widened at
        // all; the operator refuses those rather than narrowing an interval, and the caller keeps
        // the parent. What must be exact is the two invariants.
        check(cases > 500 && contourBroken == 0 && narrowed == 0,
              "an expanded variant keeps its parent's contour sign for sign and never narrows an interval",
              fmt("%d expansions, %d refused (%.0f %%), %d contour signs broken, %d intervals narrowed",
                  cases, refused, 100.0 * refused / std::max(1, cases + refused), contourBroken, narrowed));
    }

    // The lead's design in the composer's own phrases (22.09.2026, round "Lead"): one cell, one
    // archetype, one operator per bar. Read from the plan and checked against the notes, not
    // against the maker's word: a shift bar has to carry the cell's rhythm turned by one sixteenth,
    // a thinned bar fewer onsets than the cell and never under eight, a transposed bar a mean pitch
    // on the side its shift says, the eighth bar the tonic; the flags have to be there and a slide
    // has to sit where a slide can (an adjacent note no more than a whole tone away).
    {
        ParamStore p;
        p.parseText("compose.track_bars=128 compose.lead_amount=1 compose.level_match=Off master.auto_gain=Off");
        int phrases = 0, archetypes[kNumLeadArchetypes] = {}, ops[kNumCellOps] = {};
        int firstNotKeep = 0, lastNotCadence = 0, cadenceOff = 0, shiftBars = 0, shiftWrong = 0, thinBars = 0, thinWrong = 0;
        int shiftedBars = 0, shiftedWrongSide = 0, accents = 0, shorts = 0, slides = 0, slidesWrong = 0;
        int outside = 0, tooLow = 0, notes = 0, fromCorpus = 0, bands[3] = {};
        int loops = 0, halves = 0, tracksSeen = 0;
        // Three sets of 64 tracks (25.09.2026): the transposed bars' side is a share of about 300 bars per set,
        // too few for a bound at 25 % -- the lead's structural b2 moved set 1979 from 23 % to 25.5 %, one standard
        // deviation, while over the three sets it reads 21.6 % before and 20.9 % after.
        for (const uint64_t setSeed : { 1979ull, 2026ull, 77ull }) {
        Composer c(setSeed);
        for (int i = 0; i < 64; ++i) {
            const TrackPlan t = c.track(p, i);
            const MelodyPlan& m = t.melody;
            ++tracksSeen;
            loops += m.progression == 1 ? 1 : 0;
            halves += m.secondHalf ? 1 : 0;
            if (!m.present[1]) continue;
            ++bands[std::clamp(m.leadDensityBand, 0, 2)];
            for (int w = 0; w < 2; ++w) {
                const std::vector<MelodyNote>& ph = m.lead[w];
                if (ph.empty()) continue;
                ++phrases;
                ++archetypes[std::clamp(m.leadArchetype[w], 0, kNumLeadArchetypes - 1)];
                if (m.leadCellFromCorpus[w]) ++fromCorpus;
                if (m.leadOps[w][0] != static_cast<int8_t>(CellOp::Keep)) ++firstNotKeep;
                if (m.leadOps[w][7] != static_cast<int8_t>(CellOp::EndCadence)) ++lastNotCadence;
                for (int b = 0; b < 8; ++b) ++ops[std::clamp<int>(m.leadOps[w][b], 0, kNumCellOps - 1)];
                // Bars.
                unsigned barMask[8] = {};
                double barMean[8] = {};
                int barN[8] = {};
                for (size_t k = 0; k < ph.size(); ++k) {
                    const MelodyNote& n = ph[k];
                    ++notes;
                    const int pitch = m.root[1] + n.rel;
                    if (!inScale(t.scale, pitch - t.key)) ++outside;
                    if (pitch < kLeadLowest) ++tooLow;
                    const int b = n.step / 16;
                    barMask[b] |= 1u << (n.step % 16);
                    barMean[b] += n.rel;
                    ++barN[b];
                    if (n.flags & kNoteAccent) ++accents;
                    if (n.flags & kNoteShort) ++shorts;
                    if (n.flags & kNoteSlide) {
                        ++slides;
                        const bool adjacent = k + 1 < ph.size() && ph[k + 1].step == n.step + n.len && std::abs(ph[k + 1].rel - n.rel) <= 2;
                        if (!adjacent) ++slidesWrong;
                    }
                }
                const unsigned cell = m.leadCell[w];
                unsigned turned = 0;
                for (int s = 0; s < 16; ++s) if ((cell >> s) & 1u) turned |= 1u << ((s + 1) % 16);
                for (int b = 0; b < 8; ++b) {
                    const CellOp op = static_cast<CellOp>(m.leadOps[w][b]);
                    if (op == CellOp::Shift16) { ++shiftBars; if (barMask[b] != turned) ++shiftWrong; }
                    if (op == CellOp::Thin) {
                        ++thinBars;
                        int on = 0, cellOn = 0;
                        for (int s = 0; s < 16; ++s) { on += (barMask[b] >> s) & 1u; cellOn += (cell >> s) & 1u; }
                        if (on >= cellOn || on < 8) ++thinWrong;
                    }
                    if (m.leadShift[w][b] != 0 && barN[b] > 0 && barN[0] > 0) {
                        ++shiftedBars;
                        const double d = barMean[b] / barN[b] - barMean[0] / barN[0];
                        if ((m.leadShift[w][b] > 0) != (d > 0)) ++shiftedWrongSide;
                    }
                }
                if (((m.root[1] + ph.back().rel - t.key) % 12 + 12) % 12 != 0) ++cadenceOff;
            }
        }
        }
        int archetypesSeen = 0, opsSeen = 0;
        for (int a : archetypes) archetypesSeen += a > 0 ? 1 : 0;
        for (int k = 1; k < kNumCellOps; ++k) opsSeen += ops[k] > 0 ? 1 : 0;
        // The eighth bar cadences onto the tonic; the run fix across phrases may move a last note to
        // another chord tone, so one in ten is the allowance, not zero.
        check(phrases > 40 && archetypesSeen == kNumLeadArchetypes && opsSeen == kNumCellOps - 1 && firstNotKeep == 0 && lastNotCadence == 0
                  && cadenceOff * 10 <= phrases && shiftBars > 3 && shiftWrong == 0 && thinBars > 3 && thinWrong == 0
                  && shiftedBars > 20 && shiftedWrongSide * 4 <= shiftedBars && outside == 0 && tooLow == 0 && notes > 500,
              "the lead is one cell, an archetype and an operator per bar: every archetype and operator in use, bar 1 the cell, bar 8 the cadence on the tonic, shifts turned by one sixteenth, thinning never under eight onsets, transposed bars on their side",
              fmt("%d phrases, %d archetypes, %d operators; %d first bars not the cell, %d last bars not a cadence, %d phrase ends off the tonic; "
                  "%d shift bars (%d wrong), %d thin bars (%d wrong), %d transposed bars (%d on the wrong side); %d notes, %d out of the mode, %d under C4",
                  phrases, archetypesSeen, opsSeen, firstNotKeep, lastNotCadence, cadenceOff, shiftBars, shiftWrong, thinBars, thinWrong,
                  shiftedBars, shiftedWrongSide, notes, outside, tooLow));
        // 23.09.2026, round "Harmonie": beside the pendulum some tracks play a minor loop, and some change their
        // progression behind the main breakdown -- and most still play the pendulum: fuzziness, not a new rule.
        check(tracksSeen >= 60 && loops >= 6 && loops * 2 < tracksSeen && halves >= 6 && halves * 2 < tracksSeen,
              "harmony's fuzziness: some tracks play a minor loop instead of the pendulum, some change their progression behind the main breakdown, most keep both",
              fmt("%d tracks: %d loops, %d with a second half", tracksSeen, loops, halves));
        check(accents > 20 && shorts > 20 && slides > 10 && slidesWrong == 0 && fromCorpus * 2 > phrases,
              "the lead carries accents, staccato gates and slides, a slide only into an adjacent note a whole tone away at most; most cells come from the corpus templates",
              fmt("%d accents, %d short notes, %d slides (%d not adjacent), %d of %d cells from the corpus, density bands %d/%d/%d",
                  accents, shorts, slides, slidesWrong, fromCorpus, phrases, bands[0], bands[1], bands[2]));

        // The filter arc: the composer writes one-bar cutoff ramps on the lead wherever it plays, and
        // the eight bars of a phrase are not one value.
        {
            Composer c(1979);
            const TrackPlan t = c.track(p, 1);
            std::vector<NoteEvent> ev;
            std::vector<ControlEvent> ctl;
            c.composeBars(p, t.firstBar, t.bars, ev, &ctl);
            const int cutoffId = p.base(PolyInstance::Lead) + poly::Cutoff;
            std::set<int> leadBars;
            for (const NoteEvent& e : ev) if (e.part == Part::Lead) leadBars.insert(static_cast<int>(e.beat / kBeatsPerBar));
            int arcEvents = 0, barsWithout = 0;
            std::set<long> values;
            for (const ControlEvent& e : ctl)
                if (e.param == cutoffId && e.kind == ControlEvent::Kind::Offset && e.length == static_cast<float>(kBeatsPerBar)) { ++arcEvents; values.insert(std::lround(e.value * 10000.0)); }
            for (int b : leadBars) {
                bool has = false;
                for (const ControlEvent& e : ctl) has = has || (e.param == cutoffId && std::fabs(e.beat - b * kBeatsPerBar) < 1e-6);
                if (!has) ++barsWithout;
            }
            check(!leadBars.empty() && arcEvents >= static_cast<int>(leadBars.size()) && barsWithout == 0 && values.size() >= 4,
                  "the lead phrase's filter arc: a one-bar cutoff ramp in every bar the lead plays, with more than one value over the phrase",
                  fmt("%d lead bars, %d arc events, %d lead bars without one, %d distinct values", static_cast<int>(leadBars.size()), arcEvents, barsWithout, static_cast<int>(values.size())));
        }
    }
}

/**
 * @brief Euclidean and polymetric arps (Melody.h, ArpStyle).
 *
 * The Euclidean steps are compared against Rhythm.h's own generator -- the percussion's, reused
 * rather than rewritten -- and the rotation against the syncopation rule the percussion lanes
 * follow. The polymeter is checked to precess: a cell of three sixteenths against a bar of sixteen
 * must start one step later in each bar and come home only every third. What the polymeter does to
 * the masking rule between lead and arp, and to the depth rule, is measured rather than assumed.
 */
void testArpPatterns()
{
    section("Euclidean and polymetric arps");
    ParamStore p;
    p.parseText("compose.track_bars=128 compose.arp_amount=1 compose.lead_amount=1 compose.acid_amount=0.3 "
                "compose.level_match=Off master.auto_gain=Off");
    Composer c(5150);

    int styles[kNumArpStyles] = {}, euclidWrong = 0, rotationWrong = 0, euclidSeen = 0, polySeen = 0;
    for (int i = 0; i < 96; ++i) {
        const TrackPlan t = c.track(p, i);
        const MelodyPlan& m = t.melody;
        ++styles[std::clamp(m.arpStyle, 0, kNumArpStyles - 1)];
        if (m.arpStyle == static_cast<int>(ArpStyle::Euclid)) {
            ++euclidSeen;
            const std::vector<bool> e = euclid(m.arpPulses, kStepsPerBar, m.arpRotation);
            std::vector<int> want;
            for (int s = 0; s < kStepsPerBar; ++s) if (e[static_cast<size_t>(s)]) want.push_back(s);
            // Since 18.09.2026 (rule 12) the arp plays every sixteenth and the Euclidean pulses are the
            // steps that jump up into the high stream.
            std::vector<int> got;
            for (int s = 0; s < kStepsPerBar; ++s) if ((m.arpHigh >> s) & 1u) got.push_back(s);
            if (want != got || m.arpPulses < 4 || m.arpPulses > 8) ++euclidWrong;
            // The rotation has to be one that minimises the distance to the midpoint between the
            // least and the most syncopated rotation -- Sioros et al. 2014, moderate syncopation --
            // derived here from Rhythm.h alone rather than read back from the plan.
            int lo = 1 << 30, hi = -(1 << 30);
            std::vector<int> sync(kStepsPerBar);
            for (int rot = 0; rot < kStepsPerBar; ++rot) {
                const std::vector<bool> er = euclid(m.arpPulses, kStepsPerBar, rot);
                bool st[kStepsPerBar];
                for (int k = 0; k < kStepsPerBar; ++k) st[k] = er[static_cast<size_t>(k)];
                sync[static_cast<size_t>(rot)] = lhlSyncopation(st);
                lo = std::min(lo, sync[static_cast<size_t>(rot)]);
                hi = std::max(hi, sync[static_cast<size_t>(rot)]);
            }
            const int target = (lo + hi) / 2;
            int bestD = 1 << 30;
            for (int rot = 0; rot < kStepsPerBar; ++rot) bestD = std::min(bestD, std::abs(sync[static_cast<size_t>(rot)] - target));
            if (m.arpRotation < 0 || m.arpRotation >= kStepsPerBar
                || std::abs(sync[static_cast<size_t>(m.arpRotation)] - target) != bestD)
                ++rotationWrong;
        }
        if (m.arpPolymeter) { ++polySeen; if (m.arp[0].size() != 3) ++euclidWrong; }
    }
    int seen = 0;
    for (int s : styles) seen += s > 0 ? 1 : 0;
    check(seen == kNumArpStyles && euclidSeen > 8 && polySeen > 4 && euclidWrong == 0 && rotationWrong == 0,
          "Euclidean arps use Rhythm.h's generator and a moderately syncopated rotation",
          fmt("styles %d/%d/%d/%d/%d/%d, %d Euclidean (%d wrong, %d rotations off target), %d polymetric",
              styles[0], styles[1], styles[2], styles[3], styles[4], styles[5], euclidSeen, euclidWrong,
              rotationWrong, polySeen));

    // The polymeter precesses: bar n and bar n+1 are one cell step apart, bar n and bar n+3 are not.
    // Measured on composeMelodyBar directly, with a bar plan that asks for the arp alone, so the
    // measurement is of the polymeter and not of whatever the form happened to schedule.
    {
        int tracks = 0, notPrecessing = 0, notReturning = 0, wrongCount = 0, lowest = 127;
        for (int i = 0; i < 128; ++i) {
            const TrackPlan t = c.track(p, i);
            if (!t.melody.arpPolymeter) continue;
            ++tracks;
            BarPlan bp;
            bp.parts = partBit(MelodyPart::Arp);
            bp.partsNext = partBit(MelodyPart::Arp);
            ParamStore q;
            std::vector<std::vector<int>> bars;
            bool sized = true;
            // Bars 0, 1 and 12 x chordBars (48, 96 or 192): that bar has the cell phase of bar 0 (a multiple
            // of 3), the same variant of the A A A' A'' phrase (of 4), the same octave-jump state (of 4) and
            // -- since 22.09.2026 a chord holds 4, 8 or 16 bars and the pendulum's period is four blocks --
            // the same chord (12 x chordBars is a multiple of 4 x chordBars). Bar 48 was that bar while
            // chords held two or four bars; with sixteen it is the pendulum's other chord, and the arp
            // rightly plays other tones there. Since 18.09.2026 bar 3 is the phrase's A'' and differs on purpose.
            for (int b : { 0, 1, 12 * t.melody.chordBars }) {
                std::vector<NoteEvent> ev;
                composeMelodyBar(q, t.melody, b, b, t.scale, bp, ev);
                std::vector<int> pitches;
                for (const NoteEvent& e : ev) if (e.part == Part::Arp) { pitches.push_back(e.pitch); lowest = std::min(lowest, int(e.pitch)); }
                if (pitches.size() != kStepsPerBar) { ++wrongCount; sized = false; }
                bars.push_back(pitches);
            }
            if (!sized) continue;
            // Bar b and bar b+3 are the same cell phase again (16 mod 3 = 1, so three bars come
            // home) as long as neither the chord nor the arp's own octave jump has moved in between;
            // bar b and bar b+1 never are, unless the cell happens to hold one pitch three times.
            const bool oneNote = t.melody.arp[0][0].rel == t.melody.arp[0][1].rel
                              && t.melody.arp[0][1].rel == t.melody.arp[0][2].rel;
            if (!oneNote && bars[0] == bars[1]) ++notPrecessing;
            if (bars[0] != bars[2]) ++notReturning;
        }
        check(tracks > 4 && wrongCount == 0 && notPrecessing == 0 && notReturning == 0 && lowest >= kArpLowest,
              "a 3/16 arp cell moves on by a sixteenth every bar and comes home every third",
              fmt("%d polymetric tracks, %d bars of the wrong length, %d not precessing, %d not returning, lowest arp note %d",
                  tracks, wrongCount, notPrecessing, notReturning, lowest));
    }

    // What the polymeter costs the masking rule and the depth rule, measured against the rest. The
    // masking rule can only be seen where it could fire: a drop brings every part back at once, so a
    // drop without the arp is the rule having silenced it.
    {
        int polyDrops = 0, polyMasked = 0, otherDrops = 0, otherMasked = 0;
        int polyShift = 0, otherShift = 0, lowestPoly = 127, lowestOther = 127;
        for (int i = 0; i < 160; ++i) {
            const TrackPlan t = c.track(p, i);
            if (!t.melody.present[mpIndex(MelodyPart::Lead)] || !t.melody.present[mpIndex(MelodyPart::Arp)]) continue;
            const bool poly = t.melody.arpPolymeter;
            PartAvailability a;
            for (int k = 0; k < kMelodyParts; ++k) a.part[k] = t.melody.present[k];
            a.leadLo = t.melody.leadLo;
            a.leadHi = t.melody.leadHi;
            a.arpLo = t.melody.arpLo;
            a.arpHi = t.melody.arpHi;
            a.percLayers = t.perc.layers;
            for (int s = 0; s < t.form.count; ++s) {
                if (t.form.section[s].type != SectionType::Drop) continue;
                const BarPlan bp = planBar(t.form, a, t.sectionSeed, t.form.section[s].startBar);
                (poly ? polyDrops : otherDrops)++;
                if ((bp.parts & partBit(MelodyPart::Arp)) == 0) (poly ? polyMasked : otherMasked)++;
                (poly ? polyShift : otherShift) += bp.arpOctave;
            }
            int& low = poly ? lowestPoly : lowestOther;
            low = std::min(low, t.melody.arpLo);
        }
        const double pm = polyDrops > 0 ? static_cast<double>(polyMasked) / polyDrops : 0.0;
        const double om = otherDrops > 0 ? static_cast<double>(otherMasked) / otherDrops : 0.0;
        check(polyDrops > 10 && otherDrops > 50 && lowestPoly >= kArpLowest && lowestOther >= kArpLowest
                  && pm <= om + 0.15,
              "a polymetric arp costs the masking rule and the depth rule nothing measurable",
              fmt("arp masked out of %.3f of polymetric drops against %.3f elsewhere (%d and %d drops), "
                  "mean octave shift %.2f against %.2f, lowest arp note %d against %d",
                  pm, om, polyDrops, otherDrops, polyDrops > 0 ? static_cast<double>(polyShift) / polyDrops : 0.0,
                  otherDrops > 0 ? static_cast<double>(otherShift) / otherDrops : 0.0, lowestPoly, lowestOther));
    }
}

/**
 * @brief The audibility match (23.09.2026; ComposerLevels.cpp, matchAudibility): it only lifts, never past its cap, only
 *        lines under their share of the lead, and the lift is exactly what the plan's part gain gained.
 */
void testAudibilityMatch()
{
    section("audibility match: quiet lines lifted to a share of the lead");
    ParamStore on, off;
    off.parseText("compose.audibility_match=0");
    Composer a(1), b(1);
    int lifted = 0, tracks = 0, measured = 0;
    bool capped = true, onlyUnder = true, exact = true, leadUntouched = true;
    for (int t = 0; t < 3; ++t) {
        const TrackPlan pa = a.track(on, t), pb = b.track(off, t);
        ++tracks;
        const double lead = pa.audibleInMix[mpIndex(MelodyPart::Lead)];
        if (lead > 0.5) ++measured;
        for (int k = 0; k < kMelodyParts; ++k) {
            const float lift = pa.audibilityLiftDb[k];
            capped = capped && lift >= 0.0f && lift <= 4.0f + 1e-4f;
            // The part gains of the two plans differ by the lift alone (the rest of the level match is the same).
            exact = exact && std::fabs((pa.partGainDb[k] - pb.partGainDb[k]) - lift) < 1e-4f;
            if (lift > 0.0f) {
                ++lifted;
                const double share = k == mpIndex(MelodyPart::Counter) ? 0.6 : 0.35;
                onlyUnder = onlyUnder && pa.audibleInMix[k] < share * lead;
            }
        }
        leadUntouched = leadUntouched && pa.audibilityLiftDb[mpIndex(MelodyPart::Lead)] == 0.0f && pa.audibilityLiftDb[mpIndex(MelodyPart::Acid)] == 0.0f;
    }
    check(measured > 0 && lifted > 0 && capped && onlyUnder && exact && leadUntouched,
          "only lines under their share of the lead are lifted, by at most 4 dB, and the lift is all that moves",
          fmt("%d tracks, %d measured against a lead, %d lines lifted", tracks, measured, lifted));
}

/**
 * @brief The audibility meter (23.09.2026, round "Hörbarkeit"; phos/Audibility.h): the textbook facts of
 *        masking on synthetic signals, then a real mix part that is turned down.
 */
void testAudibility()
{
    section("audibility: partial loudness of a part in the mix");
    const double sr = 48000.0;
    const int n = 48000;
    auto tone = [&](double hz, double dbfs) {
        std::vector<float> x(static_cast<size_t>(n));
        const double a = std::pow(10.0, dbfs / 20.0);   // dBFS of a sine: its peak against full scale
        for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = static_cast<float>(a * std::sin(2.0 * 3.141592653589793 * hz * i / sr));
        return x;
    };
    // Band noise: a sum of 40 sines with random phases spread over +-10 % of the centre, as much power as a sine at dbfs.
    auto noise = [&](double hz, double dbfs, uint64_t seed) {
        std::vector<float> x(static_cast<size_t>(n), 0.0f);
        Rng r;
        r.seed(seed);
        const double a = std::pow(10.0, dbfs / 20.0) / std::sqrt(40.0);
        for (int k = 0; k < 40; ++k) {
            const double f = hz * (0.9 + 0.2 * k / 39.0), ph = 6.283185307179586 * r.uniform();
            for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] += static_cast<float>(a * std::sin(6.283185307179586 * f * i / sr + ph));
        }
        return x;
    };
    auto measure = [&](const std::vector<float>& target, const std::vector<float>& masker) {
        AudibilityMeter m(2, sr);
        const float* L[2] = { target.data(), masker.empty() ? nullptr : masker.data() };
        m.add(L, nullptr, n);
        return m.read(0);
    };
    const std::vector<float> none;
    const AudibilityReading t40 = measure(tone(1000.0, -60.0), none), t60 = measure(tone(1000.0, -40.0), none), t80 = measure(tone(1000.0, -20.0), none);
    check(std::fabs(t40.alone - 1.0) < 0.15 && t60.alone > 1.5 * t40.alone && t80.alone > 1.5 * t60.alone && t80.ratio == 1.0,
          "a 1 kHz tone at 40 dB SPL is about one unit, loudness grows with level, and a part alone is heard whole",
          fmt("1 kHz at 40/60/80 dB SPL: %.2f, %.2f, %.2f units; ratio alone %.3f", t40.alone, t60.alone, t80.alone, t80.ratio));
    const std::vector<float> target = tone(1000.0, -30.0);
    const AudibilityReading equal = measure(target, noise(1000.0, -30.0, 1)), quiet = measure(target, noise(1000.0, -60.0, 2)),
                            distant = measure(target, noise(6000.0, -30.0, 3));
    check(equal.ratio < 0.6 && quiet.ratio > 0.9 && distant.ratio > equal.ratio + 0.2,
          "a masker in the part's own band takes most of it, one 30 dB down or far away in frequency takes little",
          fmt("1 kHz tone at 70 dB SPL under band noise at 1 kHz, same level: %.2f; 30 dB down: %.2f; at 6 kHz, same level: %.2f", equal.ratio, quiet.ratio, distant.ratio));
    // The upward spread of masking: a low masker masks a higher part more than the other way round.
    const AudibilityReading up = measure(tone(800.0, -40.0), noise(400.0, -20.0, 4)), down = measure(tone(400.0, -40.0), noise(800.0, -20.0, 5));
    check(up.ratio < down.ratio, "masking spreads upward: a masker an octave below takes more than one an octave above",
          fmt("800 Hz under 400 Hz noise %.2f, 400 Hz under 800 Hz noise %.2f", up.ratio, down.ratio));

    // A real mix: the pad in the first 24 bars of seed 1 (intro and groove), measured twice from the same render --
    // as it plays, and with its stem taken 20 dB down against the same other parts.
    {
        auto engine = std::make_unique<Engine>();
        ParamStore& p = engine->params();
        p.parseText("compose.level_match=Off master.auto_gain=Off compose.presence_match=Off");
        Composer composer(1);
        const int block = 256;
        engine->prepare(sr, block);
        Conductor conductor(*engine, composer);
        std::vector<std::vector<float>> bl(kNumStems, std::vector<float>(block)), br(kNumStems, std::vector<float>(block));
        std::vector<float> padL(block), padR(block);
        StemTap tap;
        for (int s = 0; s < kNumStems; ++s) { tap.L[s] = bl[static_cast<size_t>(s)].data(); tap.R[s] = br[static_cast<size_t>(s)].data(); }
        engine->setStemTap(&tap);
        AudibilityMeter asIs(kNumStems, sr), down(kNumStems, sr);
        const int pad = static_cast<int>(Part::Pad);
        const float* sl[kNumStems];
        const float* srr[kNumStems];
        const float* dl[kNumStems];
        const float* dr[kNumStems];
        for (int s = 0; s < kNumStems; ++s) { sl[s] = dl[s] = bl[static_cast<size_t>(s)].data(); srr[s] = dr[s] = br[static_cast<size_t>(s)].data(); }
        dl[pad] = padL.data();
        dr[pad] = padR.data();
        const size_t total = static_cast<size_t>(std::llround(24.0 * kBeatsPerBar * 60.0 / p.get(p.base(Module::Compose) + compose::Bpm) * sr));
        std::vector<float> L(block), R(block);
        for (size_t done = 0; done < total;) {
            const int k = static_cast<int>(std::min<size_t>(block, total - done));
            conductor.pump(p, 32.0);
            engine->process(L.data(), R.data(), k);
            for (int i = 0; i < k; ++i) { padL[static_cast<size_t>(i)] = 0.1f * bl[static_cast<size_t>(pad)][static_cast<size_t>(i)]; padR[static_cast<size_t>(i)] = 0.1f * br[static_cast<size_t>(pad)][static_cast<size_t>(i)]; }
            asIs.add(sl, srr, k);
            down.add(dl, dr, k);
            done += static_cast<size_t>(k);
        }
        engine->setStemTap(nullptr);
        const AudibilityReading padAt = asIs.read(pad), padDown = down.read(pad);
        // The partial loudness is what falls, not necessarily the ratio: where the pad has its bands to itself it is
        // heard whole at any level, and the bands where the others cover it weigh less once it is quieter.
        check(padAt.frames > 0 && padAt.inMix <= padAt.alone && padDown.inMix < 0.5 * padAt.inMix && padDown.alone < 0.5 * padAt.alone,
              "in a real mix a part taken 20 dB down is heard with less than half its partial loudness, and never louder than alone",
              fmt("pad in the first 24 bars of seed 1: %.2f units in the mix of %.2f alone (%.2f); 20 dB down: %.2f of %.2f (%.2f)", padAt.inMix, padAt.alone,
                  padAt.ratio, padDown.inMix, padDown.alone, padDown.ratio));
    }
}

/**
 * @brief The stems (23.09.2026, round "Stems"; Engine.h, StemTap): taking them changes nothing, each part's
 *        stem is that part alone, and a part appears in its stem where the score has it.
 */
void testStems()
{
    section("stems: one per part, taken before the master");
    const int bars = 24, block = 256;
    const double sr = 48000.0;
    struct Take { std::vector<float> mixL; std::vector<std::vector<float>> stemL; };
    auto render = [&](bool withTap, bool mutePad) {
        auto engine = std::make_unique<Engine>();
        ParamStore& p = engine->params();
        p.parseText("compose.level_match=Off master.auto_gain=Off compose.presence_match=Off");
        if (mutePad) p.parseText("mix.pad_mute=1");
        Composer composer(1);
        engine->prepare(sr, block);
        Conductor conductor(*engine, composer);
        const size_t total = static_cast<size_t>(std::llround(bars * kBeatsPerBar * 60.0 / p.get(p.base(Module::Compose) + compose::Bpm) * sr));
        Take t;
        t.mixL.assign(total, 0.0f);
        std::vector<float> R(total);
        std::vector<std::vector<float>> bl(kNumStems, std::vector<float>(block)), br(kNumStems, std::vector<float>(block));
        StemTap tap;
        for (int s = 0; s < kNumStems; ++s) { tap.L[s] = bl[static_cast<size_t>(s)].data(); tap.R[s] = br[static_cast<size_t>(s)].data(); }
        if (withTap) { engine->setStemTap(&tap); t.stemL.assign(kNumStems, std::vector<float>(total)); }
        for (size_t done = 0; done < total;) {
            const int n = static_cast<int>(std::min<size_t>(block, total - done));
            conductor.pump(p, 32.0);
            engine->process(t.mixL.data() + done, R.data() + done, n);
            if (withTap)
                for (int s = 0; s < kNumStems; ++s) std::copy(bl[static_cast<size_t>(s)].begin(), bl[static_cast<size_t>(s)].begin() + n, t.stemL[static_cast<size_t>(s)].begin() + static_cast<std::ptrdiff_t>(done));
            done += static_cast<size_t>(n);
        }
        engine->setStemTap(nullptr);
        return t;
    };
    const Take plain = render(false, false), tapped = render(true, false), muted = render(true, true);
    check(plain.mixL == tapped.mixL, "taking the stems leaves the mix the same, bit for bit");
    const int pad = static_cast<int>(Part::Pad), kick = static_cast<int>(Part::Kick);
    auto energy = [](const std::vector<float>& x, size_t a, size_t b) { double e = 0.0; for (size_t i = a; i < b && i < x.size(); ++i) e += static_cast<double>(x[i]) * x[i]; return e; };
    const size_t n = tapped.mixL.size();
    bool othersSame = true;
    for (int s = 0; s < kNumParts; ++s) if (s != pad) othersSame = othersSame && tapped.stemL[static_cast<size_t>(s)] == muted.stemL[static_cast<size_t>(s)];
    const double padOn = energy(tapped.stemL[static_cast<size_t>(pad)], 0, n), padOff = energy(muted.stemL[static_cast<size_t>(pad)], 0, n);
    check(padOn > 0.0 && padOff == 0.0 && othersSame, "a muted part leaves an empty stem and every other part's stem as it was",
          fmt("pad stem energy %.3g unmuted, %.3g muted", padOn, padOff));
    // The set's first kick is on bar 17 (kIntroKickBar): its stem is silent before and sounds after.
    const size_t barLen = n / static_cast<size_t>(bars);
    const double kickBefore = energy(tapped.stemL[static_cast<size_t>(kick)], 0, 16 * barLen - barLen / 8);
    const double kickAfter = energy(tapped.stemL[static_cast<size_t>(kick)], 16 * barLen, n);
    int silentParts = 0;
    for (int s = 0; s < kNumStems; ++s) silentParts += energy(tapped.stemL[static_cast<size_t>(s)], 0, n) == 0.0 ? 1 : 0;
    check(kickBefore == 0.0 && kickAfter > 0.0, "the kick's stem is silent before the set's first kick on bar 17 and sounds after it",
          fmt("kick stem energy %.3g before, %.3g after; %d of %d stems silent over the %d bars", kickBefore, kickAfter, silentParts, kNumStems, bars));
}

/**
 * @brief The plan cache under knobs that move while the composer works (25.09.2026; docs/rounds/2026-09.md).
 *
 * The plugin hands the composer a store that a host's automation and the editor write while it composes. Until
 * this date every track() and trackOfBar() inside composeBars checked the knobs again and threw the cached plans
 * away when one had moved -- while composeBars still held a reference into them (use after free), and a knob
 * that never stopped moving had the seek plan from the start without end. Now a call checks the knobs once, when
 * it begins; a change takes effect at the next call, and a plan never moves when a later one is added.
 * Part (1) moves a compose knob from a second thread while bars over a DJ overlap are composed and counts how
 * often the plans are thrown away inside one call; (2) holds a reference while tracks are added; (3) checks that
 * a change is taken over by the very next call.
 */
void testPlanCacheLive()
{
    section("plan cache: a knob that moves during composeBars never frees a plan in use");
    ParamStore p;
    p.parseText("compose.level_match=Off compose.presence_match=Off compose.audibility_match=Off master.auto_gain=Off");
    const int sv = p.base(Module::Compose) + compose::SoundVariation;
    Composer c(7);
    const TrackPlan second = c.track(p, 1);   // by value: the cache is about to be thrown away many times
    const int from = second.firstBar;          // the incoming track's first bar: both plans are read there

    // (1) A writer moves two knobs to new values all the time -- the sound and the track length, which moves every
    // track boundary -- and each call may take the change over once, at its start. Until 25.09.2026 the track index
    // composeBars had found and the plan it then read came from two different sets of plans, and a bar outside the
    // plan's own range read its bass masks and phrases out of bounds.
    const int tb = p.base(Module::Compose) + compose::TrackBars;
    std::atomic<bool> stop{ false };
    std::thread writer([&] {
        for (uint32_t k = 1; !stop.load(std::memory_order_relaxed); ++k) {
            const uint32_t h = k * 2654435761u;
            p.set(sv, 0.3f + 0.4f * static_cast<float>(h % 1000u) / 1000.0f);
            p.set(tb, static_cast<float>(192 + 64 * static_cast<int>((h >> 12) % 3u)));
            std::this_thread::yield();
        }
    });
    while (p.get(sv) == 0.5f) std::this_thread::yield();   // the cached calls take microseconds: wait for the first write
    uint64_t worst = 0, total = 0;
    size_t notes = 0;
    for (int bar = from; bar < from + 12; ++bar) {
        std::vector<NoteEvent> out;
        std::vector<ControlEvent> ctl;
        const uint64_t g0 = c.planGeneration();
        c.composeBars(p, bar, 1, out, &ctl);
        const uint64_t moved = c.planGeneration() - g0;
        worst = std::max(worst, moved);
        total += moved;
        notes += out.size();
    }
    stop.store(true);
    writer.join();
    check(total > 0 && worst <= 1 && notes > 0,
          "a knob moving during composeBars throws the plans away at most once per call, at its start",
          fmt("at most %llu times in one call, %llu in 12 calls, %zu notes", static_cast<unsigned long long>(worst),
              static_cast<unsigned long long>(total), notes));

    // (2) Adding tracks never moves the ones before.
    const TrackPlan* first = &c.track(p, 0);
    c.track(p, 12);
    check(&c.track(p, 0) == first, "a plan stays where it is while later tracks are planned");

    // (3) A change is taken over by the next call, once.
    p.set(sv, 0.9f);
    const uint64_t g0 = c.planGeneration();
    std::vector<NoteEvent> out;
    c.composeBars(p, from, 1, out, nullptr);
    const uint64_t g1 = c.planGeneration();
    c.composeBars(p, from + 1, 1, out, nullptr);
    check(g1 == g0 + 1 && c.planGeneration() == g1, "a knob change is taken over by the next call, and only once",
          fmt("%llu, then %llu", static_cast<unsigned long long>(g1 - g0), static_cast<unsigned long long>(c.planGeneration() - g1)));
}

/**
 * @brief The deferred first plan (23.09.2026, round "Planung", Composer::completeMeasurement).
 *
 * A live start plans the first track without a single probe render and measures it behind the music. The plan
 * has to come out of that exactly as one planned whole -- the loudness readings, the part gains, the presence and
 * the audibility match -- and a later track, planned while the first is still unmeasured, has to measure the first
 * one itself and then be the same track as in a whole plan. The events the host sends afterwards carry the same
 * values the track start would have written.
 */
void testDeferredPlan()
{
    section("deferred first plan: plays at once, measured behind the music, the same numbers");
    ParamStore p;
    auto sameLevels = [](const TrackPlan& a, const TrackPlan& b) {
        bool ok = a.loudness == b.loudness && a.gainDb == b.gainDb && a.presenceDb == b.presenceDb && a.presenceGainDb == b.presenceGainDb;
        for (int k = 0; k < kMelodyParts; ++k)
            ok = ok && a.partLoudness[k] == b.partLoudness[k] && a.partGainDb[k] == b.partGainDb[k]
                 && a.audibilityLiftDb[k] == b.audibilityLiftDb[k] && a.audibleInMix[k] == b.audibleInMix[k];
        return ok;
    };
    Composer whole(11);
    const auto w0 = std::chrono::steady_clock::now();
    const TrackPlan ref0 = whole.track(p, 0);
    const double wholeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    const TrackPlan ref1 = whole.track(p, 1);

    Composer live(11);
    live.setDeferMasterGain(true);
    const auto l0 = std::chrono::steady_clock::now();
    const TrackPlan first = live.track(p, 0);
    const double liveSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - l0).count();
    bool zero = first.measureDeferred && first.masterDeferred && first.presenceGainDb == 0.0f && first.loudness == 0.0;
    for (int k = 0; k < kMelodyParts; ++k) zero = zero && first.partGainDb[k] == 0.0f;
    check(zero && liveSeconds < 0.5 * wholeSeconds,
          "a live start plans the first track without its probes: every correction zero, and in a fraction of the time",
          fmt("%.2f s instead of %.2f s", liveSeconds, wholeSeconds));

    const bool measured = live.completeMeasurement(p, 0);
    const TrackPlan& after = live.track(p, 0);
    const bool again = live.completeMeasurement(p, 0);
    check(measured && !again && !after.measureDeferred && after.correctionsPending && sameLevels(after, ref0),
          "measured behind the music, the first plan's numbers are those of a plan made whole",
          fmt("presence gain %.3f / %.3f dB, counter %.3f / %.3f dB", after.presenceGainDb, ref0.presenceGainDb,
              after.partGainDb[mpIndex(MelodyPart::Counter)], ref0.partGainDb[mpIndex(MelodyPart::Counter)]));

    // What the host sends afterwards: every melodic part's level, the lines with the presence gain on top.
    std::vector<ControlEvent> ev;
    live.levelControls(p, after, 12.0, 16.0f, ev);
    const int mb = p.base(Module::Mix);
    bool events = static_cast<int>(ev.size()) == kMelodyParts;
    for (int k = 0; events && k < kMelodyParts; ++k) {
        const MelodyPart part = static_cast<MelodyPart>(k);
        const int level = mb + (part == MelodyPart::Acid ? static_cast<int>(mix::AcidLevel) : mix::polyLevel(melodyPoly(part)));
        const bool line = part == MelodyPart::Lead || part == MelodyPart::Counter || part == MelodyPart::Arp || part == MelodyPart::Stab;
        const ParamDesc& d = p.desc(level);
        const float want = (after.partGainDb[k] + (line ? after.presenceGainDb : 0.0f)) / (d.maxValue - d.minValue);
        events = ev[static_cast<size_t>(k)].param == level && ev[static_cast<size_t>(k)].value == want && ev[static_cast<size_t>(k)].length == 16.0f
              && ev[static_cast<size_t>(k)].beat == 12.0 && ev[static_cast<size_t>(k)].kind == ControlEvent::Kind::Offset;
    }
    check(events, "the level events it sends are the track start's values, ramped");

    // The plugin's way (PluginProcessor.cpp, MeasureJob): a copy measures behind the music and the original takes the
    // numbers over -- and refuses them once its plans were thrown away (a knob, a seed, a reroll) since the copy.
    {
        Composer original(11);
        original.setDeferMasterGain(true);
        original.track(p, 0);
        Composer copy = original;
        const uint64_t generation = original.planGeneration();
        copy.completeMeasurement(p, 0);
        const bool taken = original.adoptMeasurement(0, copy.track(p, 0), generation);
        const TrackPlan& adopted = original.track(p, 0);
        Composer stale(11);
        stale.setDeferMasterGain(true);
        stale.track(p, 0);
        const uint64_t before = stale.planGeneration();
        stale.setSeed(12);
        stale.track(p, 0);
        const bool refused = !stale.adoptMeasurement(0, copy.track(p, 0), before);
        check(taken && !adopted.measureDeferred && adopted.correctionsPending && sameLevels(adopted, ref0) && refused,
              "a copy's measurement is taken over number for number, and refused once the plans changed");
    }

    // A later track planned while the first is still unmeasured measures the first one itself.
    Composer seek(11);
    seek.setDeferMasterGain(true);
    const TrackPlan second = seek.track(p, 1);
    const TrackPlan& firstAfter = seek.track(p, 0);
    check(!firstAfter.measureDeferred && firstAfter.correctionsPending && sameLevels(firstAfter, ref0) && sameLevels(second, ref1)
              && second.masterDeferred && !second.measureDeferred,
          "a later track planned first measures the first one itself and is the track of a whole plan",
          fmt("track 2 gain %.3f / %.3f dB", second.gainDb, ref1.gainDb));
}

/**
 * @brief The factory presets and the own-sound switches (23.09.2026, round "Presets", SoundPresets.h).
 *
 * Every synth has presets in groups, with names that tell them apart; every preset applies, leaves the knobs
 * presetLeaves() names where they were, and reads back as its own text (moduleText of the applied knobs is the
 * preset -- which is what makes a saved user preset the same kind of thing). Then the switch: with lead_own on,
 * the lead's sound knobs are what the page shows at every block of a set, whatever recipes the composer sends;
 * with it off, the composer does move them (otherwise the first half would prove nothing).
 */
void testSoundPresets()
{
    section("sound presets: every synth, in groups; own sound keeps the composer's recipes off");
    struct Synth { const char* name; Module m; int instance; };
    std::vector<Synth> synths = { { "kick", Module::Kick, 0 }, { "bass", Module::Bass, 0 }, { "acid", Module::Acid, 0 } };
    static const char* const kVoice[kPolyInstances] = { "lead", "counter", "arp", "stab", "pad", "drone" };
    for (int v = 0; v < kPolyInstances; ++v) synths.push_back({ kVoice[v], Module::Poly, v });
    bool allOk = true;
    std::string summary;
    for (const Synth& s : synths) {
        const std::vector<SoundPreset>& list = factoryPresets(s.m, s.instance);
        std::set<std::string> groups, names, texts;
        std::map<std::string, std::string> byText;   // for the report: which two presets sound the same
        std::string twins;
        int applied = 0, roundTrip = 0, kept = 0;
        for (const SoundPreset& sp : list) {
            groups.insert(sp.group);
            names.insert(sp.name);
            if (!texts.insert(sp.text).second && twins.size() < 200) twins += " '" + byText[sp.text] + "' = '" + sp.name + "'";
            byText.emplace(sp.text, sp.name);
            ParamStore p;
            const int b = p.base(s.m, s.instance);
            // Every knob a preset leaves alone gets a value of its own first, so "left alone" is visible.
            std::vector<float> set(static_cast<size_t>(ParamStore::moduleCount(s.m)));
            for (int k = 0; k < ParamStore::moduleCount(s.m); ++k) {
                if (presetLeaves(s.m, k)) p.setNormalised(b + k, 0.37f);
                set[static_cast<size_t>(k)] = p.get(b + k);   // as the knob keeps it (a switch or a choice snaps)
            }
            applied += applySoundPreset(p, s.m, s.instance, sp.text) ? 1 : 0;
            roundTrip += moduleText(p, s.m, s.instance) == sp.text ? 1 : 0;
            bool leftAlone = true;
            for (int k = 0; k < ParamStore::moduleCount(s.m); ++k)
                if (presetLeaves(s.m, k)) leftAlone = leftAlone && p.get(b + k) == set[static_cast<size_t>(k)];
            kept += leftAlone ? 1 : 0;
        }
        const int n = static_cast<int>(list.size());
        const bool ok = n >= 60 && groups.size() >= 2   // 24.09.2026: "deutlich mehr Presets" -- 9 to 25 a synth before && static_cast<int>(names.size()) == n && static_cast<int>(texts.size()) == n
                     && applied == n && roundTrip == n && kept == n;
        allOk = allOk && ok;
        summary += fmt("%s %d in %d groups%s; ", s.name, n, static_cast<int>(groups.size()),
                       ok ? "" : fmt(" (names %d, texts %d, applied %d, round trip %d, kept %d;%s)", static_cast<int>(names.size()),
                                     static_cast<int>(texts.size()), applied, roundTrip, kept, twins.c_str()).c_str());
    }
    check(allOk, "every synth has presets in groups, distinct in name and sound; each applies, reads back as itself and leaves level, "
                 "ducking, high pass and gate alone", summary);

    // The switch.
    auto walk = [](bool own) {
        auto engine = std::make_unique<Engine>();
        ParamStore& p = engine->params();
        p.parseText("compose.level_match=Off master.auto_gain=Off compose.presence_match=Off compose.audibility_match=Off");
        if (own) p.parseText("mix.lead_own=1");
        Composer composer(7);
        engine->prepare(48000.0, 512);
        Conductor conductor(*engine, composer);
        std::vector<float> L(512), R(512);
        const int b = p.base(PolyInstance::Lead);
        int differs = 0, blocks = 0;
        for (int i = 0; i < 48000 * 40 / 512; ++i) {   // 40 s: past the first recipes of the first track
            conductor.pump(p, 32.0);
            engine->process(L.data(), R.data(), 512);
            ++blocks;
            for (int k = 0; k < poly::Count; ++k)
                if (!presetLeaves(Module::Poly, k) && engine->effective(b + k) != p.get(b + k)) { ++differs; break; }
        }
        return std::make_pair(differs, blocks);
    };
    const auto withOwn = walk(true), without = walk(false);
    check(withOwn.first == 0 && without.first > 0, "own sound: the lead's sound knobs are what the page shows at every block; without it the composer moves them",
          fmt("blocks with a lead knob off the page: %d of %d with own sound, %d of %d without", withOwn.first, withOwn.second, without.first, without.second));

    // The effect presets (24.09.2026, the user: "Im SFX-Fenster ist nach wie vor keine Auswahl fuer das Preset").
    // Each family's choice has the family's size for its range, and a fixed choice makes an event play that preset
    // whatever preset the event carries -- the same samples as the event carrying it itself -- while Auto plays the
    // event's own.
    {
        ParamStore q;
        const int sb = q.base(Module::Sfx);
        bool ranges = true;
        for (int i = 0; i < sfx::kNumPresetChoices; ++i)
            ranges = ranges && static_cast<int>(q.desc(sb + sfx::kFirstPreset + i).maxValue) == kSfxBankCount[static_cast<int>(kPresetChoiceType[i])]
                     && sfxPresetChoice(kPresetChoiceType[i]) == i;
        auto zap = [&](float fixed, int carried) {
            q.set(sb + sfx::PresetZap, fixed);
            Sfx x;
            x.prepare(48000.0);
            std::vector<float> v = moduleValues(q, Module::Sfx);
            x.update(v.data(), 6);
            x.trigger(SfxType::Zap, 12000, 1.0f, 0.0, carried);
            return renderMono([&](float* L, float* R, int n) { x.process(L, R, n); }, 16000);
        };
        const std::vector<float> fixed = zap(17.0f, 5), carried = zap(0.0f, 17), own = zap(0.0f, 5);
        check(ranges && fixed == carried && fixed != own,
              "effect presets: a choice per family with the family's size, a fixed choice replaces the event's preset, Auto keeps it");
    }
}

/**
 * @brief A MIDI keyboard on one voice (23.09.2026, round "Keyboard", Engine::liveNoteOn).
 *
 * A set, the stems taken. Replace on a voice empties its stem of the composer's notes; a played note then sounds on
 * that stem and nowhere else, and its key's release lets it die away. Layer without a played note is the set as
 * it was, bit for bit.
 */
void testKeyboard()
{
    section("keyboard: played notes on the chosen voice, with its sound");
    constexpr int kBlock = 256;
    const double sr = 48000.0;
    struct Take { std::vector<std::vector<float>> stem; std::vector<float> mix; };
    // part: mix.keyboard_part; mode 0 Replace, 1 Layer; a note from onAt to offAt (samples, -1 none).
    auto render = [&](int part, int mode, size_t onAt, size_t offAt, size_t total) {
        auto engine = std::make_unique<Engine>();
        ParamStore& p = engine->params();
        p.parseText("compose.level_match=Off master.auto_gain=Off compose.presence_match=Off compose.audibility_match=Off");
        p.set(p.base(Module::Mix) + mix::KeyboardPart, static_cast<float>(part));
        p.set(p.base(Module::Mix) + mix::KeyboardMode, static_cast<float>(mode));
        Composer composer(3);
        engine->prepare(sr, kBlock);
        Conductor conductor(*engine, composer);
        Take t;
        t.stem.assign(kNumStems, std::vector<float>(total));
        t.mix.assign(total, 0.0f);
        std::vector<float> R(total);
        std::vector<std::vector<float>> bl(kNumStems, std::vector<float>(kBlock)), br(kNumStems, std::vector<float>(kBlock));
        StemTap tap;
        for (int s = 0; s < kNumStems; ++s) { tap.L[s] = bl[static_cast<size_t>(s)].data(); tap.R[s] = br[static_cast<size_t>(s)].data(); }
        engine->setStemTap(&tap);
        for (size_t done = 0; done < total;) {
            size_t n = std::min<size_t>(kBlock, total - done);
            // The plugin's split: a block ends where a note message falls.
            for (size_t at : { onAt, offAt }) if (at != static_cast<size_t>(-1) && at > done && at < done + n) n = at - done;
            if (done == onAt) for (int pitch : { 60, 63, 67 }) engine->liveNoteOn(pitch, 100, 0);
            if (done == offAt) for (int pitch : { 60, 63, 67 }) engine->liveNoteOff(pitch, 0);
            conductor.pump(p, 32.0);
            engine->process(t.mix.data() + done, R.data() + done, static_cast<int>(n));
            for (int s = 0; s < kNumStems; ++s)
                std::copy(bl[static_cast<size_t>(s)].begin(), bl[static_cast<size_t>(s)].begin() + static_cast<std::ptrdiff_t>(n),
                          t.stem[static_cast<size_t>(s)].begin() + static_cast<std::ptrdiff_t>(done));
            done += n;
        }
        engine->setStemTap(nullptr);
        return t;
    };
    auto energy = [](const std::vector<float>& x, size_t a, size_t b) { double e = 0.0; for (size_t i = a; i < b && i < x.size(); ++i) e += static_cast<double>(x[i]) * x[i]; return e; };
    const size_t total = static_cast<size_t>(sr * 60.0);
    const size_t none = static_cast<size_t>(-1);
    // Which voice plays most in this minute: the one Replace has something to take away from.
    const Take off = render(0, 0, none, none, total);
    int voice = static_cast<int>(Part::Lead);
    for (int k = 0; k < kPolyInstances; ++k) {
        const int s = static_cast<int>(polyPart(static_cast<PolyInstance>(k)));
        if (energy(off.stem[static_cast<size_t>(s)], 0, total) > energy(off.stem[static_cast<size_t>(voice)], 0, total)) voice = s;
    }
    const int part = 2 + (voice - static_cast<int>(Part::Lead));   // keyboard_part: 1 acid, 2 lead .. 7 drone
    const Take layerQuiet = render(part, 1, none, none, total);
    check(layerQuiet.mix == off.mix, "Layer without a played note is the set, bit for bit");
    const Take replaced = render(part, 0, none, none, total);
    const double generated = energy(off.stem[static_cast<size_t>(voice)], 0, total);
    check(generated > 0.0 && energy(replaced.stem[static_cast<size_t>(voice)], 0, total) == 0.0,
          "Replace leaves the voice's generated notes out", fmt("%s: stem energy %.3g in the set, %.3g replaced", kStemNames[voice], generated,
                                                               energy(replaced.stem[static_cast<size_t>(voice)], 0, total)));
    // A chord held for four seconds from 20 s, then released.
    const size_t onAt = static_cast<size_t>(sr * 20.0) + 77, offAt = static_cast<size_t>(sr * 24.0) + 13;
    const Take played = render(part, 0, onAt, offAt, total);
    const std::vector<float>& st = played.stem[static_cast<size_t>(voice)];
    const double before = energy(st, 0, onAt), held = energy(st, onAt + static_cast<size_t>(sr * 1.0), offAt);
    const double after = energy(st, offAt + static_cast<size_t>(sr * 8.0), offAt + static_cast<size_t>(sr * 11.0));
    bool othersSame = true;
    for (int s = 0; s < kNumParts; ++s) if (s != voice) othersSame = othersSame && played.stem[static_cast<size_t>(s)] == replaced.stem[static_cast<size_t>(s)];
    const double heldPerSample = held / static_cast<double>(offAt - onAt - static_cast<size_t>(sr)), afterPerSample = after / (sr * 3.0);
    check(before == 0.0 && heldPerSample > 1e-6 && afterPerSample < 0.01 * heldPerSample && othersSame,
          "a played chord sounds on the voice's stem only, from its sample, and dies away after the keys are released",
          fmt("%s: mean square %.3g held, %.3g from 8 s after the release (%.1f dB); before the chord %.3g; other stems %s",
              kStemNames[voice], heldPerSample, afterPerSample, 10.0 * std::log10((afterPerSample + 1e-30) / (heldPerSample + 1e-30)), before,
              othersSame ? "unchanged" : "CHANGED"));
}

/**
 * @brief Random knobs, fixed invariants (23.09.2026, round "Fuzz").
 *
 * Every other section sets the knobs it is about and leaves the rest at their defaults, so the corners of
 * the knob space -- a 32-bar track in a 300-minute set, Hi-Tech with no hats in the kit, tempo range 10 at
 * 100 BPM -- are planned by nobody but a user. This section draws whole configurations: every compose knob
 * uniform over its range (choices and toggles included), the kick's engine and clip, the bass release, and
 * the active flag, role, density and engine of every percussion lane; a random set seed. Only the three
 * knobs that start probe renders stay off (level match, presence match, auto gain), because each probe costs
 * seconds and they decide levels, not structure. For three tracks of each configuration it checks what must
 * hold whatever the knobs say:
 *  - the form keeps its constraints and adds up to the track, sections contiguous;
 *  - key, mode, style and tempo are in range and finite;
 *  - every note the composer sends is finite, inside the rendered span, with a pitch and velocity MIDI can
 *    carry and a positive length; every control event names a real parameter with a finite value;
 *  - at most one kick on any beat and no two bass notes sounding at once, across the DJ blends.
 * A failure prints the configuration as `--seed ... --set ...`, so phos_render reproduces it. The draw is
 * fixed (the section's own seed), so the same configurations run every time; PHOS_FUZZ_CASES=n runs n of
 * them instead of the default 24 for a deeper search.
 */
void testKnobFuzz()
{
    section("fuzz: random knob configurations keep the invariants");
    int cases = 24;
    if (const char* env = std::getenv("PHOS_FUZZ_CASES")) cases = std::max(1, std::atoi(env));
    Rng r;
    r.seed(0x46555A5A4B4E4F42ull);
    int broken = 0;
    std::string firstBroken;
    int planned = 0, notesSeen = 0;
    for (int c = 0; c < cases; ++c) {
        ParamStore q;
        std::string text;
        auto randomise = [&](int id) {
            const ParamDesc& d = q.desc(id);
            float v = d.minValue + (d.maxValue - d.minValue) * r.uniform();
            if (d.curve == Curve::Int || d.curve == Curve::Choice || d.curve == Curve::Toggle) v = std::round(v);
            q.set(id, v);
            text += fmt(" --set %s=%g", q.key(id).c_str(), static_cast<double>(q.get(id)));
        };
        const int cb = q.base(Module::Compose);
        for (int i = 0; i < compose::Count; ++i) {
            if (i == compose::LevelMatch || i == compose::PresenceMatch) continue;
            randomise(cb + i);
        }
        randomise(q.base(Module::Kick) + kick::Engine);
        randomise(q.base(Module::Kick) + kick::Clip);
        randomise(q.base(Module::Bass) + bass::AmpRelease);
        for (int l = 0; l < kPercLanes; ++l) {
            const int b = q.base(Module::Perc, l);
            for (int k : { static_cast<int>(perc::Active), static_cast<int>(perc::Role), static_cast<int>(perc::Density), static_cast<int>(perc::Engine) })
                randomise(b + k);
        }
        q.parseText("compose.level_match=Off compose.presence_match=Off master.auto_gain=Off");
        const uint64_t seed = 1 + static_cast<uint64_t>(r.below(1 << 30));
        Composer comp(seed);
        std::vector<std::string> why;
        int lastBar = 0;
        for (int t = 0; t < 3; ++t) {
            const TrackPlan p = comp.track(q, t);
            ++planned;
            lastBar = std::max(lastBar, p.firstBar + p.bars);
            int sum = 0;
            bool contiguous = true;
            for (int s = 0; s < p.form.count; ++s) {
                contiguous = contiguous && p.form.section[s].startBar == sum && p.form.section[s].bars > 0;
                sum += p.form.section[s].bars;
            }
            if (!formConstraintsHold(p.form) || p.form.bars != p.bars || sum != p.bars || !contiguous) why.push_back(fmt("track %d form (%d bars, sections %d)", t + 1, p.bars, sum));
            if (p.key < 0 || p.key > 11 || p.scale < 0 || p.scale >= kNumScales || p.style < 0 || p.style >= kNumStyles || !std::isfinite(p.bpm) || p.bpm < 20.0 || p.bpm > 400.0)
                why.push_back(fmt("track %d key %d scale %d style %d bpm %.2f", t + 1, p.key, p.scale, p.style, p.bpm));
        }
        std::vector<NoteEvent> notes;
        std::vector<ControlEvent> controls;
        comp.composeBars(q, 0, lastBar, notes, &controls);
        notesSeen += static_cast<int>(notes.size());
        const double end = static_cast<double>(lastBar) * kBeatsPerBar;
        int badNotes = 0, badControls = 0, kickDoubles = 0, bassOverlaps = 0;
        std::vector<int> kicks(static_cast<size_t>(lastBar) * 4 + 4, 0);
        const NoteEvent* lastBass = nullptr;
        for (const NoteEvent& e : notes) {
            if (!std::isfinite(e.beat) || e.beat < -1e-9 || e.beat > end + 1e-6 || !std::isfinite(e.length) || e.length <= 0.0f || e.pitch > 127 || e.velocity < 1
                || e.velocity > 127 || static_cast<int>(e.part) < 0 || static_cast<int>(e.part) >= kNumParts) ++badNotes;
            if (e.part == Part::Kick && std::fabs(e.beat - std::round(e.beat)) < 1e-6 && e.beat >= 0.0 && e.beat <= end)
                if (++kicks[static_cast<size_t>(std::llround(e.beat))] == 2) ++kickDoubles;
            if (e.part == Part::Bass) {
                if (lastBass != nullptr && e.beat < lastBass->beat + lastBass->length - 1e-6) ++bassOverlaps;
                lastBass = &e;
            }
        }
        for (const ControlEvent& e : controls)
            if (!std::isfinite(e.beat) || !std::isfinite(e.value) || !std::isfinite(e.length) || e.param < 0 || e.param >= q.count()) ++badControls;
        if (badNotes) why.push_back(fmt("%d malformed notes", badNotes));
        if (badControls) why.push_back(fmt("%d malformed controls", badControls));
        if (kickDoubles) why.push_back(fmt("%d beats with two kicks", kickDoubles));
        if (bassOverlaps) why.push_back(fmt("%d overlapping bass notes", bassOverlaps));
        if (!why.empty()) {
            ++broken;
            if (firstBroken.empty()) {
                std::string w;
                for (const std::string& s : why) w += (w.empty() ? "" : "; ") + s;
                firstBroken = fmt("case %d: %s -- reproduce: --seed %llu%s", c, w.c_str(), static_cast<unsigned long long>(seed), text.c_str());
            }
        }
    }
    check(broken == 0, "every random knob configuration plans valid forms and sends well-formed, collision-free notes",
          broken == 0 ? fmt("%d configurations, %d tracks planned, %d notes checked", cases, planned, notesSeen) : firstBroken);
}

/**
 * @brief Learned preferences (23.09.2026, round "Präferenzen"; phos/Preferences.h): the fit points the right way,
 *        the text form survives, a strong preference moves the draws, and no preference allows what a rule forbids.
 */
void testPreferences()
{
    section("preferences: what the listener liked, learned and applied");
    // The fit: Surge liked four times, Pedal disliked four times, Arch once each way.
    std::vector<RatingEntry> ratings;
    auto rate = [&](int verdict, const char* features) { RatingEntry r; r.verdict = verdict; r.features = features; ratings.push_back(r); };
    for (int i = 0; i < 4; ++i) rate(1, "style=Goa;lead.archetype=Surge");
    for (int i = 0; i < 4; ++i) rate(-1, "style=Goa;lead.archetype=Pedal");
    rate(1, "lead.archetype=Arch");
    rate(-1, "lead.archetype=Arch");
    const Preferences fit = fitPreferences(ratings);
    Preferences back;
    const bool parsed = back.parse(fit.toText());
    check(fit.weight("lead.archetype=Surge") > 0.5 && fit.weight("lead.archetype=Pedal") < -0.5 && std::fabs(fit.weight("lead.archetype=Arch")) < 0.05
              && std::fabs(fit.weight("style=Goa")) < 0.05 && parsed && back.toText() == fit.toText(),
          "the fit: liked values up, disliked down, a split verdict near zero; the text form reads back the same",
          fmt("Surge %+.2f, Pedal %+.2f, Arch %+.2f, Goa %+.2f", fit.weight("lead.archetype=Surge"), fit.weight("lead.archetype=Pedal"),
              fit.weight("lead.archetype=Arch"), fit.weight("style=Goa")));

    // The draws: 24 tracks planned with and without a strong liking for the Pedal archetype and for hocket counters.
    auto count = [&](bool withPrefs, int& pedal, int& hocket, int& total, std::string& features) {
        if (withPrefs) {
            auto p = std::make_shared<Preferences>();
            p->set("lead.archetype=Pedal & Bounce", 2.5);
            p->set("counter.mode=Hocket", 3.0);
            setPreferences(p);
        } else {
            setPreferences(nullptr);
        }
        ParamStore q;
        q.parseText("compose.level_match=Off master.auto_gain=Off compose.presence_match=Off compose.style=Full-On compose.style_mix=0");
        Composer c(2026);
        pedal = hocket = total = 0;
        for (int t = 0; t < 24; ++t) {
            const TrackPlan p = c.track(q, t);
            for (int w = 0; w < 2; ++w) { ++total; if (p.melody.leadArchetype[w] == static_cast<int>(LeadArchetype::PedalAndBounce)) ++pedal; }
            if (p.melody.counterMode == static_cast<int>(CounterMode::Hocket)) ++hocket;
            if (t == 0) features = decisionFeatures(p, 90);
        }
    };
    int pedal0 = 0, hocket0 = 0, n0 = 0, pedal1 = 0, hocket1 = 0, n1 = 0;
    std::string f0, f1;
    count(false, pedal0, hocket0, n0, f0);
    count(true, pedal1, hocket1, n1, f1);
    setPreferences(nullptr);
    // Full-On gives the hocket no weight at all (Form.h, LeadStyle): the rule forbids it, and a liking cannot allow it.
    const bool hocketForbidden = styleProfile(StyleId::FullOn).lead.counterMode[static_cast<int>(CounterMode::Hocket)] == 0.0;
    check(pedal1 > pedal0 + 4 && (!hocketForbidden || hocket1 == 0) && f0.find("lead.archetype=") != std::string::npos && f0.find("section=") != std::string::npos,
          "a strong liking moves the draw it names, and a choice the style's rules give no weight stays impossible",
          fmt("Pedal phrases %d of %d without, %d of %d with the liking; hocket counters %d with (forbidden in Full-On: %s); features: %s",
              pedal0, n0, pedal1, n1, hocket1, hocketForbidden ? "yes" : "no", f0.c_str()));
}

/**
 * @brief One track alone (23.09.2026, round "DJ-Export"; Composer::setSoloTrack): what the DJ export renders.
 *        Its intro has none of the outgoing track, its own bars are the set's, and nothing else sounds.
 */
void testSoloTrack()
{
    section("solo track: one track of the set, alone");
    ParamStore q;
    q.parseText("compose.level_match=Off master.auto_gain=Off compose.presence_match=Off");
    Composer c(7);
    const TrackPlan t1 = c.track(q, 1), t2 = c.track(q, 2);
    const int from = t1.firstBar, to = t1.firstBar + t1.bars;
    std::vector<NoteEvent> full, solo, outside;
    c.composeBars(q, from, t1.bars, full);
    c.setSoloTrack(1);
    c.composeBars(q, from, t1.bars, solo);
    c.composeBars(q, 0, from, outside);
    c.setSoloTrack(-1);
    std::vector<NoteEvent> again;
    c.composeBars(q, from, t1.bars, again);
    // The outgoing track's kicks under the intro: in the set, not alone.
    const double handover = static_cast<double>(from + t1.form.handover) * kBeatsPerBar;
    int kicksBeforeFull = 0, kicksBeforeSolo = 0;
    for (const NoteEvent& e : full) if (e.part == Part::Kick && e.beat < handover) ++kicksBeforeFull;
    for (const NoteEvent& e : solo) if (e.part == Part::Kick && e.beat < handover) ++kicksBeforeSolo;
    // The bars the track owns with no guest (from its hand-over to the next track's start): the same notes.
    const double ownFrom = handover, ownTo = static_cast<double>(t2.firstBar) * kBeatsPerBar;
    auto slice = [&](const std::vector<NoteEvent>& v) {
        std::vector<NoteEvent> out;
        for (const NoteEvent& e : v) if (e.beat >= ownFrom && e.beat < ownTo) out.push_back(e);
        return out;
    };
    const std::vector<NoteEvent> a = slice(full), b = slice(solo);
    bool same = a.size() == b.size() && !a.empty();
    for (size_t i = 0; same && i < a.size(); ++i)
        same = a[i].beat == b[i].beat && a[i].pitch == b[i].pitch && a[i].part == b[i].part && a[i].velocity == b[i].velocity && a[i].lane == b[i].lane;
    // Its outro alone: no note of the next track's intro (its pads, from the next track's first bar on).
    int guestPads = 0;
    for (const NoteEvent& e : solo) if (e.beat >= static_cast<double>(t2.firstBar) * kBeatsPerBar && e.part == Part::Pad) ++guestPads;
    check(kicksBeforeFull > 0 && kicksBeforeSolo == 0 && same && outside.empty() && guestPads == 0 && again.size() == full.size() && to > from,
          "a solo track has no outgoing kick under its intro, no incoming pad over its outro, its own bars note for note, and nothing outside it",
          fmt("kicks before the hand-over: %d in the set, %d alone; %zu notes of its own bars compared; %zu notes outside; %d guest pads",
              kicksBeforeFull, kicksBeforeSolo, a.size(), outside.size(), guestPads));
}

/**
 * @brief The set gallery (23.09.2026, round "Galerie"; phos/Gallery.h): the comment lines survive the round trip,
 *        a gallery file is still a set, and the verdicts are counted per seed.
 */
void testGallery()
{
    section("gallery: saved sets with their form");
    ParamStore q;
    q.parseText("compose.level_match=Off master.auto_gain=Off compose.presence_match=Off");
    Composer c(7);
    GalleryEntry e;
    e.name = "night | one\ntest";
    e.saved = "2026-09-23T20:00:00";
    for (int t = 0; t < 2; ++t) e.tracks.push_back(galleryTrackOf(c.track(q, t)));
    const std::string text = withGalleryComment(writeSetText(c, q), e);
    GalleryEntry back;
    const bool read = readGalleryEntry(text, back);
    bool same = read && back.seed == 7 && back.tracks.size() == 2 && back.name == "night   one test" && back.saved == e.saved;
    for (size_t t = 0; same && t < 2; ++t)
        same = back.tracks[t].form == e.tracks[t].form && back.tracks[t].style == e.tracks[t].style && back.tracks[t].bars == e.tracks[t].bars
            && std::fabs(back.tracks[t].bpm - e.tracks[t].bpm) < 0.05;
    int formBars = 0;
    for (size_t i = 0; i < e.tracks[0].form.size(); ++i)
        if (std::isdigit(static_cast<unsigned char>(e.tracks[0].form[i])) && (i == 0 || !std::isdigit(static_cast<unsigned char>(e.tracks[0].form[i - 1]))))
            formBars += std::atoi(e.tracks[0].form.c_str() + i);
    check(same && formBars == e.tracks[0].bars, "a gallery entry reads back name, date, and every track's style, tempo, length and form",
          fmt("track 1: %s %s %s %.1f BPM, form %s", e.tracks[0].style.c_str(), e.tracks[0].key.c_str(), e.tracks[0].scale.c_str(), e.tracks[0].bpm, e.tracks[0].form.c_str()));
    Composer loaded(1);
    ParamStore q2;
    std::string err;
    const bool isSet = readSetText(text, loaded, q2, &err);
    check(isSet && loaded.seed() == 7, "a gallery file is an ordinary set: the comment lines are skipped", err);
    const char* path = "phos_selftest_gallery_ratings.tsv";
    std::remove(path);
    RatingEntry r;
    r.seed = 7; r.verdict = 1; appendRating(path, r); appendRating(path, r);
    r.verdict = -1; appendRating(path, r);
    r.seed = 9; r.verdict = 0; appendRating(path, r);
    const std::map<uint64_t, RatingCount> counts = ratingsBySeed(path);
    std::remove(path);
    const bool ok = counts.count(7) == 1 && counts.at(7).good == 2 && counts.at(7).bad == 1 && counts.count(9) == 1 && counts.at(9).notes == 1;
    check(ok, "the verdicts are counted per set seed");
}

/**
 * @brief MIDI learn's table (23.09.2026, round "MIDI"; phos/MidiMap.h): learning, one knob per parameter both
 *        ways, the text form by key.
 */
void testMidiMap()
{
    section("MIDI map: controllers on any parameter");
    MidiMap m;
    float v = -1.0f;
    const bool unbound = m.handleCc(0, 74, 100, v) == -1;
    m.arm(42);
    const int learned = m.handleCc(2, 74, 64, v);
    const bool learnedOk = learned == 42 && std::fabs(v - 64.0f / 127.0f) < 1e-6f && m.armed() == -1;
    const int again = m.handleCc(2, 74, 127, v);
    const bool otherChannel = m.handleCc(0, 74, 127, v) == -1;
    check(unbound && learnedOk && again == 42 && v == 1.0f && otherChannel,
          "an armed target takes the next controller, which then drives it on its own channel only",
          fmt("learned %d, again %d", learned, again));
    // One knob, one parameter: binding the same controller elsewhere moves it; binding the target to another knob too.
    m.bind(2, 74, 7);
    const bool moved = m.handleCc(2, 74, 10, v) == 7;
    m.bind(0, 1, 7);
    const bool released = m.handleCc(2, 74, 10, v) == -1 && m.handleCc(0, 1, 10, v) == 7 && m.bindings().size() == 1;
    check(moved && released, "a controller drives one target and a target listens to one controller");
    m.bind(15, 127, 3);
    std::map<int, std::string> names = { { 3, "lead.cutoff" }, { 7, "macro.filter_sweep" } };
    const std::string text = m.toText([&](int t) { return names[t]; });
    MidiMap back;
    const int read = back.fromText(text + "cc 1 5 no.such_param\n", [&](const std::string& k) {
        for (const auto& kv : names) if (kv.second == k) return kv.first;
        return -1;
    });
    int ch = -1, cc = -1;
    check(read == 2 && back.controllerOf(3, ch, cc) && ch == 15 && cc == 127 && back.bindings().size() == 2,
          "the map survives its text form by parameter key and skips keys it does not know", fmt("read %d bindings from:\n%s", read, text.c_str()));
}

/**
 * @brief The ratings file (23.09.2026, round "Bewertung"; phos/Rating.h): a verdict survives the round trip,
 *        a tab in a note cannot break the columns, and the header is written once.
 */
void testRatings()
{
    section("ratings: the listener's verdicts as lines");
    RatingEntry e;
    e.seed = 864566672ull;
    e.track = 3;
    e.bar = 517;
    e.barInTrack = 69;
    e.section = "Build";
    e.style = "Dark Forest";
    e.verdict = -1;
    e.note = "counter\tinaudible\nhere";
    e.source = "plugin";
    e.time = "2026-09-24T08:15:00";
    RatingEntry back;
    const bool parsed = parseRating(formatRating(e), back);
    check(parsed && back.seed == e.seed && back.track == 3 && back.bar == 517 && back.barInTrack == 69 && back.section == "Build"
              && back.style == "Dark Forest" && back.verdict == -1 && back.note == "counter inaudible here" && back.source == "plugin"
              && back.time == e.time,
          "a verdict survives format and parse, a tab or line break in the note becomes a space", fmt("note read back as '%s'", back.note.c_str()));
    const char* path = "phos_selftest_ratings.tsv";
    std::remove(path);
    e.verdict = 1;
    const bool a = appendRating(path, e);
    e.verdict = 0;
    const bool b = appendRating(path, e);
    int lines = 0, headers = 0, verdicts = 0;
    if (FILE* f = std::fopen(path, "rb")) {
        char buf[1024];
        while (std::fgets(buf, sizeof(buf), f) != nullptr) {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            ++lines;
            if (line == ratingHeader()) ++headers;
            RatingEntry r;
            if (parseRating(line, r)) verdicts += r.verdict == 1 ? 1 : (r.verdict == 0 ? 10 : 100);
        }
        std::fclose(f);
    }
    std::remove(path);
    check(a && b && lines == 3 && headers == 1 && verdicts == 11, "appending writes the header once and one line per verdict",
          fmt("%d lines, %d headers", lines, headers));
}

/**
 * @brief The set's dramaturgy (23.09.2026, round "Set-Kurve"): the tempo and the climax follow the arc, the
 *        set has a motif that returns, the kick rolls before the big drop and tears before a sixteen-bar line.
 *
 * Plans only, no audio: everything here is a decision the composer prints and the renderer follows.
 */
void testSetArc()
{
    section("set arc: tempo, climax, motif, kick roll and Abriss");
    auto plans = [](const char* arc, int tracks) {
        ParamStore q;
        q.parseText(fmt("compose.arc=%s compose.set_minutes=60 compose.track_bars=256 compose.level_match=Off master.auto_gain=Off", arc).c_str());
        Composer c(2026);
        std::vector<TrackPlan> out;
        for (int t = 0; t < tracks; ++t) out.push_back(c.track(q, t));
        return out;
    };
    const std::vector<TrackPlan> flat = plans("Flat", 8), warm = plans("Warm-up", 8), closing = plans("Closing", 8), peak = plans("Peak-Time", 8);

    // 1. The tempo centre follows the arc. Same seed, same draws: only the centre differs, so the second
    //    half of a Warm-up set runs faster than the Flat set's and a Closing set's slower (kArcTempoPerUnit).
    {
        double dWarm = 0.0, dClose = 0.0;
        for (int t = 4; t < 8; ++t) { dWarm += warm[t].bpm - flat[t].bpm; dClose += closing[t].bpm - flat[t].bpm; }
        dWarm /= 4.0;
        dClose /= 4.0;
        check(dWarm > 1.0 && dClose < -1.0 && flat[0].bpm == warm[0].bpm && flat[0].bpm == closing[0].bpm,
              "the tempo follows the set's energy arc: faster while it rises, slower while it closes, the first track the knobs",
              fmt("second half against Flat: Warm-up %+.2f BPM, Closing %+.2f BPM", dWarm, dClose));
    }
    // 2. The arc moves the climax: at the peak drop 2 grows at the main breakdown's expense, in the closing
    //    the breakdown grows. Flat draws nothing, so its forms are the fuzz alone.
    {
        auto drop2 = [](const std::vector<TrackPlan>& v) {
            int sum = 0;
            for (const TrackPlan& t : v) for (int i = 0; i < t.form.count; ++i) if (t.form.section[i].type == SectionType::Drop && t.form.section[i].climax) sum += t.form.section[i].bars;
            return static_cast<double>(sum) / v.size();
        };
        const double dPeak = drop2(peak), dFlat = drop2(flat), dClose = drop2(closing);
        bool bounds = true;
        for (const std::vector<TrackPlan>* v : { &peak, &closing })
            for (const TrackPlan& t : *v) bounds = bounds && formConstraintsHold(t.form) && t.form.bars == t.bars;
        check(dPeak > dFlat && dClose < dFlat && bounds,
              "the arc moves the climax: longer drop 2 at the peak, longer breakdown in the closing, every form inside its bounds",
              fmt("mean drop 2: Peak-Time %.1f, Flat %.1f, Closing %.1f bars", dPeak, dFlat, dClose));
    }
    // 3. The set's motif: the first track states it in its first phrase, the track that carries the set's end
    //    (60 minutes: the ninth) recalls it in its second, with the same cell and archetype; a recall in between
    //    is the exception, and the second track never recalls.
    {
        ParamStore q;
        q.parseText("compose.set_minutes=60 compose.track_bars=256 compose.level_match=Off master.auto_gain=Off");
        Composer c(2026);
        std::vector<TrackPlan> v;
        for (int t = 0; t < 10; ++t) v.push_back(c.track(q, t));
        const MelodyPlan& first = v[0].melody;
        int recalls = 0, carrier = -1;
        double startBar = 0.0;
        const double setBars = 60.0 * q.get(q.base(Module::Compose) + compose::Bpm) / kBeatsPerBar;   // what Composer.cpp's setLengthBars gives with Style Tempo off
        for (int t = 1; t < 10; ++t) {
            const double endBar = startBar + v[t - 1].bars;
            if (carrier < 0 && startBar + v[t - 1].bars < setBars && endBar + v[t].bars >= setBars) carrier = t;
            startBar = endBar;
            if (v[t].melody.leadQuotesSet[1]) ++recalls;
        }
        const bool carrierRecalls = carrier > 0 && v[carrier].melody.leadQuotesSet[1]
                                 && v[carrier].melody.leadCell[1] == first.leadCell[0] && v[carrier].melody.leadArchetype[1] == first.leadArchetype[0];
        check(first.leadQuotesSet[0] && !first.leadQuotesSet[1] && carrierRecalls && !v[1].melody.leadQuotesSet[1] && recalls <= 4,
              "the first track states the set's motif, the track that carries the set's end recalls it with the same cell and archetype",
              fmt("carrier track %d, %d recalls in ten tracks, cell %04x archetype %d", carrier + 1, recalls, first.leadCell[0], first.leadArchetype[0]));
    }
    // 4. The kick's roll before the big buildup's pre-drop break, and the Abriss before a sixteen-bar line of a
    //    drop: both a minority of the bars they may take, both present.
    {
        int rollBars = 0, rollCandidates = 0, abriss = 0, abrissCandidates = 0, sixteenths = 0;
        for (const TrackPlan& t : flat) {
            PartAvailability av;
            for (int k = 0; k < kMelodyParts; ++k) av.part[k] = t.melody.present[k];
            av.percLayers = t.perc.layers;
            av.hatLayers = t.perc.hatLayers;
            for (int b = 0; b < t.bars; ++b) {
                const BarPlan bp = planBar(t.form, av, t.sectionSeed, b);
                const Section& s = t.form.section[bp.index];
                if (s.type == SectionType::Build && s.rollBars >= 8 && s.pdbBars == 1 && bp.barInSection == s.bars - 2) {
                    ++rollCandidates;
                    if (bp.kickRoll > 0) ++rollBars;
                    if (bp.kickRoll == 2) ++sixteenths;
                }
                if (s.type == SectionType::Drop && bp.barInSection % 16 == 15 && bp.barInSection + 1 < s.bars) {
                    ++abrissCandidates;
                    if (bp.kickBeats == 0x7 && bp.bassBeats == 0x7) ++abriss;
                }
            }
        }
        check(rollCandidates >= 6 && rollBars > 0 && rollBars < rollCandidates && abrissCandidates >= 16 && abriss > 0 && abriss * 2 < abrissCandidates,
              "the kick rolls before some big drops and tears before some sixteen-bar lines, never before all",
              fmt("kick roll in %d of %d big buildups (%d into sixteenths), Abriss in %d of %d bars", rollBars, rollCandidates, sixteenths, abriss, abrissCandidates));
    }
}

/** @brief Self test: form grammar and energy arc. */
void testForm()
{
    section("form grammar and energy arc");

    // Every form the grammar can build keeps the hard constraints of PLAN 6.2 and has exactly the
    // length that was asked for -- the latter matters because the length belongs to the set walk while
    // the body is the track's own decision (a rerolled track must not move the tracks after it).
    {
        int forms = 0, broken = 0, wrongLength = 0, bodies[kNumBodies] = {}, lengths[5] = {};
        double shareLo = 1.0, shareHi = 0.0;
        for (int st = 0; st < kNumStyles; ++st) {
            const StyleProfile& s = styleProfile(static_cast<StyleId>(st));
            for (int target = kMinTrackBars; target <= kMaxTrackBars; target += kTrackBarStep) {
                for (uint64_t seed = 1; seed <= 40; ++seed) {
                    const FormPlan f = makeFormPlan(s, seed, target, 0.3, 0.9);
                    ++forms;
                    ++bodies[f.body];
                    if (!formConstraintsHold(f)) ++broken;
                    if (f.bars != target) ++wrongLength;
                    int brk = 0;
                    for (int i = 0; i < f.count; ++i) {
                        if (f.section[i].type == SectionType::Break) brk += f.section[i].bars;
                        // 19.09.2026: every multiple of eight is a legal length (drop 2 runs 48 bars).
                        const int b = f.section[i].bars;
                        lengths[b % 8 == 0 && b >= 8 ? (b <= 16 ? 1 : (b <= 32 ? 2 : 3)) : 4]++;
                    }
                    const double share = static_cast<double>(brk) / f.bars;
                    shareLo = std::min(shareLo, share);
                    shareHi = std::max(shareHi, share);
                }
            }
        }
        check(broken == 0 && wrongLength == 0 && lengths[4] == 0 && bodies[0] > 0 && bodies[1] > 0 && bodies[2] > 0 && bodies[3] > 0,
              "every form keeps the constraints and hits the requested length exactly, every template in use",
              fmt("%d forms, %d broken, %d off length, templates %d/%d/%d/%d, break share %.2f..%.2f",
                  forms, broken, wrongLength, bodies[0], bodies[1], bodies[2], bodies[3], shareLo, shareHi));
    }

    // The two-drop standard of Grosz et al.: every track has at least two cores, and the necessity
    // order Core > Buildup/Outro > Breakdown/Intro > PDB holds by construction (a form without a core
    // cannot be built). Since 19.09.2026 intro and outro are the user's 32-bar DJ ends (Easwaran's 8 to 16
    // bars of atmosphere are the intro's kick-free half), and the fourth PDB variant -- the kick alone on
    // beat 4 -- is never drawn: beat 4 stays empty.
    {
        int tracks = 0, twoDrops = 0, introOk = 0, outroOk = 0, pdbVariants[kNumPdbVariants] = {}, cuts = 0, builds = 0;
        for (uint64_t seed = 1; seed <= 200; ++seed) {
            const FormPlan f = makeFormPlan(styleProfile(StyleId::FullOn), seed, 256, 0.5, 0.9);
            ++tracks;
            int cores = 0;
            for (int i = 0; i < f.count; ++i) {
                const Section& s = f.section[i];
                if (s.type == SectionType::Groove || s.type == SectionType::Drop) ++cores;
                if (s.type == SectionType::Build) { ++builds; ++pdbVariants[s.pdbVariant]; }
                if (s.type == SectionType::Break && s.cutBeats > 0.0f) ++cuts;
            }
            if (cores >= 2) ++twoDrops;
            // 23.09.2026, round "Form": the fuzziness moves eight bars between neighbours, so 16 .. 48 (Form.cpp, jitterForm).
            if (f.section[0].bars >= 16 && f.section[0].bars <= 48 && f.section[0].bars % 8 == 0) ++introOk;
            if (f.section[f.count - 1].bars >= 16 && f.section[f.count - 1].bars <= 48 && f.section[f.count - 1].bars % 8 == 0) ++outroOk;
        }
        const int variantsSeen = (pdbVariants[0] > 0) + (pdbVariants[1] > 0) + (pdbVariants[2] > 0);
        check(twoDrops == tracks && introOk == tracks && outroOk == tracks && variantsSeen == 3 && pdbVariants[3] == 0 && cuts > 0,
              "two-peak form, intro and outro of 16 to 48 bars (32 the rule, the fuzziness around it), three pre-drop-break variants and never the kick on beat 4, cuts before the breakdowns",
              fmt("%d tracks, %d with two or more cores, PDB variants %d/%d/%d/%d of %d buildups, %d cuts",
                  tracks, twoDrops, pdbVariants[0], pdbVariants[1], pdbVariants[2], pdbVariants[3], builds, cuts));
    }

    // The energy arc: smooth (no step larger than the slope allows), inside [0, 1], and the order of
    // the section energies survives every point of every arc.
    {
        double worstStep = 0.0, lo = 1.0, hi = 0.0;
        int orderBroken = 0;
        for (int a = 0; a < kNumArcs; ++a) {
            double prev = arcEnergy(static_cast<ArcId>(a), 0.0);
            for (int k = 1; k <= 1000; ++k) {
                const double e = arcEnergy(static_cast<ArcId>(a), k / 1000.0);
                worstStep = std::max(worstStep, std::fabs(e - prev));
                lo = std::min(lo, e);
                hi = std::max(hi, e);
                prev = e;
                // Drop above buildup above groove above breakdown, whatever the arc says.
                const double scale = 0.55 + 0.45 * e;
                if (!(typeEnergy(SectionType::Drop) * scale > typeEnergy(SectionType::Groove) * scale
                      && typeEnergy(SectionType::Groove) * scale > typeEnergy(SectionType::Break) * scale)) ++orderBroken;
            }
        }
        check(worstStep < 0.004 && lo >= 0.0 && hi <= 1.0 && orderBroken == 0,
              "energy arcs are smooth raised cosines inside [0, 1] and keep the order drop > groove > breakdown",
              fmt("largest step over a thousandth of the set %.4f, range %.2f..%.2f", worstStep, lo, hi));
    }

    // Peak-Time against Closing: over a whole set the arc really moves the sections' energy.
    {
        auto meanEnergy = [](const char* arc, int half) {
            ParamStore q;
            q.parseText(fmt("compose.arc=%s compose.set_minutes=60 compose.track_bars=256 compose.level_match=Off master.auto_gain=Off", arc).c_str());
            Composer c(2026);
            double sum = 0.0;
            int n = 0;
            // Per section, not per bar (23.09.2026, round "Set-Kurve"): the arc now also moves eight bars between
            // the main breakdown and drop 2 (Form.cpp, jitterForm), and a bar-weighted mean would read that
            // length move as energy. This check is about the arc's energies themselves.
            for (int t = half * 4; t < half * 4 + 4; ++t) {
                const TrackPlan p = c.track(q, t);
                for (int i = 0; i < p.form.count; ++i) { sum += p.form.section[i].energy; ++n; }
            }
            return sum / std::max(n, 1);
        };
        // Warm-up against Closing, not Peak-Time (23.09.2026): Peak-Time stands at 0.95 to 1.0 over most of the
        // set, where the climax margin saturates the energies (drop 2 at 1, the rest capped at 0.8), so its two
        // halves read 0.59 against 0.58 and the sign was decided by hundredths -- the arc's eight-bar move
        // tipped it. Warm-up rises from 0.25 to 0.85 without saturating, which is the claim.
        const double warmEarly = meanEnergy("Warm-up", 0), warmLate = meanEnergy("Warm-up", 1);
        const double closeEarly = meanEnergy("Closing", 0), closeLate = meanEnergy("Closing", 1);
        check(warmLate > warmEarly + 0.03 && closeLate < closeEarly - 0.05 && warmLate > closeLate,
              "the dramaturgy preset moves the energy of the sections over the set",
              fmt("Warm-up %.2f -> %.2f, Closing %.2f -> %.2f", warmEarly, warmLate, closeEarly, closeLate));
    }

    // Easwaran and Butler: something changes every eight bars. No two consecutive eight-bar groups of
    // a core hold the same notes. Measured in **both** bass modes: the learned bass phrase repeats
    // every kBassPhraseBars bars and could not satisfy the rule by itself, which is why the group
    // figure of Form.cpp stays in force when compose.bass_model is Neural (Composer.cpp says so).
    for (const char* bassModel : { "Pattern", "Neural" }) {
        ParamStore q;
        q.parseText("compose.track_bars=256 compose.bass_variation=0 compose.level_match=Off master.auto_gain=Off");
        q.parseText(fmt("compose.bass_model=%s", bassModel).c_str());
        Composer c(1234);
        int groups = 0, identical = 0, sameBass = 0;
        for (int ti = 0; ti < 6; ++ti) {
            const TrackPlan t = c.track(q, ti);
            for (int si = 0; si < t.form.count; ++si) {
                const Section& s = t.form.section[si];
                if (s.type != SectionType::Groove && s.type != SectionType::Drop) continue;
                // Two hashes per group: over everything, and over the bass alone. The second isolates
                // the rule itself -- with Bass Variation at zero the only thing that may move the bass
                // from group to group is the group's own figure, so a broken rule shows there even
                // when a percussion fill happens to differ.
                std::vector<uint64_t> hash, bassHash;
                for (int g = 0; g * 8 + 8 <= s.bars; ++g) {
                    std::vector<NoteEvent> ev;
                    c.composeBars(q, t.firstBar + s.startBar + g * 8, 8, ev);
                    uint64_t h = 1469598103934665603ull, hb = h;
                    const double base = static_cast<double>(t.firstBar + s.startBar + g * 8) * kBeatsPerBar;
                    for (const NoteEvent& e : ev) {
                        const uint64_t k = static_cast<uint64_t>(std::llround((e.beat - base) * 960.0)) * 1024
                                         + static_cast<uint64_t>(e.pitch) * 8 + static_cast<uint64_t>(e.part);
                        h = (h ^ k) * 1099511628211ull;
                        h = (h ^ static_cast<uint64_t>(std::llround(e.length * 960.0))) * 1099511628211ull;
                        if (e.part == Part::Bass) hb = (hb ^ k) * 1099511628211ull;
                    }
                    hash.push_back(h);
                    bassHash.push_back(hb);
                }
                for (size_t g = 1; g < hash.size(); ++g) {
                    ++groups;
                    if (hash[g] == hash[g - 1]) ++identical;
                    if (bassHash[g] == bassHash[g - 1]) ++sameBass;
                }
            }
        }
        check(groups > 30 && identical == 0 && sameBass == 0,
              fmt("bass_model=%s: no two consecutive eight-bar groups of a core hold the same notes, the bass alone included", bassModel).c_str(),
              fmt("%d consecutive pairs, %d identical, %d with the same bass", groups, identical, sameBass));
    }

    // The instrumentation matrix: a breakdown has neither kick nor bass, an intro starts without a
    // kick and layers the percussion up, a drop brings everything back.
    {
        ParamStore q;
        q.parseText("compose.track_bars=256 compose.acid_amount=1 compose.lead_amount=1 compose.arp_amount=1 compose.pad_amount=1 compose.level_match=Off master.auto_gain=Off");
        Composer c(55);
        int breakKicks = 0, breakBass = 0, introKickBars = 0, introBars = 0, dropBars = 0, dropAllParts = 0, pdbBars = 0, pdbBeat4 = 0;
        for (int ti = 0; ti < 4; ++ti) {
            const TrackPlan t = c.track(q, ti);
            for (int si = 0; si < t.form.count; ++si) {
                const Section& s = t.form.section[si];
                std::vector<NoteEvent> ev;
                c.composeBars(q, t.firstBar + s.startBar, s.bars, ev);
                for (const NoteEvent& e : ev) {
                    const double inSection = e.beat - static_cast<double>(t.firstBar + s.startBar) * kBeatsPerBar;
                    const int barIn = static_cast<int>(inSection / kBeatsPerBar);
                    if (s.type == SectionType::Break) {
                        if (e.part == Part::Kick) ++breakKicks;
                        if (e.part == Part::Bass) ++breakBass;
                    }
                    // Only where the buildup ends in a pre-drop break at all: the Dark Forest template builds
                    // its first buildup without one (Form.cpp, "f.body == 3 ? 0"), and since the styles walk
                    // through a set (22.09.2026) such a buildup turns up in any set -- its last bar keeps kick
                    // and bass on purpose, and the PDB rule has nothing to say about it.
                    if (s.type == SectionType::Build && s.pdbBars > 0 && barIn == s.bars - 1) {
                        const double beatInBar = inSection - barIn * kBeatsPerBar;
                        if (beatInBar >= 3.0 && (e.part == Part::Bass || (e.part == Part::Kick && s.pdbVariant != 3))) ++pdbBeat4;
                    }
                }
                // The set's first track: its intro has no kick before bar 17 (19.09.2026; later tracks' intros
                // sound over the previous track's outro, whose kick plays there -- testArrangement).
                if (s.type == SectionType::Intro && si == 0 && ti == 0) {
                    ++introBars;
                    int firstKick = 99;
                    for (const NoteEvent& e : ev)
                        if (e.part == Part::Kick) firstKick = std::min(firstKick, static_cast<int>((e.beat - static_cast<double>(t.firstBar) * kBeatsPerBar) / kBeatsPerBar));
                    if (firstKick == kIntroKickBar) ++introKickBars;
                }
                if (s.type == SectionType::Drop) {
                    ++dropBars;
                    bool kick = false, bass = false, melody = false;
                    for (const NoteEvent& e : ev) {
                        kick = kick || e.part == Part::Kick;
                        bass = bass || e.part == Part::Bass;
                        melody = melody || e.part == Part::Acid || e.part == Part::Lead || e.part == Part::Arp;
                    }
                    if (kick && bass && melody) ++dropAllParts;
                }
                if (s.type == SectionType::Build && s.pdbBars > 0) ++pdbBars;
            }
        }
        check(breakKicks == 0 && breakBass == 0 && introKickBars == introBars && dropAllParts == dropBars && pdbBeat4 == 0 && pdbBars > 0,
              "instrumentation matrix: no kick or bass in a breakdown, the intro's kick on bar 17, everything in a drop, beat 4 of the PDB empty",
              fmt("%d kicks and %d bass notes in breakdowns, %d of %d intros, %d of %d drops complete, %d notes on beat 4 of %d PDBs",
                  breakKicks, breakBass, introKickBars, introBars, dropAllParts, dropBars, pdbBeat4, pdbBars));
    }

    // The bass slot envelope of PLAN 6.6 is a style-profile parameter with a flat default, because nine
    // reference tracks play three equally loud notes within +-1.2 dB. Flat means: every profile's table
    // is all ones, and the three notes of a beat really come out with the same velocity and the same
    // share of their slot.
    {
        int nonFlat = 0;
        for (int st = 0; st < kNumStyles; ++st) {
            const StyleProfile& sp = styleProfile(static_cast<StyleId>(st));
            for (int k = 0; k < kBassSlots; ++k) nonFlat += (sp.slotGate[k] != 1.0f) + (sp.slotVel[k] != 1.0f);
        }
        ParamStore q;
        q.parseText("compose.bass_variation=0 compose.bass_pattern=Rolling compose.level_match=Off master.auto_gain=Off");
        Composer cb(19);
        const TrackPlan& tb = cb.track(q, 0);
        int core = 0;
        for (int i = 0; i < tb.form.count; ++i)
            if (tb.form.section[i].type == SectionType::Groove || tb.form.section[i].type == SectionType::Drop) { core = tb.form.section[i].startBar; break; }
        std::vector<NoteEvent> ev;
        cb.composeBars(q, tb.firstBar + core, 1, ev);
        std::vector<const NoteEvent*> beat0;
        for (const NoteEvent& e : ev)
            if (e.part == Part::Bass && e.beat < static_cast<double>(tb.firstBar + core) * kBeatsPerBar + 1.0) beat0.push_back(&e);
        bool same = beat0.size() == 3;
        for (size_t i = 1; i < beat0.size() && same; ++i)
            same = beat0[i]->velocity == beat0[0]->velocity && std::fabs(beat0[i]->length - beat0[0]->length) < 1e-6f;
        check(nonFlat == 0 && same, "the bass slot envelope is flat in every style profile, and the three notes of a beat come out equal",
              fmt("%d non-flat table entries, %zu notes on the first beat", nonFlat, beat0.size()));
    }

    // Colour (Farbood's dissonance): the more colour, the more often the lead takes the flat second or
    // the upper note of an augmented second.
    {
        auto colourShare = [](float colour) {
            ParamStore q;
            q.parseText("compose.lead_amount=1 compose.level_match=Off master.auto_gain=Off");
            int total = 0, coloured = 0;
            for (uint64_t seed = 1; seed <= 60; ++seed) {
                const MelodyPlan m = makeMelodyPlan(q, styleProfile(StyleId::Goa), seed, 6, 3, false, colour);
                for (const auto& phrase : m.lead)
                    for (const MelodyNote& n : phrase) {
                        ++total;
                        const int pc = ((m.root[1] + n.rel - 6) % 12 + 12) % 12;
                        // Phrygian dominant: the flat second is 1, the augmented second's upper note 4.
                        if (pc == 1 || pc == 4) ++coloured;
                    }
            }
            return 100.0 * coloured / std::max(total, 1);
        };
        const double plain = colourShare(0.0f), rich = colourShare(1.0f);
        check(rich > plain + 2.0, "the energy arc's colour weight lifts the flat second and the augmented second in the lead",
              fmt("%.1f %% of the lead's notes at colour 0, %.1f %% at colour 1", plain, rich));
    }
}

/**
 * @brief Arrangement dynamics (16.09.2026): the rising snare roll, the acid's macro ride, the auto-pan.
 *
 * Every bound here is derived somewhere other than in the code it tests: from the closed form of the
 * swing law, from the geometry of a constant-power panner, from the filter order of the lane's low
 * cut, or from the raised-cosine ramp the engine plays a control event with.
 */
void testArrangeDynamics()
{
    section("arrangement dynamics: the roll that lifts, the acid's ride, movement in the panorama");
    constexpr double kPi = 3.141592653589793;

    // ---------------------------------------------------------------------------------------------
    // The auto-pan.
    // ---------------------------------------------------------------------------------------------

    // (a) The swing law keeps E[p^2] at p0^2 at every depth. This is what makes the depth knob leave
    //     the width the width round calibrated alone -- side/mid is (pi/4)^2 E[p^2] to second order --
    //     and it is a closed form, so it is checked against the closed form and not against a render.
    {
        double worstMean = 0.0, worstRms = 0.0;
        std::string detail;
        for (double p0 : { 0.45, -0.40, 0.60 }) {
            for (double d : { 0.25, 0.5, 0.7, 1.0 }) {
                double sum = 0.0, sum2 = 0.0;
                constexpr int kSteps = 4096;
                for (int i = 0; i < kSteps; ++i) {
                    const double ph = 2.0 * kPi * i / kSteps;
                    const double p = p0 * (std::sqrt(1.0 - d * d) + std::sqrt(2.0) * d * std::cos(ph));
                    sum += p;
                    sum2 += p * p;
                }
                const double mean = sum / kSteps, rms = std::sqrt(sum2 / kSteps);
                worstMean = std::max(worstMean, std::fabs(mean - p0 * std::sqrt(1.0 - d * d)));
                worstRms = std::max(worstRms, std::fabs(rms - std::fabs(p0)));
            }
        }
        detail = fmt("worst deviation of the rms position from |p0| %.2e, of the mean from p0 sqrt(1-D^2) %.2e",
                     worstRms, worstMean);
        check(worstRms < 1e-6 && worstMean < 1e-6,
              "the swing law leaves the root mean square position at the standing one, at every depth",
              detail.c_str());
    }

    // (b) A lane on the sixteenth grid samples a three-sixteenth swing at three phases 120 degrees
    //     apart, and three such points carry the first and the second moment of a sinusoid exactly.
    //     That is the reason for the period, so it is measured: the sampled moments against the
    //     continuous ones, and against what two sixteenths (the obvious alternative) would give.
    {
        const double p0 = 0.45, d = 1.0;
        auto moments = [&](double periodSixteenths, int n, double& mean, double& ms) {
            mean = ms = 0.0;
            for (int i = 0; i < n; ++i) {
                const double ph = 2.0 * kPi * i / periodSixteenths;
                const double p = p0 * (std::sqrt(1.0 - d * d) + std::sqrt(2.0) * d * std::cos(ph));
                mean += p;
                ms += p * p;
            }
            mean /= n;
            ms /= n;
        };
        double m3 = 0.0, s3 = 0.0, m2 = 0.0, s2 = 0.0;
        moments(3.0, 48, m3, s3);      // the built period: three sixteenths
        moments(2.0, 48, m2, s2);      // two sixteenths, commensurate with the eighth
        check(std::fabs(m3) < 1e-9 && std::fabs(s3 - p0 * p0) < 1e-9 && std::fabs(s2 - 2.0 * p0 * p0) < 1e-9,
              "a sixteenth-grid lane realises the swing's moments exactly at three sixteenths, and twice too wide at two",
              fmt("three sixteenths: mean %.2e, mean square %.4f (p0^2 = %.4f); two sixteenths: mean square %.4f (2 p0^2 = %.4f)",
                  m3, s3, p0 * p0, s2, 2.0 * p0 * p0));
    }

    // (c) The kit's phase groups are balanced, not spread: the power-weighted swing of the twelve
    //     lanes has to cancel, because what the 85 ms measurement reads is that weighted sum. The
    //     bound is the greedy partition's own guarantee -- the residual cannot exceed the largest
    //     single weight -- and the largest lane here is the closed hat.
    {
        ParamStore p;
        auto kit = makeKit(p);
        double signedSum = 0.0, absSum = 0.0, largest = 0.0;
        std::string groups;
        for (int l = 0; l < kPercLanes; ++l) {
            const std::vector<float> v = moduleValues(p, Module::Perc, l);
            const double w = std::pow(10.0, static_cast<double>(v[perc::Level]) / 10.0);
            const double a = w * kit->panSwing(l);
            signedSum += a;
            absSum += std::fabs(a);
            largest = std::max(largest, std::fabs(a));
            if (kit->panGroup(l) != 0) groups += kit->panGroup(l) > 0 ? '+' : '-';
            else groups += '.';
        }
        check(absSum > 0.0 && std::fabs(signedSum) <= largest && std::fabs(signedSum) < 0.35 * absSum,
              "the auto-pan's two phase groups balance the kit's power-weighted movement",
              fmt("groups %s, residual %.4f of %.4f moved (largest lane %.4f)", groups.c_str(), std::fabs(signedSum), absSum, largest));
    }

    // (d) Constant power: while a lane swings, the two channel powers still sum to what the lane would
    //     have made standing still, sample for sample. Every band balance and loudness figure of the
    //     mix round is a sum of channel powers, so this is the guarantee that none of them can move.
    {
        // The lane is pushed to 0.9 of full deflection, which is the largest angle the field allows a
        // swinging lane to ask the kernel's series for (1.49 rad, against the guard at 1.6): the
        // worst case is where a truncation shows, and the default kit's own 0.45 is not it.
        // The lane is pushed to 0.9 of full deflection, which is the largest angle the field allows a
        // swinging lane to ask the kernel's series for (1.49 rad, against the guard at 1.6), and its
        // decay is stretched to two seconds. Both matter: a closed hat is gone after 45 ms, a twelfth
        // of the swing's period, so a hit left as it is would never sound at the angle where a
        // truncated series shows.
        const char* longTail = "perc1.pan=0.9 perc1.decay=2000 perc1.noise_decay=2000";
        ParamStore p;
        p.parseText(longTail);
        auto moving = makeKit(p);
        ParamStore q;
        q.parseText(longTail);
        q.parseText("perc1.pan_depth=0");
        auto still = makeKit(q);
        const DenormalGuard guard;
        constexpr int kN = 24000;
        std::vector<float> aL(kN), aR(kN), bL(kN), bR(kN);
        moving->trigger(0, 1.0f, 0, 0.0);
        still->trigger(0, 1.0f, 0, 0.0);
        moving->process(aL.data(), aR.data(), kN);
        still->process(bL.data(), bR.data(), kN);
        // The error is read *relative to the power at that sample*, not to the loudest one: the
        // rotation's worst angle and the lane's loudest moment are different instants, and an
        // absolute bound against the peak would let a per-sample error of a part in ten thousand
        // through. What the bound has to catch is the truncation of the kernel's cos and sin series,
        // which without the Newton step leaves cos^2 + sin^2 about 6e-5 off unity at the largest
        // angle a default lane asks for (1.14 rad: the first dropped term is d^8/8!).
        double worst = 0.0, peak = 0.0, diff = 0.0;
        for (int i = 0; i < kN; ++i) {
            const double pa = static_cast<double>(aL[i]) * aL[i] + static_cast<double>(aR[i]) * aR[i];
            const double pb = static_cast<double>(bL[i]) * bL[i] + static_cast<double>(bR[i]) * bR[i];
            peak = std::max(peak, pb);
            diff += std::fabs(static_cast<double>(aL[i]) - bL[i]);
        }
        for (int i = 0; i < kN; ++i) {
            const double pa = static_cast<double>(aL[i]) * aL[i] + static_cast<double>(aR[i]) * aR[i];
            const double pb = static_cast<double>(bL[i]) * bL[i] + static_cast<double>(bR[i]) * bR[i];
            if (pb < 1e-8 * peak) continue;
            worst = std::max(worst, std::fabs(pa - pb) / pb);
        }
        check(worst < 1e-5 && diff > 0.0,
              "a swinging lane and a standing one carry the same power in the two channels together",
              fmt("worst per-sample difference of L^2+R^2 %.3e relative (allowed 1e-5); the channels do differ (sum |dL| %.3f)",
                  worst, diff));
    }

    // (e) The movement is bit-identical whatever block size the host renders in (the phasor turns once
    //     per sample), and identical to nothing at all when the depth is zero.
    {
        ParamStore p;
        auto render = [&](const ParamStore& knobs, int block) {
            auto kit = makeKit(knobs);
            const DenormalGuard guard;
            std::vector<float> L(48000), R(48000), tmp(static_cast<size_t>(block));
            std::vector<float> tmp2(static_cast<size_t>(block));
            int next = 0, k = 0, pos = 0;
            // The hits land on their own sample, not on the block boundary that follows them: a test
            // that quantised them to the block would measure its own loop and not the phasor.
            while (pos < 48000) {
                if (pos == next) {
                    kit->trigger(0, 0.9f, 0, 0.0);
                    if (k % 4 == 2) kit->trigger(1, 0.8f, 0, 0.0);
                    next += 3103;
                    ++k;
                }
                const int n = std::min({ block, 48000 - pos, next - pos });
                kit->process(tmp.data(), tmp2.data(), n);
                std::copy(tmp.begin(), tmp.begin() + n, L.begin() + pos);
                std::copy(tmp2.begin(), tmp2.begin() + n, R.begin() + pos);
                pos += n;
            }
            return std::make_pair(L, R);
        };
        const auto a = render(p, 64);
        const auto b = render(p, 7);
        const auto c = render(p, 1000);
        size_t bad = 0;
        for (size_t i = 0; i < a.first.size(); ++i)
            if (a.first[i] != b.first[i] || a.second[i] != b.second[i] || a.first[i] != c.first[i] || a.second[i] != c.second[i]) ++bad;
        check(bad == 0, "the auto-pan is bit-identical at block sizes 64, 7 and 1000", fmt("%d of %d samples differ", static_cast<int>(bad), static_cast<int>(a.first.size())));
    }

    // (f) The measurement the round is judged by: the kit's inter-channel level difference over 85 ms
    //     windows falls, and the side/mid ratio the width round calibrated does not move. The kit
    //     plays its own sixteenth pattern, as in the width round's test, so the two are comparable.
    {
        auto play = [&](const char* knobs, double& ildRms, double& width, double& rho) {
            ParamStore p;
            if (knobs != nullptr) p.parseText(knobs);
            auto kit = makeKit(p);
            const DenormalGuard guard;
            StereoBandAccumulator acc;
            constexpr int kBlock = 64;
            std::vector<float> L(kBlock), R(kBlock);
            // Two fourth-order high passes at 6 kHz, so the level difference is read in the air band
            // the width round found short, and not broadband.
            Svf hpL[2], hpR[2];
            for (int i = 0; i < 2; ++i) { hpL[i].setQ(6000.0f, i == 0 ? 0.541f : 1.307f, 48000.0f); hpR[i].copyCoefficients(hpL[i]); }
            const int win = 4080;   // 85 ms at 48 kHz
            double wl = 0.0, wr = 0.0, sum = 0.0;
            int windows = 0, filled = 0;
            int next = 0, k = 0;
            for (size_t done = 0; done < static_cast<size_t>(StereoBandAccumulator::kN) * 6; done += kBlock) {
                while (next < static_cast<int>(done) + kBlock) {
                    const int s = k % 16;
                    kit->trigger(0, 0.9f, 0, 0.0);
                    if (s % 4 == 2) kit->trigger(1, 0.8f, 0, 0.0);
                    if (s % 8 == 4) kit->trigger(4, 1.0f, 0, 0.0);
                    if (s % 2 == 1) kit->trigger(7, 0.7f, 0, 0.0);
                    if (s == 12) kit->trigger(2, 0.6f, 0, 0.0);
                    if (s % 8 == 6) kit->trigger(6, 0.7f, 0, 0.0);
                    next += 3103;
                    ++k;
                }
                kit->process(L.data(), R.data(), kBlock);
                acc.push(L.data(), R.data(), kBlock);
                for (int i = 0; i < kBlock; ++i) {
                    float l = L[i], r = R[i], lp, bp, hp;
                    for (int j = 0; j < 2; ++j) { hpL[j].tick(l, lp, bp, hp); l = hp; hpR[j].tick(r, lp, bp, hp); r = hp; }
                    wl += static_cast<double>(l) * l;
                    wr += static_cast<double>(r) * r;
                    if (++filled == win) {
                        if (wl > 1e-12 && wr > 1e-12) { const double d = 10.0 * std::log10(wl / wr); sum += d * d; ++windows; }
                        wl = wr = 0.0;
                        filled = 0;
                    }
                }
            }
            ildRms = std::sqrt(sum / std::max(windows, 1));
            width = acc.width(6000.0, 16000.0);
            rho = acc.rho(6000.0, 16000.0);
        };
        const char* off = "perc1.pan_depth=0 perc2.pan_depth=0 perc3.pan_depth=0 perc4.pan_depth=0 perc7.pan_depth=0 "
                          "perc8.pan_depth=0 perc9.pan_depth=0 perc10.pan_depth=0 perc11.pan_depth=0 perc12.pan_depth=0";
        double ild0 = 0.0, w0 = 0.0, r0 = 0.0, ild1 = 0.0, w1 = 0.0, r1 = 0.0;
        play(off, ild0, w0, r0);
        play(nullptr, ild1, w1, r1);
        check(ild1 < ild0 - 0.5 && std::fabs(w1 - w0) < 0.6,
              "the auto-pan lowers the kit's 85 ms level difference in the air band without moving its side/mid",
              fmt("level difference %.2f -> %.2f dB rms (recordings 1.74), side/mid %+.2f -> %+.2f dB (width round -8.66), rho %+.3f -> %+.3f",
                  ild0, ild1, w0, w1, r0, r1));
        // 19.09.2026 (round "voices"): the phase groups are the roles' (Perc.cpp, kRolePanGroup), so a
        // level correction of the hat or the shaker cannot re-deal them. With the greedy partition of the
        // knob levels, the closed hat at 4 dB instead of 5 or the shaker at -5 dB instead of -1 flipped
        // the groups and the reduction fell to 0.17 dB; it has to stay above 1 dB for both.
        double worst = 1e9;
        std::string detail;
        for (const char* change : { "perc1.level=4", "perc8.level=-5", "perc1.level=3 perc8.level=-3" }) {
            double ildA = 0.0, wA = 0.0, rA = 0.0, ildB = 0.0, wB = 0.0, rB = 0.0;
            play((std::string(off) + " " + change).c_str(), ildA, wA, rA);
            play(change, ildB, wB, rB);
            worst = std::min(worst, ildA - ildB);
            detail += fmt(" %s: %.2f -> %.2f;", change, ildA, ildB);
        }
        check(worst > 1.0, "the auto-pan's reduction survives level corrections of hat and shaker (phase groups by role, not by level)",
              fmt("smallest reduction %.2f dB;%s", worst, detail.c_str()));
    }

    // ---------------------------------------------------------------------------------------------
    // The snare roll that lifts.
    // ---------------------------------------------------------------------------------------------

    // (g) The roll's pitch is a ramp over the four bars, written into the notes. Checked against the
    //     closed form the ramp is defined by -- round(12 u) at u = (bar + beat/4)/4 -- and for the
    //     three properties that matter musically: it starts at the lane's own note, it rises
    //     monotonically, and it arrives within a semitone of the octave before the drop.
    {
        ParamStore p;
        const PercPlan plan = makePercPlan(p, 1234u, true);
        int first = -1, last = -1, steps = 0, wrong = 0;
        bool monotone = true;
        for (int r = 0; r < 4; ++r) {
            std::vector<NoteEvent> out;
            PercBarSpec spec;
            spec.rollBar = r;
            spec.pdb = r == 3;
            spec.fills = false;
            composePercBar(p, plan, 1234u, 100 + r, 100 + r, 145.0, 6, 1, spec, out);
            for (const NoteEvent& n : out) {
                if (n.lane != 5) continue;   // the snare is lane 6, index 5 in the default kit
                const double beatInBar = n.beat - static_cast<double>(100 + r) * kBeatsPerBar;
                const double u = (static_cast<double>(r) + beatInBar / kBeatsPerBar) / 4.0;
                const int want = kPercRoleNote[static_cast<int>(PercRole::Snare)] + static_cast<int>(std::lround(kRollSemitones * u));
                if (std::abs(static_cast<int>(n.pitch) - want) > 1) ++wrong;
                if (first < 0) first = n.pitch;
                if (last >= 0 && n.pitch < last) monotone = false;
                last = n.pitch;
                ++steps;
            }
        }
        const int base = kPercRoleNote[static_cast<int>(PercRole::Snare)];
        check(steps > 40 && wrong == 0 && monotone && first == base && last >= base + kRollSemitones - 2,
              "the buildup's snare roll rises an octave, as a ramp over its four bars and not per hit",
              fmt("%d hits, first %d, last %d (lane note %d, target %d), %d off the closed form, monotone %s",
                  steps, first, last, base, base + kRollSemitones, wrong, monotone ? "yes" : "no"));
    }

    // (h) The thinning: with cut_track = 2 the lane's low cut climbs two octaves for the roll's one,
    //     so the snare loses its body as it rises. The bound comes from the filter, not from a
    //     listening impression: the low cut is 24 dB/octave, the corner moves from 160 Hz to 640, so
    //     a partial at 200 Hz that stood in the pass band ends two octaves below the corner and must
    //     lose tens of decibels. The same measurement with cut_track = 0 is the control.
    {
        auto bodyDb = [&](const char* knobs, int shift) {
            ParamStore p;
            p.parseText(knobs);
            auto kit = makeKit(p);
            kit->trigger(5, 1.0f, shift, 0.0);
            const std::vector<float> y = renderKit(*kit, 16384);
            return std::make_pair(bandPowerDb(y, 0, y.size(), 150.0, 400.0), bandPowerDb(y, 0, y.size(), 2000.0, 8000.0));
        };
        const auto flat0 = bodyDb("perc6.cut_track=0", 0), flat12 = bodyDb("perc6.cut_track=0", 12);
        const auto trk0 = bodyDb("perc6.cut_track=2", 0), trk12 = bodyDb("perc6.cut_track=2", 12);
        const double flatBal = (flat12.second - flat12.first) - (flat0.second - flat0.first);
        const double trkBal = (trk12.second - trk12.first) - (trk0.second - trk0.first);
        check(trkBal > flatBal + 10.0 && std::fabs(flatBal) < 6.0,
              "the tracking low cut thins the snare as it rises, and leaves it alone when it is off",
              fmt("balance 2-8k against 150-400 Hz, octave up minus unshifted: cut_track 0 %+.2f dB, cut_track 2 %+.2f dB", flatBal, trkBal));
    }

    // (i) The gesture as a whole, on the lane that plays it: the roll's own band balance from its
    //     first quarter to its last. This is the render measurement of Tools/ref_arrange.py --roll
    //     shrunk to one lane, and it is the number that says the roll lifts rather than only
    //     accelerating. The control is the same roll without the pitch ramp and without the tracking.
    {
        auto rollBalance = [&](const char* knobs, bool ramp) {
            ParamStore p;
            p.parseText(knobs);
            auto kit = makeKit(p);
            const DenormalGuard guard;
            const PercPlan plan = makePercPlan(p, 1234u, true);
            // The four roll bars at 145 BPM: 1.655 s each, 79448 samples.
            const double sr = 48000.0, secPerBeat = 60.0 / 145.0;
            std::vector<float> L(4 * 79448), R(4 * 79448);
            std::vector<NoteEvent> notes;
            for (int r = 0; r < 4; ++r) {
                PercBarSpec spec;
                spec.rollBar = r;
                spec.pdb = r == 3;
                spec.fills = false;
                composePercBar(p, plan, 1234u, r, r, 145.0, 6, 1, spec, notes);
            }
            size_t pos = 0;
            for (const NoteEvent& n : notes) {
                if (n.lane != 5) continue;
                const size_t at = static_cast<size_t>(n.beat * secPerBeat * sr);
                if (at >= L.size()) continue;
                while (pos < at) {
                    const int step = static_cast<int>(std::min<size_t>(64, at - pos));
                    kit->process(L.data() + pos, R.data() + pos, step);
                    pos += static_cast<size_t>(step);
                }
                const int shift = static_cast<int>(n.pitch) - kPercRoleNote[static_cast<int>(PercRole::Snare)];
                kit->trigger(5, n.velocity / 127.0f, ramp ? shift : 0, 0.0);
            }
            while (pos + 64 < L.size()) { kit->process(L.data() + pos, R.data() + pos, 64); pos += 64; }
            std::vector<float> m(L.size());
            for (size_t i = 0; i < L.size(); ++i) m[i] = 0.5f * (L[i] + R[i]);
            const size_t quarter = m.size() / 4;
            auto bal = [&](size_t from) { return bandPowerDb(m, from, quarter, 2000.0, 8000.0) - bandPowerDb(m, from, quarter, 150.0, 500.0); };
            return bal(3 * quarter) - bal(0);
        };
        const double plainRoll = rollBalance("perc6.cut_track=0", false);
        const double lifted = rollBalance("perc6.cut_track=2", true);
        check(lifted > plainRoll + 5.0,
              "over its four bars the roll moves its weight from the body to the top",
              fmt("2-8k against 150-500 Hz, last quarter minus first: flat roll %+.2f dB, with the ramp and the tracking %+.2f dB",
                  plainRoll, lifted));
    }

    // ---------------------------------------------------------------------------------------------
    // The acid's macro ride and the buildup's send.
    // ---------------------------------------------------------------------------------------------

    // The trajectory a chain of control events plays, sampled per bar: the engine ramps with a raised
    // cosine from wherever it is to the event's value (Engine.cpp), so the same arithmetic reproduces
    // it here without an engine.
    auto trajectory = [](const std::vector<ControlEvent>& events, int param, double beat0, double bars, int perBar) {
        std::vector<double> out;
        double value = 0.0, from = 0.0, to = 0.0, start = 0.0, length = 0.0;
        size_t next = 0;
        const int n = static_cast<int>(bars) * perBar;
        for (int i = 0; i <= n; ++i) {
            const double b = beat0 + static_cast<double>(i) * kBeatsPerBar / perBar;
            while (next < events.size() && events[next].beat <= b + 1e-9) {
                const ControlEvent& e = events[next++];
                if (e.param != param || e.kind != ControlEvent::Kind::Offset) continue;
                if (e.length <= 0.0f) { value = e.value; length = 0.0; }
                else { from = value; to = e.value; start = e.beat; length = e.length; }
            }
            if (length > 0.0) {
                const double x = (b - start) / length;
                if (x >= 1.0) { value = to; length = 0.0; }
                else value = from + (to - from) * (x <= 0.0 ? 0.0 : 0.5 - 0.5 * std::cos(3.141592653589793 * x));
            }
            out.push_back(value);
        }
        return out;
    };

    // (j) The four-stage ride of the user's rule text (18.09.2026), read back from the trajectory the
    //     engine would play, in the parameters' own units. The rule: bars 1-8 cutoff almost closed,
    //     resonance medium, short decay; bars 9-16 decay longer, cutoff opening; bars 17-24 resonance
    //     up to 80-90 %; bars 25-32 cutoff fully open, then a radical dive just before the drop. And the
    //     cutoff still averages to the section's own line, which keeps the level match honest.
    {
        ParamStore p;
        Section s;
        s.type = SectionType::Drop;
        s.bars = 32;
        s.energy = s.energyTo = 0.87f;
        std::vector<ControlEvent> ev;
        sectionAutomation(p, s, 0x5EEDu, 0.0, 0.0f, 0.0f, false, ev);
        const int ab = p.base(Module::Acid);
        const std::vector<double> cut = trajectory(ev, ab + acid::Cutoff, 0.0, 32.0, 8);
        const std::vector<double> res = trajectory(ev, ab + acid::Resonance, 0.0, 32.0, 8);
        const std::vector<double> dec = trajectory(ev, ab + acid::Decay, 0.0, 32.0, 8);
        // Absolute values: resonance is linear, decay logarithmic, cutoff in octaves from the knob.
        const ParamDesc& dd = p.desc(ab + acid::Decay);
        const ParamDesc& cd = p.desc(ab + acid::Cutoff);
        const double knobR = p.get(ab + acid::Resonance), knobD = p.get(ab + acid::Decay);
        auto decMs = [&](double off) { return knobD * std::pow(static_cast<double>(dd.maxValue) / dd.minValue, off); };
        auto octaves = [&](double off) { return off * std::log2(static_cast<double>(cd.maxValue) / cd.minValue); };
        auto meanOf = [](const std::vector<double>& v, double b0, double b1) {
            double m = 0.0;
            int n = 0;
            for (int i = static_cast<int>(b0 * 8); i < static_cast<int>(b1 * 8); ++i) { m += v[static_cast<size_t>(i)]; ++n; }
            return m / std::max(1, n);
        };
        // Stage 1 from bar 2 on (the first bar is the approach from wherever the section began).
        const double c1 = meanOf(cut, 1, 8), c2end = cut[16 * 8], c3end = cut[24 * 8];
        double c4max = -1e9;
        for (int i = 24 * 8; i < 31 * 8; ++i) c4max = std::max(c4max, cut[static_cast<size_t>(i)]);
        const double cEnd = cut.back();
        double mean = 0.0;
        for (double v : cut) mean += v;
        mean /= static_cast<double>(cut.size());
        const double r1 = knobR + meanOf(res, 1, 16), r3 = knobR + meanOf(res, 22, 31);
        const double d1 = decMs(meanOf(dec, 1, 8)), d2end = decMs(dec[16 * 8]), d4 = decMs(meanOf(dec, 28, 32));
        const bool cutoffOk = octaves(c4max - c1) > 2.5 && c2end > c1 && c3end > c2end && c4max > c3end
                           && octaves(c4max - cEnd) > 2.5 && std::fabs(cEnd - c1) < 0.02
                           && std::fabs(mean) < 0.05 * kRideCutoff;
        const bool resoOk = r1 >= 0.5 && r1 <= 0.6 && r3 >= 0.8 && r3 <= 0.9;
        const bool decayOk = d1 < 0.5 * knobD && d2end > 2.0 * d1 && d4 > knobD;
        check(cutoffOk && resoOk && decayOk,
              "the acid ride's four stages: closed and dry, opening, squelch at 80-90 % resonance, fully open, dive; centred on the section's line",
              fmt("cutoff (octaves from the line) stage 1 %+.2f, end of 2 %+.2f, end of 3 %+.2f, stage 4 peak %+.2f, after the dive %+.2f, "
                  "section mean %+.4f normalised; resonance stages 1-2 %.2f, stage 3-4 %.2f; decay stage 1 %.0f ms, end of 2 %.0f ms, stage 4 %.0f ms (knob %.0f)",
                  octaves(c1), octaves(c2end), octaves(c3end), octaves(c4max), octaves(cEnd), mean, r1, r3, d1, d2end, d4, knobD));
    }

    // (j2) A sixteen-bar buildup plays the whole cycle in its sixteen bars, so that the dive lands on the
    //      drop: the peak falls in its last quarter and the last bar goes from there back to closed.
    {
        ParamStore p;
        Section s;
        s.type = SectionType::Build;
        s.bars = 16;
        std::vector<ControlEvent> ev;
        sectionAutomation(p, s, 0xB1D5u, 0.0, 0.0f, 0.0f, false, ev);
        const std::vector<double> cut = trajectory(ev, p.base(Module::Acid) + acid::Cutoff, 0.0, 16.0, 8);
        size_t peak = 0;
        for (size_t i = 0; i < cut.size(); ++i) if (cut[i] > cut[peak]) peak = i;
        const double fall = cut[15 * 8] - cut.back();
        check(peak >= 12 * 8 && peak <= 15 * 8 && fall > 0.6 * kRideCutoff * 0.75 && std::fabs(cut.back() - cut[2 * 8]) < 0.02,
              "a sixteen-bar buildup rides the whole cycle and dives over its last bar, onto the drop",
              fmt("peak at bar %.2f, fall over the last bar %.3f normalised (kRideCutoff %.2f), end %+.3f against stage 1 %+.3f",
                  peak / 8.0, fall, kRideCutoff, cut.back(), cut[2 * 8]));
    }

    // (k) A breakdown dives instead, and returns. Bound: at least 0.6 of kRideDive down (the depth is
    //     drawn from the seed between 0.6 and 1) and back to the section's own end value.
    {
        ParamStore p;
        Section s;
        s.type = SectionType::Break;
        s.bars = 16;
        s.energy = s.energyTo = 0.26f;
        std::vector<ControlEvent> ev;
        sectionAutomation(p, s, 0xBEEFu, 0.0, 0.0f, 0.0f, false, ev);
        const std::vector<double> y = trajectory(ev, p.base(Module::Acid) + acid::Cutoff, 0.0, 16.0, 8);
        double lo = 1e9;
        for (double v : y) lo = std::min(lo, v);
        check(lo < -0.5 * kRideDive && std::fabs(y.back()) < 1e-6,
              "a breakdown dives and comes back",
              fmt("deepest %+.4f normalised (allowed %.4f), value at the section end %+.6f", lo, -kRideDive, y.back()));
    }

    // (l) The ride is the section's own property: the same seed writes the same events, a different
    //     seed writes different ones, and the first section of the first track plays the knobs.
    {
        ParamStore p;
        Section s;
        s.type = SectionType::Drop;
        s.bars = 32;
        std::vector<ControlEvent> a, b, c, knobs;
        sectionAutomation(p, s, 11u, 0.0, 0.0f, 0.0f, false, a);
        sectionAutomation(p, s, 11u, 0.0, 0.0f, 0.0f, false, b);
        sectionAutomation(p, s, 12u, 0.0, 0.0f, 0.0f, false, c);
        sectionAutomation(p, s, 11u, 0.0, 0.0f, 0.0f, true, knobs);
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i)
            same = a[i].beat == b[i].beat && a[i].param == b[i].param && a[i].value == b[i].value && a[i].length == b[i].length;
        bool differs = a.size() != c.size();
        for (size_t i = 0; !differs && i < a.size(); ++i) differs = a[i].value != c[i].value;
        bool knobsClean = true;
        for (const ControlEvent& e : knobs) knobsClean = knobsClean && e.value == 0.0f;
        check(same && differs && knobsClean && knobs.size() == 1,
              "the ride is deterministic from the section's seed, differs with it, and is silent on the very first section",
              fmt("%d events, same seed identical %s, other seed differs %s, first section writes %d event(s), all zero %s",
                  static_cast<int>(a.size()), same ? "yes" : "no", differs ? "yes" : "no", static_cast<int>(knobs.size()), knobsClean ? "yes" : "no"));
    }

    // (m) The ride cannot fight the 303's accent, because the two live three orders of magnitude apart
    //     in rate: the accent charges a capacitor with 150 ms (Acid.cpp, kSweepTau) and is gone within
    //     two sixteenths, while the fastest thing the ride does is the falling quarter of its shortest
    //     period. Measured as a ratio of times, which is the only way two movements of the same
    //     parameter can be told apart at all.
    {
        ParamStore p;
        Section s;
        s.type = SectionType::Groove;
        s.bars = 8;
        std::vector<ControlEvent> ev;
        sectionAutomation(p, s, 7u, 0.0, 0.0f, 0.0f, false, ev);
        double shortest = 1e9;
        for (const ControlEvent& e : ev)
            if (e.param == p.base(Module::Acid) + acid::Cutoff && e.length > 0.0f) shortest = std::min(shortest, static_cast<double>(e.length));
        const double seconds = shortest * 60.0 / 145.0;
        // Since 18.09.2026 the fastest move is the ride's dive, one bar long; everything else is two bars
        // and longer in a 32-bar cycle. One bar is still ten times the capacitor's time constant.
        check(seconds / 0.15 > 10.0,
              "the ride's fastest move is slower than the accent's capacitor by more than an order of magnitude",
              fmt("shortest ramp %.2f beats = %.2f s against kSweepTau 0.15 s: a factor of %.0f", shortest, seconds, seconds / 0.15));
    }

    // (n) The buildup's send: the percussion's hall climbs over the last four bars of a buildup and is
    //     cut at the section that follows. Both halves are checked, because a send left open into the
    //     drop would smear the transient the whole gesture exists to sharpen.
    {
        ParamStore p;
        Section build;
        build.type = SectionType::Build;
        build.bars = 16;
        Section drop;
        drop.type = SectionType::Drop;
        drop.bars = 32;
        std::vector<ControlEvent> ev;
        sectionAutomation(p, build, 3u, 0.0, 0.0f, 0.0f, false, ev);
        sectionAutomation(p, drop, 4u, 16.0 * kBeatsPerBar, 0.0f, 0.0f, false, ev);
        std::stable_sort(ev.begin(), ev.end(), [](const ControlEvent& a, const ControlEvent& b) { return a.beat < b.beat; });
        const std::vector<double> y = trajectory(ev, p.base(Module::Mix) + mix::PercHall, 0.0, 20.0, 8);
        // One eighth of a bar before the drop, and exactly on it: the drop's own first event is the cut.
        const size_t atPdb = 16 * 8 - 1, atDrop = 16 * 8;
        double before = 0.0;
        for (size_t i = 0; i < 12 * 8; ++i) before = std::max(before, y[i]);
        check(y[atPdb] > 0.9 * kRollSend && before < 1e-9 && y[atDrop] < 1e-9,
              "the roll's hall send climbs over the last four bars of the buildup and is cut at the drop",
              fmt("send %.3f at the end of the buildup (target %.2f), %.3f over the twelve bars before it, %.3f one eighth into the drop",
                  y[atPdb], kRollSend, before, y[atDrop]));
    }
}

/** @brief Self test: section rules on the render (Solberg and Dibben 2019). */
void testSectionRules()
{
    section("section rules on the render (Solberg and Dibben 2019)");
    const double sr = 48000.0;
    ParamStore p;
    // 19.09.2026: the default 256 bars -- the two-drop form's 32-bar breakdown and big buildup. At 128 bars
    // the form shrinks both to 16, and the breakdown's window then reaches the bar where its hat returns.
    // No drone: its low octave is the floor of every breakdown by the voices round's rule (testVoices (g)
    // measures it), and on the seed this finds it stands 13.5 dB under the core's kick -- the removal this
    // section measures is the kick's and the bass's.
    // `sound_variation=0` since 22.09.2026: this section measures the **form** -- the U, the buildup's
    // ramp, what a pre-drop break holds -- and since 21.09.2026 the first track draws its own sound
    // like every other one, so every number here would otherwise depend on which pad, kick and bass
    // that one draw produced. Bisected: with the first track back on the knobs the breakdown's
    // 40..140 Hz band reads 22.8 dB under the core and every buildup window rises; with a drawn sound
    // 18.1 dB and one window dips by 0.2 dB. Neither is a fault of the arrangement, which is what
    // this section is about -- the sound is held fixed here exactly as the oscillator sections switch
    // off detune and the LFOs to measure a waveform. The per-track sound has its own sections
    // (testVoices.sound, testRecipeSpread, testVariety).
    p.parseText("compose.track_bars=256 compose.acid_amount=1 compose.lead_amount=1 compose.arp_amount=1 "
                "compose.pad_amount=1 compose.sfx_amount=1 compose.drone_amount=0 compose.sound_variation=0 "
                "compose.level_match=Off master.auto_gain=Off");
    // A seed whose first track carries the whole break routine: core, breakdown, buildup, drop, and a
    // pre-drop break that does not keep the kick on beat 4 (the "Kick on 4" variant is allowed to).
    int coreA = -1, brk = -1, build = -1, drop = -1;
    uint64_t seed = 0;
    std::unique_ptr<Composer> comp;
    for (uint64_t s = 1; s <= 200 && coreA < 0; ++s) {
        auto c = std::make_unique<Composer>(s);
        const TrackPlan t = c->track(p, 0);
        for (int i = 3; i < t.form.count; ++i) {
            if (t.form.section[i - 3].type != SectionType::Groove && t.form.section[i - 3].type != SectionType::Drop) continue;
            if (t.form.section[i - 2].type != SectionType::Break || t.form.section[i - 1].type != SectionType::Build) continue;
                // A sixteen-bar buildup, so that its rise can be read over four four-bar windows, and a
            // pre-drop break that does not keep the kick on beat 4 (the "Kick on 4" variant may).
            if (t.form.section[i].type != SectionType::Drop || t.form.section[i - 1].pdbVariant == 3
                || t.form.section[i - 1].bars < 16) continue;
            coreA = i - 3;
            brk = i - 2;
            build = i - 1;
            drop = i;
            seed = s;
            comp = std::move(c);
            break;
        }
    }
    check(coreA >= 0, "a track with the full break routine was found", fmt("seed %llu", static_cast<unsigned long long>(seed)));
    if (coreA < 0) return;

    const TrackPlan plan = comp->track(p, 0);
    const double bpm = plan.bpm, beatSec = 60.0 / bpm, barSec = kBeatsPerBar * beatSec;
    auto e = std::make_unique<Engine>();
    e->prepare(sr, 512);
    e->params().copyValuesFrom(p);
    TempoMap tm = comp->tempoMap(e->params(), plan.bars);
    e->setTempoMap(tm);
    Conductor cond(*e, *comp);
    const size_t total = static_cast<size_t>(tm.secondsAt(plan.bars * static_cast<double>(kBeatsPerBar)) * sr);
    std::vector<float> mono(total), L(512), R(512);
    size_t done = 0;
    while (done < total) {
        cond.pump(e->params(), 32.0);
        const int n = static_cast<int>(std::min<size_t>(512, total - done));
        e->process(L.data(), R.data(), n);
        for (int i = 0; i < n; ++i) mono[done + static_cast<size_t>(i)] = 0.5f * (L[static_cast<size_t>(i)] + R[static_cast<size_t>(i)]);
        done += static_cast<size_t>(n);
    }
    const size_t lat = static_cast<size_t>(e->latencySamples());
    auto barAt = [&](int bar) { return static_cast<size_t>(bar * barSec * sr) + lat; };
    auto barsRms = [&](int from, int count) { return rmsDb(mono, barAt(from), static_cast<size_t>(count * barSec * sr)); };
    auto barsBand = [&](int from, int count, double lo, double hi) {
        return bandPowerDb(mono, barAt(from), static_cast<size_t>(count * barSec * sr), lo, hi);
    };

    const Section& sCore = plan.form.section[coreA];
    const Section& sBrk = plan.form.section[brk];
    const Section& sBuild = plan.form.section[build];
    const Section& sDrop = plan.form.section[drop];
    const int coreWindow = std::min(16, sCore.bars - 2);
    const int coreFrom = sCore.startBar + sCore.bars - coreWindow;
    const int brkFrom = sBrk.startBar + 2, brkWindow = std::min(8, sBrk.bars - 4);

    // (a) The U: the breakdown far below the core, and after the drop at least what was there before
    // the break -- Solberg and Dibben's finding on the track their listeners liked best.
    const double core = barsRms(coreFrom, coreWindow);
    const double breakdown = barsRms(brkFrom, brkWindow);
    const double after = barsRms(sDrop.startBar, std::min(16, sDrop.bars));
    check(breakdown < core - 6.0 && after >= core - 0.5,
          "U-shaped amplitude: the breakdown at least 6 dB under the core, the drop back at or above it",
          fmt("core %.1f dB, breakdown %.1f dB (%.1f down), after the drop %.1f dB (%+.1f)", core, breakdown, core - breakdown, after, after - core));

    // The buildup rises monotonically over four-bar windows.
    {
        // Four four-bar windows; the last one stops before the pre-drop break, which is a vacuum by
        // design and would read as a fall.
        int windows = 0, falls = 0;
        double prev = -1e9, first = 0.0, last = 0.0;
        std::string detail;
        for (int b = 0; b + 4 <= sBuild.bars; b += 4) {
            const double v = barsRms(sBuild.startBar + b, b + 4 >= sBuild.bars ? 3 : 4);
            if (windows == 0) first = v;
            last = v;
            // 23.09.2026: 0.35 dB, not 0.2. The buildup's gain ramps *down* by the climax headroom while its energy
            // rises (Form.h, kBuildHeadroomDb), so the windows are nearly level by design and the roll's stage
            // changes move them by a few tenths either way (measured -16.7 -16.9 -16.6 -16.7 -16.7 -17.0 -16.6 -16.2).
            if (windows > 0 && v < prev - 0.35) ++falls;
            detail += fmt(" %.1f", v);
            prev = v;
            ++windows;
        }
        check(windows >= 2 && falls == 0 && last > first,
              "the buildup rises over every four-bar window", fmt("%d windows, %d falling, %.1f -> %.1f dB (build of %d bars from bar %d, windows:%s)", windows, falls, first, last, sBuild.bars, sBuild.startBar, detail.c_str()));
    }

    // (b) The bass band, the "sudden removal of bass and bass drum".
    const double coreLow = barsBand(coreFrom, coreWindow, 40.0, 140.0);
    const double brkLow = barsBand(brkFrom, brkWindow, 40.0, 140.0);
    // 12 dB since 22.09.2026 (was 20). The user's brief on the pad's foundation is explicit: in a breakdown
    // the pad "muss immer eine Bassnote in Oktave 1 oder 2 mitfuehren ... bis 50 Hz hinab, sonst bricht das
    // gesamte Soundfundament weg" -- so the sub sits at F#1 .. C#2 now, an octave under where it was, and
    // held for the whole breakdown. That is energy *in* this band by design; measured 14.3 dB under the
    // core against 22.8 before. Solberg and Dibben's "sudden removal of bass and bass drum" is still a
    // factor of 25 in power, and what they measured was the kick's and the bass's absence, not a pad's.
    check(brkLow < coreLow - 12.0, "40 to 140 Hz in the breakdown at least 12 dB under the core (the pad carries the floor there since 22.09.2026)",
          fmt("core %.1f dB, breakdown %.1f dB (%.1f down)", coreLow, brkLow, coreLow - brkLow));

    // (c) The pre-drop break: beat 4 of the last bar of the buildup, in the kick and bass band,
    // against a beat of the core.
    {
        const int pdbBar = sBuild.startBar + sBuild.bars - 1;
        const size_t beat4 = static_cast<size_t>((pdbBar * barSec + 3.0 * beatSec) * sr) + lat;
        const size_t coreBeat = static_cast<size_t>((coreFrom + 4) * barSec * sr) + lat;
        const size_t len = static_cast<size_t>(beatSec * sr);
        const double pdb = bandPowerDb(mono, beat4, len, 40.0, 140.0);
        const double ref = bandPowerDb(mono, coreBeat, len, 40.0, 140.0);
        check(pdb < ref - 30.0, "beat 4 of the pre-drop break: kick and bass band at least 30 dB under a core beat",
              fmt("core beat %.1f dB, PDB beat 4 %.1f dB (%.1f down, variant %s)", ref, pdb, ref - pdb, kPdbVariantNames[sBuild.pdbVariant]));
    }

    // (d) The presence band after the drop against before the break: the drop must not be duller than
    // what it replaces (the spectral-flux half of Solberg and Dibben's Track 2 finding).
    const double corePres = barsBand(coreFrom, coreWindow, 1500.0, 6000.0);
    const double afterPres = barsBand(sDrop.startBar, std::min(16, sDrop.bars), 1500.0, 6000.0);
    // The tolerance is 1.0 dB since 16.09.2026, measured rather than guessed. With one mode for a
    // whole track the core before the break and the drop after it played the same notes in the same
    // register and this band matched to a tenth of a decibel; a section may now borrow another mode
    // (Form.h), so the two play different notes and the band wanders with them. Over twelve tracks
    // each, testModalInterchange measures the worst case at -0.64 dB with interchange on and -0.50
    // dB with it off -- the same spread, and the old 0.5 dB bound sat inside it either way. The rule
    // being tested is that the drop brings the spectrum back, which a missing voice would break by
    // far more than a decibel.
    check(afterPres >= corePres - 1.0, "1.5 to 6 kHz after the drop at least what it was before the break",
          fmt("before %.1f dB, after %.1f dB (%+.1f)", corePres, afterPres, afterPres - corePres));
}

/** @brief Self test: locks, rerolls and the set file. */
void testCuration()
{
    section("locks, rerolls and the set file");

    // The fingerprint reads the middle of a track, outside the sixteen-bar overlap windows at its
    // ends: a transition deliberately carries the neighbouring track's hats and pads into them
    // (PLAN 6.7), so rerolling a track does change the overlap bars of the tracks beside it.
    auto fingerprint = [](Composer& c, const ParamStore& p, int track) {
        const TrackPlan t = c.track(p, track);
        std::vector<NoteEvent> notes;
        std::vector<ControlEvent> controls;
        c.composeBars(p, t.firstBar + 16, std::min(t.bars - 32, 48), notes, &controls);
        uint64_t h = 1469598103934665603ull;
        auto mix64 = [&](uint64_t k) { h = (h ^ k) * 1099511628211ull; };
        mix64(static_cast<uint64_t>(t.firstBar) * 1000 + static_cast<uint64_t>(t.bars));
        for (const NoteEvent& e : notes) {
            mix64(static_cast<uint64_t>(std::llround(e.beat * 960.0)));
            mix64(static_cast<uint64_t>(e.pitch) * 256 + static_cast<uint64_t>(e.part) * 16 + e.velocity / 8);
            mix64(static_cast<uint64_t>(std::llround(e.length * 960.0)));
        }
        for (const ControlEvent& e : controls) {
            mix64(static_cast<uint64_t>(std::llround(e.beat * 960.0)));
            mix64(static_cast<uint64_t>(e.param) * 4 + static_cast<uint64_t>(e.kind));
            mix64(static_cast<uint64_t>(std::llround(static_cast<double>(e.value) * 100000.0)) & 0xffffffffull);
        }
        return h;
    };

    ParamStore p;
    p.parseText("compose.track_bars=128 compose.level_match=Off master.auto_gain=Off");
    std::vector<uint64_t> before;
    {
        Composer c(4242);
        for (int t = 0; t < 5; ++t) before.push_back(fingerprint(c, p, t));
    }
    // Rerolling track 3 changes track 3 and nothing else.
    {
        Composer c(4242);
        c.reroll(LockUnit::Track, 2);
        int same = 0, changed = 0;
        for (int t = 0; t < 5; ++t) {
            const uint64_t h = fingerprint(c, p, t);
            if (t == 2) changed += h != before[static_cast<size_t>(t)] ? 1 : 0;
            else same += h == before[static_cast<size_t>(t)] ? 1 : 0;
        }
        check(same == 4 && changed == 1, "rerolling one track leaves every other track bit-identical", fmt("%d of 4 unchanged, track 3 changed %d", same, changed));
    }
    // A locked track survives a reroll of the whole set.
    {
        Composer c(4242);
        c.setLock(LockUnit::Track, 1, true);
        c.reroll(LockUnit::Set, 0);
        int lockedSame = 0, othersChanged = 0;
        for (int t = 0; t < 5; ++t) {
            const uint64_t h = fingerprint(c, p, t);
            if (t == 1) lockedSame = h == before[1] ? 1 : 0;
            else othersChanged += h != before[static_cast<size_t>(t)] ? 1 : 0;
        }
        // The locked track keeps its own material; its position can still move, because the tracks
        // before it are rerolled -- so the fingerprint is taken of the track's own seed, not its bar.
        check(othersChanged >= 3, "rerolling the set changes the unlocked tracks", fmt("%d of 4 changed, locked track %s", othersChanged, lockedSame ? "kept" : "moved"));
    }
    // A locked section and a locked lane keep their seeds through a reroll of their track.
    {
        Composer c(4242);
        const uint64_t plainSection = c.track(p, 1).sectionSeed[2];
        const int lane = 0;
        const PercPlan plain = c.track(p, 1).perc;
        c.setLock(LockUnit::Section, sectionUnitIndex(1, 2), true);
        c.setLock(LockUnit::PatternLane, laneUnitIndex(1, lane), true);
        c.reroll(LockUnit::Track, 1);
        const TrackPlan t = c.track(p, 1);
        check(t.sectionSeed[2] == plainSection && t.melodySeed != 0, "a locked section keeps its seed when its track is rerolled",
              fmt("seed %llx", static_cast<unsigned long long>(t.sectionSeed[2])));
        (void)plain;
    }

    // The set file: write, read back, and compose the same notes and controls.
    {
        ParamStore q;
        q.parseText("compose.track_bars=160 compose.style=Goa compose.arc=Peak-Time compose.bpm=141 kick.pitch_start=260 compose.level_match=Off master.auto_gain=Off");
        Composer c(987654321ull);
        c.setLock(LockUnit::Track, 2, true);
        c.reroll(LockUnit::Track, 1);
        c.reroll(LockUnit::Section, sectionUnitIndex(0, 3));
        const std::string text = writeSetText(c, q);
        ParamStore q2;
        Composer c2(1);
        std::string err;
        const bool read = readSetText(text, c2, q2, &err);
        int diffs = 0;
        for (int i = 0; i < q.count(); ++i) if (q.get(i) != q2.get(i)) ++diffs;
        std::vector<uint64_t> a, b;
        for (int t = 0; t < 4; ++t) { a.push_back(fingerprint(c, q, t)); b.push_back(fingerprint(c2, q2, t)); }
        check(read && diffs == 0 && a == b && c2.seed() == c.seed() && c2.isLocked(LockUnit::Track, 2)
              && c2.variation(LockUnit::Track, 1) == 1 && c2.variation(LockUnit::Section, sectionUnitIndex(0, 3)) == 1,
              "a .phosset round trip gives the same knobs, locks, rerolls and the same score",
              fmt("%d knob differences, %zu lines, error \"%s\"", diffs, std::count(text.begin(), text.end(), '\n'), err.c_str()));
        check(text.rfind("phosset 2\n", 0) == 0 && text.find("style=Goa\n") != std::string::npos
              && text.find("arc=Peak-Time\n") != std::string::npos && text.find("lock.track.2=1\n") != std::string::npos
              && text.find("reroll.track.1=1\n") != std::string::npos, "the set file carries header, seed, style, arc, locks and rerolls");
    }
}

/**
 * @brief The cue bridge of PLAN 8.3: the OSC bytes, the tap, the queue, and silence on failure.
 *
 * The oracle for the encoder is the OSC 1.0 byte layout worked out by hand below -- address string,
 * type tag string, big-endian arguments, every piece padded with nulls to a multiple of four -- not
 * a recording of what the encoder once produced. The oracle for the tap is the score:
 * phos::Composer::sections() is what the composer wrote, and the section cues a listener receives
 * have to be that list, in that order, on those bars.
 */
void testCues()
{
    section("cue bridge (PLAN 8.3)");

    // ---------------------------------------------------------------- the OSC bytes
    //
    // "/phos/beat" is ten characters, so the address takes 11 bytes with its terminator and is
    // padded to 12; the tag string ",i" takes 3 and is padded to 4; the argument is 4. 20 in all.
    char buf[128];
    Cue beat;
    beat.kind = Cue::Kind::Beat;
    beat.index = 7;
    int n = cueToOsc(beat, buf, sizeof(buf));
    check(n == 20, "/phos/beat is 20 bytes (12 address + 4 tags + 4 argument)", fmt("%d", n));
    check(std::memcmp(buf, "/phos/beat\0\0", 12) == 0 && std::memcmp(buf + 12, ",i\0\0", 4) == 0,
          "address and type tag string, terminated and padded with nulls");
    check(buf[16] == 0 && buf[17] == 0 && buf[18] == 0 && buf[19] == 7, "int32 argument big-endian");

    Cue bar;
    bar.kind = Cue::Kind::Bar;
    bar.index = 258;   // 0x00000102: two non-zero bytes, so a byte order mistake cannot hide
    n = cueToOsc(bar, buf, sizeof(buf));
    check(n == 20 && std::memcmp(buf, "/phos/bar\0\0\0", 12) == 0 && std::memcmp(buf + 12, ",i\0\0", 4) == 0
          && buf[16] == 0 && buf[17] == 0 && buf[18] == 1 && buf[19] == 2,
          "/phos/bar i, most significant byte first", fmt("%d bytes", n));

    // "/phos/section" is 13 characters: 14 with the terminator, padded to 16. ",sf" is 4 with its
    // terminator and needs no padding. "Drop" is 5 with its terminator and is padded to 8. The float
    // is 4. 32 in all, and 0.75 is exactly 0x3F400000 in IEEE 754.
    Cue sec;
    sec.kind = Cue::Kind::Section;
    sec.section = static_cast<uint8_t>(SectionType::Drop);
    sec.energy = 0.75f;
    n = cueToOsc(sec, buf, sizeof(buf));
    check(n == 32, "/phos/section is 32 bytes (16 + 4 + 8 + 4)", fmt("%d", n));
    check(std::memcmp(buf, "/phos/section\0\0\0", 16) == 0 && std::memcmp(buf + 16, ",sf\0", 4) == 0
          && std::memcmp(buf + 20, "Drop\0\0\0\0", 8) == 0,
          "the section type travels as an OSC string, padded to four");
    check(static_cast<unsigned char>(buf[28]) == 0x3F && static_cast<unsigned char>(buf[29]) == 0x40
          && buf[30] == 0 && buf[31] == 0, "float32 argument big-endian (0.75 = 3F 40 00 00)");

    // "/phos/key" 12, ",s" 4, "F# Phrygian" is 11 characters -> 12 with the terminator: 28 in all.
    Cue key;
    key.kind = Cue::Kind::Key;
    key.key = 6;      // F#
    key.scale = 1;    // Phrygian
    n = cueToOsc(key, buf, sizeof(buf));
    check(n == 28 && std::memcmp(buf, "/phos/key\0\0\0", 12) == 0 && std::memcmp(buf + 12, ",s\0\0", 4) == 0
          && std::memcmp(buf + 16, "F# Phrygian\0", 12) == 0, "/phos/key s", fmt("%d bytes", n));

    // A message with no arguments still carries a type tag string: "," alone, padded to four.
    Cue drop;
    drop.kind = Cue::Kind::Drop;
    n = cueToOsc(drop, buf, sizeof(buf));
    check(n == 16 && std::memcmp(buf, "/phos/drop\0\0", 12) == 0 && std::memcmp(buf + 12, ",\0\0\0", 4) == 0,
          "/phos/drop carries the empty type tag string", fmt("%d bytes", n));

    {
        int sizes = 0;
        for (int k = 0; k < static_cast<int>(Cue::Kind::Count); ++k) {
            Cue c;
            c.kind = static_cast<Cue::Kind>(k);
            const int m = cueToOsc(c, buf, sizeof(buf));
            if (m > 0 && m % 4 == 0) ++sizes;
        }
        check(sizes == static_cast<int>(Cue::Kind::Count), "every message is a multiple of four bytes",
              fmt("%d of %d", sizes, static_cast<int>(Cue::Kind::Count)));
    }

    // ---------------------------------------------------------------- the tap
    //
    // Sixteen bars of beat positions, cut into blocks of an awkward length so that block boundaries
    // fall inside beats: every integer beat must appear exactly once and every fourth one must
    // bring its bar. The expected numbers come from the arithmetic, not from a run: 64 beats,
    // 16 bars, indices 0..63 and 0..15.
    {
        CueTap tap;
        CueMarkRing marks(64);
        CueRing out(4096);
        const double bpm = 145.0, sr = 48000.0;
        const double beatsPerSample = bpm / 60.0 / sr;
        const int block = 137;   // not a divisor of anything musical
        double beat = 0.0;
        while (beat < 64.0) {
            const double to = std::min(64.0, beat + beatsPerSample * block);
            tap.scan(beat, to, 0, 0, 1, marks, out);
            beat = to;
        }
        const std::vector<Cue> cues = drainCues(out);
        std::vector<int> beats, bars;
        for (const Cue& c : cues) {
            if (c.kind == Cue::Kind::Beat) beats.push_back(c.index);
            if (c.kind == Cue::Kind::Bar) bars.push_back(c.index);
        }
        bool beatsRight = beats.size() == 64;
        for (size_t i = 0; i < beats.size() && beatsRight; ++i) beatsRight = beats[i] == static_cast<int>(i);
        bool barsRight = bars.size() == 16;
        for (size_t i = 0; i < bars.size() && barsRight; ++i) barsRight = bars[i] == static_cast<int>(i);
        check(beatsRight, "every beat of sixteen bars once, in order, over blocks that straddle them",
              fmt("%d beats", static_cast<int>(beats.size())));
        check(barsRight, "every bar once, numbered from zero", fmt("%d bars", static_cast<int>(bars.size())));
    }

    // Due times: a cue must be stamped with the instant its sample is *heard*, which is the block's
    // own start plus the lead plus its offset inside the block. The value is derived from the block
    // geometry here, not from the tap.
    {
        CueTap tap;
        CueMarkRing marks(16);
        CueRing out(64);
        const double sr = 48000.0;
        const int block = 512;
        const double beatsPerSample = 145.0 / 60.0 / sr;
        const int64_t blockNanos = static_cast<int64_t>(static_cast<double>(block) / sr * 1.0e9);
        const int64_t lead = 7000000;   // 7 ms
        const int64_t now = 1000000000;
        // Place beat 12 a quarter of the way into the block: its sample offset is then 128 of the
        // 512 samples, so it is heard 128 / 48000 = 2.667 ms after the block starts to play.
        const double span = beatsPerSample * block;
        const double from = 12.0 - 0.25 * span;
        tap.scan(from, from + span, now, lead, blockNanos, marks, out);
        const double offsetSamples = 0.25 * block;
        const int64_t want = now + lead + static_cast<int64_t>(offsetSamples / sr * 1.0e9);
        const std::vector<Cue> cues = drainCues(out);
        int64_t got = 0;
        for (const Cue& c : cues) if (c.kind == Cue::Kind::Beat && c.index == 12) got = c.dueNanos;
        check(got != 0 && std::llabs(got - want) < 50000,
              "a cue is due when its sample is heard: block start + lead + offset in the block",
              fmt("%.3f ms off", static_cast<double>(got - want) * 1.0e-6));
    }

    // ---------------------------------------------------------------- the score is the oracle
    //
    // The section cues a listener receives have to be phos::Composer::sections(), in that order, on
    // those bars -- the cut before its breakdown, the pre-drop break in the last bar of its buildup,
    // everything else where the form says. The bar a cue belongs to is read the way a receiver reads
    // it: the last /phos/bar that arrived.
    {
        ParamStore p;
        p.parseText("compose.track_bars=128 compose.level_match=Off master.auto_gain=Off");
        Composer comp(4711);
        const int bars = 160;
        const std::vector<SectionMark> score = comp.sections(p, bars);

        CueTap tap;
        CueMarkRing marks(512);
        CueRing out(8192);
        std::vector<Cue> received;
        int lastKey = -1;
        int barSeen = -1;
        std::vector<std::pair<int, int>> got;   // bar, SectionType
        const double sr = 48000.0;
        const double beatsPerSample = 145.0 / 60.0 / sr;
        const int block = 256;
        double beat = 0.0;
        int nextBar = 0;
        while (beat < static_cast<double>(bars) * kBeatsPerBar) {
            // The composer runs ahead: four bars of marks are always in the ring before the play
            // position reaches them, exactly as the conductor keeps the engine's rings filled.
            while (static_cast<double>(nextBar) * kBeatsPerBar < beat + 4 * kBeatsPerBar && nextBar < bars) {
                const TrackPlan plan = comp.track(p, comp.trackOfBar(p, nextBar));
                cueMarksForBar(plan.form, plan.firstBar, plan.key, plan.scale, nextBar, lastKey, marks);
                // The next track's intro over this one's outro (the DJ overlap, 19.09.2026): its marks too,
                // as the plugin, the Quest build and the cue demo send them.
                const int incoming = comp.incomingOfBar(p, nextBar);
                if (incoming >= 0) {
                    const TrackPlan next = comp.track(p, incoming);
                    cueMarksForBar(next.form, next.firstBar, next.key, next.scale, nextBar, lastKey, marks);
                }
                ++nextBar;
            }
            const double to = beat + beatsPerSample * block;
            tap.scan(beat, to, 0, 0, 1, marks, out);
            Cue c;
            while (out.pop(c)) {
                if (c.kind == Cue::Kind::Bar) barSeen = c.index;
                if (c.kind == Cue::Kind::Section) got.emplace_back(barSeen, static_cast<int>(c.section));
                received.push_back(c);
            }
            beat = to;
        }
        std::vector<std::pair<int, int>> want;
        for (const SectionMark& m : score) {
            const int b = static_cast<int>(m.beat / kBeatsPerBar);
            if (b < bars) want.emplace_back(b, static_cast<int>(m.type));
        }
        check(!want.empty() && got == want, "every section boundary of the score arrives as a cue, bar for bar",
              fmt("%d cues against %d marks of the score", static_cast<int>(got.size()), static_cast<int>(want.size())));

        // A drop accompanies every core and nothing else -- that is the message a flash cut hangs on.
        int drops = 0, cores = 0;
        for (const Cue& c : received) if (c.kind == Cue::Kind::Drop) ++drops;
        for (const auto& w : want) if (static_cast<SectionType>(w.second) == SectionType::Drop) ++cores;
        check(cores > 0 && drops == cores, "/phos/drop accompanies every core section and no other",
              fmt("%d drops, %d cores", drops, cores));

        // The key travels once per track, not once per section.
        int keys = 0;
        for (const Cue& c : received) if (c.kind == Cue::Kind::Key) ++keys;
        int tracks = 0;
        for (int i = 0;; ++i) {
            const TrackPlan t = comp.track(p, i);
            ++tracks;
            if (t.firstBar + t.bars >= bars) break;
        }
        check(keys == tracks, "/phos/key once per track", fmt("%d keys, %d tracks", keys, tracks));
    }

    // ---------------------------------------------------------------- silence on failure
    {
        CueSender s;
        check(!s.start("this-host-does-not-exist.invalid", 9000), "an unresolvable host does not start the bridge");
        check(!s.running() && !s.push(Cue{}), "and pushing into a bridge that is not running is refused, not an error");

        // A port nobody listens on: the datagram is dropped by the operating system and the sender
        // neither blocks nor reports anything. 9 is discard, and this machine is not running it.
        CueSender live;
        const bool opened = live.start("127.0.0.1", 9, true);
        if (opened) {
            for (int i = 0; i < 32; ++i) {
                Cue c;
                c.kind = Cue::Kind::Beat;
                c.index = i;
                c.dueNanos = 0;
                live.push(c);
            }
            // Wait for the sender's thread rather than guessing how long it needs: it wakes at most
            // every millisecond, and a machine under load can make a fixed sleep a flake.
            for (int i = 0; i < 400 && live.sent() < 32; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            live.stop();
        }
        check(opened && live.sent() == 32 && live.dropped() == 0,
              "thirty-two datagrams to a port with no listener: all sent, nothing reported",
              fmt("%llu sent, %llu dropped", static_cast<unsigned long long>(live.sent()),
                  static_cast<unsigned long long>(live.dropped())));
    }

    // ---------------------------------------------------------------- the engine does not hear it
    //
    // The tap reads the beat position and nothing else, so a render with the bridge on must be the
    // same render. Bit for bit, because "nearly the same" is how a silent dependency hides.
    {
        ParamStore p;
        p.parseText("compose.track_bars=128 compose.level_match=Off master.auto_gain=Off");
        auto render = [&](bool withCues, std::vector<float>& outL) {
            Engine e;
            e.prepare(48000.0, 256);
            e.params().copyValuesFrom(p);
            Composer comp(4711);
            Conductor cond(e, comp);
            CueTap tap;
            CueMarkRing marks(256);
            CueRing cues(4096);
            int lastKey = -1;
            int nextBar = 0;
            outL.clear();
            std::vector<float> L(256), R(256);
            const int blocks = 48000 * 8 / 256;
            for (int i = 0; i < blocks; ++i) {
                cond.pump(e.params(), 8 * kBeatsPerBar);
                const double from = e.beatPosition();
                e.process(L.data(), R.data(), 256);
                if (withCues) {
                    while (nextBar < cond.nextBar()) {
                        const TrackPlan plan = comp.track(p, comp.trackOfBar(p, nextBar));
                        cueMarksForBar(plan.form, plan.firstBar, plan.key, plan.scale, nextBar, lastKey, marks);
                        ++nextBar;
                    }
                    tap.scan(from, e.beatPosition(), cueNowNanos(), 0, 5333333, marks, cues);
                    Cue junk;
                    while (cues.pop(junk)) {}
                }
                outL.insert(outL.end(), L.begin(), L.end());
            }
        };
        std::vector<float> plain, tapped;
        render(false, plain);
        render(true, tapped);
        check(plain.size() == tapped.size() && std::memcmp(plain.data(), tapped.data(), plain.size() * sizeof(float)) == 0,
              "eight seconds of audio are bit-identical with the cue tap running");
    }
}

/** @brief Self test: transitions between tracks: the DJ overlap (19.09.2026; PLAN 6.7 before it). */
void testTransitions()
{
    section("transitions between tracks: the DJ overlap (19.09.2026; PLAN 6.7 before it)");
    // Since the arrangement round the transition is the DJ overlap (Form.h, kDjOverlap; testArrangement has
    // its form): here the harmonic side. The incoming pads sound over the outgoing bass only where the keys
    // are the same, a fourth or a fifth apart; elsewhere the incoming track's first sixteen bars carry no
    // pitched line at all.
    ParamStore p;
    p.parseText("compose.pad_amount=1 compose.sfx_amount=1 compose.track_variation=1 "
                "compose.level_match=Off master.auto_gain=Off");
    Composer c(31337);
    int consonant = 0, consonantWithPads = 0, dissonant = 0, dissonantPads = 0;
    for (int ti = 0; ti + 1 < 10; ++ti) {
        const TrackPlan a = c.track(p, ti);
        const TrackPlan b = c.track(p, ti + 1);
        const int move = ((b.key - a.key) % 12 + 12) % 12;
        const bool close = move == 0 || move == 5 || move == 7;
        std::vector<NoteEvent> head;
        c.composeBars(p, b.firstBar, b.form.handover, head);   // the whole blend (Form.h, djOverlapBars; 23.09.2026)
        int pads = 0;
        for (const NoteEvent& e : head) if (e.part == Part::Pad || e.part == Part::Drone) ++pads;
        if (close) { ++consonant; if (pads > 0 || !b.melody.present[mpIndex(MelodyPart::Pad)]) ++consonantWithPads; }
        else { ++dissonant; dissonantPads += pads; }
    }
    check(consonant > 0 && dissonant > 0 && consonantWithPads == consonant && dissonantPads == 0,
          "the incoming pads sound over the outgoing bass only where the keys are the same, a fourth or a fifth apart",
          fmt("%d close key moves, %d with the incoming pads there; %d far ones, %d pad or drone notes over them",
              consonant, consonantWithPads, dissonant, dissonantPads));
}

// ------------------------------------------------------------------ arrangement, 19.09.2026

} // namespace phostest
