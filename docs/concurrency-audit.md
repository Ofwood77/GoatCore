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
