# SubIntSplit: porting notes

The split-selection layer under `btrblocks/scheme/integer/subintsplit/` is a port of the
implementation in Nimble (`dwio/nimble/encodings/SubIntSplit{Sampler,Metrics,CostModels,Selector,Config}.h`).
This file records every deviation and why it was necessary, so a future re-sync against upstream
changes does not have to re-derive the analysis.

The encode/decode path is **not** a port. Nimble's is built on its own `Encoding` hierarchy, memory
pools and Velox-native scan interfaces, none of which exist here; `SubIntSplitCore.hpp` is written
against BtrBlocks' contracts directly. Only the ideas carry over — the wire format's shape, the
chunked accumulate loop, and the constant-section handling.

## File correspondence

| BtrBlocks | Nimble original |
|---|---|
| `subintsplit/Sampler.hpp` | `SubIntSplitSampler.h` |
| `subintsplit/Metrics.hpp` | `SubIntSplitMetrics.h` |
| `subintsplit/CostModels.hpp` | `SubIntSplitCostModels.h` |
| `subintsplit/Selector.{hpp,cpp}` | `SubIntSplitSelector.h` |
| `subintsplit/Plan.hpp` | `SubIntSplitConfig.h` (boundary parsing) + `SegmentPlan` from the selector |

## Deviations

### 1. C++17 (mechanical)

BtrBlocks is C++17 (`CMakeLists.txt`), Nimble is C++20. Replaced:

- designated initializers → explicit field assignment;
- `std::span<const T>` → `(const T* values, std::size_t count)` pairs;
- `std::bit_width` → `bitWidthOf()`, a `__builtin_clzll` helper in `CostModels.hpp`.

Nothing semantic. If BtrBlocks moves to C++20 these can be reverted to reduce the diff against
upstream.

### 2. Counting without abseil (behaviour-preserving, faster here)

The metric collector's frequency map was `absl::flat_hash_map<uint64_t, uint32_t>`. The BtrBlocks
core links only `Threads fsst fastpfor croaring dynamic_bitset` — no abseil, and adding a dependency
to a third-party framework for one hash map is not proportionate.

Replaced with two paths, both in `Metrics.hpp`:

- **at most `kDirectCountBits` (16) wide** — a flat `std::vector<uint32_t>` indexed by value, with a
  dirty list so clearing costs the number of distinct values seen rather than the array size. No
  hashing at all. This is the common case: the bit-range grid is dominated by narrow ranges.
- **wider** — `std::unordered_map`.

The direct path is faster than the abseil original for the ranges it covers, so this is not purely a
downgrade. `CountingPathsAgree` in `test/test-cases/SubIntSplitSelector.cpp` pins that both paths
produce identical statistics across the threshold.

**If profiling shows the hash path is a real encode-time cost**, the fallback is to vendor
`ankerl::unordered_dense` (already used by the research harness in `EncodingsPlayground/`) or take
the abseil dependency. Both are a few lines: the choice is deliberately confined to one file behind
one interface.

### 3. Section storage width — a semantic fix, not a port artefact

**This is the deviation that matters most.** Nimble's `storageWidthBits()` returns 8/16/32/64,
because Nimble narrows a section to the smallest physical type that fits before nested encoding: a
12-bit section becomes a `uint16_t` stream.

BtrBlocks cannot do that. A section is handed to `IntegerSchemePicker`, whose input type is
`INTEGER`, so a 1-bit section still costs a full 4 bytes per value until its sub-scheme compresses
it. Ported verbatim, every cost model would charge 8 bits per value for a 1-bit section when the real
floor is 32 — and since the error grows as sections get narrower, the planner would systematically
over-split.

`kSectionStorageBits` is therefore a constant 32 for sections handed to the 32-bit picker, and
`kWideSectionStorageBits` (64) for wide sections of a 64-bit column, which go to the 64-bit picker;
the planner records which applies in `SegmentMetrics::storageBits`. This is a correctness fix for
this framework, not a simplification, and it must not be "corrected back" to 8/16 during a re-sync.

The underlying limitation is worth fixing upstream in BtrBlocks rather than worked around here: a
sub-stream interface parameterised on width would recover both the ratio (dictionary entries and RLE
run values currently cost 4 bytes regardless of section width) and the decode traffic.

### 4. Cost models as an interface (extension seam)

Nimble hardcodes a fixed set of cost functions behind one `bestCostBits()` free function. Here they
are an `ICostModel` interface and the planner takes a `span` of them.

The virtual call is negligible — it happens once per grid cell, against an O(n) metric pass — and it
is the seam through which the research harness's richer models can be dropped in without touching the
planner: `EncodingsPlayground/Source/encoders/selectors/costs/` has around thirty compression models
plus a speed-cost dimension and a weighted composite (`CostModelSet.hpp`). Those were deliberately
left out of this change to keep it minimal, and their speed constants are calibrated to the
playground's codecs rather than to BtrBlocks' anyway.

### 5. Scheme labels

`EncodingType` (Nimble) → `IntegerSchemeType` (BtrBlocks):

| Nimble | BtrBlocks |
|---|---|
| `Trivial` | `UNCOMPRESSED` |
| `FixedBitWidth` | `BP` |
| `Constant` | `ONE_VALUE` |
| `MainlyConstant` | `FREQUENCY` |
| `Dictionary` | `DICT` |
| `RLE` | `RLE` |
| `Varint` | *dropped* — no counterpart exists |

Per-scheme header constants were also re-tuned to BtrBlocks' actual structures (`XPBPStructure`,
`RLEStructure`, the dictionary header) rather than Nimble's encoding prefixes. These are
approximations; they matter only for very small streams, where the header is a meaningful fraction of
the total.

Note the labels are advisory. The planner records which model won for each range, but the section's
scheme is chosen independently by the picker at encode time. Nimble does the same — its
`SegmentPlan::encoding` is computed and then discarded. Keeping the prediction lets the two be
compared, which is a useful check on the cost models.

### 6. Section width cap (changed)

Nimble has no cap: its sections can be any width up to the value width. Here a section that goes to
`IntegerSchemePicker` must fit `INTEGER`, so for a 32-bit column sections cannot exceed 32 bits. For a
64-bit column a wider section is extracted as `BIGINT` and compressed by `Integer64SchemePicker`
instead (flagged `kSectionWide` in its descriptor, format version 2), so sections go up to 64 bits as
in Nimble. The planner prices such a section at 64 bits of storage (`SegmentMetrics::storageBits`,
set from the width in `selectSplits`).

The 32-bit pool's ceiling is derived at plan time from the *enabled scheme set* rather than being a
constant, because 32 is only safe when no sign-sensitive sub-scheme is enabled. `FOR` and
`Truncation8/16` do signed arithmetic on the value (`src[i] - stats.min`), which overflows for a
full-width section, so the ceiling drops to 31 when any of them is on, and a 32-bit-wide section of
a 64-bit column then goes to the 64-bit pool. See `narrowSectionMaxBits()` and
`effectiveMaxSectionBits()` in `Plan.hpp`.

This is not hypothetical: CI builds a matrix that includes `-DENABLE_FOR_SCHEME=ON`.

The fixed halves split stays a representable point in the planner's own search space, so the control
arm is a true ablation rather than a comparison against something the planner was forbidden to
choose. `max_section_bits = 32` restores the old 32-bit cap for 64-bit columns.

### 7. Section count cap (new)

Nimble's DP is one-dimensional over bit positions and bounds the section count only indirectly, via
the split penalty. Here the DP carries a second dimension over sections used, so the count can be
bounded outright.

The bound is a size guarantee, not a quality knob. Every section costs a full `INTEGER` per value
before sub-compression, so an unbounded count could produce an encoding several times the size of the
input — written into a datablock buffer shared with the other columns and sized on the assumption
that it is not. `SubIntSplitCore::encode` additionally abandons a plan that exceeds the raw size and
falls back to storing the values verbatim, which bounds the output unconditionally.

### 8. Null handling in the sampler (new)

`sampleIntoU64` takes the nullmap and skips absent rows. Null slots hold whatever was in the input
buffer, and letting that garbage into the sample would corrupt the split plan for a column with many
nulls. Nimble's sampler has no nullmap parameter.

Extraction still writes a value for every row, including null ones — matching what the dictionary
schemes do, and safe because `Chunk::operator==` only compares rows the bitmap marks present.

### 9. Fallback plan (changed)

Nimble's selector, when the DP finds nothing finite, emits a single segment covering the whole value.
That is legal here too for a 64-bit value now that wide sections exist, but not for a 32-bit value
when the section ceiling is 31, or when `max_section_bits` is lowered. The fallback therefore slices
the value at the maximum section width, which always yields a legal plan (one section when the cap
allows it).

## Things deliberately not ported

- **The AVX2 accumulate kernel** (`SubIntSplitEncoding.h`). Its narrowing paths handle `uint8_t` and
  `uint16_t` section streams, which cannot occur here — every section arrives from a sub-scheme as
  `INTEGER`. What remains would be a same-width copy loop the compiler already vectorises.
- **Chunked decode.** Nimble decodes in 4096-row chunks so scratch and output stay L1/L2-resident.
  Not portable: `IntegerScheme::decompress` has no offset or length parameter, and BP, DICT and RLE
  are not resumable. This is the single largest performance gap against Nimble and is documented in
  `subintsplit.md` as motivating an upstream range-decode API.
- **Sub-stream reorderers** (BWT, MTF). The header reserves a byte for one, so they can be added
  without a format break.
- **Preserve mode.** Nimble carries split boundaries through its encoding-selection config so a
  rewrite can reuse a previous plan. There is no equivalent config channel here; the thread-local
  override in `Plan.hpp` covers the benchmark's need, which is the only current consumer.

## Pre-existing BtrBlocks issues found while doing this

Both were found while porting the split-selection layer and were **not** fixed as part of it — but
both were fixed later, in the random-access/int64 work that builds on top of this port. Recorded here
for anyone re-syncing against upstream who runs into either symptom in an older checkout.

- **`TRUNCATION_8` and `TRUNCATION_16` were write-only.** `ITruncCompress` is implemented and
  `ITruncExpectedCF` advertised a positive ratio whenever the value range fit the code type, but
  `ITruncDecompress` (`scheme/integer/Truncation.hpp`) was a bare `UNREACHABLE()` — which expands to
  `__builtin_unreachable()`, undefined behaviour rather than a trap. Enabling them let the picker
  cascade a sub-stream into a scheme that could never read it back; the observed symptom was a smashed
  stack during decompress. `test/test-cases/RandomAccess.cpp` excluded them for this reason.
  **Fixed in `d93c723` ("fix(truncation): implement ITruncDecompress instead of bare
  UNREACHABLE()")**, part of giving the 32-bit integer schemes real `gather`/`lookupAt`. `Truncation8`
  and `Truncation16` are back in `RandomAccess.cpp`'s "every scheme" sweep as a result, and gained a
  real, addressable decode path (see `docs/subintsplit.md`'s random-access section).
- **`FBP64::compress`** (`scheme/integer/PBP.cpp`) passed `tuple_count` as the element count to a
  `u32` codec for an array of `tuple_count` `u64`s, so it compressed only the first half of the
  buffer, and `decompress` wrote `tuple_count` `u32`s. Nothing called it. It was considered as a
  64-bit baseline for the benchmark and rejected for this reason; the benchmark used a raw copy as
  its 64-bit floor instead. **Removed (not fixed in place) in `a3616ac` ("feat(int64): add FOR64 and
  BP64, remove dead/buggy free-standing FBP64")**: the native `BP64` added in that commit (a real
  `Integer64Scheme`, splitting each value into low/high 32-bit halves compressed independently
  through the ordinary 32-bit `BP`/`FBP`) supersedes it entirely, so there was nothing left worth
  patching in the old free-standing function.

### `SubIntSplit64` is no longer free-standing

At the time this file was written, `integers::SubIntSplit64` was a free-standing class driven
directly (constructing an instance, calling `compress`/`decompress` by hand) because BtrBlocks had no
64-bit scheme hierarchy for it to join — see the "encode/decode path is not a port" note above, which
still describes `SubIntSplitCore.hpp` accurately.

That changed in `9d07157` ("feat(int64): promote SubIntSplit64 to a registered Integer64Scheme"):
`SubIntSplit64` is now a normal `Integer64Scheme` subclass, registered under
`Integer64SchemeType::SUB_INT_SPLIT` (opt-in, exactly like the 32-bit scheme), reachable through
`Datablock::compress`/`decompress`, `BtrReader`, and `Integer64SchemePicker::chooseScheme` like any
other `BIGINT` codec. Its sections are still compressed by the ordinary 32-bit scheme pool — that part
of the design is unchanged — but the outer scheme itself is no longer a special case callers have to
know about. See `scheme/CompressionScheme64.hpp` and `docs/subintsplit.md`'s "Status and scope"
section for the current picture.
