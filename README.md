# 🚀 Giga-Server C (Minecraft 26.1)

A highly performant Minecraft server written entirely in C (C11) from scratch. This project aims to push the boundaries of Vanilla performance by ditching Java's Object-Oriented Programming (OOP) in favor of a strict **Data-Oriented Design (DOD)** architecture.

The ultimate goal: Zero garbage collection, zero memory fragmentation, and a minimal RAM footprint through aggressive bit-packing.

## ✨ Current Features (Phase 2 Completed)

* **Protocol 26.1 Support (Protocol 775):** Complete implementation of the network state machine (Handshake -> Login -> Configuration -> Play).
* **RAM Engine (Paletted Container):** In-memory chunk storage with dynamic resizing (0-bit, 4-bit, or 15-bit fallback). The network layer reads this structure directly for "zero-copy" serialization.
* **Ultra-fast Anvil Reader:** NBT parsing of Vanilla save files (`.mca`) via a linear **Arena Allocator** (zero `malloc` calls per tag). Strict `DataVersion` validation (4786).
* **Block Entity Store (Spatial Isolation):** Spatial hash table (*Open Addressing*) to completely separate entities (chests, signs) from the global block grid.
* **1:1 Vanilla Synchronization:** Automatic C code generation from official Mojang data reports (`registries.json`, `blocks.json`) to guarantee 100% accurate network IDs and Item -> BlockState mapping.
* **Built-in `mc_recorder` tool:** A custom "Man-in-the-Middle" network tool to capture complex Vanilla packets (like registry blobs or Chunk Templates) for local reuse and testing.

## 🧠 Architecture (Data-Oriented Design)

This server is designed to fit snugly inside your CPU's L1/L2 caches. 
* Physical block properties are heavily packed into `uint8_t` bitfields.
* Complex blocks with directional properties and their placement/drop logic are resolved via a static lookup table generated at compile time, eliminating expensive runtime "overrides" and branching.

## 🛠️ Build & Run

### Prerequisites
* A C compiler (GCC/Clang) with `C11` support.
* `Python 3` (for the code generation toolchain).
* `Make`.

### Instructions

1. **Generate assets and compile
