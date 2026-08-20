#pragma once
// -------------------------------------------------------------------------------------
#include "scheme/SchemeType.hpp"
#include "scheme/integer/subintsplit/Metrics.hpp"
// -------------------------------------------------------------------------------------
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>
// -------------------------------------------------------------------------------------
// Cost models for the SubIntSplit split planner.
//
// Each model estimates, in bits, what one BtrBlocks integer scheme would cost
// to store a given bit range. The planner uses the cheapest estimate as the
// score of that range; the scheme actually used for the section is chosen
// independently by the ordinary scheme picker at encode time. The models exist
// to place split boundaries well, not to make the final scheme decision, so
// being directionally correct matters much more than being exact.
//
// Models are a virtual interface rather than a fixed set of functions so that
// richer models -- speed-aware ones, or weighted composites over several
// dimensions -- can be supplied by the caller without changing the planner.
// -------------------------------------------------------------------------------------
namespace btrblocks::subintsplit {
// -------------------------------------------------------------------------------------
// Number of bits a section value occupies before sub-compression.
//
// Always 32: a section is handed to the ordinary integer scheme picker, whose
// input is INTEGER, so even a 1-bit section costs a full 4 bytes per value
// until its sub-scheme compresses it. (Nimble narrows a section to the smallest
// physical type that fits, which is why its equivalent returns 8/16/32/64 --
// porting that verbatim would make every model under-charge narrow sections and
// the planner would over-split.)
inline constexpr int kSectionStorageBits = 32;
// -------------------------------------------------------------------------------------
inline int bitWidthOf(uint64_t value) noexcept {
  return value == 0 ? 0 : 64 - __builtin_clzll(value);
}
// -------------------------------------------------------------------------------------
inline double roundUpToByte(double bits) noexcept {
  return static_cast<double>((static_cast<uint32_t>(bits) + 7u) & ~7u);
}
// -------------------------------------------------------------------------------------
struct ICostModel {
  virtual ~ICostModel() = default;
  // Estimated cost in bits of storing `numValues` values of this bit range.
  // Returning infinity means "not applicable".
  virtual double costBits(const SegmentMetrics& metrics,
                          std::size_t numValues,
                          int bitWidth) const = 0;
  // Metrics this model reads. The planner computes the union across models.
  virtual MetricFlags requiredMetrics() const = 0;
  // The scheme this model stands for. Recorded in the plan so the planner's
  // prediction can be compared against what the picker actually chose.
  virtual IntegerSchemeType label() const = 0;
};
// -------------------------------------------------------------------------------------
// UNCOMPRESSED: every value stored raw.
struct UncompressedCostModel : ICostModel {
  double costBits(const SegmentMetrics&, std::size_t numValues, int) const override {
    return static_cast<double>(numValues) * kSectionStorageBits;
  }
  MetricFlags requiredMetrics() const override {
    return static_cast<MetricFlags>(MetricFlag::None);
  }
  IntegerSchemeType label() const override { return IntegerSchemeType::UNCOMPRESSED; }
};
// -------------------------------------------------------------------------------------
// BP: bit-packing at the width implied by the observed value range.
struct BitPackingCostModel : ICostModel {
  double costBits(const SegmentMetrics& m, std::size_t numValues, int bitWidth) const override {
    if (numValues == 0) {
      return 0.0;
    }
    // XPBPStructure plus FastPFor's own framing and alignment slack.
    constexpr double kHeaderBits = 24.0 * 8.0;
    const int rangeBits = bitWidthOf(m.range);
    const int packedBits = std::min(bitWidth, rangeBits);
    return kHeaderBits + static_cast<double>(packedBits) * static_cast<double>(numValues);
  }
  MetricFlags requiredMetrics() const override {
    return static_cast<MetricFlags>(MetricFlag::MinMax);
  }
  IntegerSchemeType label() const override { return IntegerSchemeType::BP; }
};
// -------------------------------------------------------------------------------------
// ONE_VALUE: applicable only when the range is constant.
struct OneValueCostModel : ICostModel {
  double costBits(const SegmentMetrics& m, std::size_t numValues, int) const override {
    if (numValues == 0) {
      return 0.0;
    }
    if (m.min != m.max) {
      return std::numeric_limits<double>::infinity();
    }
    return kSectionStorageBits;
  }
  MetricFlags requiredMetrics() const override {
    return static_cast<MetricFlags>(MetricFlag::MinMax);
  }
  IntegerSchemeType label() const override { return IntegerSchemeType::ONE_VALUE; }
};
// -------------------------------------------------------------------------------------
// FREQUENCY: one dominant value plus a bitmap and a stream of exceptions.
// Only worthwhile when a single value covers most of the range.
struct FrequencyCostModel : ICostModel {
  double costBits(const SegmentMetrics& m, std::size_t numValues, int bitWidth) const override {
    if (numValues == 0) {
      return 0.0;
    }
    if (m.dominantCountCapped || m.dominantCount == 0) {
      return std::numeric_limits<double>::infinity();
    }
    const std::size_t exceptions = m.dominantCount >= numValues ? 0 : numValues - m.dominantCount;
    // FrequencyStructure, plus a roaring bitmap over the exception positions.
    // The bitmap is charged at roughly two bytes per exception, which is the
    // right order for the scattered positions this scheme sees.
    constexpr double kHeaderBits = 16.0 * 8.0;
    const double bitmapBits = 16.0 * static_cast<double>(exceptions);
    const int rangeBits = bitWidthOf(m.range);
    const int packedBits = std::min(bitWidth, rangeBits);
    const double exceptionBits = static_cast<double>(packedBits) * static_cast<double>(exceptions);
    return kHeaderBits + bitmapBits + exceptionBits;
  }
  MetricFlags requiredMetrics() const override {
    return MetricFlag::MinMax | MetricFlag::DominantValue;
  }
  IntegerSchemeType label() const override { return IntegerSchemeType::FREQUENCY; }
};
// -------------------------------------------------------------------------------------
// DICT: a table of distinct values plus bit-packed codes.
struct DictionaryCostModel : ICostModel {
  double costBits(const SegmentMetrics& m, std::size_t numValues, int) const override {
    if (numValues == 0 || m.uniqueCount == 0) {
      return 0.0;
    }
    // A dictionary cannot pay for itself once the code is as wide as the value.
    if (m.uniqueCountCapped) {
      return std::numeric_limits<double>::infinity();
    }
    const std::size_t uniques = m.uniqueCount;
    const double dictBits = static_cast<double>(uniques) * kSectionStorageBits;
    const int codeBits = uniques <= 1 ? 1 : bitWidthOf(static_cast<uint64_t>(uniques - 1));
    const double codesBits = static_cast<double>(codeBits) * static_cast<double>(numValues);
    // DynamicDictionaryStructure plus the nested header for the codes stream.
    constexpr double kHeaderBits = 32.0 * 8.0;
    return kHeaderBits + dictBits + codesBits;
  }
  MetricFlags requiredMetrics() const override {
    return MetricFlag::MinMax | MetricFlag::UniqueCount;
  }
  IntegerSchemeType label() const override { return IntegerSchemeType::DICT; }
};
// -------------------------------------------------------------------------------------
// RLE: run values plus run lengths, each recursively compressed.
struct RleCostModel : ICostModel {
  double costBits(const SegmentMetrics& m, std::size_t numValues, int bitWidth) const override {
    if (numValues == 0 || m.avgRunLength <= 0.0) {
      return 0.0;
    }
    // avgRunLength is scale-invariant, so the run count extrapolates from the
    // sample to the full stream.
    const double runs = static_cast<double>(numValues) / m.avgRunLength;
    // RLEStructure plus two nested sub-stream headers.
    constexpr double kHeaderBits = 36.0 * 8.0;
    const int rangeBits = bitWidthOf(m.range);
    const int packedBits = std::min(bitWidth, rangeBits);
    const double runValueBits = runs * static_cast<double>(packedBits);
    // Run lengths are themselves compressed; 16 bits each is conservative.
    const double runLengthBits = runs * 16.0;
    return kHeaderBits + runValueBits + runLengthBits;
  }
  MetricFlags requiredMetrics() const override { return MetricFlag::MinMax | MetricFlag::RunStats; }
  IntegerSchemeType label() const override { return IntegerSchemeType::RLE; }
};
// -------------------------------------------------------------------------------------
// PFOR: same range-based bit-width estimate as BitPackingCostModel. SegmentMetrics
// (see Metrics.hpp) has no outlier-fraction field, so this converges to BP's cost
// estimate -- it's still a useful distinct label for `predicted` vs `actual`
// comparison, consistent with this file's philosophy of being directionally
// correct rather than exact (see top-of-file comment).
struct PforCostModel : ICostModel {
  double costBits(const SegmentMetrics& m, std::size_t numValues, int bitWidth) const override {
    if (numValues == 0) {
      return 0.0;
    }
    // XPBPStructure is tiny (u32_count + padding), but SIMDFastPFor's
    // patched-exception layout carries its own internal framing/alignment
    // slack beyond that visible header, so this stays in the same ballpark
    // as BitPackingCostModel's kHeaderBits rather than XPBPStructure's raw size.
    constexpr double kHeaderBits = 128.0;
    const int rangeBits = bitWidthOf(m.range);
    const int packedBits = std::min(bitWidth, rangeBits);
    return kHeaderBits + static_cast<double>(packedBits) * static_cast<double>(numValues);
  }
  MetricFlags requiredMetrics() const override {
    return static_cast<MetricFlags>(MetricFlag::MinMax);
  }
  IntegerSchemeType label() const override { return IntegerSchemeType::PFOR; }
};
// -------------------------------------------------------------------------------------
// FOR: a bias transform (FORStructure: INTEGER bias + next_scheme byte) that
// recurses into the picker one level down. Modeled as its common case (BP
// underneath), with a small header since FORStructure itself is tiny.
struct ForCostModel : ICostModel {
  double costBits(const SegmentMetrics& m, std::size_t numValues, int bitWidth) const override {
    if (numValues == 0) {
      return 0.0;
    }
    constexpr double kHeaderBits = 40.0;
    const int rangeBits = bitWidthOf(m.range);
    const int packedBits = std::min(bitWidth, rangeBits);
    return kHeaderBits + static_cast<double>(packedBits) * static_cast<double>(numValues);
  }
  MetricFlags requiredMetrics() const override {
    return static_cast<MetricFlags>(MetricFlag::MinMax);
  }
  IntegerSchemeType label() const override { return IntegerSchemeType::FOR; }
};
// -------------------------------------------------------------------------------------
// The models corresponding to the currently enabled scheme set (filtered
// against BtrBlocksConfig::get().integers.schemes at each call), so the
// planner never scores candidates the picker cannot actually choose. Built
// fresh per call rather than a stable shared instance list, since the
// enabled set can change at runtime.
std::vector<const ICostModel*> defaultCostModels();
// -------------------------------------------------------------------------------------
inline MetricFlags unionRequiredMetrics(const std::vector<const ICostModel*>& models) {
  MetricFlags flags = static_cast<MetricFlags>(MetricFlag::None);
  for (const auto* model : models) {
    flags |= model->requiredMetrics();
  }
  return flags;
}
// -------------------------------------------------------------------------------------
// Cheapest estimate across `models`, recording which one won.
inline double bestCostBits(const std::vector<const ICostModel*>& models,
                           const SegmentMetrics& metrics,
                           std::size_t numValues,
                           int bitWidth,
                           IntegerSchemeType& bestScheme) {
  double best = std::numeric_limits<double>::infinity();
  bestScheme = IntegerSchemeType::UNCOMPRESSED;
  for (const auto* model : models) {
    const double cost = model->costBits(metrics, numValues, bitWidth);
    if (cost < best) {
      best = cost;
      bestScheme = model->label();
    }
  }
  return best;
}
// -------------------------------------------------------------------------------------
}  // namespace btrblocks::subintsplit
// -------------------------------------------------------------------------------------
