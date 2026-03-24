CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Iinclude -Isrc/generated -D_POSIX_C_SOURCE=200809L -MMD -MP -pthread
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
    src/world/container_store.c \
    src/world/anvil.c \
    src/world/nbt.c \
    src/world/packed.c \
    src/world/player_store.c \
    src/world/world.c \
    src/util/mc_util.c \
    src/generated/generated_minecraft_ids.c \
    src/generated/generated_registries.c \
    src/generated/generated_item_place.c

OBJ = $(SRC:.c=.o)

all: mc_server

MC_CLIENT_ROOT ?= mc_vania_asset

src/generated/generated_minecraft_ids.c src/generated/generated_minecraft_ids.h src/generated/generated_minecraft_ids.json: \
	tools/gen_minecraft_ids.py \
	$(MC_CLIENT_ROOT)/net/minecraft/network/protocol/game/GameProtocols.class \
	$(MC_CLIENT_ROOT)/net/minecraft/network/protocol/configuration/ConfigurationProtocols.class \
	$(MC_CLIENT_ROOT)/net/minecraft/network/protocol/login/LoginProtocols.class \
	$(MC_CLIENT_ROOT)/net/minecraft/network/protocol/status/StatusProtocols.class \
	$(MC_CLIENT_ROOT)/net/minecraft/network/protocol/handshake/HandshakeProtocols.class \
	$(MC_CLIENT_ROOT)/net/minecraft/world/entity/EntityType.class \
	$(MC_CLIENT_ROOT)/net/minecraft/world/level/block/entity/BlockEntityType.class \
	$(MC_CLIENT_ROOT)/net/minecraft/world/item/Items.class
	@mkdir -p src/generated
	python3 tools/gen_minecraft_ids.py $(MC_CLIENT_ROOT) src/generated/generated_minecraft_ids.c src/generated/generated_minecraft_ids.h src/generated/generated_minecraft_ids.json

src/generated/generated_registries.c src/generated/generated_registries.h: tools/gen_registries.py assets/block_states.json
	@mkdir -p src/generated
	python3 tools/gen_registries.py assets/block_states.json src/generated/generated_registries.c src/generated/generated_registries.h

src/generated/generated_item_place.c src/generated/generated_item_place.h: tools/gen_item_place_map.py src/generated/generated_minecraft_ids.json assets/blocks.json
	@mkdir -p src/generated
	python3 tools/gen_item_place_map.py src/generated/generated_minecraft_ids.json assets/blocks.json src/generated/generated_item_place.c src/generated/generated_item_place.h

src/protocol/handlers/play.o: src/generated/generated_registries.h src/generated/generated_item_place.h src/generated/generated_minecraft_ids.h
src/protocol/inventory.o: src/generated/generated_minecraft_ids.h
src/net/server.o: src/generated/generated_minecraft_ids.h

mc_server: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

mc_recorder: tools/mc_recorder.c src/net/buffer.c src/protocol/varint.c src/protocol/framing.c src/protocol/crypto_stub.c src/util/mc_util.c src/generated/generated_minecraft_ids.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

mc_anvil_dump: tools/mc_anvil_dump.c src/world/anvil.c src/world/nbt.c src/util/mc_util.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

mc_gen_bench: tools/mc_gen_bench.c src/world/world.c src/world/chunk_store.c src/world/anvil.c src/world/nbt.c src/world/packed.c src/util/mc_util.c src/generated/generated_registries.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_varint: tests/test_varint.c src/protocol/varint.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_nbt: tests/test_nbt.c src/world/nbt.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_anvil_roundtrip: tests/test_anvil_roundtrip.c src/protocol/handlers/play.c src/protocol/inventory.c src/net/buffer.c src/protocol/varint.c src/world/world.c src/world/chunk_store.c src/world/container_store.c src/world/anvil.c src/world/nbt.c src/world/packed.c src/world/player_store.c src/util/mc_util.c src/generated/generated_minecraft_ids.c src/generated/generated_registries.c src/generated/generated_item_place.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f mc_server mc_recorder mc_anvil_dump mc_gen_bench test_varint test_nbt test_anvil_roundtrip $(OBJ) $(OBJ:.o=.d) src/generated/generated_minecraft_ids.c src/generated/generated_minecraft_ids.h src/generated/generated_minecraft_ids.json src/generated/generated_registries.c src/generated/generated_registries.h src/generated/generated_item_place.c src/generated/generated_item_place.h

-include $(OBJ:.o=.d)
