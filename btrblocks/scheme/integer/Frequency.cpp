// ------------------------------------------------------------------------------
#include "Frequency.hpp"
// ------------------------------------------------------------------------------
#include "common/Units.hpp"
#include "compression/SchemePicker.hpp"
#include "scheme/CompressionScheme.hpp"
#include "scheme/SchemeConfig.hpp"
#include "scheme/templated/Frequency.hpp"
#include "storage/Chunk.hpp"
// -------------------------------------------------------------------------------------
#include "common/Log.hpp"
// -------------------------------------------------------------------------------------
#include <cmath>
// -------------------------------------------------------------------------------------
namespace btrblocks::integers {
// -------------------------------------------------------------------------------------
using MyFrequency = TFrequency<INTEGER, IntegerScheme, SInteger32Stats, IntegerSchemeType>;
// -------------------------------------------------------------------------------------
double Frequency::expectedCompressionRatio(SInteger32Stats& stats, u8 allowed_cascading_level) {
  if (CD(stats.unique_count) * 100.0 / CD(stats.tuple_count) >
      SchemeConfig::get().integers.frequency_threshold_pct) {
    return 0;
  }
  return IntegerScheme::expectedCompressionRatio(stats, allowed_cascading_level);
}
// -------------------------------------------------------------------------------------
u32 Frequency::compress(const INTEGER* src,
                        const BITMAP* nullmap,
                        u8* dest,
                        SInteger32Stats& stats,
                        u8 allowed_cascading_level) {
  return MyFrequency::compressColumn(src, nullmap, dest, stats, allowed_cascading_level);
}
// -------------------------------------------------------------------------------------
void Frequency::decompress(INTEGER* dest,
                           BitmapWrapper* nullmap,
                           const u8* src,
                           u32 tuple_count,
                           u32 level) {
  return MyFrequency::decompressColumn(dest, nullmap, src, tuple_count, level);
}
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
namespace {
thread_local std::vector<std::vector<INTEGER>> frequency_exceptions_scratch;
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
void Frequency::gather(INTEGER* dest,
                       const u8* src,
                       BitmapWrapper*,
                       u32 tuple_count,
                       const u32* positions,
                       u32 position_count,
                       u32 level) {
  if (tuple_count == 0 || position_count == 0) {
    return;
  }
  // Same layout-compatibility note as DynamicDictionary: use the actual
  // (also unpacked, but field-for-field identical) templated struct rather
  // than this file's own declaration, to make the wire format explicit.
  const auto& col_struct = *reinterpret_cast<const ::btrblocks::FrequencyStructure<INTEGER>*>(src);
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
    auto& scheme = IntegerSchemePicker::MyTypeWrapper::getScheme(col_struct.next_scheme);
    INTEGER* resolved =
        get_level_data(frequency_exceptions_scratch, static_cast<u32>(exception_ranks.size()), level);
    scheme.gather(resolved, col_struct.data + col_struct.exceptions_offset, nullptr,
                 static_cast<u32>(cardinality), exception_ranks.data(),
                 static_cast<u32>(exception_ranks.size()), level + 1);
    for (std::size_t j = 0; j < exception_ranks.size(); j++) {
      dest[exception_slots[j]] = resolved[j];
    }
  }
}
// -------------------------------------------------------------------------------------
INTEGER Frequency::lookupAt(const u8* src,
                            BitmapWrapper* nullmap,
                            u32 tuple_count,
                            u32 position,
                            u32 level) {
  INTEGER result = 0;
  this->gather(&result, src, nullmap, tuple_count, &position, 1, level);
  return result;
}
// -------------------------------------------------------------------------------------
INTEGER Frequency::lookup(u32) {
  UNREACHABLE();
}
void Frequency::scan(Predicate, BITMAP*, const u8*, u32) {
  UNREACHABLE();
}

std::string Frequency::fullDescription(const u8* src) {
  return MyFrequency::fullDescription(src, this->selfDescription());
}
// -------------------------------------------------------------------------------------
}  // namespace btrblocks::integers
// -------------------------------------------------------------------------------------
