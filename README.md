# GoatCore

![GoatCore mascot](assets/pp.png)

**Fast ticks. Pure C.**

GoatCore is an experimental Minecraft server engine written in C, targeting
Minecraft `26.1.1` and protocol `775`.

It focuses on low-level networking, predictable ticks, and a lightweight
architecture built around C11, `epoll`, `pthread`, and `zlib`. The goal is to
control the whole server stack directly, from packet framing to gameplay state,
without a JVM and without wrapping the official server.

GoatCore is still experimental. It is meant for protocol research, gameplay
systems work, and aggressive iteration on a small, inspectable C codebase.

## Status

Current focus:

- Stabilize compilation and automated tests on Debian.
- Keep Minecraft `26.1.1` / protocol `775` compatibility aligned.
- Improve real Java client connection, movement visibility, inventory, and
  container behavior.
- Grow the QA tooling without changing the core architecture too quickly.

Known limitations:

- The PLAY protocol is partial and still evolving.
- Advanced client behavior can expose missing packet details.
- Compression, container sync, and long-term chunk/world format work still need
  hardening.

## Features

- Standalone Minecraft server engine, not a fork and not a wrapper.
- Modern protocol target: Minecraft `26.1.1`, protocol `775`.
- Server-authoritative 20 TPS tick loop.
- Non-blocking networking using `epoll`.
- Tick and network work split across `pthread` based runtime paths.
- Chunk streaming with async generation workers.
- Custom world and player persistence.
- Player inventory and containers, including chest and furnace paths.
- Crafting and cooking MVP.
- Health, hunger, death, and respawn systems.
- Item entities with basic physics.
- Server-authoritative mining with timing validation.
- Local QA bot tooling for smoke, movement, inventory, and command checks.

## Install

Clone the repository:

```bash
git clone https://github.com/Ofwood77/OfwoodCore.git
cd OfwoodCore
```

Install the Debian build dependencies:

```bash
sudo apt update
sudo apt install build-essential make gcc git python3 zlib1g-dev pkg-config
```

OpenSSL is optional and only needed when building with `USE_OPENSSL=1`:

```bash
sudo apt install libssl-dev
```

Build GoatCore:

```bash
make
```

Optional QA bot setup, only needed for Minecraft client automation:

```bash
cd tests/minecraft-bot
npm install
cd ../..
```

Node.js `22` or newer is recommended for the Mineflayer / `minecraft-protocol`
tooling.

## Build

Compile the server:

```bash
make
```

Clean generated build outputs:

```bash
make clean
```

## Run

Default launch:

```bash
./mc_server
```

Useful runtime overrides:

```bash
MC_BIND_PORT=25566 ./mc_server
MC_WORLD_PATH=/tmp/goatcore_world ./mc_server
MC_PERF=1 ./mc_server
```

The default server port is normally `25565`, unless overridden by environment
or configuration.

## Tests

C tests:

```bash
make test
```

Server smoke start:

```bash
make smoke-start
```

Repository checks:

```bash
make hygiene-check
make generated-check
```

Minecraft QA bot tests live under `tests/minecraft-bot` and require Node.js 22
or newer:

```bash
cd tests/minecraft-bot
npm install
cd ../..
RUN_MINECRAFT_E2E_TESTS=1 MC_HOST=127.0.0.1 MC_PORT=25565 MC_TEST_MODE=smoke ./scripts/minecraft-e2e-test.sh
```

The QA mode can drive GoatCore through chat commands and verify connection,
movement, gamemode changes, health, inventory, and chest placement:

```bash
RUN_MINECRAFT_E2E_TESTS=1 RUN_DESTRUCTIVE_TESTS=1 MC_TEST_MODE=qa ./scripts/minecraft-e2e-test.sh
```

## QA Commands

For a real Java client, the current debug command path uses normal chat messages
prefixed with `!qa`:

```text
!qa state
!qa creative
!qa survival
!qa damage 1
!qa heal 20
!qa give stone 8
!qa chest
```

These commands are development helpers. They are intended for local worlds and
test sessions, not public servers.

## Architecture

Important areas:

- `src/`: main server implementation.
- `include/`: public internal headers shared across modules.
- `src/net/`: socket handling, `epoll`, connection lifecycle, task queue integration, and
  server tick coordination.
- `src/protocol/`: packet framing, VarInt handling, protocol states, and clientbound/serverbound
  handlers.
- `src/protocol/handlers/play.c`: main PLAY state implementation. Large today, and a candidate for progressive
  slicing once behavior is stable.
- `src/world/`: chunks, Anvil import, block storage, players, containers, NBT, and world
  persistence.
- `src/gameplay/`: mining, block drops, crafting, furnace/cooking logic.
- `src/generated/`: generated registries and Minecraft data tables for the current target.
- `tools/`: data generation, protocol utilities, ping tools, recorder tools.
- `scripts/`: higher-level test wrappers and local automation.
- `tests/`: focused C tests plus the Minecraft QA bot.

The runtime is organized around three major flows:

- Network path: accepts clients, reads sockets, decodes packet frames, and queues work.
- Tick path: applies authoritative gameplay and protocol state at predictable intervals.
- World worker path: loads, generates, streams, saves, and evicts chunks.

## Protocol

Implemented states include:

- Handshake
- Status
- Login in offline mode
- Configuration
- Play

The protocol is implemented manually, including VarInt encoding, packet framing,
selected registry/tag/chunk payloads, and partial PLAY packet handling.

## Mascot

GoatCore's mascot is a charging voxel goat: cute enough to remember, aggressive
enough to smash through slow ticks.

The image in `assets/pp.png` is the public mascot/logo used by the project.

## AI Use

Parts of the project documentation, QA workflow, and development planning have
been assisted by AI tools. Code changes still need normal review, local testing,
and project owner approval before being pushed or released.

## License And Notice

GoatCore is an independent experimental server engine. Minecraft is a trademark
of Mojang/Microsoft. This project is not affiliated with Mojang or Microsoft.
