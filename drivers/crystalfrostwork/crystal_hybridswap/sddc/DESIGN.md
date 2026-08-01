# Crystal SDDC Design

## 1. Scope

Crystal SDDC is the resident similarity-deduplication and delta-compression
layer in Crystal Hybridswap's private zram driver. It applies Huawei's LZ4KD
delta codec to the byte stream that zram would normally store, while keeping
the normal zram read, free, recompress, memcg, reset, and ZMS writeback paths
coherent.

The implementation is deliberately per-zram-device. It does not register
LZ4KD in the global Crypto API, share references between devices, or persist
an SDDC reference graph on the backing device. SDDC is enabled only when all
of the following are true:

- `CONFIG_CRYSTAL_HYBRIDSWAP_SDDC` is enabled;
- the kernel uses 4 KiB pages;
- the primary zram compressor implements the zcomp delta operations; and
- the zram device has been initialized with a non-zero `disksize`.

The private LZ4KD backend and its 8 KiB reference/current window are selected
with `CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD`. If the runtime preconditions are
not met, zram continues to use its ordinary representation and the SDDC
manager is not created.

## 2. Stored Representations

Each zram slot has one SDDC kind in addition to the normal zram table entry:

| Kind | zram handle and size | SDDC reference | Meaning |
|---|---|---|---|
| `NONE` | Ordinary zsmalloc handle and exact stream size | None | A normal zram object that can be observed as a candidate. |
| `REF` | Handle `0`, size `0` | Required | The source slot for one immutable reference object. |
| `ALIAS` | Handle `0`, size `0` | Required | An exact duplicate whose bytes come entirely from the reference. |
| `DELTA` | zsmalloc handle and delta-wire size | Required | A target represented by a delta against the immutable reference. |

An "ordinary stream" means the representation zram would have stored without
SDDC. It is either a compressed stream shorter than `PAGE_SIZE`, or the raw
4 KiB page when ordinary compression did not reduce its size. SDDC compares
and delta-compresses these ordinary streams, not decompressed page contents.

Every logical slot has an 8-byte `mutation_seq`, advanced by the ordinary
store/free lifecycle. The sequence remains dense because ordinary slots need
the same ABA protection as managed slots. All other SDDC state is sparse: an
XArray node exists only while the slot is `REF`, `ALIAS`, or `DELTA`. That node
contains the reference cookie, `accounted_size`, `saved_size`, and kind. A
normal `NONE` slot therefore consumes no managed-state node. In-place SDDC
transitions retain the mutation sequence and are additionally identified by
their kind and reference cookie.

State-node allocation and XArray reservation happen before a slot lock is
taken. Publication consumes that reservation without sleeping and precedes
the representation change. Allocation or reservation failure abandons only
the opportunistic conversion; it never fails the ordinary zram store. If the
manager's fixed metadata cannot be allocated at device initialization, zram
continues with ordinary LZ4KD compression and no SDDC manager.

The reservation owner also retains the target/source mutation sequence for its
abort path. A concurrent free/store leaves the reservation in place and bumps
that sequence, so abort releases the reservation and schedules one latest-state
observation afterward. An ordinary no-gain, same-class, or resource miss keeps
the sequence unchanged and is not requeued; this prevents a permanent retry
loop on data that cannot benefit from SDDC.

### 2.1 Immutable references

A reference object owns the original zsmalloc handle moved out of a source
slot. It records the stream size, compressor priority, source memcg ID, and a
cookie `{id, generation}`. References are stored in a per-device XArray and
have an explicit refcount.

The IDA can reuse an ID after a reference is released. The generation value
makes the cookie unique across such reuse, so a stale slot or delta header
cannot pin a different reference that later acquired the same ID. Reference
bytes are immutable after publication, allowing readers to pin and copy them
without retaining the source slot lock.

The first promotion initializes two owners: one for the source `REF` slot and
one temporary owner held by the conversion. A successful target commit
transfers the temporary owner to the new `ALIAS` or `DELTA` slot. A failed
commit drops it. When an existing `REF` is selected, the candidate snapshot
pins an equivalent temporary owner and follows the same transfer rule.

The last put removes the object from the XArray before queueing physical
zsmalloc release on the high-priority free workqueue. No teardown path forces
the release of a still-owned reference.

The ownership graph is always one level deep. Only an ordinary `NONE` source
or an existing `REF` source can back a new target; `ALIAS` and `DELTA` slots
are never promoted or indexed as references. There are consequently no
reference chains, delta-on-delta decode, or cycles. The immutable reference
can outlive its original `REF` slot while aliases or deltas still own it.

## 3. Delta Wire Format

Only a resident `DELTA` slot stores the SDDC wire format. All integer fields
are little-endian:

| Offset | Size | Field | Validation |
|---:|---:|---|---|
| 0 | 4 | `magic` | `0x43444453` (`SDDC` in memory on little-endian systems) |
| 4 | 2 | `version` | `1` |
| 6 | 2 | `header_size` | Current header size, 24 bytes |
| 8 | 4 | `ref_id` | Must match the pinned reference cookie |
| 12 | 4 | `ref_generation` | Must match the pinned reference cookie |
| 16 | 4 | `ref_size` | Must equal the immutable reference stream size |
| 20 | 4 | `target_size` | Restored ordinary stream size, in `1..PAGE_SIZE` |
| 24 | variable | LZ4KD delta payload | Occupies the remainder and must pass codec validation |

The complete wire object must be larger than the header and no larger than a
page. Decode rejects an unknown magic or version, a changed header size, a
cookie or reference-size mismatch, an invalid target size, a codec error, or
a restored length different from `target_size`.

This format is an internal resident representation, not an on-disk or
user-space ABI. Before ZMS writeback, the slot is flattened back to its
ordinary compressed stream or raw page. The reference cookie and delta header
are therefore never required to survive reset or backing-device persistence.

## 4. Candidate Discovery

An ordinary store captures a job key under the slot lock. A valid key contains
the slot index, mutation sequence, zsmalloc handle, exact object size, and
compressor priority. The object must:

- be larger than 256 bytes and no larger than `PAGE_SIZE`;
- use the primary compressor;
- use a compressor with delta support; and
- not be `SAME`, `WB`, `UNDER_WB`, or already SDDC-managed.

The key is used as an admission hint, then only its slot index is enqueued. A
device owns one pending bit per slot in an `nr_slots`-sized bitmap and two
embedded, reclaim-capable observation work items. The workqueue itself is
ordered, so only one callback drains the bitmap at a time; the second item
closes the hand-off window while a callback is returning. Under `state_lock`,
the producer sets the bit and increments `pending` only when it was previously
clear. Repeated stores for the same slot therefore increment `coalesced`
instead of allocating another work item; the queue has latest-state semantics,
not a backlog of stale keys. `queued` counts these unique bit insertions.

The producer that successfully queues a work item transfers one zram reference
and one SDDC `active_ops` token to that callback. If both embedded items are
already queued or running, the bit remains set and one of those callbacks owns
the drain; a failed `queue_work()` attempt does not clear it. The worker drains
bits in round-robin order, clears a bit and decrements `pending` under
`state_lock`, then reacquires the slot lock to read the current handle, size,
flags, and mutation sequence. A store that raced with enqueue is consequently
observed using its newest representation. Each callback has a bounded slot
budget and hands remaining bits to the alternate embedded work item, so a
continuously rewritten device cannot keep freezer or reset waiting indefinitely.
There is no fixed 1024-entry allocation; the observation buffers come from the
single workspace allocated with the manager rather than from each store event.
Both embedded items run on the same ordered workqueue, so they cannot use that
workspace concurrently. Pending work is discarded only once stop has closed
admission.

The worker holds `init_lock` for read while it validates the latest job key and
copies the target stream under the slot lock. Hashing, codec calls, and
allocations happen after releasing the slot lock. Three hashes are calculated:

- an exact hash over the complete ordinary stream;
- a hash over the first 16 bytes; and
- a hash over the last 16 bytes.

If no conversion commits, the worker validates the same job key once more
under the slot lock before publishing any exact or sample index cell. A store
or free that raced with hashing therefore cannot leave a candidate cell for a
different object. The mutation sequence and complete identity remain the
authoritative check at every later candidate lookup.

There are 65536 exact buckets and 65536 sample buckets, each with eight
ways. Observation admission starts above 256 bytes. Such streams, including
raw `PAGE_SIZE` streams, enter both the exact index and the head/tail sample
buckets; shorter streams are kept out of the asynchronous index path.
An index cell stores a slot index only; the dense per-slot mutation sequence is
checked separately and the cell does not own the slot. Cells are
not synchronously removed when a slot changes. Every later use revalidates the
mutation sequence and complete slot identity, making stale cells harmless.
The replacement score follows Huawei's high-yield heuristic: lower-reference
and smaller objects are evicted first, while a highly shared reference survives
an equal-size tie. Replacement scoring only tries a candidate slot lock while
holding the index lock; a busy cell is protected for that insertion instead of
reversing the publication lock order. Source promotion intentionally retains
its mutation sequence so an existing candidate cell can resolve the new `REF`;
the kind and cookie then identify that in-place representation transition.

Candidate lookup first returns up to eight exact candidates. If none commits,
it combines and de-duplicates up to sixteen head/tail sample candidates. A slot
present in both buckets retains both sample bits and is ranked by its better
head/tail score. Tail scoring anchors before the terminal 16-byte sample, so a
score never exceeds the shorter input. The candidate arrays are bounded stack
storage, so duplicate head/tail index cells cannot multiply codec work for one
target. Ranking maps each validated candidate directly while its slot or pinned
reference protects the handle; only a candidate selected for a conversion
attempt is copied into the observation workspace.

## 5. Conversion Transaction

Conversion is split into snapshot, compute, source promotion, and target
commit stages. Source and target slot locks are never held together.

### 5.1 Exact alias

For an exact candidate, SDDC first compares the directly mapped stream, then
snapshots and copies candidates in ranked order before the transactional check.
A hash match alone is never sufficient. The source is promoted to or pinned as
an immutable reference, and the target commits as `ALIAS` with no zsmalloc
handle.

### 5.2 Delta

For a sample candidate, source and target must both be raw-page streams or
both be compressed streams. LZ4KD receives the source ordinary stream as its
reference and the target ordinary stream as its current data.

A delta is eligible only when all of these checks pass:

- the target is larger than `PAGE_SIZE / 8` and can accommodate the 24-byte
  header plus at least an 8-byte logical gain;
- the codec output fits the resulting limit;
- the complete wire object maps to a strictly smaller zsmalloc size class
  than the old target object;
- zsmalloc allocation succeeds; and
- the device memory limit remains satisfied after allocation.

The size-class check is important: a smaller byte count that occupies the same
zsmalloc class is not a physical allocator improvement and is rejected.

### 5.3 Commit rules

Reference allocation and delta encoding happen outside slot locks. Source
promotion reacquires the source slot lock and verifies mutation sequence,
kind, handle, size, compressor priority, and conflicting zram flags before it
moves ownership of the existing zsmalloc handle into the reference object.

Target commit independently reacquires the target slot lock and validates the
entire original job key. Only then does it remove the old accounting, free the
old target handle, install the alias or delta representation, transfer the
reference owner, and apply the new accounting.

If target validation fails, the stale result is discarded. A source already
promoted during the attempt may remain a valid `REF`; promotion itself does
not allocate duplicate payload bytes or lose data. The unchanged target stays
ordinary and can be observed again later.

If neither alias nor delta conversion succeeds, the worker indexes the target
as a future candidate. All conversion paths are opportunistic: allocation,
codec, benefit, limit, or stale-snapshot failures must never make an ordinary
store fail.

## 6. Read and Writeback Paths

### 6.1 Resident reads

A managed read obtains a manager token and maps its destination page. Under the
slot lock it validates the current kind, pins the reference, and copies any
delta wire bytes into that page as temporary staging. It then releases the slot
lock and restores from the immutable reference:

- `REF` and `ALIAS` copy the reference stream directly;
- `DELTA` validates its header and invokes LZ4KD delta decode.

Delta restore first writes the ordinary stream into the existing per-CPU zcomp
buffer, then either copies a raw `PAGE_SIZE` stream or decompresses a shorter
stream over the staging bytes. `REF` and `ALIAS` map the pinned reference
directly into the final copy/decompression. A pinned reference and copied wire
make concurrent slot replacement safe after capture.

`-EAGAIN` denotes a transient identity or lifecycle conflict and lets the zram
read path retry. Corrupt metadata, a missing reference, or codec failure
returns an I/O error and increments decode diagnostics.

### 6.2 ZMS writeback flattening

Writeback snapshots both normal zram identity and SDDC identity
`{mutation_seq, kind, ref cookie}` while claiming `ZRAM_UNDER_WB`. A managed
slot is flattened outside the slot lock to its ordinary stream; writeback does
not decompress that stream to the original page.

Before replacing the resident slot with a ZMS handle, writeback rechecks the
normal handle, size, flags, memcg ID, and the complete SDDC snapshot. A
mismatch discards the stale ZMS object and leaves the current slot intact. On
success, normal slot free drops the SDDC owner and the ZMS entry records the
flattened ordinary size and compressor priority. A `PAGE_SIZE` flattened
stream restores the normal `ZRAM_HUGE` state.

Writeback allocation, store, flatten, and snapshot failures clear
`ZRAM_UNDER_WB` and ask the still-resident slot to be observed again when it
is eligible. No resident reference graph is written to ZMS.

### 6.3 Batch-in, prefetch, rewrite, and recompress

ZMS batch-in and prefetch decode the flattened ordinary stream to a page and
then use the normal zram compressor to create a new resident object. The
restored slot receives a new mutation identity and observation job. Readback
failure or snapshot mismatch clears `ZRAM_UNDER_WB` and re-observes any
eligible resident object.

A normal write first frees the old SDDC representation and its reference
owner, then commits an ordinary object and queues a new observation. Ordinary
recompression skips managed slots. When a recompressed ordinary object is
committed, it receives a new SDDC mutation and can be observed again.

## 7. Locking and Lifetime

The teardown protocol is ordered as follows:

```text
sddc_lifecycle_lock
  -> close admission under sddc_lock (set stopping)
  -> clear pending bitmap under state_lock and account shutdown discards
  -> flush observation work items and wait for active_ops == 0
  -> acquire init_lock for write
  -> free slots and their reference owners
  -> detach the quiesced manager under sddc_lock
  -> destroy workqueues, pools, indexes, XArray, and IDA
```

`disksize` initialization also holds `sddc_lifecycle_lock` and `init_lock` for
write while publishing a new manager. Publication and final detachment are
allowed to take `sddc_lock` inside `init_lock` only because no old manager can
admit active operations at those points. Normal teardown never waits for work
while holding `init_lock` or `sddc_lock`.

The individual locks have narrow ownership:

| Protection | State protected | Ordering rule |
|---|---|---|
| `sddc_lifecycle_lock` | Create, quiesce, reset, and destroy serialization | Outermost lifecycle serialization. |
| `sddc_lock` | Manager pointer, admission, and `stopping` transition | Held only briefly; never held while flushing or waiting. |
| `init_lock` | zram table, compressors, and initialized-device lifetime | Observation work holds read; reset takes write only after admission drains. |
| slot bit lock | zram entry and matching per-slot SDDC record | Required for identity capture, validation, representation commit, and free. |
| slot-state XArray lock | Sparse managed-state publication and removal | Taken briefly below the matching slot lock; never used to acquire a slot lock. |
| reference XArray lock | Cookie lookup, publication, pin, erase, and refcount-to-zero | May be taken below a slot lock; no path takes a slot lock while holding it. |
| `memcg_stats_lock` | Per-memcg cached accounting | May be taken below a slot lock during representation accounting. |
| `index_lock` | Exact/sample bucket cells | Candidate lookup releases it before blocking on a slot; final index publication may take it below the validated target slot lock. Replacement scoring uses only a non-blocking slot trylock. |
| `state_lock` | Pending bitmap, round-robin cursor, worker admission, and queue counters | Never held across slot or index processing. |
| `active_lock` | `active_ops` decrement/wakeup and teardown idle confirmation | The final put and the idle check are serialized so teardown cannot free the manager while a put is still waking its waitqueue. |

Every operation that can retain a manager pointer beyond the caller's
`init_lock`/slot critical section obtains an `active_ops` token through
`sddc_lock`. The short slot-locked integration helpers instead rely on that
caller-owned device lifetime protection. Setting `stopping` prevents new
tokens. Observation submission keeps separate producer and worker tokens so
completion of one cannot expose manager or statistics memory to use-after-free
by the other. The final `active_ops` put performs its wakeup while holding
`active_lock`; teardown takes that lock after the wait condition becomes true,
which closes the last-put versus manager-free window.

Slot `mutation_seq`, full job-key validation, writeback SDDC snapshots, and
reference generations address different ABA domains and must all remain in
place:

- mutation sequences invalidate reused slot indexes in candidate jobs;
- handle, size, priority, kind, and flags reject partial identity matches;
- reference generations invalidate reused XArray/IDA IDs; and
- writeback snapshots prevent a flattened old representation from replacing
  a newly mutated slot.

## 8. Accounting

SDDC preserves the distinction between logical pages and physical resident
payload bytes:

- `pages_stored` still counts every allocated logical zram slot, including
  `REF`, `ALIAS`, and `DELTA` slots;
- zram `compr_data_size` counts each immutable reference stream once and each
  resident delta wire once; aliases add no payload bytes;
- promoting an ordinary source moves ownership of its existing zsmalloc
  object and therefore does not itself increase physical payload bytes;
- `ref_bytes` and `delta_bytes` count exact object byte lengths, not zsmalloc
  class size, allocator metadata, fragmentation, indexes, or workspaces; and
- allocator page usage and `mem_limit` continue to come from zsmalloc itself.

For memcg reporting, the immutable reference bytes remain charged once to the
memcg that owned the promoted source. An alias contributes zero resident
payload bytes to its target memcg. A delta contributes its wire size to the
target slot's memcg. This avoids charging one shared reference once per owner,
but means cross-memcg aliases do not move the reference charge away from its
origin.

`saved_bytes` is the current sum of `old_target_size - new_target_size` for
live aliases and deltas. An alias has `new_target_size == 0`; a delta uses its
complete wire size. `saved_bytes_total` accumulates the same value at each
successful target commit and is not decremented when a slot is freed. Source
promotion has zero saved size because it only changes ownership of existing
bytes. Neither value estimates allocator fragmentation or includes SDDC
metadata overhead.

## 9. Failure Recovery

The data path follows these recovery rules:

- requests that cannot acquire manager admission or a zram lifetime reference
  are dropped while their slots remain ordinary; those early failures cannot
  be attributed to a manager that may already be gone. A request whose
  pending bit is already set is coalesced instead of dropped;
- a consumed bit whose latest slot is empty, raw-ineligible, writeback-owned,
  or already managed is counted as ineligible and leaves the slot ordinary;
- a stale job, source candidate, or target commit is rejected by identity
  validation;
- codec no-gain results and same-class results are normal misses, not data
  errors;
- delta allocation or memory-limit rejection frees all temporary objects;
- target commit failure drops the temporary reference owner and any new delta
  handle;
- slot free clears its kind and cookie exactly once, updates current counters,
  and drops exactly one reference owner;
- resident decode corruption returns an error rather than guessing or using a
  mismatched reference;
- non-stale flatten failures leave the resident slot intact, clear writeback
  ownership in the caller, and are visible in diagnostics; and
- reset first closes admission, clears unconsumed bitmap bits (accounting them
  as shutdown discards), and drains the observation workers and active
  operations;
  it then frees every slot so reference teardown follows normal ownership
  rules. A worker-owned bit is allowed to finish before teardown.

One fixed three-page workspace serves observation on the ordered worker.
Resident reads stage wire bytes in the destination page, while flatten stages
them in its caller-provided page and uses the existing per-CPU zcomp buffer for
delta restore. Reference payload release uses a dedicated reclaim-capable,
unbound, high-priority workqueue so zsmalloc release is not performed inside a
slot lock.

## 10. Diagnostics

`/sys/block/zram<id>/sddc_stat` is read-only and emits one decimal
`name: value` pair per line. The fields are:

| Field | Unit | Meaning |
|---|---|---|
| `enabled` | boolean | A running SDDC manager admitted the snapshot. |
| `queued` | count | Observation requests that set a previously clear per-slot pending bit. |
| `coalesced` | count | Repeated requests merged into an already-set slot bit. |
| `dropped` | count | Requests rejected after an active manager was found (for example, an invalid slot index or a stop race). Requests rejected before a manager can own the statistic are not attributed here. |
| `ineligible` | count | Bits consumed after the latest slot state was no longer a valid observation target. |
| `shutdown_discarded` | count | Pending bits cleared during stop/reset without being consumed. |
| `worker_runs` | count | Executions of either embedded observation work item. |
| `pending` | count | Set bitmap bits not yet taken by the worker. |
| `pending_max` | count | Maximum observed number of set pending bits. |
| `observed` | count | Jobs that completed candidate processing. |
| `stale` | count | Target observations whose identity changed before final index publication. |
| `indexed` | count | Observations inserted as future candidates after no conversion committed. |
| `refs` | count | Current immutable reference objects. |
| `ref_bytes` | bytes | Current immutable reference payload bytes. |
| `aliases` | count | Current `ALIAS` slots. |
| `deltas` | count | Current `DELTA` slots. |
| `delta_bytes` | bytes | Current complete delta-wire bytes. |
| `alias_attempts` | count | Exact-index candidates examined; stale candidates can be rejected before byte comparison. |
| `alias_hits` | count | Exact candidates successfully committed as aliases. |
| `delta_attempts` | count | Head/tail sample candidates examined for delta conversion. |
| `delta_hits` | count | Delta candidates successfully committed. |
| `delta_matches` | count | Ranked candidates that passed the 16-byte similarity floor. |
| `delta_small_rejects` | count | Targets rejected from sample/delta processing because they were at or below the `PAGE_SIZE / 8` delta admission floor. |
| `delta_no_gain` | count | Candidates whose codec output or zsmalloc size class did not produce a physical gain. |
| `delta_match_bytes_max` | bytes | Largest byte-match score seen by delta candidate ranking, bounded by the shorter ordinary stream. |
| `saved_bytes` | bytes | Current logical resident payload bytes avoided by live targets. |
| `saved_bytes_total` | bytes | Cumulative bytes avoided at successful target commits. |
| `conversion_failures` | count | Resource, promotion, allocation, limit, or stale-commit failures after a conversion path had actionable output. |
| `decode_failures` | count | Resident read or flatten restoration failures. |
| `flatten_failures` | count | Non-`EAGAIN` failures while preparing an ordinary stream for writeback. |
| `limit_rejects` | count | Delta allocations rejected by zram's memory limit. |

Attempts, hits, cumulative totals, and failures reset when the zram device is
reset and its manager is destroyed. Current gauges fall as slots and
references are freed. If SDDC is not built, cannot run for the selected
compressor, or is quiescing, the node reports `enabled: 0` and zero values.
The queue counters are diagnostic rather than a success ratio: a high
`coalesced` count is expected when a hot slot is rewritten repeatedly, while
`ineligible` and `shutdown_discarded` identify work that did not reach
candidate discovery.

Codec and state-machine KUnit coverage is selected by
`CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_KUNIT_TEST`. Tests should preserve the wire
validation, cookie generation, slot mutation, owner transfer, admission, and
writeback snapshot invariants described above.
