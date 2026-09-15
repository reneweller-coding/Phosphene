/**
 * @file SetFile.cpp
 * @brief Reading and writing `.phosset`.
 */
#include "phos/SetFile.h"
#include "phos/Composer.h"
#include "phos/Form.h"
#include "phos/Params.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace phos {

namespace {

/** @brief The unit a name stands for, or -1. */
int unitOfName(std::string_view name)
{
    for (int u = 0; u < kNumLockUnits; ++u) if (name == kLockUnitNames[u]) return u;
    return -1;
}

/** @brief Splits "lock.track.3" into its three parts; false when it is not of that shape. */
bool splitUnitKey(std::string_view key, std::string_view& head, int& unit, int& index)
{
    const size_t a = key.find('.');
    if (a == std::string_view::npos) return false;
    const size_t b = key.find('.', a + 1);
    if (b == std::string_view::npos) return false;
    head = key.substr(0, a);
    unit = unitOfName(key.substr(a + 1, b - a - 1));
    if (unit < 0) return false;
    const std::string digits(key.substr(b + 1));
    if (digits.empty()) return false;
    char* end = nullptr;
    index = static_cast<int>(std::strtol(digits.c_str(), &end, 10));
    return end != nullptr && *end == '\0';
}

/** @brief Index of a choice name in a table, compared without case, or -1. */
int choiceIndex(std::string_view name, const char* const* names, int count)
{
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    for (int i = 0; i < count; ++i) {
        const std::string_view n(names[i]);
        if (n.size() != name.size()) continue;
        size_t k = 0;
        while (k < n.size() && lower(n[k]) == lower(name[k])) ++k;
        if (k == n.size()) return i;
    }
    return -1;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

} // namespace

std::string writeSetText(const Composer& composer, const ParamStore& p)
{
    const int cb = p.base(Module::Compose);
    std::string out = "phosset 1\n";
    char buf[128];
    std::snprintf(buf, sizeof(buf), "seed=%llu\n", static_cast<unsigned long long>(composer.seed()));
    out += buf;
    out += std::string("style=") + kStyleNames[static_cast<int>(styleOf(p))] + "\n";
    out += std::string("arc=") + kArcNames[static_cast<int>(arcOf(p))] + "\n";
    (void)cb;
    out += p.toText(true);
    for (int u = 0; u < kNumLockUnits; ++u) {
        for (const auto& kv : composer.locks(static_cast<LockUnit>(u))) {
            std::snprintf(buf, sizeof(buf), "lock.%s.%d=1\n", kLockUnitNames[u], kv.first);
            out += buf;
        }
        for (const auto& kv : composer.variations(static_cast<LockUnit>(u))) {
            if (kv.second == 0) continue;
            std::snprintf(buf, sizeof(buf), "reroll.%s.%d=%u\n", kLockUnitNames[u], kv.first, kv.second);
            out += buf;
        }
    }
    return out;
}

bool readSetText(std::string_view text, Composer& composer, ParamStore& p, std::string* error)
{
    // The header names the format; without it the file is not a set.
    std::string_view rest = text;
    const size_t nl = rest.find('\n');
    const std::string_view header = trim(rest.substr(0, nl == std::string_view::npos ? rest.size() : nl));
    if (header.rfind("phosset", 0) != 0) {
        if (error != nullptr) *error = "not a phosset file (the first line must be \"phosset 1\")";
        return false;
    }
    rest = nl == std::string_view::npos ? std::string_view() : rest.substr(nl + 1);

    composer.clearLocks();
    bool ok = true;
    std::string knobs;
    size_t pos = 0;
    while (pos <= rest.size()) {
        const size_t end = rest.find('\n', pos);
        const std::string_view line = trim(rest.substr(pos, (end == std::string_view::npos ? rest.size() : end) - pos));
        pos = end == std::string_view::npos ? rest.size() + 1 : end + 1;
        if (line.empty() || line.front() == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            if (error != nullptr && ok) *error = "line without '=': " + std::string(line);
            ok = false;
            continue;
        }
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        if (key == "seed") {
            composer.setSeed(std::strtoull(std::string(value).c_str(), nullptr, 10));
        } else if (key == "style" || key == "arc") {
            const bool isStyle = key == "style";
            const int i = choiceIndex(value, isStyle ? kStyleNames : kArcNames, isStyle ? kNumStyles : kNumArcs);
            if (i < 0) {
                if (error != nullptr && ok) *error = "unknown " + std::string(key) + ": " + std::string(value);
                ok = false;
            } else {
                p.set(p.base(Module::Compose) + (isStyle ? compose::Style : compose::Arc), static_cast<float>(i));
            }
        } else if (key.rfind("lock.", 0) == 0 || key.rfind("reroll.", 0) == 0) {
            std::string_view head;
            int unit = 0, index = 0;
            if (!splitUnitKey(key, head, unit, index)) {
                if (error != nullptr && ok) *error = "bad unit key: " + std::string(key);
                ok = false;
                continue;
            }
            const long v = std::strtol(std::string(value).c_str(), nullptr, 10);
            if (head == "lock") composer.setLock(static_cast<LockUnit>(unit), index, v != 0);
            else composer.setVariation(static_cast<LockUnit>(unit), index, static_cast<uint32_t>(v < 0 ? 0 : v));
        } else {
            knobs += std::string(line);
            knobs += '\n';
        }
    }
    if (!knobs.empty()) {
        std::string err;
        if (!p.parseText(knobs, &err)) {
            if (error != nullptr && ok) *error = err;
            ok = false;
        }
    }
    return ok;
}

bool writeSetFile(const char* path, const Composer& composer, const ParamStore& p)
{
    const std::string text = writeSetText(composer, p);
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const size_t n = std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return n == text.size();
}

bool readSetFile(const char* path, Composer& composer, ParamStore& p, std::string* error)
{
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        if (error != nullptr) *error = std::string("cannot read ") + path;
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    return readSetText(text, composer, p, error);
}

} // namespace phos
