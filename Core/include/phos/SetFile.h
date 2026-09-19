/**
 * @file SetFile.h
 * @brief The `.phosset` file: a whole set as text (PLAN 7).
 *
 * A set is a seed, a style, an energy arc, the knobs that differ from their defaults, and the locks and
 * rerolls the user has made while curating (PLAN 6.8). All of that is text in the line form Noctuary's
 * `.ambientset` uses, so a set can be read, diffed and edited by hand:
 * @code
 *   phosset 2
 *   seed=12345
 *   style=Goa
 *   arc=Peak-Time
 *   compose.bpm=143
 *   kick.pitch_start=260
 *   lock.track.3=1
 *   reroll.track.5=2
 * @endcode
 * The first line names the format and its version. `seed`, `style` and `arc` are written out even
 * though the last two are also parameters (`compose.style`, `compose.arc`), because they are what the
 * file is about; reading a file applies them to the parameters. Everything else is `ParamStore::toText`
 * with only the changed values, so a file stays short and survives new parameters with sensible
 * defaults. Unit names of locks and rerolls are those of kLockUnitNames ("set", "track", "section",
 * "lane").
 *
 * **Versions.** "phosset 2" since 19.09.2026 (round "voices": the counter-lead, the stab and the drone,
 * and the polyphonic instances reordered into groups). Both versions are read the same way, because a
 * knob is named by its key and no key changed its meaning: reading first puts every knob to its
 * default, then applies the file. A version-1 file therefore plays its old voices exactly as saved and
 * the three new ones from their defaults -- the composition around them is new either way.
 *
 * The score itself is not written: seed plus knobs plus locks reproduce it exactly (the determinism
 * rule of PLAN 3). Writing the rolled-out score for sets that must survive a change of the rules is
 * left for later, as the plan allows.
 */
#pragma once
#include <string>
#include <string_view>

namespace phos {

class Composer;
class ParamStore;

/** @brief The text form of a set: header, seed, style, arc, changed knobs, locks and rerolls. */
std::string writeSetText(const Composer& composer, const ParamStore& params);

/**
 * @brief Reads a `.phosset` text into a composer and a parameter store.
 * @param text     the file's contents
 * @param composer receives the seed, the locks and the reroll counters (its old ones are cleared)
 * @param params   receives the knobs: every knob the file does not name goes back to its default
 * @param error    receives a message for the first bad line, may be null
 * @return false when the header is missing or a line could not be applied
 */
bool readSetText(std::string_view text, Composer& composer, ParamStore& params, std::string* error = nullptr);

/** @brief Writes a `.phosset` file. */
bool writeSetFile(const char* path, const Composer& composer, const ParamStore& params);
/** @brief Reads a `.phosset` file. */
bool readSetFile(const char* path, Composer& composer, ParamStore& params, std::string* error = nullptr);

} // namespace phos
