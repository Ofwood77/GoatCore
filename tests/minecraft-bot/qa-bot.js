#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");

const ROOT = path.resolve(__dirname, "../..");
const RESULTS_ROOT = path.join(ROOT, "test-results", "minecraft-e2e");
const DEFAULT_TIMEOUT_MS = 60000;

let minecraftData26Shim = null;
try {
  minecraftData26Shim = require("./lib/minecraft-data-26-shim").installMinecraftData26Shim({ root: ROOT });
} catch (err) {
  minecraftData26Shim = null;
}

let mineflayer;
let minecraftProtocol;
let minecraftData;
try {
  mineflayer = require("mineflayer");
  minecraftProtocol = require("minecraft-protocol");
  minecraftData = require("minecraft-data");
} catch (err) {
  console.error("[FAIL] Dependances Node absentes: executez `npm install` dans tests/minecraft-bot");
  process.exit(2);
}

let pathfinderApi = null;
try {
  pathfinderApi = require("mineflayer-pathfinder");
} catch (err) {
  pathfinderApi = null;
}

let Vec3 = null;
try {
  Vec3 = require("vec3").Vec3;
} catch (err) {
  Vec3 = null;
}

function env(name, fallback) {
  const value = process.env[name];
  return value === undefined || value === "" ? fallback : value;
}

function parseIntEnv(name, fallback) {
  const raw = env(name, "");
  if (!raw) return fallback;
  const parsed = Number.parseInt(raw, 10);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function loadJson(file, fallback) {
  try {
    return JSON.parse(fs.readFileSync(file, "utf8"));
  } catch (err) {
    return fallback;
  }
}

function timestampDirName() {
  return new Date().toISOString().replace(/[:.]/g, "-");
}

function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true });
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function withTimeout(promise, ms, label) {
  let timer = null;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`${label} timeout after ${ms}ms`)), ms);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

function posToJson(pos) {
  if (!pos) return null;
  return {
    x: Number(pos.x.toFixed(3)),
    y: Number(pos.y.toFixed(3)),
    z: Number(pos.z.toFixed(3))
  };
}

function distance(a, b) {
  if (!a || !b) return 0;
  const dx = a.x - b.x;
  const dy = a.y - b.y;
  const dz = a.z - b.z;
  return Math.sqrt(dx * dx + dy * dy + dz * dz);
}

class Reporter {
  constructor(config) {
    this.startedAt = Date.now();
    this.resultsDir = path.join(RESULTS_ROOT, timestampDirName());
    ensureDir(this.resultsDir);
    this.logLines = [];
    this.tests = [];
    this.config = config;
  }

  log(line) {
    this.logLines.push(line);
    console.log(line);
  }

  ok(name, detail) {
    const line = `[OK] ${name}${detail ? ` - ${detail}` : ""}`;
    this.tests.push({ name, status: "ok", detail: detail || null });
    this.log(line);
  }

  warn(name, detail) {
    const line = `[WARN] ${name}${detail ? ` - ${detail}` : ""}`;
    this.tests.push({ name, status: "warn", detail: detail || null });
    this.log(line);
  }

  fail(name, error) {
    const message = error && error.message ? error.message : String(error || "unknown error");
    const line = `[FAIL] ${name} - ${message}`;
    this.tests.push({ name, status: "fail", detail: message });
    this.log(line);
  }

  write(bot, mode) {
    const durationMs = Date.now() - this.startedAt;
    const report = {
      date: new Date(this.startedAt).toISOString(),
      host: this.config.host,
      port: this.config.port,
      version: this.config.version || null,
      detectedVersion: this.config.detectedVersion || null,
      detectedProtocol: this.config.detectedProtocol || null,
      mode,
      durationMs,
      tests: this.tests,
      bot: bot && bot.entity ? {
        username: bot.username,
        position: posToJson(bot.entity.position),
        health: bot.health,
        food: bot.food,
        inventory: inventorySnapshot(bot)
      } : null
    };

    fs.writeFileSync(path.join(this.resultsDir, "summary.txt"), this.logLines.join("\n") + "\n");
    fs.writeFileSync(path.join(this.resultsDir, "report.json"), JSON.stringify(report, null, 2) + "\n");
    fs.writeFileSync(path.join(this.resultsDir, "bot.log"), this.logLines.join("\n") + "\n");
    writeServerErrors(this.config.serverLog, this.resultsDir, this);
    this.log(`[OK] Rapport genere - ${this.resultsDir}`);
  }

  hasCriticalFailure() {
    return this.tests.some((test) => test.status === "fail");
  }
}

function inventorySnapshot(bot) {
  if (!bot || !bot.inventory) return [];
  return bot.inventory.items().map((item) => ({
    name: item.name,
    type: item.type,
    count: item.count,
    slot: item.slot
  }));
}

function writeServerErrors(serverLog, resultsDir, reporter) {
  if (!serverLog) return;
  try {
    const raw = fs.readFileSync(serverLog, "utf8");
    const lines = raw.split(/\r?\n/).filter((line) =>
      /error|exception|traceback|segmentation|assert|fatal/i.test(line)
    );
    fs.writeFileSync(path.join(resultsDir, "server-errors.log"), lines.join("\n") + (lines.length ? "\n" : ""));
    if (lines.length > 0) reporter.warn("Erreurs serveur detectees", `${lines.length} ligne(s) suspecte(s)`);
  } catch (err) {
    reporter.warn("Logs serveur ignores", `impossible de lire ${serverLog}`);
  }
}

function loadConfig() {
  const configPath = env("MC_BOT_CONFIG", path.join(__dirname, "config.example.json"));
  const fileConfig = loadJson(configPath, {});
  return {
    host: env("MC_HOST", fileConfig.host || "127.0.0.1"),
    port: parseIntEnv("MC_PORT", fileConfig.port || 25565),
    username: env("MC_USERNAME", fileConfig.username || "TestBot"),
    auth: env("MC_AUTH", fileConfig.auth || "offline"),
    version: env("MC_VERSION", fileConfig.version || ""),
    timeoutMs: parseIntEnv("MC_TEST_TIMEOUT", fileConfig.timeoutMs || DEFAULT_TIMEOUT_MS),
    testArea: fileConfig.testArea || { x: 0, y: 65, z: 0, radius: 8 },
    serverLog: env("MC_SERVER_LOG", fileConfig.serverLog || "")
  };
}

function loadScenario(mode) {
  const file = path.join(__dirname, "scenarios", `${mode}.json`);
  const scenario = loadJson(file, null);
  if (!scenario) throw new Error(`scenario inconnu: ${mode}`);
  return scenario;
}

function packageVersion(name) {
  try {
    return require(`${name}/package.json`).version;
  } catch (err) {
    return "unknown";
  }
}

function pingServer(config) {
  return new Promise((resolve, reject) => {
    minecraftProtocol.ping({
      host: config.host,
      port: config.port,
      closeTimeout: config.timeoutMs,
      noPongTimeout: config.timeoutMs
    }, (err, response) => {
      if (err) reject(err);
      else resolve(response);
    });
  });
}

function mineflayerCanLoadVersion(version) {
  const data = minecraftData(version);
  if (!data) return false;
  const versionInfo = minecraftData.versionsByMinecraftVersion.pc[version];
  return !versionInfo || versionInfo.version === data.version.version;
}

function candidateVersions(status) {
  const version = status && status.version ? status.version : {};
  const protocol = Number(version.protocol);
  const brandedName = version.name || "";
  const fromName = [brandedName]
    .concat(brandedName.match(/((\d+\.)+\d+)/g) || [])
    .map((name) => minecraftData.versionsByMinecraftVersion.pc[name])
    .filter(Boolean);
  const fromProtocol = minecraftData.postNettyVersionsByProtocolVersion.pc[protocol] || [];
  const seen = new Set();
  return fromProtocol.concat(fromName)
    .map((info) => info.minecraftVersion)
    .filter((name) => {
      if (!name || seen.has(name)) return false;
      seen.add(name);
      return true;
    });
}

function warnIfLocal26Data(version, reporter) {
  if (!minecraftData.isLocal26Version || !minecraftData.isLocal26Version(version)) return;
  const compatibility = minecraftData.getLocal26Compatibility ? minecraftData.getLocal26Compatibility() : null;
  if (compatibility) {
    reporter.warn(
      "Donnees minecraft-data locales",
      `${version}; ${compatibility.mappedPackets} paquet(s) mappes, ` +
      `${compatibility.unsupportedPackets.length} paquet(s) non types`
    );
  } else {
    reporter.warn("Donnees minecraft-data locales", version);
  }
}

function local26SmokePlugins(config, reporter) {
  if (!minecraftData.isLocal26Version || !minecraftData.isLocal26Version(config.version)) return {};
  if (config.mode !== "smoke" && config.mode !== "qa") return {};

  reporter.warn("Mode compat minimal", "plugins Mineflayer avances desactives pour 26.1.1 experimental");
  return {
    anvil: false,
    bed: false,
    block_actions: false,
    blocks: false,
    book: false,
    boss_bar: false,
    breath: false,
    chest: false,
    command_block: false,
    craft: false,
    digging: false,
    enchantment_table: false,
    experience: false,
    explosion: false,
    fishing: false,
    furnace: false,
    generic_place: false,
    inventory: false,
    particle: false,
    physics: false,
    place_block: false,
    place_entity: false,
    rain: false,
    ray_trace: false,
    scoreboard: false,
    simple_inventory: false,
    sound: false,
    spawn_point: false,
    tablist: false,
    time: false,
    title: false,
    villager: false
  };
}

function isLocal26Smoke(config) {
  return (config.mode === "smoke" || config.mode === "qa") &&
    minecraftData.isLocal26Version &&
    minecraftData.isLocal26Version(config.version);
}

async function verifyMineflayerSupport(config, reporter) {
  const status = await pingServer(config);
  const serverVersion = status && status.version ? status.version : {};
  config.detectedVersion = serverVersion.name || "";
  config.detectedProtocol = Number(serverVersion.protocol) || null;
  reporter.ok("Status serveur", `${config.detectedVersion || "unknown"} protocol=${config.detectedProtocol || "unknown"}`);

  if (process.env.MC_ALLOW_UNSUPPORTED_MINEFLAYER === "1") {
    reporter.warn("Compatibilite Mineflayer ignoree", "MC_ALLOW_UNSUPPORTED_MINEFLAYER=1");
    return;
  }

  if (config.version) {
    if (mineflayerCanLoadVersion(config.version)) {
      warnIfLocal26Data(config.version, reporter);
      return;
    }
    throw new Error(
      `MC_VERSION=${config.version} n'est pas chargeable par minecraft-data ${packageVersion("minecraft-data")}. ` +
      "Essayez sans MC_VERSION ou utilisez le test protocolaire Python."
    );
  }

  const candidates = candidateVersions(status);
  const supported = candidates.filter(mineflayerCanLoadVersion);
  if (supported.length > 0) {
    config.version = supported.includes(config.detectedVersion) ? config.detectedVersion : supported[0];
    warnIfLocal26Data(config.version, reporter);
    return;
  }

  throw new Error(
    `Mineflayer ne peut pas encore charger les donnees du protocole ${config.detectedProtocol}. ` +
    `Versions candidates: ${candidates.join(", ") || "aucune"}. ` +
    `minecraft-data=${packageVersion("minecraft-data")}. ` +
    "Fallback non-Mineflayer: python3 tools/mc_ping.py login 127.0.0.1 25565 --username TestBot --timeout 5"
  );
}

async function connectBot(config, reporter) {
  const options = {
    host: config.host,
    port: config.port,
    username: config.username,
    auth: config.auth,
    plugins: local26SmokePlugins(config, reporter)
  };
  if (config.version) options.version = config.version;

  const bot = mineflayer.createBot(options);
  attachWatchers(bot, reporter);

  await withTimeout(new Promise((resolve, reject) => {
    bot.once("login", resolve);
    bot.once("kicked", (reason) => reject(new Error(`kick pendant login: ${reason}`)));
    bot.once("error", reject);
  }), config.timeoutMs, "login");
  reporter.ok("Connexion au serveur", `${config.host}:${config.port}`);

  await withTimeout(new Promise((resolve, reject) => {
    if (bot.entity) return resolve();
    bot.once("spawn", resolve);
    bot.once("kicked", (reason) => reject(new Error(`kick avant spawn: ${reason}`)));
    bot.once("end", () => reject(new Error("deconnexion avant spawn")));
  }), config.timeoutMs, "spawn");
  reporter.ok("Spawn detecte", JSON.stringify(posToJson(bot.entity.position)));

  if (pathfinderApi) {
    bot.loadPlugin(pathfinderApi.pathfinder);
  }

  return bot;
}

function attachWatchers(bot, reporter) {
  bot.on("kicked", (reason) => reporter.fail("Kick inattendu", reason));
  bot.on("error", (err) => reporter.fail("Erreur bot", err));
  bot.on("death", () => reporter.fail("Mort du bot", "death event"));
  bot.on("health", () => {
    if (bot.health <= 0) reporter.fail("Sante du bot", "health <= 0");
  });
  bot.on("message", (msg) => {
    const text = msg ? msg.toString() : "";
    if (text.startsWith("QA ")) return;
    if (/error|exception|fatal|stack/i.test(text)) {
      reporter.warn("Message serveur suspect", text);
    }
  });
}

function waitForMessageText(bot, predicate, timeoutMs, label) {
  const seenEcho = new Promise((resolve) => {
    const onMessage = (msg) => {
      const text = String(msg || "");
      if (predicate(text)) {
        bot.removeListener("messagestr", onMessage);
        resolve(text);
      }
    };
    bot.on("messagestr", onMessage);
  });
  return withTimeout(seenEcho, timeoutMs, label);
}

async function sendChat(bot, reporter) {
  const text = "Test automatique Minecraft OK";
  const seenEcho = waitForMessageText(bot, (msg) => msg.includes(text), 3000, "echo chat serveur");
  bot.chat(text);
  reporter.ok("Chat envoye");
  await seenEcho;
  reporter.ok("Echo chat serveur");
}

async function sendQaCommand(bot, reporter, command, predicate, label) {
  const wait = waitForMessageText(bot, predicate, 4000, label || `qa ${command}`);
  bot.chat(`!qa ${command}`);
  const reply = await wait;
  reporter.ok(`QA ${command}`, reply);
  return reply;
}

function parseQaState(line) {
  const pos = line.match(/pos=([-\d.]+) ([-\d.]+) ([-\d.]+)/);
  const gm = line.match(/gm=([a-z]+)/);
  const health = line.match(/health=([-\d.]+)/);
  const food = line.match(/food=(-?\d+)/);
  const items = line.match(/items=(-?\d+)/);
  return {
    x: pos ? Number.parseFloat(pos[1]) : NaN,
    y: pos ? Number.parseFloat(pos[2]) : NaN,
    z: pos ? Number.parseFloat(pos[3]) : NaN,
    gm: gm ? gm[1] : "",
    health: health ? Number.parseFloat(health[1]) : NaN,
    food: food ? Number.parseInt(food[1], 10) : NaN,
    items: items ? Number.parseInt(items[1], 10) : NaN
  };
}

async function runQaProtocol(bot, reporter) {
  await sendChat(bot, reporter);

  const beforeLine = await sendQaCommand(bot, reporter, "state", (msg) => msg.includes("QA state"), "qa state");
  const before = parseQaState(beforeLine);

  const target = {
    x: Number.isFinite(before.x) ? before.x + 1.25 : (bot.entity.position.x + 1.25),
    y: Number.isFinite(before.y) ? before.y : bot.entity.position.y,
    z: Number.isFinite(before.z) ? before.z + 0.75 : (bot.entity.position.z + 0.75)
  };
  bot._client.write("position", {
    x: target.x,
    y: target.y,
    z: target.z,
    flags: { onGround: true, hasHorizontalCollision: false }
  });
  reporter.ok("QA mouvement envoye", `target=${target.x.toFixed(2)} ${target.y.toFixed(2)} ${target.z.toFixed(2)}`);
  await sleep(250);

  const movedLine = await sendQaCommand(bot, reporter, "state", (msg) => msg.includes("QA state"), "qa state apres mouvement");
  const moved = parseQaState(movedLine);
  if (Number.isFinite(moved.x) && Math.abs(moved.x - target.x) < 0.2 && Math.abs(moved.z - target.z) < 0.2) {
    reporter.ok("QA mouvement verifie", `pos=${moved.x.toFixed(2)} ${moved.y.toFixed(2)} ${moved.z.toFixed(2)}`);
  } else {
    reporter.fail("QA mouvement verifie", new Error(`position serveur inattendue: ${movedLine}`));
  }

  await sendQaCommand(bot, reporter, "creative", (msg) => msg.includes("QA gamemode creative"), "qa creative");
  const creativeLine = await sendQaCommand(bot, reporter, "state", (msg) => msg.includes("gm=creative"), "qa state creative");
  reporter.ok("QA creative verifie", creativeLine);

  await sendQaCommand(bot, reporter, "survival", (msg) => msg.includes("QA gamemode survival"), "qa survival");
  const survivalLine = await sendQaCommand(bot, reporter, "state", (msg) => msg.includes("gm=survival"), "qa state survival");
  reporter.ok("QA survival verifie", survivalLine);

  await sendQaCommand(bot, reporter, "damage 1", (msg) => msg.includes("QA damage"), "qa damage");
  await sendQaCommand(bot, reporter, "heal 20", (msg) => msg.includes("QA heal"), "qa heal");
  await sendQaCommand(bot, reporter, "give stone 8", (msg) => msg.includes("QA give"), "qa give");
  const invLine = await sendQaCommand(bot, reporter, "state", (msg) => msg.includes("QA state"), "qa inventory state");
  const inv = parseQaState(invLine);
  if (Number.isFinite(inv.items) && inv.items >= 8) {
    reporter.ok("QA inventaire verifie", `items=${inv.items}`);
  } else {
    reporter.fail("QA inventaire verifie", new Error(invLine));
  }

  if (process.env.RUN_DESTRUCTIVE_TESTS === "1") {
    await sendQaCommand(bot, reporter, "chest", (msg) => msg.includes("QA chest") && msg.includes("placed=1"), "qa chest");
  } else {
    reporter.warn("QA chest ignore", "RUN_DESTRUCTIVE_TESTS=1 requis");
  }

  const holdMs = parseIntEnv("MC_QA_HOLD_MS", 0);
  if (holdMs > 0) {
    reporter.ok("QA maintien connexion", `${holdMs}ms`);
    await sleep(holdMs);
  }
}

async function moveControl(bot, reporter, control, durationMs, label) {
  const start = bot.entity.position.clone();
  bot.setControlState(control, true);
  await sleep(durationMs);
  bot.setControlState(control, false);
  await sleep(250);
  const end = bot.entity.position.clone();
  const d = distance(start, end);
  if (d > 0.03) {
    reporter.ok(label, `distance=${d.toFixed(3)}`);
  } else {
    reporter.fail(label, new Error(`position inchangee ou trop faible: distance=${d.toFixed(3)}`));
  }
}

async function jump(bot, reporter) {
  const startY = bot.entity.position.y;
  bot.setControlState("jump", true);
  await sleep(350);
  bot.setControlState("jump", false);
  await sleep(450);
  const dy = Math.abs(bot.entity.position.y - startY);
  reporter.ok("Saut", `delta_y=${dy.toFixed(3)}`);
}

async function pathfinderClose(bot, reporter) {
  if (!pathfinderApi || !bot.pathfinder) {
    reporter.warn("Pathfinder ignore", "mineflayer-pathfinder indisponible");
    return;
  }
  const { Movements, goals } = pathfinderApi;
  const target = bot.entity.position.offset(2, 0, 0);
  bot.pathfinder.setMovements(new Movements(bot));
  await withTimeout(bot.pathfinder.goto(new goals.GoalNear(target.x, target.y, target.z, 1)), 10000, "pathfinder");
  reporter.ok("Pathfinder proche", JSON.stringify(posToJson(bot.entity.position)));
}

async function runSmoke(bot, reporter, scenario) {
  await sendChat(bot, reporter);
  await sleep(scenario.waitMs || 3000);
  reporter.ok("Attente courte");
}

async function runMovement(bot, reporter) {
  await moveControl(bot, reporter, "forward", 800, "Mouvement avant");
  await moveControl(bot, reporter, "back", 600, "Mouvement arriere");
  await moveControl(bot, reporter, "left", 600, "Mouvement gauche");
  await moveControl(bot, reporter, "right", 600, "Mouvement droite");
  await jump(bot, reporter);
  bot.setControlState("sneak", true);
  await sleep(300);
  bot.setControlState("sneak", false);
  reporter.ok("Sneak");
  bot.setControlState("sprint", true);
  await moveControl(bot, reporter, "forward", 700, "Sprint");
  bot.setControlState("sprint", false);
  await pathfinderClose(bot, reporter);
}

async function runInventory(bot, reporter) {
  const items = bot.inventory.items();
  reporter.ok("Inventaire verifie", `${items.length} stack(s) observe(s)`);
  if (items.length === 0) {
    reporter.warn("Item equipe ignore", "inventaire vide; utilisez une commande serveur ou un monde de test prepare");
    return;
  }
  const item = items[0];
  if (item.slot >= 36 && item.slot <= 44) {
    bot.setQuickBarSlot(item.slot - 36);
    reporter.ok("Slot hotbar selectionne", `${item.name} slot=${item.slot}`);
  }
  try {
    await bot.equip(item, "hand");
    reporter.ok("Item equipe", `${item.name} x${item.count}`);
  } catch (err) {
    reporter.fail("Item equipe", err);
  }
  if (process.env.RUN_DESTRUCTIVE_TESTS === "1") {
    reporter.warn("Drop/pickup inventaire non automatise", "operation destructive gardee pour un scenario dedie");
  } else {
    reporter.warn("Drop/pickup ignore", "RUN_DESTRUCTIVE_TESTS non defini");
  }
}

function findPlaceableItem(bot) {
  return bot.inventory.items().find((item) =>
    /dirt|cobblestone|stone|planks|sand/.test(item.name)
  );
}

async function runBlocks(bot, reporter) {
  if (process.env.RUN_DESTRUCTIVE_TESTS !== "1") {
    reporter.warn("Scenario blocks ignore", "RUN_DESTRUCTIVE_TESTS=1 requis");
    return;
  }
  const item = findPlaceableItem(bot);
  if (!item) {
    reporter.warn("Bloc place ignore", "aucun bloc simple dans l'inventaire");
    return;
  }
  await bot.equip(item, "hand");
  const reference = bot.blockAt(bot.entity.position.offset(0, -1, 1));
  if (!reference) {
    reporter.fail("Bloc place", new Error("bloc de reference introuvable"));
    return;
  }
  try {
    if (!Vec3) {
      reporter.warn("Bloc place ignore", "module vec3 indisponible");
      return;
    }
    await bot.placeBlock(reference, new Vec3(0, 1, 0));
    const placed = bot.blockAt(reference.position.offset(0, 1, 0));
    reporter.ok("Bloc place", placed ? placed.name : "unknown");
    if (placed) {
      await bot.dig(placed);
      reporter.ok("Bloc casse", placed.name);
    }
  } catch (err) {
    reporter.fail("Interaction blocs", err);
  }
}

async function runMode(bot, reporter, mode) {
  const scenario = loadScenario(mode);
  if (scenario.destructive && process.env.RUN_DESTRUCTIVE_TESTS !== "1") {
    reporter.warn(`Scenario ${mode} ignore`, "RUN_DESTRUCTIVE_TESTS=1 requis");
    return;
  }
  if (mode === "smoke") return runSmoke(bot, reporter, scenario);
  if (mode === "qa") return runQaProtocol(bot, reporter);
  if (mode === "movement") return runMovement(bot, reporter);
  if (mode === "inventory") return runInventory(bot, reporter);
  if (mode === "blocks") return runBlocks(bot, reporter);
  if (mode === "full") {
    await runMode(bot, reporter, "smoke");
    await runMode(bot, reporter, "movement");
    await runMode(bot, reporter, "inventory");
    if (process.env.RUN_DESTRUCTIVE_TESTS === "1") {
      await runMode(bot, reporter, "blocks");
    } else {
      reporter.warn("Scenario blocks ignore", "RUN_DESTRUCTIVE_TESTS non defini");
    }
    return;
  }
  throw new Error(`mode non supporte: ${mode}`);
}

async function main() {
  const config = loadConfig();
  const mode = env("MC_TEST_MODE", "smoke");
  config.mode = mode;
  const reporter = new Reporter(config);
  let bot = null;

  try {
    loadScenario(mode);
    await verifyMineflayerSupport(config, reporter);
    bot = await connectBot(config, reporter);
    if ((bot.health === undefined || bot.health === null) && isLocal26Smoke(config)) {
      reporter.warn("Sante non observee", "spawn detecte; health packet non requis en compat smoke");
    } else if (!bot.health || bot.health <= 0) {
      throw new Error("bot non vivant apres spawn");
    } else {
      reporter.ok("Bot vivant", `health=${bot.health}`);
    }
    await withTimeout(runMode(bot, reporter, mode), config.timeoutMs, `scenario ${mode}`);
  } catch (err) {
    reporter.fail(`Scenario ${mode}`, err);
  } finally {
    if (bot) {
      if (typeof bot.clearControlStates === "function") bot.clearControlStates();
      if (typeof bot.quit === "function") bot.quit("Minecraft E2E test finished");
      reporter.ok("Deconnexion propre");
    }
    reporter.write(bot, mode);
  }

  process.exit(reporter.hasCriticalFailure() ? 1 : 0);
}

main().catch((err) => {
  console.error("[FAIL] Erreur fatale", err);
  process.exit(1);
});
