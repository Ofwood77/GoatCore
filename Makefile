CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Iinclude -Isrc/generated -Isrc/world -Isrc/util -D_POSIX_C_SOURCE=200809L -MMD -MP -pthread
LDFLAGS ?= -lz -lm -pthread -lcurl

USE_OPENSSL ?= 1
ifeq ($(USE_OPENSSL),1)
    LDFLAGS += -lssl -lcrypto
    CRYPTO_SRC = src/protocol/crypto_openssl.c
    AUTH_SRC = src/protocol/auth_service.c
else
    CRYPTO_SRC = src/protocol/crypto_stub.c
    AUTH_SRC = src/protocol/auth_stub.c
endif

# G2 uses a private, checksum-pinned SQLite with the upstream WAL-reset fix.
STORAGE_SDK = .local/storage-deps
SQLITE_LIB = $(STORAGE_SDK)/libsqlite3.a
SQLITE_CFLAGS ?= -std=c11 -O2 -pthread -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION=1
CFLAGS += -I$(STORAGE_SDK)/include
# Integrity SHA-256 also needs libcrypto when Mojang authentication is stubbed.
LDFLAGS += $(SQLITE_LIB) -ldl -l:liblz4.so.1 -l:libzstd.so.1 -lcrypto
GOATWORLD_SRC = src/world/goatworld_store.c src/experimental/goatworld_codec.c src/experimental/goatworld_pack.c src/experimental/goatworld_sqlite.c

SRC = \
    src/main.c \
    src/net/server.c \
    src/net/buffer.c \
    src/net/task_queue.c \
    src/gameplay/block_drops.c \
    src/gameplay/crafting.c \
    src/gameplay/furnace.c \
    src/gameplay/mining.c \
    src/gameplay/placement.c \
    src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c \
    src/world/block_state.c \
    src/generated/generated_block_ids.c \
    src/protocol/varint.c \
    src/protocol/inventory.c \
    src/protocol/framing.c \
    src/protocol/offline_uuid.c \
    src/protocol/profile_json.c \
    src/protocol/profile_service.c \
    src/protocol/profile_wire.c \
    $(CRYPTO_SRC) \
    $(AUTH_SRC) \
    src/protocol/handlers/handshake.c \
    src/protocol/handlers/login.c \
    src/protocol/handlers/configuration.c \
    src/protocol/handlers/play.c \
    src/protocol/handlers/status.c \
    src/world/chunk_store.c \
    src/world/chunk.c \
    src/world/container_store.c \
    src/world/anvil.c src/world/region_storage.c \
    src/world/nbt.c \
    src/world/packed.c \
    src/world/block_entity_store.c \
    src/world/paletted_container.c \
    src/world/player_store.c \
    src/world/block_registry.c \
    src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/save_pipeline.c \
    src/util/arena.c \
    src/util/setup.c \
    src/util/mc_util.c \
    src/util/atomic_file.c \
    src/util/memory.c \
    src/generated/generated_minecraft_ids.c \
    src/generated/generated_registries.c \
    src/generated/generated_block_loot.c \
    src/generated/generated_block_hardness.c \
    src/generated/generated_mining_data.c \
    src/generated/generated_item_food.c \
    src/generated/generated_crafting_recipes.c \
    src/generated/generated_cooking_recipes.c \
    src/generated/generated_item_place.c

OBJ = $(SRC:.c=.o)

TOOL_BINS = mc_recorder mc_anvil_dump mc_gen_bench mc_save_bench mc_trace_io mc_region_bench mc_region_txn mc_native_bench mc_world_migrate mc_migrate_fault mc_goatworld mc_goatworld_fault test_goatworld test_goatworld_storage_fault mc_server_g2_fault

TEST_BINS = \
    test_varint \
    test_login \
    test_profile \
    test_auth \
    test_login_online \
    test_framing_compression \
    test_framing_crypto \
    test_pvp \
    test_nbt \
    test_nbt_arena \
    test_block_registry \
    test_stairs \
    test_block_entity_store \
    test_paletted_container \
    test_chunk_network_codec \
    test_anvil_roundtrip \
    test_region_storage \
    test_region_txn \
    test_atomic_persistence \
    test_player_nbt \
    test_mining \
    test_block_drops \
    test_world_consistency \
    test_native_storage \
    test_save_pipeline \
    test_chunk_migration \
    test_setup \
    test_task_queue \
    test_perf_config \
    test_goatworld_storage \
    test_hinged_blocks \
    test_attached_blocks \
    test_player_save \
    test_memory

GENERATED_HEADERS = \
    src/generated/generated_block_ids.h \
    src/world/block_registry.h \
    src/generated/generated_minecraft_ids.h \
    src/generated/generated_registries.h \
    src/generated/generated_block_loot.h \
    src/generated/generated_block_hardness.h \
    src/generated/generated_mining_data.h \
    src/generated/generated_item_food.h \
    src/generated/generated_crafting_recipes.h \
    src/generated/generated_cooking_recipes.h \
    src/generated/generated_item_place.h

GENERATED_SOURCES = \
    src/generated/generated_block_ids.c \
    src/world/block_registry.c \
    src/generated/generated_minecraft_ids.c \
    src/generated/generated_registries.c \
    src/generated/generated_block_loot.c \
    src/generated/generated_block_hardness.c \
    src/generated/generated_mining_data.c \
    src/generated/generated_item_food.c \
    src/generated/generated_crafting_recipes.c \
    src/generated/generated_cooking_recipes.c \
    src/generated/generated_item_place.c

all: mc_server

DATA_REPORTS_DIR ?= data/26.1.1/reports
MC_IDS_SOURCE ?= $(DATA_REPORTS_DIR)
MC_RECIPE_SOURCE ?= mc_vania_asset/client/data/minecraft/recipe
MC_ITEM_TAG_SOURCE ?= mc_vania_asset/client/data/minecraft/tags/item
MC_SERVER_JAR_SOURCE ?= mc_vania_asset/server/server.jar
MC_BLOCK_LOOT_SOURCE ?= $(MC_SERVER_JAR_SOURCE)
GENERATE ?= 0

ifeq ($(GENERATE),1)
src/generated/generated_block_ids.c src/generated/generated_block_ids.h: tools/gen_block_ids.py $(DATA_REPORTS_DIR)/blocks.json $(DATA_REPORTS_DIR)/registries.json
	python3 tools/gen_block_ids.py $(DATA_REPORTS_DIR)/blocks.json $(DATA_REPORTS_DIR)/registries.json src/generated/generated_block_ids.c src/generated/generated_block_ids.h

src/world/block_registry.c src/world/block_registry.h: tools/generate_registry.py $(DATA_REPORTS_DIR)/blocks.json
	python3 tools/generate_registry.py $(DATA_REPORTS_DIR)/blocks.json src/world/block_registry.c src/world/block_registry.h

src/generated/generated_minecraft_ids.c src/generated/generated_minecraft_ids.h src/generated/generated_minecraft_ids.json: \
	tools/gen_minecraft_ids.py \
	$(MC_IDS_SOURCE)
	@mkdir -p src/generated
	python3 tools/gen_minecraft_ids.py $(MC_IDS_SOURCE) src/generated/generated_minecraft_ids.c src/generated/generated_minecraft_ids.h src/generated/generated_minecraft_ids.json

src/generated/generated_registries.c src/generated/generated_registries.h: tools/gen_registries.py src/world/block_registry.h src/world/block_registry.c
	@mkdir -p src/generated
	python3 tools/gen_registries.py src/world/block_registry.h src/generated/generated_registries.c src/generated/generated_registries.h

src/generated/generated_block_loot.c src/generated/generated_block_loot.h: tools/gen_block_loot.py src/generated/generated_minecraft_ids.json $(DATA_REPORTS_DIR)/blocks.json $(MC_BLOCK_LOOT_SOURCE) $(wildcard $(MC_BLOCK_LOOT_SOURCE)/*.json)
	@mkdir -p src/generated
	python3 tools/gen_block_loot.py src/generated/generated_minecraft_ids.json $(DATA_REPORTS_DIR)/blocks.json $(MC_BLOCK_LOOT_SOURCE) src/generated/generated_block_loot.c src/generated/generated_block_loot.h

src/generated/generated_block_hardness.c src/generated/generated_block_hardness.h: tools/gen_block_hardness.py tools/attached_reference.py tests/fixtures/vanilla-26.1.1-attached.tsv $(DATA_REPORTS_DIR)/blocks.json
	@mkdir -p src/generated
	python3 tools/gen_block_hardness.py $(DATA_REPORTS_DIR)/blocks.json src/generated/generated_block_hardness.c src/generated/generated_block_hardness.h

src/generated/generated_mining_data.c src/generated/generated_mining_data.h: tools/gen_mining_data.py tools/attached_reference.py tests/fixtures/vanilla-26.1.1-attached.tsv $(DATA_REPORTS_DIR)/blocks.json src/generated/generated_minecraft_ids.json $(MC_SERVER_JAR_SOURCE)
	@mkdir -p src/generated
	python3 tools/gen_mining_data.py $(DATA_REPORTS_DIR)/blocks.json src/generated/generated_minecraft_ids.json $(MC_SERVER_JAR_SOURCE) src/generated/generated_mining_data.c src/generated/generated_mining_data.h

src/generated/generated_item_food.c src/generated/generated_item_food.h: tools/gen_item_food.py src/generated/generated_minecraft_ids.json $(wildcard $(DATA_REPORTS_DIR)/minecraft/components/item/*.json)
	@mkdir -p src/generated
	python3 tools/gen_item_food.py src/generated/generated_minecraft_ids.json $(DATA_REPORTS_DIR)/minecraft/components/item src/generated/generated_item_food.c src/generated/generated_item_food.h

src/generated/generated_crafting_recipes.c src/generated/generated_crafting_recipes.h: tools/gen_crafting_recipes.py src/generated/generated_minecraft_ids.json $(wildcard $(MC_RECIPE_SOURCE)/*.json) $(wildcard $(MC_ITEM_TAG_SOURCE)/*.json)
	@mkdir -p src/generated
	python3 tools/gen_crafting_recipes.py src/generated/generated_minecraft_ids.json $(MC_RECIPE_SOURCE) $(MC_ITEM_TAG_SOURCE) src/generated/generated_crafting_recipes.c src/generated/generated_crafting_recipes.h

src/generated/generated_cooking_recipes.c src/generated/generated_cooking_recipes.h: tools/gen_cooking_recipes.py src/generated/generated_minecraft_ids.json $(wildcard $(MC_RECIPE_SOURCE)/*.json) $(wildcard $(MC_ITEM_TAG_SOURCE)/*.json)
	@mkdir -p src/generated
	python3 tools/gen_cooking_recipes.py src/generated/generated_minecraft_ids.json $(MC_RECIPE_SOURCE) $(MC_ITEM_TAG_SOURCE) src/generated/generated_cooking_recipes.c src/generated/generated_cooking_recipes.h

src/generated/generated_item_place.c src/generated/generated_item_place.h: tools/gen_item_place_map.py src/generated/generated_minecraft_ids.json $(DATA_REPORTS_DIR)/blocks.json
	@mkdir -p src/generated
	python3 tools/gen_item_place_map.py src/generated/generated_minecraft_ids.json $(DATA_REPORTS_DIR)/blocks.json src/generated/generated_item_place.c src/generated/generated_item_place.h
else
$(GENERATED_HEADERS) $(GENERATED_SOURCES):
	@test -f "$@" || { echo "missing generated file: $@; run make regenerate with local raw data" >&2; exit 1; }
endif

.PHONY: generated_headers generated_sources regenerate test generated-check hygiene-check smoke-start multi-epoll-check login-integration-check precommit-check dev-run clean distclean

generated_headers: $(GENERATED_HEADERS)

generated_sources: $(GENERATED_HEADERS) $(GENERATED_SOURCES)

regenerate:
	$(MAKE) -B GENERATE=1 generated_sources

$(OBJ): $(GENERATED_HEADERS)

src/protocol/handlers/play.o: src/generated/generated_registries.h src/generated/generated_block_loot.h src/generated/generated_block_hardness.h src/generated/generated_mining_data.h src/generated/generated_item_food.h src/generated/generated_crafting_recipes.h src/generated/generated_cooking_recipes.h src/generated/generated_item_place.h src/generated/generated_minecraft_ids.h
src/protocol/inventory.o: src/generated/generated_minecraft_ids.h
src/net/server.o: src/generated/generated_minecraft_ids.h
src/gameplay/block_drops.o: src/generated/generated_block_loot.h
src/gameplay/crafting.o: src/generated/generated_crafting_recipes.h
src/gameplay/furnace.o: src/generated/generated_cooking_recipes.h
src/gameplay/mining.o: src/generated/generated_block_hardness.h src/generated/generated_mining_data.h

mc_server: generated_headers generated_sources $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

mc_recorder: tools/mc_recorder.c src/net/buffer.c src/protocol/varint.c src/protocol/framing.c src/protocol/crypto_stub.c src/util/mc_util.c src/generated/generated_minecraft_ids.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

mc_anvil_dump: tools/mc_anvil_dump.c src/world/anvil.c src/world/region_storage.c src/util/atomic_file.c src/world/nbt.c src/util/arena.c src/util/mc_util.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

mc_gen_bench: tools/mc_gen_bench.c src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/block_state.c src/world/chunk_store.c src/world/chunk.c src/world/anvil.c src/world/region_storage.c src/util/atomic_file.c src/world/nbt.c src/world/packed.c src/world/paletted_container.c src/world/block_registry.c src/util/arena.c src/util/mc_util.c src/generated/generated_registries.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_varint: tests/test_varint.c src/protocol/varint.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_login: tests/test_login.c src/protocol/handlers/login.c src/protocol/offline_uuid.c src/protocol/profile_wire.c src/protocol/varint.c src/protocol/auth_stub.c src/protocol/crypto_stub.c src/net/buffer.c src/util/mc_util.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_profile: tests/test_profile.c src/protocol/profile_json.c src/protocol/profile_service.c src/protocol/profile_wire.c src/protocol/varint.c src/util/mc_util.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_auth: tests/test_auth.c src/protocol/auth_service.c src/protocol/profile_json.c src/util/mc_util.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lcrypto

test_login_online: tests/test_login_online.c src/protocol/handlers/login.c src/protocol/offline_uuid.c src/protocol/profile_wire.c src/protocol/varint.c src/protocol/auth_service.c src/protocol/profile_json.c src/protocol/crypto_openssl.c src/net/buffer.c src/util/mc_util.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lcrypto

test_framing_compression: tests/test_framing_compression.c src/net/buffer.c src/protocol/varint.c src/protocol/framing.c src/protocol/crypto_stub.c src/util/mc_util.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_framing_crypto: tests/test_framing_crypto.c src/net/buffer.c src/protocol/varint.c src/protocol/framing.c src/protocol/crypto_openssl.c src/util/mc_util.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lcrypto

test_pvp: src/protocol/offline_uuid.c src/protocol/profile_wire.c

test_pvp: tests/test_pvp.c src/protocol/handlers/play.c src/gameplay/placement.c src/world/block_state.c src/generated/generated_block_ids.c src/protocol/inventory.c src/net/buffer.c src/protocol/varint.c src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/chunk_store.c src/world/chunk.c src/world/container_store.c src/world/anvil.c src/world/region_storage.c src/util/atomic_file.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/world/player_store.c src/gameplay/block_drops.c src/gameplay/crafting.c src/gameplay/furnace.c src/gameplay/mining.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_block_loot.c src/generated/generated_block_hardness.c src/generated/generated_mining_data.c src/generated/generated_item_food.c src/generated/generated_crafting_recipes.c src/generated/generated_cooking_recipes.c src/generated/generated_item_place.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -Wl,--wrap=fsync

test_nbt: tests/test_nbt.c src/world/nbt.c src/util/arena.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_nbt_arena: tests/test_nbt_arena.c src/world/nbt.c src/util/arena.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_stairs: tests/test_stairs.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/world/block_state.c src/world/block_registry.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_block_registry: tests/test_block_registry.c src/world/block_registry.c src/world/block_state.c src/gameplay/placement.c src/generated/generated_block_ids.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_block_entity_store: tests/test_block_entity_store.c src/world/block_entity_store.c src/protocol/inventory.c src/protocol/varint.c src/world/nbt.c src/util/arena.c src/generated/generated_minecraft_ids.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_paletted_container: tests/test_paletted_container.c src/world/paletted_container.c src/world/block_registry.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_chunk_network_codec: src/protocol/offline_uuid.c src/protocol/profile_wire.c
test_anvil_roundtrip: src/protocol/offline_uuid.c src/protocol/profile_wire.c

test_chunk_network_codec: tests/test_chunk_network_codec.c src/protocol/handlers/play.c src/gameplay/placement.c src/world/block_state.c src/generated/generated_block_ids.c src/protocol/inventory.c src/net/buffer.c src/protocol/varint.c src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/chunk_store.c src/world/chunk.c src/world/container_store.c src/world/anvil.c src/world/region_storage.c src/util/atomic_file.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/world/player_store.c src/gameplay/block_drops.c src/gameplay/crafting.c src/gameplay/furnace.c src/gameplay/mining.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_block_loot.c src/generated/generated_block_hardness.c src/generated/generated_mining_data.c src/generated/generated_item_food.c src/generated/generated_crafting_recipes.c src/generated/generated_cooking_recipes.c src/generated/generated_item_place.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_anvil_roundtrip: tests/test_anvil_roundtrip.c src/protocol/handlers/play.c src/gameplay/placement.c src/world/block_state.c src/generated/generated_block_ids.c src/protocol/inventory.c src/net/buffer.c src/protocol/varint.c src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/chunk_store.c src/world/chunk.c src/world/container_store.c src/world/anvil.c src/world/region_storage.c src/util/atomic_file.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/world/player_store.c src/gameplay/block_drops.c src/gameplay/crafting.c src/gameplay/furnace.c src/gameplay/mining.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_block_loot.c src/generated/generated_block_hardness.c src/generated/generated_mining_data.c src/generated/generated_item_food.c src/generated/generated_crafting_recipes.c src/generated/generated_cooking_recipes.c src/generated/generated_item_place.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_player_nbt: tests/test_player_nbt.c tests/fixtures/vanilla-26.1.1-player.dat src/protocol/inventory.c src/protocol/varint.c src/world/player_store.c src/world/nbt.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDFLAGS) -Wl,--wrap=rename

test_player_save: tests/test_player_save.c src/protocol/inventory.c src/protocol/varint.c src/world/player_store.c src/world/nbt.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -Wl,--wrap=rename,--wrap=fsync,--wrap=gzwrite

test_memory: tests/test_memory.c src/util/memory.c src/util/mc_util.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_mining: tests/test_mining.c src/gameplay/mining.c src/generated/generated_block_hardness.c src/generated/generated_mining_data.c src/generated/generated_minecraft_ids.c src/world/block_registry.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_block_drops: tests/test_block_drops.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/world/block_state.c src/gameplay/block_drops.c src/gameplay/mining.c src/generated/generated_block_loot.c src/generated/generated_block_hardness.c src/generated/generated_mining_data.c src/generated/generated_minecraft_ids.c src/world/block_registry.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_world_consistency: tests/test_world_consistency.c src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/block_state.c src/world/chunk_store.c src/world/chunk.c src/world/anvil.c src/world/region_storage.c src/util/atomic_file.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/gameplay/furnace.c src/protocol/inventory.c src/protocol/varint.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_cooking_recipes.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_setup: tests/test_setup.c src/util/setup.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_task_queue: tests/test_task_queue.c src/net/task_queue.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_perf_config: tests/test_perf_config.c src/util/mc_util.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: $(TEST_BINS)
	@set -e; \
	for test_bin in $(TEST_BINS); do \
		echo "[test] $$test_bin"; \
		./$$test_bin; \
	done

generated-check:
	tools/check_generated.sh

hygiene-check:
	tools/check_repo_hygiene.sh

smoke-start: mc_server
	@world_path="$${MC_WORLD_PATH:-/tmp/mc_c_server_smoke_world}"; \
	port="$${MC_BIND_PORT:-25566}"; \
	timeout_s="$${SMOKE_TIMEOUT:-2}"; \
	echo "[smoke] starting ./mc_server for $${timeout_s}s on port $${port} with world $${world_path}"; \
	status=0; \
	MC_WORLD_PATH="$$world_path" MC_BIND_PORT="$$port" timeout "$$timeout_s" ./mc_server || status=$$?; \
	if [ "$$status" -eq 124 ]; then \
		echo "[smoke] server stayed up until timeout"; \
		exit 0; \
	fi; \
	exit "$$status"

multi-epoll-check: mc_server
	python3 tests/test_multi_epoll.py

login-integration-check: mc_server
	python3 tests/test_login_integration.py

precommit-check: hygiene-check generated-check test smoke-start multi-epoll-check login-integration-check migration-check
	python3 tests/test_registry_generator.py
	python3 tests/stress/test_stress.py
	python3 tests/stress/test_tick_profile.py
	python3 tests/stress/test_io_probe.py

dev-run:
	$(MAKE) clean
	$(MAKE) precommit-check
	@if command -v clear >/dev/null 2>&1; then clear || true; fi
	./mc_server

clean:
	rm -f mc_server $(TOOL_BINS) $(TEST_BINS) $(OBJ) $(OBJ:.o=.d) $(TEST_BINS:=.d)
	find src tools tests -name '*.o' -delete
	find src tools tests -name '*.d' -delete
	rm -f *.o *.d

distclean: clean
	rm -rf /tmp/mc_c_server_smoke_world

-include $(OBJ:.o=.d)

# Isolated remote persistence microbenchmark (no server listener).
mc_save_bench: tools/mc_save_bench.c src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/block_state.c src/world/chunk_store.c src/world/chunk.c src/world/anvil.c src/world/region_storage.c src/util/atomic_file.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/gameplay/furnace.c src/protocol/inventory.c src/protocol/varint.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_cooking_recipes.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

mc_trace_io: tools/mc_trace_io.c
	$(CC) $(CFLAGS) -o $@ $<

test_atomic_persistence: tests/test_atomic_persistence.c src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/block_state.c src/world/chunk_store.c src/world/chunk.c src/world/anvil.c src/world/region_storage.c src/util/atomic_file.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/gameplay/furnace.c src/protocol/inventory.c src/protocol/varint.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_cooking_recipes.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -Wl,--wrap=fread,--wrap=__fread_chk,--wrap=openat,--wrap=write,--wrap=pwrite,--wrap=pread,--wrap=fsync,--wrap=renameat,--wrap=close,--wrap=fstat,--wrap=flock,--wrap=calloc

test_region_storage: tests/test_region_storage.c src/world/region_storage.c src/util/atomic_file.c src/util/mc_util.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

mc_region_bench: tools/mc_region_bench.c src/world/region_storage.c src/world/anvil.c src/util/atomic_file.c src/world/nbt.c src/util/arena.c src/util/mc_util.c | generated_sources
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $^ $(LDFLAGS)

# Experimental only: intentionally absent from SRC and mc_server.
REGION_TXN_SRC = src/world/region_txn.c src/world/region_storage.c src/world/anvil.c src/util/atomic_file.c src/world/nbt.c src/util/arena.c src/util/mc_util.c
mc_region_txn: tools/mc_region_txn.c $(REGION_TXN_SRC) | generated_sources
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $^ $(LDFLAGS) -lcrypto

test_region_txn: tests/test_region_txn.c $(REGION_TXN_SRC) | generated_sources
	$(CC) $(CFLAGS) -DMC_REGION_TXN_TESTING -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--wrap=write,--wrap=pwrite,--wrap=fsync,--wrap=unlinkat,--wrap=ftruncate,--wrap=realloc,--wrap=close,--wrap=renameat -o $@ $^ $(LDFLAGS) -lcrypto

test_native_storage: tests/test_native_storage.c src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/block_state.c src/world/chunk_store.c src/world/chunk.c src/world/anvil.c src/world/region_storage.c src/util/atomic_file.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/gameplay/furnace.c src/protocol/inventory.c src/protocol/varint.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_cooking_recipes.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -Wl,--wrap=fsync

mc_native_bench: tools/mc_native_bench.c src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/block_state.c src/world/chunk_store.c src/world/chunk.c src/world/anvil.c src/world/region_storage.c src/util/atomic_file.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/gameplay/furnace.c src/protocol/inventory.c src/protocol/varint.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_cooking_recipes.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_save_pipeline: tests/test_save_pipeline.c src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/block_state.c src/world/chunk_store.c src/world/chunk.c src/world/anvil.c src/world/region_storage.c src/util/atomic_file.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/gameplay/furnace.c src/protocol/inventory.c src/protocol/varint.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_cooking_recipes.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -Wl,--wrap=fsync,--wrap=malloc,--wrap=calloc

# Offline migration only. No migration code or fault hook is linked into SRC.
MIGRATION_SRC = src/world/chunk_migration.c src/world/anvil.c src/world/nbt.c src/world/packed.c src/world/chunk.c src/world/paletted_container.c src/world/block_registry.c src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/region_storage.c src/util/atomic_file.c src/util/arena.c src/util/mc_util.c src/world/block_entity_store.c src/protocol/inventory.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c
mc_world_migrate: tools/mc_world_migrate.c $(MIGRATION_SRC) | generated_sources
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $^ $(LDFLAGS) -lcrypto

test_chunk_migration: tests/test_chunk_migration.c $(MIGRATION_SRC) | generated_sources
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $^ $(LDFLAGS) -lcrypto

mc_migrate_fault: tools/mc_world_migrate.c tests/migration_fault.c $(MIGRATION_SRC) | generated_sources
	$(CC) $(CFLAGS) -DMC_MIGRATION_TESTING -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--wrap=pread,--wrap=write,--wrap=fsync,--wrap=mkdirat,--wrap=renameat2 -o $@ $^ $(LDFLAGS) -lcrypto

.PHONY: migration-check
migration-check: mc_world_migrate mc_migrate_fault
	python3 tests/test_world_migration.py --tool ./mc_world_migrate --fault-tool ./mc_migrate_fault
	python3 tests/test_migration_mounts.py --tool ./mc_world_migrate

# G1 append-only tools stay separate; G2 reuses their SQLite/codec components.
G1_CPPFLAGS ?=
G1_LIBS = -l:liblz4.so.1 -l:libzstd.so.1
G1_SRC = src/experimental/goatworld_pack.c src/experimental/goatworld.c src/experimental/goatworld_codec.c src/experimental/goatworld_sqlite.c src/util/atomic_file.c src/world/region_storage.c src/world/anvil.c src/world/nbt.c src/world/packed.c src/world/chunk.c src/world/paletted_container.c src/world/block_registry.c src/world/block_entity_store.c src/protocol/inventory.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c
mc_goatworld: tools/mc_goatworld.c $(G1_SRC) | generated_sources
	$(CC) $(CFLAGS) $(G1_CPPFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $^ $(LDFLAGS) -lcrypto $(G1_LIBS)
mc_goatworld_fault: tools/mc_goatworld.c tests/goatworld_fault.c $(G1_SRC) | generated_sources
	$(CC) $(CFLAGS) $(G1_CPPFLAGS) -DMC_GOATWORLD_TESTING -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $^ $(LDFLAGS) -lcrypto $(G1_LIBS)
test_goatworld: tests/test_goatworld.c $(G1_SRC) | generated_sources
	$(CC) $(CFLAGS) $(G1_CPPFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $^ $(LDFLAGS) -lcrypto $(G1_LIBS)
.PHONY: goatworld-check
goatworld-check: mc_goatworld mc_goatworld_fault test_goatworld
	./test_goatworld
	python3 tests/test_goatworld.py --tool ./mc_goatworld --fault-tool ./mc_goatworld_fault
ifeq ($(GOATWORLD_G1_CHECK),1)
precommit-check: goatworld-check
endif

.PHONY: storage-deps
storage-deps: $(SQLITE_LIB)
$(STORAGE_SDK)/sqlite3.c $(STORAGE_SDK)/include/sqlite3.h $(STORAGE_SDK)/include/lz4.h $(STORAGE_SDK)/include/zstd.h &: tools/prepare_storage_deps.py
	python3 tools/prepare_storage_deps.py
$(SQLITE_LIB): $(STORAGE_SDK)/sqlite3.c $(STORAGE_SDK)/include/sqlite3.h
	$(CC) $(SQLITE_CFLAGS) -c $(STORAGE_SDK)/sqlite3.c -o $(STORAGE_SDK)/sqlite3.o
	ar rcs $@ $(STORAGE_SDK)/sqlite3.o
$(OBJ) $(TEST_BINS) $(TOOL_BINS) mc_server: | storage-deps
$(TEST_BINS) $(TOOL_BINS) mc_server: $(SQLITE_LIB)

test_goatworld_storage: tests/test_goatworld_storage.c $(MIGRATION_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/block_state.c src/world/chunk_store.c src/gameplay/furnace.c src/protocol/varint.c src/generated/generated_cooking_recipes.c
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $^ $(LDFLAGS)

test_goatworld_storage_fault: tests/test_goatworld_storage.c $(MIGRATION_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/block_state.c src/world/chunk_store.c src/gameplay/furnace.c src/protocol/varint.c src/generated/generated_cooking_recipes.c tests/goatworld_fault.c
	$(CC) $(CFLAGS) -DMC_GOATWORLD_TESTING -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $^ $(LDFLAGS)

.PHONY: integrated-goatworld-fault-check
integrated-goatworld-fault-check: test_goatworld_storage test_goatworld_storage_fault
	python3 tests/test_goatworld_integrated.py --tool ./test_goatworld_storage --fault-tool ./test_goatworld_storage_fault
precommit-check: integrated-goatworld-fault-check
test_goatworld_storage test_goatworld_storage_fault: | storage-deps

# Deterministic hooks only in this disposable integration binary.
mc_server_g2_fault: $(SRC) tests/goatworld_fault.c | generated_sources storage-deps
	$(CC) $(CFLAGS) -DMC_GOATWORLD_TESTING -o $@ $^ $(LDFLAGS)

test_hinged_blocks: tests/test_hinged_blocks.c src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/block_state.c src/world/chunk_store.c src/world/chunk.c src/world/anvil.c src/world/region_storage.c src/util/atomic_file.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/gameplay/furnace.c src/protocol/inventory.c src/protocol/varint.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_cooking_recipes.c src/gameplay/block_drops.c src/generated/generated_block_loot.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -Wl,--wrap=malloc,--wrap=calloc

test_attached_blocks: tests/test_attached_blocks.c src/world/world.c src/world/world_storage.c $(GOATWORLD_SRC) src/world/save_pipeline.c src/gameplay/block_neighbors.c src/gameplay/hinged_blocks.c src/gameplay/attached_blocks.c src/gameplay/hinged_world.c src/gameplay/attached_world.c src/world/block_state.c src/world/chunk_store.c src/world/chunk.c src/world/anvil.c src/world/region_storage.c src/util/atomic_file.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/gameplay/furnace.c src/protocol/inventory.c src/protocol/varint.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_cooking_recipes.c src/gameplay/block_drops.c src/generated/generated_block_loot.c src/gameplay/mining.c src/generated/generated_block_hardness.c src/generated/generated_mining_data.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -Wl,--wrap=realloc
