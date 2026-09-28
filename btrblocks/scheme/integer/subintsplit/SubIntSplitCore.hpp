#pragma once
// -------------------------------------------------------------------------------------
#include "cache/ThreadCache.hpp"
#include "common/Exceptions.hpp"
#include "common/SIMD.hpp"
#include "common/Units.hpp"
#include "compression/SchemePicker.hpp"
#include "scheme/SchemeConfig.hpp"
#include "scheme/integer/subintsplit/CostModels.hpp"
#include "scheme/integer/subintsplit/Plan.hpp"
#include "scheme/integer/subintsplit/Sampler.hpp"
#include "scheme/integer/subintsplit/Selector.hpp"
// -------------------------------------------------------------------------------------
#include <chrono>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>
// -------------------------------------------------------------------------------------
// Encoding and decoding for SubIntSplit, templated on the unsigned value type
// so 32- and 64-bit columns share one implementation.
//
// Only the extraction and accumulation loops are width-aware. A section of at
// most 32 bits (31 with a sign-sensitive sub-scheme enabled, see
// narrowSectionMaxBits) is extracted as an ordinary INTEGER stream that the
// existing 32-bit scheme picker compresses. A 64-bit column may also have wider
// sections; those are extracted as BIGINT and compressed by the 64-bit picker,
// the same pool AUTO uses for BIGINT columns, and flagged as such in their
// descriptor so the decoder knows which pool the scheme code refers to.
// -------------------------------------------------------------------------------------
namespace btrblocks::subintsplit {
// -------------------------------------------------------------------------------------
// Wire format:
//
//   SubIntSplitHeader
//   SectionDescriptor  x section_count
//   payload            x section_count   (at the recorded offsets)
//
// Packed because dest points into the middle of a compression output block and
// is never aligned. Sections are stored least-significant range first.
struct __attribute__((packed)) SubIntSplitHeader {
  u8 format_version;
  u8 section_count;
  u8 value_bits;  // 32 or 64
  // Reserved for a sub-stream reordering transform (BWT, MTF, ...). 0 is
  // "none"; the field exists so one can be added without a format break.
  u8 reorderer_id;
  u8 data[];
};
// -------------------------------------------------------------------------------------
struct __attribute__((packed)) SectionDescriptor {
  u8 bit_start;  // inclusive, from the least significant bit
  u8 bit_end;    // inclusive
  u8 scheme_code;
  u8 flags;    // kSectionWide or 0; was padding (always 0) in format version 1
  u32 offset;  // from the start of the payload area
};
// -------------------------------------------------------------------------------------
// Version 2 added kSectionWide. Version 1 data never sets it (the byte was
// written as zero padding), so it still decodes; a version 1 reader rejects
// version 2 data rather than misreading a wide section's scheme code.
inline constexpr u8 kFormatVersion = 2;
inline constexpr u8 kMinFormatVersion = 1;
// The section's values are BIGINT and scheme_code is an Integer64SchemeType,
// compressed by the 64-bit picker. Only 64-bit columns have such sections.
inline constexpr u8 kSectionWide = 1;
inline constexpr u8 kReordererNone = 0;
// Sentinel scheme code for the uncompressed fallback: the payload is the raw
// value array. Distinct from every real code, and equal to autoScheme(), which
// is never stored as one.
inline constexpr u8 kRawSectionScheme = 255;
// -------------------------------------------------------------------------------------
// Marks the section loop as nested for the duration of its scope.
//
// Sections must never themselves be compressed with SubIntSplit: splitting an
// already-split section is meaningless, and it recurses -- the picker asks each
// candidate scheme to estimate itself, which re-enters encode() and clobbers
// the shared scratch buffers. SubIntSplit::expectedCompressionRatio declines
// when it sees itself nested, but "nested" is measured by cascade depth, and
// entering encode() directly rather than through the picker starts a level
// shallower than entering it as a chosen scheme. Raising the level explicitly
// makes the guard fire either way.
//
// Raising it also suppresses the picker's top-level ONE_VALUE special case,
// which is why encode() forces ONE_VALUE itself for constant sections.
struct NestedCompressionScope {
  NestedCompressionScope() { ThreadCache::get().compression_level++; }
  ~NestedCompressionScope() { ThreadCache::get().compression_level--; }

  NestedCompressionScope(const NestedCompressionScope&) = delete;
  NestedCompressionScope& operator=(const NestedCompressionScope&) = delete;
};
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
template <typename UT>
class SubIntSplitCore {
 public:
  static_assert(std::is_same<UT, u32>::value || std::is_same<UT, u64>::value,
                "SubIntSplit supports 32- and 64-bit values");

  static constexpr int kValueBits = static_cast<int>(sizeof(UT) * 8);

  // -----------------------------------------------------------------------------------
  static u32 encode(const UT* src,
                    const BITMAP* nullmap,
                    u8* dest,
                    u32 tuple_count,
                    u8 allowed_cascading_level) {
    // Estimation compresses a 640-value sample, and the picker runs one *after*
    // the real compress purely to log a ratio. Recording those would leave the
    // report describing a sample rather than the column, so only real
    // compressions are recorded. estimation_level is non-zero throughout any
    // estimation, since expectedCompressionRatio increments it in its condition.
    const bool record = ThreadCache::get().estimation_level == 0;
    auto& report = lastPlanReport();
    if (record) {
      report = PlanReport{};
      report.value_bits = static_cast<u8>(kValueBits);
      report.tuple_count = tuple_count;
    }

    auto& header = *reinterpret_cast<SubIntSplitHeader*>(dest);
    header.format_version = kFormatVersion;
    header.value_bits = static_cast<u8>(kValueBits);
    header.reorderer_id = kReordererNone;

    if (tuple_count == 0) {
      header.section_count = 1;
      auto& descriptor = *reinterpret_cast<SectionDescriptor*>(header.data);
      descriptor.bit_start = 0;
      descriptor.bit_end = static_cast<u8>(kValueBits - 1);
      descriptor.scheme_code = kRawSectionScheme;
      descriptor.flags = 0;
      descriptor.offset = 0;
      if (record) {
        report.raw_fallback = true;
        report.total_bytes = sizeof(SubIntSplitHeader) + sizeof(SectionDescriptor);
        report.valid = true;
      }
      return sizeof(SubIntSplitHeader) + sizeof(SectionDescriptor);
    }

    const auto plan_start = std::chrono::steady_clock::now();
    const auto* forced = forcedSplitBoundaries();
    const auto segments = planSplit(src, nullmap, tuple_count);
    if (record) {
      report.forced_boundaries = forced != nullptr && !forced->empty();
      report.plan_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - plan_start)
              .count();
    }
    const auto section_count = static_cast<u8>(segments.size());

    const u32 header_size = sizeof(SubIntSplitHeader) + section_count * sizeof(SectionDescriptor);
    auto* descriptors = reinterpret_cast<SectionDescriptor*>(header.data);
    u8* payload = dest + header_size;

    // Every section costs a full INTEGER (or BIGINT, for a wide one) per value
    // before its sub-scheme runs, so a plan whose sections all resist
    // compression can outgrow the raw input. Track the running size and abandon
    // the plan if it does; the caller hands us a buffer sized for the whole
    // datablock, so silently overrunning it would corrupt other columns.
    const u32 raw_size = tuple_count * sizeof(UT);

    const int narrow_max_bits = narrowSectionMaxBits();
    u32 written = 0;

    NestedCompressionScope nested;
    for (u8 s = 0; s < section_count; s++) {
      const auto& segment = segments[s];
      // Only a 64-bit column can have a section wider than the 32-bit pool
      // takes; the planner prices it at 64 bits and it goes to the 64-bit pool.
      const bool wide = kValueBits > 32 && segment.width() > narrow_max_bits;

      auto& descriptor = descriptors[s];
      descriptor.bit_start = static_cast<u8>(segment.bitStart);
      descriptor.bit_end = static_cast<u8>(segment.bitEnd);
      descriptor.flags = wide ? kSectionWide : 0;
      descriptor.offset = written;

      u32 used = 0;
      u8 scheme_code = 0;
      if (wide) {
        compressSection<Integer64SchemePicker>(
            src, tuple_count, segment, sectionScratch<BIGINT>(tuple_count), payload + written,
            allowed_cascading_level, CB(Integer64SchemeType::ONE_VALUE), used, scheme_code);
      } else {
        compressSection<IntegerSchemePicker>(
            src, tuple_count, segment, sectionScratch<INTEGER>(tuple_count), payload + written,
            allowed_cascading_level, CB(IntegerSchemeType::ONE_VALUE), used, scheme_code);
      }
      descriptor.scheme_code = scheme_code;
      written += used;

      if (record) {
        SectionReport entry;
        entry.bit_start = descriptor.bit_start;
        entry.bit_end = descriptor.bit_end;
        entry.predicted = segment.predictedScheme;
        entry.wide = wide;
        if (wide) {
          entry.actual64 = static_cast<Integer64SchemeType>(scheme_code);
        } else {
          entry.actual = static_cast<IntegerSchemeType>(scheme_code);
        }
        entry.bytes = used;
        report.sections.push_back(entry);
      }

      if (written > raw_size) {
        const u32 raw_bytes = encodeRaw(dest, src, tuple_count);
        if (record) {
          report.raw_fallback = true;
          report.sections.clear();
          report.total_bytes = raw_bytes;
          report.valid = true;
        }
        return raw_bytes;
      }
    }

    header.section_count = section_count;
    if (record) {
      report.total_bytes = header_size + written;
      report.valid = true;
    }
    return header_size + written;
  }

  // -----------------------------------------------------------------------------------
  static void decode(UT* dest, const u8* src, u32 tuple_count, u32 level) {
    const auto& header = *reinterpret_cast<const SubIntSplitHeader*>(src);
    validate(header);
    if (tuple_count == 0) {
      return;
    }

    const auto* descriptors = reinterpret_cast<const SectionDescriptor*>(header.data);
    const u8* payload =
        src + sizeof(SubIntSplitHeader) + header.section_count * sizeof(SectionDescriptor);

    if (isRaw(header, descriptors)) {
      std::memcpy(dest, payload, static_cast<std::size_t>(tuple_count) * sizeof(UT));
      return;
    }

    // One scratch buffer per section storage width, reused across sections.
    // BtrBlocks sub-schemes decode a whole stream at a time -- there is no
    // range or offset variant -- so a plan with N sections makes N passes over
    // the column no matter what. See docs/subintsplit.md on what that costs.
    INTEGER* scratch = decodeScratch<INTEGER>(tuple_count, level);
    BIGINT* scratch64 = nullptr;

    for (u8 s = 0; s < header.section_count; s++) {
      const auto& descriptor = descriptors[s];
      validate(descriptor, header.value_bits);
      const u8* section = payload + descriptor.offset;

      if (descriptor.flags & kSectionWide) {
        if (scratch64 == nullptr) {
          scratch64 = decodeScratch<BIGINT>(tuple_count, level);
        }
        auto& scheme = Integer64SchemePicker::MyTypeWrapper::getScheme(descriptor.scheme_code);
        scheme.decompress(scratch64, nullptr, section, tuple_count, level + 1);
        accumulate(dest, scratch64, tuple_count, descriptor, s == 0);
      } else {
        auto& scheme = IntegerSchemePicker::MyTypeWrapper::getScheme(descriptor.scheme_code);
        scheme.decompress(scratch, nullptr, section, tuple_count, level + 1);
        accumulate(dest, scratch, tuple_count, descriptor, s == 0);
      }
    }
  }

  // -----------------------------------------------------------------------------------
  // Materialize only the requested rows.
  //
  // Delegates per-section to the section's own scheme.gather() instead of a
  // full decompress(): as of the random-access work on FOR, RLE,
  // DynamicDictionary, Frequency, Truncation and FBP, most sections no longer
  // need a full O(tuple_count) decode to answer a handful of positions, so
  // SubIntSplit inherits that directly. Net cost per section is now whatever
  // that section's own scheme achieves (see each scheme's gather() for its
  // complexity), not O(tuple_count) -- the one exception being PFOR-coded
  // sections, which still fall back to full decode inside PFOR itself (see
  // extern/FastPFOR.hpp). Either way this composes automatically: SubIntSplit
  // does not need to know which case it's in.
  static void gather(UT* dest,
                     const u8* src,
                     u32 tuple_count,
                     const u32* positions,
                     u32 position_count,
                     u32 level) {
    const auto& header = *reinterpret_cast<const SubIntSplitHeader*>(src);
    validate(header);
    if (tuple_count == 0 || position_count == 0) {
      return;
    }

    const auto* descriptors = reinterpret_cast<const SectionDescriptor*>(header.data);
    const u8* payload =
        src + sizeof(SubIntSplitHeader) + header.section_count * sizeof(SectionDescriptor);

    if (isRaw(header, descriptors)) {
      const auto* values = reinterpret_cast<const UT*>(payload);
      for (u32 i = 0; i < position_count; i++) {
        dest[i] = values[positions[i]];
      }
      return;
    }

    INTEGER* scratch = gatherScratch<INTEGER>(position_count, level);
    BIGINT* scratch64 = nullptr;

    for (u8 s = 0; s < header.section_count; s++) {
      const auto& descriptor = descriptors[s];
      validate(descriptor, header.value_bits);
      const u8* section = payload + descriptor.offset;

      if (descriptor.flags & kSectionWide) {
        if (scratch64 == nullptr) {
          scratch64 = gatherScratch<BIGINT>(position_count, level);
        }
        auto& scheme = Integer64SchemePicker::MyTypeWrapper::getScheme(descriptor.scheme_code);
        scheme.gather(scratch64, section, nullptr, tuple_count, positions, position_count,
                      level + 1);
        accumulate(dest, scratch64, position_count, descriptor, s == 0);
      } else {
        auto& scheme = IntegerSchemePicker::MyTypeWrapper::getScheme(descriptor.scheme_code);
        scheme.gather(scratch, section, nullptr, tuple_count, positions, position_count,
                      level + 1);
        accumulate(dest, scratch, position_count, descriptor, s == 0);
      }
    }
  }

  // -----------------------------------------------------------------------------------
  // Largest encoding this can produce for `tuple_count` values: the raw
  // fallback, which is what encode() falls back to whenever a plan would be
  // bigger. The section-count cap is what makes this bound finite.
  static u64 maxCompressedSize(u32 tuple_count) {
    const auto max_sections = SchemeConfig::get().integers.subintsplit.max_sections;
    const u64 header =
        sizeof(SubIntSplitHeader) + static_cast<u64>(max_sections) * sizeof(SectionDescriptor);
    // A section is abandoned as soon as the running total passes the raw size,
    // so at most one section's worth of overshoot is ever written. A section
    // stores at most sizeof(UT) bytes per value (INTEGER, or BIGINT for a wide
    // section of a 64-bit column).
    const u64 overshoot = static_cast<u64>(tuple_count) * sizeof(UT) + 1024;
    return header + static_cast<u64>(tuple_count) * sizeof(UT) + overshoot;
  }

  // -----------------------------------------------------------------------------------
  // Describes the split and each section's chosen scheme, e.g.
  //   SUB_INT_SPLIT[0-9;10-22;23-63] -> ([0-9] BP) -> ([10-22] ONE_VALUE) ...
  static std::string describe(const u8* src, const std::string& self) {
    const auto& header = *reinterpret_cast<const SubIntSplitHeader*>(src);
    if (header.format_version < kMinFormatVersion || header.format_version > kFormatVersion) {
      return self + "[unknown format version]";
    }
    const auto* descriptors = reinterpret_cast<const SectionDescriptor*>(header.data);
    const u8* payload =
        src + sizeof(SubIntSplitHeader) + header.section_count * sizeof(SectionDescriptor);

    if (isRaw(header, descriptors)) {
      return self + "[raw]";
    }

    std::string boundaries;
    std::string children;
    for (u8 s = 0; s < header.section_count; s++) {
      const auto& descriptor = descriptors[s];
      const std::string range =
          std::to_string(descriptor.bit_start) + "-" + std::to_string(descriptor.bit_end);
      if (s > 0) {
        boundaries += ';';
      }
      boundaries += range;

      const u8* section = payload + descriptor.offset;
      const std::string description =
          (descriptor.flags & kSectionWide)
              ? Integer64SchemePicker::MyTypeWrapper::getScheme(descriptor.scheme_code)
                    .fullDescription(section)
              : IntegerSchemePicker::MyTypeWrapper::getScheme(descriptor.scheme_code)
                    .fullDescription(section);
      children += "\n\t-> ([" + range + "] section" +
                  ((descriptor.flags & kSectionWide) ? ", 64-bit" : "") + ") " + description;
    }
    return self + "[" + boundaries + "]" + children;
  }

  // -----------------------------------------------------------------------------------
  static u8 sectionCount(const u8* src) {
    return reinterpret_cast<const SubIntSplitHeader*>(src)->section_count;
  }

 private:
  // -----------------------------------------------------------------------------------
  static UT maskFor(int width) { return width >= kValueBits ? ~UT{0} : ((UT{1} << width) - 1); }
  // -----------------------------------------------------------------------------------
  static bool isRaw(const SubIntSplitHeader& header, const SectionDescriptor* descriptors) {
    return header.section_count == 1 && descriptors[0].scheme_code == kRawSectionScheme;
  }
  // -----------------------------------------------------------------------------------
  static void validate(const SubIntSplitHeader& header) {
    // The header is on-disk format, so a corrupt or future one must be
    // rejected rather than misparsed.
    if (header.format_version < kMinFormatVersion || header.format_version > kFormatVersion) {
      throw Generic_Exception("SubIntSplit: unsupported format version " +
                              std::to_string(header.format_version));
    }
    if (header.value_bits != kValueBits) {
      throw Generic_Exception("SubIntSplit: data was encoded at " +
                              std::to_string(header.value_bits) + " bits, decoded at " +
                              std::to_string(kValueBits));
    }
    if (header.section_count == 0) {
      throw Generic_Exception("SubIntSplit: zero sections");
    }
  }
  // -----------------------------------------------------------------------------------
  static void validate(const SectionDescriptor& descriptor, u8 value_bits) {
    if (descriptor.bit_end < descriptor.bit_start || descriptor.bit_end >= value_bits) {
      throw Generic_Exception("SubIntSplit: corrupt section bit range");
    }
    if ((descriptor.flags & ~kSectionWide) != 0 ||
        ((descriptor.flags & kSectionWide) && value_bits <= 32)) {
      throw Generic_Exception("SubIntSplit: corrupt section flags");
    }
  }
  // -----------------------------------------------------------------------------------
  // Fallback: store the values verbatim. Bounds the encoded size at the raw
  // size plus the header, unconditionally.
  static u32 encodeRaw(u8* dest, const UT* src, u32 tuple_count) {
    auto& header = *reinterpret_cast<SubIntSplitHeader*>(dest);
    header.format_version = kFormatVersion;
    header.section_count = 1;
    header.value_bits = static_cast<u8>(kValueBits);
    header.reorderer_id = kReordererNone;

    auto& descriptor = *reinterpret_cast<SectionDescriptor*>(header.data);
    descriptor.bit_start = 0;
    descriptor.bit_end = static_cast<u8>(kValueBits - 1);
    descriptor.scheme_code = kRawSectionScheme;
    descriptor.flags = 0;
    descriptor.offset = 0;

    const u32 header_size = sizeof(SubIntSplitHeader) + sizeof(SectionDescriptor);
    const auto bytes = static_cast<std::size_t>(tuple_count) * sizeof(UT);
    std::memcpy(dest + header_size, src, bytes);
    return header_size + static_cast<u32>(bytes);
  }
  // -----------------------------------------------------------------------------------
  static std::vector<SegmentPlan> planSplit(const UT* src, const BITMAP* nullmap, u32 tuple_count) {
    if (auto* forced = forcedSplitBoundaries(); forced != nullptr && !forced->empty()) {
      return *forced;
    }

    const auto& tuning = SchemeConfig::get().integers.subintsplit;
    SamplerConfig sampler_cfg;
    sampler_cfg.maxSamples = tuning.sample_size;
    sampler_cfg.blockSize = tuning.sample_block_size;

    thread_local std::vector<uint64_t> samples;
    sampleIntoU64(src, tuple_count, nullmap, samples, sampler_cfg);

    auto plan = selectSplits(samples, kValueBits, tuple_count, defaultCostModels(),
                             defaultSelectorConfig(kValueBits));
    return plan.segments;
  }
  // -----------------------------------------------------------------------------------
  // Extracts one section into `values` and compresses it with `Picker`, the
  // 32-bit picker for INTEGER sections or the 64-bit one for wide BIGINT ones.
  //
  // Constancy is tracked during extraction: the picker skips ONE_VALUE for
  // nested streams, so a constant section (a snowflake's sign bit, say) would
  // otherwise pay per-value costs for a single value.
  template <typename Picker, typename T>
  static void compressSection(const UT* src,
                              u32 tuple_count,
                              const SegmentPlan& segment,
                              std::vector<T>& values,
                              u8* dest,
                              u8 allowed_cascading_level,
                              u8 one_value_code,
                              u32& used,
                              u8& scheme_code) {
    using Unsigned = std::make_unsigned_t<T>;
    const UT mask = maskFor(segment.width());
    T first = 0;
    bool constant = true;
    for (u32 i = 0; i < tuple_count; i++) {
      const auto value = static_cast<T>(static_cast<Unsigned>((src[i] >> segment.bitStart) & mask));
      values[i] = value;
      if (i == 0) {
        first = value;
      } else if (value != first) {
        constant = false;
      }
    }
    Picker::compress(values.data(), nullptr, dest, tuple_count, allowed_cascading_level - 1, used,
                     scheme_code, constant ? one_value_code : autoScheme(), "sis_section");
  }
  // -----------------------------------------------------------------------------------
  // ORs one decoded section into place (or assigns it, for the first section).
  template <typename T>
  static void accumulate(UT* dest,
                         const T* values,
                         u32 count,
                         const SectionDescriptor& descriptor,
                         bool first) {
    using Unsigned = std::make_unsigned_t<T>;
    const int shift = descriptor.bit_start;
    const UT mask = maskFor(descriptor.bit_end - descriptor.bit_start + 1);
    if (first) {
      for (u32 i = 0; i < count; i++) {
        dest[i] = (static_cast<UT>(static_cast<Unsigned>(values[i])) & mask) << shift;
      }
    } else {
      for (u32 i = 0; i < count; i++) {
        dest[i] |= (static_cast<UT>(static_cast<Unsigned>(values[i])) & mask) << shift;
      }
    }
  }
  // -----------------------------------------------------------------------------------
  template <typename T>
  static std::vector<T>& sectionScratch(u32 tuple_count) {
    thread_local std::vector<T> scratch;
    if (scratch.size() < tuple_count) {
      scratch.resize(tuple_count);
    }
    return scratch;
  }
  // -----------------------------------------------------------------------------------
  template <typename T>
  static T* decodeScratch(u32 tuple_count, u32 level) {
    // The SIMD slack is required: TRLE stores whole 256-bit vectors past the
    // logical end of its output.
    thread_local std::vector<std::vector<T>> scratch;
    return get_level_data(scratch, tuple_count + SIMD_EXTRA_ELEMENTS(T), level);
  }
  // -----------------------------------------------------------------------------------
  template <typename T>
  static T* gatherScratch(u32 position_count, u32 level) {
    // No SIMD slack needed here: unlike decompress(), every scheme's gather()
    // writes exactly position_count entries (see e.g. IntegerScheme::gather's
    // default implementation), never a padded/rounded-up amount. A separate
    // stack from decodeScratch's, since both may be live at once (e.g. a
    // benchmark comparing decode() and gather() at the same level).
    thread_local std::vector<std::vector<T>> scratch;
    return get_level_data(scratch, position_count, level);
  }
};
// -------------------------------------------------------------------------------------
}  // namespace btrblocks::subintsplit
// -------------------------------------------------------------------------------------
