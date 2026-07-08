"use strict";

const fs = require("fs");
const path = require("path");
const Module = require("module");

const TARGET_PROTOCOL = 775;
const TARGET_MAJOR = "26.1";
const COMPAT_MAJOR = "1.21";
const BASE_DATA_VERSION = "1.21.11";

const STATE_MAP = {
  handshake: "handshaking",
  status: "status",
  login: "login",
  configuration: "configuration",
  play: "play"
};

const DIRECTION_MAP = {
  serverbound: "toServer",
  clientbound: "toClient"
};

const ALIASES = {
  "handshake:serverbound:intention": "set_protocol",

  "status:serverbound:status_request": "ping_start",
  "status:serverbound:ping_request": "ping",
  "status:clientbound:status_response": "server_info",
  "status:clientbound:pong_response": "ping",

  "login:serverbound:hello": "login_start",
  "login:serverbound:key": "encryption_begin",
  "login:serverbound:custom_query_answer": "login_plugin_response",
  "login:clientbound:login_disconnect": "disconnect",
  "login:clientbound:hello": "encryption_begin",
  "login:clientbound:login_finished": "success",
  "login:clientbound:login_compression": "compress",
  "login:clientbound:custom_query": "login_plugin_request",

  "configuration:serverbound:client_information": "settings",
  "configuration:serverbound:resource_pack": "resource_pack_receive",
  "configuration:clientbound:resource_pack_pop": "remove_resource_pack",
  "configuration:clientbound:resource_pack_push": "add_resource_pack",
  "configuration:clientbound:update_enabled_features": "feature_flags",
  "configuration:clientbound:update_tags": "tags",

  "play:clientbound:add_entity": "spawn_entity",
  "play:clientbound:animate": "animation",
  "play:clientbound:award_stats": "statistics",
  "play:clientbound:block_changed_ack": "acknowledge_player_digging",
  "play:clientbound:block_destruction": "block_break_animation",
  "play:clientbound:block_entity_data": "tile_entity_data",
  "play:clientbound:block_event": "block_action",
  "play:clientbound:block_update": "block_change",
  "play:clientbound:boss_event": "boss_bar",
  "play:clientbound:change_difficulty": "difficulty",
  "play:clientbound:chunks_biomes": "chunk_biomes",
  "play:clientbound:command_suggestions": "tab_complete",
  "play:clientbound:commands": "declare_commands",
  "play:clientbound:container_close": "close_window",
  "play:clientbound:container_set_content": "window_items",
  "play:clientbound:container_set_data": "craft_progress_bar",
  "play:clientbound:container_set_slot": "set_slot",
  "play:clientbound:cooldown": "set_cooldown",
  "play:clientbound:custom_chat_completions": "chat_suggestions",
  "play:clientbound:delete_chat": "hide_message",
  "play:clientbound:disconnect": "kick_disconnect",
  "play:clientbound:disguised_chat": "profileless_chat",
  "play:clientbound:entity_event": "entity_status",
  "play:clientbound:entity_position_sync": "sync_entity_position",
  "play:clientbound:explode": "explosion",
  "play:clientbound:forget_level_chunk": "unload_chunk",
  "play:clientbound:game_event": "game_state_change",
  "play:clientbound:initialize_border": "initialize_world_border",
  "play:clientbound:level_chunk_with_light": "map_chunk",
  "play:clientbound:level_event": "world_event",
  "play:clientbound:level_particles": "world_particles",
  "play:clientbound:light_update": "update_light",
  "play:clientbound:map_item_data": "map",
  "play:clientbound:merchant_offers": "trade_list",
  "play:clientbound:move_entity_pos": "rel_entity_move",
  "play:clientbound:move_entity_pos_rot": "entity_move_look",
  "play:clientbound:move_entity_rot": "entity_look",
  "play:clientbound:move_minecart_along_track": "move_minecart",
  "play:clientbound:move_vehicle": "vehicle_move",
  "play:clientbound:mount_screen_open": "open_horse_window",
  "play:clientbound:open_screen": "open_window",
  "play:clientbound:open_sign_editor": "open_sign_entity",
  "play:clientbound:place_ghost_recipe": "craft_recipe_response",
  "play:clientbound:player_abilities": "abilities",
  "play:clientbound:player_combat_end": "end_combat_event",
  "play:clientbound:player_combat_enter": "enter_combat_event",
  "play:clientbound:player_combat_kill": "death_combat_event",
  "play:clientbound:player_info_remove": "player_remove",
  "play:clientbound:player_info_update": "player_info",
  "play:clientbound:player_look_at": "face_player",
  "play:clientbound:player_position": "position",
  "play:clientbound:pong_response": "ping_response",
  "play:clientbound:projectile_power": "set_projectile_power",
  "play:clientbound:remove_entities": "entity_destroy",
  "play:clientbound:remove_mob_effect": "remove_entity_effect",
  "play:clientbound:resource_pack_pop": "remove_resource_pack",
  "play:clientbound:resource_pack_push": "add_resource_pack",
  "play:clientbound:rotate_head": "entity_head_rotation",
  "play:clientbound:section_blocks_update": "multi_block_change",
  "play:clientbound:select_advancements_tab": "select_advancement_tab",
  "play:clientbound:set_action_bar_text": "action_bar",
  "play:clientbound:set_border_center": "world_border_center",
  "play:clientbound:set_border_lerp_size": "world_border_lerp_size",
  "play:clientbound:set_border_size": "world_border_size",
  "play:clientbound:set_border_warning_delay": "world_border_warning_delay",
  "play:clientbound:set_border_warning_distance": "world_border_warning_reach",
  "play:clientbound:set_camera": "camera",
  "play:clientbound:set_chunk_cache_center": "update_view_position",
  "play:clientbound:set_chunk_cache_radius": "update_view_distance",
  "play:clientbound:set_default_spawn_position": "spawn_position",
  "play:clientbound:set_display_objective": "scoreboard_display_objective",
  "play:clientbound:set_entity_data": "entity_metadata",
  "play:clientbound:set_entity_link": "attach_entity",
  "play:clientbound:set_entity_motion": "entity_velocity",
  "play:clientbound:set_equipment": "entity_equipment",
  "play:clientbound:set_experience": "experience",
  "play:clientbound:set_health": "update_health",
  "play:clientbound:set_held_slot": "held_item_slot",
  "play:clientbound:set_objective": "scoreboard_objective",
  "play:clientbound:set_player_team": "teams",
  "play:clientbound:set_score": "scoreboard_score",
  "play:clientbound:set_simulation_distance": "simulation_distance",
  "play:clientbound:set_subtitle_text": "set_title_subtitle",
  "play:clientbound:set_time": "update_time",
  "play:clientbound:set_titles_animation": "set_title_time",
  "play:clientbound:sound": "sound_effect",
  "play:clientbound:sound_entity": "entity_sound_effect",
  "play:clientbound:tab_list": "playerlist_header",
  "play:clientbound:tag_query": "nbt_query_response",
  "play:clientbound:take_item_entity": "collect",
  "play:clientbound:teleport_entity": "entity_teleport",
  "play:clientbound:ticking_state": "set_ticking_state",
  "play:clientbound:ticking_step": "step_tick",
  "play:clientbound:update_advancements": "advancements",
  "play:clientbound:update_attributes": "entity_update_attributes",
  "play:clientbound:update_mob_effect": "entity_effect",
  "play:clientbound:update_recipes": "declare_recipes",
  "play:clientbound:update_tags": "tags",
  "play:clientbound:waypoint": "tracked_waypoint",

  "play:serverbound:accept_teleportation": "teleport_confirm",
  "play:serverbound:block_entity_tag_query": "query_block_nbt",
  "play:serverbound:bundle_item_selected": "select_bundle_item",
  "play:serverbound:change_difficulty": "set_difficulty",
  "play:serverbound:change_game_mode": "change_gamemode",
  "play:serverbound:chat": "chat_message",
  "play:serverbound:chat_ack": "message_acknowledgement",
  "play:serverbound:client_information": "settings",
  "play:serverbound:client_tick_end": "tick_end",
  "play:serverbound:command_suggestion": "tab_complete",
  "play:serverbound:container_button_click": "enchant_item",
  "play:serverbound:container_click": "window_click",
  "play:serverbound:container_close": "close_window",
  "play:serverbound:container_slot_state_changed": "set_slot_state",
  "play:serverbound:custom_payload": "custom_payload",
  "play:serverbound:entity_tag_query": "query_entity_nbt",
  "play:serverbound:interact": "use_entity",
  "play:serverbound:jigsaw_generate": "generate_structure",
  "play:serverbound:move_player_pos": "position",
  "play:serverbound:move_player_pos_rot": "position_look",
  "play:serverbound:move_player_rot": "look",
  "play:serverbound:move_player_status_only": "flying",
  "play:serverbound:move_vehicle": "vehicle_move",
  "play:serverbound:paddle_boat": "steer_boat",
  "play:serverbound:place_recipe": "craft_recipe_request",
  "play:serverbound:player_abilities": "abilities",
  "play:serverbound:player_action": "block_dig",
  "play:serverbound:player_command": "entity_action",
  "play:serverbound:recipe_book_change_settings": "recipe_book",
  "play:serverbound:recipe_book_seen_recipe": "displayed_recipe",
  "play:serverbound:rename_item": "name_item",
  "play:serverbound:resource_pack": "resource_pack_receive",
  "play:serverbound:seen_advancements": "advancement_tab",
  "play:serverbound:set_beacon": "set_beacon_effect",
  "play:serverbound:set_carried_item": "held_item_slot",
  "play:serverbound:set_command_block": "update_command_block",
  "play:serverbound:set_command_minecart": "update_command_block_minecart",
  "play:serverbound:set_creative_mode_slot": "set_creative_slot",
  "play:serverbound:set_jigsaw_block": "update_jigsaw_block",
  "play:serverbound:set_structure_block": "update_structure_block",
  "play:serverbound:sign_update": "update_sign",
  "play:serverbound:spectate_entity": "spectate",
  "play:serverbound:swing": "arm_animation",
  "play:serverbound:teleport_to_entity": "spectate",
  "play:serverbound:use_item_on": "block_place"
};

function deepClone(value) {
  return JSON.parse(JSON.stringify(value));
}

function packetName(name) {
  return name.replace(/^minecraft:/, "").replace(/\//g, "_");
}

function aliasFor(state, direction, name) {
  const cleanName = packetName(name);
  return ALIASES[`${state}:${direction}:${cleanName}`] || cleanName;
}

function hexId(id) {
  return `0x${Number(id).toString(16).padStart(2, "0")}`;
}

function packetContainer(protocol, state, direction) {
  const container = protocol[state] &&
    protocol[state][direction] &&
    protocol[state][direction].types &&
    protocol[state][direction].types.packet;
  if (!container || !Array.isArray(container[1])) return null;
  const nameField = container[1].find((field) => field.name === "name");
  const paramsField = container[1].find((field) => field.name === "params");
  if (!nameField || !paramsField) return null;
  return {
    nameType: nameField.type[1],
    paramsType: paramsField.type[1]
  };
}

function loadPacketsReport(root) {
  const packetsPath = path.join(root, "data", "26.1.1", "reports", "packets.json");
  return JSON.parse(fs.readFileSync(packetsPath, "utf8"));
}

function applyPacketIds(protocol, packetsReport) {
  const unsupported = [];
  let mapped = 0;

  for (const [reportState, directions] of Object.entries(packetsReport)) {
    const state = STATE_MAP[reportState];
    if (!state) continue;

    for (const [reportDirection, packets] of Object.entries(directions)) {
      const direction = DIRECTION_MAP[reportDirection];
      if (!direction) continue;

      const packet = packetContainer(protocol, state, direction);
      if (!packet) continue;

      const baseFields = packet.paramsType.fields;
      const nextMappings = {};
      const nextFields = {};
      const sortedPackets = Object.entries(packets)
        .sort((left, right) => left[1].protocol_id - right[1].protocol_id);

      for (const [name, meta] of sortedPackets) {
        const mappedName = aliasFor(reportState, reportDirection, name);
        const fieldType = baseFields[mappedName];
        if (!fieldType) {
          unsupported.push({
            state: reportState,
            direction: reportDirection,
            id: meta.protocol_id,
            name: packetName(name),
            attemptedName: mappedName
          });
          continue;
        }
        nextMappings[hexId(meta.protocol_id)] = mappedName;
        nextFields[mappedName] = fieldType;
        mapped += 1;
      }

      if (reportState === "handshake" && reportDirection === "serverbound" && baseFields.legacy_server_list_ping) {
        nextMappings["0xfe"] = "legacy_server_list_ping";
        nextFields.legacy_server_list_ping = baseFields.legacy_server_list_ping;
      }

      packet.nameType.mappings = nextMappings;
      packet.paramsType.fields = nextFields;
    }
  }

  return { mappedPackets: mapped, unsupportedPackets: unsupported };
}

function findVersionInfo(originalMinecraftData, version) {
  if (Number(version) === TARGET_PROTOCOL) {
    return originalMinecraftData.versionsByMinecraftVersion.pc["26.1.1"];
  }
  return originalMinecraftData.versionsByMinecraftVersion.pc[String(version)];
}

function isLocal26Version(originalMinecraftData, version) {
  const info = findVersionInfo(originalMinecraftData, version);
  return Boolean(info && info.version === TARGET_PROTOCOL && info.majorVersion === TARGET_MAJOR);
}

function versionDataFor(wrapper, info) {
  const dataVersion = Number(info.dataVersion) || 0;
  const versionObj = {
    dataVersion,
    type: "pc",
    majorVersion: COMPAT_MAJOR,
    version: info.version,
    minecraftVersion: info.minecraftVersion,
    releaseType: info.releaseType
  };

  function otherDataVersion(other) {
    const target = wrapper.versionsByMinecraftVersion.pc[other];
    if (!target) {
      throw new RangeError(`Version '${other}' not found for pc`);
    }
    return Number(target.dataVersion) || 0;
  }

  versionObj[">="] = (other) => dataVersion >= otherDataVersion(other);
  versionObj[">"] = (other) => dataVersion > otherDataVersion(other);
  versionObj["<"] = (other) => dataVersion < otherDataVersion(other);
  versionObj["<="] = (other) => dataVersion <= otherDataVersion(other);
  versionObj["=="] = (other) => dataVersion === otherDataVersion(other);
  return versionObj;
}

function addUnique(array, value) {
  if (Array.isArray(array) && !array.includes(value)) array.push(value);
}

function patchPrismarineSupportedVersions() {
  try {
    const mineflayerVersion = require("mineflayer/lib/version");
    addUnique(mineflayerVersion.testedVersions, "26.1.1");
    mineflayerVersion.latestSupportedVersion = "26.1.1";
  } catch (err) {
    // Mineflayer may not be installed yet; dependency loading will report that later.
  }

  try {
    const protocolVersion = require("minecraft-protocol/src/version");
    addUnique(protocolVersion.supportedVersions, "26.1.1");
  } catch (err) {
    // minecraft-protocol may not be installed yet; dependency loading will report that later.
  }
}

function createPrismarineRegistryWrapper() {
  let originalRegistry = null;
  try {
    originalRegistry = require("prismarine-registry");
  } catch (err) {
    return null;
  }

  function wrapRegistry(version) {
    const registry = originalRegistry(version);
    if (!registry || typeof registry.loadDimensionCodec !== "function") return registry;
    ensureDefaultDimensions(registry);

    const originalLoadDimensionCodec = registry.loadDimensionCodec.bind(registry);
    registry.loadDimensionCodec = (codec) => {
      if (codec && Array.isArray(codec.entries)) {
        const registryId = String(codec.id || "").replace(/^minecraft:/, "");
        const handled = new Set(["dimension_type", "worldgen/biome", "chat_type"]);
        if (!handled.has(registryId)) return;

        const entries = codec.entries.filter((entry) => entry && entry.value !== undefined && entry.value !== null);
        if (entries.length === 0) return;
        const result = originalLoadDimensionCodec(Object.assign({}, codec, { entries }));
        ensureDefaultDimensions(registry);
        return result;
      }
      const result = originalLoadDimensionCodec(codec);
      ensureDefaultDimensions(registry);
      return result;
    };

    return registry;
  }

  Object.assign(wrapRegistry, originalRegistry);
  return wrapRegistry;
}

function ensureDefaultDimensions(registry) {
  if (Array.isArray(registry.dimensionsArray) && registry.dimensionsArray.length > 0) return;

  const dimensions = [
    { id: 0, name: "overworld", minY: -64, height: 384 },
    { id: 1, name: "the_nether", minY: 0, height: 256 },
    { id: 2, name: "the_end", minY: 0, height: 256 }
  ];
  registry.dimensionsArray = dimensions;
  registry.dimensionsById = {};
  registry.dimensionsByName = {};
  for (const dimension of dimensions) {
    registry.dimensionsById[dimension.id] = dimension;
    registry.dimensionsByName[dimension.name] = dimension;
  }
}

function cloneRawBaseData(rawData) {
  const base = rawData.pc[BASE_DATA_VERSION];
  const raw = {};
  for (const key of Object.keys(base)) {
    if (key === "version" || key === "protocol") continue;
    raw[key] = base[key];
  }
  return raw;
}

function installMinecraftData26Shim(options) {
  const root = options && options.root ? options.root : path.resolve(__dirname, "../../..");
  const originalMinecraftData = require("minecraft-data");
  const rawData = require("minecraft-data/data.js");
  const mcDataToNode = require("minecraft-data/lib/loader");
  const supportFeature = require("minecraft-data/lib/supportsFeature");
  const prismarineRegistryWrapper = createPrismarineRegistryWrapper();

  const versionsByMinecraftVersion = Object.assign({}, originalMinecraftData.versionsByMinecraftVersion, {
    pc: Object.assign({}, originalMinecraftData.versionsByMinecraftVersion.pc)
  });
  const postNettyVersionsByProtocolVersion = Object.assign({}, originalMinecraftData.postNettyVersionsByProtocolVersion, {
    pc: Object.assign({}, originalMinecraftData.postNettyVersionsByProtocolVersion.pc)
  });
  const versions = Object.assign({}, originalMinecraftData.versions, {
    pc: originalMinecraftData.versions.pc.slice()
  });

  const preferred775 = ["26.1.1", "26.1.2", "26.1"]
    .map((version) => versionsByMinecraftVersion.pc[version])
    .filter(Boolean);
  postNettyVersionsByProtocolVersion.pc[TARGET_PROTOCOL] = preferred775;

  const protocol = deepClone(rawData.pc[BASE_DATA_VERSION].protocol);
  const compatibility = applyPacketIds(protocol, loadPacketsReport(root));
  const localCache = {};

  function loadLocalData(version) {
    const info = findVersionInfo(wrapper, version) || versionsByMinecraftVersion.pc["26.1.1"];
    const cacheKey = info.minecraftVersion;
    if (localCache[cacheKey]) return localCache[cacheKey];

    const raw = cloneRawBaseData(rawData);
    raw.protocol = protocol;
    raw.version = {
      version: info.version,
      minecraftVersion: info.minecraftVersion,
      majorVersion: COMPAT_MAJOR,
      releaseType: info.releaseType
    };

    const nodeData = mcDataToNode(raw);
    nodeData.type = "pc";
    nodeData.version = versionDataFor(wrapper, info);
    nodeData.isNewerOrEqualTo = (other) => nodeData.version[">="](other);
    nodeData.isOlderThan = (other) => nodeData.version["<"](other);
    nodeData.supportFeature = supportFeature(nodeData.version, versions.pc);
    localCache[cacheKey] = nodeData;
    return nodeData;
  }

  function wrapper(version, preNetty) {
    if (isLocal26Version(wrapper, version)) return loadLocalData(version);
    return originalMinecraftData(version, preNetty);
  }

  Object.assign(wrapper, originalMinecraftData);
  wrapper.versionsByMinecraftVersion = versionsByMinecraftVersion;
  wrapper.postNettyVersionsByProtocolVersion = postNettyVersionsByProtocolVersion;
  wrapper.versions = versions;
  wrapper.isLocal26Version = (version) => isLocal26Version(wrapper, version);
  wrapper.getLocal26Compatibility = () => compatibility;

  if (!Module._goatcoreMinecraftData26ShimInstalled) {
    const originalLoad = Module._load;
    Module._load = function patchedLoad(request, parent, isMain) {
      if (request === "minecraft-data") return wrapper;
      if (request === "prismarine-registry" && prismarineRegistryWrapper) return prismarineRegistryWrapper;
      return originalLoad.apply(this, arguments);
    };
    Module._goatcoreMinecraftData26ShimInstalled = true;
  }
  patchPrismarineSupportedVersions();

  return wrapper;
}

module.exports = {
  installMinecraftData26Shim
};
