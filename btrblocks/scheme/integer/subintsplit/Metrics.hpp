#pragma once
// -------------------------------------------------------------------------------------
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
// -------------------------------------------------------------------------------------
// Per-segment metric collection for the SubIntSplit split planner.
//
// Deliberately minimal: only the statistics the cost models actually consume,
// with no cardinality sketch, no entropy and no residual-frame tracking. The
// planner calls compute() once for every bit range in an O(bits^2) grid, so the
// per-call cost matters far more than the richness of the statistics.
// -------------------------------------------------------------------------------------
namespace btrblocks::subintsplit {
// -------------------------------------------------------------------------------------
enum class MetricFlag : uint32_t {
  None = 0,
  MinMax = 1u << 0,         // min, max, range
  RunStats = 1u << 1,       // runCount, avgRunLength
  UniqueCount = 1u << 2,    // uniqueCount (capped)
  DominantValue = 1u << 3,  // frequency of the most common value
  All = (1u << 4) - 1,
};
using MetricFlags = uint32_t;
// -------------------------------------------------------------------------------------
inline constexpr MetricFlags operator|(MetricFlag a, MetricFlag b) noexcept {
  return static_cast<MetricFlags>(a) | static_cast<MetricFlags>(b);
}
inline constexpr MetricFlags operator|(MetricFlags a, MetricFlag b) noexcept {
  return a | static_cast<MetricFlags>(b);
}
inline constexpr bool hasFlag(MetricFlags flags, MetricFlag f) noexcept {
  return (flags & static_cast<MetricFlags>(f)) != 0;
}
// -------------------------------------------------------------------------------------
struct SegmentMetrics {
  uint64_t min{0};
  uint64_t max{0};
  uint64_t range{0};

  std::size_t uniqueCount{0};
  bool uniqueCountCapped{false};

  // Frequency of the most common value; consumed by the frequency cost model.
  // Unreliable, and flagged capped, once cardinality exceeds the cap.
  std::size_t dominantCount{0};
  bool dominantCountCapped{false};

  std::size_t runCount{0};
  double avgRunLength{0.0};

  // Bits one value of this range occupies before its sub-scheme runs: 32 for a
  // section handed to the 32-bit scheme picker, 64 for one wider than that
  // picker accepts, which goes to the 64-bit picker instead. Not computed from
  // the values; the planner sets it from the range's width, so the cost models
  // price each section at the storage width it will actually be encoded at.
  int storageBits{32};
};
// -------------------------------------------------------------------------------------
// Single-pass collector over the values of one extracted bit range.
//
// Counting takes one of two paths. For ranges of at most kDirectCountBits the
// value space is small enough to count in a flat array indexed by value, which
// avoids hashing entirely; a dirty list records which slots were touched so the
// array can be cleared in time proportional to the distinct values seen rather
// than to its size. Wider ranges fall back to a hash map.
//
// Both the array and the map are reusable members. The planner calls compute()
// across the whole bit-range grid, so allocating per call dominated the cost.
class MetricCollector {
 public:
  static constexpr std::size_t kUniqueCountCap = 1u << 14;
  static constexpr int kDirectCountBits = 16;

  SegmentMetrics compute(const std::vector<uint64_t>& values,
                         int bitWidth,
                         MetricFlags flags = static_cast<MetricFlags>(MetricFlag::All)) {
    const bool doMinMax = hasFlag(flags, MetricFlag::MinMax);
    const bool doRun = hasFlag(flags, MetricFlag::RunStats);
    const bool doUnique = hasFlag(flags, MetricFlag::UniqueCount);
    const bool doDominant = hasFlag(flags, MetricFlag::DominantValue);
    // Unique and dominant counts share one frequency pass.
    const bool doFreq = doUnique || doDominant;

    SegmentMetrics out;
    const std::size_t n = values.size();
    if (n == 0) {
      return out;
    }

    if (doMinMax) {
      out.min = values[0];
      out.max = values[0];
    }
    if (doRun) {
      out.runCount = 1;
    }

    bool capped = false;
    std::size_t distinct = 0;
    uint32_t maxCount = 0;
    const bool direct = doFreq && bitWidth <= kDirectCountBits;

    if (direct) {
      counts_.assign(std::size_t{1} << bitWidth, 0u);
      touched_.clear();
    } else if (doFreq) {
      freqMap_.clear();
      freqMap_.reserve(std::min(n, kUniqueCountCap));
    }

    uint64_t prev = values[0];
    for (std::size_t i = 0; i < n; i++) {
      const uint64_t v = values[i];

      if (doMinMax) {
        if (v < out.min) {
          out.min = v;
        }
        if (v > out.max) {
          out.max = v;
        }
      }
      if (doRun && i > 0 && v != prev) {
        out.runCount++;
      }
      if (direct) {
        uint32_t& slot = counts_[static_cast<std::size_t>(v)];
        if (slot == 0) {
          distinct++;
          touched_.push_back(static_cast<uint32_t>(v));
        }
        const uint32_t count = ++slot;
        if (count > maxCount) {
          maxCount = count;
        }
      } else if (doFreq && !capped) {
        auto emplaced = freqMap_.emplace(v, 0u);
        const uint32_t count = ++emplaced.first->second;
        if (count > maxCount) {
          maxCount = count;
        }
        if (emplaced.second && freqMap_.size() > kUniqueCountCap) {
          capped = true;
        }
      }
      prev = v;
    }

    if (direct) {
      // Clear only what was touched, so the cost is in distinct values seen
      // rather than in the size of the counter array.
      for (const auto value : touched_) {
        counts_[value] = 0;
      }
      distinct = touched_.size();
    } else if (doFreq) {
      distinct = capped ? (kUniqueCountCap + 1) : freqMap_.size();
    }

    if (doMinMax) {
      out.range = out.max - out.min;
    }
    if (doRun) {
      out.avgRunLength = static_cast<double>(n) / static_cast<double>(out.runCount);
    }
    if (doUnique) {
      out.uniqueCount = distinct;
      out.uniqueCountCapped = capped;
    }
    if (doDominant) {
      out.dominantCount = maxCount;
      out.dominantCountCapped = capped;
    }
    return out;
  }

 private:
  // Direct counting path, for narrow bit ranges.
  std::vector<uint32_t> counts_;
  std::vector<uint32_t> touched_;
  // Hash path, for ranges too wide to index directly.
  std::unordered_map<uint64_t, uint32_t> freqMap_;
};
// -------------------------------------------------------------------------------------
}  // namespace btrblocks::subintsplit
// -------------------------------------------------------------------------------------
