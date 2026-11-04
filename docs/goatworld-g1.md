# GoatWorld G1 — prototype hors ligne, format expérimental v1

G1 est un outil C11 de comparaison, pas un backend de `mc_server`. Les défauts
`.bin`, les modes Anvil S4 et S2 ne changent pas. Aucun import automatique ni
migration de monde. Le format et les extensions ne sont pas stabilisés.

## Construction et commandes

Les dépendances facultatives des seuls outils sont SQLite3, LZ4 et Zstandard,
en plus de zlib/OpenSSL/pthread existants. Installer leurs en-têtes dans un SDK
privé si nécessaire ; `G1_CPPFLAGS=-I/chemin/sdk/usr/include` permet de les choisir.
Les bibliothèques partagées sont liées par leurs SONAME Linux. La compilation
ordinaire du serveur ne dépend pas de ces nouvelles bibliothèques.

```sh
make mc_goatworld mc_goatworld_fault test_goatworld
make goatworld-check
# Inclure aussi G1 dans le contrôle complet :
make -B precommit-check GOATWORLD_G1_CHECK=1
./mc_goatworld --experimental-disposable versions
./mc_goatworld --experimental-disposable unpack /copies/region /copies/corpus-absent
./mc_goatworld --experimental-disposable import /copies/corpus /copies/store-absent native zstd3 64
./mc_goatworld --experimental-disposable verify /copies/store-absent native
./mc_goatworld --experimental-disposable compact /copies/store-absent native
./mc_goatworld --experimental-disposable export /copies/store-absent native /copies/export-absent
```

Les parents doivent exister, les destinations de création être absentes. Les
rapports doivent rester extérieurs aux arbres protégés (une redirection shell
peut tronquer avant l'exécution). Source/destination identiques, imbriquées,
liens et alias de répertoires ambigus sont refusés. Les accès source sont en
lecture seule ; la lecture peut modifier l'atime. Le parent est supposé privé,
sur un système Linux local fiable, sans administrateur hostile ni montage mouvant.

`unpack` produit des `c_X_Z.nbt` exacts. L'import archive des racines Compound
entièrement décodables et réencodables sans perte, DataVersion Int 4788,
coordonnées concordantes, Status `minecraft:full`. Les autres DataVersion,
protochunks, codecs MCA non pris en charge et références externes sont refusés,
sans régénération ni conversion implicite. Les BE et champs inconnus restent
opaques, sans prétention de simulation ou de modification gameplay sûre.

`native`, `sqlite`, `anvil` sélectionnent trois adaptateurs de l'API indépendante
`mc_goatworld.h`. Un batch est limité à 64 chunks, 32 Mio et une région. Les
snapshots entrants sont indépendants, les générations natives/SQLite croissantes,
les préimages vérifiées par SHA-256. Anvil ne stocke pas de génération G1 : son
adaptateur retourne zéro et vérifie la préimage ; les générations du pipeline S4
restent gérées par son amont existant. Le tag de benchmark permet seulement de
reprendre les commandes de mutation de test sur cet adaptateur. L'écrivain appartient à un seul pthread. Un verrou global
exclusif interdit deux écrivains, même dans deux processus ; les lecteurs prennent
un verrou partagé, donc ne coexistent pas avec un écrivain. Aucun nouveau worker.
Les données de retour appartiennent à l'appelant, le cache de NBT est borné.

## Trois architectures

- **Anvil S4** réutilise directement `mc_anvil_write_batch_expected`, le même
  écrivain atomique groupé que S4, et l'index S1 pour les lectures. zlib6 impose la
  même compression que la comparaison principale. Il reconstruit la région et
  conserve ses deux barrières de durabilité. Ce test isole les backends ; le
  worker et l'amont gameplay S4 ne sont pas simulés par un nouveau serveur.
- **Native** conserve les anciennes données, ajoute des enregistrements, puis
  publie un index complet par remplacement atomique. Accès direct par région et
  slot, sans balayage du monde pour une lecture. Pas de journal à rejouer pour
  les transactions ordinaires ; seul l'index publié fait autorité.
- **SQLite** utilise une seule connexion, WAL, synchronous FULL, checkpoint
  automatique désactivé et checkpoint TRUNCATE vérifié à la fermeture. Aucun
  benchmark NORMAL/OFF n'est présenté comme équivalent. Un fsync du répertoire
  suit le COMMIT. Une table WITHOUT ROWID indexe `(dimension,x,z)`.

SQLite WAL FULL et le rôle du checkpoint sont documentés par
[SQLite WAL](https://www.sqlite.org/wal.html) et
[PRAGMA synchronous](https://www.sqlite.org/pragma.html#pragma_synchronous).
Les versions livrées par les hôtes sont consignées dans les manifestes. La famille
de défauts WAL-reset signalée par SQLite requiert plusieurs connexions écrivant/
checkpointant simultanément ; G1 exclut ce scénario par sa connexion unique,
son propriétaire pthread et son verrou global. Une future concurrence G2 exige
une version corrigée vérifiée, pas une supposition sur les correctifs de distribution.

## Format natif v1

Tous les entiers du conteneur sont **big endian**, tailles fixes, sans sérialiser
une structure C ni son padding. Les coordonnées sont int32 signés ; division
floor et slot `floorMod(x,32)+32*floorMod(z,32)`. Dimensions prototypes 0 Overworld,
1 Nether, 2 End ; seul l'import/export Overworld est validé. Aucune donnée de
worldgen n'est inventée.

La racine contient `GOATWORLD-G1` (64 octets) : magic `G1WORLD1` à 0, version u32
à 8, backend u32 à 12, UUID aléatoire 16 octets à 16, SHA-256 des 32 premiers
octets à 32 ; `OWNER.lock` assure l'exclusivité. La version du conteneur et la
DataVersion Minecraft sont distinctes. Une identité d'une autre racine est refusée.

Par région : `d.D.r.RX.RZ.owner` (garde de présence vide durable),
`d.D.r.RX.RZ.idx` et `d.D.r.RX.RZ.data.ID` (ID hexadécimal aléatoire 64 bits).
L'absence d'index avec présence de `.owner` est une erreur, jamais un chunk vide.
Les entrées périmées occupent de la place jusqu'au compactage.

L'index fait **32 896 octets**, en-tête 128 puis 1 024 entrées de 32 octets :

| Offset en-tête | Type / contenu |
|---:|---|
| 0 | Magic 8 octets `GWIDX001` |
| 8,12,16,20 | u32 version=1, taille=128, DataVersion=4788, dimension |
| 24,28 | int32 RX,RZ |
| 32,40,48 | u64 génération index, ID données, fin engagée |
| 56,60 | u32 nombre=1024, taille entrée=32 |
| 64 | UUID racine 16 octets |
| 80 | 16 octets réservés zéro |
| 96 | SHA-256 index entier, ce champ mis à zéro pour le calcul |

Une entrée contient offset u64 à 0, longueur enregistrement u32 à 8, longueur
NBT u32 à 12, génération chunk u64 à 16, codec u8 à 24, layout u8 à 25,
niveau i8 à 26, réservés zéro jusqu'à 32. Une entrée absente est entièrement
nulle. Bornes, débordements, réserves, secteurs chevauchants, identité et fin
engagée sont contrôlés avant utilisation ; les fichiers tronqués sont refusés.

L'en-tête données de 128 octets commence par `GWLOG001`, reprend version,
taille, DataVersion, dimension, RX/RZ, ID à 40 et UUID à 64 ; réserves zéro,
SHA-256 des 96 premiers octets à 96.

L'enregistrement fait **96 octets d'en-tête** puis le payload encodé : magic
`GWREC001`, version u32 à 8, taille=96 à 12, int32 x/z à 16/20, dimension u32
à 24, NBT brut u32 à 28, longueur encodée u32 à 32, codec/layout/niveau à
36/37/38, réservé à 39, génération u64 à 40, SHA-256 du NBT brut à 48,
CRC32 des 80 premiers octets à 80, 12 réservés zéro à 84.

NBT brut limité à 8 Mio, entrée encodée bornée, corpus CLI à 100 000 chunks et
512 Mio de NBT. Le début de chaque transaction append est aligné **au prochain
bloc de 4 Kio après EOF**, jamais dans le dernier bloc engagé. Ce choix préserve
les blocs anciens pendant l'append et coûte de l'espace ; ce n'est pas une preuve
d'atomicité des secteurs matériels ni une garantie universelle de toute taille
physique de bloc. Les deux systèmes de fichiers testés exposent 4 096 octets ;
les tailles supérieures restent non qualifiées. Les enregistrements du batch sont contigus.

## Encodage et compression

Codecs 0 brut, 1 zlib, 2 LZ4 block, 3 Zstandard. Les paramètres sont stockés par
record, pas imposés au monde entier. Profils étudiés : none, zlib1, zlib6, lz4,
zstd1/3/9, lz4-16k, zstd3-16k, none-pack, lz4-pack, zstd3-pack. Les décodeurs
valident longueurs et consommation intégrale lorsque le codec le permet.

Layout bit 0 : trames indépendantes de 16 Kio, préfixe u32 taille=16384 puis u32
nombre, chaque trame ayant u32 brut et u32 encodé. C'est une étude de granularité
par octets ; elle ne donne pas encore une lecture ciblée de section Minecraft.
Layout bit 1 : préfixe `GPK0` (identité) ou `GPK1` (palette réversible), u32 taille
intermédiaire, puis compression de ce payload.

Le packing optionnel transforme seulement les tableaux Vanilla alignés de
`block_states.data`, en conservant palettes et ordre, propriétés et tous les
champs NBT. Le minimum quatre bits de Vanilla peut devenir un bit pour deux
valeurs ; une section uniforme ne gagne pas un tableau inutile. La reconstruction
restitue exactement le NBT original et vérifie son SHA. Toute section non
représentable sans perte fait choisir **GPK0 pour tout le chunk** ; les padding
non nuls ne sont pas jetés. Le coût CPU du parse/repacking est mesuré séparément.
Pas de dictionnaire entraîné ni de séparation sémantique NBT opaque/sections
annoncée comme implémentée ; ce sont des études possibles pour G2.

## Contrat de durabilité et récupération

Un succès d'écriture signifie que les barrières requises ont réussi. Pour native :

1. Vérifier génération et préimage de tous les chunks du batch.
2. Ajouter les nouveaux enregistrements sans écraser les anciens ; contrôler
   écritures courtes/EINTR et synchroniser les données.
3. Écrire et synchroniser un index temporaire avec le composant atomique existant.
4. Renommer l'index et synchroniser le répertoire racine.
5. Acquitter, invalider les caches concernés. **Trois synchronisations en régime
   établi**, plus les barrières de création initiale de région.

Avant publication, l'ancien index reste entier. Après publication il désigne
uniquement des données déjà synchronisées. En cas de crash du processus, ouvrir
l'index courant suffit : les queues non référencées ne sont jamais promues.
Une erreur après rename avant fsync du parent retourne **-2 / CLI code 3**,
résultat durable incertain ; aucun rollback destructeur. Toute erreur d'écriture
empoisonne le handle, il faut fermer et rouvrir. Les erreurs de fermeture et de
checkpoint sont contrôlées, pas converties en succès.

Le compactage valide chaque record, le copie dans un nouveau fichier indépendant,
synchronise fichier et répertoire, publie un index atomique, puis supprime l'ancien
fichier et resynchronise le répertoire (normalement cinq synchronisations).
Les orphelins de données reconnaissables ne sont nettoyés qu'après resynchronisation
du fichier/index/répertoire courant. Les temporaires d'index après crash restent
conservés, ignorés et jamais promus ; G1 n'est pas un service complet de maintenance.
Une première création interrompue avant publication de l'index peut laisser une
garde `.owner` sans index : **refus explicite**, inspection/discard du corpus jetable,
pas une réparation arbitraire. G2 doit traiter cette disponibilité initiale.

Les barrières protègent dans les limites du système de fichiers et du matériel.
SIGKILL ne simule pas la perte des caches d'une panne électrique. Ni corruption
matérielle réparée, ni transaction globale chunks/joueurs/entités, ni récupération
silencieuse d'un index corrompu. Pas de promesse de survie à un matériel mentant
sur les synchronisations. Les checksums détectent, ils ne réparent pas.

## Import/export et périmètre de compatibilité

Chaque export `.mca` utilise le remplacement atomique durable existant, puis relit
et compare tous les NBT byte-for-byte. Les fichiers sont lisibles sans outil
propriétaire après export. Le stockage natif n'est pas directement lisible par
Vanilla. L'export est **chunks seulement**, pas monde complet ; plusieurs régions
ne sont pas publiées dans une transaction globale. Un échec peut laisser un
répertoire de sortie partiel, sans reçu de succès. Ne pas le promouvoir comme
monde valide ni écraser une destination existante. L'outil S3C reste l'outil de
migration hors ligne avec publication globale vérifiée pour son propre périmètre.

Les données globales, dimensions non testées, entités, POI, joueurs, inventaires,
avancements et statistiques ne sont pas encapsulés par G1. Les essais Vanilla
composent explicitement des copies avec leurs métadonnées de référence ; ils
n'autorisent pas une migration réelle. Les protochunks et .mcc restent protégés.
Les NBT inconnus sont exacts avant Vanilla ; Vanilla peut normaliser/supprimer
ses propres extensions lors d'une sauvegarde, distinction conservée dans les preuves.

## Reproduction des mesures

`tests/stress/prepare_goatworld_corpus.py` prépare des corpus identifiés par
empreintes (sources historiques/officielles distinctes des synthétiques).
`tests/stress/profile_goatworld.py` utilise les mêmes chunks, compression zlib6,
quantités et ordre d'actions par backend. Cent modifications isolées puis cent
distribuées touchent uniquement un tag de benchmark, pas le gameplay. Les états
finaux sont comparés, y compris une empreinte commune entre backends. Trois
répétitions, ordre de backends tournant ; sorties brutes JSONL conservées.

Les passes chronométrées n'ont pas de preload. Des passes comptées séparées
observent write/pwrite/pwrite64, fsync/fdatasync et `/proc/self/io` ; les CSV sont
rapprochés des compteurs, sans événements perdus. `st_blocks*512` mesure
l'allocation de fichiers, pas les écritures physiques du RAID. Les données WAL
et SHM ouverts puis fermés, le drainage/checkpoint et le compactage sont inclus.

Lectures séquentielles après fadvise(DONTNEED) : mincore vérifie la résidence
réelle ; aucune purge globale ni cache de contrôleur froid prétendu. Lectures
aléatoires/voisines chaudes, prélecture et cache applicatif borné sont séparés.
Cache index natif/Anvil : huit régions ; SQLite page cache 4 Mio ; cache NBT optionnel
8 Mio/64 entrées. Les budgets internes diffèrent et la mémoire réelle est publiée.
Les P99 avec peu d'échantillons ne sont pas une garantie de queue statistique.
Les coûts complets incluent ouverture, création, seed, écritures durables,
vérifications et compactage, mais aussi les inventaires/conseils de cache et
la sortie des échantillons. Les phases isolées servent à distinguer ces coûts ;
aucun gain de TPS n'est déduit d'un outil non intégré.

Les mesures G1 favorisent SQLite WAL FULL comme référence principale de G2 sur
les scénarios réels/sparse ; le natif reste un candidat dense, sans choix de
production ni défaut nouveau. Les résultats, empreintes et limites G1 figurent dans la section G1 de
`docs/vanilla-compatibility.md` et le manifeste léger de validation. Les mondes
et SDK de test ne sont pas versionnés.

## Suite G2 : intégration SQLite

G2 intègre uniquement la branche SQLite dans `mc_server`, avec le codec G1 et
le pipeline S4. Le natif append-only de G1 demeure un outil séparé. Voir la section
G2 de [vanilla-compatibility.md](vanilla-compatibility.md) pour le schéma réutilisé,
les connexions bornées, le SQLite 3.51.3 épinglé, les checkpoints et l'activation
explicite `chunk-storage=goatworld-sqlite-experimental` + `chunk-save-mode=async`.
Les mesures historiques G1 restent celles de leurs versions de bibliothèques ;
les nouveaux outils et tests G2 utilisent la version corrigée embarquée.
