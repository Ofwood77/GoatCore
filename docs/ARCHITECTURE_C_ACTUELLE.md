# Architecture C Actuelle

Analyse de l'implementation actuelle du serveur C, basee sur le code present au 24 mars 2026.

## Sources principales de cette synthese

- `src/main.c`
- `src/net/server.c`
- `src/net/task_queue.c`
- `src/protocol/handlers/play.c`
- `src/protocol/framing.c`
- `src/world/world.c`
- `src/world/chunk_store.c`
- `src/world/player_store.c`
- `src/world/container_store.c`
- `REPORT.md`

## Vue d'ensemble

Le serveur est aujourd'hui organise autour de 3 sous-systemes majeurs :

1. Une couche reseau non bloquante basee sur `epoll`, responsable de l'accept des sockets clients et de la lecture des paquets entrants.
2. Un thread de tick a 20 TPS, responsable de toute la logique gameplay/protocole appliquee aux frames deja decodees, du tick monde, de la diffusion des updates et du flush sortant.
3. Un sous-systeme monde/chunks asynchrone, avec workers de chargement/generation, persistance custom et import Anvil en fallback.

Le point fort actuel du projet est une separation assez nette entre :

- I/O reseau entrant temps reel
- traitement logique centralise dans le thread tick
- chargement de chunks hors thread principal via workers

Le point faible majeur est que l'etat gameplay reste encore fortement represente par des `block_state_id` Vanilla "bruts", sans couche semantique intermediaire stable.

## Demarrage et configuration

Le point d'entree est `main()` dans `src/main.c`.

Au boot :

- le serveur charge `server.config`
- si le fichier est absent, il le cree avec des valeurs par defaut
- il force le monde sur `./world`
- il ignore explicitement `MC_WORLD_PATH` et `MC_WORLD_SEED`
- il construit un `mc_server_config_t`
- il appelle `net_server_init()`
- il lance la boucle principale via `net_server_run()`

Les parametres disque effectivement supportes aujourd'hui sont :

- `level-seed`
- `view-distance`
- `simulation-distance`

## Architecture reseau

### Boucle reseau

Le coeur reseau est dans `src/net/server.c`.

`net_server_init()` :

- cree la socket d'ecoute TCP
- la passe en non bloquante
- initialise `epoll`
- enregistre la socket d'ecoute dans `epoll`
- cree le monde via `mc_world_create()`
- initialise la file de taches `mc_task_queue_t`

`net_server_run()` :

- demarre le thread tick `tick_thread_main`
- reste dans une boucle `epoll_wait()`
- accepte les nouvelles connexions
- met chaque socket client en non bloquante
- cree une structure `mc_conn_t` par client
- inscrit chaque client en `EPOLLIN | EPOLLET`

### Lecture des donnees entrantes

Quand un client devient lisible :

- `handle_client_read()` lit en boucle avec `recv()` jusqu'a `EAGAIN`
- les octets sont accumules dans `mc_conn_t.in`
- `conn_read_frame()` decode les frames Minecraft
- chaque frame decodee devient une `mc_task_t`
- la tache est poussee dans `mc_task_queue_t`

Important :

- le thread reseau ne traite pas directement la logique protocole/gameplay
- il se limite a lire, decoder et pousser des taches
- le `refcount` atomique de `mc_conn_t` evite une destruction prematuree pendant qu'une tache est encore en file

### Ecriture des donnees sortantes

L'ecriture n'est pas pilotee par `EPOLLOUT`.

Le projet a explicitement abandonne cette approche apres un bug documente dans `REPORT.md` :

- les handlers alimentaient `conn->out`
- la socket etait deja writable
- `epoll` ne retriggerait pas forcement l'ecriture
- le serveur pouvait "recevoir mais ne plus rien envoyer"

Le modele actuel est :

- `conn_write_packet()` serialize un paquet dans `mc_conn_t.out`
- l'acces au buffer sortant est protege par `mc_conn_t.out_lock`
- le thread tick appelle `handle_client_write()` a chaque tick
- `send()` est fait depuis le thread tick
- `MSG_NOSIGNAL` est utilise quand disponible

Autrement dit :

- lecture entrante = thread `epoll`
- traitement logique + ecriture sortante = thread tick

### Decodage / encodage protocolaire

Le framing bas niveau est dans `src/protocol/framing.c`.

`conn_read_frame()` :

- compacte le buffer entrant
- lit la longueur VarInt
- gere eventuellement la decompression
- applique `mc_crypto_read()`
- extrait le `packet_id`
- alloue un buffer payload independant

`conn_write_packet()` :

- prefixe le `packet_id`
- prepare eventuellement le mode compression
- applique `mc_crypto_write()`
- append le resultat dans `mc_conn_t.out`

La compression cote sortie est encore volontairement simplifiee :

- le chemin de "vraie" compression n'est pas pleinement utilise
- si la compression est activee, le code reste majoritairement en chemin "data_len = 0"

## Gestion des threads

### Thread reseau

Responsabilites :

- accept
- lecture socket
- decodage des frames
- creation des taches

Il ne tick pas le monde et ne pousse pas directement la logique gameplay.

### Thread tick

`tick_thread_main()` tourne a 20 TPS, soit un tick toutes les 50 ms.

Responsabilites :

- drainer `mc_task_queue_t`
- appeler les handlers protocole selon l'etat (`HANDSHAKING`, `STATUS`, `LOGIN`, `CONFIGURATION`, `PLAY`)
- appeler `mc_world_tick()`
- diffuser les `mc_block_update_t` accumules par le monde
- piloter `proto_play_tick()` pour keepalive et streaming de chunks
- synchroniser les joueurs distants (`proto_play_sync_remote_player()`)
- flusher tous les buffers sortants des connexions
- gerer le TTL des item entities
- calculer periodiquement l'eviction des chunks selon la distance de simulation

Ce thread est aujourd'hui le veritable "thread autoritaire" du gameplay et du protocole PLAY.

### Workers monde

Le monde cree entre 1 et 4 workers (`MAX_WORKERS = 4`) dans `mc_world_create()`.

Le nombre est derive du nombre de CPU en ligne, borne a 4.

Responsabilites des workers :

- charger un chunk depuis le chunk store custom
- sinon tenter un import depuis Anvil
- sinon generer proceduralement le chunk
- normaliser certains etats de conteneurs
- renvoyer le chunk charge dans une file "done"

Le thread tick monde integre ensuite ces chunks precharges dans la map principale.

## File de taches

La file `mc_task_queue_t` est une simple liste chainee protegee par mutex/condvar.

Etat actuel notable :

- `mc_task_queue_push()` signale une condition
- `mc_task_queue_drain()` ne bloque pas et vide toute la file d'un coup
- le thread tick ne dort pas sur la condition : il dort sur son rythme 20 TPS

Conclusion :

- la file joue surtout le role de tampon locke entre thread reseau et thread tick
- il n'existe pas encore de planification fine ni de backpressure applicative

## Structures memoire importantes

## `mc_server`

`struct mc_server` dans `src/net/server.c` contient :

- la socket d'ecoute
- le fd `epoll`
- la config serveur
- le flag atomique `running`
- la liste chainee des connexions `mc_conn_t *conns`
- le compteur `next_entity_id`
- le mutex `conns_lock`
- la file de taches
- le thread tick
- le pointeur vers `mc_world_t`
- le tableau dynamique des item entities monde

Observation importante :

- il n'existe pas encore de store generique d'entites monde
- les joueurs sont portes par les connexions
- les drops d'items sont portes par `mc_server`

## `mc_conn`

Chaque connexion client (`mc_conn_t`) porte a la fois l'etat reseau, protocolaire et une partie de l'etat joueur.

Champs majeurs :

- `fd`
- buffers `in` et `out`
- `out_lock`
- `state` protocolaire atomique
- `closing`
- `refcount`
- `username`, `uuid`, `has_uuid`
- flags de progression login/config/play
- `entity_id`
- `gamemode`
- `teleport_id`
- `mc_player_data_t *player`
- keepalive courant
- position/rotation courantes
- etat d'encryption
- liste chainee `next`
- etat de streaming chunks :
  - `has_center_chunk`
  - `center_cx`, `center_cz`
  - `sent_chunks`
  - `pending_chunks`
- etat des autres joueurs visibles :
  - `remote_players`
  - `remote_players_len`
- fenetre actuellement ouverte :
  - `mc_active_window_t active_window`

Consequence architecturale :

- le joueur n'est pas un objet monde autonome
- son etat runtime vit accroche a la connexion
- la persistance joueur (`.mcp`) est chargee dans `mc_conn->player`

## Joueurs, entites et memoire gameplay

### Joueurs locaux

Le "joueur local" d'une connexion est represente par :

- `mc_conn_t` pour la presence reseau et la position runtime
- `mc_player_data_t` pour l'inventaire, l'ender chest, le gamemode et l'identite persistable

`ensure_player_loaded()` :

- alloue `mc_player_data_t`
- tente `mc_player_store_load()`
- sinon remplit un loadout de depart

Les snapshots joueurs sont stockes dans :

- `world/players/<uuid|username>.mcp`

### Joueurs distants

Chaque client garde un tableau `remote_players` de `mc_remote_player_t`.

Ce tableau stocke pour chaque autre joueur :

- `entity_id`
- `uuid`
- dernier transform envoye
- flags `listed` / `spawned`

But :

- eviter les respawns repetes
- choisir entre move relatif, look ou teleport

### Item entities

Les drops d'objets sont stockes dans `mc_server.item_entities`.

Chaque entree contient :

- `entity_id`
- `uuid`
- position
- date d'expiration
- `mc_slot_t slot`

Limitations :

- pas de vrai systeme physique serveur
- pas de pickup complet
- duree de vie fixe a 30 secondes
- pas de persistance disque

## Boucle principale de tick

La boucle de tick reelle peut se resumer ainsi :

1. dormir jusqu'au tick suivant si necessaire
2. drainer toutes les taches reseau accumulees
3. appeler les handlers protocole sur le thread tick
4. ticker le monde via `mc_world_tick()`
5. recuperer la liste des block updates
6. pousser ces updates a tous les joueurs PLAY + ready
7. executer `proto_play_tick()` pour keepalive et streaming
8. synchroniser les autres joueurs visibles
9. flusher les buffers sortants des connexions
10. gerer l'eviction de chunks
11. vider la liste des updates monde

Ce design garantit que :

- la plupart des mutations gameplay restent centralisees
- les writes socket sortantes restent serializees
- le monde n'est pas modifie depuis le thread `epoll`

## Monde, chunks et persistance

## Representation memoire du monde

`mc_world_t` contient principalement :

- le chemin du monde
- la seed
- les parametres de generation
- un petit cache d'IDs frequents (`mc_world_ids_t`)
- une hashmap ouverte de chunks (`mc_chunk_map_t`)
- une `chunk_list` dense pour save/eviction
- un tableau d'updates blocs (`mc_block_update_t`)
- les queues de jobs et de chunks termines
- les workers et leur synchronisation

## Representation memoire d'un chunk

`mc_chunk_t` contient :

- `cx`, `cz`
- `blocks[MC_BLOCKS_PER_CHUNK]`
- flags `loaded`, `dirty`, `evict_after_save`
- `list_index`

Le layout bloc est un tableau plat :

- type : `int32_t`
- contenu : `block_state_id` Vanilla
- index : `(y_index * 16 + z) * 16 + x`
- hauteur globale : 384 couches (`-64 .. 319`)

Il n'y a pas de palette memoire par chunk ni par section en runtime.

## Chargement des chunks

Ordre de priorite actuel :

1. chunk store custom `world/chunks/c.<cx>.<cz>.mcc`
2. import Anvil `world/region/r.<rx>.<rz>.mca`
3. generation procedurale fallback

`mc_world_get_chunk()` :

- renvoie le chunk s'il est deja pret
- renvoie `NULL` si le chunk est en cours de chargement
- sinon cree une entree "loading", pousse un job worker, puis renvoie `NULL`

Consommation pratique :

- le streaming de chunks cote protocole sait reessayer plus tard
- le code appelant un simple `get_block` peut voir `air` tant que le chunk n'est pas arrive

## Persistance custom

### Chunk store

`src/world/chunk_store.c` implemente un format custom `.mcc` :

- magic `MCC1`
- version
- coordonnees chunk
- `min_y`, `height`, nombre de blocs
- taille brute
- CRC32
- payload zlib

Le payload est simplement le tableau complet des `int32_t block_state_id` du chunk.

Conclusion :

- le format disque custom est proche du layout RAM
- il ne preserve pas les structures Vanilla de plus haut niveau
- il ne stocke pas de palette, seulement des IDs deja resolves

### Sauvegarde joueurs

`src/world/player_store.c` implemente des snapshots `.mcp`.

Ils contiennent :

- identite joueur
- gamemode
- inventaire complet
- cursor slot
- ender chest

### Sauvegarde conteneurs

`src/world/container_store.c` implemente des snapshots `.mct`.

Ils contiennent :

- type de conteneur
- position
- `state_id`
- contenu des 27 slots

## Tick monde et save

`mc_world_tick()` :

- integre les chunks termines par les workers
- applique les modifications en attente si un chunk a ete modifie avant sa fin de chargement
- sauvegarde au plus `ANVIL_SAVE_ATTEMPTS_PER_TICK = 1` chunk dirty par tick dans le chunk store
- evicte plus tard les chunks dirty marques `evict_after_save`

Ce point est important pour la perf :

- le monde reste essentiellement autoritaire en RAM
- la persistance est lissee dans le temps

## Streaming de chunks cote protocole

Le streaming PLAY est gere dans `src/protocol/handlers/play.c`.

Par connexion :

- `sent_chunks` est un hash set des chunks deja envoyes
- `pending_chunks` est une file des chunks a tenter d'envoyer

`chunk_stream_tick()` :

- recalcule le centre chunk depuis la position joueur
- reconstruit la fenetre envoyee si le centre a change
- pre-enqueue les chunks requis
- demande au monde de charger ceux qui manquent
- envoie jusqu'a `CHUNKS_PER_TICK = 4`

Le paquet chunk reencode les sections depuis le tableau `chunk->blocks[]`.

## Encodage chunk reseau

`proto_play_encode_chunkdata_for_test()` reconstruit un encodage palettise section par section pour le protocole.

En runtime :

- la RAM contient un tableau plat d'IDs
- le reseau reconstruit une palette temporaire par section 16x16x16
- les indices sont compactes selon les regles Vanilla

Donc :

- palette = format de transport/import
- pas format natif de stockage runtime

## Systeme actuel des blocs

## Source de verite actuelle

Le projet utilise directement les `block_state_id` Vanilla comme identite principale du bloc.

Cela se voit partout :

- `mc_chunk_t.blocks[]` stocke des `int32_t`
- `mc_world_set_block()` prend un `state_id`
- `mc_world_get_block()` renvoie un `state_id`
- le chunk store custom ecrit les `state_id` bruts
- la pose d'objet convertit un item en `state_id`
- les conteneurs deduisent leur type a partir de la cle de block state

## Generation de la table des IDs

La resolution `nom canonical -> state_id` est generee offline :

- `tools/blocks_json_to_block_states.py` produit `assets/block_states.json`
- `tools/gen_registries.py` produit `src/generated/generated_registries.c/.h`
- `mc_block_state_id()` fait le lookup `string -> int`
- `mc_block_state_key()` fait le lookup inverse `int -> string`

Le format canonical des cles est de type :

- `minecraft:stone`
- `minecraft:chest[facing=north,type=single,waterlogged=false]`

avec les proprietes triees lexicalement.

## Conversion item -> bloc pose

La pose utilise `proto_play_item_to_state()`.

Chemin actuel :

- base : `mc_item_default_place_state(item_id)`
- cette table vient de `assets/blocks.json defaultState`
- puis quelques overrides manuels corrigent certains cas

Overrides explicites actuels :

- `grass_block`
- `redstone`
- `redstone_lamp`
- `flint_and_steel`
- `water_bucket`
- `lava_bucket`
- `chest`
- `trapped_chest`
- `ender_chest`

Cela veut dire que le comportement de pose ne vient pas encore d'une reemulation generale des regles Vanilla, mais d'un melange :

- `defaultState`
- quelques states cibles hardcodes

## Conteneurs et normalisation forcee

Le monde applique une normalisation speciale via `mc_world_normalize_container_state_id()`.

Exemples :

- tous les coffres deviennent `type=single`
- `waterlogged` est force a `false`
- `facing` est conservee dans la mesure du possible

Cette normalisation est appliquee :

- au chargement depuis chunk store
- au chargement depuis Anvil
- a `mc_world_set_block()`
- a l'ouverture d'un conteneur
- a l'encodage de certains paquets

Cette logique stabilise certains cas v1, mais elle introduit aussi une divergence volontaire par rapport a l'etat source Vanilla.

## Gestion actuelle des block entities

Il n'existe pas encore de couche block entity generale.

Etat actuel :

- le type de block entity est deduit a partir du `block_state_id`
- seules quelques block entities de conteneurs sont traitees
- le NBT envoye dans les chunks est minimal
- le NBT envoye via `tile_entity_data` est lui aussi minimal

En pratique :

- bloc et block entity ne sont pas modeles comme deux objets distincts
- le serveur derive l'un de l'autre de facon opportuniste

## Probleme central : pourquoi le systeme de Block IDs est fragile

Les causes probables des bugs actuels sont les suivantes.

### 1. Usage direct du `block_state_id` Vanilla partout

Le `block_state_id` n'est pas seulement une cle de serialisation reseau :

- il est devenu la representation memoire primaire du monde
- il est aussi le format disque custom
- il pilote certaines decisions gameplay

Cela couple trop fortement :

- le stockage RAM
- le stockage disque
- le protocole reseau
- la logique de jeu

### 2. Absence de separation bloc / etat / block entity

Le projet n'a pas encore de couche intermediaire du type :

- block type stable
- set de proprietes
- block entity separee

Resultat :

- des decisions gameplay sont prises par inspection de chaine (`mc_block_state_key`)
- les block entities ne sont pas des objets premier niveau
- toute evolution des states devient fragile

### 3. Mapping `item -> state` encore heuristique

Le chemin actuel repose sur :

- `defaultState` dans `assets/blocks.json`
- quelques overrides manuels

Ce n'est pas une simulation generale des regles Vanilla de placement.

Risques :

- mauvais state par defaut
- mauvaise orientation
- proprietes oubliees
- divergence selon item ou contexte

### 4. Normalisation forcee des conteneurs

La normalisation corrige des bugs visibles, mais masque aussi un probleme de fond :

- l'etat source charge ou place n'est pas toujours l'etat autoritaire final
- plusieurs couches du code peuvent le reecrire

Cela complique le raisonnement et la reproductibilite.

### 5. `mc_world_get_block()` peut renvoyer `air` pendant un chargement asynchrone

Si le chunk n'est pas encore pret :

- `mc_world_get_chunk()` renvoie `NULL`
- `mc_world_get_block()` renvoie alors `air`

Pour du code gameplay, cela peut ressembler a un vrai bloc air alors qu'il s'agit seulement d'un chunk encore en transit.

### 6. Dependances a des fallbacks d'IDs runtime

Le projet utilise encore des resolutions ou fallbacks pour certains IDs d'entites et de block entities.

Exemples visibles :

- `minecraft:player` via resolution runtime avec fallback/override env
- `minecraft:item` via resolution runtime avec override possible
- block entity types deduits dynamiquement

Cela reste acceptable pour un MVP reseau, mais fragile pour une reproduction Vanilla stricte.

### 7. Import Anvil et runtime n'utilisent pas la meme abstraction native

Anvil arrive sous forme :

- palettes
- `block_states.data`

mais une fois importe :

- tout est immediatement converti en tableau plat de `state_id`

Cela simplifie le MVP, mais perd au passage une abstraction proche du modele Vanilla par section.

## Implications pratiques pour la suite

Pour les evolutions futures, il faut garder en tete que le systeme actuel privilegie :

- la simplicite immediate
- le debuggage direct
- une bonne performance brute du tableau plat

Mais il sacrifie :

- la fidelite semantique
- la clarte des transitions d'etat
- la separabilite entre bloc, state et block entity

Une refonte future du systeme de blocs devra probablement choisir explicitement entre :

- conserver un coeur RAM tres simple base sur IDs entiers
- ou introduire une couche semantique stable tout en gardant un layout DOD/cache-friendly

## Reference Java locale disponible

Le depot contient deja une extraction locale du client/decompilation reference dans `mc_vania_asset`.

Point de repere utile constate localement :

- version : `26.1-rc-3`
- nom : `26.1 Release Candidate 3`
- `build_time` : `2026-03-23T11:10:34+00:00`

Cette base locale pourra servir de reference initiale pour la future documentation Vanilla, tant qu'aucune autre source mappee plus complete n'est fournie.

## Resume court

Aujourd'hui, le serveur est deja solide sur sa separation reseau/tick/chargement asynchrone.

Le noeud de dette technique majeur est ailleurs :

- le monde stocke des `block_state_id` Vanilla bruts comme representation universelle
- les comportements de pose et de conteneurs reposent encore sur des heuristiques et des normalisations
- les block entities ne sont pas encore modelisees comme un sous-systeme propre

Cette doc doit donc etre lue comme la photo d'un socle performant mais encore "trop proche du wire format Vanilla" pour la couche gameplay.
