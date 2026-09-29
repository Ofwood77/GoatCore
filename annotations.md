# Annotations du projet Minecraft

## Fichiers d'en-tête (include/)

### mc_block_drops.h
- **Fonction principale** : `mc_block_drop_resolve_default` - Résout le drop par défaut d'un bloc
- **Structure** : `mc_block_drop_t` - Définit un drop de bloc avec item_id et count
- **Usage** : Gestion des drops lors de la destruction de blocs

### mc_chunk.h
- **Structure principale** : `mc_chunk_t` - Représente un chunk Minecraft
- **Sections** : 24 sections de 16x16x256 blocs
- **Fonctions** : 
  - `mc_chunk_init` - Initialisation d'un chunk
  - `mc_chunk_destroy` - Destruction d'un chunk
  - `mc_chunk_get_block` - Récupération d'un bloc
  - `mc_chunk_set_block` - Définition d'un bloc

### mc_inventory.h
- **Structures principales** :
  - `mc_slot_t` - Représente une pile d'objets
  - `mc_inventory_t` - Inventaire joueur (46 slots)
  - `mc_container_instance_t` - Conteneur (furnace, chest, etc.)
- **Fonctions clés** :
  - `mc_inventory_init` - Initialisation de l'inventaire
  - `mc_slot_set_simple` - Définition simple d'un slot
  - `mc_inventory_try_absorb_slot` - Absorption d'un slot dans l'inventaire

### mc_mining.h
- **Structures principales** :
  - `mc_mining_break_info_t` - Informations sur la rupture d'un bloc
  - `mc_mining_session_t` - Session de minage
- **Fonctions clés** :
  - `mc_mining_break_info` - Calcule les infos de minage pour un bloc
  - `mc_mining_session_validate_stop` - Validation de la fin du minage
  - `mc_mining_elapsed_enough` - Vérifie si le temps de minage est suffisant

### mc_nbt.h
- **Structure principale** : `mc_nbt_tag_t` - Tag NBT avec différents types
- **Fonctions clés** :
  - `mc_nbt_read_named_root` - Lecture d'un tag NBT nommé
  - `mc_nbt_write_named_root` - Ecriture d'un tag NBT nommé
  - `mc_anvil_validate_chunk` - Validation d'un chunk Anvil

### mc_net.h
- **Structure principale** : `mc_conn_t` - Connexion réseau client
- **Buffers** : `mc_buf_t` - Buffer de données réseau
- **Fonctions clés** :
  - `conn_read_frame` - Lecture d'une trame réseau
  - `conn_write_packet` - Ecriture d'un paquet réseau

### mc_packed.h
- **Fonctions principales** :
  - `mc_packed_pack_compact_u32` - Pack des données compactées
  - `mc_packed_unpack_compact_u32` - Dépack des données compactées

### mc_protocol.h
- **Fonctions de protocole** :
  - `proto_play_handle` - Gestion des paquets PLAY
  - `proto_handle_login` - Gestion du login
  - `proto_config_handle` - Gestion de la configuration

### mc_server.h
- **Structure principale** : `mc_server_t` - Serveur Minecraft
- **Fonctions clés** :
  - `net_server_init` - Initialisation du serveur
  - `net_server_run` - Lancement du serveur
  - `net_server_spawn_item_drop` - Spawn d'un item sur le sol

### mc_task_queue.h
- **Structure principale** : `mc_task_t` - Tâche asynchrone
- **Fonctions clés** :
  - `mc_task_queue_init` - Initialisation de la file d'attente
  - `mc_task_queue_push` - Ajout d'une tâche

### mc_util.h
- **Fonctions utilitaires** :
  - `log_info` / `log_error` - Logging
  - `read_file` - Lecture de fichier
  - `mc_now_us` - Timestamp actuel en microsecondes

### mc_world.h
- **Structure principale** : `mc_world_t` - Monde Minecraft
- **Fonctions clés** :
  - `mc_world_create` - Création du monde
  - `mc_world_tick` - Tick du monde
  - `mc_world_get_block` - Récupération d'un bloc

## Fichiers sources (src/)

### gameplay/block_drops.c
- **Fonction principale** : `mc_block_drop_resolve_default`
- **Algorithme** : Génération aléatoire de drops avec RNG basé sur position et temps
- **Données utilisées** : `mc_block_loot_entry_t` depuis generated_block_loot.h

### gameplay/mining.c
- **Fonctions principales** :
  - `mc_mining_break_info` - Calcul des infos de minage
  - `mc_mining_session_validate_stop` - Validation de la fin du minage
- **Données utilisées** : `mc_mining_block_tool_entry_t`, `mc_mining_tool_item_entry_t`
- **Logique** : Vérification des outils, niveaux de récolte, vitesse de minage

### generated/generated_block_loot.h
- **Structure** : `mc_block_loot_entry_t` - Définition d'un drop de bloc
- **Flags** : Indicateurs de support, items fixes, silk touch, etc.
- **Fonctions** :
  - `mc_block_loot_entry_from_state` - Récupération du drop par état
  - `mc_block_loot_default_item_id_from_state` - Récupération de l'item par défaut

### generated/generated_cooking_recipes.h
- **Structures** : `mc_cooking_recipe_t`, `mc_fuel_entry_t`
- **Données** : Recettes de cuisson (furnace, smoker, blast furnace)
- **Usage** : Système de cuisson dans le jeu

### generated/generated_crafting_recipes.h
- **Structures** : `mc_crafting_recipe_t`, `mc_crafting_ingredient_t`
- **Données** : Recettes d'artisanat (shaped, shapeless)
- **Usage** : Système d'artisanat dans le jeu

### net/buffer.c
- **Fonctions principales** :
  - `buf_init` - Initialisation du buffer
  - `buf_write` - Ecriture dans le buffer
  - `buf_reserve` - Réservation de mémoire
- **Gestion** : Buffer réseau avec allocation dynamique

### protocol/inventory.c
- **Fonctions principales** :
  - `mc_slot_read_net` - Lecture d'un slot depuis le réseau
  - `mc_slot_write_net` - Ecriture d'un slot vers le réseau
  - `mc_inventory_try_absorb_slot` - Absorption d'un slot dans l'inventaire
- **Protocole** : Gestion des objets entre client et serveur

### protocol/varint.c
- **Fonctions principales** :
  - `varint_read` - Lecture de varint depuis buffer
  - `varint_write` - Ecriture de varint vers buffer
- **Usage** : Encodage/décodage des varints dans le protocole Minecraft

### util/mc_util.c
- **Fonctions principales** :
  - `log_info` / `log_error` - Logging avec timestamps
  - `read_file` - Lecture de fichiers
  - `mc_now_us` - Timestamp actuel
- **Utilitaires** : Fonctions utilitaires pour le développement

### world/packed.c
- **Fonctions principales** :
  - `mc_packed_pack_compact_u32` - Pack des données compactées
  - `mc_packed_unpack_compact_u32` - Dépack des données compactées
- **Usage** : Manipulation de données packées pour les paletted containers

## Architecture globale

### Système de minage (mc_mining.h + gameplay/mining.c)
- Gestion des outils et leurs catégories (pickaxe, axe, etc.)
- Calcul de la vitesse de minage selon l'outil
- Vérification des niveaux de récolte requis
- Session de minage pour validation des STOP packets

### Système d'inventaire (mc_inventory.h + protocol/inventory.c)
- Gestion des slots avec composants
- Transfert entre client et serveur
- Absorption automatique des objets dans l'inventaire

### Système de monde (mc_world.h + world/)
- Gestion des chunks et sections
- Stockage des entités de blocs
- Calcul des drops de blocs

### Système réseau (mc_net.h + net/)
- Gestion des connexions clients
- Buffering des paquets
- Encodage/décodage du protocole

### Système de génération (generated/)
- Données statiques générées automatiquement
- Recettes de craft et cuisson
- Informations sur les blocs et outils
