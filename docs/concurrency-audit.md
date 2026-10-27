# Concurrency audit and first epoll isolation step

## Threads and ownership

`net_server_run()` runs the one network owner on its calling thread. It owns the
listener, accepted sockets, `epoll_wait`, all `epoll_ctl` calls, input buffers,
frame decoding, socket sends/receives, and physical connection removal. The
tick thread owns protocol handlers, player and inventory state, item entities,
live world mutations, and remote player views. It runs at 20 TPS and also
performs an optional fast task pass between scheduled ticks. The world creates
one to four persistent workers for chunk load/generation. Those workers own
their unfinished chunks until they enqueue a result; the tick integrates the
result into the live chunk map. World jobs and completions use separate queues.

The connection list and item entity list use `conns_lock`. A connection's
outbound byte buffer and fd transition use `out_lock`. The inbound task queue
uses its own mutex and condition variable and is bounded at 8192 tasks. The
world job and completion queues use `job_lock` and `done_lock`. Connection
state, close flags, detached state, and reference count are atomic. The live
world/chunk map and player objects are tick owned; world workers do not mutate
them. Outbound buffers are bounded at 32 MiB per connection.

Lock order for this step: code may take `conns_lock` and then `out_lock`, but
never the reverse. Network I/O is performed without `conns_lock`. The network
owner may lock `out_lock` while sending and changing an epoll registration.
The tick only appends framed bytes under `out_lock`; it does not touch sockets
or epoll. A successful empty-to-nonempty transition wakes the network owner
through `eventfd`. `EPOLLOUT` remains armed while bytes are pending, including
after a partial write or `EAGAIN`. Each flush sends at most 256 KiB to keep the
event loop responsive to other clients.

## Findings and changes

Before this step, both tick and network paths called `handle_client_write()`.
`server_close_conn()` held `out_lock` while taking `conns_lock`, opposite to
other paths, and could free a connection while another event in the same epoll
batch still held its pointer. It also changed remote player views from the
network thread. The tick no longer flushes socket bytes. Close now releases
`out_lock` before removing the connection from the list, retains the object
through the epoll batch, and passes gameplay departure work to the tick. The
last reference cannot destroy a player object before tick cleanup has run.

The task queue still preserves network arrival order for a connection because
one network owner appends decoded packets to one FIFO. If it reaches its bound,
the connection is closed rather than silently dropping a packet. Output
buffer saturation likewise closes the connection and wakes the network owner.

## Remaining risks and next steps

The input task queue is global; a large burst can make one tick spend too long
draining it. Chunk serialization and bounded chunk saves still run on the tick
thread. Player cleanup can include synchronous persistence. The existing
OpenSSL path has not been exercised by this audit. The generated-data check
currently falls back to existence checks because a local raw JSON input is
empty. Multiple network workers need explicit connection dispatch, per-worker
queues/counters, and client E2E validation before enabling them. Async chunk
saves and parallel simulation are deferred until this single-owner path is
stable under full client tests and sanitizers.

Targeted network regression: `python3 tests/test_network_epoll.py 127.0.0.1
<port>` against a running server. It exercises repeated concurrent status
requests, ordered ping/pong responses, early disconnects, and incomplete
frames. `make test_framing_compression` checks output wakeup coalescing and
buffer saturation.

Validation on this change: the normal precommit gate passed, including all
14 C tests and the server smoke start. The network regression passed with 500
clients on the normal build, 200 under AddressSanitizer/UndefinedBehaviorSanitizer,
and 100 under ThreadSanitizer. Four simultaneous logins also passed under
ThreadSanitizer. The sanitizer logs reported no remaining issue in these runs.
Node.js is unavailable in this environment, so the Mineflayer scenarios were
not run. One `MC_PERF` sample around a login reported 20 ticks averaging
1.372 ms with a 19.665 ms maximum; this is a local sample, not a throughput
benchmark or a before/after comparison.

## Remote flat-world stress follow-up (2026-10-19)

The disposable profile world on port 25588 was tested with 1, 5, 10, 25, and
50 Python protocol bots in connect, movement, and place/break scenarios. Each
run records connection-to-spawn and connection-to-origin-chunk time, chunk and
movement packet counts, position corrections, and unexpected disconnects in
`tests/stress/results/*/report.json`. The fixed comparison used two copies of
the same seed-0 flat world, fresh player data, 50 moving bots, 12 requested
seconds, and identical 50 ms spawn spacing. The old code delivered 517 chunks
and reached the origin chunk in 4.186 s on average; the optimized code
delivered 23,283 chunks and reached it in 0.049 s on average. These chunk
counts cover the whole run including shutdown, so they are not per-second rates.
Neither comparison had an unexpected disconnect. The block test on a separate
world placed and broke 171 blocks with 50 bots, again without a disconnect.

The main bottleneck was CPU work on the tick thread, not the 256 KiB epoll
write-pass cap. `send_chunk_ready` scanned every block of every section for
block entities, including flat chunks with no block-entity palette states;
one measured chunk spent 8.32 ms in that scan. The post-stream refresh did
the same scan for every previously sent chunk. Both now skip sections whose
palette cannot contain a block entity. Repeated serialization of unchanged
chunks and their heightmaps was the second bottleneck (3–9 ms per chunk in
samples); revision-keyed caches now serve subsequent viewers. A block change
invalidates both caches, while a no-op write preserves them.

The `perf summary` log reports 20-tick windows with 0.1 ms histogram P50,
P95, P99, maximum, and number of ticks above 50 ms. Its histogram clips at
1000 ms: a P95/P99 shown as 1000.0 ms means *at least* that value. Even after
the fixes, the 50-bot join/movement run had a 111.455 ms tick (tasks 75.829 ms,
protocol 30.125 ms) and 49 of 200 active-window ticks exceeded 50 ms. The
remaining global task drain and all-to-all remote player synchronization
(`remote_syncs=2450` in one 50-player tick) therefore remain scalability
risks. This test demonstrates a large chunk/spawn improvement, not a guarantee
of steady 20 TPS at 50 simultaneous players or visually verified movement.

## CPU setup and chunk-worker validation (2026-10-19)

`MC_SETUP_FILE` selects a per-instance `[performance]` configuration. The
process validates the selected Linux CPU mask and applies it before starting
the network, tick, or chunk threads; those threads inherit the mask. The
effective CPU count is a logical-CPU affinity, not a CPU-time quota or
exclusive reservation. The network remains one epoll owner, the tick remains
one thread at 20 TPS, and chunk load/generation uses the configured worker
count. Save workers and parallel simulation are not implemented; their keys
are rejected rather than silently accepted. A default `setup.conf` is
optional for older launch paths, while an explicit missing `MC_SETUP_FILE`
fails startup.

On the remote host, a disposable profile instance on port 25588 used the
same flat-world snapshot in sequential runs with 1, 2, 4, and 8 allowed
logical CPUs. Automatic chunk workers resolved to 1, 1, 2, and 4,
respectively. Each configuration completed 1-, 10-, 25-, and 50-bot connect
runs lasting eight requested seconds: all 16 runs had zero connection
failures and unexpected disconnects. With 50 bots, the final 200-tick
`MC_PERF` windows reported TPS 19.53, 19.98, 19.98, and 19.98; P95 tick
duration 29.5, 13.1, 13.0, and 12.6 ms; and 1, 0, 0, and 0 ticks over
50 ms, in CPU-count order. These are short, sequential samples with warm
world caches and external bot load, not a controlled scaling benchmark.

Two simultaneous local instances on separate worlds and ports 25590/25591
were inspected per Linux thread: all threads inherited CPU masks 0-3 and
6-7, with two and one chunk workers, respectively. Ten bots connected to
each concurrently without failures. A separate remote 4-CPU run sent 766
movement packets with ten bots and no disconnect or action error; without
QA instrumentation this does not prove visual movement. Ten block bots
verified 63 placements and 63 breaks with no action error. Stopping that
profile service under a 25-bot mixed load completed cleanly; the bot harness
correctly reported 25 expected disconnects because the server was stopped.
The new setup and world consistency tests passed under ASan/UBSan and TSan.

At this stage, network-worker sharding was deferred until the ownership
changes and E2E tests described below. A simulation pool still needs region
ownership and ordered cross-region commits; merely starting threads would
race the tick-owned world and player state.

## Multi-epoll socket ownership (2026-10-19)

The previous network design had one thread that accepted clients, performed
all `epoll_wait`/`epoll_ctl`, and sent/received every socket. The tick thread
already owned gameplay and world mutation. The new design retains that tick
ownership but splits networking into an acceptor and 1, 2, or 4 independent
epoll workers. The acceptor assigns each accepted fd round-robin and hands it
off through that worker's mutex-protected, globally bounded command queue and
eventfd. Only the assigned worker adds/modifies/deletes that fd in its epoll,
calls `recv`/`send`, and closes the socket. A connection never migrates.
Outbound packet construction still occurs on the tick thread under each
connection's `out_lock`; the empty-to-nonempty transition wakes its owner.
Each owner keeps its own live list and retired list so pointers from a closed
connection remain valid until the entire `epoll_wait` batch is processed.
The global connection list remains for tick-side snapshots, but no global
mutex is held during socket I/O. Entity IDs are now allocated atomically.
The existing 8192-task and 32 MiB/connection output limits remain in force;
the accept handoff queue cannot exceed the configured connection cap.

Shutdown signals are blocked before world/thread creation and consumed by
the acceptor through `signalfd`. It stops accepting, wakes and joins every
network worker, lets the tick drain departure cleanup, then joins the tick.
The 20 TPS tick is not parallelized. All workers inherit the process CPU
affinity, without pinning a worker to a dedicated CPU.

Integration tests used 81 short TCP clients per configuration, with status,
ping/pong, early disconnect, incomplete frames, and rapid reconnection. The
observed accept counts were 81 (one worker), 41/40 (two), and 21/20/20/20
(four); every connection was detached at shutdown. Four workers also passed
with only one logical CPU allowed. A separate 100-socket incomplete-frame
shutdown test verified all sockets closed. The bounded task queue has a new
unit test. The integration test passed under ASan/UBSan and TSan on this
host; sanitizer coverage does not prove absence of all races.

A reproducible local comparison is in
`tests/stress/benchmark_network.py`. It used fresh seed-0 flat worlds,
the same chunk-worker count (2), and 50 connect bots for 12 requested
seconds, sequentially on the same 8-CPU laptop. Each run connected all
50 bots without an unexpected disconnect and received 22,050 chunks.
The most active 100-tick `MC_PERF` window in each run was:

| Network workers | TPS | P95/P99 MSPT | Ticks >50 ms | Mean first chunk |
| --- | ---: | ---: | ---: | ---: |
| 1 | 18.41 | 66.5/88.0 ms | 7 | 0.095 s |
| 2 | 18.14 | 70.6/82.5 ms | 7 | 0.112 s |
| 4 | 18.38 | 58.0/83.2 ms | 6 | 0.119 s |

The network workers consumed roughly 1.28 GB of outgoing data per run,
about 105 MB/s over the bot session, spread across 1/2/4 owners; their
per-worker packet, byte, task-wait, epoll-wait, and CPU counters are in the
benchmark logs. These short, fresh-world runs show no demonstrated TPS or
first-chunk improvement from extra network workers. The tick's chunk and
player processing remains the likely bottleneck; isolating network work may
matter at other traffic patterns, but that has not been established here.

Additional local gameplay checks passed without unexpected disconnects:
25 moving bots on both 2 and 4 workers sent 1,695 movement packets each;
10 block bots verified 57 placements and 57 breaks on both 2 and 4 workers.
The one-worker regression sent 563 movement packets with 10 bots and
verified 43 placements and 43 breaks in a separate run. The four-worker
configuration also connected all 100 bots over a 15-second run. Movement
packet counts are not client-side visual confirmation.

Remaining risks: player-state reads outside the tick thread and some
tick-side broadcasts under `conns_lock` predate this change and need a
separate ownership audit. The shared inbound task queue can still make a
tick overrun under burst load. Before parallel simulation, partition world
state into regions, give each region one writer, define ordered cross-region
messages/commits, and test deterministic results and persistence. Starting
simulation threads without those rules would be unsafe.

## Tick-path audit and chunk-refresh optimization (2026-10-22)

The network architecture remains acceptor + 1/2/4 epoll owners + one
authoritative 20 TPS tick + chunk workers. No save or simulation worker has
been enabled. The tick drains the shared FIFO both at fixed ticks and in fast
packet passes between them; chunk integration, bounded periodic persistence,
furnaces, player protocol ticks, item entities and broadcasts remain serial.
`MC_PERF` now separates handler/fast-pass time, per-PLAY-packet cost, periodic
chunk saves, explicit `mc_world_flush_block` calls, world integration, tick
CPU and selected mutex waits. The queue records peak depth and producer/
consumer lock time only while profiling is enabled. The previous lazy
initialization of `MC_PERF` options raced when chunk and tick threads called
it concurrently; it now uses `pthread_once`. UBSan on a block workload also
found undefined behavior in the tick's keep-set key for negative chunk X
coordinates (`int64_t` left shift). It now packs two unsigned 32-bit
coordinates, matching the world chunk-map key.

On a fresh flat world with 50 `fastblocks` bots, the pre-change instrumented
run had a fully populated 100-tick window at TPS 19.55, P95/P99 54.9/62.7 ms
and 7 ticks >50 ms. Its queue peaked at 142/8192, rejected no task, and
recorded only 0.86 ms total producer mutex wait over 15,898 packets. The
handler total includes fast passes, so its 36.6 ms/tick-window average must
*not* be added to the scheduled tick's 14.6 ms average. Periodic saves took
0.52 ms/tick on average; explicit block flushes another 3.9 ms/tick; item
entities 3.9 ms/tick. During the busy window, 3,001
`SERVERBOUND_PONG` handlers consumed 2,121 ms cumulatively. A PONG rescanned
the player's entire sent-chunk set to refresh container block entities,
although most flat chunks contain none. This—not the task-queue mutex—was a
measured critical-path cost. Block placement and break handlers, item physics,
and outbound buffering remain substantial costs.

`send_chunk_ready` now sends the post-stream refresh ping only if the chunk
actually serialized one or more block entities. Chunks without containers
still send their full chunk data, and ordinary block changes still use the
existing update path. The ping/PONG refresh path remains for container chunks;
it has not been removed or replaced with an unverified client-side assumption.

The benchmark helper now accepts the same executable, worker count, mode,
duration, bot count, and optional copied world template for paired tests.
It reports the worst *fully populated* 100-tick P95 window as well as the
previous maximum-player window, because a late idle window can hide an
earlier spike. The following paired 15-second 50-bot runs used one network
worker, two chunk workers, the same CPU mask, seed-0 disposable flat worlds,
the same stress client, and a 400/s aggregate place/break cap. “Warm” means
each binary received an identical copy of a previously generated world, not
that it reused the other's mutated world.

| Initial world / binary | Verified place+break | TPS, worst active 100 ticks | P95/P99 MSPT | Ticks >50 ms | Unexpected disconnects |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fresh / `28c8f827` | 2302+2302 (1 action failure) | 19.60 | 57.5/68.0 ms | 9 | 0 |
| Fresh / optimized | 2019+2019 | 19.98 | 22.0/27.6 ms | 0 | 0 |
| Copied world / `28c8f827` | 2085+2085 | 18.24 | 63.3/96.8 ms | 11 | 0 |
| Copied world / optimized | 1907+1907 | 19.91 | 24.6/50.1 ms | 2 | 0 |

All four runs delivered 22,050 chunks. The completed block-operation counts
differ by about 9–12%; these are short end-to-end samples, **not** a
same-number-of-actions throughput proof. They support a reduction in the
specific ping/PONG scan cost and in measured tick tails, but do not establish
scaling under arbitrary gameplay. The Python bot and network output (~1.4 GB
per run) can influence the results. Per-run reports and server logs are in
`tests/stress/results/network-benchmark-20261022T061930Z` (reference copied
world) and `network-benchmark-20261022T061949Z` (optimized copied world),
with fresh-world runs at `...T061835Z` and `...T061556Z` respectively.

Async saves are deferred: measured periodic saves are below 1 ms/tick here,
and the current persistence path writes individual `chunks/c_x_z.bin` files
with `fopen("wb")` after NBT encoding/compression. A correct save worker needs
immutable versioned snapshots, same-chunk read/write exclusion, atomic file
replacement, bounded queue/backpressure, eviction gating, and a shutdown
barrier; simply sending the live chunk to another thread would race the tick
and could lose data. `save_workers` remains rejected by setup validation.
The first candidate for parallel simulation is item physics, but it currently
reads live chunks and combines collision, pickup, inventory changes and
broadcasts inside one pass. It needs a stable snapshot and deterministic
single-threaded commit before `simulation_workers` can safely be accepted.

Remaining lock risks: `conns_lock` still protects item-entity broadcasts and
some administrative/container operations while outbound packet buffers are
filled. The item list and player gameplay fields are tick-owned, whereas the
connection list, lifetime and outbound buffers are cross-thread resources.
The task queue is globally shared but its observed contention did not justify
an MPSC or lock-free rewrite; fairness under one malicious high-rate player
is not yet proven. `out_lock` and the world job/completion queues deserve
targeted contention measurements under different workloads. No tick-region
threads or parallel item physics are introduced by this change.

Validation on this step: `make -B precommit-check` passed, including the
new concurrent performance-config test, task queue test, world consistency
tests, chunk codec tests and the 1/2/4-worker epoll integration/shutdown
cases. ASan+UBSan unit tests for these changed paths and multi-epoll
integration passed. The first ASan+UBSan 10-bot block run stopped at the
negative-shift UBSan finding above; after the fix, 10 bots verified 30 places
and 30 breaks with no sanitizer report, action error or disconnect. TSan
unit tests and multi-epoll integration passed; a separate 10-bot movement
run sent 750 movements with no TSan report or unexpected disconnect. Its
11 TPS is sanitizer overhead, not a production benchmark. A normal 4-worker
100-bot movement run sent 6,933 movements, received 42,557 chunks, had no
unexpected disconnect, and reported TPS 19.95 with P95/P99 23.9/27.0 ms in
the fully populated window. A one-bot connect run received 441 chunks with
no error at TPS 19.96. A 2-worker 25-bot mixed run verified 84 places
and breaks without error, but sent no movement packets, so it does not
validate mixed-mode movement. The sanitizer and stress runs cover sampled
interleavings, not a proof of race freedom. A separate normal-build shutdown
test sent SIGTERM to a two-worker server after seven seconds of 25 active
`fastblocks` bots on a disposable world: the server exited 0, 1,501 places
and 1,484 breaks had been verified, and all 25 clients observed the expected
shutdown disconnect. The world retained 441 per-chunk files; this checks
clean termination under writes, but not full post-restart durability of every
individual block action.

## Simulation-worker gate: fixed work and item ownership (2026-10-22)

`stress.py --mode blocks --cycles-per-bot N` now completes exactly N verified
place+break cycles per bot or reports how many remain at its deadline. The
benchmark records confirmed work, elapsed time, per-operation latency, packet
profiles and sampled item-tick phases. Its 100-tick summaries omit the final
partial window; scheduled-tick `tasks` also omit packets handled by the fast
pass. Neither is a complete operation counter. The authoritative completion
counter is the client-observed block update after each action. Tests use a
fresh bot identity and private targets; the existing safe target limit is 50
bots. Extending to 100 block bots needs a distinct, valid, within-reach target
layout, not two clients racing the same block.

With one epoll worker, two chunk workers, fresh flat worlds, a 10 ms pause
after each place and break, and a 120-second deadline (60 seconds for 10 bots):

| Bots | Confirmed place+break | Missing cycles | Elapsed | Confirmed blocks/s | Worst fully populated 100-tick TPS; P95/P99 MSPT; ticks >50 ms |
| ---: | ---: | ---: | ---: | ---: | --- |
| 10 | 1000+1000 | 0 | 8.520 s | 234.7 | 19.95; 14.0/15.7 ms; 0 |
| 25 | 2500+2500 | 0 | 8.050 s | 621.1 | 19.92; 19.8/20.6 ms; 0 |
| 50 | 5000+5000 | 0 | 20.758 s | 481.7 | 19.93; 37.4/49.9 ms; 1 |

There were no action failures or unexpected disconnects. Different bot counts
are different workloads and these short samples must not be interpreted as a
scaling curve. Logs/reports are in ignored `tests/stress/results/` directories
`network-benchmark-20261022T064044Z`, `...T064102Z`, and `...T064116Z`.

A same-work 50-bot, 100-cycle pair copied the *same* disposable world into
each run. The `28c8f827` reference finished 5000+5000 in 21.801 s; the
current instrumented binary finished 5000+5000 in 23.772 s. Their worst
fully populated 100-tick windows were respectively 17.84 TPS, P95/P99
70.3/96.6 ms, 12 ticks >50 ms, and 19.49 TPS, P95/P99 50.0/73.6 ms,
6 ticks >50 ms. Both had zero missing cycles and zero unexpected disconnects.
The current run was **slower end-to-end** despite a better sampled tick tail;
extra profiling logs and run-to-run effects confound attribution. No
throughput gain is claimed. Reports: `network-benchmark-20261022T064248Z`
and `...T064326Z`.

The new `MC_PERF` spans separate the normal placement path's world write,
connected-block recomputation, outbound queueing and item relocation scan.
In the 50-bot run one 1.250 ms placement spent 1.242 ms in
`net_server_resolve_item_entities_for_block`; world write and recomputation
were 0.001 ms each. The relocation helper scans *all* live items for each
placement, so many drops make placement O(items). Player-action/break and
`USE_ITEM_ON` packet totals also rise as entities accumulate. Break profiling
now separates drop resolution, world mutation, synchronous flush, outbound
updates and item spawn. These spans are opt-in with `MC_PERF` and only log
operations above `MC_PERF_SLOW_MS`; they are samples, not latency percentiles.
In a separate 25-bot fixed-work sample, two >1 ms break handlers spent
1.596/1.620 ms and 1.224/1.267 ms in the synchronous flush; drop resolution
was at most 0.001 ms in those cases. The same run confirmed 2500+2500 edits
with no missing cycle (`network-benchmark-20261022T205629Z`). This identifies
occasional flush latency without yet demonstrating that async saves would
improve total throughput or preserve durability.

Item-tick profiling separates collision relocation, physics/world lookup,
outbound teleports, and pickup/inventory work. At 4,407 items, sampled pure
physics cost was 0.265 ms and collision relocation 0.184 ms. An earlier
1,229-item tick took 34.503 ms, of which 32.184 ms was pickup processing
and only 0.138 ms physics. Parallelizing the tiny arithmetic phase would
require copying thousands of records, a synchronization barrier and ordered
commit, while leaving the measured dominant pickup, broadcasts and placement
scan serial. Consequently **no `simulation_workers` or job pool is enabled**;
the existing setup rejection remains correct. This is a measured gate, not a
claim that all future simulations should remain serial.

The authoritative tick owns player state, inventories, world/chunk mutations,
item-array contents, pickups and packet ordering. A future independent job
must receive a value-only snapshot (item ID/generation, scalar position and
velocity, pickup delay, tick number, and copied collision samples or an
immutable versioned chunk view). It must contain no pointer to a live chunk,
connection, item slot or inventory. The tick must validate item identity and
world/chunk revision before committing results, recompute stale proposals
serially, apply proposals in original entity order, and perform pickups,
creation/destruction and broadcasts only after commit. The snapshot and
result buffers must remain owned until every worker has joined the barrier;
shutdown must drain or cancel jobs before freeing world and connections.
No such immutable collision snapshot or bounded proposal queue exists yet.

The next lower-risk performance candidate is a tick-owned spatial broadphase
for item/block intersections, with explicit updates on item spawn, motion and
destruction; it should be benchmarked against the O(items) scan before adding
threads. Pickup candidate filtering and broadcast fanout also merit profiling.
Do not move world writes or live connection pointers to workers. The
single-threaded item update order is the determinism baseline for any later
0/1/2/4-worker comparison.

`test_block_persistence.py` uses only a disposable local world and QA/admin
flags on that temporary process. Three bots each confirmed a place+break
cycle on a private target, then two left stone placed. A fresh server read
states `[1, 0, 1]` at the three coordinates after clean shutdown; a bot
broke the first stone, and a third server read `[0, 0, 1]` after the next
shutdown. This verifies actual block states after restart, not just the
existence of chunk files. It does not test crash consistency or concurrent
saving. The QA block read command is gated by existing QA and command
authorization checks.

Validation for this step: `make -B precommit-check` passed. An ASan+UBSan
server passed the three-bot, three-start persistence scenario and a two-epoll-worker
10-bot fixed workload (200 verified poses and 200 verified breaks, no missing
cycles or sanitizer report). A TSan server passed the same three-bot persistence test
and a four-epoll-worker 10-bot workload with the same 200+200 verified edits,
again without a reported race. These small sanitizer runs end before a full
100-tick `MC_PERF` window and their timing is not a performance baseline.
Sanitizers sample executed paths only; they do not prove race freedom.

## Player persistence ownership (phase 1E)

Player gameplay, inventory changes, disconnect cleanup and player saves remain
owned by the authoritative tick. No save worker is introduced. The player
store serializes direct load/save callers with one process-local mutex;
each write uses a unique temporary gzip file, file fsync, atomic rename and
directory fsync. The loaded passthrough NBT tree belongs exclusively to its
`mc_player_data_t`; slot values are copied independently and the tree is
freed with the player. Serialization clones unhandled tags rather than
sharing mutable subtrees with the output file.

The duplicate-identity gate reserves identities while departure cleanup is
pending, preventing a reconnect from loading before the departing player's
save finishes. These guarantees apply to one process using a world. There
is no interprocess world lock, and player/chunk/container files do not form
one atomic transaction. Running multiple server processes against the same
world remains unsupported. TSan's minimal runtime probe on 10.0.4.66 exits
66 with `unexpected memory mapping`; it does not validate these paths.
The first multi-bot sanitizer attempts exposed a test-probe error: the QA
read used `mc_world_get_block`, which reports air for a chunk still loading.
It now uses `mc_world_get_block_ready`, and the test retries an unavailable
chunk rather than confusing it with persisted air. The corrected three-bot
scenario passed under both sanitizers.
