// ------------------------------------------------------------------------------
#include "Frequency64.hpp"
// ------------------------------------------------------------------------------
#include "common/Units.hpp"
#include "compression/SchemePicker.hpp"
#include "scheme/CompressionScheme64.hpp"
#include "scheme/SchemeConfig.hpp"
#include "scheme/templated/Frequency.hpp"
#include "storage/Chunk.hpp"
// -------------------------------------------------------------------------------------
#include "common/Log.hpp"
// -------------------------------------------------------------------------------------
#include <cmath>
// -------------------------------------------------------------------------------------
namespace btrblocks::integers64 {
// -------------------------------------------------------------------------------------
using MyFrequency64 = TFrequency<s64, Integer64Scheme, SInteger64Stats, Integer64SchemeType>;
// -------------------------------------------------------------------------------------
double Frequency64::expectedCompressionRatio(SInteger64Stats& stats, u8 allowed_cascading_level) {
  // Shares the 32-bit threshold: there is no separate integers64 config
  // section.
  if (CD(stats.unique_count) * 100.0 / CD(stats.tuple_count) >
      SchemeConfig::get().integers.frequency_threshold_pct) {
    return 0;
  }
  return Integer64Scheme::expectedCompressionRatio(stats, allowed_cascading_level);
}
// -------------------------------------------------------------------------------------
u32 Frequency64::compress(const BIGINT* src,
                          const BITMAP* nullmap,
                          u8* dest,
                          SInteger64Stats& stats,
                          u8 allowed_cascading_level) {
  return MyFrequency64::compressColumn(src, nullmap, dest, stats, allowed_cascading_level);
}
// -------------------------------------------------------------------------------------
void Frequency64::decompress(BIGINT* dest,
                             BitmapWrapper* nullmap,
                             const u8* src,
                             u32 tuple_count,
                             u32 level) {
  return MyFrequency64::decompressColumn(dest, nullmap, src, tuple_count, level);
}
// -------------------------------------------------------------------------------------
namespace {
thread_local std::vector<std::vector<BIGINT>> frequency64_exceptions_scratch;
// 0-based index of `position` among the exceptions, for a position known to be
// one. That is rank(position) - 1, but the vendored CRoaring (b88b002) gets
// bitset-container rank wrong when position % 64 == 63: its word loop already
// covers that word, then it adds the popcount of the *next* word as well. For
// a member, rank(position) - 1 == rank(position - 1), and position - 1 is then
// 62 mod 64, so ask for whichever of the two avoids the bad case.
u32 exceptionIndex(const Roaring& exceptions, u32 position) {
  if (position % 64 == 63) {
    return static_cast<u32>(exceptions.rank(position - 1));
  }
  return static_cast<u32>(exceptions.rank(position) - 1);
}
}  // namespace
// -------------------------------------------------------------------------------------
void Frequency64::gather(BIGINT* dest,
                         const u8* src,
                         BitmapWrapper*,
                         u32 tuple_count,
                         const u32* positions,
                         u32 position_count,
                         u32 level) {
  if (tuple_count == 0 || position_count == 0) {
    return;
  }
  // Same layout-compatibility note as DynamicDictionary64: use the actual
  // (also unpacked, but field-for-field identical) templated struct rather
  // than this file's own declaration, to make the wire format explicit.
  const auto& col_struct = *reinterpret_cast<const ::btrblocks::FrequencyStructure<s64>*>(src);
  Roaring exceptions_bitmap = Roaring::read(reinterpret_cast<const char*>(col_struct.data), false);
  const u64 cardinality = exceptions_bitmap.cardinality();
  if (cardinality == 0) {
    for (u32 i = 0; i < position_count; i++) {
      dest[i] = col_struct.top_value;
    }
    return;
  }
  // Split into "dominant value" (most positions) vs "exception" (needs a
  // sub-scheme lookup), each resolved without ever decoding tuple_count rows.
  std::vector<u32> exception_ranks;
  std::vector<u32> exception_slots;
  for (u32 i = 0; i < position_count; i++) {
    if (exceptions_bitmap.contains(positions[i])) {
      exception_ranks.push_back(exceptionIndex(exceptions_bitmap, positions[i]));
      exception_slots.push_back(i);
    } else {
      dest[i] = col_struct.top_value;
    }
  }
  if (!exception_ranks.empty()) {
    auto& scheme = Integer64SchemePicker::MyTypeWrapper::getScheme(col_struct.next_scheme);
    BIGINT* resolved = get_level_data(frequency64_exceptions_scratch,
                                      static_cast<u32>(exception_ranks.size()), level);
    scheme.gather(resolved, col_struct.data + col_struct.exceptions_offset, nullptr,
                 static_cast<u32>(cardinality), exception_ranks.data(),
                 static_cast<u32>(exception_ranks.size()), level + 1);
    for (std::size_t j = 0; j < exception_ranks.size(); j++) {
      dest[exception_slots[j]] = resolved[j];
    }
  }
}
// -------------------------------------------------------------------------------------
BIGINT Frequency64::lookupAt(const u8* src,
                             BitmapWrapper* nullmap,
                             u32 tuple_count,
                             u32 position,
                             u32 level) {
  BIGINT result = 0;
  this->gather(&result, src, nullmap, tuple_count, &position, 1, level);
  return result;
}
// -------------------------------------------------------------------------------------
BIGINT Frequency64::lookup(u32) {
  UNREACHABLE();
}
void Frequency64::scan(Predicate, BITMAP*, const u8*, u32) {
  UNREACHABLE();
}

std::string Frequency64::fullDescription(const u8* src) {
  return MyFrequency64::fullDescription(src, this->selfDescription());
}
// -------------------------------------------------------------------------------------
}  // namespace btrblocks::integers64
// -------------------------------------------------------------------------------------
