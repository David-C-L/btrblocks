#ifndef BTRBLOCKS_SCHEMECONFIG_H_
#define BTRBLOCKS_SCHEMECONFIG_H_
// ------------------------------------------------------------------------------
// Scheme-specific configuration options. You probably don't want to change
// these unless you really know what you are doing.
// ------------------------------------------------------------------------------
#include <cstdint>
#include "scheme/SchemeType.hpp"
// ------------------------------------------------------------------------------
namespace btrblocks {
// ------------------------------------------------------------------------------
struct SchemeConfig {
  // ------------------------------------------------------------------------------
  struct {
    // maximum percentage of unique elements in a block
    // for which frequency compression will be enabled
    uint32_t frequency_threshold_pct{50};
    // the average run length has to be higher than this for integer RLE to be
    // condierered
    uint32_t rle_run_length_threshold{2};
    // in integer RLE, override the scheme used for values with
    // this scheme instead of using the scheme picking algorithm
    IntegerSchemeType rle_force_values_scheme{autoScheme()};
    // in integer RLE, override the scheme used for run lengths with
    // this scheme instead of using the scheme picking algorithm
    IntegerSchemeType rle_force_counts_scheme{autoScheme()};
    // ----------------------------------------------------------------------
    // SubIntSplit
    // ----------------------------------------------------------------------
    struct {
      // cost in bits charged for introducing an extra section, which is what
      // stops the DP from splitting the value into many tiny sections whose
      // individual savings do not pay for their per-section overhead
      double split_penalty{10.0};
      // hard cap on sections per value. Also bounds the encoded size: every
      // section costs a full INTEGER per value before sub-compression, so an
      // unbounded section count could outgrow the compression output buffer.
      uint8_t max_sections{8};
      // widest a single section may be. A 32-bit column's sections go to the
      // ordinary 32-bit scheme picker, so for it this is further capped at 32
      // (31 when a sign-sensitive sub-scheme is enabled). A 64-bit column's
      // sections wider than that go to the 64-bit picker, so for it this is
      // the real cap; 32 restores the old behaviour. See
      // subintsplit::effectiveMaxSectionBits().
      uint8_t max_section_bits{64};
      // narrowest a single section may be
      uint8_t min_section_bits{1};
      // upper bound on values sampled when planning the split
      uint32_t sample_size{2048};
      // samples are drawn in contiguous blocks of this many values, so that
      // locally-structured fields (e.g. a timestamp that only advances every
      // few thousand rows) are visible to the run-length metrics
      uint32_t sample_block_size{128};
    } subintsplit;
  } integers;
  // ------------------------------------------------------------------------------
  struct {
    // maximum percentage of unique elements in a block
    // for which frequency compression will be enabled
    uint32_t frequency_threshold_pct{50};
    // in double RLE, override the scheme used for values with
    // this scheme instead of using the scheme picking algorithm
    DoubleSchemeType rle_force_values_scheme{autoScheme()};
    // in double RLE, override the scheme used for run lengths with
    // this scheme instead of using the scheme picking algorithm
    IntegerSchemeType rle_force_counts_scheme{autoScheme()};
    // in pseudodecimal encoding, treat a value as an outlier and patch
    // it if it requires more than this many significant digit bits
    uint32_t pseudodecimal_significant_digit_bits_limits{31};
  } doubles;
  // ------------------------------------------------------------------------------
  static constexpr size_t FSST_THRESHOLD = 16ul * 1024;
  struct {
    // in fused dictionary + fsst encoding, the dictionary needs
    // to have at least this size before we try to use FSST
    size_t dict_fsst_input_size_threshold{FSST_THRESHOLD};
    // whether to consider using FSST for dictionary encoding (does not
    // force FSST usage, merely allows it)
    bool dict_allow_fsst{true};
    // whether to force FSST usage for the dictionary (useful for
    // experimentation)
    bool dict_force_fsst{false};
    // in fsst encoding, allow this many recursive
    // compression calls for the fsst codes
    uint8_t fsst_codes_max_cascade_depth{2};
    // in FSST, override the scheme used for the codes with
    // this scheme instead of using the scheme picking algorithm
    IntegerSchemeType fsst_force_codes_scheme{autoScheme()};
  } strings;
  // ------------------------------------------------------------------------------
  static SchemeConfig& get() {
    static SchemeConfig instance;
    return instance;
  }
  // ------------------------------------------------------------------------------
};
// ------------------------------------------------------------------------------
}  // namespace btrblocks
// ------------------------------------------------------------------------------
#endif
