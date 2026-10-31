# Proposition S3C — adoption Anvil et migration hors place

Cette proposition prépare une intervention future. S5 ne convertit aucun monde,
ne déplace aucun `.bin` et ne change aucun défaut de configuration. Le point de
départ est S4 `1f05f9532cdbe1374761695790d4d1a68e014048`.

## Deux décisions indépendantes

L'adoption d'Anvil pour un **nouveau monde** ne nécessite pas de convertir un
ancien monde. Elle nécessite cependant un contrat de durabilité accepté, la
validation Xeon/HDD, les protections S3B et des métadonnées globales cohérentes.
Le mode natif reste expérimental tant que ces conditions ne sont pas remplies.
Une future modification du défaut devra être explicite et concerner seulement
les mondes neufs ; les marqueurs des mondes existants restent contraignants.

La **migration d'un ancien monde** est une opération distincte, hors ligne, vers
une destination séparée. Une sauvegarde de région atomique ne rend pas cette
opération multi-fichiers atomique. L'activation de la destination exige une
autorisation explicite après vérification du résultat concret.

## Différences à inventorier

| Source historique | Destination native | Validation nécessaire |
|---|---|---|
| `chunks/c_CX_CZ.bin`, flux NBT zlib/gzip lu par le codec historique | `dimensions/minecraft/overworld/region/r.RX.RZ.mca` | Nom/coordonnées NBT, limites, doublons, compression, absence d'ambiguïté de backend |
| GoatCore DataVersion 4785 et 4788, lecture historique | Chunk natif strict 4788 | Identifier la provenance exacte ; le nombre DataVersion seul ne décrit pas les conventions internes |
| Palettes historiques, packing compact pouvant traverser les longs | SimpleBitStorage Vanilla avec valeurs alignées dans chaque long | Décoder et comparer les états nommés/propriétés ; repacker les données, ne pas recopier aveuglément le payload |
| Sérialisation historique via `mc_anvil_encode_chunk_nbt` | Encodage natif préservant le NBT source | Ne pas reconstruire un chunk à partir des seuls champs internes si des données opaques seraient perdues |
| Conteneurs et inventaires historiques partiellement interprétés | Block entities opaques protégées par S3B | Conserver types, composants, coordonnées et champs inconnus ; refuser une conversion non démontrée |
| Données de joueurs déjà séparées sous `players/data` | Même organisation 26.1.1 | Conserver UUID, modes d'authentification, inventaires, fichiers associés et permissions ; aucune réinitialisation |
| Métadonnées mondiales/entités/POI éventuellement absentes ou partielles | Organisation complète 26.1.1 | Inventaire explicite ; ne pas prétendre qu'un ensemble de `.mca` est un monde complet |

Le code actuel utilise bien `mc_packed_pack_compact_u32` pour les données
historiques. La présence de NBT et de DataVersion 4788 dans un `.bin` ne suffit
donc pas à prouver qu'un simple emballage dans Anvil serait compatible Vanilla.
Le lecteur natif ne doit pas être élargi silencieusement pour masquer cet écart.

## Protocole proposé

1. Arrêter l'instance source et établir son exclusivité. Capturer la version,
   configuration, marqueur du backend, identité des joueurs et inventaire des
   fichiers. Une copie prise pendant des écritures concurrentes n'est pas une
   sauvegarde de migration cohérente.
2. Faire une sauvegarde intégrale hors place, avec modes/permissions et manifeste
   SHA-256. Relire tous les fichiers copiés et vérifier le manifeste. Prévoir
   assez d'espace pour source, sauvegarde, destination et temporaires de région.
3. Effectuer un audit sans écriture : types NBT, coordonnées, DataVersion,
   palettes, champs opaques, conteneurs, fichiers externes et fichiers globaux.
   Chaque chunk reçoit un statut prêt/refusé et une raison exploitable.
4. Convertir uniquement les cas démontrés dans une destination vide et marquée
   distinctement. Garder le NBT original ; transformer seulement les champs dont
   la propriété est connue. Les types/valeurs opaques doivent rester exacts.
   Refuser états inconnus, ambiguïtés 4785/4788, payloads malformés ou données non
   conservables. Aucun remplacement par air ou génération de secours.
5. Comparer les 98 304 états nommés de chaque chunk, coordonnées négatives et
   frontières comprises. Comparer les BE/inventaires/composants et les champs
   non concernés avec un parseur indépendant. Chaque divergence bloque la
   destination ; aucun fichier source n'est modifié.
6. Tester plusieurs ouvertures/arrêts GoatCore sur une copie de la destination,
   avec `.bin` absent. Vérifier les fichiers joueurs séparément et les refus
   S3B. Réouvrir une autre copie par le JAR officiel 26.1.1 puis vérifier les
   données pertinentes, avec témoins pour les normalisations officielles.
7. Publier un rapport et les empreintes de la destination validée. Après
   autorisation explicite seulement, sélectionner cette destination comme
   monde actif, sans supprimer la source ni sa sauvegarde.
8. Le rollback arrête l'instance et resélectionne la copie source conservée,
   avec sa configuration d'origine. Les nouvelles mutations après activation
   ne se fusionnent pas automatiquement avec l'ancien monde : documenter ce
   point de coupure et préserver aussi la destination avant tout retour.

## Refus et garanties qui restent nécessaires

Les protochunks, `.mcc`, codecs non pris en charge, scheduled ticks et autres
relations non simulées restent protégés conformément à S3B. Ne pas faire
disparaître une catégorie pour obtenir artificiellement une migration réussie.
Un futur convertisseur doit également refuser les liens/chemins ambigus, les
chunks dupliqués et les versions non identifiées, sans écrire dans la source.

Les objets au sol ne disposent pas aujourd'hui d'une transaction globale avec
chunks et inventaires. Le convertisseur ne doit ni inventer des entités perdues
ni promettre une cohérence inter-fichiers absente. Une migration hors ligne
préserve un checkpoint existant ; elle ne répare pas rétroactivement les pertes
antérieures. Des tests d'interruption/reprise de la conversion et de publication
du manifeste seront nécessaires avant de proposer un outil utilisable.

L'organisation globale inclut notamment `level.dat`,
`data/minecraft/world_gen_settings.dat`, les règles/horloges pertinentes,
`players/`, `entities/` et `poi/`. Les fixtures composées des tests S3A/S4 ne
sont pas un générateur de monde complet. Préserver les fichiers disponibles
ne démontre pas encore l'implémentation de tous ces sous-systèmes.

## Conditions de passage

- Validation de charge, erreurs, drainage et redémarrages sur Xeon/HDD RAID.
- Contrats immediate/batched/async documentés sans assimiler ACK et durabilité.
- Corpus source `.bin` 4785/4788 représentatif, avec composants et conteneurs.
- Transformations limitées et comparaisons indépendantes de toutes les données.
- Procédure de sauvegarde/rollback testée sur copies, espace disque borné.
- Aucune bascule de défaut ou activation réelle avant une décision distincte.

La fidélité du terrain à Vanilla reste une question de phase 3 ; ce chantier
ne commence pas worldgen, fluides, mobs ou nouvelles interactions de blocs.
