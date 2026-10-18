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
