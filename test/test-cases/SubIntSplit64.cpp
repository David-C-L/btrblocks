// -------------------------------------------------------------------------------------
// Round-trip tests for the 64-bit SubIntSplit variant.
//
// SubIntSplit64 is a registered Integer64Scheme now, but these tests still
// drive it directly (constructing an instance and calling compress/decompress
// /gather/lookupAt) rather than through Relation/Datablock, so they can
// inspect the encoded buffer's header/section layout directly (section
// count, chosen scheme codes, forced-boundary description) the way the
// higher-level pipeline doesn't expose. Sections of at most 32 bits are
// compressed by the ordinary 32-bit scheme pool, wider ones by the 64-bit pool.
// -------------------------------------------------------------------------------------
#include "TestHelper.hpp"
// -------------------------------------------------------------------------------------
#include "btrblocks.hpp"
#include "scheme/SchemePool.hpp"
#include "scheme/integer/SubIntSplit64.hpp"
#include "scheme/integer/subintsplit/Plan.hpp"
#include "scheme/integer/subintsplit/SubIntSplitCore.hpp"
// -------------------------------------------------------------------------------------
#include "gtest/gtest.h"
// -------------------------------------------------------------------------------------
#include <cstdint>
#include <limits>
#include <random>
#include <vector>
// -------------------------------------------------------------------------------------
using namespace btrblocks;
using namespace btrblocks::integers;
using namespace btrblocks::subintsplit;
// -------------------------------------------------------------------------------------
namespace {
// -------------------------------------------------------------------------------------
// The Instagram snowflake layout: 41-bit millisecond timestamp, 13-bit shard
// id, 10-bit per-millisecond sequence, sign bit always zero.
std::vector<s64> makeSnowflake64(std::size_t count, uint32_t seed = 1) {
  std::mt19937_64 gen(seed);
  std::vector<s64> values;
  values.reserve(count);
  uint64_t timestamp = 1700000000000ull;
  uint64_t sequence = 0;
  for (std::size_t i = 0; i < count; i++) {
    const uint64_t shard = gen() % 16;
    values.push_back(static_cast<s64>((timestamp << 23) | (shard << 10) | sequence));
    if (++sequence == 1024) {
      sequence = 0;
      timestamp++;
    }
  }
  return values;
}
// -------------------------------------------------------------------------------------
// Round-trips through SubIntSplit64 and asserts every value returns. Returns
// the encoded size in bytes.
u32 roundTrip64(const std::vector<s64>& data, u8 cascade_level = 3) {
  const auto tuple_count = static_cast<u32>(data.size());
  auto compressed = makeBytesArray(SubIntSplit64::maxCompressedSize(tuple_count));
  SubIntSplit64 scheme;
  SInteger64Stats stats = SInteger64Stats::generateStats(data.data(), nullptr, tuple_count);
  const u32 size =
      scheme.compress(data.data(), nullptr, compressed.get(), stats, cascade_level);

  std::vector<s64> decoded(tuple_count + 64);
  scheme.decompress(decoded.data(), nullptr, compressed.get(), tuple_count, 0);

  for (u32 i = 0; i < tuple_count; i++) {
    EXPECT_EQ(decoded[i], data[i]) << "row " << i;
    if (decoded[i] != data[i]) {
      break;
    }
  }
  return size;
}
// -------------------------------------------------------------------------------------
}  // namespace
// -------------------------------------------------------------------------------------
TEST(SubIntSplit64, Begin) {
  BtrBlocksConfig::get().integers.schemes = defaultIntegerSchemes();
  BtrBlocksConfig::get().integers.schemes.enable(IntegerSchemeType::SUB_INT_SPLIT);
  SchemePool::refresh();
}
// -------------------------------------------------------------------------------------
TEST(SubIntSplit64, RoundTripSnowflake) {
  roundTrip64(makeSnowflake64(65536));
}
// -------------------------------------------------------------------------------------
TEST(SubIntSplit64, RoundTripEdgeSizes) {
  for (const u32 count : {0u, 1u, 7u, 127u, 128u, 129u, 640u, 65536u}) {
    SCOPED_TRACE("tuple_count = " + std::to_string(count));
    roundTrip64(makeSnowflake64(count));
  }
}
// -------------------------------------------------------------------------------------
TEST(SubIntSplit64, RoundTripValueEdges) {
  const u32 count = 4096;

  {
    SCOPED_TRACE("all zero");
    roundTrip64(std::vector<s64>(count, 0));
  }
  {
    SCOPED_TRACE("all same");
    roundTrip64(std::vector<s64>(count, 0x0123456789ABCDEFll));
  }
  {
    // Every bit set, which puts the sign bit inside the top section.
    SCOPED_TRACE("all bits set");
    roundTrip64(std::vector<s64>(count, -1));
  }
  {
    SCOPED_TRACE("negatives");
    std::vector<s64> data(count);
    for (u32 i = 0; i < count; i++) {
      data[i] = -static_cast<s64>(i) - 1;
    }
    roundTrip64(data);
  }
  {
    SCOPED_TRACE("extremes");
    std::vector<s64> data(count);
    for (u32 i = 0; i < count; i++) {
      data[i] = (i % 2 == 0) ? std::numeric_limits<s64>::min() : std::numeric_limits<s64>::max();
    }
    roundTrip64(data);
  }
  {
    SCOPED_TRACE("uniform random");
    std::mt19937_64 gen(29);
    std::vector<s64> data(count);
    for (auto& value : data) {
      value = static_cast<s64>(gen());
    }
    roundTrip64(data);
  }
}
// -------------------------------------------------------------------------------------
TEST(SubIntSplit64, EncodedSizeNeverExceedsRaw) {
  std::mt19937_64 gen(31);
  const u32 count = 32768;
  std::vector<s64> data(count);
  for (auto& value : data) {
    value = static_cast<s64>(gen());
  }

  const u32 size = roundTrip64(data);
  const u32 raw = count * sizeof(s64);
  ASSERT_LE(size, raw + 64) << "encoded " << size << " vs raw " << raw;
}
// -------------------------------------------------------------------------------------
TEST(SubIntSplit64, TerminatesAtEveryCascadeDepth) {
  const auto data = makeSnowflake64(8192);
  for (u8 depth = 1; depth <= 4; depth++) {
    SCOPED_TRACE("max_cascade_depth = " + std::to_string(depth));
    roundTrip64(data, depth);
  }
}
// -------------------------------------------------------------------------------------
// A section of at most 32 bits goes to the 32-bit scheme picker and a wider
// one to the 64-bit picker, flagged in its descriptor; none may be SubIntSplit
// itself. The 64-bit path is where the nesting guard matters most: it is
// free-standing, so it always starts at the shallowest cascade depth.
TEST(SubIntSplit64, SectionsFitTheirPool) {
  const auto data = makeSnowflake64(16384);
  const auto tuple_count = static_cast<u32>(data.size());
  auto compressed = makeBytesArray(SubIntSplit64::maxCompressedSize(tuple_count));
  SubIntSplit64 scheme;
  SInteger64Stats stats = SInteger64Stats::generateStats(data.data(), nullptr, tuple_count);

  ASSERT_EQ(ThreadCache::get().compression_level, 0);
  scheme.compress(data.data(), nullptr, compressed.get(), stats, 3);
  ASSERT_EQ(ThreadCache::get().compression_level, 0);

  const auto* header = reinterpret_cast<const SubIntSplitHeader*>(compressed.get());
  const auto* descriptors = reinterpret_cast<const SectionDescriptor*>(header->data);
  ASSERT_EQ(header->value_bits, 64);
  ASSERT_GT(header->section_count, 1) << "structured input should have been split";

  for (u8 s = 0; s < header->section_count; s++) {
    const int width = descriptors[s].bit_end - descriptors[s].bit_start + 1;
    const bool wide = (descriptors[s].flags & kSectionWide) != 0;
    ASSERT_EQ(wide, width > narrowSectionMaxBits())
        << "section " << static_cast<int>(s) << " went to the wrong pool";
    const u8 sis_code =
        wide ? CB(Integer64SchemeType::SUB_INT_SPLIT) : CB(IntegerSchemeType::SUB_INT_SPLIT);
    ASSERT_NE(descriptors[s].scheme_code, sis_code)
        << "section " << static_cast<int>(s) << " recursed into SubIntSplit";
  }
}
// -------------------------------------------------------------------------------------
// A 40-bit field drawn from 64 distinct values, under a slowly changing 24-bit
// field. The 40-bit field is best kept whole: one dictionary with 6-bit codes,
// where any cut at or below bit 32 pays for the codes twice (the two halves are
// correlated, so each keeps all 64 values). The 32-bit section cap forbade
// that; wide sections go to the 64-bit pool, so the planner must now choose a
// section wider than 32 bits, and it must round-trip through decode and gather.
TEST(SubIntSplit64, WideSectionChosenAndRoundTrips) {
  std::mt19937_64 gen(5);
  std::vector<uint64_t> alphabet(64);
  for (auto& symbol : alphabet) {
    symbol = (gen() & ((uint64_t{1} << 40) - 1)) | (uint64_t{1} << 39);
  }
  std::vector<s64> data(32768);
  for (std::size_t i = 0; i < data.size(); i++) {
    const uint64_t high = i / 1000;
    data[i] = static_cast<s64>((high << 40) | alphabet[gen() % alphabet.size()]);
  }
  const auto tuple_count = static_cast<u32>(data.size());

  roundTrip64(data);

  auto compressed = makeBytesArray(SubIntSplit64::maxCompressedSize(tuple_count));
  SubIntSplit64 scheme;
  SInteger64Stats stats = SInteger64Stats::generateStats(data.data(), nullptr, tuple_count);
  const u32 size = scheme.compress(data.data(), nullptr, compressed.get(), stats, 3);

  const auto* header = reinterpret_cast<const SubIntSplitHeader*>(compressed.get());
  const auto* descriptors = reinterpret_cast<const SectionDescriptor*>(header->data);
  int widest = 0;
  bool wide_flagged = false;
  for (u8 s = 0; s < header->section_count; s++) {
    const int width = descriptors[s].bit_end - descriptors[s].bit_start + 1;
    if (width > widest) {
      widest = width;
      wide_flagged = (descriptors[s].flags & kSectionWide) != 0;
    }
  }
  ASSERT_GT(widest, 32) << scheme.fullDescription(compressed.get());
  ASSERT_TRUE(wide_flagged) << "a section wider than 32 bits must use the 64-bit pool";

  std::mt19937 pick(41);
  std::vector<u32> positions(1024);
  for (auto& position : positions) {
    position = pick() % tuple_count;
  }
  std::vector<s64> gathered(positions.size());
  scheme.gather(gathered.data(), compressed.get(), nullptr, tuple_count, positions.data(),
               static_cast<u32>(positions.size()), 0);
  for (std::size_t i = 0; i < positions.size(); i++) {
    ASSERT_EQ(gathered[i], data[positions[i]]) << "row " << positions[i];
  }

  // And it must beat the old cap, which had to cut the field at bit 32.
  u32 capped_size = 0;
  {
    auto& tuning = SchemeConfig::get().integers.subintsplit;
    const auto saved = tuning.max_section_bits;
    tuning.max_section_bits = 32;
    capped_size = roundTrip64(data);
    tuning.max_section_bits = saved;
  }
  ASSERT_LT(size, capped_size) << "wide " << size << " vs 32-bit cap " << capped_size;
}
// -------------------------------------------------------------------------------------
// Data encoded at one width must not be decodable at the other. Both widths
// share a format, so without the check a 64-bit encoding read as 32-bit would
// silently produce garbage.
TEST(SubIntSplit64, RejectsWidthMismatch) {
  const auto data = makeSnowflake64(1024);
  const auto tuple_count = static_cast<u32>(data.size());
  auto compressed = makeBytesArray(SubIntSplit64::maxCompressedSize(tuple_count));
  SubIntSplit64 scheme;
  SInteger64Stats stats = SInteger64Stats::generateStats(data.data(), nullptr, tuple_count);
  scheme.compress(data.data(), nullptr, compressed.get(), stats, 3);

  std::vector<u32> decoded(data.size() + 64);
  ASSERT_THROW(SubIntSplitCore<u32>::decode(decoded.data(), compressed.get(),
                                            static_cast<u32>(data.size()), 0),
               Generic_Exception);
}
// -------------------------------------------------------------------------------------
TEST(SubIntSplit64, GatherMatchesDecompress) {
  const auto data = makeSnowflake64(20000);
  const auto tuple_count = static_cast<u32>(data.size());
  auto compressed = makeBytesArray(SubIntSplit64::maxCompressedSize(tuple_count));
  SubIntSplit64 scheme;
  SInteger64Stats stats = SInteger64Stats::generateStats(data.data(), nullptr, tuple_count);
  scheme.compress(data.data(), nullptr, compressed.get(), stats, 3);

  std::mt19937 gen(37);
  std::vector<u32> positions(1024);
  for (auto& position : positions) {
    position = gen() % tuple_count;
  }

  std::vector<s64> gathered(positions.size());
  scheme.gather(gathered.data(), compressed.get(), nullptr, tuple_count, positions.data(),
               static_cast<u32>(positions.size()), 0);

  for (std::size_t i = 0; i < positions.size(); i++) {
    ASSERT_EQ(gathered[i], data[positions[i]]) << "row " << positions[i];
  }
  ASSERT_EQ(scheme.lookupAt(compressed.get(), nullptr, tuple_count, 4321, 0), data[4321]);
}
// -------------------------------------------------------------------------------------
// The control arm the benchmark compares the planner against: the naive
// halves-split, driven through the same machinery so only the boundaries differ.
TEST(SubIntSplit64, ForcedHalvesSplit) {
  const auto data = makeSnowflake64(16384);

  std::vector<SegmentPlan> boundaries;
  ASSERT_TRUE(parseSplitBoundaries("0-31;32-63", 64, boundaries));

  EnforceSplitBoundaries enforcer(boundaries);
  roundTrip64(data);

  const auto tuple_count = static_cast<u32>(data.size());
  auto compressed = makeBytesArray(SubIntSplit64::maxCompressedSize(tuple_count));
  SubIntSplit64 scheme;
  SInteger64Stats stats = SInteger64Stats::generateStats(data.data(), nullptr, tuple_count);
  scheme.compress(data.data(), nullptr, compressed.get(), stats, 3);
  ASSERT_EQ(SubIntSplit64::sectionCount(compressed.get()), 2);
  ASSERT_NE(scheme.fullDescription(compressed.get()).find("0-31;32-63"), std::string::npos);
}
// -------------------------------------------------------------------------------------
// The whole point: a planned split should beat the naive halves split on data
// whose fields do not fall on a 32-bit boundary.
TEST(SubIntSplit64, PlannedSplitBeatsHalvesSplit) {
  const auto data = makeSnowflake64(65536, 3);

  u32 halves_size = 0;
  {
    std::vector<SegmentPlan> boundaries;
    ASSERT_TRUE(parseSplitBoundaries("0-31;32-63", 64, boundaries));
    EnforceSplitBoundaries enforcer(boundaries);
    halves_size = roundTrip64(data);
  }
  const u32 planned_size = roundTrip64(data);

  ASSERT_LT(planned_size, halves_size)
      << "planned " << planned_size << " vs halves " << halves_size;
}
// -------------------------------------------------------------------------------------
TEST(SubIntSplit64, End) {
  BtrBlocksConfig::get().integers.schemes = defaultIntegerSchemes();
  SchemePool::refresh();
}
// -------------------------------------------------------------------------------------
