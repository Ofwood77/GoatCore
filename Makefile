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
    src/generated/generated_item_place.c

OBJ = $(SRC:.c=.o)

GENERATED_HEADERS = \
    src/world/block_registry.h \
    src/generated/generated_minecraft_ids.h \
    src/generated/generated_registries.h \
    src/generated/generated_item_place.h

GENERATED_SOURCES = \
    src/world/block_registry.c

all: mc_server

MC_CLIENT_ROOT ?= mc_vania_asset/client
MC_IDS_SENTINEL ?= $(MC_CLIENT_ROOT)/net/minecraft/network/protocol/handshake/HandshakeProtocols.class

src/world/block_registry.c src/world/block_registry.h: tools/generate_registry.py data/26.1-rc-3/reports/blocks.json
	python3 tools/generate_registry.py data/26.1-rc-3/reports/blocks.json src/world/block_registry.c src/world/block_registry.h

src/generated/generated_minecraft_ids.c src/generated/generated_minecraft_ids.h src/generated/generated_minecraft_ids.json: \
	tools/gen_minecraft_ids.py \
	$(MC_CLIENT_ROOT)
	@mkdir -p src/generated
	@if [ -f "$(MC_IDS_SENTINEL)" ]; then \
		python3 tools/gen_minecraft_ids.py $(MC_CLIENT_ROOT) src/generated/generated_minecraft_ids.c src/generated/generated_minecraft_ids.h src/generated/generated_minecraft_ids.json; \
	else \
		test -f src/generated/generated_minecraft_ids.c && test -f src/generated/generated_minecraft_ids.h && test -f src/generated/generated_minecraft_ids.json || \
			( echo "missing generated minecraft id files and no class extraction available under $(MC_CLIENT_ROOT)" >&2; exit 1 ); \
		echo "using checked-in generated_minecraft_ids.* (no class extraction under $(MC_CLIENT_ROOT))"; \
		touch src/generated/generated_minecraft_ids.c src/generated/generated_minecraft_ids.h src/generated/generated_minecraft_ids.json; \
	fi

src/generated/generated_registries.c src/generated/generated_registries.h: tools/gen_registries.py assets/block_states.json
	@mkdir -p src/generated
	python3 tools/gen_registries.py assets/block_states.json src/generated/generated_registries.c src/generated/generated_registries.h

src/generated/generated_item_place.c src/generated/generated_item_place.h: tools/gen_item_place_map.py src/generated/generated_minecraft_ids.json assets/blocks.json
	@mkdir -p src/generated
	python3 tools/gen_item_place_map.py src/generated/generated_minecraft_ids.json assets/blocks.json src/generated/generated_item_place.c src/generated/generated_item_place.h

generated_headers: $(GENERATED_HEADERS)

generated_sources: $(GENERATED_SOURCES)

$(OBJ): generated_headers

src/protocol/handlers/play.o: src/generated/generated_registries.h src/generated/generated_item_place.h src/generated/generated_minecraft_ids.h
src/protocol/inventory.o: src/generated/generated_minecraft_ids.h
src/net/server.o: src/generated/generated_minecraft_ids.h

mc_server: generated_sources $(OBJ)
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

test_block_entity_store: tests/test_block_entity_store.c src/world/block_entity_store.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_paletted_container: tests/test_paletted_container.c src/world/paletted_container.c src/world/block_registry.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_chunk_network_codec: tests/test_chunk_network_codec.c src/protocol/handlers/play.c src/protocol/inventory.c src/net/buffer.c src/protocol/varint.c src/world/world.c src/world/chunk_store.c src/world/chunk.c src/world/container_store.c src/world/anvil.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/world/player_store.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_item_place.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_anvil_roundtrip: tests/test_anvil_roundtrip.c src/protocol/handlers/play.c src/protocol/inventory.c src/net/buffer.c src/protocol/varint.c src/world/world.c src/world/chunk_store.c src/world/chunk.c src/world/container_store.c src/world/anvil.c src/world/nbt.c src/world/packed.c src/world/block_entity_store.c src/world/paletted_container.c src/world/block_registry.c src/world/player_store.c src/util/arena.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_item_place.c | generated_sources
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f mc_server mc_recorder mc_anvil_dump mc_gen_bench test_varint test_nbt test_nbt_arena test_block_registry test_block_entity_store test_paletted_container test_chunk_network_codec test_anvil_roundtrip $(OBJ) $(OBJ:.o=.d) src/world/block_registry.c src/world/block_registry.h

-include $(OBJ:.o=.d)
