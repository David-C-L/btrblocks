#pragma once
// -------------------------------------------------------------------------------------
#include "scheme/CompressionScheme64.hpp"
// -------------------------------------------------------------------------------------
#include <string>
// -------------------------------------------------------------------------------------
namespace btrblocks::integers {
// -------------------------------------------------------------------------------------
// SubIntSplit for 64-bit columns, registered as a real Integer64Scheme
// (Integer64SchemeType::SUB_INT_SPLIT) and selectable through
// Integer64SchemePicker like every other *64 codec. Delegates entirely to the
// existing, unmodified subintsplit::SubIntSplitCore<u64> -- the same core
// that already backs the 32-bit SubIntSplit (SubIntSplitCore<u32>) -- so
// nothing about the encoding, planning, or wire format changes here; this is
// purely a wrapper promotion from free-standing statics to virtual overrides.
//
// A section of at most 32 bits (31 with a sign-sensitive 32-bit scheme
// enabled) is an ordinary INTEGER stream compressed by the whole existing
// 32-bit scheme pool. A wider section, up to the full 64 bits, is extracted as
// BIGINT and compressed by the 64-bit pool (Integer64SchemePicker, the one AUTO
// uses for BIGINT columns), and the planner prices it at 64 bits of storage.
// Only the extraction and accumulation loops are 64-bit.
//
// Bit patterns are round-tripped, so signedness is irrelevant: values are
// taken as u64 and never arithmetically manipulated.
//
// The value is not split into two 32-bit halves. Where the boundaries fall is
// the entire question the planner answers, and 0-31/32-63 is just one
// candidate answer among many -- one the benchmark uses as a control arm.
//
// Opt-in only (not in defaultInteger64Schemes()), mirroring the 32-bit
// SubIntSplit's opt-in status exactly.
class SubIntSplit64 : public Integer64Scheme {
 public:
  double expectedCompressionRatio(SInteger64Stats& stats, u8 allowed_cascading_level) override;
  u32 compress(const BIGINT* src,
               const BITMAP* nullmap,
               u8* dest,
               SInteger64Stats& stats,
               u8 allowed_cascading_level) override;
  void decompress(BIGINT* dest,
                  BitmapWrapper* nullmap,
                  const u8* src,
                  u32 tuple_count,
                  u32 level) override;
  // Delegates per-section to each section's own scheme.gather() (see
  // SubIntSplitCore::gather), so random access is exactly as good as the
  // composition of its sections' own schemes -- not a fixed constant-factor
  // win over decompress() anymore. See docs/subintsplit.md and
  // integers::SubIntSplit::gather.
  void gather(BIGINT* dest,
              const u8* src,
              BitmapWrapper* nullmap,
              u32 tuple_count,
              const u32* positions,
              u32 position_count,
              u32 level) override;
  BIGINT lookupAt(const u8* src,
                  BitmapWrapper* nullmap,
                  u32 tuple_count,
                  u32 position,
                  u32 level) override;
  std::string fullDescription(const u8* src) override;
  inline Integer64SchemeType schemeType() override { return staticSchemeType(); }
  inline static Integer64SchemeType staticSchemeType() { return Integer64SchemeType::SUB_INT_SPLIT; }
  BIGINT lookup(u32) override;
  void scan(Predicate, BITMAP*, const u8*, u32) override;

  // Bytes an encoding of `tuple_count` values can occupy at most, so callers
  // can size a destination buffer. Encoding never exceeds this. Not part of
  // the Integer64Scheme interface (no other scheme exposes a sizing bound),
  // kept as a static utility for callers that need to size a buffer ahead of
  // calling compress(), same as before this class existed.
  static u64 maxCompressedSize(u32 tuple_count);

  // The split and the scheme chosen for each section count. Static utilities
  // for tooling/tests that want to introspect an already-encoded buffer
  // without going through the picker.
  static u8 sectionCount(const u8* src);
};
// -------------------------------------------------------------------------------------
}  // namespace btrblocks::integers
// -------------------------------------------------------------------------------------
