// -------------------------------------------------------------------------------------
// SubIntSplit benchmark.
//
// Compares SubIntSplit against the integer codecs BtrBlocks already has, across
// encode time, compression ratio, and the bulk, gather and point decode paths.
//
// The 32-bit arm runs through Relation, Datablock and BtrReader, so it measures
// the real chunked storage path rather than a synthetic one. The 64-bit arm is
// driven directly, because BtrBlocks has no 64-bit scheme hierarchy for it to
// join -- but its sections are still compressed by the ordinary 32-bit pool, so
// the comparison is against the same codecs either way.
//
// Two control arms matter for interpreting the results:
//   - a fixed halves split (0-31;32-63), which isolates what the planner's
//     choice of boundaries is worth from what splitting at all is worth;
//   - uniformly random data, where there is no bit-range structure and
//     splitting should not help.
// -------------------------------------------------------------------------------------
#include "BenchCore.hpp"
#include "SnowflakeGen.hpp"
#include "Traces.hpp"
// -------------------------------------------------------------------------------------
#include "btrblocks.hpp"
// -------------------------------------------------------------------------------------
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
// -------------------------------------------------------------------------------------
using namespace btrblocks;
using namespace btrblocks::subintsplit_bench;
// -------------------------------------------------------------------------------------
int main(int argc, char** argv) {
  uint32_t rows = 1u << 20;
  std::vector<uint32_t> block_sizes{4096, 8192, 65536};
  int repeats = 5;
  uint32_t seed = 42;
  std::string csv_path;
  std::string sections_csv_path;
  std::string input_i64;

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
    } else if (arg == "--help") {
      std::cout << "usage: subintsplit_bench [--rows N] [--block-sizes a,b,c] [--repeats N]\n"
                   "                         [--seed N] [--csv PATH] [--input-i64 PATH]\n"
                   "                         [--sections-csv PATH]\n"
                   "\n"
                   "  --input-i64 PATH     add a 64-bit dataset read from a flat little-endian\n"
                   "                       int64 file, for measuring against real data rather\n"
                   "                       than generated. Produce one with parquet_to_i64.py.\n"
                   "\n"
                   "  --sections-csv PATH  write one row per section of each SubIntSplit plan:\n"
                   "                       bit range, the scheme the planner predicted, the\n"
                   "                       scheme actually chosen, and the bytes it cost.\n";
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return 1;
    }
  }

  BtrBlocksConfig::configure([](BtrBlocksConfig& config) {
    config.integers.schemes = defaultIntegerSchemes();
    config.integers.schemes.enable(IntegerSchemeType::SUB_INT_SPLIT);
    config.integers64.schemes = defaultInteger64Schemes();
    config.integers64.schemes.enable(Integer64SchemeType::SUB_INT_SPLIT);
    // FOR64 is a legacy scheme excluded from defaultInteger64Schemes() (like
    // its 32-bit counterpart), but the FOR64 codec below forces it directly,
    // so it has to be in the pool for the override to find.
    config.integers64.schemes.enable(Integer64SchemeType::FOR);
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
  const auto emit = [&](const Result& result) {
    writeCsvRow(csv, result);
    if (sections_file.is_open()) {
      writeSectionRows(sections_file, result);
    }
  };

  for (const auto block_size : block_sizes) {
    for (const auto& dataset : datasets32) {
      for (const auto& codec : codecs32) {
        std::cerr << "32/" << dataset.name << "/" << codec.name << " @" << block_size << "\n";
        emit(run32(dataset.name, dataset.data, codec, block_size, repeats));
      }
    }
    for (const auto& dataset : datasets64) {
      for (const auto& codec : codecs64) {
        std::cerr << "64/" << dataset.name << "/" << codec.name << " @" << block_size << "\n";
        emit(run64(dataset.name, dataset.data, codec, block_size, repeats));
      }
    }
    csv.flush();
    if (sections_file.is_open()) {
      sections_file.flush();
    }
  }

  return 0;
}
