# PLAN

## Réseau / protocole
- [x] Handshake / status / login / configuration / play de base
- [x] Streaming chunks autour du joueur avec `Set Center Chunk` + unload
- [x] Joueurs visibles en multi avec spawn / move / destroy MVP
- [~] IDs d'entités et de block entities encore partiellement sur fallback local
- [ ] Résolution complète des registries runtime pour entity types / block entity types
- [ ] Synchronisation de drops/items au login d'un joueur déjà connecté

## Monde / chunks / Anvil
- [x] Chargement Anvil en lecture
- [x] Génération procédurale de fallback
- [x] Chunkstore custom persistant pour les chunks modifiés
- [x] Heightmaps envoyés dans `map_chunk`
- [x] Fake skylight full-bright
- [~] Anvil reste surtout une source d'import/lecture
- [ ] Streaming vraiment infini avec budgets/priorités plus fins
- [ ] Sauvegarde Anvil vanilla complète et fiable si on veut revenir au `.mca`

## Containers / inventaire
- [x] Inventaire joueur autoritaire
- [x] Persistance joueur `world/players/*.mcp`
- [x] Coffres simples persistants `world/containers/`
- [x] Ender chest persistant par joueur
- [x] Suppression du store coffre à la casse
- [~] `window_click` survival fonctionne en best-effort + resync
- [ ] Double chests
- [ ] Synchronisation live d'un même coffre ouvert par plusieurs joueurs
- [ ] Règles vanilla fines de split/merge/drag/drop dans les fenêtres

## Entités / multijoueur
- [x] Entités joueur visibles
- [x] Drops d'items MVP à la casse de coffre
- [~] Item entities sans pickup/physics complets
- [ ] Pickup des items au sol
- [ ] Physics/velocity/gravity serveur pour les items
- [ ] Équipement des joueurs, metadata riche, skins/nametags

## Simulation gameplay
- [x] Eau / lave / feu / redstone MVP
- [x] `/setblock`
- [~] Casse/pose vanilla utilisable
- [ ] Fluids plus proches de vanilla (drainage, reflow, interactions)
- [ ] Redstone plus complète
- [ ] Loot de casse des blocs hors containers

## Persistance / save
- [x] Bootstrap auto de `world/`, `world/region/`, `world/chunks/`, `world/players/`, `world/containers/`
- [x] Fail-fast si le monde n'est pas accessible
- [x] Autosave chunks custom
- [x] Persistance des inventaires joueurs
- [~] Les entities temporaires (drops) ne persistent pas encore
- [ ] Sauvegarde/chargement des entities monde si on veut persistance complète

## Outils / tests / debug
- [x] `test_anvil_roundtrip`
- [x] bench génération
- [x] logs debug ciblés (`MC_DEBUG_CHUNK_RELOAD`, `MC_DEBUG_PLACE`, `MC_DEBUG_PLAYERS`)
- [ ] Tests automatisés pour les containers ouverts à plusieurs joueurs
- [ ] Tests réseau d'intégration à 2 clients
- [ ] Outil d'inspection des registries runtime (entity/block-entity ids)
