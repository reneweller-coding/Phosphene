/**
 * @file FieldPresets.cpp
 * @brief The Field track's factory presets (FieldPresets.h).
 */
#include "phos/FieldPresets.h"
#include "phos/Dsp.h"
#include "phos/FieldLibrary.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace phos {

namespace {
const FieldPreset kRows[] = {
#include "FieldPresetData.inl"
};
} // namespace

const std::vector<FieldPreset>& fieldPresets()
{
    static const std::vector<FieldPreset> presets(std::begin(kRows), std::end(kRows));
    return presets;
}

int fieldVariationOf(int category, const std::string& stem)
{
    const int n = fieldClipCount(category);
    for (int v = 0; v < n; ++v)
        if (fieldClipName(fieldClipIndex(category, v)) == stem) return v;
    return -1;
}

bool fieldPresetAvailable(const FieldPreset& preset)
{
    return fieldVariationOf(preset.categoryA, preset.fileA) >= 0
        && (preset.categoryB < 0 || fieldVariationOf(preset.categoryB, preset.fileB) >= 0);
}

std::vector<std::pair<int, float>> fieldPresetValues(const ParamStore& p, const FieldPreset& preset)
{
    const int b = p.base(Module::Field);
    std::vector<float> v(static_cast<size_t>(field::Count));
    for (int i = 0; i < field::Count; ++i) v[static_cast<size_t>(i)] = p.defaultValue(b + i);
    std::istringstream in(preset.text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const int id = p.find("field." + line.substr(0, eq));
        if (id < b || id >= b + field::Count) continue;
        v[static_cast<size_t>(id - b)] = std::strtof(line.c_str() + eq + 1, nullptr);
    }
    v[field::ACategory] = static_cast<float>(preset.categoryA + 1);
    v[field::AVariation] = static_cast<float>(fieldPresetVariation(preset.categoryA, preset.fileA, preset.variationA));
    v[field::BOn] = preset.categoryB >= 0 ? 1.0f : 0.0f;
    if (preset.categoryB >= 0) {
        v[field::BCategory] = static_cast<float>(preset.categoryB + 1);
        v[field::BVariation] = static_cast<float>(fieldPresetVariation(preset.categoryB, preset.fileB, preset.variationB));
    }
    std::vector<std::pair<int, float>> out;
    out.reserve(static_cast<size_t>(field::Count));
    for (int i = 0; i < field::Count; ++i) out.emplace_back(i, v[static_cast<size_t>(i)]);
    return out;
}

int fieldPresetVariation(int category, const char* file, int shipped)
{
    const int v = fieldVariationOf(category, file);
    return v >= 0 ? v : shipped;
}

int fieldPresetFor(int style, uint64_t seed)
{
    const std::vector<FieldPreset>& all = fieldPresets();
    const int s = std::clamp(style, 0, 4);
    double total = 0.0;
    std::vector<double> w(all.size(), 0.0);
    for (size_t i = 0; i < all.size(); ++i) {
        // Squared: a style's own places come up far more often than the ones that only fit it.
        w[i] = static_cast<double>(all[i].style[s]) * all[i].style[s];
        total += w[i];
    }
    if (total <= 0.0) return -1;
    Rng r;
    r.seed(seed);
    double x = static_cast<double>(r.uniform()) * total;
    for (size_t i = 0; i < all.size(); ++i) {
        if (w[i] <= 0.0) continue;
        if (x < w[i]) return static_cast<int>(i);
        x -= w[i];
    }
    for (size_t i = all.size(); i-- > 0;) if (w[i] > 0.0) return static_cast<int>(i);
    return -1;
}

} // namespace phos
