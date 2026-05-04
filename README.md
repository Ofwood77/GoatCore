# OfwoodCore

OfwoodCore is an experimental Minecraft 26.1.1 server implementation written entirely in C.

The goal of the project is to understand and control the full server stack — from networking to gameplay — without relying on the JVM or the official server codebase.

---

## Features

- Standalone implementation (not a fork, not a wrapper)
- Modern protocol support (`26.1.1`, protocol `775`)
- Server-authoritative architecture (20 TPS tick loop)
- Non-blocking networking using `epoll`
- Chunk streaming with async generation workers
- Custom world and player persistence
- Player inventory and containers (chest, furnace, etc.)
- Crafting and cooking (MVP)
- Health, hunger, death, and respawn systems
- Item entities with basic physics
- Server-authoritative mining (timing validation)

---

## Architecture

The server is structured around three main subsystems:

- Network thread  
  Handles accept, socket I/O, packet decoding, and enqueues tasks.

- Tick thread (20 TPS)  
  Executes gameplay logic, world updates, protocol handling, and outgoing flush.

- World workers  
  Handle chunk loading, generation, and Anvil fallback.

This model ensures that all gameplay logic remains authoritative on the server side.

---

## Protocol

Supported states:

- Handshake
- Status
- Login (offline mode)
- Configuration
- Play

The protocol is implemented manually via reverse engineering, including:

- VarInt encoding/decoding
- Custom packet framing
- Partial compression support
- Core PLAY packet handling

---

## World and Chunks

- In-memory chunk management
- Async chunk generation and loading
- Anvil import support
- Custom storage format (`.mcc`)
- Autosave and eviction

Current runtime format:

- Flat storage using `block_state_id`
- Full height range (-64 to 319)

A future refactor toward section-based palettes is planned.

---

## Gameplay

Currently implemented:

- Player inventory
- Containers (chest, furnace, etc.)
- Crafting (2x2 and 3x3)
- Cooking systems
- Health, hunger, regeneration, death
- Item drops

### Mining

- Server-side validation (`START / ABORT / STOP`)
- Block hardness-based breaking time
- Rejection of invalid client actions

---

## Performance

The server includes built-in instrumentation:

```bash
MC_PERF=1 ./mc_server