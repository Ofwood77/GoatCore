CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Iinclude -Isrc/generated -Isrc/world -Isrc/util -D_POSIX_C_SOURCE=200809L -MMD -MP -pthread
LDFLAGS ?= -lz -pthread

USE_OPENSSL ?= 0
ifeq ($(USE_OPENSSL),1)
    CFLAGS += -DMC_USE_OPENSSL
    LDFLAGS += -lssl -lcrypto
    CRYPTO_SRC = src/protocol/crypto_openssl.c
else
    CRYPTO_SRC = src/protocol/crypto_stub.c
endif

SRC = \
    src/main.c \
    src/net/server.c \
    src/net/buffer.c \
    src/net/task_queue.c \
    src/gameplay/block_drops.c \
    src/gameplay/crafting.c \
    src/gameplay/furnace.c \
    src/gameplay/mining.c \
    src/protocol/varint.c \
    src/protocol/inventory.c \
    src/protocol/framing.c \
    $(CRYPTO_SRC) \
    src/protocol/handlers/handshake.c \
    src/protocol/handlers/login.c \
    src/protocol/handlers/configuration.c \
    src/protocol/handlers/play.c \
    src/protocol/handlers/status.c \
    src/world/chunk_store.c \
    src/world/chunk.c \
    src/world/container_store.c \
    src/world/anvil.c \
    src/world/nbt.c \
    src/world/packed.c \
    src/world/block_entity_store.c \
    src/world/paletted_container.c \
    src/world/player_store.c \
    src/world/block_registry.c \
    src/world/world.c \
    src/util/arena.c \
    src/util/mc_util.c \
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

TOOL_BINS = mc_recorder mc_anvil_dump mc_gen_bench

TEST_BINS = \
    test_varint \
    test_nbt \
    test_nbt_arena \
    test_block_registry \
    test_block_entity_store \
    test_paletted_container \
    test_chunk_network_codec \
    test_anvil_roundtrip \
    test_player_nbt \
    test_mining \
    test_block_drops \
    test_world_consistency

GENERATED_HEADERS = \
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

src/generated/generated_block_hardness.c src/generated/generated_block_hardness.h: tools/gen_block_hardness.py $(DATA_REPORTS_DIR)/blocks.json
	@mkdir -p src/generated
	python3 tools/gen_block_hardness.py $(DATA_REPORTS_DIR)/blocks.json src/generated/generated_block_hardness.c src/generated/generated_block_hardness.h

src/generated/generated_mining_data.c src/generated/generated_mining_data.h: tools/gen_mining_data.py $(DATA_REPORTS_DIR)/blocks.json src/generated/generated_minecraft_ids.json $(MC_SERVER_JAR_SOURCE)
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

.PHONY: generated_headers generated_sources regenerate test generated-check hygiene-check smoke-start precommit-check dev-run clean distclean

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

mc_anvil_dump: tools/mc_anvil_dump.c src/world/anvil.c src/world/nbt.c src/util/arena.c src/util/mc_util.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

mc_gen_bench: tools/mc_gen_bench.c src/world/world.c src/world/chunk_store.c src/world/chunk.c src/world/anvil.c src/world/nbt.c src/world/packed.c src/world/paletted_container.c src/world/block_registry.c src/util/arena.c src/util/mc_util.c src/generated/generated_registries.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_varint: tests/test_varint.c src/protocol/varint.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_nbt: tests/test_nbt.c src/world/nbt.c src/util/arena.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_nbt_arena: tests/test_nbt_arena.c src/world/nbt.c src/util/arena.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_block_registry: tests/test_block_registry.c src/world/block_registry.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_block_entity_store: tests/test_block_entity_store.c src/world/block_entity_store.c src/protocol/inventory.c src/protocol/varint.c src/generated/generated_minecraft_ids.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_paletted_container: tests/test_paletted_container.c src/world/paletted_container.c src/world/block_registry.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_chunk_network_codec: tests/test_chunk_network_codec.c src/protocol/handlers/play.c src/protocol/inventory.c src/net/buffer.c src/protocol/varint.c src/world/world.c src/world/chunk_store.c src/world/chunk.c src/world/container_store.c src/world/anvil.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/world/player_store.c src/gameplay/block_drops.c src/gameplay/crafting.c src/gameplay/furnace.c src/gameplay/mining.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_block_loot.c src/generated/generated_block_hardness.c src/generated/generated_mining_data.c src/generated/generated_item_food.c src/generated/generated_crafting_recipes.c src/generated/generated_cooking_recipes.c src/generated/generated_item_place.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_anvil_roundtrip: tests/test_anvil_roundtrip.c src/protocol/handlers/play.c src/protocol/inventory.c src/net/buffer.c src/protocol/varint.c src/world/world.c src/world/chunk_store.c src/world/chunk.c src/world/container_store.c src/world/anvil.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/world/player_store.c src/gameplay/block_drops.c src/gameplay/crafting.c src/gameplay/furnace.c src/gameplay/mining.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_block_loot.c src/generated/generated_block_hardness.c src/generated/generated_mining_data.c src/generated/generated_item_food.c src/generated/generated_crafting_recipes.c src/generated/generated_cooking_recipes.c src/generated/generated_item_place.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_player_nbt: tests/test_player_nbt.c src/protocol/inventory.c src/protocol/varint.c src/world/player_store.c src/world/nbt.c src/util/arena.c src/generated/generated_minecraft_ids.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_mining: tests/test_mining.c src/gameplay/mining.c src/generated/generated_block_hardness.c src/generated/generated_mining_data.c src/generated/generated_minecraft_ids.c src/world/block_registry.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_block_drops: tests/test_block_drops.c src/gameplay/block_drops.c src/gameplay/mining.c src/generated/generated_block_loot.c src/generated/generated_block_hardness.c src/generated/generated_mining_data.c src/generated/generated_minecraft_ids.c src/world/block_registry.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_world_consistency: tests/test_world_consistency.c src/world/world.c src/world/chunk_store.c src/world/chunk.c src/world/anvil.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/gameplay/furnace.c src/protocol/inventory.c src/protocol/varint.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_cooking_recipes.c | generated_sources
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

precommit-check: hygiene-check generated-check test smoke-start

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
