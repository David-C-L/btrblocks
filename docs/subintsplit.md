# SubIntSplit

SubIntSplit decomposes each integer into contiguous bit-range sub-streams — *sections* — chosen by a
sample-driven dynamic program, and compresses each section independently through the ordinary scheme
picker.

It targets values assembled from semantic bit-fields, where different ranges of the same integer have
wildly different statistics and no single scheme fits the value as a whole. A snowflake identifier is
the canonical case:

```
 63          23           10        0
  +-----------+-----------+---------+
  | timestamp |   shard   | sequence|      41 / 13 / 10 bits
  +-----------+-----------+---------+
   slow, monotone   few distinct   cycles 0..1023
```

Bit-packing the whole value is bounded by the timestamp's magnitude. A dictionary sees a million
distinct values. RLE sees no runs, because the sequence changes every row. Split into three streams,
each field is trivial: the timestamp is long runs, the shard is a handful of values, the sequence is
a dense 10-bit counter.

Other data with the same shape: IPv4 addresses, composite keys, packed enums and flags, fixed-point
values with a coarse exponent.

## Status and scope

- **32-bit** (`IntegerSchemeType::SUB_INT_SPLIT`) is a normal registered scheme, usable anywhere in
  the Relation/Datablock/BtrReader pipeline.
- **64-bit** (`integers::SubIntSplit64`) is *also* a normal registered scheme now
  (`Integer64SchemeType::SUB_INT_SPLIT`), reachable through the same Relation/Datablock/BtrReader
  pipeline as any other `ColumnType::BIGINT` codec. It started out free-standing and driven directly,
  because BtrBlocks originally had no 64-bit scheme hierarchy for it to join; that hierarchy
  (`Integer64Scheme`, `scheme/CompressionScheme64.hpp`) was added later and `SubIntSplit64` was
  promoted onto it. Its sections are still compressed by the ordinary 32-bit pool, so it competes
  against the same codecs either way — that part of the design never changed.
- **Opt-in.** `SUB_INT_SPLIT` is deliberately absent from both `defaultIntegerSchemes()` and
  `defaultInteger64Schemes()`, so existing behaviour and benchmark numbers do not move unless it is
  enabled:

```cpp
BtrBlocksConfig::configure([](BtrBlocksConfig& config) {
  config.integers.schemes.enable(IntegerSchemeType::SUB_INT_SPLIT);
  config.integers64.schemes.enable(Integer64SchemeType::SUB_INT_SPLIT);
});
```

### 64-bit support

`ColumnType::BIGINT` is a full native column type now, not a bolt-on grafted onto the 32-bit path: it
has its own scheme base class (`Integer64Scheme`, `scheme/CompressionScheme64.hpp`), its own scheme
code space (`Integer64SchemeType`, `scheme/SchemeType.hpp`) and its own picker
(`Integer64SchemePicker`). `Datablock::compress`/`decompress` and `BtrReader` (including
`gatherColumn64`/`lookupColumn64`, the 64-bit siblings of `gatherColumn`/`lookupColumn`) dispatch on
it exactly as they do for `INTEGER`. The registered `Integer64SchemeType` set is `UNCOMPRESSED`,
`ONE_VALUE`, `DICT` (`DynamicDictionary64`), `RLE`, `BP` (splits each value into low/high 32-bit
halves, each compressed through the ordinary 32-bit picker — the vendored FastPFOR library has no
native 64-bit bit-packing), `PFOR` (`PFOR64` — same low/high halves split as `BP64`, but forces
`PFOR`/`PBP` specifically on both halves rather than letting the picker choose, so it measures
patched bit-packing rather than duplicating `BP64`), `FOR`, `TRUNCATION`, `DICTIONARY_8`/`DICTIONARY_16`,
`FREQUENCY`, and `SUB_INT_SPLIT`. `FOR`, `TRUNCATION`, `DICTIONARY_8`/`16` and `SUB_INT_SPLIT` are
legacy/opt-in, like their 32-bit counterparts — `PFOR` is not: it was already listed in
`defaultInteger64Schemes()` before `PFOR64` existed to back it, which made enabling it a silent no-op;
it is a real registered scheme now. `PFOR64` inherits 32-bit `PBP`'s lack of a mini-block
`gather`/`lookupAt` override (see *Random-access cost by scheme family* below), per half, so it falls
back to full-chunk decode there like `PBP` itself does.

This means every `*64` codec — including `SubIntSplit64` — automatically inherits whatever
random-access capability its composition provides: `BP64`'s `gather` delegates to whichever 32-bit
scheme each half's picker chose, so it gets `FBP`'s mini-block gather (see below) for free; SubIntSplit64
delegates per-section the same way. See `test/test-cases/RandomAccess64.cpp` for the cross-codec
correctness sweep this enables at 64 bits, mirroring `RandomAccess.cpp` at 32.

## Wire format

Version 1. Little-endian, packed — the destination points into the middle of a compression output
block and is never aligned.

```
struct SubIntSplitHeader {      // 4 bytes
  u8 format_version;            // 1
  u8 section_count;             // 1..max_sections
  u8 value_bits;                // 32 or 64
  u8 reorderer_id;              // 0 = none; reserved
};

struct SectionDescriptor {      // 8 bytes, section_count of them
  u8  bit_start;                // inclusive, from the least significant bit
  u8  bit_end;                  // inclusive
  u8  scheme_code;              // IntegerSchemeType, or 255 for the raw fallback
  u8  padding;
  u32 offset;                   // from the start of the payload area
};

// then: section payloads, at the recorded offsets
```

Sections are stored least-significant range first and tile `[0, value_bits)` exactly.

Two fields exist purely so the format can evolve without breaking already-written data.
`format_version` is validated on decode, so an unrecognised version is refused rather than misparsed
into a wild section count. `reorderer_id` reserves room for a sub-stream reordering transform (BWT,
MTF) to be added later.

`scheme_code` is stored per section because BtrBlocks sub-streams are not self-describing — the
parent must record what compressed each child. `255` marks the raw fallback, where the payload is the
value array verbatim; it never collides with a real code.

Decode also validates each section's bit range, since the header is on-disk format and a corrupt one
must not produce out-of-range shifts.

## Selection

Planning runs on a sample, not the full column.

1. **Sample.** Up to `sample_size` values, drawn as contiguous blocks of `sample_block_size`. Blocks
   rather than a stride because run-length statistics depend on local structure: a timestamp field
   that only changes every few thousand rows looks like noise under stride sampling and like the long
   runs it actually has under block sampling. Rows the nullmap marks absent are skipped, since null
   slots hold arbitrary bytes that would otherwise corrupt the plan.

2. **Score every candidate range.** For each bit range `[l, r]`, extract its values and estimate what
   each cost model says it would cost. The extractor extends `r` one bit at a time so the inner loop
   reuses the previous range's work. Per-sample costs are scaled to the full stream, so the split
   penalty is denominated in the same units as the savings.

3. **Dynamic program.** `dp[i][k]` is the cheapest way to cover bits `[0, i)` using exactly `k`
   sections; each additional section is charged `split_penalty`. Backtracking from the best `k` gives
   the plan. Complexity is `O(value_bits² × sample_size)` for the scoring grid, which dominates.

4. **Encode.** Each section is extracted and handed to `IntegerSchemePicker`, which chooses its scheme
   independently. The planner's prediction is recorded but not binding — keeping both lets them be
   compared, which is a useful check on the cost models.

Cost models are an interface (`ICostModel`), not a fixed set, so richer ones — speed-aware, or
weighted composites over several dimensions — can be supplied without changing the planner.

## Configuration

Under `SchemeConfig::get().integers.subintsplit`:

| Field | Default | Effect |
|---|---|---|
| `split_penalty` | 10.0 | Bits charged per extra section. Raise to split less. |
| `max_sections` | 8 | Hard cap on sections. Also a size bound — see below. |
| `max_section_bits` | 32 | Widest section. Cannot exceed 32; narrowed to 31 automatically when a sign-sensitive scheme is enabled. |
| `min_section_bits` | 1 | Narrowest section. |
| `sample_size` | 2048 | Values sampled for planning. Raising it sharpens estimates marginally and slows planning quadratically. |
| `sample_block_size` | 128 | Contiguous sample block length. 0 selects stride sampling. |

## Random access

This change also adds a random-access API to `IntegerScheme` and `BtrReader`. It is a framework
capability, available to every codec, not a SubIntSplit-private one — which is what makes the
benchmark a fair comparison rather than one scheme measured natively and the rest through ad-hoc
driver code.

**Per chunk**, on `IntegerScheme`:

```cpp
virtual void gather(INTEGER* dest, const u8* src, BitmapWrapper* nullmap, u32 tuple_count,
                    const u32* positions, u32 position_count, u32 level);
virtual INTEGER lookupAt(const u8* src, BitmapWrapper* nullmap, u32 tuple_count,
                         u32 position, u32 level);
```

The defaults decompress the chunk once into thread-local scratch and index it, so every registered
scheme gets a working implementation with no per-scheme work. `gather` is
`O(tuple_count + position_count)`, never `O(position_count × tuple_count)`. `lookupAt` is left at
`O(tuple_count)` per call rather than memoizing on `src` — that is the true cost of a point lookup
against a scheme with no random access, and caching would be unsound since the caller may reuse the
buffer.

That default is no longer the whole story. Most 32-bit codecs, and every 64-bit codec through
composition (see *64-bit support* above), now override `gather`/`lookupAt` with something better than
"decode the chunk and index it" — see *Random-access cost by scheme family* below for exactly what
each one buys. `Integer64Scheme` (`scheme/CompressionScheme64.hpp`) mirrors this interface exactly,
`s64`/`SInteger64Stats`/`Integer64SchemeType` in place of `INTEGER`/`SInteger32Stats`/
`IntegerSchemeType`, with the same default-then-override structure.

**Per column**, on `BtrReader`:

```cpp
void gatherColumn(INTEGER* dest, const u32* positions, u32 position_count,
                  u32* chunks_touched = nullptr);
INTEGER lookupColumn(u32 position);
// and the BIGINT siblings:
void gatherColumn64(BIGINT* dest, const u32* positions, u32 position_count,
                    u32* chunks_touched = nullptr);
BIGINT lookupColumn64(u32 position);
```

A column is stored as independently-compressed chunks, each recording its own scheme, so a scheme
instance only ever sees one chunk. `gatherColumn`/`gatherColumn64` bucket global positions by chunk
and call each touched chunk's `gather` once, dispatching on that chunk's own `compression_type`
exactly as `readColumn` does. They therefore work for every registered scheme (32- and 64-bit alike)
and pick up any native override for free.

`chunks_touched` is reported because it, not the row count, is what a gather costs.

Scope: `IntegerScheme`/`Integer64Scheme` only. `DoubleScheme` would be a mechanical addition;
`StringScheme` is a different problem (variable length, `decompressNoCopy`).

### Random-access cost by scheme family

What each family's `gather`/`lookupAt` override actually costs, now that most of them have one. This
supersedes the old blanket claim that nothing but `Uncompressed`/`OneValue` had real random access.

| Family | Cost | Why |
|---|---|---|
| `Uncompressed`, `Uncompressed64` | `O(1)` | Values are stored verbatim; a lookup is a direct index. |
| `OneValue`, `OneValue64` | `O(1)` | Every row holds the same value; a lookup never touches storage. |
| `Dictionary8`/`16`, `Dictionary8_64`/`16_64` | `O(1)` | Fixed-width codes into a fixed-width dictionary — a code lookup, then a dictionary lookup, both direct indexing. |
| `Truncation8`/`16`, `Truncation64` | `O(1)` | A fixed-width biased code; add the base back. Previously write-only (`ITruncDecompress` was a bare `UNREACHABLE()`) — see `docs/subintsplit-porting.md`. |
| `FOR`, `FOR64` | `O(child)` | A bias wrapper: `gather`/`lookupAt` delegate to the child scheme, then add the bias back. Costs whatever the child costs. |
| `RLE`, `RLE64` | `O(log runs + child)` | A run-offset index (binary search over run boundaries) locates which run a row falls in, then one child lookup for that run's value. |
| `DynamicDictionary`, `DynamicDictionary64` | `O(child code lookup)` | Codes are themselves compressed by a nested scheme (not fixed-width, unlike `Dictionary8/16`), so a lookup costs one child `lookupAt` for the code plus a dictionary-slot read. |
| `Frequency`, `Frequency64` | `O(1)` typical, `O(log exceptions)` worst case | The dominant value is O(1); an exception position is located via a roaring bitmap's `rank`/`contains`, which is sublinear in the exception count. |
| `FBP`/`BP` (`FastPFOR`-backed bit-packing), `BP64` | `O(mini-block)` | FastPFOR's fixed-size blocks let a single value be unpacked from one mini-block without decoding the rest of the array — new methods added to the FastPFOR wrapper (`extern/FastPFOR.hpp`/`.cpp`) expose this. `BP64` inherits it automatically: it delegates to whichever 32-bit scheme each half's picker chose. |
| `PFOR` (`SIMDFastPFor`-backed), `PFOR64` | `O(tuple_count)` — no override | Deliberately **not** given mini-block access. `PFOR`'s per-page patched-exception layout (exceptions stored out-of-line, patched back in during a full decode) does not offer the same cheap fixed-block structure `FBP` has; giving it real random access would mean re-deriving which page a row's exception patch belongs to without decoding the page, which is a materially bigger change than the other schemes here needed. Left as a deliberate scope decision rather than attempted and abandoned. `PFOR64` inherits this per half, same as `BP64` inherits `FBP`'s mini-block gather per half — it just inherits the absence instead. |
| `SubIntSplit`, `SubIntSplit64` | Sum of each section's cost | `gather` delegates per-section to that section's own (now-improved) `gather` rather than decoding the whole value — see `SubIntSplitCore::gather` in `scheme/integer/subintsplit/SubIntSplitCore.hpp`. A lookup costs the sum of each section's own lookup cost, not a full-value decode. This is the change that makes the *Speed* results below possible; the old design decoded every section on every lookup regardless of what each section's own scheme could do. |
| Everything else | `O(tuple_count + position_count)` for `gather`, `O(tuple_count)` per `lookupAt` | The framework default: decode the chunk into thread-local scratch and index it. |

## Datasets

Four inputs, one real and three generated. Every figure below is measured over the first 1,048,576
rows — the benchmark's default — not asserted from the generator definitions.

| Dataset | Source | Width | Structure | Role |
|---|---|---|---|---|
| `tweet_ids` | Real: 30.7M Twitter IDs | 64 | Four bit-fields, timestamp near-unique per row | **The number to quote** |
| `snowflake` | Generated | 32 & 64 | Same field shape, dense burst | Upper bound: the easy case |
| `increasing` | Generated | 32 & 64 | Monotone, small steps | What bit-packing already handles |
| `uniform` | Generated | 32 & 64 | No structure | **Control:** a split must not help |

### `tweet_ids` — real Twitter identifiers

`EncodingsPlayground/Datasets/TwitterSnowflake/tweet_ids.parquet`, the same column the research
harness uses. 30,761,504 `int64` values; the benchmark reads the leading `--rows` of them.

Layout is Twitter's, and the fields behave very differently from one another:

| Field | Bits | Distinct | Range | Avg run |
|---|---|---|---|---|
| `sequence` | 0–11 (12) | 4,096 | 0–4,095 | 1.6 |
| `worker` | 12–16 (5) | 32 | 0–31 | 1.1 |
| `datacenter` | 17–21 (5) | 32 | 0–31 | 8.3 |
| `timestamp` | 22–62 (41) | 992,155 | spans ~527 days | 1.0 |

The column is **99.96% descending** — newest ID first, the usual export order — with a few large
discontinuities where sources were concatenated.

The property that actually governs compressibility is not the ordering but the **sampling density**:
these are ~1M tweets drawn across roughly 527 days, a median of ~47 minutes apart. At millisecond
resolution that means the 41-bit timestamp field takes a near-unique value on every row (992,155
distinct in 1,048,576, average run 1.0). There is simply nothing for a run- or dictionary-based scheme
to exploit in the field that occupies two thirds of the value.

### `snowflake` — generated, and deliberately the easy case

Instagram's layout: 41-bit timestamp, 13-bit shard, 10-bit sequence at 64 bits; a 21/7/4 analogue at
32 bits. The shard field carries only 16 of its 8,192 possible values (5 of 128 at 32 bits), which is
realistic — a deployment sizes the field for growth.

| Field (64-bit) | Bits | Distinct | Avg run |
|---|---|---|---|
| `sequence` | 0–9 (10) | 1,024 | 1.0 |
| `shard` | 10–22 (13) | 16 | 1.1 |
| `timestamp` | 23–63 (41) | 1,024 | **1,024.0** |

That last figure is the whole difference from the real data. The generator models a **dense burst** —
it emits 1,024 IDs per millisecond before ticking the clock — so across a million rows the timestamp
takes just 1,024 distinct values in runs of 1,024. The real column's timestamp takes 992,155 values in
runs of 1.

So `snowflake` is best read as an upper bound on what splitting can achieve, and `tweet_ids` as what
it achieves on data nobody shaped for it. The two differ by roughly 4x in ratio for exactly this
reason.

### `increasing` — monotone, with a caveat

Starts at 1,000,000 and steps by 1–8. Strictly ascending, every value distinct.

Worth being explicit about why this scores so well at 64 bits: over a million rows the values never
exceed 2^23, so **41 of the 64 bits are constant zero**. Much of the ratio there comes from isolating
that dead range rather than from anything subtle, and it flatters the 64-bit numbers. The 32-bit arm,
where only 9 bits are dead, is the more meaningful reading.

### `uniform` — the control

Full-width random values: all 64 bits live, every value distinct, no runs. There is no bit-range
structure to find, so a split cannot help. Any ratio above 1.00 here would mean the planner had
fooled itself, and the size bound is what guarantees it never lands below.

## Results

Produced by `subintsplit_bench`; see *Reproducing* below. Compression ratio is raw bytes over encoded
bytes, so higher is better.

The tables here are a narrative subset — the comparisons that carry the argument. For the full grid,
every codec against every dataset at every block size with column winners marked, see
[`benchmark-results/`](../benchmark-results/), generated by `tools/subintsplit/run_benchmarks.sh`.
It carries its own copy of the CSVs it was built from, so the two sets of numbers are internally
consistent but will differ from each other by a percent or two — see the caveat at the end of *Where
the bytes actually go*.

One million rows per dataset, five repeats, median. `AUTO_BASELINE` is automatic selection with
SubIntSplit taken out of the pool — what BtrBlocks does today. `*_HALVES` is a fixed split at the
32-bit boundary (`0-15;16-31` and `0-31;32-63`), run through identical machinery so that the only
difference from `*_PLANNED` is where the boundaries fall.

See *Datasets* above for what each input is. In short: `tweet_ids` is real and is the number to
quote; the generated `snowflake` is an upper bound, because its timestamp field repeats in runs of
1,024 where the real one is near-unique per row.

### Compression ratio

`block_size = 65536`, higher is better.

| | `tweet_ids` *(real)* | snowflake | increasing | uniform *(control)* |
|---|---|---|---|---|
| **32-bit** | | | | |
| `BP` | — | 1.11 | 1.43 | 1.00 |
| `PFOR` | — | 1.12 | 1.44 | 1.00 |
| `AUTO_BASELINE` | — | 1.11 | 1.43 | 1.00 |
| `SIS_HALVES` | — | 2.63 | 2.09 | 1.00 |
| **`SIS_PLANNED`** | — | **4.11** | **4.29** | 1.00 |
| **64-bit** | | | | |
| `UNCOMPRESSED64` | 1.00 | 1.00 | 1.00 | 1.00 |
| `BP64` | 1.09 | 1.09 | 2.06 | 1.00 |
| `FOR64` | 1.09 | 1.09 | 2.27 | 1.00 |
| `RLE64` | 1.09 | 1.09 | 2.06 | 1.00 |
| `AUTO_BASELINE64` | 1.09 | 1.09 | 2.06 | 1.00 |
| `SIS64_HALVES` | 1.09 | 1.09 | 2.06 | 1.00 |
| **`SIS64_PLANNED`** | **1.54** | **6.29** | **8.58** | 1.00 |

`tweet_ids` is 64-bit only, matching the research harness: truncating a 64-bit snowflake to 32 bits
would destroy the field structure under test — hence the 32-bit rows have no `tweet_ids` entry, not
because those codecs weren't run against it.

Five things to read out of this — regenerated end-to-end through the rewired `run64()` (Relation →
Datablock → BtrReader, the same pipeline `run32()` always used; see *64-bit support* above), not
carried over from before that existed.

**On real data the scheme wins, but modestly: 1.54× against the best incumbent 64-bit codec's 1.09×.**
Unlike when this was first measured, BtrBlocks now *has* real 64-bit codecs to compare against —
`BP64`/`FOR64`/`RLE64` are registered `Integer64Scheme`s, not a hypothetical — and `AUTO_BASELINE64`
(the picker with SubIntSplit excluded) already lands on `BP64`'s 1.09× on its own. So the honest
baseline is 1.09×, not 1.00×, and SubIntSplit's real contribution on real data is that gap: roughly
1.4× on top of what BtrBlocks already does. The generated snowflake's 6.29× is still not the number to
quote for real data — see *Datasets* for why: the real timestamp field is near-unique per row, the
generated one repeats in runs of 1,024.

**Choosing where to split is worth most of the benefit, and more so on real data.** At 64 bits the
planner reaches 6.29× against the fixed halves split's 1.09× on generated data (the halves split
doesn't even beat plain `BP64` here, since Instagram's synthetic boundaries don't land on bit 32
either), and 1.54× against 1.09× on real. On the real column the fixed split recovers nothing beyond
what `BP64` already gets, because Twitter's field boundaries — 12, 17, 22 — are nowhere near bit 32.
That is the case a fixed split structurally cannot serve, and it is why the planner exists.

**The gain over the incumbent is large on generated data because the incumbent has less to work with
there.** Bit-packing a snowflake is bounded by the timestamp's magnitude, so 32-bit `BP`/`PFOR` manage
1.1× and 64-bit `BP64`/`FOR64`/`RLE64` all land within noise of 1.09×. That is not a weakness of those
schemes; it is what motivates splitting the value in the first place.

**Uniform data yields exactly 1.00 for every codec, including this one.** There is no bit-range
structure to find, the planner declines to split, and the size bound keeps a bad plan from ever being
worse than raw. A number above 1.00 there would mean the planner was fooling itself.

`AUTO_WITH_SIS64` matches `SIS64_PLANNED` on every dataset measured (1.543 vs 1.539 on `tweet_ids`,
identical elsewhere — the small `tweet_ids` gap is run-to-run planner sampling noise, see the caveat
in *Where the bytes actually go*), so the picker does select the scheme when it is available — though
see limitation 5 before relying on that. This now holds at both widths: `AUTO_WITH_SIS` (32-bit) and
`AUTO_WITH_SIS64` behave the same way.

### The planner recovers the real field layout

On the real Twitter column the planner chooses:

```
0-2 ; 3-11 ; 12-17 ; 18-21 ; 22-50 ; 51-55 ; 56-63
```

Twitter's actual snowflake layout is `sequence` in bits 0–11, `worker id` in 12–16, `datacenter id`
in 17–21 and `timestamp` in 22–62. Two of the three field boundaries — **12** and **22** — are
recovered exactly, and the third lands one bit out (18 against 17). The planner is given no schema,
only 2048 sampled values.

That it rediscovers the layout from the data is the clearest evidence the cost models are measuring
something real rather than curve-fitting. It also explains the shape of the plan: the timestamp
(22–63) is subdivided further, because its high bits barely move and go to `RLE` while its low bits
do and go to `BP`; the near-constant `datacenter` range lands on `RLE`; the sequence and worker
ranges land on `BP`.

Boundary 18 rather than 17 is not really an error. `datacenter` holds only five distinct values in
1–13, so bit 21 is almost never set and the entropy boundary genuinely sits a bit above the schema
boundary.

### Where the bytes actually go

`--sections-csv` reports one row per section: its bit range, the scheme the planner predicted, the
scheme the picker actually chose, and what it cost. The run-level CSV gives one total per encoding,
which cannot say whether a wide field dominates the output or a narrow one is being wasteful.

For `tweet_ids` at `block_size = 65536`:

| Section | Bits | Scheme | bits/value | Share |
|---|---|---|---|---|
| 0–2 | 3 | `BP` | 2.80 | 6.9% |
| 3–11 | 9 | `BP` | 0.83 | 2.0% |
| 12–17 | 6 | `BP` | 6.14 | 15.2% |
| 18–21 | 4 | `RLE` | 0.43 | 1.1% |
| **22–50** | **29** | **`BP`** | **28.93** | **71.4%** |
| 51–55 | 5 | `RLE` | 1.24 | 3.1% |
| 56–63 | 8 | `RLE` | 0.13 | 0.3% |

**One section is 71% of the output, and it is incompressible.** The low 29 bits of the timestamp cost
28.93 bits per value out of 29 — essentially pure entropy, exactly as the sampling density in
*Datasets* predicts. That single row explains the whole 1.55× ceiling on real data, and it says the
ceiling is a property of the data rather than something better cost models or more sections could
lift. The other six sections together are already compressed to about 10.6 bits for 35 bits of field.

**Planning, not compression, dominates encode time.** The report times the sampler and the DP
separately: 459 ms of a 65536-row chunk's encode, against roughly 854 ms total — over half. That is
the concrete answer to limitation 4, and it points at `sample_size` and `max_sections` as the knobs
that matter. Forced-boundary runs report 0 ms, which is the expected self-check: they bypass the
planner entirely.

**The planner's predictions agree with the picker 84% of the time** (26 of 31 planner-chosen
sections). The disagreements are not random — the dominant one is `BP` predicted where `DICT` won, on
low-cardinality ranges worth 54–94% of their encodings. The dictionary cost model is too pessimistic,
which is a concrete lead for improving the models rather than a vague suspicion. Sections whose
boundaries were forced report `-` rather than a prediction, since the planner never ran for them and
counting them would flatter the figure.

*Caveat:* per-section byte counts come from one chunk and vary slightly between runs, because
BtrBlocks' scheme selection samples with a `std::random_device` seed (`NumberStats::samples`).
Repeated identical invocations differ by around 2% in total encoded size, so these figures indicate
proportions rather than exact byte counts.

### Speed

Regenerated after the random-access work (`tools/subintsplit/run_benchmarks.sh`, 1,048,576 rows).
Snowflake, `block_size = 65536`, milliseconds. Gather is the clustered trace (1% selectivity, mean run
64); point access is 256 individual lookups.

| | encode | bulk decode | gather | point (256) |
|---|---|---|---|---|
| `UNCOMPRESSED` | 215 | 0.54 | 0.11 | 0.07 |
| `PFOR` | 220 | 0.49 | 0.34 | 3.73 |
| `BP` | 217 | 0.83 | 0.64 | 0.48 |
| `RLE` | 436 | 2.49 | 2.20 | 10.90 |
| `AUTO_BASELINE` | 220 | 0.80 | 0.64 | 0.48 |
| `SIS_PLANNED` (32) | 672 | 1.74 | 1.24 | 1.25 |
| `SIS64_PLANNED` | 1363 | 4.40 | 1.83 | 1.87 |
| `SIS64_PLANNED` on `tweet_ids` | 2343 | 8.15 | 3.67 | 4.71 |

Encode is roughly 3× the incumbent and gets worse as blocks shrink, because planning runs per chunk
(see the block-size table below). Bulk decode is still the per-section pass structure of limitation 1
(each section is a full decode pass), so it stays a few times the incumbent's.

**Every cost scales with the section count, so the real column is the more expensive one.** The
planner picks 7 sections for `tweet_ids` against 5 for the generated snowflake — the same shape as
before this work; splitting itself did not change. Anyone benchmarking encode throughput should lower
`max_sections`, which trades ratio for time directly.

**Point access is no longer the outlier it was.** Two things moved it, and it's worth separating them:
`BP`'s own point access dropped from a full-chunk decode to 0.48 ms once it got mini-block `gather`
(this branch's FBP mini-block work) — plain BP, unrelated to SubIntSplit. On top of that,
`SIS_PLANNED`'s point access dropped from 53 ms to 1.25 ms and `SIS64_PLANNED` on the real `tweet_ids`
column from 70 ms to 4.71 ms, because `SubIntSplitCore::gather` now delegates per-section to each
section's own (also-improved) `gather` instead of decoding every section on every lookup. SubIntSplit
is still slower than a single-stream scheme at point access — `SIS64_PLANNED` costs about 10× `BP64`'s
0.86 ms on `tweet_ids`, since a lookup composes several sections' costs instead of paying one — but the
gap closed from roughly two orders of magnitude to one, and it is no longer *worse* than `PFOR`
(3.73 ms), which has no mini-block access at all (a deliberate scope decision — see the random-access
cost table above). `RLE`'s point access also dropped an order of magnitude (129.92 ms → 10.90 ms) from
its own run-offset index. `PFOR` and `UNCOMPRESSED` are essentially unchanged, as expected: neither
was touched by this work.

### Block size

Snowflake, 64-bit planned split.

| `block_size` | ratio | encode ms | decode ms | gather ms |
|---|---|---|---|---|
| 4096 | 6.91 | 24333 | 4.99 | 1.61 |
| 8192 | 7.05 | 7456 | 3.60 | 1.22 |
| 65536 | 6.29 | 1363 | 4.40 | 1.83 |

Smaller blocks improve decode and gather — the working set stays cache-resident, and a gather pays
for less of each chunk it barely touches — while costing dramatically more to encode, since the
planner runs once per chunk. Ratio is nearly flat, so the block size is a pure speed trade here.

The decode and gather half of that trade is a workaround for a missing capability, not a real tuning
knob: with a range-capable `decompress` the same locality would be available at any block size.

### Gather traces

Uniform gather touches every chunk at every block size tested — 16 of 16, 128 of 128, 256 of 256 —
because 4096 positions over a column of at most 256 chunks hits all of them with overwhelming
probability. In that regime every codec degenerates to a full column decode and the measurement says
nothing about any of them.

Clustered traces (1% selectivity, mean run 64) touch 118 of 256 chunks at `block_size = 4096` and 91
of 128 at 8192, so chunk locality is actually exercised. This is why the benchmark reports chunks
touched next to every gather timing, and why the numbers above use the clustered trace.

## Restricted codec set

`subintsplit_bench` compares SubIntSplit against BtrBlocks' full incumbent pool. A second binary,
`subintsplit_restricted_bench`, asks a narrower question: how does SubIntSplit compare against, and
compose with, a deliberately smaller set of five codec families — DynamicDictionary (`DICT`), RLE,
FBP/PBP/FOR (`BP`/`PFOR`/`FOR`), Uncompressed, and Frequency — rather than everything BtrBlocks can
register?

The restriction is built from `restrictedIntegerSchemes()`/`restrictedInteger64Schemes()`
(`scheme/SchemeType.hpp`, next to `defaultIntegerSchemes()`/`defaultInteger64Schemes()`) and applies
at both levels the question can be asked at:

- **Per-codec forced runs.** `UNCOMPRESSED`, `DICT`, `RLE`, `BP`, `PFOR`, `FOR`, `FREQUENCY` each
  forced directly, plus `AUTO_BASELINE`/`AUTO_WITH_SIS` (the restricted pool's own automatic selector,
  with SubIntSplit excluded and included) and `SIS_HALVES`/`SIS_PLANNED`. Symmetric at 64 bits —
  `PFOR64` existing now (see *64-bit support* above) is what makes that symmetry possible.
- **SubIntSplit's own per-section sub-codec choice.** Each SubIntSplit section is still handed to the
  ordinary `IntegerSchemePicker`, which only ever sees whatever `BtrBlocksConfig::get().integers.schemes`
  currently has enabled — restricted or not, this was already true (see *Selection* above, step 4).
  What was **not** already true: the DP planner's own cost-model candidates (`CostModels.hpp`,
  `defaultCostModels()`) used to be a fixed, compile-time list — `{Uncompressed, BitPacking, OneValue,
  Frequency, Dictionary, RLE}` — with no `PFOR`/`FOR` model at all, and no awareness of what was
  actually enabled. `defaultCostModels()` now filters its (now eight-model, `PFOR`/`FOR` added)
  candidate list against `BtrBlocksConfig::get().integers.schemes` on every call, so the planner's
  *predictions* and the picker's *actual* choices always draw from the identical pool — restricted here,
  the full default set in `subintsplit_bench`, or anything else a caller configures. Before this fix the
  two could silently diverge (the planner could "predict" a disabled scheme, or never predict one that
  was enabled but unmodeled).

**`ONE_VALUE` ("Constant") is not one of the five families and cannot be restricted out.** It is
required infrastructure, not a benchmark arm: `scheme/SchemePool.cpp`'s `die_if(...)` checks abort at
startup if it (or `UNCOMPRESSED`) is ever disabled, because two call sites depend on it unconditionally
and outside the normal enabled/disabled machinery — `SchemePicker.hpp`'s constant-column shortcut and
`SubIntSplitCore`'s constant-section shortcut both call `getScheme(ONE_VALUE)` directly, with no cost
comparison and no enabled check. `getScheme()` is an unchecked map lookup; if `ONE_VALUE` were ever
unregistered, both would dereference a null scheme and segfault rather than fail cleanly. Making it
safely optional would mean auditing and changing both bypass sites to fall back (e.g. to
`UNCOMPRESSED`) — out of scope here, and `ONE_VALUE`/Constant was never one of the requested families.
It stays enabled everywhere, alongside the five.

### Results

Primary dataset is `tweet_ids` — the real column, not the generated snowflake (see *Datasets* above for
why the generated one is an upper bound, not the number to quote). `subintsplit_restricted_bench`
enforces this: it requires `--input-i64` by default and refuses to run without it, unless
`--allow-missing-real-data` is passed (`run_restricted_benchmarks.sh` mirrors this with
`ALLOW_MISSING_REAL_DATA=1`).

TODO: full-sweep numbers (1,048,576 rows, five repeats, all three block sizes) — regenerate with
`tools/subintsplit/run_restricted_benchmarks.sh` and replace this table. The figures below are from a
pipeline-check run only (`QUICK=1`, 262,144 rows, one repeat, `block_size = 65536`) and are directional,
not final:

| Codec (restricted pool, 64-bit) | Ratio on `tweet_ids` |
|---|---|
| `UNCOMPRESSED64` | 1.00 |
| `DICT64` | 1.00 |
| `BP64` / `PFOR64` / `FOR64` / `RLE64` / `FREQUENCY64` | 1.07 |
| `AUTO_BASELINE64` (restricted pool, no SIS) | 1.07 |
| `SIS64_HALVES` | 1.07 |
| `SIS64_PLANNED` | ~1.55 |
| `AUTO_WITH_SIS64` (restricted pool, with SIS) | ~1.55 |

Directionally, this reproduces the same story as the full-pool results above: every restricted-pool
incumbent lands around the same 1.07× on real data (none of the five families has anything to exploit
in the near-unique timestamp field on its own), the fixed halves split recovers nothing beyond that
because Twitter's field boundaries don't land near bit 32, and the planner's chosen boundaries are what
gets to ~1.55× — consistent with `SIS64_PLANNED` finding two of the three real field boundaries exactly
(see *The planner recovers the real field layout* above). `AUTO_WITH_SIS64` matching `SIS64_PLANNED`
confirms the restricted pool's own automatic selector does pick SubIntSplit when it's available, same
as the full pool does.

### Reproducing

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target subintsplit_restricted_bench
tools/subintsplit/run_restricted_benchmarks.sh
```

Same flags, same CSV schema, same `make_tables.py` rendering as `subintsplit_bench` (see *Reproducing*
above) — only the candidate pool and the primary-dataset requirement differ. Output lands in
`benchmark-results-restricted/` by default (`OUT_DIR` to change it); CSVs are named
`restricted-codecs-results.csv`/`restricted-codecs-sections.csv` so they never collide with
`subintsplit_bench`'s output when both are run against the same `build/`.

## Limitations

These are properties of BtrBlocks as it stands, not of the idea, and each points at something worth
fixing upstream.

### 1. Bulk decode makes one pass per section

`IntegerScheme::decompress` has no offset or length parameter, and BP, PFOR, DICT and RLE are not
resumable. A plan with `S` sections therefore makes `S` full passes over the chunk, each writing a
scratch buffer and reading it back to accumulate.

Nimble's implementation avoids this by decoding in 4096-row chunks, so scratch and output stay
L1/L2-resident. That optimisation is simply not expressible against this interface.

The mitigation available today is a smaller `block_size`, which is why the benchmark sweeps it rather
than fixing it — the sweep separates "SubIntSplit is slow" from "these blocks are too large for
SubIntSplit". **A range-capable `decompress` overload is the fix**, and would benefit any scheme with
sub-streams, not just this one.

### 2. Sections cannot use narrow physical types

A section is handed to `IntegerSchemePicker`, whose input is `INTEGER`, so a 12-bit section costs four
bytes per value before its sub-scheme runs. Bit-packing recovers this, but a dictionary's entries and
an RLE stream's run values do not: they stay four bytes wide regardless of section width.

This also forces the planner's cost models to charge 32 bits per value for every section, which is
less accurate than it could be. A width-parameterised sub-stream interface would recover both.

### 3. Point access is no longer a structural dead end, but it is not free

This used to be titled "there is no point-access win" and said flatly that a point lookup cost a full
chunk decode for every scheme except `UNCOMPRESSED`/`ONE_VALUE`, and that SubIntSplit made it *worse*
by decoding every section on every lookup regardless of what each section's own scheme could do.
Measured then: 53 ms for 256 lookups against `PFOR`'s 8 ms.

That premise is gone. Most 32-bit schemes, and every 64-bit scheme through composition, now have a
real `gather`/`lookupAt` override instead of the decode-and-index default — see *Random-access cost by
scheme family* above for what each family actually costs. `SubIntSplitCore::gather` was rewritten to
delegate per-section to each section's own (now-improved) `gather`, rather than decoding the whole
value, so a SubIntSplit lookup now costs the *sum* of its sections' own lookup costs instead of a
full-value decode repeated per section.

The honest characterization now is a tradeoff, not a dead end: SubIntSplit's point access is
competitive with `BP`/`FBP` where its sections land on schemes with cheap random access (`BP`'s own
mini-block gather, fixed dictionaries, `FOR`), and it is still slower than a single-stream scheme
would be, because a lookup composes several sections' costs instead of paying one. See the *Speed*
section below for regenerated measurements against this design.

One thing did *not* change: `PFOR` (the `SIMDFastPFor`-backed scheme) still has no mini-block
`gather`. That was a deliberate scope decision, not an oversight — see the `PFOR` row in *Random-access
cost by scheme family* above for why its per-page patched-exception layout doesn't offer the same
cheap structure `FBP`'s fixed blocks do. A section that lands on `PFOR` still falls back to the
decode-and-index default, so SubIntSplit's point-access story depends in part on the picker
preferring `BP` (which it typically does for the ranges this scheme carves out — see *Where the bytes
actually go* below).

### 4. Planning is expensive

`O(value_bits² × sample_size)`, and it runs twice per column — `SchemePicker` calls
`expectedCompressionRatio` a second time on the hot path purely for logging. SubIntSplit is by a wide
margin the slowest encoder here.

### 5. Automatic selection is unreliable for this scheme

`expectedCompressionRatio` estimates from 640 values, drawn as ten contiguous runs of 64. That is far
too few to characterise a 64-bit snowflake's timestamp cardinality, and ten contiguous runs of a
monotone field look far more compressible than the column is. Force the scheme (`EnforceScheme` or
`override_scheme`) for anything load-bearing, and treat automatic-selection hit rate as a separate
result.

## Reproducing

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target subintsplit_bench    # explicit target: the playground
                                                     # tools pull in the AWS SDK
./build/subintsplit_bench --rows 1048576 --block-sizes 4096,8192,65536 --repeats 5 \
                          --input-i64 build/tweet_ids.i64 --csv results.csv \
                          --sections-csv sections.csv
```

`--sections-csv` is what produced *Where the bytes actually go*: one row per section of each
SubIntSplit plan, with its bit range, predicted and actual scheme, byte cost and share of the
encoding. Omit it and nothing else changes.

Three datasets are generated in memory from a seed — snowflake, uniformly random, and slowly
increasing. Uniform data is a control: it has no bit-range structure, so a split cannot help, and
anything other than parity there would mean the planner is fooling itself.

### The real dataset

`--input-i64` adds a 64-bit dataset read from a flat little-endian `int64` file. BtrBlocks has no
Parquet reader and should not grow a dependency on Arrow for one benchmark input, so the conversion
happens once, out of band:

```
# Note the interpreter. The project's own .venv has pandas but not pyarrow;
# the research harness's venv has it.
../EncodingsPlayground/Benchmarks/.venv/bin/python3 tools/subintsplit/parquet_to_i64.py \
    --parquet ../EncodingsPlayground/Datasets/TwitterSnowflake/tweet_ids.parquet \
    --out build/tweet_ids.i64 --limit 4000000
```

The full column is 30,761,504 rows — 246 MB as raw `int64` — so write it under `build/` (gitignored)
or outside the repo, and use `--limit` to keep it to what the benchmark will actually read. The
converted file is **not** committed.

A file shorter than `--rows` is used as-is rather than cycled; repeating a column would manufacture
periodicity that flatters every codec measured on it. A missing or unreadable file costs that one
dataset and the sweep continues.

Tests:

```
cmake --build build -j --target tester
./build/tester --gtest_filter='SubIntSplit*:RandomAccess*:Integer64Picker*'
```

The selection layer has no dependency on schemes or the wire format, so its tests
(`SubIntSplitSelector`) run standalone. `RandomAccess` (32-bit, `test/test-cases/RandomAccess.cpp`)
and `RandomAccess64` (64-bit, `test/test-cases/RandomAccess64.cpp`) are each parametrised over every
registered scheme of their width, which is what keeps the random-access API honest as a cross-codec
baseline. `Integer64Picker` checks that `Integer64SchemePicker` (via the real
Relation/Datablock/BtrReader pipeline) selects sensible schemes for representative `BIGINT` shapes
rather than always falling back to `UNCOMPRESSED`.

## Files

| Path | Contents |
|---|---|
| `btrblocks/scheme/integer/subintsplit/Sampler.hpp` | Block-stratified sampling |
| `btrblocks/scheme/integer/subintsplit/Metrics.hpp` | Per-range statistics |
| `btrblocks/scheme/integer/subintsplit/CostModels.hpp` | `ICostModel` and the per-scheme models |
| `btrblocks/scheme/integer/subintsplit/Selector.{hpp,cpp}` | The dynamic program |
| `btrblocks/scheme/integer/subintsplit/Plan.hpp` | Plan types, boundary strings, forced boundaries |
| `btrblocks/scheme/integer/subintsplit/SubIntSplitCore.hpp` | Wire format, encode, decode, gather |
| `btrblocks/scheme/integer/SubIntSplit.{hpp,cpp}` | The registered 32-bit scheme |
| `btrblocks/scheme/integer/SubIntSplit64.{hpp,cpp}` | The registered 64-bit scheme (`Integer64SchemeType::SUB_INT_SPLIT`) |
| `btrblocks/scheme/CompressionScheme64.hpp/.cpp` | `Integer64Scheme`, the base class every `*64` codec (including `SubIntSplit64`) implements |
| `btrblocks/scheme/integer64/` | The other registered `Integer64Scheme` codecs: `Uncompressed64`, `OneValue64`, `BP64`, `PFOR64`, `FOR64`, `RLE64`, `DynamicDictionary64`, `Dictionary8_64`/`16_64`, `Frequency64`, `Truncation64` |
| `btrblocks/scheme/SchemeType.hpp` | `IntegerSchemeType`/`Integer64SchemeType`, `default*Schemes()`, `restrictedIntegerSchemes()`/`restrictedInteger64Schemes()` |
| `tools/subintsplit/BenchCore.hpp` | Shared benchmark core (`run32`/`run64`, `Result`, CSV writers, timing) — used by both binaries below |
| `tools/subintsplit/subintsplit_bench.cpp` | Full incumbent codec pool |
| `tools/subintsplit/restricted_bench.cpp` | Five-family restricted codec pool (see *Restricted codec set* above) |
| `tools/subintsplit/SnowflakeGen.hpp` / `Traces.hpp` | Generated datasets; gather/point-access position traces |
| `tools/subintsplit/parquet_to_i64.py` | One-off Parquet → flat int64 conversion for the real dataset |
| `tools/subintsplit/run_benchmarks.sh` / `run_restricted_benchmarks.sh` | Run each sweep and render its tables |
| `tools/subintsplit/make_tables.py` | CSVs → the comparison tables in `benchmark-results/`/`benchmark-results-restricted/` |
| `test/test-cases/RandomAccess.cpp` / `RandomAccess64.cpp` | Cross-codec gather/lookupAt correctness, 32- and 64-bit |
| `docs/subintsplit-porting.md` | Deviations from the Nimble original |
| `docs/subintsplit-results.csv` | Raw output behind the tables above |
| `docs/subintsplit-sections.csv` | Per-section breakdown of each plan |
