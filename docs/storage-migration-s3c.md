# S3C — Migration hors ligne GoatCore vers Anvil 26.1.1

S3C ajoute un outil C11 indépendant, `mc_world_migrate`. Il n'est pas lié à
`mc_server`, ne change aucun défaut et ne migre aucun monde automatiquement.
Le stockage historique reste `bin`, le mode natif et les sauvegardes S4 restent
expérimentaux ; S2 n'est pas activé. Révision de départ :
`28b3c5e253dfec8da2d5303d7b861463afe8f638`.

## Utilisation et résultat

```sh
make mc_world_migrate
./mc_world_migrate --analyze --source /copie/source --destination /copie/nouveau --source-offline > /rapports/analyse.jsonl
./mc_world_migrate --convert --source /copie/source --destination /copie/nouveau --source-offline > /rapports/conversion.jsonl
./mc_world_migrate --verify --source /copie/source --destination /copie/nouveau --source-offline > /rapports/verification.jsonl
```

Le parent de destination doit exister. La destination doit être **absente** pour
l'analyse et la conversion, et présente pour la vérification. Les rapports doivent
être extérieurs aux deux arbres. La redirection shell peut tronquer un fichier
avant l'exécution : utiliser uniquement des chemins de rapports indépendants.
Aucun privilège administrateur n'est requis. `--source-offline` est une déclaration
explicite de l'opérateur ; elle ne remplace pas l'arrêt du serveur source.

La sortie JSONL décrit chaque entrée, ses coordonnées, région, version, empreinte,
statut et motif de refus, puis un résumé. Le succès requiert code 0 et
`complete=true`. En conversion, `converted=verified=accepted` et
`durable_publication=true` sont également requis. Le manifeste déterministe
`goatcore-migration.json` décrit tous les fichiers et les empreintes source/cible.
Les catégories `kind` sont 0 répertoire, 1 copie ordinaire, 2 chunk, 3 marqueur
de backend et 4 verrou ; une empreinte vide signifie qu’elle n’est pas applicable
ou n’a pas pu être établie pour une entrée refusée.
Il n'est pas à lui seul un reçu de durabilité : il est écrit avant publication.
La vérification compare le checkpoint source à une destination pristine ; elle
n’est pas un outil de réparation d’un monde déjà utilisé. Les essais et
modifications légitimes se font sur d’autres copies, qui peuvent ensuite
différer de ce checkpoint sans devoir être reconverties ou écrasées.

| Code | Signification |
|---|---|
| 0 | Mode terminé et vérifications réussies ; conversion publiée et synchronisée |
| 1 | Refus ou erreur ; consulter le résumé et le chemin de staging éventuel |
| 2 | Arguments, redirection dangereuse ou sûreté du rapport non démontrable ; aucun rapport écrit dans un arbre protégé |
| 3 | Publication visible mais acquittement durable incertain ; conserver et inspecter la destination |
| 4 | Résultat interne réussi mais écriture du rapport final non acquittée |

Ne jamais relancer aveuglément sur une destination existante après un code 3/4 ou
une interruption. La vérifier explicitement, sans en déduire rétroactivement la
résistance à une coupure électrique. Une nouvelle conversion exige un nouveau
chemin absent. Les stagings orphelins sont conservés, jamais promus ou supprimés
automatiquement.

## Formats et politique de propriété

Source : `chunks/c_CX_CZ.bin`, flux zlib/gzip unique et intégralement consommé,
NBT racine Compound complet. Les DataVersion GoatCore **4785 et 4788** sont
acceptées dans le schéma démontré ; il ne s'agit pas d'un DataFixer universel.
Le nom et les coordonnées NBT doivent correspondre. Tous les états doivent
exister dans le registre officiel 26.1.1. Aucun fallback vers l'air.

Destination : `dimensions/minecraft/overworld/region/r.RX.RZ.mca`, compression
zlib, DataVersion Int 4788. Les divisions de région et emplacements couvrent les
coordonnées négatives. Le codec de migration réutilise NBT, registre, lecteurs
historique/natif, index S1 et écriture atomique durable S3A ; il ne recrée pas le
moteur de chunks. Le pipeline S4 n'est pas modifié.

Les seules transformations du NBT sont la DataVersion et le repacking des longs
`block_states.data` compact historique vers SimpleBitStorage aligné Vanilla.
Un packing déjà natif est conservé s'il est valide. Palettes, ordre, propriétés,
sections et tous les champs opaques gardent leurs types et valeurs. Une lecture/
écriture exacte préalable détecte les doublons et données que le parseur ne
pourrait pas conserver. La comparaison C confronte les **98 304 états** du chunk
aux deux lecteurs ; les tests disposent aussi d'un oracle Python indépendant.

| Données | Traitement S3C | Limite |
|---|---|---|
| États officiels, palettes compactes ou alignées | Repacking contrôlé, comparaison exhaustive | Indices, tailles, bits cachés et propriétés invalides refusés |
| Champs NBT inconnus, listes, composants | Conservation typée exacte | NBT ambigu ou hors limites refusé |
| Biomes | Conservation des palettes uniformes `minecraft:plains` démontrées | Autres biomes/encodages refusés ; aucune reconstruction fictive |
| Heightmaps et éclairage | Conservation si tailles/types/bornes validés | Aucune promesse de moteur d'éclairage/worldgen complet |
| Coffre, four froid, panneau, bannière, shulker, tonneau, pot décoré | Conservation opaque native S3B ; blocs porteurs obligatoires | Aucune interaction gameplay nouvelle |
| Anciens inventaires `slot`/count Byte, fichiers sidecars de conteneurs | Refus explicite | Pas de conversion approximative des composants/inventaires |
| Scheduled ticks, four actif, BE inconnue/malformée, protochunk | Refus selon protections S3B | Pas de simulation ni achèvement de génération |
| `.mcc`, codecs/régions préexistantes, journaux S2 | Refus | Pas de transaction multi-fichiers ni mélange des backends |
| Joueurs, permissions et autres fichiers ordinaires | Copie byte-for-byte et vérification SHA-256 | Aucune réparation de cohérence globale, aucune réinitialisation |
| Marqueur du backend et verrous | Marqueur Anvil explicite ; verrous neufs | Transformation consignée, aucun fichier source modifié |

Les champs inconnus sont conservés dans le convertisseur, sans prétendre que
Vanilla conservera lui-même des extensions non officielles lors de sa sauvegarde.
Les données globales absentes ne sont pas inventées : un ensemble de régions
lisibles n'est pas un monde Minecraft complet.

## Exclusivité, limites et préservation

Tous les accès source sont en lecture seule. Le verrou de répertoire propre à
l'outil, le verrou de stockage existant et un verrou OFD sur `session.lock`
excluent les outils coopératifs, GoatCore actuel et le verrou POSIX Vanilla.
Les anciens mondes sans verrou de propriétaire restent subordonnés à la déclaration
hors ligne ; un écrivain non coopératif ne peut pas être rendu sûr par cette API.
Les alias statiques par montage bind sont détectés par les identités
des répertoires et des fichiers de rapport, pas seulement par leurs chemins.
Identités, tailles, modes, dates de modification et SHA-256 sont recontrôlés à
plusieurs barrières, notamment avant publication. L'atime peut changer à la lecture.

Les chemins identiques/imbriqués, liens symboliques, fichiers multi-liens,
fichiers spéciaux, chemins non ASCII/de contrôle, attributs étendus de fichiers
et ambiguïtés de backend sont refusés. Les permissions ordinaires rwx des fichiers copiés
sont préservées ; la racine de destination est privée (0700), les autres
répertoires sont initialisés selon le backend ; propriétaires, ACL, dates et métadonnées système ne constituent
pas une sauvegarde administrative. Limites : 100 000 entrées, profondeur 32,
fichier ordinaire 512 Mio, chunk compressé 16 Mio, NBT 8 Mio, profondeur NBT 64.
Un chunk nécessitant plus de 255 secteurs est refusé avant création du staging.
L’espace annoncé reste une estimation : blocs de répertoire, manifeste,
allocation du système de fichiers et écritures concurrentes exigent une marge ;
le contrôle préalable ne garantit pas qu’un ENOSPC ultérieur soit impossible.
Les limites sont des refus documentés, pas des troncatures. L'outil suppose un
système de fichiers Linux local fiable et un répertoire parent sous contrôle de
l'opérateur ; il ne prétend pas se protéger d'un administrateur hostile, de
montages changeants ou d'un matériel qui ment sur les synchronisations.

## Construction et publication durables

1. Inventorier et valider tous les fichiers ; aucune conversion partielle acceptée.
2. Créer un staging privé unique dans le parent de destination.
3. Copier les fichiers annexes, convertir par région avec lots bornés (64 chunks,
   32 Mio de NBT) et réutiliser le remplacement atomique durable existant.
4. Écrire le manifeste ; relire chaque région et comparer exactement le NBT
   transformé, les copies et la liste des fichiers (aucun extra accepté).
5. Synchroniser tous les fichiers et répertoires, de bas en haut ; revérifier
   l'intégrité et l'identité de la source.
6. Publier avec `renameat2(RENAME_NOREPLACE)` puis synchroniser le parent.

Avant publication, toute panne laisse la source intacte et la destination finale
absente ; des fichiers temporaires peuvent subsister. Après renommage, le contenu
est cohérent mais le nom n'est durablement acquitté qu'après fsync du parent.
Une erreur de renommage après effet visible ou de synchronisation du parent
produit le code 3 : aucune restauration destructrice n'est tentée. La synchronisation
du répertoire ne remplace pas celle des fichiers. SIGKILL teste l'interruption de
processus, **pas** la perte de caches d'une panne électrique.

## Procédure de migration réelle et rollback

Le convertisseur a été validé sur le volume du serveur Xeon. La validation de
charge S5 et les autres critères de production Xeon/HDD RAID restent obligatoires.
Aucune opération de cette liste
n'a été effectuée sur les services ou mondes principaux dans S3C.

1. Obtenir une autorisation explicite d'arrêt et de bascule ; identifier le
   service, le monde, le format et le point de coupure.
2. Arrêter proprement le service ; sauvegarder intégralement monde, joueurs,
   configurations et permissions dans une destination distincte. Vérifier tous
   les fichiers copiés par SHA-256 et tester la restauration sur une copie.
3. Analyser la copie hors ligne ; résoudre chaque refus. Dimensionner l'espace
   pour source, sauvegarde, staging et temporaires de régions.
4. Convertir vers une destination indépendante absente et vérifier l'intégralité.
5. Tester des copies sous GoatCore Anvil, sauvegarde, redémarrages, inventaires,
   multijoueur, puis Vanilla avec ses métadonnées réelles conservées.
6. Valider charge, durabilité et arrêts sur Xeon/HDD RAID, avec manifestes,
   empreintes du binaire et critères explicites ; préparer le rollback.
7. Après une nouvelle autorisation de bascule seulement, sélectionner explicitement
   la destination et sa configuration. Garder source et sauvegarde inchangées.
8. Pour revenir : arrêter le service, sauvegarder aussi la destination récente,
   restaurer la configuration et sélectionner la source conservée. Les mutations
   postérieures à la bascule ne sont pas fusionnées automatiquement ; conserver
   les deux historiques et documenter les pertes temporelles du retour.

Les objets au sol et inventaires n'ont pas de transaction globale avec les chunks.
Une migration préserve un checkpoint existant ; elle ne répare pas rétroactivement
les incohérences antérieures. Aucun ancien `.bin` ne doit être supprimé automatiquement.

## Validation reproductible

Les commandes de tests et les résultats du lot sont décrits dans
`docs/vanilla-compatibility.md`, section S3C, avec le manifeste léger
`tests/fixtures/vanilla-26.1.1-s3c-validation.json`. Les mondes volumineux restent
hors Git sous `tests/stress/results/storage-s3c/`.
