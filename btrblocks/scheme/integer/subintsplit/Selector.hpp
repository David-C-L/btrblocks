#pragma once
// -------------------------------------------------------------------------------------
#include "scheme/integer/subintsplit/CostModels.hpp"
#include "scheme/integer/subintsplit/Metrics.hpp"
#include "scheme/integer/subintsplit/Plan.hpp"
// -------------------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <vector>
// -------------------------------------------------------------------------------------
// Split planner.
//
// Scores every candidate bit range [l..r] on a sample, then runs a dynamic
// program over bit positions to find the cheapest way to tile the whole value
// with non-overlapping ranges.
// -------------------------------------------------------------------------------------
namespace btrblocks::subintsplit {
// -------------------------------------------------------------------------------------
struct SelectorConfig {
  int minSectionBits{1};
  int maxSectionBits{32};
  // Widest section the 32-bit scheme picker takes. Wider ones (possible only
  // when maxSectionBits exceeds this, i.e. for 64-bit columns) are encoded by
  // the 64-bit picker and priced at 64 bits of storage per value.
  int narrowSectionBits{32};
  int maxSections{8};
  // Bits charged for each split beyond the first. Without it the DP would
  // happily carve the value into many tiny sections whose individual savings
  // do not pay for their per-section header and decode pass.
  double splitPenalty{10.0};
};
// -------------------------------------------------------------------------------------
SelectorConfig defaultSelectorConfig(int valueBits = 32);
// -------------------------------------------------------------------------------------
// Materializes the values of a bit range, extending the range one bit at a time
// so the inner loop of the scoring grid reuses the previous range's work
// instead of re-extracting from scratch.
class BitRangeExtractor {
 public:
  explicit BitRangeExtractor(const std::vector<uint64_t>& samples)
      : samples_(samples), values_(samples.size(), 0) {}

  void reset(int bitStart) {
    bitStart_ = bitStart;
    bitEnd_ = bitStart;
    for (std::size_t i = 0; i < samples_.size(); i++) {
      values_[i] = (samples_[i] >> bitStart) & uint64_t{1};
    }
  }

  void extend(int bitEnd) {
    for (int b = bitEnd_ + 1; b <= bitEnd; b++) {
      const int shift = b - bitStart_;
      for (std::size_t i = 0; i < samples_.size(); i++) {
        values_[i] |= ((samples_[i] >> b) & uint64_t{1}) << shift;
      }
    }
    bitEnd_ = std::max(bitEnd_, bitEnd);
  }

  const std::vector<uint64_t>& values() const { return values_; }

 private:
  const std::vector<uint64_t>& samples_;
  std::vector<uint64_t> values_;
  int bitStart_{0};
  int bitEnd_{-1};
};
// -------------------------------------------------------------------------------------
// Plan a split of `totalBits`-wide values, given a sample of their bit patterns.
//
// `fullCount` is the length of the whole stream; per-sample costs are scaled up
// to it so the split penalty is denominated in the same units as the savings.
//
// Always returns at least one segment covering the whole value.
SplitPlan selectSplits(const std::vector<uint64_t>& samples,
                       int totalBits,
                       std::size_t fullCount,
                       const std::vector<const ICostModel*>& models,
                       const SelectorConfig& cfg);
// -------------------------------------------------------------------------------------
}  // namespace btrblocks::subintsplit
// -------------------------------------------------------------------------------------
