#ifndef BTRBLOCKS_SCHEMETYPE_H_
#define BTRBLOCKS_SCHEMETYPE_H_
// ------------------------------------------------------------------------------
#include <cstdint>
#include "SchemeSet.hpp"
// ------------------------------------------------------------------------------
namespace btrblocks {
// ------------------------------------------------------------------------------
// List of available compression schemes. Can be enabled/disabled for
// compression in using the configuration mechanism.
// ------------------------------------------------------------------------------
enum class IntegerSchemeType : uint8_t {
  UNCOMPRESSED = 0,
  ONE_VALUE = 1,
  DICT = 2,
  RLE = 3,
  PFOR = 4,
  BP = 5,
  // Bit-range splitting: decomposes each value into contiguous bit-range
  // sub-streams chosen by a sample-driven DP, each compressed independently by
  // the ordinary scheme picker. Opt-in; not in defaultIntegerSchemes().
  SUB_INT_SPLIT = 6,
  // legacy schemes
  FREQUENCY = 25,
  FOR = 26,
  PFOR_DELTA = 27,
  TRUNCATION_8 = 28,
  TRUNCATION_16 = 29,
  DICTIONARY_8 = 30,
  DICTIONARY_16 = 31,
  SCHEME_MAX = 32
};
using IntegerSchemeSet = SchemeSet<IntegerSchemeType>;
constexpr IntegerSchemeSet defaultIntegerSchemes() {
  return {IntegerSchemeType::UNCOMPRESSED, IntegerSchemeType::ONE_VALUE, IntegerSchemeType::DICT,
          IntegerSchemeType::RLE,          IntegerSchemeType::PFOR,      IntegerSchemeType::BP};
};
// The five requested codec "families" plus the two schemes every scheme set
// must include (see scheme/SchemePool.cpp's die_if(...ONE_VALUE...)/
// die_if(...UNCOMPRESSED...)): DynamicDictionary, RLE, FBP/PBP/FOR,
// Uncompressed, Frequency.
constexpr IntegerSchemeSet restrictedIntegerSchemes() {
  return {IntegerSchemeType::UNCOMPRESSED, IntegerSchemeType::ONE_VALUE,
          IntegerSchemeType::DICT,         IntegerSchemeType::RLE,
          IntegerSchemeType::BP,           IntegerSchemeType::PFOR,
          IntegerSchemeType::FOR,          IntegerSchemeType::FREQUENCY};
};
// ------------------------------------------------------------------------------
// Native 64-bit sibling of IntegerSchemeType, for ColumnType::BIGINT. Mirrors
// the 32-bit enum's registered set 1:1 (values need not match the 32-bit
// enum's legacy-code numbering, since this is a separate scheme-code space).
// SUB_INT_SPLIT stays opt-in-only here too, exactly like the 32-bit scheme.
// ------------------------------------------------------------------------------
enum class Integer64SchemeType : uint8_t {
  UNCOMPRESSED = 0,
  ONE_VALUE = 1,
  DICT = 2,
  RLE = 3,
  PFOR = 4,
  BP = 5,
  SUB_INT_SPLIT = 6,
  FREQUENCY = 7,
  FOR = 8,
  TRUNCATION = 9,
  // Split like the 32-bit enum's DICTIONARY_8/DICTIONARY_16 (fixed, O(1)
  // random-access dictionaries with u8/u16 codes respectively), rather than
  // sharing one slot, so both code widths can be registered and picked
  // between at once -- exactly as on the 32-bit side.
  DICTIONARY_8 = 10,
  DICTIONARY_16 = 11,
  SCHEME_MAX = 32
};
using Integer64SchemeSet = SchemeSet<Integer64SchemeType>;
constexpr Integer64SchemeSet defaultInteger64Schemes() {
  return {Integer64SchemeType::UNCOMPRESSED, Integer64SchemeType::ONE_VALUE,
          Integer64SchemeType::DICT,         Integer64SchemeType::RLE,
          Integer64SchemeType::PFOR,         Integer64SchemeType::BP};
};
// 64-bit sibling of restrictedIntegerSchemes(), same five families plus the
// two always-required schemes.
constexpr Integer64SchemeSet restrictedInteger64Schemes() {
  return {Integer64SchemeType::UNCOMPRESSED, Integer64SchemeType::ONE_VALUE,
          Integer64SchemeType::DICT,         Integer64SchemeType::RLE,
          Integer64SchemeType::BP,           Integer64SchemeType::PFOR,
          Integer64SchemeType::FOR,          Integer64SchemeType::FREQUENCY};
};
// ------------------------------------------------------------------------------
enum class DoubleSchemeType : uint8_t {
  UNCOMPRESSED = 0,
  ONE_VALUE = 1,
  DICT = 2,
  RLE = 3,
  FREQUENCY = 4,
  PSEUDODECIMAL = 5,
  // legacy schemes
  DOUBLE_BP = 28,
  DICTIONARY_8 = 29,
  DICTIONARY_16 = 31,
  SCHEME_MAX = 32
};
using DoubleSchemeSet = SchemeSet<DoubleSchemeType>;
constexpr DoubleSchemeSet defaultDoubleSchemes() {
  return {DoubleSchemeType::UNCOMPRESSED, DoubleSchemeType::ONE_VALUE,
          DoubleSchemeType::DICT,         DoubleSchemeType::RLE,
          DoubleSchemeType::FREQUENCY,    DoubleSchemeType::PSEUDODECIMAL};
};
// ------------------------------------------------------------------------------
enum class StringSchemeType : uint8_t {
  UNCOMPRESSED = 0,
  ONE_VALUE = 1,
  DICT = 2,
  FSST = 3,
  // legacy schemes
  DICTIONARY_8 = 30,
  DICTIONARY_16 = 31,
  SCHEME_MAX = 32
};
using StringSchemeSet = SchemeSet<StringSchemeType>;
constexpr StringSchemeSet defaultStringSchemes() {
  return {StringSchemeType::UNCOMPRESSED, StringSchemeType::ONE_VALUE, StringSchemeType::DICT,
          StringSchemeType::FSST};
};
// ------------------------------------------------------------------------------
// When overriding schemes, pass this value to use automatic scheme selection.
constexpr auto autoScheme() {
  return 255;
}
// ------------------------------------------------------------------------------
}  // namespace btrblocks
// ------------------------------------------------------------------------------
#endif  // BTRBLOCKS_SCHEMETYPE_H_
