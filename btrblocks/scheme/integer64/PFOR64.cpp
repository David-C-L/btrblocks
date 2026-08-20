#include "PFOR64.hpp"
#include "common/Units.hpp"
#include "compression/SchemePicker.hpp"
#include "scheme/CompressionScheme64.hpp"
// -------------------------------------------------------------------------------------
#include <vector>
// -------------------------------------------------------------------------------------
namespace btrblocks::integers64 {
// -------------------------------------------------------------------------------------
double PFOR64::expectedCompressionRatio(SInteger64Stats& stats, u8 allowed_cascading_level) {
  return Integer64Scheme::expectedCompressionRatio(stats, allowed_cascading_level);
}
// -------------------------------------------------------------------------------------
u32 PFOR64::compress(const BIGINT* src,
                     const BITMAP* nullmap,
                     u8* dest,
                     SInteger64Stats& stats,
                     u8 allowed_cascading_level) {
  auto& col_struct = *reinterpret_cast<PFOR64Structure*>(dest);
  const u32 n = stats.tuple_count;
  std::vector<INTEGER> low(n), high(n);
  for (u32 i = 0; i < n; i++) {
    const auto v = static_cast<UBIGINT>(src[i]);
    low[i] = static_cast<INTEGER>(static_cast<u32>(v));
    high[i] = static_cast<INTEGER>(static_cast<u32>(v >> 32));
  }
  u8* write_ptr = col_struct.data;
  u32 used_low = 0;
  u32 used_high = 0;
  IntegerSchemePicker::compress(low.data(), nullmap, write_ptr, n, allowed_cascading_level - 1,
                                used_low, col_struct.low_scheme_code, CB(IntegerSchemeType::PFOR),
                                "pfor64_low");
  write_ptr += used_low;
  col_struct.low_used = used_low;
  IntegerSchemePicker::compress(high.data(), nullmap, write_ptr, n, allowed_cascading_level - 1,
                                used_high, col_struct.high_scheme_code,
                                CB(IntegerSchemeType::PFOR), "pfor64_high");
  write_ptr += used_high;
  return write_ptr - dest;
}
// -------------------------------------------------------------------------------------
namespace {
thread_local std::vector<std::vector<INTEGER>> pfor64_decode_low_scratch;
thread_local std::vector<std::vector<INTEGER>> pfor64_decode_high_scratch;
thread_local std::vector<std::vector<INTEGER>> pfor64_gather_low_scratch;
thread_local std::vector<std::vector<INTEGER>> pfor64_gather_high_scratch;
}  // namespace
// -------------------------------------------------------------------------------------
void PFOR64::decompress(BIGINT* dest,
                        BitmapWrapper* nullmap,
                        const u8* src,
                        u32 tuple_count,
                        u32 level) {
  const auto& col_struct = *reinterpret_cast<const PFOR64Structure*>(src);
  INTEGER* low = get_level_data(pfor64_decode_low_scratch,
                                tuple_count + SIMD_EXTRA_ELEMENTS(INTEGER), level);
  INTEGER* high = get_level_data(pfor64_decode_high_scratch,
                                 tuple_count + SIMD_EXTRA_ELEMENTS(INTEGER), level);
  IntegerSchemePicker::MyTypeWrapper::getScheme(col_struct.low_scheme_code)
      .decompress(low, nullmap, col_struct.data, tuple_count, level + 1);
  IntegerSchemePicker::MyTypeWrapper::getScheme(col_struct.high_scheme_code)
      .decompress(high, nullmap, col_struct.data + col_struct.low_used, tuple_count, level + 1);
  for (u32 i = 0; i < tuple_count; i++) {
    dest[i] = static_cast<BIGINT>(static_cast<UBIGINT>(static_cast<u32>(low[i])) |
                                  (static_cast<UBIGINT>(static_cast<u32>(high[i])) << 32));
  }
}
// -------------------------------------------------------------------------------------
void PFOR64::gather(BIGINT* dest,
                    const u8* src,
                    BitmapWrapper* nullmap,
                    u32 tuple_count,
                    const u32* positions,
                    u32 position_count,
                    u32 level) {
  if (tuple_count == 0 || position_count == 0) {
    return;
  }
  const auto& col_struct = *reinterpret_cast<const PFOR64Structure*>(src);
  INTEGER* low = get_level_data(pfor64_gather_low_scratch, position_count, level);
  INTEGER* high = get_level_data(pfor64_gather_high_scratch, position_count, level);
  IntegerSchemePicker::MyTypeWrapper::getScheme(col_struct.low_scheme_code)
      .gather(low, col_struct.data, nullmap, tuple_count, positions, position_count, level + 1);
  IntegerSchemePicker::MyTypeWrapper::getScheme(col_struct.high_scheme_code)
      .gather(high, col_struct.data + col_struct.low_used, nullmap, tuple_count, positions,
             position_count, level + 1);
  for (u32 i = 0; i < position_count; i++) {
    dest[i] = static_cast<BIGINT>(static_cast<UBIGINT>(static_cast<u32>(low[i])) |
                                  (static_cast<UBIGINT>(static_cast<u32>(high[i])) << 32));
  }
}
// -------------------------------------------------------------------------------------
BIGINT PFOR64::lookupAt(const u8* src,
                        BitmapWrapper* nullmap,
                        u32 tuple_count,
                        u32 position,
                        u32 level) {
  BIGINT result = 0;
  this->gather(&result, src, nullmap, tuple_count, &position, 1, level);
  return result;
}
// -------------------------------------------------------------------------------------
BIGINT PFOR64::lookup(u32) {
  UNREACHABLE();
}
void PFOR64::scan(Predicate, BITMAP*, const u8*, u32) {
  UNREACHABLE();
}
// -------------------------------------------------------------------------------------
std::string PFOR64::fullDescription(const u8* src) {
  const auto& col_struct = *reinterpret_cast<const PFOR64Structure*>(src);
  auto& low_scheme = IntegerSchemePicker::MyTypeWrapper::getScheme(col_struct.low_scheme_code);
  auto& high_scheme = IntegerSchemePicker::MyTypeWrapper::getScheme(col_struct.high_scheme_code);
  return this->selfDescription() + " -> ([low32] section) " + low_scheme.fullDescription(col_struct.data) +
        " -> ([high32] section) " + high_scheme.fullDescription(col_struct.data + col_struct.low_used);
}
// -------------------------------------------------------------------------------------
}  // namespace btrblocks::integers64
// -------------------------------------------------------------------------------------
