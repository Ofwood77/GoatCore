# Proposal New Block System

Statut : RFC interne

Objectif : remplacer le stockage actuel base sur des `block_state_id` Vanilla bruts par une architecture C data-oriented, compacte et extensible pour le gameplay.

## Probleme a resoudre

Le systeme actuel a plusieurs limites structurelles :

- le chunk en RAM stocke directement des `int32_t block_state_id`
- les IDs Vanilla servent a la fois d'identifiant runtime, d'identifiant de serialisation et de pseudo-modele gameplay
- le systeme ne separe pas proprement :
  - le type de bloc
  - l'etat du bloc
  - la block entity dynamique
- les donnees de conteneurs et de block entities risquent de contaminer les hot paths de scan de chunks

Resultat :

- dette technique forte
- bugs de mapping
- difficultes a reproduire fideltement Vanilla
- difficulte a faire evoluer les structures sans casser l'I/O

## Principes de conception

La proposition repose sur 6 principes.

1. Le runtime C doit avoir ses propres identifiants internes.
2. Le stockage voxel doit etre sectionne en `16 x 16 x 16`.
3. Chaque section doit utiliser une palette locale et un packing dynamique similaire a Vanilla.
4. Les donnees dynamiques de block entities doivent vivre hors du chunk chaud.
5. Le chemin `I/O Anvil / protocole` doit convertir vers et depuis le modele interne, sans en dicter le design.
6. Les compteurs et flags utiles au tick doivent etre precomputes par section.

## Vue d'ensemble de l'architecture proposee

Le nouveau systeme introduit trois couches bien separees :

1. Une palette globale runtime :
   - `global_state_id -> description riche du BlockState`
2. Un stockage chunk/section :
   - `voxel -> local_palette_index`
   - `local_palette_index -> global_state_id`
3. Un store externe de block entities :
   - `position spatiale -> handle`
   - `handle -> donnees denses par composant`

## Couche 1 : Global Palette runtime

### But

Decoupler completement la representation interne des blocs du `block_state_id` Vanilla.

### Type propose

```c
typedef uint16_t mc_block_type_id_t;
typedef uint16_t mc_property_schema_id_t;
typedef uint16_t mc_block_entity_kind_t;
typedef uint32_t mc_global_state_id_t;

typedef struct {
    mc_block_type_id_t block_type;
    mc_property_schema_id_t property_schema;
    uint32_t property_bits;
    uint32_t vanilla_state_id;
    uint16_t flags;
    uint8_t light_emission;
    uint8_t opacity_class;
    mc_block_entity_kind_t block_entity_kind;
    uint16_t reserved;
} mc_block_state_desc_t;
```

### Semantique

`mc_global_state_id_t` devient l'identifiant central du runtime.

Chaque entree de la palette globale decrit :

- le type de bloc logique
- le schema des proprietes applicables
- les proprietes compactees en bits
- l'ID Vanilla equivalent pour l'I/O
- des flags precomputes utiles au gameplay
- le type de block entity requis, si applicable

### Flags recommandes

Le champ `flags` doit contenir des booleens deja precomputes, par exemple :

- `MC_BLOCK_AIR`
- `MC_BLOCK_HAS_FLUID`
- `MC_BLOCK_RANDOM_TICK`
- `MC_BLOCK_HAS_BLOCK_ENTITY`
- `MC_BLOCK_OCCLUDING`
- `MC_BLOCK_REQUIRES_SHAPE_UPDATE`

Ce choix est important : les iterations frequentes ne doivent pas decoder du NBT ni reparcourir des tables de proprietes textuelles.

### Schema de proprietes

Les noms de proprietes et leurs domaines ne doivent pas etre stockes inline dans chaque voxel.

On propose :

- une table `property_schema_id -> schema`
- chaque schema decrit :
  - les proprietes du bloc
  - l'offset bit de chaque propriete
  - le nombre de valeurs possibles

Ainsi :

- `block_type` dit "quel bloc"
- `property_bits` dit "quelle configuration concrete"
- `global_state_id` pointe vers l'entree deja resolue et precomputee

## Couche 2 : Chunk et sections palettees

### Objectif

Conserver le modele gagnant de Vanilla :

- une section = `16 x 16 x 16`
- une palette locale par section
- un `bits_per_block` dynamique

mais l'exprimer en C sans overhead objet ni allocations diffuses.

### Meta-donnees chaudes

Le coeur chaud d'un chunk doit exposer rapidement les informations utiles au tick et au streaming.

```c
#define MC_CHUNK_SECTION_COUNT 24

typedef enum {
    MC_SECTION_UNIFORM = 0,
    MC_SECTION_PALETTED = 1,
    MC_SECTION_DIRECT = 2
} mc_section_mode_t;

typedef struct {
    uint16_t non_air[MC_CHUNK_SECTION_COUNT];
    uint16_t fluid[MC_CHUNK_SECTION_COUNT];
    uint16_t random_tick[MC_CHUNK_SECTION_COUNT];
    uint16_t block_entity_count[MC_CHUNK_SECTION_COUNT];
    uint8_t mode[MC_CHUNK_SECTION_COUNT];
    uint8_t bits_per_block[MC_CHUNK_SECTION_COUNT];
    uint16_t palette_len[MC_CHUNK_SECTION_COUNT];
    uint16_t dirty_mask[MC_CHUNK_SECTION_COUNT];
} mc_chunk_section_meta_t;
```

Cette partie doit rester tres petite et tres cache-friendly.

Un scan de chunk pour savoir quelles sections sont actives ne doit presque jamais toucher les payloads lourds.

### Payload section

```c
typedef struct {
    mc_global_state_id_t uniform_state;
    mc_global_state_id_t palette_inline[16];
    mc_global_state_id_t *palette;
    uint64_t *words;
    uint16_t palette_capacity;
    uint16_t word_count;
} mc_chunk_section_storage_t;

typedef struct {
    mc_chunk_section_meta_t meta;
    mc_chunk_section_storage_t section[MC_CHUNK_SECTION_COUNT];
} mc_chunk_block_store_t;
```

### Pourquoi ce layout

- `meta` est scannee tres souvent
- `section[i]` n'est touchee que si la section est pertinente
- `palette_inline[16]` couvre un grand nombre de sections communes sans allocation
- `palette` pointe soit vers `palette_inline`, soit vers un buffer heap plus grand
- `words` contient le vrai payload bit-packe

### Modes de section

#### `MC_SECTION_UNIFORM`

Cas uniforme :

- `uniform_state` suffit
- `words == NULL`
- `palette_len = 1`
- cout memoire minimal

#### `MC_SECTION_PALETTED`

Cas courant :

- `palette_len > 1`
- `palette` contient des `mc_global_state_id_t`
- `words` contient des indices locaux compactes
- `bits_per_block` varie dynamiquement

#### `MC_SECTION_DIRECT`

Cas de forte diversite :

- la palette locale devient peu rentable
- `words` stocke directement des `mc_global_state_id_t` compactes ou quasi-directes
- ce mode doit rester rare

Le mode direct est l'equivalent C du basculement Vanilla vers la global palette.

## Packing binaire de la section

### Stockage

Le payload `words` encode `4096` entrees.

Chaque entree stocke :

- soit un indice de palette locale
- soit un identifiant global direct

La largeur en bits est identique pour toute la section.

### Cible de performance

Les operations suivantes doivent etre O(1) et branch-light :

- lire un voxel
- ecrire un voxel
- rescanner une section pour recalculer ses compteurs
- serialiser une section vers le protocole ou Anvil

### Index lineaire recommande

Pour rester proche de Vanilla et simplifier les conversions :

```c
index = ((y << 4) | z) << 4 | x;
```

Ce layout donne 4096 cases et reste compatible avec une serialisation section par section.

## API runtime recommandee

Le code gameplay ne doit plus demander ou retourner des `int32_t` Vanilla.

API cible minimale :

```c
mc_global_state_id_t mc_chunk_get_state(const mc_chunk_t *chunk, int x, int y, int z);
void mc_chunk_set_state(mc_chunk_t *chunk, int x, int y, int z, mc_global_state_id_t state);
const mc_block_state_desc_t *mc_state_desc(mc_global_state_id_t state);
```

Le code gameplay peut ensuite interroger :

- le type logique
- les flags
- la presence d'une block entity
- les proprietes compactees

sans redevenir dependant des IDs Vanilla.

## Politique de resize de palette

### Regle generale

Lors d'une ecriture :

1. chercher si le `global_state_id` existe deja dans la palette locale
2. sinon essayer de l'inserer
3. si la largeur en bits doit augmenter, re-encoder la section
4. si la palette locale devient trop grande, passer en mode direct

### Seuils recommandes

La premiere implementation peut reprendre une politique proche de Java :

- uniforme : 1 seul state
- paletted : jusqu'a un certain nombre de bits
- direct : au-dela

Mais contrairement a Java, nous pouvons nous permettre des seuils ajustes au profil reel du serveur C.

Recommandation initiale simple :

- `1 state` -> uniforme
- `2 a 16 states` -> palette locale 4 bits minimum
- `17 a 256 states` -> palette locale dynamique
- `> 256 states` -> mode direct

Ces seuils sont une premiere proposition, pas un dogme. Ils devront etre confirmes par profilage.

## Couche 3 : BlockEntity store externe type ECS

### Objectif

Eviter que les donnees dynamiques lourdes perturbent les scans de blocs et de sections.

### Principe

Le chunk ne stocke pas le contenu complet d'une block entity.

Le chunk ne garde qu'un index spatial compact qui relie une position locale a un handle.

Proposition :

```c
typedef uint32_t mc_block_entity_handle_t;

typedef struct {
    uint16_t *local_index;
    mc_block_entity_handle_t *handle;
    uint16_t count;
    uint16_t capacity;
} mc_section_be_index_t;
```

Chaque section possede donc un petit index sparse :

- `local_index[i]` contient un index `0..4095`
- `handle[i]` pointe vers la store globale des block entities

Comme les block entities sont rares, ce format est plus compact qu'un tableau 4096-entrees, tout en restant cache-friendly.

### Store global des block entities

On separe ensuite les donnees par type logique.

```c
typedef struct {
    int32_t *chunk_x;
    int32_t *chunk_z;
    uint8_t *section_y;
    uint16_t *local_index;
    mc_block_entity_kind_t *kind;
    uint32_t *generation;
    uint32_t count;
    uint32_t capacity;
} mc_be_header_store_t;

typedef struct {
    uint32_t *first_slot;
    uint16_t *slot_count;
    uint8_t *custom_name_present;
} mc_chest_component_t;

typedef struct {
    int16_t *burn_time;
    int16_t *cook_time;
    int16_t *cook_time_total;
    uint32_t *recipe_id;
} mc_furnace_component_t;
```

Idee cle :

- le header commun est dense
- chaque type de block entity a ses tableaux specialises
- les conteneurs ne sont parcourus que lorsqu'on ticke ces conteneurs

### NBT

Le NBT doit etre traite comme une couche de chargement/sauvegarde et de compatibilite.

Deux regles recommandees :

1. Au chargement, parser le NBT vers des composants denses connus.
2. Ne conserver du NBT brut que pour :
   - les types encore non implementes
   - les champs inconnus a roundtrip

Cela evite de trainer une structure NBT lourde dans chaque acces gameplay.

## Separation chaud / froid

### Donnees chaudes

Doivent rester proches des hot paths :

- meta de section
- payload palette/words
- flags de states
- compteurs non-air/fluid/random-tick

### Donnees froides

Doivent rester en dehors :

- noms textuels de proprietes
- schemas debug
- NBT brut
- contenu complet des conteneurs
- metadonnees de serialisation

Cette separation est le coeur du DOD applique au monde Minecraft.

## Couche d'adaptation Vanilla

Le mapping Vanilla doit devenir une couche peripherique.

### Import

Au chargement Anvil ou paquet reseau :

1. lire le `block_state_id` Vanilla
2. le convertir vers `mc_global_state_id_t`
3. inserer ce state dans la section palettee

### Export

Pour la serialisation :

1. lire le `mc_global_state_id_t`
2. retrouver `vanilla_state_id` via la global palette
3. reconstruire la palette reseau ou disque demandee

Le runtime cesse ainsi d'etre prisonnier du format d'echange.

## Compatibilite avec le code existant

Une migration brutale serait risquee. Il faut donc une transition par couches.

### Phase 1 : registre global

- introduire `mc_global_state_id_t`
- generer ou construire la global palette a partir des assets existants
- fournir des helpers `vanilla_state_id <-> mc_global_state_id_t`

### Phase 2 : storage de section

- introduire `mc_chunk_block_store_t`
- conserver temporairement des wrappers compatibilite pour les API actuelles
- faire fonctionner lecture/ecriture chunk sans changer tout le gameplay d'un coup

### Phase 3 : conversion des call sites

- remplacer progressivement les usages de `int32_t block_state_id`
- faire raisonner le gameplay sur `mc_global_state_id_t` + `mc_state_desc(...)`

### Phase 4 : externalisation des block entities

- sortir les block entities et conteneurs du chunk chaud
- introduire les index spatiaux de section
- migrer les conteneurs vers des composants denses

### Phase 5 : nettoyage

- supprimer les anciens chemins qui supposent que l'ID Vanilla est le coeur du runtime
- reduire les conversions au strict perimetre I/O

## Benefices attendus

### Correctness

- meilleure separation des concepts
- moins de bugs de mapping
- parite Vanilla plus facile a raisonner

### Performance

- moins de memoire lue dans les scans de chunks
- sections uniformes tres bon marche
- meilleures chances de skip des sections inactives
- meilleur cache behavior grace a la separation chaud/froid

### Evolutivite

- block entities plus simples a faire evoluer
- gameplay plus simple a brancher sur des flags et descriptions de states
- serialisation et runtime enfin decouples

## Risques et points a arbitrer

### Largeur des identifiants

`mc_global_state_id_t` est propose en `uint32_t` pour rester confortable. Si le registre final reste petit, une reduction vers `uint16_t` pourra etre evaluee plus tard.

### Palette inline

Une inline palette de 16 entrees est un bon depart, mais ce seuil doit etre mesure sur des mondes reels.

### Index spatial des block entities

Un tableau trie par `local_index` est tres compact et probablement suffisant. Si les profils montrent trop de recherches, un petit hash ouvert par section pourra etre evalue plus tard.

### Format disque interne

La premiere migration peut continuer d'utiliser le format disque actuel avec adaptation. Un nouveau format de chunk interne ne doit etre envisage qu'une fois le runtime stabilise.

## Recommendation finale

La meilleure direction pour ce projet est :

1. adopter immediatement un `mc_global_state_id_t` interne
2. introduire un stockage par section palettee
3. sortir les block entities dans une store dense externe
4. releguer les `block_state_id` Vanilla au bord du systeme

Cette architecture reprend le bon principe Vanilla, mais l'adapte a un serveur C oriente cache CPU, profiling et forte charge simultanee.
