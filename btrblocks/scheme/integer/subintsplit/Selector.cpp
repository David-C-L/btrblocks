// -------------------------------------------------------------------------------------
#include "scheme/integer/subintsplit/Selector.hpp"
// -------------------------------------------------------------------------------------
#include "btrblocks.hpp"
#include "scheme/SchemeConfig.hpp"
// -------------------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
// -------------------------------------------------------------------------------------
namespace btrblocks::subintsplit {
// -------------------------------------------------------------------------------------
std::vector<const ICostModel*> defaultCostModels() {
  static const UncompressedCostModel uncompressed;
  static const BitPackingCostModel bitPacking;
  static const OneValueCostModel oneValue;
  static const FrequencyCostModel frequency;
  static const DictionaryCostModel dictionary;
  static const RleCostModel rle;
  static const PforCostModel pfor;
  static const ForCostModel forModel;
  static const std::vector<const ICostModel*> allModels{
      &uncompressed, &bitPacking, &oneValue, &frequency, &dictionary, &rle, &pfor, &forModel};

  const auto& enabled = BtrBlocksConfig::get().integers.schemes;
  std::vector<const ICostModel*> models;
  models.reserve(allModels.size());
  for (const auto* model : allModels) {
    if (enabled.isEnabled(model->label())) {
      models.push_back(model);
    }
  }
  return models;
}
// -------------------------------------------------------------------------------------
SelectorConfig defaultSelectorConfig() {
  const auto& tuning = SchemeConfig::get().integers.subintsplit;
  SelectorConfig cfg;
  cfg.minSectionBits = tuning.min_section_bits;
  cfg.maxSectionBits = effectiveMaxSectionBits();
  cfg.maxSections = tuning.max_sections;
  cfg.splitPenalty = tuning.split_penalty;
  return cfg;
}
// -------------------------------------------------------------------------------------
bool parseSplitBoundaries(const std::string& text, int totalBits, std::vector<SegmentPlan>& out) {
  out.clear();
  std::size_t pos = 0;
  int expectedStart = 0;

  while (pos <= text.size()) {
    const std::size_t sep = text.find(';', pos);
    const std::string token =
        text.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
    if (token.empty()) {
      return false;
    }
    const std::size_t dash = token.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= token.size()) {
      return false;
    }

    char* end = nullptr;
    const long start = std::strtol(token.substr(0, dash).c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
      return false;
    }
    const long stop = std::strtol(token.substr(dash + 1).c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
      return false;
    }

    // Ranges must be ascending, contiguous, and stay inside the value.
    if (start != expectedStart || stop < start || stop >= totalBits) {
      return false;
    }

    SegmentPlan segment;
    segment.bitStart = static_cast<int>(start);
    segment.bitEnd = static_cast<int>(stop);
    out.push_back(segment);
    expectedStart = static_cast<int>(stop) + 1;

    if (sep == std::string::npos) {
      break;
    }
    pos = sep + 1;
  }

  // The ranges must tile the value exactly.
  if (out.empty() || expectedStart != totalBits) {
    out.clear();
    return false;
  }
  return true;
}
// -------------------------------------------------------------------------------------
std::vector<SegmentPlan>*& forcedSplitBoundaries() {
  static thread_local std::vector<SegmentPlan>* forced = nullptr;
  return forced;
}
// -------------------------------------------------------------------------------------
SplitPlan selectSplits(const std::vector<uint64_t>& samples,
                       int totalBits,
                       std::size_t fullCount,
                       const std::vector<const ICostModel*>& models,
                       const SelectorConfig& cfg) {
  SplitPlan result;

  const int maxWidth = std::max(1, std::min(cfg.maxSectionBits, totalBits));
  const int minWidth = std::max(1, std::min(cfg.minSectionBits, maxWidth));
  const int maxSections = std::max(1, cfg.maxSections);

  // The whole value in one section, used as a fallback whenever the DP cannot
  // produce anything (no samples, or a width cap that admits no tiling).
  const auto singleSectionFallback = [&]() {
    SplitPlan fallback;
    int start = 0;
    while (start < totalBits) {
      SegmentPlan segment;
      segment.bitStart = start;
      segment.bitEnd = std::min(start + maxWidth, totalBits) - 1;
      fallback.segments.push_back(segment);
      start = segment.bitEnd + 1;
    }
    return fallback;
  };

  if (samples.empty() || totalBits <= 0) {
    return singleSectionFallback();
  }

  // Score every candidate bit range on the sample, then scale to the full
  // stream so the split penalty is comparable against the savings.
  struct RangeScore {
    double cost{std::numeric_limits<double>::infinity()};
    IntegerSchemeType scheme{IntegerSchemeType::UNCOMPRESSED};
  };
  std::vector<RangeScore> scores(static_cast<std::size_t>(totalBits) * totalBits);

  const MetricFlags required = unionRequiredMetrics(models);
  MetricCollector collector;
  BitRangeExtractor extractor(samples);
  const double scale = static_cast<double>(fullCount) / static_cast<double>(samples.size());

  for (int l = 0; l < totalBits; l++) {
    extractor.reset(l);
    const int rLimit = std::min(totalBits - 1, l + maxWidth - 1);
    for (int r = l; r <= rLimit; r++) {
      extractor.extend(r);
      const int width = r - l + 1;
      if (width < minWidth) {
        continue;
      }
      const auto metrics = collector.compute(extractor.values(), width, required);
      IntegerSchemeType scheme = IntegerSchemeType::UNCOMPRESSED;
      const double perSample = bestCostBits(models, metrics, samples.size(), width, scheme);
      auto& score = scores[static_cast<std::size_t>(l) * totalBits + r];
      score.cost = perSample * scale;
      score.scheme = scheme;
    }
  }

  // dp[i][k] = cheapest way to cover bits [0, i) using exactly k sections.
  // The section-count dimension is what enforces maxSections; without it the
  // penalty alone only discourages splitting, it does not bound it -- and the
  // bound is a size guarantee, since each section costs a full INTEGER per
  // value before sub-compression.
  const int bits = totalBits;
  const auto index = [&](int i, int k) {
    return static_cast<std::size_t>(i) * (maxSections + 1) + k;
  };

  std::vector<double> dp((bits + 1) * (maxSections + 1), std::numeric_limits<double>::infinity());
  std::vector<int> prev((bits + 1) * (maxSections + 1), -1);
  dp[index(0, 0)] = 0.0;

  for (int i = 1; i <= bits; i++) {
    for (int k = 1; k <= maxSections; k++) {
      for (int j = std::max(0, i - maxWidth); j <= i - minWidth; j++) {
        const double previous = dp[index(j, k - 1)];
        if (!std::isfinite(previous)) {
          continue;
        }
        const auto& score = scores[static_cast<std::size_t>(j) * totalBits + (i - 1)];
        if (!std::isfinite(score.cost)) {
          continue;
        }
        const double penalty = (k == 1) ? 0.0 : cfg.splitPenalty;
        const double candidate = previous + score.cost + penalty;
        if (candidate < dp[index(i, k)]) {
          dp[index(i, k)] = candidate;
          prev[index(i, k)] = j;
        }
      }
    }
  }

  int bestSections = -1;
  double bestCost = std::numeric_limits<double>::infinity();
  for (int k = 1; k <= maxSections; k++) {
    if (dp[index(bits, k)] < bestCost) {
      bestCost = dp[index(bits, k)];
      bestSections = k;
    }
  }

  if (bestSections < 0 || !std::isfinite(bestCost)) {
    return singleSectionFallback();
  }

  int i = bits;
  int k = bestSections;
  while (i > 0 && k > 0) {
    const int j = prev[index(i, k)];
    if (j < 0) {
      return singleSectionFallback();
    }
    const auto& score = scores[static_cast<std::size_t>(j) * totalBits + (i - 1)];
    SegmentPlan segment;
    segment.bitStart = j;
    segment.bitEnd = i - 1;
    segment.predictedScheme = score.scheme;
    segment.cost = score.cost;
    result.segments.push_back(segment);
    i = j;
    k--;
  }
  std::reverse(result.segments.begin(), result.segments.end());
  result.totalCost = bestCost;

  if (result.segments.empty()) {
    return singleSectionFallback();
  }
  return result;
}
// -------------------------------------------------------------------------------------
}  // namespace btrblocks::subintsplit
// -------------------------------------------------------------------------------------
