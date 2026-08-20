// -------------------------------------------------------------------------------------
// Tests for the random-access API on Integer64Scheme/BIGINT columns -- the
// 64-bit sibling of RandomAccess.cpp. Mirrors that file's structure and
// intent: gather()/lookupAt() must agree with decompress()-then-index for
// every registered 64-bit scheme, which is what makes the API usable as a
// fair baseline across codecs.
// -------------------------------------------------------------------------------------
#include "TestHelper.hpp"
// -------------------------------------------------------------------------------------
#include "btrblocks.hpp"
#include "common/SIMD.hpp"
#include "common/Utils.hpp"
#include "compression/BtrReader.hpp"
#include "compression/Datablock.hpp"
#include "compression/SchemePicker.hpp"
#include "scheme/SchemePool.hpp"
#include "storage/Chunk.hpp"
#include "storage/Relation.hpp"
// -------------------------------------------------------------------------------------
#include "gtest/gtest.h"
// -------------------------------------------------------------------------------------
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>
// -------------------------------------------------------------------------------------
using namespace btrblocks;
// -------------------------------------------------------------------------------------
namespace {
// -------------------------------------------------------------------------------------
// Values that every 64-bit scheme registered by default can compress without
// tripping over its preconditions: a narrow non-negative range (safe for
// Truncation64's "range must fit in a u32 code" precondition, see
// scheme/integer64/Truncation64.hpp / scheme/integer/Truncation.hpp's
// ITruncCompress die_if), low cardinality (safe for Dictionary8_64/16_64's
// "distinct values must fit the code width" precondition, see
// scheme/templated/FixedDictionary.hpp's die_if), and runs (RLE64). Same
// shape as RandomAccess.cpp's makeUniversallyCompressibleData, just BIGINT.
std::vector<BIGINT> makeUniversallyCompressibleData64(u32 count, u32 seed = 42) {
  std::mt19937 gen(seed);
  std::vector<BIGINT> data;
  data.reserve(count);
  while (data.size() < count) {
    const auto value = static_cast<BIGINT>(gen() % 200);
    const u32 run = 1 + (gen() % 8);
    for (u32 i = 0; i < run && data.size() < count; i++) {
      data.push_back(value);
    }
  }
  return data;
}
// -------------------------------------------------------------------------------------
// Values that actually need more than 32 bits: high 32 bits vary across a
// wide range while the low 32 bits vary independently, so neither half is
// degenerate and (max - min) comfortably exceeds u32's range. This is what
// exercises BP64's halves-split design and FOR64's bias for real, unlike the
// narrow low-cardinality data above (which any 32-bit-range codec would
// handle trivially even if it were only wired for 64 bits nominally).
//
// Deliberately NOT run through Truncation64/Dictionary8_64/Dictionary16_64:
// full-range data blows both of their real preconditions (Truncation64 needs
// max-min to fit a u32 code; the fixed dictionaries need cardinality to fit
// their code width), so those three are excluded from the "every scheme"
// loop when this dataset is used.
std::vector<BIGINT> makeFullRange64Data(u32 count, u32 seed = 4242) {
  std::mt19937_64 gen(seed);
  std::vector<BIGINT> data;
  data.reserve(count);
  for (u32 i = 0; i < count; i++) {
    data.push_back(static_cast<BIGINT>(gen()));
  }
  return data;
}
// -------------------------------------------------------------------------------------
std::vector<u32> makeRandomPositions(u32 count, u32 upper_bound, u32 seed = 7) {
  std::mt19937 gen(seed);
  std::vector<u32> positions(count);
  for (auto& position : positions) {
    position = gen() % upper_bound;
  }
  return positions;
}
// -------------------------------------------------------------------------------------
// Runs the gather/lookupAt contract against one scheme on one dataset.
void checkGatherAgainstDecompress64(Integer64Scheme& scheme,
                                    const std::vector<BIGINT>& data,
                                    const std::string& scheme_name) {
  const auto tuple_count = static_cast<u32>(data.size());
  const std::vector<BITMAP> bitmap(tuple_count, 1);

  auto stats = SInteger64Stats::generateStats(data.data(), bitmap.data(), tuple_count);
  auto compressed = makeBytesArray(tuple_count * sizeof(BIGINT) * 10 + 4096);
  ASSERT_NO_THROW(scheme.compress(data.data(), bitmap.data(), compressed.get(), stats, 3))
      << "scheme " << scheme_name;

  BitmapWrapper nullmap(nullptr, BitmapType::ALLONES, tuple_count);

  // Reference: whole-chunk decompress. The SIMD slack mirrors the 32-bit test.
  std::vector<BIGINT> reference(tuple_count + SIMD_EXTRA_ELEMENTS(BIGINT));
  scheme.decompress(reference.data(), &nullmap, compressed.get(), tuple_count, 0);

  const auto positions = makeRandomPositions(std::min<u32>(tuple_count, 512), tuple_count);
  std::vector<BIGINT> gathered(positions.size());
  scheme.gather(gathered.data(), compressed.get(), &nullmap, tuple_count, positions.data(),
               static_cast<u32>(positions.size()), 0);

  for (std::size_t i = 0; i < positions.size(); i++) {
    ASSERT_EQ(gathered[i], reference[positions[i]])
        << "scheme " << scheme_name << ", gather index " << i << " (row " << positions[i] << ")";
  }

  // lookupAt must agree with gather on a handful of single rows.
  for (u32 i = 0; i < std::min<u32>(tuple_count, 16); i++) {
    const u32 position = positions[i];
    ASSERT_EQ(scheme.lookupAt(compressed.get(), &nullmap, tuple_count, position, 0),
              reference[position])
        << "scheme " << scheme_name << ", lookupAt row " << position;
  }
}
// -------------------------------------------------------------------------------------
// Same as above, but also asserts the decompressed reference matches the
// original data -- worth checking once directly (checkGatherAgainstDecompress64
// only checks gather/decompress self-consistency), and required for the
// full-range dataset since that is the case that would catch BP64/FOR64
// getting the high 32 bits wrong.
void checkDecompressMatchesOriginal64(Integer64Scheme& scheme,
                                      const std::vector<BIGINT>& data,
                                      const std::string& scheme_name) {
  const auto tuple_count = static_cast<u32>(data.size());
  auto stats = SInteger64Stats::generateStats(data.data(), nullptr, tuple_count);
  auto compressed = makeBytesArray(tuple_count * sizeof(BIGINT) * 10 + 4096);
  ASSERT_NO_THROW(scheme.compress(data.data(), nullptr, compressed.get(), stats, 3))
      << "scheme " << scheme_name;

  std::vector<BIGINT> reference(tuple_count + SIMD_EXTRA_ELEMENTS(BIGINT));
  scheme.decompress(reference.data(), nullptr, compressed.get(), tuple_count, 0);

  for (u32 i = 0; i < tuple_count; i++) {
    ASSERT_EQ(reference[i], data[i]) << "scheme " << scheme_name << ", row " << i;
  }
}
// -------------------------------------------------------------------------------------
}  // namespace
// -------------------------------------------------------------------------------------
TEST(RandomAccess64, Begin) {
  auto schemes = Integer64SchemeSet{};
  schemes.enableAll();
  BtrBlocksConfig::get().integers64.schemes = schemes;
  BtrBlocksConfig::get().integers.schemes = defaultIntegerSchemes();
  BtrBlocksConfig::get().doubles.schemes = defaultDoubleSchemes();
  BtrBlocksConfig::get().strings.schemes = defaultStringSchemes();
  SchemePool::refresh();
}
// -------------------------------------------------------------------------------------
// The headline test: the default gather()/lookupAt() implementations must be
// correct for every registered 64-bit scheme, with no per-scheme work.
TEST(RandomAccess64, GatherMatchesDecompressForEveryScheme) {
  const auto data = makeUniversallyCompressibleData64(9000);
  ASSERT_FALSE(SchemePool::available_schemes->integer64_schemes.empty());

  for (auto& entry : SchemePool::available_schemes->integer64_schemes) {
    const auto scheme_name = ConvertSchemeTypeToString(entry.first);
    SCOPED_TRACE("scheme = " + scheme_name);
    checkGatherAgainstDecompress64(*entry.second, data, scheme_name);
  }
}
// -------------------------------------------------------------------------------------
TEST(RandomAccess64, GatherMatchesDecompressOnConstantData) {
  const std::vector<BIGINT> data(4096, 0x1234567890ABCDEFll);

  for (auto& entry : SchemePool::available_schemes->integer64_schemes) {
    const auto scheme_name = ConvertSchemeTypeToString(entry.first);
    SCOPED_TRACE("scheme = " + scheme_name);
    checkGatherAgainstDecompress64(*entry.second, data, scheme_name);
  }
}
// -------------------------------------------------------------------------------------
// Values spanning the full 64-bit range -- what actually exercises the high
// 32 bits of BP64's halves-split design and FOR64's bias, unlike the narrow
// low-cardinality data above. This only checks gather/decompress
// self-consistency (like GatherMatchesDecompressForEveryScheme), not
// agreement with the original data: several schemes (ONE_VALUE64,
// TRUNCATION64, DICTIONARY_8/16_64, and effectively RLE64/FREQUENCY64 once
// their "small number of distinct values" assumption stops holding) only
// promise correctness under a precondition this dataset deliberately
// violates (constant value / narrow range / low cardinality) -- calling
// compress() on data outside that precondition is a misuse, not a bug, and
// is exactly why the 32-bit RandomAccess.cpp test never checks against
// original data either. Schemes that ARE supposed to be correct for
// arbitrary 64-bit data get a real correctness check in
// FullRangeCorrectness below instead.
TEST(RandomAccess64, GatherMatchesDecompressOnFullRangeData) {
  const auto data = makeFullRange64Data(9000);
  ASSERT_FALSE(SchemePool::available_schemes->integer64_schemes.empty());

  // Truncation64/Dictionary8_64/Dictionary16_64 are deliberately excluded --
  // see makeFullRange64Data's comment. Calling compress() on data outside a
  // scheme's precondition is a misuse, not a bug: Truncation64's own die_if
  // aborts the process rather than gracefully rejecting, so this is not
  // optional the way it is for e.g. a scheme that would just compress badly.
  for (auto& entry : SchemePool::available_schemes->integer64_schemes) {
    if (entry.first == Integer64SchemeType::TRUNCATION ||
        entry.first == Integer64SchemeType::DICTIONARY_8 ||
        entry.first == Integer64SchemeType::DICTIONARY_16) {
      continue;
    }
    const auto scheme_name = ConvertSchemeTypeToString(entry.first);
    SCOPED_TRACE("scheme = " + scheme_name);
    checkGatherAgainstDecompress64(*entry.second, data, scheme_name);
  }
}
// -------------------------------------------------------------------------------------
// The schemes that ARE supposed to be correct for arbitrary 64-bit data, with
// no cardinality/range/constant-value precondition: Uncompressed64 trivially,
// BP64 because its halves-split must handle full-width values, PFOR64
// because it's built the same way BP64 is (halves-split, just forcing PFOR
// on each half instead of auto-picking), FOR64 because its bias must handle
// a full-width min, and DynamicDictionary64 because it recurses into the
// ordinary picker for its (unbounded-width) codes rather than assuming a
// fixed code width.
TEST(RandomAccess64, FullRangeCorrectness) {
  const auto data = makeFullRange64Data(9000);
  for (const auto type : {Integer64SchemeType::UNCOMPRESSED, Integer64SchemeType::BP,
                          Integer64SchemeType::PFOR, Integer64SchemeType::FOR,
                          Integer64SchemeType::DICT}) {
    auto& scheme = Integer64SchemePicker::MyTypeWrapper::getScheme(CB(type));
    const auto scheme_name = ConvertSchemeTypeToString(type);
    SCOPED_TRACE("scheme = " + scheme_name);
    checkDecompressMatchesOriginal64(scheme, data, scheme_name);
  }
}
// -------------------------------------------------------------------------------------
TEST(RandomAccess64, GatherHandlesEmptyRequests) {
  const auto data = makeUniversallyCompressibleData64(1024);
  const auto tuple_count = static_cast<u32>(data.size());
  const std::vector<BITMAP> bitmap(tuple_count, 1);

  auto& scheme =
      Integer64SchemePicker::MyTypeWrapper::getScheme(CB(Integer64SchemeType::UNCOMPRESSED));
  auto stats = SInteger64Stats::generateStats(data.data(), bitmap.data(), tuple_count);
  auto compressed = makeBytesArray(tuple_count * sizeof(BIGINT) * 10 + 4096);
  scheme.compress(data.data(), bitmap.data(), compressed.get(), stats, 3);

  BitmapWrapper nullmap(nullptr, BitmapType::ALLONES, tuple_count);

  // Zero positions and zero tuples must both be no-ops rather than crashes.
  BIGINT sentinel = -1;
  scheme.gather(&sentinel, compressed.get(), &nullmap, tuple_count, nullptr, 0, 0);
  ASSERT_EQ(sentinel, -1);

  const u32 position = 0;
  scheme.gather(&sentinel, compressed.get(), &nullmap, 0, &position, 1, 0);
  ASSERT_EQ(sentinel, -1);
}
// -------------------------------------------------------------------------------------
// Column-level gather spans chunks, and chunks may use different schemes.
// block_size is forced small so the column is genuinely multi-chunk.
TEST(RandomAccess64, GatherColumnMatchesReadColumn) {
  const u32 saved_block_size = BtrBlocksConfig::get().block_size;
  BtrBlocksConfig::get().block_size = 8192;

  const u32 tuple_count = 50000;
  const auto data = makeUniversallyCompressibleData64(tuple_count, 99);

  Relation relation;
  {
    Vector<BIGINT> column_data(tuple_count);
    for (u32 i = 0; i < tuple_count; i++) {
      column_data[i] = data[i];
    }
    relation.addColumn({"ids", std::move(column_data)});
  }

  const auto ranges = relation.getRanges(SplitStrategy::SEQUENTIAL, 9999);
  ASSERT_GT(ranges.size(), 1u) << "test needs a multi-chunk column";

  ColumnPart part;
  for (std::size_t chunk_i = 0; chunk_i < ranges.size(); chunk_i++) {
    auto input_chunk = relation.getInputChunk(ranges[chunk_i], chunk_i, 0);
    auto compressed = Datablock::compress(input_chunk);
    part.addCompressedChunk(std::move(compressed));
  }

  const std::string path = "gather_column64_test.btr";
  part.writeToDisk(path);

  std::vector<char> file_contents;
  Utils::readFileToMemory(path, file_contents);
  BtrReader reader(file_contents.data());

  ASSERT_GT(reader.getChunkCount(), 1u);

  // Reference: decode every chunk and concatenate.
  std::vector<BIGINT> reference;
  reference.reserve(tuple_count);
  for (u32 chunk_i = 0; chunk_i < reader.getChunkCount(); chunk_i++) {
    std::vector<u8> output;
    reader.readColumn(output, chunk_i);
    const auto* values = reinterpret_cast<const BIGINT*>(output.data());
    for (u32 i = 0; i < reader.getTupleCount(chunk_i); i++) {
      reference.push_back(values[i]);
    }
  }
  ASSERT_EQ(reference.size(), tuple_count);

  const auto positions = makeRandomPositions(4096, tuple_count, 3);
  std::vector<BIGINT> gathered(positions.size());
  u32 chunks_touched = 0;
  reader.gatherColumn64(gathered.data(), positions.data(), static_cast<u32>(positions.size()),
                        &chunks_touched);

  for (std::size_t i = 0; i < positions.size(); i++) {
    ASSERT_EQ(gathered[i], reference[positions[i]]) << "row " << positions[i];
  }
  // With this many uniform positions every chunk is touched -- the effect that
  // makes uniform gather degenerate into a full column decode.
  ASSERT_EQ(chunks_touched, reader.getChunkCount());

  // A single lookup must touch exactly one chunk.
  ASSERT_EQ(reader.lookupColumn64(12345), reference[12345]);

  const u32 single = 40000;
  BIGINT single_result = 0;
  reader.gatherColumn64(&single_result, &single, 1, &chunks_touched);
  ASSERT_EQ(single_result, reference[single]);
  ASSERT_EQ(chunks_touched, 1u);

  std::remove(path.c_str());
  BtrBlocksConfig::get().block_size = saved_block_size;
}
// -------------------------------------------------------------------------------------
// Confirms BP64 actually exercises both halves: values that need more than 32
// bits, per earlier smoke-testing in this session ((1LL<<40) + i).
TEST(RandomAccess64, BP64ExercisesBothHalves) {
  const u32 tuple_count = 8192;
  std::vector<BIGINT> data(tuple_count);
  for (u32 i = 0; i < tuple_count; i++) {
    data[i] = (static_cast<BIGINT>(1) << 40) + static_cast<BIGINT>(i);
  }

  auto& scheme = Integer64SchemePicker::MyTypeWrapper::getScheme(CB(Integer64SchemeType::BP));
  checkGatherAgainstDecompress64(scheme, data, "BP64");
  checkDecompressMatchesOriginal64(scheme, data, "BP64");
}
// -------------------------------------------------------------------------------------
// The picker (via Datablock/Relation, the real production path) must select
// sensible schemes for a few representative shapes rather than always
// falling back to the same one. Deliberately uses defaultInteger64Schemes()
// (the real production default set) rather than whatever the surrounding
// tests in this file have enabled, so the result reflects what BtrBlocks
// actually does out of the box.
TEST(Integer64Picker, ChoosesSensibleSchemesForRepresentativeDatasets) {
  const auto saved_schemes = BtrBlocksConfig::get().integers64.schemes;
  BtrBlocksConfig::get().integers64.schemes = defaultInteger64Schemes();
  SchemePool::refresh();

  const auto compressAndDescribe = [](std::vector<BIGINT> data) -> std::string {
    Relation relation;
    Vector<BIGINT> column(static_cast<u64>(data.size()));
    for (std::size_t i = 0; i < data.size(); i++) {
      column[i] = data[i];
    }
    relation.addColumn({"ids", std::move(column)});
    const auto ranges = relation.getRanges(SplitStrategy::SEQUENTIAL, 999999);
    auto input_chunk = relation.getInputChunk(ranges[0], 0, 0);
    auto compressed = Datablock::compress(input_chunk);

    ColumnPart part;
    part.addCompressedChunk(std::move(compressed));
    const std::string path = "integer64_picker_test.btr";
    part.writeToDisk(path);

    std::vector<char> file_contents;
    Utils::readFileToMemory(path, file_contents);
    BtrReader reader(file_contents.data());
    // The basic (top-level-only) description: fullDescription() recurses into
    // nested child schemes (e.g. BP64's low/high halves each further wrapping
    // an RLE/DICT child), whose own description can legitimately contain
    // "UNCOMPRESSED" as a leaf even when the top-level scheme is not -- see
    // the BP64 -> RLE -> counts:UNCOMPRESSED example this test caught.
    const auto description = reader.getBasicSchemeDescription(0);
    std::remove(path.c_str());
    return description;
  };

  {
    // A single repeated value: OneValue64 is the only sane choice.
    const std::vector<BIGINT> data(4096, 777777777777ll);
    const auto description = compressAndDescribe(data);
    EXPECT_NE(description.find("ONE_VALUE"), std::string::npos)
        << "constant data picked: " << description;
  }
  {
    // Highly repetitive with a handful of distinct values: some dictionary or
    // RLE-flavoured scheme should win, never plain UNCOMPRESSED.
    std::mt19937 gen(5);
    std::vector<BIGINT> data(8192);
    for (auto& value : data) {
      value = static_cast<BIGINT>(gen() % 8);
    }
    const auto description = compressAndDescribe(data);
    EXPECT_EQ(description.find("UNCOMPRESSED"), std::string::npos)
        << "highly repetitive data fell back to UNCOMPRESSED: " << description;
  }
  {
    // Snowflake-like structured IDs: SubIntSplit is opt-in-only, so with the
    // default 64-bit scheme set this should land on FOR64 or BP64 rather than
    // raw uncompressed -- the range is far too structured for that.
    std::mt19937_64 gen(11);
    std::vector<BIGINT> data(16384);
    uint64_t timestamp = 1700000000000ull;
    uint64_t sequence = 0;
    for (auto& value : data) {
      const uint64_t shard = gen() % 16;
      value = static_cast<BIGINT>((timestamp << 23) | (shard << 10) | sequence);
      if (++sequence == 1024) {
        sequence = 0;
        timestamp++;
      }
    }
    const auto description = compressAndDescribe(data);
    EXPECT_EQ(description.find("UNCOMPRESSED"), std::string::npos)
        << "structured snowflake IDs fell back to UNCOMPRESSED: " << description;
  }

  BtrBlocksConfig::get().integers64.schemes = saved_schemes;
  SchemePool::refresh();
}
// -------------------------------------------------------------------------------------
TEST(RandomAccess64, End) {
  BtrBlocksConfig::get().integers64.schemes = defaultInteger64Schemes();
  SchemePool::refresh();
}
// -------------------------------------------------------------------------------------
