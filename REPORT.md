# Rapport d'avancement - 2026-03-21

## Fix coffres — états normalisés + rendu au chargement
- Les coffres ne posent plus leur `defaultState` brut : `chest`, `trapped_chest` et `ender_chest` sont maintenant normalisés en variantes **ouvrables et stables** (`type=single` pour les coffres, `waterlogged=false`).
- La normalisation s’applique aussi au rechargement depuis `chunkstore` et depuis Anvil, pour éviter qu’un vieux snapshot ou un monde existant garde des états `left/right` ou `waterlogged=true` hors scope v1.
- Le paquet `map_chunk` n’envoie plus `blockEntities=0` en dur : il sérialise maintenant les **block entities de coffre / ender chest** trouvées dans le chunk, avec NBT vide en v1.
- Renfort incrémental : quand un coffre ou ender chest est posé via `Use Item On`, le serveur envoie aussi un `tile_entity_data` immédiat au client.

### Tests
- `make test_anvil_roundtrip mc_server`
- `./test_anvil_roundtrip`
- Manuel : poser 5 coffres, vérifier qu’ils s’ouvrent tous ; déco/reco puis reboot, vérifier qu’ils restent visibles immédiatement

## Sprint “Joueurs corrects + containers coffre/ender persistants”
- Correction du bug multijoueur “les joueurs sont des cochons” : le type entité joueur utilisé par `spawn_entity` n’est plus l’ancienne valeur erronée et part maintenant sur une résolution/fallback actuelle (`149`, avec override possible via `MC_PLAYER_ENTITY_TYPE_ID` pour debug).
- Ajout des **containers persistants v1** : ouverture automatique des `minecraft:chest[...]` et `minecraft:ender_chest[...]` via `open_window`, sync complète via `window_items`, fermeture propre via `close_window`.
- **Chest** : snapshot persistant par bloc dans `./world/containers/` (format custom versionné + CRC32), 27 slots simples, sans double chest dans cette version.
- **Ender chest** : 27 slots persistés dans le snapshot joueur `./world/players/*.mcp`, partagés entre tous les ender chests d’un même joueur.
- `window_click` gère maintenant aussi les fenêtres de container : application best-effort des `changedSlots`, resync complet, save du coffre / ender chest + inventaire joueur.

### Limitations v1
- Le type entité joueur reste sur un fallback/runtime override, faute de registry entité exploitable localement dans les assets actuels.
- Coffres limités à **single chest 27 slots** ; pas de double chest, pas de trapped chest, pas de block entities Anvil.
- `window_click` côté containers reste dans le mode actuel **best-effort + resync**, pas encore une réémulation vanilla parfaite des cas complexes.

### Tests
- `make clean test_anvil_roundtrip mc_server`
- `./test_anvil_roundtrip`
- Manuel : 2 clients → vérifier que les joueurs sont bien visibles comme des joueurs
- Manuel : poser/ouvrir un coffre, déplacer des items, fermer/rouvrir, déco/reco puis reboot
- Debug : `MC_DEBUG_PLAYERS=1 ./mc_server`

## Sprint “Joueurs visibles en multi” (MVP)
- Ajout d’un pipeline multijoueur minimal : `player_info`, `spawn_entity`, `entity_move_look` / `rel_entity_move` / `entity_look`, `entity_head_rotation`, `entity_teleport`, `entity_destroy`, `player_remove`.
- Chaque connexion garde maintenant un petit état des **joueurs distants déjà connus/spawnés** avec leur dernière transform envoyée, pour éviter les respawns en boucle et limiter les téléports.
- Quand un joueur atteint `PLAY + ready`, les autres joueurs `PLAY + ready` lui sont annoncés automatiquement, et réciproquement. Les déplacements/rotations sont ensuite synchronisés à chaque tick.
- Déconnexion propre : destruction de l’entité distante + retrait de la player list sur les autres clients.
- Ajout d’un UUID offline stable si le client n’en fournit pas au login, réutilisé pour login, persistance joueur et entité multijoueur.
- Debug optionnel : `MC_DEBUG_PLAYERS=1` pour tracer add/spawn/move/remove/destroy des joueurs.

### Tests
- `make clean mc_server test_anvil_roundtrip`
- `./test_anvil_roundtrip`
- Manuel : connecter 2 clients, vérifier visibilité réciproque, mouvement, déco/reco sans doublon
- Debug : `MC_DEBUG_PLAYERS=1 ./mc_server`

## Sprint “Inventaire autoritaire complet” (v1)
- Remplacement de la hotbar debug “source de vérité” par un **inventaire joueur serveur** : 46 slots vanilla + cursor + slot hotbar sélectionné + `state_id`.
- Persistance joueur ajoutée dans `./world/players/<uuid|username>.mcp` (format custom versionné + CRC32) : chargement au login PLAY, save sur changements inventaire/slot/gamemode et au disconnect.
- Sync PLAY côté client : envoi de `window_items (0x13)` pour l’inventaire complet + `held_item_slot (0x53)` ; `set_slot (0x15)` sert aux updates incrémentales.
- Support des paquets client : `held_item_slot`, `set_creative_slot`, `window_click` (application best-effort des slots modifiés + resync complet), `close_window`.
- Mapping auto `item -> block state` généré au build depuis `assets/items.json` + `assets/blocks.json` (`tools/gen_item_place_map.py`) pour couvrir **tous les blocs simples** sans table hardcodée ; overrides runtime pour `grass_block`, `redstone`, `redstone_lamp`, `water_bucket`, `lava_bucket`, `flint_and_steel`.
- La pose utilise désormais **l’item réellement contenu dans le slot sélectionné** ; en survival, la stack est décrémentée si la pose réussit.

### Limitations v1
- Les `Slot` avec components non vides (`added/removed components`) ne sont pas encore décodés : support actuel fiable pour les blocs/items simples sans components.
- `window_click` applique actuellement les `changedSlots` du client puis resynchronise l’inventaire ; le framework est prêt, mais les règles vanilla détaillées (drag/double-click/etc.) ne sont pas encore re-simulées côté serveur.
- Les containers monde (`chest`, `furnace`, `anvil`, etc.) ne sont pas encore ouverts depuis les blocs, même si la base `window/container` est posée.

### Tests
- `make clean test_anvil_roundtrip mc_server`
- `./test_anvil_roundtrip`
- Manuel : mode créatif → remplir un slot, sélectionner la hotbar, poser un bloc, déco/reco puis reboot
- Debug : `MC_DEBUG_PLACE=1 ./mc_server`

## Fix pose vanilla Java : hotbar debug + bloc réel posé
- Le paquet `Use Item On` ne force plus `minecraft:stone` : la pose est maintenant résolue depuis un **slot hotbar suivi côté serveur**.
- Ajout d'une hotbar debug serveur fixe (9 slots) envoyée au client au login PLAY via `set_slot` + `held_item_slot` : stone, dirt, grass, water, lava, fire, redstone_block, redstone_wire, redstone_lamp.
- Le serveur écoute aussi `held_item_slot` côté client pour suivre le slot sélectionné et persister le **vrai bloc posé** dans le `chunkstore`.
- Ajout d'un debug `MC_DEBUG_PLACE=1` pour tracer : slot actif, item logique, position ciblée, position posée, `state_id` et clé bloc.

### Tests
- `make clean test_anvil_roundtrip mc_server`
- `./test_anvil_roundtrip`

## Pivot persistance fiable : `chunkstore` custom > Anvil > génération
- Les chunks **modifiés ou générés** ne sont plus sauvegardés dans les `.mca` : ils sont persistés dans `./world/chunks/c.<cx>.<cz>.mcc`, un snapshot complet compressé (zlib) avec header fixe + CRC32.
- Ordre de chargement côté worker : **snapshot custom**, sinon **Anvil** (lecture seule, monde de base), sinon **génération**.
- En cas de snapshot corrompu, le serveur logue l'erreur, retombe proprement sur `Anvil > génération`, puis réécrit un snapshot sain au prochain save.
- Objectif du pivot : supprimer le bug côté client Java où un bloc posé/cassé redevient `stone` après déco/reco ou reboot malgré une sauvegarde serveur apparemment correcte.

### Tests
- `make clean test_anvil_roundtrip mc_server`
- `./test_anvil_roundtrip`

## Fix reconnect “chunk packet retombe sur stone”
- Le round-trip Anvil passait en test, mais le paquet réseau `Chunk Data` ré-encodait encore les indices de palette en mode **paddé** par `long`, ce qui pouvait faire relire `palette[0]` (souvent `stone`) au client après déco/reco ou reboot.
- L'encodeur réseau des sections utilise maintenant le même **bitstream compact** que vanilla pour les blocs non uniformes.
- Résultat attendu : un bloc cassé ou un `/setblock air` reste correct après reconnect, même sans `Block Update` incrémental.

### Tests
- `make clean test_anvil_roundtrip mc_server`
- `./test_anvil_roundtrip`

## Fix “air redevient stone” (save/load/evict)
- Loader Anvil durci : une section `block_states` avec `palette_len > 1` mais sans `data`, ou avec un `long_count` inattendu, est maintenant rejetée explicitement au lieu de retomber silencieusement sur `palette[0]`.
- Ajout d'un helper interne de décodage de palette par section pour centraliser le chemin `compact/padded` et mieux tracer les reloads.
- Ajout d'un mode debug ciblé via `MC_DEBUG_CHUNK_RELOAD=cx,cz,x,y,z` : logs au save, au load, et dans le worker pour savoir si le chunk vient de l'Anvil ou de la génération.
- Test persistance renforcé : couvre maintenant `stone -> air`, sauvegarde disque, `mc_world_evict_outside(...)+reload`, `destroy/recreate`, et vérification intermédiaire du `.mca`.

### Tests
- `make test_anvil_roundtrip && ./test_anvil_roundtrip`
- `make clean && make`

# Rapport d'avancement - 2026-03-17

## Fixes persistance / rendu (Anvil + heightmaps + light)
- **Anvil round-trip (bug “tout redevient stone”)** : `block_states.data` est maintenant écrit en **bitstream compact** (format vanilla, chevauchement 64-bit) et la lecture supporte **compact + ancien format paddé** (rétro-compat).
- **Heightmaps** : génération par chunk du `MOTION_BLOCKING` (256 colonnes, 9 bits → 36 longs) envoyée dans `map_chunk (0x27)` au lieu de réutiliser le template (corrige la couleur biome dès le chargement).
- **Lumière (temp)** : injection d'un fake **Sky Light full-bright** dans `map_chunk (0x27)` (masques allumés + arrays 2048 bytes à `0xFF`) pour éviter les chunks sombres/glitchés.

### Tests
- `make test_anvil_roundtrip && ./test_anvil_roundtrip`

# Rapport d'avancement - 2026-03-16

## Résumé
Fix du brick "le serveur reçoit mais n'envoie plus" : avec `EPOLLET` et `EPOLLOUT` armé en permanence, les handlers alimentaient `conn->out` mais la boucle epoll ne retriggerait pas l'écriture (socket déjà writable) → aucun paquet sortant (pas de réponse STATUS, pas de login, etc.). Le flush des buffers sortants est maintenant fait côté thread tick (20 TPS) et `EPOLLOUT` n'est plus utilisé sur les sockets clients. Les `send()` utilisent `MSG_NOSIGNAL` quand dispo pour éviter les SIGPIPE.

## Outil de régression
- `python3 tools/mc_ping.py status 127.0.0.1 25565` (doit afficher le JSON de status + `pong ok`)
- `python3 tools/mc_ping.py login 127.0.0.1 25565 --username Test` (doit atteindre PLAY et afficher `play ok (join game + sync pos)`)

## Sprint “Config disque + biomes + monde infini + workers”
- `server.config` (auto-créé si absent) : `level-seed`, `view-distance`, `simulation-distance` (valeur invalide → **fail-fast**).
- Monde disque strict : monde toujours `./world` (région `./world/region`), création auto au boot, refus de démarrer si pas **R/W**.
- Biomes dans `Chunk Data` : le serializer écrit toujours le container biomes (uniforme) pour éviter le monde “gris”. Le `biome_id` est extrait de `assets/chunk_0_0.bin` au chargement (log `chunk template loaded: biome_id=...`).
- Monde “infini” v1 : streaming de chunks par joueur selon `view-distance` avec `Set Center Chunk` + envoi progressif (`CHUNKS_PER_TICK=4`) + `unload_chunk (0x21)` pour décharger côté client.
- Cache serveur + eviction : hashmap de chunks ; eviction basée sur `simulation-distance` (dirty sauvegardé puis free).
- Workers : pool threads pour load/génération (Anvil prioritaire sinon génération), insertion non-bloquante côté tick.
- Note compat : `MC_WORLD_PATH` / `MC_WORLD_SEED` sont ignorées (tout passe par `server.config` + `./world`).

### Commandes
- Démarrage “from scratch” : `rm -rf world server.config && ./mc_server`
- Éditer `server.config` puis relancer (seed + distances).
- Bench génération : `make mc_gen_bench && ./mc_gen_bench --chunks 1024 --seed 0`
- Test persistance : `make test_anvil_roundtrip && ./test_anvil_roundtrip`

## Sprint “Blocs dynamiques v1” (Anvil + eau/lave/feu + redstone MVP)
- Remplacement des chunks statiques : les chunks envoyés au client sont maintenant encodés depuis un monde **Anvil (.mca)** chargé en mémoire (grille 3x3 autour de (0,0)).
- Encodeur `Chunk Data and Update Light` : réutilisation de `assets/chunk_0_0.bin` comme **template** (heightmaps + light), et génération du `chunkdata` (24 sections) depuis le monde.
- Génération des `block_state_id` : ajout de `tools/blocks_json_to_block_states.py` pour convertir `assets/blocks.json` → `assets/block_states.json` (clés canonical triées), puis lookup via `tools/gen_registries.py` (table triée + binary search).
- Simulation tick-thread v1 : à rebrancher sur le nouveau monde async (actuellement `/setblock` place les blocs mais sans propagation automatique).
- Commande de test : `/setblock <x> <y> <z> <block>` (support: `minecraft:water`, `minecraft:lava`, `minecraft:fire`, `minecraft:redstone_wire`, `minecraft:redstone_block`, `minecraft:redstone_lamp`, `minecraft:air`, `minecraft:stone`).

### Commandes
- Générer les block states : `python3 tools/blocks_json_to_block_states.py assets/blocks.json assets/block_states.json`
- Rebuild : `make clean && make`
- Lancer : `./mc_server` (monde ancré sur disque dans `./world/region`)

### Limitations (v1)
- Streaming dynamique ajouté ensuite (voir sprint “Config disque + biomes + monde infini + workers”).
- Heightmaps + light réutilisent le template (pas de recalcul).
- Fluids : pas de drainage/reflow réaliste.
- Redstone : uniquement wire + redstone_block + redstone_lamp (connections wire fixées, power variable).

## Sprint “Génération procédurale + Sauvegarde Anvil” (Perlin + /setblock persistant)
- Monde procédural MVP : ajout d'un bruit **Perlin** (`include/stb_perlin.h`) + `mc_world_generate_chunk()` (stone/dirt/grass) utilisé quand un chunk est **absent** sur disque.
- Priorité source : **Anvil si chunk existe**, sinon **génération**.
- Sauvegarde Anvil : ajout d'un writer `.mca` (NBT write + zlib deflate + allocation secteurs) + tracking `dirty` par chunk ; **auto-save** en monde disque (`./world`) (chunks générés + `/setblock`).
- Bootstrap FS : `./world` et `./world/region` sont auto-créés au démarrage ; si non writable → **fail-fast** (le serveur refuse de démarrer).
- Outils offline :
  - Bench : `make mc_gen_bench && ./mc_gen_bench --chunks 1024 --seed 0`
  - Test persistance : `make test_anvil_roundtrip && ./test_anvil_roundtrip`

### Commandes
- Seed monde : `level-seed=` dans `server.config` (défaut: `0`)
- Lancer : `./mc_server` (crée/écrit dans `./world/region/`)

### Limitations (v0)
- Chunk NBT sauvegardé minimal (focus `sections`/`block_states`) : **ne préserve pas** entities/block entities/heightmaps/status/biomes (à patcher plus tard si on vise une compat vanilla complète).
- Auto-save capé (par défaut) à **1 chunk / tick**.
- Streaming dynamique ajouté ensuite (voir sprint “Config disque + biomes + monde infini + workers”).

### Assets présents/utilisés
```text
README.txt
attributes.json
blocks.json
chunk_0_0.bin
enchantments.json
foods.json
items.json
language.json
loginPacket.json
materials.json
proto.yml
protocol.json
recipes.json
registry_packets_1_21_1.bin
sounds.json
tints.json
version.json
```

## Sprint NBT / Anvil (début)
- Ajout d'un parseur NBT robuste en C (tagged union + allocation récursive) : lecture root nommée/unnommée, `nbt_free`, dump et lookup compound.
- Ajout d'un lecteur Anvil `.mca` : parsing header + extraction d'un chunk + décompression zlib/gzip → buffer NBT brut.
- Outil `mc_anvil_dump` pour valider sur un vrai fichier region : lecture chunk + parse NBT + affiche `xPos/zPos` (option `--tree`).

# Rapport d'avancement - 2026-03-14

## Résumé
Le serveur 1.21.1 en C passe la phase de connexion complète (handshake → login → configuration → play), envoie les registries, charge des chunks statiques, gère le keepalive, et permet les commandes de base (/gamemode, /tp) avec permissions OP côté client. Le cœur est désormais piloté par une boucle de tick 20 TPS via un thread dédié + file de tâches, et les `block_state_id` sont générés au build (Python + gperf).

## Fonctionnalités opérationnelles
- Connexion client vanilla 1.21.1 jusqu'au state PLAY.
- Envoi des registries depuis `assets/registry_packets_1_21_1.bin`.
- Envoi de 9 chunks (grille 3x3) via duplication de `assets/chunk_0_0.bin`.
- KeepAlive client/serveur avec timeout.
- Debug réseau côté serveur (log des paquets reçus par état/ID/longueur).
- Changement de gamemode via commandes (ex. `F3+F4` → /gamemode).
- Téléportation basique via `/tp`.
- Pose et cassage de bloc (mise à jour par `Block Update`).
- Boucle de tick 20 TPS (thread dédié) + file de tâches réseau.

## Permissions / OP
- Tous les joueurs reçoivent un statut OP niveau 4 via `Entity Event`.
- Envoi de `Player Abilities` aligné avec le gamemode courant.
- Le switch de gamemode via hotbar a été supprimé pour revenir au comportement vanilla (F3+F4).

## Commandes (Command Graph)
- Paquet `Commands` minimal avec:
  - `/gamemode <survival|creative|spectator|adventure>`
  - `/tp <x> <y> <z>`
  - `/tp <joueur> <x> <y> <z>`
  - `/tp <joueur> <joueur>`
- Gestion côté serveur du paquet `Serverbound Chat Command` (et Signed) : exécution minimale des commandes ci-dessus.

## Monde / Blocs
- Monde en mémoire (pas de sauvegarde).
- Mapping des `block_state_id` généré au build via `tools/gen_registries.py` + gperf.
- Fallback en 0/1 si une clé est absente (lookup).
- Casser un bloc -> `minecraft:air`.
- Poser un bloc -> `minecraft:stone` (ID via registries générées).

## Mouvement (minimal)
- Parse des paquets de position/rotation pour suivre les coordonnées côté serveur.
- Utilisé par `/tp` pour les destinations joueur.

## Limitations connues
- Pas de persistance de monde (chunks statiques).
- Pas d'inventaire réel ni d'items/slots.
- Pas de génération de chunks ni de chargement dynamique.
- Pas de validation de mouvement ni anti-triche.
- Commandes très limitées (pas de selectors, pas de parsing avancé).
- Un seul monde/dimension logique.
- NBT/Anvil, ECS et inventaire avancé non implémentés.

## Prochaines étapes suggérées
1. Gestion complète des chunks (chargement/déchargement dynamiques).
2. Parsing inventaire (Window Items, Click Container).
3. Persistance NBT / format Anvil.
4. Gestion d'entités (joueurs, mobs).
5. ECS + tracking multi‑joueurs.
