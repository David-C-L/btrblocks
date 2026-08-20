// -------------------------------------------------------------------------------------
// SubIntSplit benchmark, restricted codec pool.
//
// Measures the same things as subintsplit_bench: encode time, compression
// ratio, and the bulk, gather and point decode paths. The difference is the
// candidate pool the automatic selector and the cost-model planner are
// allowed to draw from -- narrowed via restrictedIntegerSchemes()/
// restrictedInteger64Schemes() to five codec families:
//
//   - DynamicDictionary (DICT)
//   - RLE
//   - FBP/PBP/FOR (BP, PFOR, FOR)
//   - Uncompressed
//   - Frequency
//
// ONE_VALUE stays in the pool alongside these -- it is not one of the five
// named families, but scheme/SchemePool.cpp's die_if(...) checks require it
// (and UNCOMPRESSED) to always be present, so it is mandatory infrastructure
// rather than a benchmark arm.
//
// The restriction applies both to the per-codec forced runs (each one forces
// a single scheme directly, so the pool only matters for section-level
// sub-choices SubIntSplit makes internally) and to the AUTO_* automatic-
// selector runs, with and without SubIntSplit itself in the pool. The intent
// is to see how SubIntSplit compares against, and composes with, a narrower
// and more realistic codec set than BtrBlocks' full default pool.
//
// Like subintsplit_bench, the 32-bit arm runs through Relation, Datablock and
// BtrReader -- the real chunked storage path. The 64-bit arm is driven the
// same way, through a real BIGINT column.
// -------------------------------------------------------------------------------------
#include "BenchCore.hpp"
#include "SnowflakeGen.hpp"
#include "Traces.hpp"
// -------------------------------------------------------------------------------------
#include "btrblocks.hpp"
#include "scheme/SchemeType.hpp"
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
  bool allow_missing_real_data = false;

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
    } else if (arg == "--allow-missing-real-data") {
      allow_missing_real_data = true;
    } else if (arg == "--help") {
      std::cout
          << "usage: subintsplit_restricted_bench [--rows N] [--block-sizes a,b,c]\n"
             "                         [--repeats N] [--seed N] [--csv PATH]\n"
             "                         [--input-i64 PATH] [--sections-csv PATH]\n"
             "                         [--allow-missing-real-data]\n"
             "\n"
             "  --input-i64 PATH     the real Twitter-snowflake tweet_ids column, read as a\n"
             "                       flat little-endian int64 file. This is this tool's\n"
             "                       primary dataset and is required by default -- produce\n"
             "                       one with tools/subintsplit/parquet_to_i64.py, or run\n"
             "                       tools/subintsplit/run_restricted_benchmarks.sh which does\n"
             "                       the conversion for you.\n"
             "\n"
             "  --allow-missing-real-data\n"
             "                       fall back to generated-only datasets with a warning if\n"
             "                       --input-i64 is missing or unreadable, instead of failing.\n"
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
    config.integers.schemes = restrictedIntegerSchemes();
    config.integers.schemes.enable(IntegerSchemeType::SUB_INT_SPLIT);
    config.integers64.schemes = restrictedInteger64Schemes();
    config.integers64.schemes.enable(Integer64SchemeType::SUB_INT_SPLIT);
    // Unlike subintsplit_bench, FOR is already in the restricted pool by
    // construction, so there is no need to enable it separately for the
    // FOR64 codec's override to find.
  });

  // The restricted pool's five forced arms, then the automatic selector with
  // and without SubIntSplit, then SubIntSplit itself with the planner's split
  // and with the fixed halves split that isolates the planner's contribution.
  const std::vector<Codec> codecs32{
      {"UNCOMPRESSED", IntegerSchemeType::UNCOMPRESSED, false, ""},
      {"DICT", IntegerSchemeType::DICT, false, ""},
      {"RLE", IntegerSchemeType::RLE, false, ""},
      {"BP", IntegerSchemeType::BP, false, ""},
      {"PFOR", IntegerSchemeType::PFOR, false, ""},
      {"FOR", IntegerSchemeType::FOR, false, ""},
      {"FREQUENCY", IntegerSchemeType::FREQUENCY, false, ""},
      // What BtrBlocks' restricted pool does today, with SubIntSplit out of it.
      {"AUTO_BASELINE", IntegerSchemeType::UNCOMPRESSED, true, "", true},
      // And with it in, which also shows whether the picker actually selects it.
      {"AUTO_WITH_SIS", IntegerSchemeType::UNCOMPRESSED, true, ""},
      {"SIS_HALVES", IntegerSchemeType::SUB_INT_SPLIT, false, "0-15;16-31"},
      {"SIS_PLANNED", IntegerSchemeType::SUB_INT_SPLIT, false, ""},
  };
  // Symmetric 64-bit list: PFOR64 is now a real registered scheme (see
  // scheme/integer64/PFOR64.hpp/.cpp), so the restricted 64-bit pool matches
  // the 32-bit one family-for-family.
  const std::vector<Codec64> codecs64{
      {"UNCOMPRESSED64", Integer64SchemeType::UNCOMPRESSED, false, ""},
      {"DICT64", Integer64SchemeType::DICT, false, ""},
      {"RLE64", Integer64SchemeType::RLE, false, ""},
      {"BP64", Integer64SchemeType::BP, false, ""},
      {"PFOR64", Integer64SchemeType::PFOR, false, ""},
      {"FOR64", Integer64SchemeType::FOR, false, ""},
      {"FREQUENCY64", Integer64SchemeType::FREQUENCY, false, ""},
      {"AUTO_BASELINE64", Integer64SchemeType::UNCOMPRESSED, true, "", true},
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

  // The real Twitter-snowflake column is this tool's primary dataset -- the
  // generated snowflake above is a controlled, easy reference, while real IDs
  // arrive unsorted with none of that textbook structure. Required by default;
  // --allow-missing-real-data opts back into the generated-only behavior.
  std::vector<s64> real_values;
  if (!input_i64.empty()) {
    real_values = readInt64Column(input_i64, rows);
  }
  if (real_values.empty()) {
    if (!allow_missing_real_data) {
      std::cerr
          << "error: no real dataset available (--input-i64 "
          << (input_i64.empty() ? "not given" : ("'" + input_i64 + "' unreadable or empty"))
          << ").\n"
             "       subintsplit_restricted_bench's primary dataset is the real\n"
             "       Twitter-snowflake tweet_ids column. Produce it with\n"
             "       tools/subintsplit/parquet_to_i64.py, or run\n"
             "       tools/subintsplit/run_restricted_benchmarks.sh, which does the\n"
             "       conversion for you. Pass --allow-missing-real-data to run on\n"
             "       generated data only.\n";
      return 1;
    }
    std::cerr << "warning: no real dataset available, falling back to generated data only "
                 "(--allow-missing-real-data)\n";
  } else {
    // Prepended, not appended: it runs first and so lands first in every
    // CSV/table, ahead of the generated datasets.
    datasets64.insert(datasets64.begin(), {"tweet_ids", std::move(real_values)});
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
