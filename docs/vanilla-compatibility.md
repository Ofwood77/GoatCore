# GoatCore 26.1.1 — compatibilité Vanilla (phase 1D en cours)

## Phase 1C : cycle de vie et visibilité des joueurs

L'audit du chemin multijoueur a suivi l'entrée en Play, l'accusé de position,
la mise en visibilité sur le tick, les transformations relatives, la
téléportation, la sortie et le chargement de la position à la reconnexion.
Les identifiants des paquets utilisés viennent des rapports générés 26.1.1 du
dépôt : Player Info Update 70, Add Entity 1, Move Entity Pos 53,
Move Entity Pos Rot 54, Move Entity Rot 56, Rotate Head 83,
Teleport Entity 125, Remove Entities 77 et Player Info Remove 69.

Le test de réception `tests/stress/test_player_visibility.py` a été exécuté
sur le serveur distant, d'abord contre l'instance offline 25587 avant la
correction, puis contre un serveur temporaire isolé sur 25589 après compilation.
Deux bots ont reçu l'entrée de profil et l'entité de l'autre, un déplacement
relatif et une rotation de tête. Un saut de position de 9 blocs a produit une
téléportation ; la déconnexion a produit Remove Entities puis Player Info
Remove. La reconnexion a créé une seule nouvelle entité et a restauré la
position sauvegardée. Des groupes de 5, 10, 25 et 50 bots ont chacun reçu
exactement une entité distincte pour chaque autre membre du groupe. Ce sont
des observations du protocole côté bots, pas une validation visuelle par deux
clients Java officiels. Après un arrêt propre et un nouveau démarrage de
l'instance temporaire, le bot testé a retrouvé sa coordonnée X de 9,750.
`make -B precommit-check` a réussi sur la copie distante isolée. La version
ASan+UBSan a passé le scénario à deux joueurs et le groupe de dix joueurs,
ainsi que le test de position après redémarrage, sans diagnostic dans le
journal de cette instance. Ces passages ne couvrent pas tous les entrelacements
des threads.

L'audit a trouvé une conversion de delta en `int16_t` effectuée avant la
décision de téléporter. Pour un déplacement supérieur à la plage relative,
cette conversion était hors plage en C. La conversion se fait désormais
seulement lorsque le paquet relatif est choisi. Le test à 9 blocs couvre ce
chemin. Les sauvegardes existantes et l'identité online/offline n'ont pas été
modifiées.

Restent à vérifier : comparaison de captures binaires avec un serveur Vanilla
26.1.1, visibilité réelle avec deux clients Java, synchronisation détaillée
des animations et postures, filtrage des entités à longue distance, perte de
connexion pendant l'envoi des chunks ou un broadcast, restauration complète
après arrêt et redémarrage (inventaire, santé, faim, expérience et dimension),
et absence de course sur tous les ordonnancements. La phase 1 n'est pas terminée.

Référence des identifiants : `data/26.1.1/reports/packets.json` et les en-têtes
générés du dépôt (protocole 775). Les formats binaires ci-dessous ne sont pas
déduits de ces identifiants : ils correspondent au code effectivement traité.
Une connexion réussie avec le client Python ne prouve **pas** la compatibilité
avec le client Java officiel.

| Paquet | État | Implémenté | Conforme | Testé |
|---|---|---|---|---|
| Intention C→S `0x00` | Handshake | Oui | Partiel : version 775 exigée pour Login, adresse acceptée sans validation complète | Ping/Login Python |
| Status request/response `0x00` | Status | Oui | Partiel : réponse JSON minimale | Test epoll |
| Ping/Pong `0x01` | Status | Oui | Oui pour l'écho des 8 octets | Test epoll |
| Hello C→S `0x00` | Login | Oui | Offline : nom 3–16 ASCII alphanumérique/`_`, UUID client ignoré | Unitaire + intégration |
| Hello S→C `0x01`, Key C→S `0x01` | Login | Oui | RSA 2048, challenge et AES-128-CFB8 sur le flux entier ; authentification Mojang requise | Unitaire, API réelle et connexion officielle Ofwood_ sur 25588 |
| Login finished S→C `0x02` | Login | Oui | UUID officiel après `hasJoined` en online ; UUID offline et `textures` cosmétiques facultatives en offline | Unitaire, intégration et client Java officiel sur 25588 ; rendu des skins non vérifié |
| Login acknowledged C→S `0x03` | Login | Oui | Oui pour paquet vide et transition unique | Unitaire |
| Login compression S→C `0x03` | Login | Non émis | Non testé avec clients officiels ; compression désactivée | Test de framing seul |
| Select known packs S→C `0x0e`, C→S `0x07` | Configuration | Oui | Partiel : liste fixe, réponse bornée et non répétable | Client Python |
| Registry data S→C `0x07` | Configuration | Oui, blobs 26.1.1 | Non validé contre capture Vanilla | Client Python |
| Update tags S→C `0x0d` | Configuration | Oui, blob 26.1.1 | Non validé contre capture Vanilla | Client Python |
| Finish configuration `0x03` dans les deux sens | Configuration | Oui | Partiel : ordre contrôlé, payload C→S vide | Client Python |
| Autres paquets Configuration | Configuration | Partiel/ignorés | Non validé | Non |
| Join game et synchronisation initiale | Play | Partiel | Monde visible avec client officiel ; parité Vanilla non établie | Stress Python et client Java sur 25588 |

## Authentification et données joueur

- `online-mode=false` par défaut. L'UUID offline est maintenant celui de
  `UUID.nameUUIDFromBytes("OfflinePlayer:" + nom)` (MD5, version 3). Le client
  ne choisit plus son UUID. Un seul login par nom (sans distinction de casse)
  ou UUID est admis à la fois, y compris pendant la sauvegarde d'une connexion
  qui se ferme. Les connexions pré-Play expirent après 30 s.
- `offline-skins=false` par défaut. Avec `offline-skins=true`, un seul thread
  interroge les API publiques Mojang pour obtenir le profil et ses textures
  signées ; le tick et les workers epoll ne font aucune requête HTTP. Le cache
  conserve les succès pendant une heure et les échecs pendant une minute ;
  la file est bornée à huit noms, le débit à deux profils par seconde et les
  requêtes HTTPS à 3,5 s chacune. Un échec ou une saturation garde le skin
  Vanilla par défaut. La connexion attend au plus quatre secondes pour son
  skin. L'UUID de connexion, les permissions et les sauvegardes restent
  **offline** : un skin récupéré ne prouve jamais la propriété du compte.
  Les textures signées sont présentes dans Login Success et Player Info Update
  dans les tests de protocole, mais leur affichage avec un UUID offline doit
  encore être vérifié avec deux vrais clients Java 26.1.1 ; certains clients
  peuvent refuser cette association d'identités.
- Anciennes sauvegardes GoatCore : si la sauvegarde au nouvel UUID n'existe
  pas, le serveur lit l'ancien nom de fichier FNV. Il conserve ce fichier et
  écrit les sauvegardes suivantes sous l'UUID corrigé. Un fichier actuel
  invalide provoque une erreur, pas un repli silencieux. Les anciennes
  sauvegardes créées sous un UUID arbitraire fourni par un client (notamment
  tout-zéro pour les bots) ne peuvent pas être attribuées sûrement à un joueur
  et ne sont **pas** migrées automatiquement.
- `online-mode=true` active l'échange RSA/challenge, AES-128-CFB8 continu
  (longueur des trames comprise) puis `hasJoined` sur un pool de quatre
  threads HTTPS. Une session absente, invalide ou indisponible est refusée,
  sans repli offline ; les requêtes vérifient TLS, ont un délai réseau borné à
  10 s et un délai de login de 15 s. Sur 10.0.4.66, la résolution DNS de
  `sessionserver.mojang.com` prend régulièrement environ 5 s à cause d'un
  premier résolveur injoignable ; l'ancien délai réseau de 3,5 s rejetait
  donc une session avant toute réponse HTTP. La correction laisse le tick et
  les workers epoll non bloquants, mais la configuration DNS du serveur reste
  à réparer pour retrouver un login rapide.
  L'UUID, le nom et les textures de Login Success proviennent seulement de la
  réponse Mojang. Les UUID annoncés par le client sont ignorés. Les jobs
  d'authentification sont bornés à 32. `USE_OPENSSL=1` est la compilation par
  défaut ; `USE_OPENSSL=0` conserve le serveur offline mais refuse le démarrage
  en online. `offline-skins` ne s'applique qu'au mode offline.
- Sur l'instance online, `MC_ADMIN_UUID` (32 caractères hexadécimaux) accorde
  les commandes administrateur uniquement à l'UUID vérifié par `hasJoined` ;
  `MC_ADMIN_USERNAME` ne confère des droits que sur une instance offline.
  Un pseudo offline peut être usurpé par tout client ayant accès au port.
- Le paquet `Player Info Update` inclut désormais l'entrée du joueur lui-même,
  avec ses propriétés de profil signées. Le skin d'Ofwood_ a été vérifié
  visuellement en vue F5 avec le client Java officiel sur 25588.
- Les signatures de texture sont exigées au décodage d'une propriété présente
  et relayées telles que reçues sur HTTPS depuis Mojang ; GoatCore ne vérifie
  pas lui-même la signature cryptographique du contenu de la texture. Le
  rendu final des skins, en particulier avec UUID offline, doit être testé
  visuellement avec deux clients Java officiels.
- Tests obtenus : RSA et hash signé Java (vecteurs positifs/négatifs),
  challenge erroné, profil simulé accepté, compte sans session refusé par
  `hasJoined` réel, trames chiffrées fragmentées, login offline, multi-epoll.
  Le client Java officiel Ofwood_ a rejoint le monde sur le port temporaire
  25588 et exécuté `/gamemode creative`. Ce test valide le chemin positif
  d'authentification et les permissions sur cette instance, pas encore
  l'ensemble du gameplay ni la parité Vanilla.

Mesure locale ponctuelle (`tests/stress/benchmark_network.py`, 100 bots,
connexion seule, 16 s, un worker réseau, mondes plats jetables) : avec
`offline-skins=false`, 100/100 connectés, 0 déconnexion inattendue, 19,99 TPS,
MSPT P95/P99 6,6/7,4 ms et réception initiale du chunk central moyenne
0,011 s. Avec `offline-skins=true`, 100/100 connectés, 0 déconnexion
inattendue, 19,99 TPS, MSPT P95/P99 7,1/8,1 ms et réception initiale
0,749 s. Le cache a mis 21 recherches en file, rejeté 79 demandes saturées
et réalisé 18 recherches pendant l'essai. L'attente cosmétique ralentit donc
les premiers logins sans améliorer le tick ; désactiver les skins publics
pour les benchmarks de bots reste recommandé. Cette mesure unique ne permet
pas d'attribuer un gain ou une régression stable.

## Restes de la phase 1

À valider/corriger par étapes : comparaison binaire avec serveur et client
Java 26.1.1 officiels,
déconnexions explicites pendant Login, validation complète
de Configuration, cycle de vie/reconnexion/sauvegarde du joueur, spawn,
mouvements, visibilité multijoueur, mort/respawn, propriétés du profil. Les
tests Python ne couvrent que l'API protocolaire partielle de GoatCore. Les mondes de test restent jetables.

Références externes primaires : [Minecraft Java 26.1.1](https://www.minecraft.net/ja-jp/article/minecraft-java-edition-26-1-1),
[Java `UUID.nameUUIDFromBytes`](https://docs.oracle.com/en/java/javase/26/docs/api/java.base/java/util/UUID.html).


## Phase 1C — finalisation du cycle de vie des joueurs

La comparaison des paquets a été faite contre le serveur officiel Java 26.1.1
(protocole 775), lancé dans un monde plat isolé sur le port temporaire 25590.
GoatCore relaie maintenant les animations de bras principal et secondaire
(identifiants 0 et 3) aux seuls joueurs qui voient déjà l'entité. Le paquet
`Player Input` pilote la pose accroupie et le drapeau de décalage ; les
commandes de sprint pilotent son drapeau. Pour ces transitions, les paquets
`Animate` et `Set Entity Data` observés ont le même contenu que ceux du
serveur officiel, hormis l'identifiant de l'entité. La synchronisation
existante de position, rotation tête/corps et équipement visible reste active.
La marche, la course et le saut sont reproduits par les déplacements et la
pose du client ; leur rendu avec deux clients Java officiels reste à vérifier.
Le minage et l'attaque émettent l'animation de bras ; la progression visuelle
des fissures et l'ensemble des effets de combat ne sont pas validés ici.

La sauvegarde NBT conserve maintenant `XpP`, `XpLevel` et `XpTotal`, puis envoie
`Set Experience` à la connexion. Sur l'instance de test 25589, un arrêt et un
redémarrage ont conservé l'UUID offline, la position, le lacet et le tangage,
le mode de jeu, les objets en inventaire, l'armure et la main secondaire, la
santé, la faim et les trois valeurs d'expérience. La dimension n'a pas été
validée : son changement n'est pas encore pris en charge par ce chantier.

Validation automatisée sur 10.0.4.66 : `make -B precommit-check` avec un
export de sources sans Git sur la machine distante ; comparaison des animations
sur le serveur officiel et GoatCore ; visibilité avec 50 joueurs (chacun voit
les 49 autres) ; 50 bots de déplacement pendant 15 s (50 connexions, aucune
déconnexion inattendue) ; déconnexions brutales pendant le déplacement et le
chargement des chunks, suppression des entités visibles et reconnexion, avec
1, 2 et 4 workers réseau ; persistance avant et après redémarrage ; ASan et
UBSan sans signalement dans ces scénarios. TSan a compilé, mais son runtime
s'arrête avant l'ouverture du port avec `unexpected memory mapping`, y compris
sur un test C minimal. Ce résultat ne permet pas de conclure sur l'absence de
courses entre threads.

La validation manuelle avec deux vrais clients Java 26.1.1 reste à faire sur
une instance temporaire de ce binaire : connecter deux comptes distincts au
même monde, vérifier mutuellement marche, course, accroupissement, saut,
rotation tête/corps, coups de bras, minage, attaque et équipement, puis fermer
brutalement un client et le reconnecter. Pour le mode online, utiliser 25588
avec des comptes Microsoft/Mojang réellement authentifiés et vérifier un login
positif avant tout redémarrage de 25565. Un rejet `hasJoined` ne prouve pas le
chemin positif. Le port 25587 reste réservé aux bots.

## Phase 1D — physique, besoins et mort (validation partielle)

Référence : serveur officiel Java 26.1.1, protocole 775, installé dans un
monde plat isolé sur 10.0.4.66:25590. Les règles numériques ci-dessous ont
été vérifiées dans les classes `ServerPlayer`, `FoodData`, `LivingEntity` et
`ServerGamePacketListenerImpl` du JAR officiel. La comparaison du code établit
la formule attendue ; elle ne remplace pas un test de comportement bout en bout.

| Scénario | Vanilla 26.1.1 | GoatCore après correction | Preuve et réserve |
|---|---|---|---|
| Marche ordinaire, sprint, saut | Marche au sol sans épuisement ; sprint 0,1/bloc ; saut 0,05 ou 0,2 en sprint | Même accumulation pour ces cas | Bytecode officiel et `test_pvp` ; eau/échelle non couverts |
| Nourriture et régénération | Épuisement consommé une fois par tick si >4 ; à nourriture 20 avec saturation, soin `min(saturation,6)/6` toutes les 10 ticks ; sinon nourriture ≥18, 1 point toutes les 80 ticks | Même seuil, cadence et formule ; épuisement consommé au tick suivant | Bytecode et `test_pvp` ; état visible avec client officiel non vérifié |
| Famine | Dégât toutes les 80 ticks ; santé minimale 10 en Easy, 1 en Normal, 0 en Hard | Seuils présents | Audit du code ; comparaison dynamique des trois difficultés à faire |
| Chute et vide | Chute : `floor(distance + 10⁻⁶ − 3)` ; vide sous `minY − 64` | Ces seuils appliqués | Bytecode et tests de frontière C ; détection de chute dépend du drapeau sol du client |
| Position reçue | Rejet des coordonnées NaN et angles non finis ; bornage X/Z à ±30 millions et Y à ±20 millions | Ces contrôles appliqués | Bytecode et `test_pvp` ; validation des collisions absente |
| Dégât répété | Pendant la fenêtre de protection, seul l'excédent d'un coup plus fort s'applique | Fenêtre temporelle et excédent pris en charge | Bytecode et `test_pvp` ; autres modificateurs de dégâts absents |
| Mort et réapparition | Événement de mort visible aux observateurs ; inventaire et XP conservés avec `keepInventory` | Événement 3 relayé ; sans `keepInventory`, inventaire lâché et XP remis à zéro ; respawn accepté | Tests distants `death`, `death-abort`, `death --expect-xp` sur 25589 ; orbes XP absents |

Le serveur relaie essentiellement les positions simulées par le client. Il
n'applique pas encore les volumes de collision Vanilla ni la vérification de
déplacement contre les formes des blocs. En particulier, mur, plafond, dalle,
escalier, passage accroupi, trou, bloc partiel et frontière de chunk ne sont
pas déclarés conformes. Le moteur doit d'abord disposer des formes de
collision de chaque block state, y compris les formes partielles, pour
valider la boîte du joueur et corriger les mouvements. Cette dépendance relève
de la phase 2 de la roadmap. Le vol Creative et la traversée Spectator sont
pilotés surtout par le client ; leur validation de sécurité côté serveur reste
à faire. La gravité, la nage, l'eau, les échelles et le knockback n'ont pas
encore de comparaison différentielle complète.

Les dégâts de suffocation, feu, lave et noyade ne disposent pas du traitement
Vanilla complet. La mortalité par chute et vide est partielle car la position
et `onGround` du client sont encore acceptés sans preuve de collision. Les
drops d'orbes XP, les règles exactes du point de réapparition (lit/ancre), les
effets de statut, les enchantements, les attributs et les détails des règles
de jeu après mort restent hors validation. Aucun résultat de bot ne doit être
interprété comme une preuve de conformité de ces mécanismes.

Sur l'instance temporaire offline 25589, les bots ont vérifié la notification
de mort d'un autre joueur, les drops d'objet, le retour à 20 points de vie,
la remise à zéro de l'XP sans `keepInventory`, la reconnexion après interruption
du respawn, la disparition/réapparition de l'entité et l'absence de duplication
constatée dans ce scénario. Les tests de visibilité à dix joueurs, de
déconnexion et d'interruption de chargement des chunks ont réussi. Le test de
visibilité filtre désormais explicitement le type `player` dans `Add Entity` :
un objet lâché à la mort n'est pas un joueur fantôme. Ces tests ne couvrent pas
la totalité des entrelacements de threads. Les deux vrais clients Java
nécessaires au contrôle visuel simultané de collision, dégâts et respawn
restent à tester.

Validation technique sur 10.0.4.66 : `make -B precommit-check` a réussi sur
l'export de sources final (tests C, démarrage de contrôle, login et réseau avec
1, 2 et 4 workers). Sur 25589, le scénario mort/respawn, l'interruption du
respawn, la visibilité à 50 joueurs (49 entités distinctes vues par chacun),
les déconnexions concurrentes, les interruptions pendant l'envoi initial des
chunks et les animations ont réussi. Mort/reconnexion et visibilité à dix
joueurs ont aussi réussi avec 2 puis 4 workers réseau. Le binaire compilé avec
ASan et UBSan a passé mort/respawn, interruption de respawn et visibilité à dix
joueurs sans diagnostic dans le journal de l'instance. TSan n'a pas pu être
utilisé : son runtime échoue sur un programme C minimal avant tout test avec
`FATAL: ThreadSanitizer: unexpected memory mapping` (code 66). Sur le serveur
Vanilla 25590, le test des paquets de mouvement de bras, d'accroupissement et
de sprint a réussi ; il sert de référence à ce sous-ensemble de la phase 1C,
pas de validation des autres scénarios physiques 1D.

Le journal de l'instance inscrit maintenant le pseudo, la commande reçue,
l'autorisation et tout changement effectif de gamemode (ancien et nouveau).
Cette trace permet de constater côté serveur l'exécution des commandes de
validation, sans déduire de la seule connexion que les permissions marchent.
