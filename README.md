# GoatCore

GoatCore is an experimental Minecraft Java Edition **26.1.1** server
implementation written in **C11 for Linux** (protocol **775**, DataVersion
**4788**). Its priority is faithful, authoritative Vanilla behavior, with
predictable ticks, efficient native networking and durable persistence.
It is an independent implementation, not a wrapper around the official server.
The server is under active development and is not a complete Survival replacement.

## Project goals

- **Small servers:** excellent responsiveness for communities of 4–5 players,
  without paying the permanent overhead of a large deployment.
- **Long-term scaling:** work toward **1,000+ concurrent active Survival players
  in one shared world**. This is an engineering ambition, **not a demonstrated
  capability**. CPU, memory bandwidth, NUMA, storage, network, active chunks,
  entities and gameplay complexity all affect capacity.
- **Vanilla fidelity:** optimize implementation costs while preserving observable
  Minecraft rules. Unsupported simulation is reported rather than counted as parity.

## Vanilla Java 26.1.1 and GoatCore

Statuses describe the covered scenarios, not complete feature parity.

| Feature | Vanilla Java 26.1.1 | GoatCore | GoatCore status |
|---|---|---|---|
| Protocol | Complete official implementation | Handshake, status, login, configuration, partial Play | Implemented subset |
| Authentication | Official account authentication | Offline and Mojang authentication paths | Positive online client validation remains required |
| Chunk loading | Full dimensions and streaming | Generation workers, streaming, eviction, persistent reload | Implemented subset |
| Ordinary block placement | Full block rules | Placement, refusal, consumption and updates | Differentially tested subset |
| Slabs | Placement and merging | Top/bottom/double slabs | Differentially tested |
| Stairs | Placement and neighbor shapes | 192 reference observations | Differentially tested |
| Doors | Two halves, hinges, interaction | Placement, interaction, support loss, drops | Differentially tested subset |
| Trapdoors | Placement, interaction, waterlogging | Facing, half, open, preserved properties | Differentially tested subset |
| Torches | Standing/wall variants and light | Ordinary, soul and copper supports/placement | Differentially tested; lighting partial |
| Lanterns | Standing/hanging variants | Support rules, state, waterlogging and drops | Differentially tested subset |
| Ladders | Attachment and climbing | Facing, support, shape and waterlogging | Attachment tested; climbing incomplete |
| Survival inventories | Complete Survival | Inventory, consumption, pickup, drops, crafting/cooking subsets | Partial |
| Block entities | Full simulation | Selected containers; seven complex types preserved in opaque NBT tests | Partial; preservation is not full gameplay |
| Entity simulation | Mobs, AI and full physics | Players and basic item entities | Partial |
| Redstone | Complete circuits | No complete circuit simulation | Not implemented as a complete system |
| Fluids | Flow and scheduled simulation | Some represented/waterlogged states | Partial; flow incomplete |
| Lighting | Propagation and updates | Partial network/world lighting | Partial |
| World generation | Full Vanilla worlds | Current native generator | Not Vanilla-complete |
| Player physics | Full movement/collisions | Movement and partial block geometry | Partial; ladder climbing pending |
| Persistence | Official world formats | Legacy, Anvil and GoatWorld; atomic player files | Experimental backends; multi-file atomicity absent |
| Multiplayer | Complete game | Visibility, state broadcasts, reconnect and selected interactions | Differentially tested subset |

The block corpora include **310 attached-block cases**, **335 door/trapdoor
scenarios**, and **192 stair observations**. They are functional comparisons,
not performance comparisons. See [compatibility reports](docs/vanilla-compatibility.md).

## Architecture

- C11, Linux `epoll`, `pthread`, bounded network work and configurable CPU affinity.
- A coherent authoritative tick; modular placement, support, neighbor and shape rules.
- Official-version registry data: 1,168 block types and 29,873 block states.
- Experimental GoatWorld: private checksum-pinned SQLite, **WAL + synchronous FULL**,
  LZ4 compression, indexed chunks and one asynchronous chunk writer.
- Independent immutable chunk snapshots, dirty generations, bounded batches and
  shutdown drainage. No parallel gameplay simulation is implied.
- Connected player state stays in RAM. Hotbar-only changes coalesce on a 30-second
  target deadline; inventory persistence barriers remain immediate.
- Player NBT files retain atomic temporary-file publication, file `fsync`, rename
  and directory `fsync`. Periodic/forced player flushes are still synchronous.

## Storage backends

| Backend | Configuration | Characteristics | Status |
|---|---|---|---|
| Legacy `.bin` | `chunk-storage=bin` | Historical chunk files | **Default** |
| Anvil | `chunk-storage=anvil-atomic-experimental` | Native `.mca`, preserved supported opaque data, atomic region rebuilds | Experimental |
| GoatWorld SQLite | `chunk-storage=goatworld-sqlite-experimental` | Indexed compressed chunks, grouped WAL FULL transactions | Experimental, explicit opt-in |

`chunk-save-mode=async` enables the existing asynchronous pipeline on supported
experimental backends. There is no automatic migration or implicit fallback.
Use a **separate new world** when selecting a different backend. GoatWorld databases
cannot be opened directly by Vanilla. Full-world import/export is not claimed.

## Performance and benchmarks

A fixed-work Xeon/HDD campaign compared the player-save fix with its parent,
with three repetitions per population. Both versions used the same disposable
world, block/support-loss workload, view/simulation distance 2, compilation and
instrumentation. The temporary cgroup was limited to 4 GiB, without swap, with
CPU affinity and reduced CPU/I/O priority. Hardware exposed a dual Xeon E5-2637 v2
and an ext4/LVM volume on rotational storage; controller-level physical writes
were not inferred from process counters.

| Bots | Median TPS before → after | MSPT P99 before → after (ms) |
|---:|---:|---:|
| 1 | 20.023 → 20.042 | 0.356 → 0.616 |
| 2 | 17.518 → 19.963 | 111.722 → 0.404 |
| 10 | 3.506 → 20.007 | 869.524 → 3.346 |
| 25 | 2.445 → 20.043 | 1,236.546 → 10.690 |
| 50 | 1.969 → 19.965 | 1,918.757 → 15.057 |

At 50 bots, player `fsync` calls during active load fell from **6,000 to zero**.
Across the full execution, including final saves, they fell from **6,500 to 300**.
Median total time fell from **173.336 to 53.608 seconds**. Each version confirmed
**23,760 actions** across its 15 runs, with matching final inventories and verified
restarts. This workload used controlled test inventories, not complete human Survival.

The Python generator approached one saturated core at 50 bots. These short
windows **do not prove constant 20 TPS**, maximum player capacity, or equivalence
to human gameplay. Values slightly above 20 reflect finite measurement windows.
The new endurance profile is a separate workload and must not be compared directly.

Source: [player-save validation manifest](tests/fixtures/vanilla-26.1.1-player-save-validation.json),
[methodology and detailed metrics](docs/vanilla-compatibility.md).

### Vanilla performance comparison

A strictly comparable direct Vanilla-versus-GoatCore TPS benchmark is not yet
published. Functional differential tests and storage microbenchmarks do not
establish that GoatCore is faster than the official server. Direct comparisons
require matching workloads, world state, resources, persistence and deferred work.

## Hardware and memory

The target is **Linux x86-64**, including homelabs, older Xeon/dual-socket ECC
servers, HDD RAID, SSD and NVMe. The official minimum target profile is **4 GiB
available per instance**; configurable budgets cover 8, 16, 32, 64, **100 GiB and
beyond**. This is a capacity budget, not an allocation requirement or player guarantee.

`memory-budget-mib` defaults to `4096`. It is a conservative **virtual address-space
ceiling**, adjusted for available memory, readable cgroup v2 headroom, inherited
limits and a 512 MiB reserve. It is not complete RSS/cache accounting. Unsupported
or unreadable cgroup hierarchies currently prevent startup. The server can consume
far less RAM than its budget; it does not reserve the configured capacity.

Use RAM where measured caching/batching benefits justify it, while preserving
headroom for the OS, page cache and other services. Never infer 1,000-player
capacity merely from a 100 GiB configuration.

## Building and running

On a Debian-compatible Linux system:

```sh
sudo apt install build-essential make git python3 pkg-config zlib1g-dev \
  libcurl4-openssl-dev libssl-dev liblz4-1 libzstd1
make
make test
```

The build prepares a private, checksum-pinned SQLite SDK through
`tools/prepare_storage_deps.py`; the initial preparation needs HTTPS access.
OpenSSL authentication is enabled by default (`USE_OPENSSL=1`). Disabling it
selects authentication stubs; libcrypto is still required for integrity checks.
Node.js tooling under `tests/minecraft-bot` is optional; the protocol-775 Python
stress clients require no Mineflayer installation.

For a fresh, isolated runtime directory, provide `server.config`:

```properties
memory-budget-mib=4096
online-mode=false
offline-skins=false
chunk-storage=goatworld-sqlite-experimental
chunk-save-mode=async
view-distance=2
simulation-distance=2
```

Run from that directory with the repository assets linked and a separate
`setup.conf` suitable for the host (use `cpu_affinity = auto` for a portable setup):

```sh
ln -s /path/to/GoatCore/assets assets
MC_WORLD_PATH=/path/to/new-disposable-world MC_BIND_PORT=25587 \
  MC_SETUP_FILE=/path/to/setup.conf /path/to/GoatCore/mc_server
```

The process creates a default config when absent. `MC_BIND_PORT` overrides the
listen port (normally 25565). Keep separate worlds, configs and binaries per
instance. Offline identities are spoofable: restrict access to offline/QA servers.
`MC_ADMIN_USERNAME=Ofwood_` grants that offline identity administrator privileges;
`MC_QA_MODE=1` exposes test commands to authorized users. Do not enable broad
`MC_ADMIN_COMMANDS=1` test permissions on a public instance.

CPU settings in `setup.conf` include `cpu_cores`, `cpu_affinity`,
`network_workers` (1/2/4), `chunk_workers` (auto or 1–16) and `tick_rate=20`.
Affinity restricts placement, not CPU consumption; use cgroups for quotas.
There is one persistence worker where supported, not a parallel simulation pool.

Repository development rules require Minecraft servers and bots to run on the
dedicated remote test host, **never on the development laptop**. See
[remote testing instructions](.agents/skills/remote-deploy/SKILL.md).

## Validation and endurance

```sh
make -B precommit-check
```

The suite includes C tests, protocol/login checks and storage validation.
Targeted ASan/UBSan and TSan runs, differential official-server fixtures, crash
injection, restart tests and Xeon measurements are documented in
[the compatibility report](docs/vanilla-compatibility.md).

A supervised endurance harness uses real offline clients, paced movement and
confirmed block actions on the test instance. It separates generator resources,
rotates evidence, protects the public service and stops on a newly observable
published commit. **Configured bot count is not evidence of active player count.**
See [endurance operation and limits](docs/endurance-testing.md) for actual campaign
results, safety thresholds, metric collection and emergency stop commands.
The current guarded ramp qualified **100 active scripted clients**. Admission
to 150 connections triggered the conservative sub-10-TPS stop; 150/200/250 are
not qualified. The published endurance campaign is capped at 100, subject to
ongoing guards. This is not a human-player capacity guarantee.

## Current limitations and roadmap

Survival gameplay, general player physics, entities/AI, world generation,
Redstone, fluid flow, lighting and several interactions remain incomplete.
Chunk/player/item persistence has no global transaction. SIGKILL tests do not
prove universal physical power-loss durability. Authentication needs positive
real-client validation before promotion to a public online deployment.

Next work includes ladder climbing and partial player physics, advanced
collisions, scheduled ticks, Redstone, fluids, lighting, entities and Vanilla
world generation. Scalability qualification will progress through representative
5/25/50/100/250/500/1,000-player targets, without invented delivery dates.

Minecraft is a trademark of Mojang/Microsoft. GoatCore is independent and is not
affiliated with Mojang or Microsoft.
