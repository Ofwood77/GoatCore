# Scalability goals and sustained endurance operation

GoatCore targets excellent 4–5-player efficiency and, long term, 1,000+ active
Survival players in one coherent shared world. Neither configured connections
nor this synthetic campaign establish human-player capacity. The 4–100 GiB and
larger memory policy remains in force without preallocation.

## Scope and isolation

Only `goatcore-test.service`, offline **25587**, is authorized for this campaign.
`goatcore.service`, 25565, its files, authentication, world and runtime are
protected. The installer does not use `deploy-remote.sh`, replace the shared
`/opt/goatcore/app/mc_server`, or edit any online configuration. A test-only
systemd drop-in selects a separate release, setup, runtime and disposable world
under `/var/lib/goatcore/endurance/<campaign>/`.

The offline QA port has broad **test-only** command permissions so scripted
identities can prepare lanes and replenish their own inventories. `Ofwood_`
remains configured as administrator. Restrict network access to this test port;
these permissions must never be copied to 25565. All bots run on the dedicated
host, using the existing protocol-775 codec and login, in a **single** test world.

## Workload

Up to 25 clients per Python process, paced independently; processes occupy a
separate cgroup from the server. No second Minecraft protocol implementation is
introduced. The controller requests 5/25/50/100/150/200/250 bots progressively.
A plateau requires all requested bots recently completing confirmed gameplay,
not merely a configured count or successful TCP connection.
New identities are admitted at one per second across each newly added cohort,
rather than issuing simultaneous login/fixture/inventory bursts. The 45-second
minimum plateau can extend until every requested client becomes active.

Each bot has a private target in one of five geographical groups, including
negative coordinates, with nearby peers sharing updates. All use Survival for
measured actions. QA prepares a full support and a walking strip. Eight cycling
families cover torch, lantern, slab, stair, door, trapdoor, ladder and stone.
Each cycle walks 18 blocks out and back in twelve paced position packets, checks
the authoritative final position, places the assigned block and verifies its
type plus sequence ACK, then starts/finishes mining and verifies air plus ACK.
Doors also verify their second half. The nominal cycle period is 40 seconds,
including a four-second mining wait, without catch-up bursts.
Stone miners also receive a QA-prepared torch above their placed support and
verify its removal after mining that support. These scaffold attachments are
counted separately from client placements and confirmed support losses.

QA supplies 64 items and a pickaxe, replenishes periodically and restores food.
The fixture waits for the target neighborhood, validates its support and empty
destination, and checks the selected item before proposing placement. Ladder
clients approach the exposed face rather than clicking through their support.
Preparation, resource grants and this deterministic route are **artificial**.
This is not full human Survival: AI, unpredictable exploration, complex farms,
Redstone, combat, fluid flow and advanced physics are not represented. Mining
ACKs and observed states are gameplay confirmations, not disk confirmations.
The end-of-route check does not prove acceptance of every intermediate movement.
Entity spawn packet counts are not unique drop counts; pickup packets are
reported separately. Full client/entity simulation is not claimed.
Logical placement/mining proposals, successfully sent requests, confirmations,
in-flight operations, timeouts and cancelled operations are distinct counters.
An error during an ambiguous action stops escalation instead of silently
replaying it and claiming success. A live sample can legitimately include
in-flight work; a final cancellation is not reported as a confirmed action.

## Resources and protection

The audited host exposes a dual Xeon E5-2637 v2, 16 logical CPUs, approximately
108 GiB visible in LXC and an ext4/LVM volume on rotational storage. Physical
RAID/controller internals and hidden LXC ancestors are not inferred from this.
Online affinity remains 0–3. The test server uses 4,5,12,13 (two other physical
cores with siblings), at most 200% CPU, 8 GiB cgroup, no swap; generators use
6,7,14,15, at most 200% CPU, 2 GiB, no swap. Both use low CPU/I/O priorities.
This reserves online CPU placement but cannot eliminate shared memory/disk
contention. Re-audit topology/resources before reusing these values elsewhere.

The controller checks every approximately five seconds:

| Guard | Response |
|---|---|
| Public PID/state/restarts or protected file hashes change | Immediate campaign stop |
| Public status response >250 ms | Stop after three consecutive samples |
| Available RAM <2 GiB | Stop after three samples |
| Filesystem free space <8 GiB | Immediate stop |
| Campaign world >8 GiB or WAL >512 MiB | Immediate stop |
| OOM/OOM-kill in either campaign cgroup | Immediate stop |
| Global memory PSI full avg10 >1%, I/O PSI full avg10 >20% | Stop after three samples |
| Worker status older than 30 s, storage error or test server exit | Immediate stop |
| Tick trace stops advancing for 60 s after startup | Immediate stop |
| Server/generator binary or code version differs | Stop; do not silently run obsolete code |
| Test TPS <10 after startup | Stop after three samples |
| Requested activity not achieved within 300 s | Stop escalation/campaign |

The public baseline must be healthy before any bot starts. The public probe is
read-only Minecraft **status**, never bot login; it does not validate real online
player latency or authentication. Health thresholds are conservative operational
safeguards, not an SLA. A sustained safety violation ends this campaign and
preserves evidence; do not force 250 by suppressing a guard.

## Evidence, retention and measurements

The test-server launcher drains opt-in tick and save traces through FIFOs into
rotating sinks, without truncating a live regular-file descriptor. Raw server
logs and two traces retain 8 MiB plus three compressed generations each. The
controller retains 8 MiB plus six compressed metrics generations and 1 MiB plus
three event generations. Worker files are atomic bounded status snapshots, not
unbounded event lists. Temporary writes of diagnostic JSON do not claim gameplay
durability. Logs may retain less elapsed history at high traffic; cumulative
counters, current summaries and plateau snapshots survive rotation.

`metrics/manifest.json` records UTC start, published/source SHA, server binary
SHA, generator hash, effective configuration, public baseline and workload.
`metrics/latest.json` and rotating `metrics.jsonl` include:

- Requested, connected, world-ready and recently active clients; confirmations,
  missing actions, retries, movement position checks and packet/byte totals.
- Last available rolling tick window (up to 30 s): TPS, MSPT percentiles/max,
  over-budget ticks, ready players, item count, queue depth and fast-pass costs.
- Save phase counts, bytes, wall/CPU time, SQLite commits/checkpoints and errors
  where exposed by the existing trace. Buffered traces can lag real time.
- Server/generator RSS/HWM, CPU including per-thread cumulative CPU, `/proc` I/O,
  cgroup memory/CPU/events, PSI, disk free space, world/database/WAL file sizes.
- Shared device `/proc/diskstats` counters: not attributable physical writes or
  per-process fsync latency. No cold-cache or power-loss claim.

SQLite runs inside the server, not a separate process. Component-level cache,
allocation, loaded-chunk/region and unique drop counts are **not all available**
in existing runtime instrumentation; absent values must not be invented. Network
counts cover each observed client session; disconnects/restarts are explicit and
can reset session counters. Plateau summaries must state measurement windows.

## Supervision and version boundary

`goatcore-endurance.service` survives SSH disconnect. Each bot has four bounded
connection attempts with exponential backoff; each generator process has at most
two controlled restarts. No unit has an infinite `Restart=always` policy.
The controller's exit closes generators and stops **only** 25587, letting native
player-save barriers and chunk drainage finish (180 s shutdown budget).

After this mission's push, the campaign records the published SHA and polls the
public GitHub refs API every 120 s, without a token, SSH key or credential helper.
A different observable `origin/main` SHA stops the campaign. Five consecutive
API failures also stop it. **A local unpushed commit cannot be detected.** The
next agent must collect metrics before its next commit and stop this campaign
explicitly if publication detection has not yet fired. A changed binary or
exited test server also stops the generator. The campaign is not enabled at boot.

## Commands (local checkout)

Inspect the existing stage/deploy scripts before use. A deployment creates a new
campaign root and test release; existing campaign evidence is never overwritten.
The published SHA argument must match the confirmed normal push:

```sh
scripts/remote-endurance-deploy.sh /opt/goatcore/phase2a-stage.EXAMPLE \
  endurance-UNIQUE-ID PUBLISHED_40_HEX_SHA true
scripts/remote-endurance.sh start
scripts/remote-endurance.sh status
scripts/remote-endurance.sh collect tests/stress/results/endurance/NEW-COLLECTION
scripts/remote-endurance.sh stop
```

`stop` is the emergency/manual stop: no command targets 25565. On the host,
`systemctl stop goatcore-endurance.service` also drains generators and stops the
test service. `touch /var/lib/goatcore/endurance/current/metrics/STOP` requests the
same controller shutdown. A stopped campaign is immutable: use a new ID to run
again. The installer preserves prior runtime/worlds and replaces only the test
service override/current symlink; it does not delete the old offline world.
To return to the prior test runtime after stopping, remove **only** the
`goatcore-test.service.d/endurance.conf` override, daemon-reload and start the
test service only when separately intended. Never restart 25565 for this rollback.

Metrics collection excludes the world, player data, FIFOs and credentials.
Keep large raw collections ignored by Git; commit lightweight validation evidence.

## Qualification status

**The current validated campaign ceiling is 100 bots.** The paced ramp reached
150 connections but only 137 recently active clients at its last observation;
the rolling TPS window fell below the conservative 10-TPS guard and the campaign
stopped cleanly. Targets 150/200/250 are not qualified. This does not establish
an absolute hardware capacity: admission, synchronous inventory persistence and
global player synchronization remain represented in this workload.

Before starting the published endurance run, set its generated `config.json`
`ramp` to `[5,25,50,100]`. The full 250 target in the example is for future guarded
qualification, not a validated default capacity. Change a campaign configuration
only before its first start; its manifest records the effective settings.

The matrix, failed fixture attempts, corrections, restart checks and limits are
recorded in `docs/vanilla-compatibility.md` and
`tests/fixtures/vanilla-26.1.1-endurance-validation.json`. An interrupted ramp or
a safely reduced/stopped workload is a result, not 250-active-player support.
