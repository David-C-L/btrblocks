// -------------------------------------------------------------------------------------
// SubIntSplit benchmark.
//
// Compares SubIntSplit against the integer codecs BtrBlocks already has, across
// encode time, compression ratio, and the bulk, gather and point decode paths.
//
// Both arms run through Relation, Datablock and BtrReader, so they measure the
// real chunked storage path rather than a synthetic one. A 64-bit column's
// SubIntSplit sections of at most 32 bits are compressed by the ordinary 32-bit
// pool and wider ones by the 64-bit pool, so the comparison is against the same
// codecs either way. --shared-codecs holds both pools to the cross-format set.
//
// Every read is checked against the input; the `validated` column says whether
// all of them matched.
//
// Two control arms matter for interpreting the results:
//   - a fixed halves split (0-31;32-63), which isolates what the planner's
//     choice of boundaries is worth from what splitting at all is worth;
//   - uniformly random data, where there is no bit-range structure and
//     splitting should not help.
// -------------------------------------------------------------------------------------
#include "SnowflakeGen.hpp"
#include "Traces.hpp"
// -------------------------------------------------------------------------------------
#include "btrblocks.hpp"
#include "common/Utils.hpp"
#include "compression/BtrReader.hpp"
#include "compression/Datablock.hpp"
#include "compression/SchemePicker.hpp"
#include "scheme/SchemePool.hpp"
#include "scheme/integer/SubIntSplit64.hpp"
#include "scheme/integer/subintsplit/Plan.hpp"
#include "storage/Chunk.hpp"
#include "storage/Relation.hpp"
// -------------------------------------------------------------------------------------
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <ctime>
#include <iostream>
#include <random>
#include <thread>
#include <type_traits>
#include <sstream>
#include <string>
#include <vector>
// -------------------------------------------------------------------------------------
using namespace btrblocks;
using namespace btrblocks::subintsplit_bench;
// -------------------------------------------------------------------------------------
namespace {
// -------------------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
// -------------------------------------------------------------------------------------
// Median of `repeats` runs. Median rather than mean because a single scheduling
// hiccup should not move the reported number.
double timeMedian(int repeats, const std::function<void()>& body) {
  std::vector<double> samples;
  samples.reserve(repeats);
  for (int i = 0; i < repeats; i++) {
    const auto start = Clock::now();
    body();
    samples.push_back(millisSince(start));
  }
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}
// -------------------------------------------------------------------------------------
// One row per section of a plan: where it sits, what compressed it, and what
// it cost. The run-level CSV reports one total per encoding, which cannot say
// whether a 41-bit timestamp dominates the output or some 3-bit field is being
// wasteful.
struct SectionRow {
  uint32_t index{0};
  uint8_t bit_start{0};
  uint8_t bit_end{0};
  std::string predicted;  // what the cost models expected to win here
  std::string actual;     // what the picker chose
  uint32_t storage_bits{32};  // 32: 32-bit pool; 64: wide section, 64-bit pool
  uint32_t bytes{0};
  double bits_per_value{0.0};
  double share_pct{0.0};  // share of the encoding this section accounts for
};
// -------------------------------------------------------------------------------------
// One row per (codec, range width): what a read of B contiguous elements at a
// uniformly drawn offset costs.
//
// This is the axis the cross-format comparison had no evidence on. It is served
// through gatherColumn/gatherColumn64, which resolve each position to its own
// chunk and decode only the chunks the range actually covers -- `chunks_touched`
// is reported so a reader can check that. A version that decoded the column and
// sliced it would report the bulk decode path with an extra memcpy and would
// mean nothing.
struct RangeRow {
  uint32_t width_b{0};      // B, elements read per range
  uint32_t offsets{0};      // distinct start offsets averaged over
  double range_ms{0.0};     // median wall time for one sweep of all offsets
  double ns_per_range{0.0};
  double ns_per_element{0.0};
  uint32_t chunks_touched{0};  // summed over the offsets in one sweep
};
// -------------------------------------------------------------------------------------
struct Result {
  std::string width;
  std::string dataset;
  std::string codec;
  uint32_t rows{0};
  uint32_t block_size{0};
  uint32_t chunks{0};
  uint64_t encoded_bytes{0};
  double ratio{0.0};
  double encode_ms{0.0};
  double decode_ms{0.0};
  double gather_uniform_ms{0.0};
  uint32_t gather_uniform_chunks{0};
  uint32_t gather_uniform_rows{0};
  double gather_clustered_ms{0.0};
  uint32_t gather_clustered_chunks{0};
  uint32_t gather_clustered_rows{0};
  double point_ms{0.0};
  uint32_t point_count{0};
  // Every read of the column -- bulk decode, both gathers, the point probes and
  // every range read -- was compared with the input and matched. A mismatch
  // clears it and the run carries on, so one broken codec cannot hide the rest.
  bool validated{true};
  std::string plan;
  // The scheme description of every chunk, not just chunk 0's in `plan`:
  // planning is per block, so blocks of one column can choose differently.
  std::vector<std::string> chunk_plans;
  // Populated only for SubIntSplit encodings.
  double plan_ms{0.0};
  bool raw_fallback{false};
  std::vector<SectionRow> sections;
  std::vector<RangeRow> ranges;
};
// -------------------------------------------------------------------------------------
// Turns the encoder's report on the last chunk it compressed into rows.
// No-ops for any codec that is not SubIntSplit, which leaves the report invalid.
void collectSections(Result& result) {
  const auto& report = subintsplit::lastPlanReport();
  if (!report.valid) {
    return;
  }
  result.plan_ms = report.plan_ms;
  result.raw_fallback = report.raw_fallback;

  uint32_t total = 0;
  for (const auto& section : report.sections) {
    total += section.bytes;
  }
  uint32_t index = 0;
  for (const auto& section : report.sections) {
    SectionRow row;
    row.index = index++;
    row.bit_start = section.bit_start;
    row.bit_end = section.bit_end;
    // With forced boundaries the planner never ran, so there is no prediction to
    // report. Emitting the default-constructed enum would look like a wrong
    // prediction and skew any accuracy figure computed from this file.
    row.predicted = report.forced_boundaries ? "-" : ConvertSchemeTypeToString(section.predicted);
    row.actual = section.wide ? ConvertSchemeTypeToString(section.actual64)
                              : ConvertSchemeTypeToString(section.actual);
    row.storage_bits = section.wide ? 64 : 32;
    row.bytes = section.bytes;
    row.bits_per_value = report.tuple_count > 0 ? (8.0 * section.bytes) / report.tuple_count : 0.0;
    row.share_pct = total > 0 ? (100.0 * section.bytes) / total : 0.0;
    result.sections.push_back(row);
  }
}
// -------------------------------------------------------------------------------------
void writeSectionsHeader(std::ostream& out) {
  out << "width,dataset,codec,block_size,rows,section,bit_start,bit_end,bits,predicted,actual,"
         "bytes,bits_per_value,share_pct,plan_ms,raw_fallback,storage_bits\n";
}
// -------------------------------------------------------------------------------------
void writeSectionRows(std::ostream& out, const Result& r) {
  for (const auto& s : r.sections) {
    out << r.width << ',' << r.dataset << ',' << r.codec << ',' << r.block_size << ',' << r.rows
        << ',' << s.index << ',' << static_cast<int>(s.bit_start) << ','
        << static_cast<int>(s.bit_end) << ',' << (s.bit_end - s.bit_start + 1) << ',' << s.predicted
        << ',' << s.actual << ',' << s.bytes << ',' << s.bits_per_value << ',' << s.share_pct
        << ','
        // Repeated per row so it survives a naive group-by.
        << r.plan_ms << ',' << (r.raw_fallback ? 1 : 0) << ',' << s.storage_bits << '\n';
  }
}
// -------------------------------------------------------------------------------------
std::string csvEscape(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"') {
      out += "\"\"";
    } else if (c == '\n' || c == '\t') {
      out += ' ';
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}
// -------------------------------------------------------------------------------------
void writeCsvHeader(std::ostream& out) {
  out << "width,dataset,codec,rows,block_size,chunks,encoded_bytes,ratio,encode_ms,decode_ms,"
         "gather_uniform_ms,gather_uniform_chunks,gather_uniform_rows,gather_clustered_ms,"
         "gather_clustered_chunks,gather_clustered_rows,point_ms,point_count,plan,validated\n";
}
// -------------------------------------------------------------------------------------
void writeCsvRow(std::ostream& out, const Result& r) {
  out << r.width << ',' << r.dataset << ',' << r.codec << ',' << r.rows << ',' << r.block_size
      << ',' << r.chunks << ',' << r.encoded_bytes << ',' << r.ratio << ',' << r.encode_ms << ','
      << r.decode_ms << ',' << r.gather_uniform_ms << ',' << r.gather_uniform_chunks << ','
      << r.gather_uniform_rows << ',' << r.gather_clustered_ms << ',' << r.gather_clustered_chunks
      << ',' << r.gather_clustered_rows << ',' << r.point_ms << ',' << r.point_count << ','
      << csvEscape(r.plan) << ',' << (r.validated ? 1 : 0) << '\n';
}
// -------------------------------------------------------------------------------------
void writePlansHeader(std::ostream& out) {
  out << "width,dataset,codec,block_size,chunk,plan\n";
}
// -------------------------------------------------------------------------------------
void writePlanRows(std::ostream& out, const Result& r) {
  for (std::size_t i = 0; i < r.chunk_plans.size(); i++) {
    out << r.width << ',' << r.dataset << ',' << r.codec << ',' << r.block_size << ',' << i << ','
        << csvEscape(r.chunk_plans[i]) << '\n';
  }
}
// -------------------------------------------------------------------------------------
// A codec under test: a top-level scheme, optionally with a forced split.
struct Codec {
  std::string name;
  IntegerSchemeType scheme{IntegerSchemeType::UNCOMPRESSED};
  bool automatic{false};          // let the picker choose
  std::string forced_boundaries;  // empty means let the planner choose
  // Takes SubIntSplit out of the pool, so an automatic codec measures what
  // BtrBlocks would do without this change at all.
  bool exclude_subintsplit{false};
};
// -------------------------------------------------------------------------------------
// Same shape as Codec, for Integer64SchemeType. A separate struct rather than
// a template: the two enums are unrelated types and every call site already
// knows which width it is dealing with.
struct Codec64 {
  std::string name;
  Integer64SchemeType scheme{Integer64SchemeType::UNCOMPRESSED};
  bool automatic{false};
  std::string forced_boundaries;
  bool exclude_subintsplit{false};
};
// -------------------------------------------------------------------------------------
// Removes SubIntSplit from the enabled set for the duration of a scope.
class ScopedSchemeSet {
 public:
  explicit ScopedSchemeSet(bool exclude_subintsplit)
      : saved_(BtrBlocksConfig::get().integers.schemes) {
    if (!exclude_subintsplit) {
      return;
    }
    active_ = true;
    BtrBlocksConfig::get().integers.schemes.disable(IntegerSchemeType::SUB_INT_SPLIT);
    SchemePool::refresh();
  }
  ~ScopedSchemeSet() {
    if (active_) {
      BtrBlocksConfig::get().integers.schemes = saved_;
      SchemePool::refresh();
    }
  }

 private:
  IntegerSchemeSet saved_;
  bool active_{false};
};
// -------------------------------------------------------------------------------------
// Same as ScopedSchemeSet, for the 64-bit scheme set.
class ScopedSchemeSet64 {
 public:
  explicit ScopedSchemeSet64(bool exclude_subintsplit)
      : saved_(BtrBlocksConfig::get().integers64.schemes) {
    if (!exclude_subintsplit) {
      return;
    }
    active_ = true;
    BtrBlocksConfig::get().integers64.schemes.disable(Integer64SchemeType::SUB_INT_SPLIT);
    SchemePool::refresh();
  }
  ~ScopedSchemeSet64() {
    if (active_) {
      BtrBlocksConfig::get().integers64.schemes = saved_;
      SchemePool::refresh();
    }
  }

 private:
  Integer64SchemeSet saved_;
  bool active_{false};
};
// -------------------------------------------------------------------------------------
// Applies a codec's forced split for the duration of a scope, if it has one.
class ScopedBoundaries {
 public:
  ScopedBoundaries(const std::string& text, int value_bits) {
    if (text.empty()) {
      return;
    }
    std::vector<subintsplit::SegmentPlan> segments;
    if (!subintsplit::parseSplitBoundaries(text, value_bits, segments)) {
      throw Generic_Exception("invalid forced boundaries: " + text);
    }
    enforcer_ = std::make_unique<subintsplit::EnforceSplitBoundaries>(std::move(segments));
  }

 private:
  std::unique_ptr<subintsplit::EnforceSplitBoundaries> enforcer_;
};
// -------------------------------------------------------------------------------------
// Range widths swept, and how many start offsets each is averaged over. Both are
// overridable from the command line so this can be matched to whatever the other
// harnesses sweep.
std::vector<uint32_t> g_range_widths{1, 8, 64, 512, 4096, 32768, 262144};
// Offsets per range width, parallel to g_range_widths.
//
// A constant count is the wrong shape. The spread across offsets is not
// constant in B: it is ~1.03x at B=1, peaks around 7.4x at B=512, and falls
// back to ~1.6x by B=32768. At the small end almost every offset costs the
// same (one chunk, one decode) so extra offsets buy nothing; at the large end
// each offset costs B element-reads, so extra offsets are the most expensive
// samples in the sweep and the spread does not justify them. Sampling is
// therefore concentrated in the middle, where the answer actually varies.
//
// Against a flat 32 this is 2.6M element-reads per repeat instead of 9.6M --
// 3.7x less work -- while sampling B=512 harder than the flat schedule did.
std::vector<uint32_t> g_range_offsets{8, 16, 24, 48, 24, 12, 8};
// Point probes, aligned across all three harnesses at 256.
//
// Alignment means the same count everywhere, and the defensible common count
// is a small one: for any codec that decodes a block per access the per-probe
// cost is constant, so a small sample estimates the same ns/probe as a large
// one. Measured on this benchmark at 64 / 256 / 20,000 probes, ns/probe agrees
// within 1.17x for every codec and within 1.10x at 256 against 20,000, while
// the point phase falls from 34% of a run to 1.4%. 64 is measurably biased
// high (up to 1.17x) because first-touch effects do not amortise over so few
// probes; 256 removes most of that for nothing.
uint32_t g_point_probes = 256;
// Empty means every codec. Otherwise only codecs named here are run.
std::vector<std::string> g_codec_filter;
// --shared-codecs: hold both scheme pools to the codec set shared by all three
// formats in the cross-format comparison (see sharedIntegerSchemes()).
bool g_shared_codecs = false;
// -------------------------------------------------------------------------------------
// The codec set Nimble, BtrBlocks and FastLanes are all compared on, as
// BtrBlocks spells it: uncompressed, one-value, dictionary (dynamic and the
// fixed 8/16-bit-code variants), RLE, patched FOR, frequency, bit-packing and
// FOR. Not truncation, not delta. The 32-bit pool is what compresses narrow
// SubIntSplit sections and BP64's halves; the 64-bit pool is what AUTO picks
// from for a BIGINT column and what compresses wide sections. SubIntSplit
// itself is added to the 64-bit pool for AUTO_WITH_SIS64 only.
IntegerSchemeSet sharedIntegerSchemes() {
  return {IntegerSchemeType::UNCOMPRESSED, IntegerSchemeType::ONE_VALUE,
          IntegerSchemeType::DICT,         IntegerSchemeType::DICTIONARY_8,
          IntegerSchemeType::DICTIONARY_16, IntegerSchemeType::RLE,
          IntegerSchemeType::PFOR,         IntegerSchemeType::FREQUENCY,
          IntegerSchemeType::BP,           IntegerSchemeType::FOR};
}
Integer64SchemeSet sharedInteger64Schemes() {
  return {Integer64SchemeType::UNCOMPRESSED, Integer64SchemeType::ONE_VALUE,
          Integer64SchemeType::DICT,         Integer64SchemeType::DICTIONARY_8,
          Integer64SchemeType::DICTIONARY_16, Integer64SchemeType::RLE,
          Integer64SchemeType::PFOR,         Integer64SchemeType::FREQUENCY,
          Integer64SchemeType::BP,           Integer64SchemeType::FOR};
}
// Range start offsets are drawn from this seed alone, so every codec, dataset
// and block size reads the same offsets and the columns compare codecs.
constexpr uint32_t kRangeSeed = 17;
// -------------------------------------------------------------------------------------
// Start offsets for one range width, drawn once and reused by every codec, so
// the columns compare codecs rather than random draws.
std::vector<uint32_t> rangeStarts(uint32_t row_count, uint32_t width, uint32_t offsets,
                                  uint32_t seed) {
  std::vector<uint32_t> starts;
  if (width > row_count) {
    return starts;
  }
  std::mt19937 rng(seed + width);
  std::uniform_int_distribution<uint32_t> pick(0, row_count - width);
  starts.reserve(offsets);
  for (uint32_t i = 0; i < offsets; i++) {
    starts.push_back(pick(rng));
  }
  return starts;
}
// -------------------------------------------------------------------------------------
// Times one range width. `gather` reads `count` contiguous positions starting at
// `begin` and reports how many chunks it had to touch.
template <typename GatherFn, typename CheckFn>
void measureRanges(uint32_t row_count, int repeats, uint32_t seed, Result& result,
                   const GatherFn& gather, const CheckFn& check) {
  for (std::size_t w = 0; w < g_range_widths.size(); w++) {
    const uint32_t width = g_range_widths[w];
    // Falls back to the last entry when the offset schedule is shorter than
    // the width list, so --range-widths alone stays usable.
    const uint32_t offsets =
        g_range_offsets.empty()
            ? 32
            : g_range_offsets[std::min(w, g_range_offsets.size() - 1)];
    const auto starts = rangeStarts(row_count, width, offsets, seed);
    if (starts.empty()) {
      continue;
    }
    // Positions are materialised outside the timed region: building them is the
    // caller's cost in a real query, not the format's.
    std::vector<std::vector<uint32_t>> position_sets;
    position_sets.reserve(starts.size());
    for (const uint32_t begin : starts) {
      std::vector<uint32_t> positions(width);
      for (uint32_t i = 0; i < width; i++) {
        positions[i] = begin + i;
      }
      position_sets.push_back(std::move(positions));
    }

    uint32_t touched_total = 0;
    const double ms = timeMedian(repeats, [&]() {
      touched_total = 0;
      for (const auto& positions : position_sets) {
        touched_total += gather(positions.data(), static_cast<uint32_t>(positions.size()));
      }
    });

    // Correctness, outside the timed region: every range read once more and
    // compared with the input.
    for (const auto& positions : position_sets) {
      gather(positions.data(), static_cast<uint32_t>(positions.size()));
      if (!check(positions.data(), static_cast<uint32_t>(positions.size()))) {
        result.validated = false;
      }
    }

    RangeRow row;
    row.width_b = width;
    row.offsets = static_cast<uint32_t>(starts.size());
    row.range_ms = ms;
    row.ns_per_range = ms * 1e6 / static_cast<double>(starts.size());
    row.ns_per_element = row.ns_per_range / static_cast<double>(width);
    row.chunks_touched = touched_total;
    result.ranges.push_back(row);
  }
}
// -------------------------------------------------------------------------------------
void writeRangeHeader(std::ostream& out) {
  out << "width,dataset,codec,rows,block_size,B,offsets,range_ms,ns_per_range,ns_per_element,"
         "chunks_touched\n";
}
// -------------------------------------------------------------------------------------
void writeRangeRows(std::ostream& out, const Result& r) {
  for (const auto& row : r.ranges) {
    out << r.width << ',' << r.dataset << ',' << r.codec << ',' << r.rows << ',' << r.block_size
        << ',' << row.width_b << ',' << row.offsets << ',' << row.range_ms << ','
        << row.ns_per_range << ',' << row.ns_per_element << ',' << row.chunks_touched << '\n';
  }
}
// -------------------------------------------------------------------------------------
// ---------------------------- 32-bit arm ----------------------------------------------
// -------------------------------------------------------------------------------------
Result run32(const std::string& dataset_name,
             const std::vector<INTEGER>& data,
             const Codec& codec,
             uint32_t block_size,
             int repeats) {
  Result result;
  result.width = "32";
  result.dataset = dataset_name;
  result.codec = codec.name;
  result.rows = static_cast<uint32_t>(data.size());
  result.block_size = block_size;

  BtrBlocksConfig::get().block_size = block_size;
  ScopedSchemeSet scheme_set(codec.exclude_subintsplit);

  Relation relation;
  {
    Vector<INTEGER> column(static_cast<u64>(data.size()));
    for (std::size_t i = 0; i < data.size(); i++) {
      column[i] = data[i];
    }
    relation.addColumn({"ids", std::move(column)});
  }
  const auto ranges = relation.getRanges(SplitStrategy::SEQUENTIAL, 999999);
  result.chunks = static_cast<uint32_t>(ranges.size());

  // ---- encode -------------------------------------------------------------------
  std::vector<std::vector<u8>> compressed(ranges.size());
  const auto compressAll = [&]() {
    ScopedBoundaries boundaries(codec.forced_boundaries, 32);
    for (std::size_t chunk_i = 0; chunk_i < ranges.size(); chunk_i++) {
      // The override is consumed by the first compress that sees it, so it has
      // to be re-armed for every chunk.
      if (!codec.automatic) {
        BtrBlocksConfig::get().integers.override_scheme = codec.scheme;
      }
      auto input_chunk = relation.getInputChunk(ranges[chunk_i], chunk_i, 0);
      compressed[chunk_i] = Datablock::compress(input_chunk);
    }
    BtrBlocksConfig::get().integers.override_scheme = static_cast<IntegerSchemeType>(autoScheme());
  };
  result.encode_ms = timeMedian(repeats, compressAll);

  uint64_t encoded_bytes = 0;
  for (const auto& chunk : compressed) {
    encoded_bytes += chunk.size();
  }
  result.encoded_bytes = encoded_bytes;
  result.ratio = static_cast<double>(data.size() * sizeof(INTEGER)) /
                 static_cast<double>(std::max<uint64_t>(encoded_bytes, 1));

  // ---- section breakdown --------------------------------------------------------
  // One more compress of chunk 0, outside the timed loop. Each chunk overwrites
  // the encoder's report, so without this the sections would describe the last
  // chunk while the plan column describes the first.
  {
    subintsplit::lastPlanReport().valid = false;
    ScopedBoundaries boundaries(codec.forced_boundaries, 32);
    if (!codec.automatic) {
      BtrBlocksConfig::get().integers.override_scheme = codec.scheme;
    }
    auto input_chunk = relation.getInputChunk(ranges[0], 0, 0);
    Datablock::compress(input_chunk);
    BtrBlocksConfig::get().integers.override_scheme = static_cast<IntegerSchemeType>(autoScheme());
    collectSections(result);
  }

  // ---- assemble a column part so the read path is the real one ------------------
  ColumnPart part;
  for (auto& chunk : compressed) {
    part.addCompressedChunk(std::move(chunk));
  }
  const std::string path = "subintsplit_bench_column.btr";
  part.writeToDisk(path);

  std::vector<char> file_contents;
  Utils::readFileToMemory(path, file_contents);
  BtrReader reader(file_contents.data());

  {
    std::vector<u8> scratch;
    reader.readColumn(scratch, 0);
    result.plan = reader.getSchemeDescription(0);
    for (u32 chunk_i = 0; chunk_i < reader.getChunkCount(); chunk_i++) {
      result.chunk_plans.push_back(reader.getSchemeDescription(chunk_i));
    }
  }

  // ---- bulk decode --------------------------------------------------------------
  std::vector<INTEGER> decoded(data.size());
  const auto decodeAll = [&]() {
    std::size_t offset = 0;
    std::vector<u8> scratch;
    for (u32 chunk_i = 0; chunk_i < reader.getChunkCount(); chunk_i++) {
      reader.readColumn(scratch, chunk_i);
      const auto tuple_count = reader.getTupleCount(chunk_i);
      std::memcpy(decoded.data() + offset, scratch.data(), tuple_count * sizeof(INTEGER));
      offset += tuple_count;
    }
  };
  result.decode_ms = timeMedian(repeats, decodeAll);

  for (std::size_t i = 0; i < data.size(); i++) {
    if (decoded[i] != data[i]) {
      std::cerr << "VALIDATION FAILED: decode mismatch at row " << i << " for " << dataset_name
                << "/" << codec.name << "\n";
      result.validated = false;
      break;
    }
  }

  // ---- gather -------------------------------------------------------------------
  const auto measureGather = [&](const std::vector<uint32_t>& positions, double& ms,
                                 uint32_t& chunks_touched, uint32_t& rows) {
    if (positions.empty()) {
      return;
    }
    rows = static_cast<uint32_t>(positions.size());
    std::vector<INTEGER> gathered(positions.size());
    u32 touched = 0;
    ms = timeMedian(repeats, [&]() {
      reader.gatherColumn(gathered.data(), positions.data(), static_cast<u32>(positions.size()),
                          &touched);
    });
    chunks_touched = touched;
    for (std::size_t i = 0; i < positions.size(); i++) {
      if (gathered[i] != data[positions[i]]) {
        std::cerr << "VALIDATION FAILED: gather mismatch for " << dataset_name << "/"
                  << codec.name << "\n";
        result.validated = false;
        break;
      }
    }
  };

  const auto rows32 = static_cast<uint32_t>(data.size());
  measureGather(uniformTrace(rows32, 4096, 7), result.gather_uniform_ms,
                result.gather_uniform_chunks, result.gather_uniform_rows);
  // Low selectivity with long runs, so whole chunks go untouched -- the regime
  // where chunk locality is visible at all.
  measureGather(clusteredTrace(rows32, 0.01, 64.0, 11), result.gather_clustered_ms,
                result.gather_clustered_chunks, result.gather_clustered_rows);

  // ---- point access -------------------------------------------------------------
  const auto point_positions = uniformTrace(rows32, g_point_probes, 13);
  result.point_count = static_cast<uint32_t>(point_positions.size());
  result.point_ms = timeMedian(repeats, [&]() {
    for (const auto position : point_positions) {
      volatile INTEGER value = reader.lookupColumn(position);
      (void)value;
    }
  });
  for (const auto position : point_positions) {
    if (reader.lookupColumn(position) != data[position]) {
      std::cerr << "VALIDATION FAILED: point mismatch at row " << position << " for "
                << dataset_name << "/" << codec.name << "\n";
      result.validated = false;
      break;
    }
  }

  // ---- range read ---------------------------------------------------------------
  {
    std::vector<INTEGER> range_out(g_range_widths.empty() ? 1 : g_range_widths.back());
    measureRanges(rows32, repeats, kRangeSeed, result,
                  [&](const uint32_t* positions, uint32_t count) -> uint32_t {
                    u32 touched = 0;
                    reader.gatherColumn(range_out.data(), positions, count, &touched);
                    return touched;
                  },
                  [&](const uint32_t* positions, uint32_t count) {
                    for (uint32_t i = 0; i < count; i++) {
                      if (range_out[i] != data[positions[i]]) {
                        std::cerr << "VALIDATION FAILED: range mismatch at row " << positions[i]
                                  << " for " << dataset_name << "/" << codec.name << "\n";
                        return false;
                      }
                    }
                    return true;
                  });
  }

  std::remove(path.c_str());
  return result;
}
// -------------------------------------------------------------------------------------
// ---------------------------- 64-bit arm ----------------------------------------------
// -------------------------------------------------------------------------------------
// Mirrors run32() exactly: a real Relation with a BIGINT column, compressed
// chunk-by-chunk through Datablock::compress (which dispatches on
// ColumnType::BIGINT into the registered Integer64Scheme pool), assembled
// into a ColumnPart, written to disk and read back through BtrReader. Now
// that BIGINT is a real column type with a real gatherColumn64/lookupColumn64
// path, there is no reason for the 64-bit arm to hand-chunk or call scheme
// methods directly the way it used to -- doing so measured a synthetic path
// that skipped BtrReader's chunk bucketing and metadata entirely.
Result run64(const std::string& dataset_name,
             const std::vector<s64>& data,
             const Codec64& codec,
             uint32_t block_size,
             int repeats) {
  Result result;
  result.width = "64";
  result.dataset = dataset_name;
  result.codec = codec.name;
  result.rows = static_cast<uint32_t>(data.size());
  result.block_size = block_size;

  BtrBlocksConfig::get().block_size = block_size;
  ScopedSchemeSet64 scheme_set(codec.exclude_subintsplit);

  Relation relation;
  {
    Vector<BIGINT> column(static_cast<u64>(data.size()));
    for (std::size_t i = 0; i < data.size(); i++) {
      column[i] = data[i];
    }
    relation.addColumn({"ids", std::move(column)});
  }
  const auto ranges = relation.getRanges(SplitStrategy::SEQUENTIAL, 999999);
  result.chunks = static_cast<uint32_t>(ranges.size());

  // ---- encode -------------------------------------------------------------------
  std::vector<std::vector<u8>> compressed(ranges.size());
  const auto compressAll = [&]() {
    ScopedBoundaries boundaries(codec.forced_boundaries, 64);
    for (std::size_t chunk_i = 0; chunk_i < ranges.size(); chunk_i++) {
      // The override is consumed by the first compress that sees it, so it has
      // to be re-armed for every chunk.
      if (!codec.automatic) {
        BtrBlocksConfig::get().integers64.override_scheme = codec.scheme;
      }
      auto input_chunk = relation.getInputChunk(ranges[chunk_i], chunk_i, 0);
      compressed[chunk_i] = Datablock::compress(input_chunk);
    }
    BtrBlocksConfig::get().integers64.override_scheme =
        static_cast<Integer64SchemeType>(autoScheme());
  };
  result.encode_ms = timeMedian(repeats, compressAll);

  uint64_t encoded_bytes = 0;
  for (const auto& chunk : compressed) {
    encoded_bytes += chunk.size();
  }
  result.encoded_bytes = encoded_bytes;
  result.ratio = static_cast<double>(data.size() * sizeof(s64)) /
                 static_cast<double>(std::max<uint64_t>(encoded_bytes, 1));

  // ---- section breakdown --------------------------------------------------------
  // One more compress of chunk 0, outside the timed loop, same reasoning as run32().
  {
    subintsplit::lastPlanReport().valid = false;
    ScopedBoundaries boundaries(codec.forced_boundaries, 64);
    if (!codec.automatic) {
      BtrBlocksConfig::get().integers64.override_scheme = codec.scheme;
    }
    auto input_chunk = relation.getInputChunk(ranges[0], 0, 0);
    Datablock::compress(input_chunk);
    BtrBlocksConfig::get().integers64.override_scheme =
        static_cast<Integer64SchemeType>(autoScheme());
    collectSections(result);
  }

  // ---- assemble a column part so the read path is the real one ------------------
  ColumnPart part;
  for (auto& chunk : compressed) {
    part.addCompressedChunk(std::move(chunk));
  }
  const std::string path = "subintsplit_bench_column64.btr";
  part.writeToDisk(path);

  std::vector<char> file_contents;
  Utils::readFileToMemory(path, file_contents);
  BtrReader reader(file_contents.data());

  {
    std::vector<u8> scratch;
    reader.readColumn(scratch, 0);
    result.plan = reader.getSchemeDescription(0);
    for (u32 chunk_i = 0; chunk_i < reader.getChunkCount(); chunk_i++) {
      result.chunk_plans.push_back(reader.getSchemeDescription(chunk_i));
    }
  }

  // ---- bulk decode --------------------------------------------------------------
  std::vector<s64> decoded(data.size());
  const auto decodeAll = [&]() {
    std::size_t offset = 0;
    std::vector<u8> scratch;
    for (u32 chunk_i = 0; chunk_i < reader.getChunkCount(); chunk_i++) {
      reader.readColumn(scratch, chunk_i);
      const auto tuple_count = reader.getTupleCount(chunk_i);
      std::memcpy(decoded.data() + offset, scratch.data(), tuple_count * sizeof(s64));
      offset += tuple_count;
    }
  };
  result.decode_ms = timeMedian(repeats, decodeAll);

  for (std::size_t i = 0; i < data.size(); i++) {
    if (decoded[i] != data[i]) {
      std::cerr << "VALIDATION FAILED: decode mismatch at row " << i << " for " << dataset_name
                << "/" << codec.name << "\n";
      result.validated = false;
      break;
    }
  }

  // ---- gather -------------------------------------------------------------------
  const auto measureGather = [&](const std::vector<uint32_t>& positions, double& ms,
                                 uint32_t& chunks_touched, uint32_t& rows) {
    if (positions.empty()) {
      return;
    }
    rows = static_cast<uint32_t>(positions.size());
    std::vector<s64> gathered(positions.size());
    u32 touched = 0;
    ms = timeMedian(repeats, [&]() {
      reader.gatherColumn64(gathered.data(), positions.data(),
                            static_cast<u32>(positions.size()), &touched);
    });
    chunks_touched = touched;
    for (std::size_t i = 0; i < positions.size(); i++) {
      if (gathered[i] != data[positions[i]]) {
        std::cerr << "VALIDATION FAILED: gather mismatch for " << dataset_name << "/"
                  << codec.name << "\n";
        result.validated = false;
        break;
      }
    }
  };

  const auto rows64 = static_cast<uint32_t>(data.size());
  measureGather(uniformTrace(rows64, 4096, 7), result.gather_uniform_ms,
                result.gather_uniform_chunks, result.gather_uniform_rows);
  // Low selectivity with long runs, so whole chunks go untouched -- the regime
  // where chunk locality is visible at all.
  measureGather(clusteredTrace(rows64, 0.01, 64.0, 11), result.gather_clustered_ms,
                result.gather_clustered_chunks, result.gather_clustered_rows);

  // ---- point access -------------------------------------------------------------
  const auto point_positions = uniformTrace(rows64, g_point_probes, 13);
  result.point_count = static_cast<uint32_t>(point_positions.size());
  result.point_ms = timeMedian(repeats, [&]() {
    for (const auto position : point_positions) {
      volatile s64 value = reader.lookupColumn64(position);
      (void)value;
    }
  });
  for (const auto position : point_positions) {
    if (reader.lookupColumn64(position) != data[position]) {
      std::cerr << "VALIDATION FAILED: point mismatch at row " << position << " for "
                << dataset_name << "/" << codec.name << "\n";
      result.validated = false;
      break;
    }
  }

  // ---- range read ---------------------------------------------------------------
  // B contiguous elements from a uniformly drawn offset, at log-spaced B.
  {
    std::vector<s64> range_out(g_range_widths.empty() ? 1 : g_range_widths.back());
    measureRanges(rows64, repeats, kRangeSeed, result,
                  [&](const uint32_t* positions, uint32_t count) -> uint32_t {
                    u32 touched = 0;
                    reader.gatherColumn64(range_out.data(), positions, count, &touched);
                    return touched;
                  },
                  [&](const uint32_t* positions, uint32_t count) {
                    for (uint32_t i = 0; i < count; i++) {
                      if (range_out[i] != data[positions[i]]) {
                        std::cerr << "VALIDATION FAILED: range mismatch at row " << positions[i]
                                  << " for " << dataset_name << "/" << codec.name << "\n";
                        return false;
                      }
                    }
                    return true;
                  });
  }

  std::remove(path.c_str());
  return result;
}
// -------------------------------------------------------------------------------------
// Reads up to `max_count` values from a flat little-endian int64 file, as
// produced by parquet_to_i64.py.
//
// Real data arrives as Parquet, which BtrBlocks cannot read and should not grow
// a dependency on for one benchmark input, so the conversion happens once
// out-of-band and this end stays an ifstream.
//
// Returns empty on any failure rather than throwing: a missing or unreadable
// file should cost the caller one dataset, not a whole sweep.
std::vector<s64> readInt64Column(const std::string& path, uint32_t max_count) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in.good()) {
    return {};
  }
  const auto bytes = static_cast<std::streamoff>(in.tellg());
  if (bytes <= 0) {
    return {};
  }

  // Short files are used as-is rather than cycled: repeating a column would
  // manufacture periodicity that flatters every codec measured on it.
  const auto available = static_cast<std::size_t>(bytes) / sizeof(s64);
  const auto count = std::min<std::size_t>(available, max_count);

  std::vector<s64> values(count);
  in.seekg(0);
  in.read(reinterpret_cast<char*>(values.data()),
          static_cast<std::streamsize>(count * sizeof(s64)));
  if (!in) {
    return {};
  }
  if (count < max_count) {
    std::cerr << "note: " << path << " holds " << available << " values, fewer than the requested "
              << max_count << "\n";
  }
  return values;
}
// -------------------------------------------------------------------------------------
// Reads a one-value-per-line text column, the format the shared corpus under
// EncodingsPlayground/Datasets is dumped in and the same input the Nimble ML-ID
// benchmark reads through --mlidc_file. Taking the LEADING max_count values in
// file order is the rule all three harnesses now follow, so the three encode the
// same rows in the same arrangement.
//
// Returns empty on any failure: a missing dataset should cost the caller one
// column, not a whole sweep.
std::vector<s64> readTextColumn(const std::string& path, uint32_t max_count) {
  std::ifstream in(path);
  if (!in.good()) {
    return {};
  }
  std::vector<s64> values;
  values.reserve(max_count);
  std::string line;
  while (values.size() < max_count && std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    try {
      values.push_back(static_cast<s64>(std::stoll(line)));
    } catch (const std::exception&) {
      std::cerr << "note: " << path << " has an unparseable line, stopping there\n";
      break;
    }
  }
  if (values.size() < max_count) {
    std::cerr << "note: " << path << " holds " << values.size()
              << " values, fewer than the requested " << max_count << "\n";
  }
  return values;
}
// -------------------------------------------------------------------------------------
// A `name=path` dataset argument. Repeating --dataset is how a column other than
// the built-in generated ones gets measured, which is what the XMark, OSM and
// Public BI columns need.
bool parseNamedDataset(const std::string& text, std::string& name, std::string& file) {
  const auto equals = text.find('=');
  if (equals == std::string::npos || equals == 0 || equals + 1 == text.size()) {
    return false;
  }
  name = text.substr(0, equals);
  file = text.substr(equals + 1);
  return true;
}
// -------------------------------------------------------------------------------------
// Records what machine produced a run.
//
// No result file in this repository carried this, which makes any timing here
// uncomparable to a timing produced anywhere else -- including to this same
// benchmark on this same machine under a different governor.
void writeRunMetadata(const std::string& path, int repeats, uint32_t rows) {
  std::ofstream out(path);
  if (!out.good()) {
    std::cerr << "cannot open " << path << " for run metadata\n";
    return;
  }
  const auto readFirst = [](const char* file, const char* prefix) -> std::string {
    std::ifstream in(file);
    std::string line;
    const std::string want(prefix);
    while (std::getline(in, line)) {
      if (want.empty()) {
        return line;
      }
      if (line.rfind(want, 0) == 0) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
          return {};
        }
        const auto first = line.find_first_not_of(" \t", colon + 1);
        return first == std::string::npos ? std::string() : line.substr(first);
      }
    }
    return {};
  };

  const auto now = std::time(nullptr);
  char stamp[32] = {};
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", std::gmtime(&now));

  out << "key,value\n";
  out << "driver,subintsplit_bench\n";
  out << "timestamp_utc," << stamp << "\n";
  out << "cpu_model," << readFirst("/proc/cpuinfo", "model name") << "\n";
  out << "hardware_threads," << std::thread::hardware_concurrency() << "\n";
  out << "scaling_governor,"
      << readFirst("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "") << "\n";
#if defined(__clang__)
  out << "compiler,clang " << __clang_major__ << "." << __clang_minor__ << "\n";
#elif defined(__GNUC__)
  out << "compiler,gcc " << __GNUC__ << "." << __GNUC_MINOR__ << "\n";
#else
  out << "compiler,unknown\n";
#endif
  out << "reduction,median_of_" << repeats << "\n";
  out << "rows," << rows << "\n";
  out << "point_probes," << g_point_probes << "\n";
  out << "range_offsets_per_width,";
  for (std::size_t i = 0; i < g_range_offsets.size(); i++) {
    out << (i ? " " : "") << g_range_offsets[i];
  }
  out << "\n";
  out << "range_widths,";
  for (std::size_t i = 0; i < g_range_widths.size(); i++) {
    out << (i ? " " : "") << g_range_widths[i];
  }
  out << "\n";
  out << "codec_set," << (g_codec_filter.empty() ? "all" : "restricted") << "\n";
  out << "scheme_pools," << (g_shared_codecs ? "shared" : "default") << "\n";
  out << "range_seed," << kRangeSeed << "\n";
}
// -------------------------------------------------------------------------------------
std::vector<std::string> parseStringList(const std::string& text) {
  std::vector<std::string> values;
  std::stringstream stream(text);
  std::string token;
  while (std::getline(stream, token, ',')) {
    if (!token.empty()) {
      values.push_back(token);
    }
  }
  return values;
}
// -------------------------------------------------------------------------------------
bool codecSelected(const std::string& name) {
  if (g_codec_filter.empty()) {
    return true;
  }
  return std::find(g_codec_filter.begin(), g_codec_filter.end(), name) != g_codec_filter.end();
}
// -------------------------------------------------------------------------------------
std::vector<uint32_t> parseUintList(const std::string& text) {
  std::vector<uint32_t> values;
  std::stringstream stream(text);
  std::string token;
  while (std::getline(stream, token, ',')) {
    if (!token.empty()) {
      values.push_back(static_cast<uint32_t>(std::strtoul(token.c_str(), nullptr, 10)));
    }
  }
  return values;
}
// -------------------------------------------------------------------------------------
}  // namespace
// -------------------------------------------------------------------------------------
int main(int argc, char** argv) {
  uint32_t rows = 1u << 20;
  std::vector<uint32_t> block_sizes{4096, 8192, 65536};
  int repeats = 5;
  uint32_t seed = 42;
  std::string csv_path;
  std::string sections_csv_path;
  std::string plans_csv_path;
  std::string input_i64;
  std::string range_csv_path;
  std::string metadata_path;
  // Named text columns from the shared corpus, in the order they were given.
  std::vector<std::pair<std::string, std::string>> named_datasets;

  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    const auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
    if (arg == "--rows") {
      rows = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
    } else if (arg == "--block-sizes") {
      block_sizes = parseUintList(next());
    } else if (arg == "--repeats") {
      repeats = std::atoi(next().c_str());
    } else if (arg == "--seed") {
      seed = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
    } else if (arg == "--csv") {
      csv_path = next();
    } else if (arg == "--input-i64") {
      input_i64 = next();
    } else if (arg == "--sections-csv") {
      sections_csv_path = next();
    } else if (arg == "--range-csv") {
      range_csv_path = next();
    } else if (arg == "--plans-csv") {
      plans_csv_path = next();
    } else if (arg == "--shared-codecs") {
      g_shared_codecs = true;
    } else if (arg == "--metadata-csv") {
      metadata_path = next();
    } else if (arg == "--range-widths") {
      g_range_widths = parseUintList(next());
    } else if (arg == "--range-offsets") {
      g_range_offsets = parseUintList(next());
    } else if (arg == "--codecs") {
      const std::string value = next();
      if (value == "common") {
        // The set every one of the three frameworks implements, and the only
        // set the cross-format figure plots. Anything outside it is
        // measurement that will not be used, and at these row counts the
        // range sweep makes unused measurement expensive.
        g_codec_filter = {"UNCOMPRESSED64", "FOR64",      "PFOR64",       "DICT64",
                          "RLE64",          "FREQUENCY64", "SIS64_PLANNED"};
      } else {
        g_codec_filter = parseStringList(value);
      }
    } else if (arg == "--point-probes") {
      g_point_probes = static_cast<uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
    } else if (arg == "--dataset") {
      std::string name;
      std::string file;
      if (!parseNamedDataset(next(), name, file)) {
        std::cerr << "--dataset expects name=path\n";
        return 1;
      }
      named_datasets.emplace_back(std::move(name), std::move(file));
    } else if (arg == "--help") {
      std::cout << "usage: subintsplit_bench [--rows N] [--block-sizes a,b,c] [--repeats N]\n"
                   "                         [--seed N] [--csv PATH] [--input-i64 PATH]\n"
                   "                         [--dataset NAME=PATH]... [--sections-csv PATH]\n"
                   "                         [--range-csv PATH] [--metadata-csv PATH]\n"
                   "                         [--range-widths a,b,c] [--range-offsets N]\n"
                   "                         [--point-probes N] [--plans-csv PATH]\n"
                   "                         [--shared-codecs]\n"
                   "\n"
                   "  --shared-codecs      hold the 32- and 64-bit scheme pools (whole columns,\n"
                   "                       AUTO arms, SubIntSplit sections) to the codec set the\n"
                   "                       cross-format comparison shares: UNCOMPRESSED,\n"
                   "                       ONE_VALUE, DICT (+ 8/16-bit fixed), RLE, PFOR,\n"
                   "                       FREQUENCY, BP, FOR. SubIntSplit64 is added for\n"
                   "                       AUTO_WITH_SIS64 only. A forced codec outside the\n"
                   "                       set is skipped. Default: the pools below.\n"
                   "\n"
                   "  --plans-csv PATH     write every chunk's scheme description (the plan\n"
                   "                       column of --csv covers chunk 0 only).\n"
                   "\n"
                   "  --input-i64 PATH     add a 64-bit dataset read from a flat little-endian\n"
                   "                       int64 file, for measuring against real data rather\n"
                   "                       than generated. Produce one with parquet_to_i64.py.\n"
                   "\n"
                   "  --dataset NAME=PATH  add a 64-bit dataset read from a one-value-per-line\n"
                   "                       text file, the format the shared column corpus is\n"
                   "                       dumped in. Repeatable. The LEADING --rows values are\n"
                   "                       taken in file order, which is the rule the FastLanes\n"
                   "                       and Nimble harnesses follow, so all three encode the\n"
                   "                       same rows in the same arrangement.\n"
                   "\n"
                   "  --sections-csv PATH  write one row per section of each SubIntSplit plan:\n"
                   "                       bit range, the scheme the planner predicted, the\n"
                   "                       scheme actually chosen, and the bytes it cost.\n"
                   "\n"
                   "  --range-csv PATH     write the range-read sweep: the cost of reading B\n"
                   "                       contiguous elements from a uniformly drawn offset,\n"
                   "                       for each B in --range-widths, together with how many\n"
                   "                       chunks the read had to touch.\n"
                   "\n"
                   "  --metadata-csv PATH  write the CPU, compiler, governor and sweep settings\n"
                   "                       this run used. Without it a timing here cannot be\n"
                   "                       compared to a timing from anywhere else.\n";
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return 1;
    }
  }

  BtrBlocksConfig::configure([](BtrBlocksConfig& config) {
    if (g_shared_codecs) {
      config.integers.schemes = sharedIntegerSchemes();
      config.integers64.schemes = sharedInteger64Schemes();
      config.integers64.schemes.enable(Integer64SchemeType::SUB_INT_SPLIT);
      return;
    }
    config.integers.schemes = defaultIntegerSchemes();
    config.integers.schemes.enable(IntegerSchemeType::SUB_INT_SPLIT);
    config.integers64.schemes = defaultInteger64Schemes();
    config.integers64.schemes.enable(Integer64SchemeType::SUB_INT_SPLIT);
    // FOR64 is a legacy scheme excluded from defaultInteger64Schemes() (like
    // its 32-bit counterpart), but the FOR64 codec below forces it directly,
    // so it has to be in the pool for the override to find.
    config.integers64.schemes.enable(Integer64SchemeType::FOR);
    config.integers64.schemes.enable(Integer64SchemeType::PFOR);
    config.integers64.schemes.enable(Integer64SchemeType::FREQUENCY);
  });

  // The incumbent codecs, then SubIntSplit with the planner's split and with
  // the fixed halves split that isolates the planner's contribution.
  const std::vector<Codec> codecs32{
      {"UNCOMPRESSED", IntegerSchemeType::UNCOMPRESSED, false, ""},
      {"BP", IntegerSchemeType::BP, false, ""},
      {"PFOR", IntegerSchemeType::PFOR, false, ""},
      {"DICT", IntegerSchemeType::DICT, false, ""},
      {"RLE", IntegerSchemeType::RLE, false, ""},
      // What BtrBlocks does today, with SubIntSplit out of the pool.
      {"AUTO_BASELINE", IntegerSchemeType::UNCOMPRESSED, true, "", true},
      // And with it in, which also shows whether the picker actually selects it.
      {"AUTO_WITH_SIS", IntegerSchemeType::UNCOMPRESSED, true, ""},
      {"SIS_HALVES", IntegerSchemeType::SUB_INT_SPLIT, false, "0-15;16-31"},
      {"SIS_PLANNED", IntegerSchemeType::SUB_INT_SPLIT, false, ""},
  };
  // The incumbent 64-bit codecs (now real registered Integer64Schemes, not a
  // free-standing bolt-on -- see scheme/CompressionScheme64.hpp), then
  // SubIntSplit with the planner's split and with the fixed halves split.
  // UNCOMPRESSED64 is the baseline every other arm should beat on ratio;
  // BP64/FOR64/RLE64/DICT64 are what SIS64_PLANNED's point-access story
  // actually needs to be competitive with, matching the spirit of what
  // codecs32 compares SubIntSplit against.
  const std::vector<Codec64> codecs64{
      {"UNCOMPRESSED64", Integer64SchemeType::UNCOMPRESSED, false, ""},
      {"BP64", Integer64SchemeType::BP, false, ""},
      {"FOR64", Integer64SchemeType::FOR, false, ""},
      {"RLE64", Integer64SchemeType::RLE, false, ""},
      {"DICT64", Integer64SchemeType::DICT, false, ""},
      // Patched FOR and top-value-plus-exceptions. Both are registered
      // Integer64Schemes but were missing from this list, which is why they
      // existed only in a separate restricted-pool run. Both are in the
      // cross-format common set, so they belong here.
      {"PFOR64", Integer64SchemeType::PFOR, false, ""},
      {"FREQUENCY64", Integer64SchemeType::FREQUENCY, false, ""},
      // What BtrBlocks does today, with SubIntSplit out of the pool.
      {"AUTO_BASELINE64", Integer64SchemeType::UNCOMPRESSED, true, "", true},
      // And with it in, which also shows whether the picker actually selects it.
      {"AUTO_WITH_SIS64", Integer64SchemeType::UNCOMPRESSED, true, ""},
      {"SIS64_HALVES", Integer64SchemeType::SUB_INT_SPLIT, false, "0-31;32-63"},
      {"SIS64_PLANNED", Integer64SchemeType::SUB_INT_SPLIT, false, ""},
  };

  struct Dataset32 {
    std::string name;
    std::vector<INTEGER> data;
  };
  struct Dataset64 {
    std::string name;
    std::vector<s64> data;
  };

  const std::vector<Dataset32> datasets32{
      {"snowflake", generateSnowflakes<INTEGER>(rows, instagramSnowflake32(), seed)},
      {"uniform", generateUniform<INTEGER>(rows, seed)},
      {"increasing", generateIncreasing<INTEGER>(rows, seed)},
  };
  std::vector<Dataset64> datasets64{
      {"snowflake", generateSnowflakes<s64>(rows, instagramSnowflake64(), seed)},
      {"uniform", generateUniform<s64>(rows, seed)},
      {"increasing", generateIncreasing<s64>(rows, seed)},
  };

  // Real identifiers, when a converted column is supplied. The generated
  // snowflake above is a controlled reference and an easy one -- its timestamp
  // advances monotonically, so every field has textbook structure. Real IDs
  // arrive unsorted, which removes exactly that structure, so this is the
  // number to quote.
  if (!input_i64.empty()) {
    auto values = readInt64Column(input_i64, rows);
    if (values.empty()) {
      std::cerr << "warning: could not read " << input_i64 << ", skipping the tweet_ids dataset\n";
    } else {
      datasets64.push_back({"tweet_ids", std::move(values)});
    }
  }

  // Named columns from the shared corpus: XMark, OSM, Public BI, and Snowflake
  // itself when it is supplied this way rather than through --input-i64. Adding
  // a column to the cross-format comparison is a --dataset argument, not a code
  // change.
  for (const auto& [name, file_path] : named_datasets) {
    auto values = readTextColumn(file_path, rows);
    if (values.empty()) {
      std::cerr << "warning: could not read " << file_path << ", skipping dataset " << name << "\n";
      continue;
    }
    datasets64.push_back({name, std::move(values)});
  }

  std::ofstream file;
  if (!csv_path.empty()) {
    file.open(csv_path);
    if (!file.good()) {
      std::cerr << "cannot open " << csv_path << "\n";
      return 1;
    }
  }
  std::ostream& csv = csv_path.empty() ? std::cout : file;
  writeCsvHeader(csv);

  std::ofstream sections_file;
  if (!sections_csv_path.empty()) {
    sections_file.open(sections_csv_path);
    if (!sections_file.good()) {
      std::cerr << "cannot open " << sections_csv_path << "\n";
      return 1;
    }
    writeSectionsHeader(sections_file);
  }
  std::ofstream plans_file;
  if (!plans_csv_path.empty()) {
    plans_file.open(plans_csv_path);
    if (!plans_file.good()) {
      std::cerr << "cannot open " << plans_csv_path << "\n";
      return 1;
    }
    writePlansHeader(plans_file);
  }
  std::ofstream range_file;
  if (!range_csv_path.empty()) {
    range_file.open(range_csv_path);
    if (!range_file.good()) {
      std::cerr << "cannot open " << range_csv_path << "\n";
      return 1;
    }
    writeRangeHeader(range_file);
  }
  const auto emit = [&](const Result& result) {
    writeCsvRow(csv, result);
    if (sections_file.is_open()) {
      writeSectionRows(sections_file, result);
    }
    if (range_file.is_open()) {
      writeRangeRows(range_file, result);
    }
    if (plans_file.is_open()) {
      writePlanRows(plans_file, result);
    }
  };
  // A forced codec whose scheme is outside the enabled pool has nothing to
  // force (the pool lookup would dereference a missing scheme), so under
  // --shared-codecs it is skipped rather than run.
  const auto forcedSchemeMissing = [](const auto& codec) {
    if (codec.automatic) {
      return false;
    }
    using SchemeCode = std::decay_t<decltype(codec.scheme)>;
    if constexpr (std::is_same_v<SchemeCode, IntegerSchemeType>) {
      return !BtrBlocksConfig::get().integers.schemes.isEnabled(codec.scheme);
    } else {
      return !BtrBlocksConfig::get().integers64.schemes.isEnabled(codec.scheme);
    }
  };

  if (!metadata_path.empty()) {
    writeRunMetadata(metadata_path, repeats, rows);
  }

  // The `common` preset names 64-bit codecs only, so a run using it encodes
  // nothing at 32 bits. That is correct for the cross-format comparison, whose
  // columns are all read as int64, but silent zero output is exactly the kind
  // of thing that gets mistaken for a broken build.
  if (!g_codec_filter.empty()) {
    const auto selected = [&](const auto& codecs) {
      std::size_t count = 0;
      for (const auto& codec : codecs) {
        count += codecSelected(codec.name) ? 1 : 0;
      }
      return count;
    };
    if (selected(codecs32) == 0) {
      std::cerr << "note: no 32-bit codec matches --codecs, so the 32-bit arm will "
                   "produce no rows\n";
    }
    if (selected(codecs64) == 0) {
      std::cerr << "note: no 64-bit codec matches --codecs, so the 64-bit arm will "
                   "produce no rows\n";
    }
  }

  for (const auto block_size : block_sizes) {
    for (const auto& dataset : datasets32) {
      for (const auto& codec : codecs32) {
        if (!codecSelected(codec.name)) {
          continue;
        }
        if (forcedSchemeMissing(codec)) {
          std::cerr << "skip 32/" << dataset.name << "/" << codec.name
                    << ": scheme not in the pool\n";
          continue;
        }
        std::cerr << "32/" << dataset.name << "/" << codec.name << " @" << block_size << "\n";
        emit(run32(dataset.name, dataset.data, codec, block_size, repeats));
      }
    }
    for (const auto& dataset : datasets64) {
      for (const auto& codec : codecs64) {
        if (!codecSelected(codec.name)) {
          continue;
        }
        if (forcedSchemeMissing(codec)) {
          std::cerr << "skip 64/" << dataset.name << "/" << codec.name
                    << ": scheme not in the pool\n";
          continue;
        }
        std::cerr << "64/" << dataset.name << "/" << codec.name << " @" << block_size << "\n";
        emit(run64(dataset.name, dataset.data, codec, block_size, repeats));
      }
    }
    csv.flush();
    if (sections_file.is_open()) {
      sections_file.flush();
    }
    if (range_file.is_open()) {
      range_file.flush();
    }
    if (plans_file.is_open()) {
      plans_file.flush();
    }
  }

  return 0;
}
