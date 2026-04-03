# Reference Vanilla Java

Documentation de reference basee sur l'extraction locale `mc_vania_asset`, version `26.1-rc-3`, build `2026-03-23T11:10:34+00:00`.

Important : l'utilisateur parle d'une "snapshot 2026.1", mais l'asset local actuellement disponible dans le depot correspond plus precisement a `26.1-rc-3` au 25 mars 2026. Cette note documente donc le comportement observe dans cette extraction locale, pas une abstraction theorique.

## Classes Java observees

- `net.minecraft.world.level.chunk.ChunkAccess`
- `net.minecraft.world.level.chunk.LevelChunk`
- `net.minecraft.world.level.chunk.LevelChunkSection`
- `net.minecraft.world.level.chunk.PalettedContainer`
- `net.minecraft.world.level.chunk.PalettedContainer$Data`
- `net.minecraft.world.level.chunk.Strategy`
- `net.minecraft.world.level.chunk.Palette`
- `net.minecraft.world.level.chunk.SingleValuePalette`
- `net.minecraft.world.level.chunk.LinearPalette`
- `net.minecraft.world.level.chunk.HashMapPalette`
- `net.minecraft.world.level.chunk.GlobalPalette`
- `net.minecraft.util.SimpleBitStorage`
- `net.minecraft.util.ZeroBitStorage`
- `net.minecraft.world.level.block.Block`
- `net.minecraft.world.level.block.state.BlockState`
- `net.minecraft.world.level.block.EntityBlock`
- `net.minecraft.world.level.block.BaseEntityBlock`
- `net.minecraft.world.level.block.entity.BlockEntity`

## Vue d'ensemble du stockage monde

Le serveur Java ne stocke pas un chunk comme un tableau plat global de `block_state_id`.

Le coeur memoire est organise ainsi :

1. `ChunkAccess` possede un tableau `LevelChunkSection[] sections`.
2. Chaque `LevelChunkSection` represente une section verticale de `16 x 16 x 16`, soit `4096` voxels.
3. Chaque section contient :
   - un `PalettedContainer<BlockState>` pour les blocs
   - un `PalettedContainerRO<Holder<Biome>>` pour les biomes
   - plusieurs compteurs compacts : `nonEmptyBlockCount`, `fluidCount`, `tickingBlockCount`, `tickingFluidCount`
4. Les `BlockEntity` ne sont pas inline dans ces `4096` voxels :
   - `ChunkAccess` garde `pendingBlockEntities : Map<BlockPos, CompoundTag>`
   - `ChunkAccess` garde `blockEntities : Map<BlockPos, BlockEntity>`
   - `LevelChunk` ajoute ensuite des wrappers de tickers et des registres runtime autour de ces block entities

Conclusion : le stockage des voxels est dense, uniforme et compact ; les donnees dynamiques et heterogenes restent hors de la structure chaude des sections.

## LevelChunkSection : unite memoire de base

`LevelChunkSection` est la vraie unite de stockage des blocs en RAM.

Champs importants observes :

- `short nonEmptyBlockCount`
- `short fluidCount`
- `short tickingBlockCount`
- `short tickingFluidCount`
- `PalettedContainer<BlockState> states`
- `PalettedContainerRO<Holder<Biome>> biomes`

Points importants :

- le serveur Java travaille section par section, pas chunk entier monolithique
- les compteurs sont maintenus au fil des `setBlockState(...)`
- `hasOnlyAir()` est un test O(1) grace a `nonEmptyBlockCount`
- la decision "cette section contient-elle du fluide ou du random tick" est egalement O(1)

En pratique, cette structure permet d'eviter de rescanner `4096` cases pour savoir si une section merite d'etre tickee, serialisee ou consideree comme vide.

## PalettedContainer : principe general

`PalettedContainer<T>` est le mecanisme generique utilise pour stocker compactement un volume d'entrees discretes.

Pour les blocs :

- `Strategy.createForBlockStates(...)` fixe `bitsPerAxis = 4`
- l'indexation couvre donc `16 x 16 x 16 = 4096` entrees

Pour les biomes :

- `Strategy.createForBiomes(...)` fixe `bitsPerAxis = 2`
- l'indexation couvre donc `4 x 4 x 4 = 64` entrees

L'index lineaire d'un voxel est calcule par `Strategy.getIndex(x, y, z)` :

```text
((y << bitsPerAxis) | z) << bitsPerAxis | x
```

Pour les blocs, cela equivaut a une adresse compacte sur `4096` slots.

## Structure interne de PalettedContainer

Le conteneur possede trois elements clefs :

- `volatile PalettedContainer$Data<T> data`
- `Strategy<T> strategy`
- `ThreadingDetector threadingDetector`

Le record `PalettedContainer$Data<T>` contient :

- `Configuration configuration`
- `BitStorage storage`
- `Palette<T> palette`

Autrement dit, un container actif est toujours :

1. une configuration de taille de palette
2. une table locale qui mappe `local_id -> valeur T`
3. un stockage compacte qui mappe `index_voxel -> local_id`

Ce point est central : la section ne stocke pas directement la valeur finale. Elle stocke un petit indice local, dont la largeur en bits varie dynamiquement.

## Palette locale et bits-per-block dynamiques

Le mecanisme Vanilla ajuste la largeur en bits selon le nombre de valeurs distinctes presentes dans la section.

### Cas des blocs

Le comportement observe dans `Strategy$1` est :

- `0 bit` : `SingleValuePalette`
- `1..4 bits demandes` : force `LinearPalette` en `4 bits`
- `5 bits` : `HashMapPalette`
- `6 bits` : `HashMapPalette`
- `7 bits` : `HashMapPalette`
- `8 bits` : `HashMapPalette`
- `> 8 bits` : `Configuration.Global`

En clair :

- section uniforme -> zero bits effectifs
- petite diversite -> palette lineaire de 4 bits
- diversite moyenne -> palette hash map de 5 a 8 bits
- section tres variee -> bascule vers le registre global

Cette decision est prise par `Strategy.getConfigurationForBitCount(...)`.

### Cas des biomes

Le comportement observe dans `Strategy$2` est :

- `0 bit` : `SingleValuePalette`
- `1 bit` : `LinearPalette`
- `2 bits` : `LinearPalette`
- `3 bits` : `LinearPalette`
- `> 3 bits` : `Configuration.Global`

Comme les biomes sont stockes sur une grille `4 x 4 x 4`, les seuils sont differents.

## BitStorage : le vrai stockage compact

La palette seule ne suffit pas. Les `4096` voxels doivent ensuite etre compactes.

Java utilise pour cela l'interface `BitStorage`, avec deux implementations observees :

- `ZeroBitStorage`
- `SimpleBitStorage`

### ZeroBitStorage

Utilise quand la section est uniforme.

Caracteristiques :

- pas de vraie donnees utiles par voxel
- toute lecture renvoie implicitement l'entree `0`
- cout memoire minimal

Ce cas couvre les sections entieres d'air, d'eau uniforme, de stone uniforme, etc.

### SimpleBitStorage

Utilise des que la section a besoin d'une vraie largeur en bits.

Champs importants observes :

- `long[] data`
- `int bits`
- `long mask`
- `int size`
- `int valuesPerLong`
- constantes de division optimisees : `divideMul`, `divideAdd`, `divideShift`

Concretement :

- les indices locaux sont empaquetes dans un tableau de `uint64` Java (`long[]`)
- le nombre de bits par entree est constant a l'echelle de la section
- le calcul `slot -> long_index + bit_offset` est optimise pour limiter le cout des divisions

Le coeur de la compaction Vanilla est donc : palette locale + packing bitfield dans des mots de 64 bits.

## Cycle de vie d'une section palettee

### Creation

Le constructeur `PalettedContainer(T defaultValue, Strategy<T>)` :

1. choisit une configuration initiale avec `createOrReuseData(null, 0)`
2. alloue soit `ZeroBitStorage`, soit `SimpleBitStorage`
3. cree la palette correspondante
4. y insere la valeur par defaut

Pour une section de blocs vide, la representation initiale est donc naturellement "uniforme".

### Lecture

`get(x, y, z)` :

1. convertit `(x, y, z)` en index lineaire via `strategy.getIndex(...)`
2. lit l'indice local dans `BitStorage`
3. convertit cet indice local en valeur `T` via `palette.valueFor(...)`

### Ecriture

`set(...)` ou `getAndSet(...)` :

1. demande a la palette un identifiant local pour la nouvelle valeur
2. si la palette n'a plus de place, `onResize(...)` est appelee
3. `onResize(...)` alloue une nouvelle `Data`
4. l'ancien contenu est recopie et re-encode dans la nouvelle representation
5. l'indice local final est ecrit dans `BitStorage`

La palette est donc un mecanisme dynamique, pas un format fixe.

## Resize et repack

Le chemin `onResize(...)` est essentiel.

Quand le nombre de valeurs distinctes depasse la capacite courante :

- une nouvelle `Configuration` est choisie
- une nouvelle palette est creee
- un nouveau `BitStorage` est alloue
- l'ancien contenu est re-encode voxel par voxel vers la nouvelle representation

Le meme principe apparait dans `unpack(...)` :

- la configuration serialisee peut ne pas etre la meme que la configuration memoire cible
- `PackedData` peut donc etre repacke en RAM lors du chargement

Cela signifie que Java se reserve le droit d'utiliser un format de stockage optimal en RAM, distinct du format brut lu sur le fil ou depuis le codec.

## Serialisation reseau / codec

Sur le chemin `FriendlyByteBuf`, `PalettedContainer.read(...)` et `write(...)` montrent la logique suivante :

1. un octet `bitsPerEntry` est lu/ecrit
2. la palette est lue/ecrite
3. le tableau de `long` empaquete est lu/ecrit

La representation serialisee contient donc :

- une largeur en bits
- les entrees de palette
- le payload packe

Ce format est exactement ce qui rend possible une section tres compacte pour les paquets de chunk.

## Format Disque Anvil (NBT)

La persistance disque recente passe par `net.minecraft.world.level.chunk.storage.RegionFile`, `RegionFileStorage` et surtout `net.minecraft.world.level.chunk.storage.SerializableChunkData`.

Le flux general observe est :

- `RegionFile.getChunkDataInputStream(ChunkPos)` lit l'entree brute dans le `.mca`
- `SerializableChunkData.parse(LevelHeightAccessor, PalettedContainerFactory, CompoundTag)` decode l'arbre NBT racine
- `SerializableChunkData.read(...)` reconstruit ensuite les `LevelChunkSection[]`, les light layers, les block entities et les autres metadonnees de chunk
- `SerializableChunkData.write()` recompose le `CompoundTag` racine avant ecriture disque

Important pour l'agent I/O : le format de region (`.mca`) et l'arbre NBT du chunk sont deux couches differentes. Pour la phase 2, la doc ci-dessous ne couvre que le contenu NBT du chunk une fois l'entree de region decompressionnee.

### Racine du chunk

Les cles NBT racine observees dans `SerializableChunkData` incluent au minimum :

- `xPos`
- `zPos`
- `Status`
- `LastUpdate`
- `InhabitedTime`
- `isLightOn`
- `Heightmaps`
- `sections`
- `block_entities`
- `block_ticks`
- `fluid_ticks`
- `PostProcessing`
- `structures`

Des cles optionnelles existent aussi selon l'etat du monde et de la generation :

- `UpgradeData`
- `blending_data`
- `below_zero_retrogen`
- `carving_mask`
- `entities`

Pour notre future implementation C, il faut retenir que `sections` et `block_entities` sont les deux branches critiques pour reconstruire le layout bloc + donnees dynamiques.

### Structure exacte d'une section

Le chemin racine est :

- `sections` : `ListTag<CompoundTag>`

Chaque element de `sections` represente une section verticale. Les champs observes dans `SerializableChunkData.parse(...)` sont :

- `Y` ou `yPos` logique de section : dans l'extraction locale, le parse lit `yPos` comme octet de section
- `block_states`
- `biomes`
- `BlockLight` optionnel
- `SkyLight` optionnel

Le chemin exact pour les blocs est donc :

- `sections[]/block_states`

et, a l'interieur de ce compound :

- `sections[]/block_states/palette`
- `sections[]/block_states/data`

Interpretation :

- `palette` est la palette locale de la section
- `data` est le `LongArray` bit-packe des indices de palette

La forme conceptuelle est la meme que celle documentee plus haut pour `PalettedContainer` :

- si la section est uniforme, la palette peut ne contenir qu'une seule entree et `data` peut etre absent ou vide selon le codec
- sinon `data` contient les indices locaux compactes en `long[]`

Les blocs sont donc sauvegardes par section `16 x 16 x 16`, pas comme un tableau plat de `4096 * 24`.

Pour les biomes, le schema miroir observe est :

- `sections[]/biomes/palette`
- `sections[]/biomes/data`

Ce n'est pas l'objectif principal de la phase 2 immediate, mais l'agent I/O doit savoir que la section NBT transporte deja aussi le conteneur palette des biomes.

### Details de la palette `block_states`

Le codec utilise par `SerializableChunkData` pour les blocs est le conteneur de block states fourni par `PalettedContainerFactory.blockStatesContainerCodec()`.

En pratique, chaque entree de `sections[]/block_states/palette` correspond a un `BlockState` complet, pas juste a un nom de bloc. La forme logique attendue est celle deja visible dans les chunks Vanilla modernes :

- `Name: "minecraft:stone"`
- `Properties: { ... }` si le state porte des proprietes

Exemples de chemins utiles :

- `sections[]/block_states/palette[0]/Name`
- `sections[]/block_states/palette[0]/Properties/facing`
- `sections[]/block_states/data`

L'agent I/O devra donc :

- decoder la palette dans l'ordre exact du NBT
- convertir chaque entree palette -> `mc_global_state_id_t`
- decoder `data` comme indices locaux vers cette palette

### Block Entities

Les block entities ne sont pas stockees dans `sections[]`.

Le chemin racine observe est :

- `block_entities` : `ListTag<CompoundTag>`

Chaque entree de cette liste represente une block entity individuelle. La reconstruction Java passe ensuite par les APIs de `ChunkAccess` et `BlockEntity` :

- `ChunkAccess.pendingBlockEntities : Map<BlockPos, CompoundTag>`
- `ChunkAccess.blockEntities : Map<BlockPos, BlockEntity>`
- `BlockEntity.saveWithFullMetadata(...)`
- `BlockEntity.getPosFromTag(ChunkPos, CompoundTag)`

Le lien spatial est direct :

- chaque block entity sauvegarde ses coordonnees absolues monde `x`, `y`, `z`
- ces coordonnees ne sont pas locales a la section
- l'appartenance au chunk se verifie avec `chunk_x = floor(x / 16)` et `chunk_z = floor(z / 16)`
- la section cible se deduit ensuite de `section_y = floor(y / 16)`

Consequences pratiques pour l'agent I/O :

- il faut parser `block_entities` independamment des voxels de `sections`
- il faut inserer chaque entree dans un index spatial externe base sur sa position monde
- il ne faut pas essayer de "cacher" le NBT d'une block entity directement dans la section de blocs

Autrement dit, le disque Vanilla confirme tres nettement la separation architecturale entre :

- voxel palette dans `sections[]/block_states`
- donnees dynamiques heterogenes dans `block_entities[]`

### DataVersion

Le `CompoundTag` de chunk ecrit par `SerializableChunkData.write()` passe par un helper qui ajoute la version de donnees courante du jeu. Dans Vanilla, cela correspond au tag racine :

- `DataVersion`

Dans l'asset local `mc_vania_asset/version.json`, la version observee est :

- `id = "26.1-rc-3"`
- `world_version = 4785`

Inference de travail raisonnable :

- pour cette base locale, l'agent I/O doit s'attendre a un `DataVersion` correspondant a la ligne de monde `4785`

Comme nous n'avons pas ici une preuve de bytecode nommant explicitement la constante litterale `4785` dans `SerializableChunkData` elle-meme, la regle sure a documenter est :

- lire `DataVersion` a la racine du chunk si present
- le comparer a la `world_version` de `mc_vania_asset/version.json`
- si la valeur differe, traiter le chunk comme potentiellement issu d'une autre version et activer soit une voie de compatibilite, soit un refus explicite
- si `DataVersion` est absent, considerer le chunk comme legacy/corrompu pour notre pipeline strict

Pour la phase 2, cela donne un contrat clair : la lecture Anvil doit etre version-gatee avant meme de decoder finement les block states et les block entities.

## Threading et acces concurent

`PalettedContainer` possede un `ThreadingDetector`.

Les operations critiques comme `set`, `getAndSet`, `read`, `write` font :

- `acquire()`
- travail
- `release()`

Cela ne transforme pas le conteneur en structure lock-free, mais sert de garde-fou contre des acces concurrents invalides.

Pour notre projet C, cela rappelle une chose importante : la structure compacte du chunk ne doit pas etre polluee par des besoins de synchronisation grossiers. Il faut plutot controler soigneusement le thread proprietaire des mutations.

## Separation Block / BlockState / BlockEntity

L'architecture Vanilla separe tres nettement trois concepts que notre implementation C actuelle melange encore trop.

### Block

`Block` represente le type immuable et les regles de comportement generales.

Elements observes dans la classe :

- `StateDefinition<Block, BlockState> stateDefinition`
- `BlockState defaultBlockState`
- `static IdMapper<BlockState> BLOCK_STATE_REGISTRY`
- `static getId(BlockState)` / `stateById(int)`
- `createBlockStateDefinition(...)`
- `defaultBlockState()`

En pratique :

- un `Block` decrit "quel genre de bloc est-ce"
- il declare l'espace des proprietes valides
- il fournit un `defaultBlockState`

### BlockState

`BlockState` est une instance concrete du bloc avec une combinaison precise de proprietes.

Le constructeur observe prend :

- le `Block`
- la liste des `Property<?>`
- les valeurs `Comparable<?>` associees

En pratique :

- `oak_log[axis=y]`
- `oak_log[axis=x]`
- `chest[facing=north,waterlogged=false,type=single]`

sont des `BlockState` differents d'un meme `Block`.

Le registre global des states (`BLOCK_STATE_REGISTRY`) identifie ces combinaisons concretes, pas seulement le type de bloc.

### BlockEntity

`BlockEntity` represente les donnees dynamiques attachees a une position de bloc particuliere.

Champs observes :

- `BlockEntityType<?> type`
- `Level level`
- `BlockPos worldPosition`
- `BlockState blockState`

Methodes importantes :

- `loadStatic(...)`
- `saveWithFullMetadata(...)`
- `saveWithoutMetadata(...)`
- `getUpdateTag()`
- `setChanged()`

Ce niveau contient donc :

- inventaires
- timers
- donnees NBT
- etat runtime specifique a une case du monde

Un coffre, un four ou un spawner ne doit donc pas etre modele comme un simple entier de bloc.

### Qui decide qu'un bloc a une BlockEntity ?

La relation est exprimee par `EntityBlock`.

`EntityBlock` expose :

- `newBlockEntity(BlockPos, BlockState)`
- eventuellement un ticker
- eventuellement un listener

`BaseEntityBlock` est la base commune pour les blocs qui portent une block entity.

Conclusion :

- `Block` = type et comportement immuable
- `BlockState` = configuration statique choisie parmi des proprietes discretes
- `BlockEntity` = donnees dynamiques par position

Cette separation est une des raisons pour lesquelles Vanilla peut rester a la fois compacte dans les chunks et riche cote gameplay.

## Stockage des BlockEntities dans le chunk Java

Le point clef pour notre future refonte est ici :

- les voxels des sections restent compacts et homogenes
- les `BlockEntity` vivent dans des maps separees
- `pendingBlockEntities` garde du NBT tant que l'objet runtime n'est pas instancie
- `LevelChunk` ajoute ensuite des wrappers de tick et d'enregistrement

Autrement dit :

- le chemin chaud "scan de blocs" ne traverse pas les donnees de coffres
- le chemin froid "ouvrir un coffre" ou "ticker un four" passe par une structure distincte

C'est exactement le genre de separation qu'un serveur C oriente DOD doit reproduire.

## Consequences pratiques pour notre implementation C

La reference Java valide plusieurs directions fortes :

1. Le runtime monde doit raisonner en sections `16 x 16 x 16`, pas en chunk comme unique tableau plat sans semantique.
2. Le stockage des voxels doit passer par une palette locale par section avec largeur en bits dynamique.
3. La representation interne ne doit pas etre limitee a un `block_state_id` brut transporte partout.
4. Les compteurs de section sont cruciaux pour les fast-paths.
5. Les `BlockEntity` doivent rester hors du tableau chaud des voxels.
6. Le mapping vers les IDs Vanilla doit etre traite comme un registre et une couche d'I/O, pas comme le coeur du runtime.

## Resume ultra court

Java optimise les chunks avec trois idees simples mais tres puissantes :

- sectionner verticalement le monde en volumes `16 x 16 x 16`
- stocker chaque section via palette locale + bit packing dynamique
- separer strictement `Block`, `BlockState` et `BlockEntity`

Notre future architecture C doit conserver ces invariants, mais les traduire en structures de donnees encore plus explicites et plus cache-friendly.
