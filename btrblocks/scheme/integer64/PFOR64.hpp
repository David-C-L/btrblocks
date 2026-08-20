#pragma once
// -------------------------------------------------------------------------------------
#include "scheme/CompressionScheme64.hpp"
// -------------------------------------------------------------------------------------
namespace btrblocks::integers64 {
// -------------------------------------------------------------------------------------
struct PFOR64Structure {
  u32 low_used;  // bytes used by the low-32-bits stream, so the high stream is
                 // addressable without decoding the low one first
  u8 low_scheme_code;
  u8 high_scheme_code;
  u8 data[];  // [low-32-bits stream][high-32-bits stream]
};
// -------------------------------------------------------------------------------------
// Near-exact mirror of BP64: splits each value into its low and high 32-bit
// halves (BtrBlocks' vendored FastPFOR has no native 64-bit bit-packing) and
// compresses each half independently through the ordinary 32-bit
// IntegerSchemePicker. Unlike BP64 -- which passes autoScheme() and lets the
// picker choose freely per half -- PFOR64 forces CB(IntegerSchemeType::PFOR)
// on both halves, so this is genuinely "PFOR applied to both halves" rather
// than a duplicate of BP64's auto-picked behavior. gather()/lookupAt() still
// delegate to whichever scheme each half's picker chose (here, always PFOR),
// inheriting that scheme's random-access behavior.
//
// Like PBP/FBP (see scheme/integer/PBP.hpp), PFOR64 inherits PBP's lack of a
// mini-block gather()/lookupAt() override per half: SIMDFastPFor's
// patched-exception layout doesn't offer FastBinaryPacking's cheap
// header-skip structure, so each half falls back to a full-chunk decode.
// This is a known, accepted limitation, not a bug to fix.
class PFOR64 : public Integer64Scheme {
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
  inline static Integer64SchemeType staticSchemeType() { return Integer64SchemeType::PFOR; }
  BIGINT lookup(u32) override;
  void scan(Predicate, BITMAP*, const u8*, u32) override;
};
// -------------------------------------------------------------------------------------
}  // namespace btrblocks::integers64
// -------------------------------------------------------------------------------------
