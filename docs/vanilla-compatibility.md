# GoatCore 26.1.1 — compatibilité Vanilla (phase 2A et diagnostic des performances)

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

## Présence des joueurs dans la liste multijoueur et le chat

Le ping Status annonce désormais le nombre de connexions entrées en Play et
la capacité configurée, au lieu de toujours annoncer `0/100`. Une connexion
encore en Login ou Configuration n'est pas comptée, et une connexion fermée
disparaît immédiatement du compteur.

Lorsqu'un joueur devient prêt, les autres joueurs prêts reçoivent un paquet
System Chat avec le composant traduit `multiplayer.player.joined`. Lorsqu'il
quitte, ils reçoivent `multiplayer.player.left` une seule fois, après le retrait
de son entité et de son profil. Le joueur concerné ne reçoit pas son propre
message. Les composants contiennent le pseudo comme argument et la couleur
jaune ; les métadonnées interactives de profil (clic et survol) présentes dans
le message Vanilla restent à reproduire.

Comparaison sur 10.0.4.66 : serveur officiel 26.1.1 sur 25590 et GoatCore
isolé sur 25589. Le même scénario à deux bots a observé sur les deux serveurs
les compteurs `0 → 1 → 2 → 1 → 0`, un seul message traduit d'arrivée et un seul
de départ chez le premier joueur, aucun message d'arrivée propre au second.
Sur GoatCore, la visibilité à 50 joueurs, les déconnexions brutales et les
interruptions de chargement de chunks ont réussi ; le scénario de présence et
les déconnexions ont aussi réussi avec 2 et 4 workers réseau. `make -B
precommit-check` a réussi sur l'export distant. ASan/UBSan n'a signalé aucune
erreur pendant présence et visibilité à dix joueurs. Le paquet Player Info
Update et la liste Tab existaient déjà ; l'utilisateur a confirmé que Tab
fonctionne. Le rendu des nouveaux messages par deux clients Java officiels
simultanés reste à confirmer visuellement.

## Phase 1D.2 — audit des dégâts et recul des joueurs

Référence directe : JAR serveur officiel Java 26.1.1 installé sur
10.0.4.66. L'audit a porté sur `LivingEntity.hurtServer`,
`LivingEntity.knockback`, `Player.attack`, `CombatRules`,
`Entity.baseTick`, `LivingEntity.baseTick` et les tags de types de dégâts
embarqués dans ce JAR. Les nombres ci-dessous sont issus de ce code ; la
correspondance dynamique des paquets est vérifiée séparément.

| Source | Règle Vanilla constatée | État GoatCore 1D.2 |
|---|---|---|
| Coup à main nue | Attribut initial d'attaque à 1 point ; l'armure utilise `max(armure × 0,2, armure − dégâts/(2 + ténacité/4))`, borné à 20 puis divisé par 25 | Dégât de base et formule armure/ténacité corrigés. Temps de recharge de l'attaque, coups critiques, enchantements et effets encore absents. |
| Coup reçu | La fenêtre `invulnerableTime` est mise à 20 ticks ; au-dessus de 10 ticks restants, un coup plus fort n'applique que l'excédent et ne réémet ni événement de blessure ni recul | Excédent déjà traité ; événement et recul désormais supprimés pour ce coup partiel. La fenêtre GoatCore reste exprimée en 500 ms, pas en nombre de ticks effectivement écoulés. |
| Recul PvP | Impulsion de 0,4 après résistance ; direction opposée à l'attaquant ; vitesse horizontale précédente divisée par deux ; au sol, vitesse Y `min(0,4, ancienne/2 + force)` ; en l'air, Y inchangé | Formule de base et paquet `Set Entity Motion` 101, encodé en `LpVec3` propre au protocole 775, vers la cible et les observateurs visibles. Le même codec corrige la vitesse des objets lâchés. Vitesse précédente estimée depuis les mouvements du client. Résistance, recul supplémentaire de sprint/enchantement et collisions serveur restent à prendre en charge. Aucune téléportation artificielle n'est utilisée. |
| Vide | Sous `minY − 64`, 4 points par activation ; `out_of_world` traverse l'invulnérabilité Creative et l'armure | Creative reçoit maintenant les dégâts du vide ; Spectator est exclu. La position du client reste insuffisamment contrôlée par collision serveur. |
| Feu / lave | `on_fire` inflige 1 point toutes les 20 ticks ; `lavaHurt` soumet 4 points lors du contact ; `on_fire` traverse l'armure, `lava` non ; la résistance au feu annule ces sources | Non conforme : contact exact avec le feu et la hauteur des fluides, état d'embrasement, résistance, durabilité et effets manquent. |
| Noyade | Air initial 300 ; perte de 1 par tick avec les yeux immergés ; à −20, retour à 0 et soumission de 2 points ; récupération de 4 par tick hors de l'eau | Non implémentée : détection des yeux dans le fluide, niveau de fluide et paquets d'air manquent. L'armure ordinaire ne réduit pas la noyade. |
| Suffocation / blocs dangereux | `in_wall` soumet 1 point lors de `baseTick` et traverse l'armure ; feu, cactus, buisson, campfire et magma ont des collisions et des types de dégâts distincts | Non implémentés. Les volumes de collision des block states, notamment partiels, restent une dépendance de la phase 2. |
| Gel | Type `freeze` indépendant, traversant l'armure ; température et durée de gel dépendent du contact avec la poudreuse et de l'équipement | Non implémenté : état de gel, collision poudreuse et immunités d'équipement absents. |

Le calcul de recul côté serveur reste partiel tant que GoatCore accepte la
position et le drapeau `onGround` du client sans simulation de collisions.
Le vrai paquet d'attaque 26.1.1 est `ServerboundAttack` (identifiant 1,
VarInt d'identifiant d'entité uniquement). Le chemin PvP écoutait auparavant
`ServerboundInteract` (26), qui sert désormais à interagir et n'envoie pas
les attaques du client officiel. La correction sépare ces deux actions.
Le tableau distingue volontairement les règles lues dans le JAR des scénarios
effectivement observés sur les serveurs. Ni les effets de statut ni les
enchantements ne sont annoncés conformes. Les interactions de blocs et de
fluides seront raccordées aux formes de collision en phase 2 ; l'architecture
d'entités servira ensuite aux résistances et sources de dégâts communes.

Scénarios numériques exécutés sur 10.0.4.66 : le script
`tests/stress/test_player_combat.py` a observé sur Vanilla 25590 un coup à
main nue de 1 point, un recul horizontal de +0,400 à 2 blocs puis de −0,400
à 2,5 blocs, et aucun second dégât immédiat. Sur GoatCore isolé 25589,
les deux mêmes directions et distances ont produit 1 point et ±0,400. Le
paquet de vitesse a été reçu par la cible et par l'observateur. Après le
combat, la déconnexion a retiré l'entité visible et la reconnexion a restauré
la santé sans doublon observé. Le test du vide en Creative a observé 20 → 16
points. Ces scénarios ne couvrent pas les attaques chargées, les armes, les
résistances, ni les collisions après le recul. Sur la référence Vanilla, des
essais supplémentaires dans le monde réutilisé ont subi d'autres dégâts
pendant la préparation des bots ; ils ne sont pas interprétés comme des
mesures de PvP.

Sur le même export de sources, `make -B precommit-check` a réussi à distance
avec son démarrage de contrôle sur 25586. Les scénarios de mort/respawn,
d'interruption du respawn, de dix déconnexions brutales et de visibilité à
50 joueurs sont passés sur 25589. Un test de 50 bots en mouvement pendant
16,19 s a compté 50 connexions, 6 348 mouvements envoyés, zéro échec de
connexion et zéro déconnexion inattendue. Les nombres de mouvements envoyés
ne prouvent pas tous une position acceptée. Le runtime TSan de cet hôte échoue
sur un programme C minimal avec `unexpected memory mapping` (code 66), avant
toute validation de GoatCore.

Les tests de combat, déconnexion et reconnexion ont également réussi avec
2 puis 4 workers réseau sur 25589, en conservant l'affinité CPU du serveur
de test. La variante ASan/UBSan a passé `test_pvp`, le combat à deux joueurs,
le vide Creative, mort/respawn, neuf départs brutaux concurrents et la
visibilité à dix joueurs sur 25586, sans diagnostic dans son journal.
L'instance temporaire de combat autorisait les commandes d'administration
pour tous les bots afin de les placer en Survival ; cette option ne doit pas
être transposée aux instances 25565/25587.

## Phase 1E — persistance avancée des joueurs

Référence : serveur officiel Java 26.1.1 sur le port temporaire 25590,
avec un joueur offline de test portant une pile de 64 pierres, un casque
et une épée en main secondaire. Après déconnexion et redémarrage officiel,
le client de test a retrouvé ces trois piles. Le fichier réel de référence
est conservé dans `tests/fixtures/vanilla-26.1.1-player.dat` ; son
`DataVersion` est 4788. GoatCore charge ce fichier, réécrit les champs qu'il
gère, puis retrouve les trois piles et les autres champs NBT non interprétés.
Cette preuve couvre ces objets simples et ce scénario, pas tous les items
Vanilla.

| Donnée | Sauvegarde et restauration GoatCore 1E | Comparaison / limite |
| --- | --- | --- |
| Identité | Fichier nommé par UUID authentifié online ou UUID offline déterministe, tag `UUID` IntArray contrôlé s'il existe ; même pseudo et UUID différents donnent deux fichiers. L'ancien `playerdata/<uuid>.dat` est lu en repli, conservé, puis migré à la prochaine sauvegarde. | Les nouveaux fichiers vont dans `players/data/<uuid>.dat` comme 26.1.1. Le contrôle des doublons couvre les connexions actives et les départs encore en sauvegarde. Une connexion client officielle reste nécessaire pour attester le chemin online après chaque binaire. |
| Position et rotation | `Pos` (trois doubles), `Rotation` (deux floats) ; coordonnées négatives restaurées après redémarrage. | Le déplacement n'est durable qu'après une sauvegarde déclenchée ou une déconnexion/arrêt propre. Les positions invalides sont refusées ou ramenées au spawn selon le contrôle de chargement existant. |
| Gamemode et vol | `playerGameType` et `abilities.flying` ; le paquet d'aptitudes client met à jour l'état de vol autorisé. | Les vitesses d'aptitudes personnalisées peuvent rester en NBT mais le paquet envoyé utilise encore les valeurs fixes du serveur. |
| Inventaire et équipement | Hotbar 0–8, inventaire 9–35 dans `Inventory` ; casque/torse/jambes/bottes/main secondaire dans `equipment` ; `SelectedItemSlot`, piles et coffre de l'End. La grille de craft est restituée à l'inventaire à la fermeture normale. | Conformes au schéma NBT observé pour les piles simples : `Slot` Byte et `count` Int. L'ancien indexage réseau GoatCore est lu puis migré. Les composants, durabilité, noms et enchantements d'items ne sont pas encore représentés de bout en bout : leur présence provoque un refus de chargement ou de sauvegarde, laissant le fichier existant intact. |
| Curseur, craft et révisions | Curseur, quatre ingrédients de craft 2×2, révision d'inventaire et du coffre de l'End dans les tags privés `mcCursorItem`, `mcCraftingItems`, `mcInventoryStateId`, `mcEnderStateId`, `mcInventoryFormat`. Le résultat de craft est recalculé et n'est jamais sauvegardé comme une seconde pile possédée. | Ces tags ne font pas partie du schéma Vanilla. Les fichiers GoatCore ne sont pas déclarés parfaitement interchangeables avec Vanilla. |
| Santé, faim et XP | `Health`, `foodLevel`, saturation, épuisement, `XpP`, `XpLevel`, `XpTotal` chargés et sauvés ; valeurs vérifiées après déconnexion et redémarrage. | Santé maximale variable, effets actifs et durées ne sont pas appliqués par le gameplay. Les tags non interprétés sont conservés dans le fichier, ce qui ne constitue pas une restauration fonctionnelle de ces effets. |
| Dimension et réapparition | `Dimension=minecraft:overworld` ; les autres dimensions sont refusées pour éviter de charger leur position dans l'Overworld. Mort et respawn, avec ou sans `keep_inventory`, ont été testés après redémarrage. | Nether/End et point de réapparition personnel non implémentés. Les tags inconnus de réapparition sont conservés mais la logique utilise encore le spawn du monde. |

Les écritures joueur sont synchrones sur le tick autoritaire et protégées
par un mutex au niveau du magasin. Un fichier gzip est préparé dans le même
répertoire, fermé et `fsync` avant `rename` atomique ; le répertoire est
ensuite synchronisé. Un échec avant `rename` laisse l'ancien fichier valide
et nettoie le temporaire. Les fichiers NBT tronqués, les données d'item
inconnues et les champs essentiels mal formés sont refusés ; ils ne sont pas
remplacés par un état par défaut. Les NBT décompressés sont plafonnés à 8 MiB.
La sauvegarde est déclenchée par les changements d'inventaire, d'état de jeu,
de faim/santé, de slot ou de vol, ainsi que par la déconnexion et l'arrêt
propre. Il n'y a pas encore d'autosave périodique de la position.

Sur un monde temporaire à 25589, un SIGKILL après sauvegarde d'un objet et
après un déplacement accepté a conservé l'objet, mais restauré la dernière
position durable. Un SIGTERM avec le joueur connecté a conservé sa dernière
position et son objet. La déconnexion brutale du client a été testée
séparément : elle suit le nettoyage normal du serveur et sauvegarde l'état.
Ces résultats ne garantissent aucune action non encore durable au moment
d'un crash, ni la cohérence avec des sauvegardes de chunks ou de conteneurs
effectuées dans une autre transaction.

La suite `tests/stress/test_player_persistence.py` sépare préparation et
vérification par un vrai redémarrage, compare les 46 slots réseau et vérifie
identité, position négative, gamemode, vol, slot sélectionné, santé, faim et
XP. Un, deux puis dix joueurs ont été vérifiés sur un worker réseau ; dix
joueurs supplémentaires ont été vérifiés sur deux et quatre workers. Les
tests C `test_player_nbt` vérifient le fichier officiel, la migration des
anciens fichiers, deux UUID portant le même nom, l'échec forcé de `rename`
et le rejet d'un gzip tronqué. Les essais à 25 joueurs et les transitions
vers d'autres dimensions restent non exécutés.

`make -B precommit-check` a réussi sur l'export distant avec
`GOATCORE_SOURCE_EXPORT=1` (hygiène Git contrôlée localement). ASan/UBSan a
passé les tests C de persistance et un cycle à deux joueurs avec redémarrage,
inventaires complets et vol Creative. Le binaire instrumenté PIE a échoué
avant l'ouverture du port avec une répétition de `DEADLYSIGNAL` ; la variante
instrumentée sans PIE a terminé les scénarios et les arrêts sans diagnostic.
Le programme minimal TSan a échoué avant toute validation, code 66,
`unexpected memory mapping`. Aucun résultat TSan positif n'est annoncé.

## Phase 2A — premier lot : registres, bûches et dalles (2026-10-27)

Ce lot établit des primitives et valide des scénarios précis. Il ne termine
ni la phase 2A entière, ni les phases 2B/2C, ni la phase 2 de la roadmap.
Le modèle de concurrence est conservé : décodage/validation ordonnés sur le
tick, mutations du monde, inventaire et notifications autoritaires sur ce tick.

### État initial et provenance

HEAD initial : `86d615c86d80e8e7441fa9e54e6e582f91e8c11c`.
`git status` ne montrait aucune modification suivie ; `.vscode/`,
`src/main.py` et `start-aider.sh` étaient non suivis et sont préservés.
`git ls-remote origin refs/heads/main refs/heads/codex/network-epoll-phase2`
confirmait ce même commit sur les deux branches distantes. Aucun push,
merge, rebase ni commande Git sur le serveur distant.

Référence officielle installée sur `10.0.4.66` :
`/opt/goatcore/vanilla-2611/versions/26.1.1/server-26.1.1.jar`.
Son `version.json` contient `26.1.1`, protocole `775`, DataVersion `4788`.
SHA-256 du JAR interne :
`3647564632a2f589be2a87bf544f628e5f67cb3780c86f7fa972ec36892aa081`.

Rapports régénérés sur l'hôte distant avec le JDK 25 livré dans cette
installation :

```sh
cd /opt/goatcore/vanilla-2611
./jdk-25.0.5+8/bin/java -DbundlerMainClass=net.minecraft.data.Main \
  -jar server.jar --reports --output /opt/goatcore/vanilla-2611/phase2a-generated
```

Le `reports/registries.json` produit est aussi identique au rapport local :
SHA-256 `2b7b9a3a5738437c9140734e2f9eebfa7c2cfea9b685efed1f3802bdbcfccf9f`.
La nouvelle table `generated_block_ids` conserve les 1 168 vrais IDs protocolaires
de blocs séparément de leurs index internes. Par exemple, chest a l'ID 201,
shulker_box 677. Les événements d'ouverture/fermeture shulker utilisent cette
conversion ; le comportement complet des block entities reste hors validation.

Le `reports/blocks.json` produit est identique octet pour octet au fichier
local `data/26.1.1/reports/blocks.json` : SHA-256
`c4f6aafb57a085df217fcb93533de7c8baa5711ff6eecad07563b28813713cc0`.
Le registre C et son en-tête se régénèrent à l'identique : **1 168 blocs,
29 873 états**. Les fichiers d'identifiants items/paquets se régénèrent aussi
à l'identique, à la ligne d'include près lorsque le fichier temporaire porte
un autre nom ; JSON et en-tête sont identiques. La table item→état par défaut
est identique : 1 014 entrées mappées, 1 506 items au total.
Cette table par égalité de nom n'est pas une preuve du comportement de tous
les BlockItem (noms spéciaux, variantes murales et objets multiparties).

Les anciennes ressources `assets/version.json` (1.21.1) et
`mc_vania_asset/server/version.json` (26.1, DataVersion 4786) ne servent pas
à cette validation. Les données complètes brutes restent ignorées par Git.

### Matrice d'audit

| Fonctionnalité existante | Conformité connue / tests | Bug ou limite constaté | Dépendance / priorité |
|---|---|---|---|
| Registre généré, clés canoniques, états par défaut | Rapport officiel identique ; parcours des 29 873 états | Domaines et produit des propriétés pas contrôlés à la génération | Corrigé dans ce lot ; aucun ID deviné |
| `block_index`, `mc_block_id_t` | Index interne trié, pas un identifiant réseau Vanilla de bloc | Un BlockEvent shulker envoyait cet index comme ID réseau de bloc | Corrigé par une table générée de `registries.json`, sans changer les index gameplay |
| Flags du registre | Validité, air et défaut renseignés | Solidité, opacité, lumière et block entity restent des placeholders | Extraire les propriétés/formes officielles avant généralisation |
| Palette réseau et Anvil | Tests palette, codec, roundtrip et invalidation de cache déjà présents | Les états inconnus lus dans certaines palettes retombent sur air | Durcissement/imports complets ultérieurs |
| Version NBT des chunks | Écriture et validateur historique à 4785 | Le serveur cible 4788 | Tag corrigé ; test du NBT réellement encodé et validation d'arène mis à jour ; fixtures d'import historiques conservées |
| World/chunk | Dirty, revision, cache invalidé, file d'updates et sauvegarde présents | `get_block` assimile un chunk indisponible à air ; `set_block` accepte des écritures différées | Placement utilise désormais `get_block_ready` avant toute écriture |
| `Use Item On` 0x42 | Bytecode officiel `FriendlyByteBuf` et `ServerboundUseItemOnPacket` | Deuxième booléen absent, séquence optionnelle, fin non contrôlée | Corrigé ; bots corrigés simultanément |
| Pierre, terre, planches | Déjà mappés et consommés côté serveur | Placement dans le voisin même si le bloc cliqué est remplaçable | Corrigé ; short grass vérifiée dynamiquement |
| Bûches orientées | Axe selon la face déjà présent | Logique enfouie dans `play.c` | Extraite dans `placement.c` ; axes X/Y/Z comparés |
| Dalles | Moitié selon face/point déjà présente | Fusion inconditionnelle sur la dalle cliquée ; pas de fusion sur cible adjacente ; collision cubique | Corrigé et comparé ; types bottom/top/double |
| Escaliers | Orientation et moitié présentes, forme straight imposée | Orientation opposée au joueur utilisée ; angles et voisinage absents ; collision cubique | État au lot 1 ; corrections et validation du lot 2 ci-dessous |
| Portes/trappes | États et deux moitiés de porte partiellement gérés | Facing, charnière, alimentation, support, formes et transaction multipartie incomplets | Non validées ; lot ultérieur |
| Torches et blocs sur support | Item→état par défaut présent | Variantes murales/support/survie des blocs non résolus | Non validés ; prochaine famille après formes/support |
| Destruction et drops | Mining/block_drops + tests C existants | Minage avancé, destruction des supports et voisinage non exhaustifs | Préservés ; pas de refonte de ces modules |
| Synchronisation A→B | File d'updates globale et envoi direct existaient | Un ACK ne prouve pas une mutation | Deux bots comparent l'état et l'inventaire ; redémarrage vérifié |

### Corrections et architecture

`mc_block_state` expose la recherche du bloc par nom, la lecture exacte d'une
propriété et son remplacement validé contre le registre ; les autres propriétés
sont conservées. Il n'accepte ni valeur inconnue, ni propriété partiellement
correspondante. Aucun tableau manuel d'identifiants supplémentaires.

`mc_placement` résout les axes et moitiés de dalles, les fusions cliquées ou
adjacentes, les bornes Y d'une dalle et la portée vers l'AABB ciblée.
La fusion supprime waterlogged comme Vanilla ; le placement dans de l'eau
initialise cette propriété lorsqu'elle existe. Les tests C couvrent ce changement
de propriété ; les fluides dynamiques ne sont pas déclarés validés.
La logique dupliquée des dalles/axes a été retirée de `play.c`.

Le handler lit maintenant les deux booléens et la séquence obligatoire, contrôle
la fin du paquet, main/face/séquence, les valeurs finies et la tolérance du point
par rapport au centre (`<1.0000001`, comme le code officiel). Il rejette une
position hors hauteur, hors limites fixes, hors portée, un joueur mort ou en
Spectator/Adventure. La portée utilise les valeurs de base 4,5/5 blocs + la
tolérance serveur de 1 bloc, et l'œil debout/accroupi. Les attributs personnalisés,
la nage et les autres poses ne sont pas pris en charge par ce contrôle.
Le flag `worldBorderHit` est décodé mais la bordure dynamique est absente.
La normalisation du yaw avant conversion entière évite aussi une conversion
hors plage avec un angle fini extrême.

Les nouveaux chunks sont écrits avec `DataVersion=4788`, contrôlé sur le NBT
réellement produit. La lecture des anciens chunks GoatCore marqués 4785 reste acceptée, et les
fixtures d'import historiques restent dans les tests ; cette correction du tag ne constitue pas une validation de tous les
champs NBT ou de la migration Vanilla/DataFixer.

Le placement de dalle passe par le même chemin de mutation/inventaire/réseau
que le placement ordinaire. Les chunks indisponibles ne reçoivent plus une
mutation différée issue d'un placement client. `/setblock`, derrière les contrôles
administrateur existants, utilise le registre pour préparer les mêmes fixtures
sur les deux serveurs ; les clés complètes doivent être canoniques.

### Validation fonctionnelle et limites

Scénario réutilisable : `tests/stress/test_block_placement.py`, exécuté sur des
mondes temporaires distincts, GoatCore 25589 et Vanilla 25590, jamais 25565.
L'opérateur offline synthétique `Ofwood_` prépare les fixtures ; les commandes
réellement exécutées vérifient ses privilèges sur les instances temporaires.
Deux bots observent chaque placement réussi ; les IDs attendus viennent du
rapport officiel, et les quantités Survival passent de 4 à 3. Les refus
préservent les blocs et n'émettent pas de consommation. Les séquences traversent
la frontière VarInt 127/128. Le lecteur comprend aussi les updates de section
Vanilla, sans imposer une égalité du délai ou du nombre de paquets.

Scénarios : pierre, terre, planches de chêne ; bûche de chêne X/Y/Z ; dalle de
chêne basse/haute ; fusion dessus/dessous/face latérale ; face latérale ne
fusionnant pas et pose adjacente ; remplacement d'herbe courte ; refus d'une
cible occupée, d'une collision avec le joueur, d'un point invalide et d'une cible
hors portée ; dalle basse sous les pieds sans collision dans la moitié haute.
La cible hors portée reste dans un chunk chargé : une première vérification
Vanilla à `(80,64,80)` était inexploitable car ce chunk n'était pas chargé ;
le fixture corrigé utilise `(-8,64,16)`.

Après arrêt propre et redémarrage, quatre poses réellement effectuées
(pierre, bûche Y, dalle basse, dalle haute), réparties autour de coordonnées
négatives et frontières de chunks, sont conservées avec le slot 36 (4 dalles).
GoatCore est interrogé via sa lecture QA autoritaire ; Vanilla via
`execute if block ... run tellraw`, et l'inventaire reçu à la reconnexion est
vérifié. Le test existant `test_block_persistence.py` vérifie aussi trois bots,
trois démarrages, les états `[1,0,1]`, puis `[0,0,1]` après destruction.
L'identifiant du paquet BlockEvent d'ouverture d'une shulker box a également
été contrôlé contre Vanilla : action 1, paramètre 1, ID de bloc 677 ; ce test
n'affirme pas la conformité de l'inventaire ou des formes de cette famille.
La comparaison est une preuve pour ces scénarios et le chêne testé, pas pour
toutes les essences, tous les items mappés ou tous les cas de voisinage.

Tests C ajoutés/complétés : parcours exhaustif des états/propriétés/défauts,
permutation complète des IDs protocolaires de blocs, valeurs invalides, domaines de génération incomplets/corrompus, faces et seuil
0,5 de fusion, effacement waterlogged, axes, bornes des dalles, portée et nombres
non finis ; codec avec deux booléens, VarInt 128, troncatures et octet final ;
hauteur, lecture de chunk indisponible, coordonnées négatives et sauvegarde de
variantes de states. Les suites palette/Anvil/cache existantes sont conservées.

Restent notamment : collision avec les autres joueurs/entités (le test du volume
ne porte encore que sur le poseur), formes hors dalles, support et voisinage,
états de redstone/fluides évolutifs, block entities, offhand, Adventure avec
composants can_place_on, resynchronisation complète de toutes les prédictions
refusées, bordure dynamique, attributs de portée, crash-consistance des chunks.
Aucun test manuel avec deux clients officiels ni nouveau login Mojang positif
sur 25588 n'a été exécuté pour ce binaire. Ces absences empêchent de promouvoir
le binaire vers 25565 ; les services principaux restent préservés.

### Compilation, sanitizers et infrastructure

Copie distante isolée : `/opt/goatcore/phase2a-stage.9yuVhu`, sans Git.
Export via `scripts/remote-build-stage.sh`, qui reprend le transfert tar/SSH
existant en excluant également node_modules et tous les artefacts personnels.
`GOATCORE_SOURCE_EXPORT=1 make -B -j4 precommit-check` a réussi après correction
du fixture du nouveau test de codec : l'ACK existant est immédiat, le test
initial supposait à tort qu'il serait différé. Les tests C, smoke, login,
arrêt et multi-epoll à 1/2/4 workers, les tests de générateurs et ceux du client
Python sont passés. Les sources de blocs/IDs sont régénérées et comparées
indépendamment ; recettes/loot n'ont pas été régénérés sur cette copie faute de
sources brutes complètes. Les étapes intermédiaires DataVersion ont fait
échouer les tests d'arène puis d'import Anvil ; la compatibilité de lecture
4785 a été conservée. Un ajout de test placé dans la mauvaise fonction a aussi
produit une erreur de compilation, corrigée avant le passage final.
La compilation locale standard a échoué faute de
`libcurl` de développement ; le test C autonome local a réussi. Toutes les
compilations complètes et exécutions de serveurs ont eu lieu sur 10.0.4.66.

ASan+UBSan : les trois tests C registre/codec/monde, les 18 scénarios à deux bots,
le BlockEvent shulker, le redémarrage et une charge de 10 bots avec 2 workers
réseau (200 poses + 200 casses, zéro cycle manquant, zéro erreur et zéro
déconnexion inattendue) ont réussi sur le binaire instrumenté du lot de placement.
Après l'alignement DataVersion, les tests NBT/arène et Anvil instrumentés
couvrent aussi l'écriture 4788 et la conservation des imports 4785. ASan a
trouvé un use-after-free dans l'ancien `test_container_state_truth` : le pointeur
`ids` appartenait au monde détruit. Le test reprend désormais celui du nouveau
monde ; le passage Anvil instrumenté complet réussit après cette correction.
Le diagnostic initial est conservé dans `asan-dataversion-failure.log`.
Options : `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie`,
linker `-fsanitize=address,undefined -no-pie`, `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1`. Aucun diagnostic sur ces passages.

Les premiers binaires ASan PIE étaient instables avant l'initialisation :
`DEADLYSIGNAL` répété sans pile, ou SIGSEGV avant ouverture du port. Un processus
a dû être interrompu ; son journal répétitif est résumé dans
`asan-startup-failure.log`. Une première reprise avec `handle_segv=0` a passé,
une autre est sortie -11. Un programme minimal C ne faisant qu'un `puts` a
échoué **avant main 6 fois sur 20** sous ASan+UBSan PIE, contre **20 réussites
sur 20** compilé sans PIE. Le passage final utilise donc sans PIE, avec le
handler ASan normal actif. L'origine exacte de l'instabilité PIE n'est pas
établie ; elle ne doit être ni cachée, ni présentée comme un défaut de placement
diagnostiqué, ni comme une preuve d'absence générale d'erreurs mémoire.

TSan : le probe minimal sort 66 avec `FATAL: ThreadSanitizer: unexpected memory
mapping`. Trois tentatives supplémentaires sans PIE échouent aussi de cette
manière. Aucun scénario GoatCore n'a donc été validé sous TSan dans cette session.

Les services principaux n'ont été ni remplacés ni redémarrés. Contrôle final
via systemd, configurations et requêtes Status/Ping du protocole :
`goatcore.service` actif, PID 10634, 25565, `online-mode=true` ;
`goatcore-test.service` actif, PID 10621, 25587, `online-mode=false`.
Les PID sont ceux de l'audit initial. Les deux ports répondent en 26.1.1/775.
Aucun monde principal, inventaire ni permission principale n'a été réinitialisé.
Les privilèges d'Ofwood_ ont été testés réellement sur les instances temporaires
par les commandes de fixture ; aucune nouvelle vérification en jeu sur les
services principaux n'est revendiquée. Aucun login officiel sur le nouveau
binaire n'a été effectué et aucune mise à jour des services principaux n'est
promue. Les instances temporaires de test sont arrêtées entre les passages.

Binaire normal testé/stagé : `mc_server.normal` dans ce répertoire, SHA-256
`e721ea5d07e37a3417386aedc4ff943ec77868732506f9c841c819b74e9644c4`.
Il est aussi restauré sous `mc_server` après les tests instrumentés.
Le binaire des services principaux est conservé.

### Mesures de charge et réserve sur les performances

Commandes distantes : `python3 tests/stress/benchmark_network.py --server-binary
./mc_server.normal --workers 1 --mode blocks --bots N --cycles-per-bot 100
--duration 180 --block-cooldown 0.01`, pour N=1,10,25,50, séquentiellement.
Mondes plats frais, 1 worker réseau, 2 workers chunks, protocole 775.
Le client attend désormais la réception du chunk de sa cible avant la première
action : le chunk central pouvait arriver avant la cible dans le chunk voisin.
La quantité de travail reste exactement 100 cycles pose+casse par bot.

| Bots | Poses/casses confirmées | Cycles manquants | Erreurs d'action | Déconnexions inattendues | Durée (s) | TPS ; P95/P99 MSPT (ms) |
|---:|---:|---:|---:|---:|---:|---|
| 1 | 100/100 | 0 | 0 | 0 | 6.617 | 19.33 ; 47.1/51.9 |
| 10 | 1000/1000 | 0 | 0 | 0 | 19.164 | 6.89 ; 161.9/231.4 |
| 25 | 2500/2500 | 0 | 0 | 0 | 48.606 | 6.20 ; 271.5/353.4 |
| 50 | 5000/5000 | 0 | 0 | 0 | 98.913 | 4.41 ; 662.5/1000.0 |

TPS/MSPT : pire fenêtre active complète de 100 ticks sélectionnée par le script,
pas un histogramme de l'ensemble du test. Le dernier bucket des percentiles est
plafonné à 1 000 ms ; les maxima exacts restent dans les logs. Les observations
diffèrent entre charges : elles ne constituent pas une courbe de capacité.

| Bots | Latence update moyenne/max (ms) | CPU serveur (%) | Sauvegarde périodique moyenne maximale par fenêtre (ms/tick) | Max sauvegarde chunk / flush explicite (ms) |
|---:|---:|---:|---:|---:|
| 1 | 20.7/123.4 | 80.0 | 2.003 | 3.731/7.251 |
| 10 | 76.2/646.1 | 56.6 | 2.253 | 3.620/74.881 |
| 25 | 167.1/6189.0 | 44.7 | 2.424 | 3.910/350.198 |
| 50 | 409.6/3532.4 | 55.8 | 4.590 | 315.198/1075.205 |

Le CPU est la somme des threads du processus avant shutdown, mesurée via
`/proc/PID/stat` (100 % = un cœur) et excluant le bot Python. Les moyennes de
sauvegarde sont des phases MC_PERF, pas le temps total d'un arrêt. Les longues
latences incluent réseau, tick et traitement client ; aucun gain n'est annoncé.

Essais initiaux conservés, avant l'attente explicite du chunk : 10 bots ont
terminé 1 000+1 000 opérations avec 1 erreur et 4,52 TPS ; 25 bots ont terminé
2 500+2 500 sans erreur à 2,63 TPS ; 50 bots, délai 120 s, n'ont terminé que
4 707 cycles (4 712 poses, 4 707 casses), avec 293 cycles manquants et 48 erreurs,
à 3,28 TPS. Le rapport final ne remplace pas ces échecs par un résultat fictif.

Comparaison supplémentaire : compilation distante des sources exactes du HEAD
initial `86d615c8`, exportées avec `git archive` sur le PC, **sans Git distant**.
Chaque version reçoit une copie du même monde, le même client attendant son
chunk, 10 bots et 100 cycles/bot :

| Version | Poses/casses | Cycles manquants / erreurs / déconnexions | Durée (s) | TPS ; P95/P99 MSPT (ms) |
|---|---:|---:|---:|---|
| 86d615c8 | 1000/1000 | 0/0/0 | 18.584 | 15.97 ; 89.7/114.1 |
| Phase 2A | 1000/1000 | 0/0/0 | 18.604 | 7.79 ; 149.1/220.5 |

Le temps global est presque identique mais la fenêtre de tick du candidat est
plus mauvaise. Une seule paire et des conditions hôte/IO non contrôlées ne
permettent pas d'attribuer précisément cet écart. L'ancien codec produit un
ACK incorrect ; ce benchmark compare les modifications réellement confirmées
par BlockUpdate, pas la conformité des ACK de la référence.
Le profilage retrouve du temps dans le ramassage d'objets et les flushs
synchrones, déjà présents dans la référence. **La tenue de 20 TPS et la
non-régression des queues de latence ne sont pas validées.** Aucun moteur de
simulation parallèle, ordonnanceur ou sauvegarde asynchrone n'est ajouté pour
masquer cette limite dans ce lot.

Les mesures ci-dessus portent sur le binaire
`a0d97b09a92cfe18a2862d081d6d130b3c7bf529b727052d3ca416499d2f463b`,
avant la dernière correction du tag DataVersion des chunks et le maintien
explicite de la lecture de l'ancien tag 4785. Les quatre charges n'ont pas été
répétées après cette correction de version ; aucune
validation des performances du binaire final à 20 TPS n'est revendiquée.
Un contrôle ciblé sur le **binaire final** après correction de version a aussi
confirmé 100 poses et 100 casses avec 1 bot en 6,613 s, sans cycle manquant,
erreur ni déconnexion inattendue ; fenêtre sélectionnée 19,32 TPS, P95/P99
50,2/52,5 ms. Ce passage ne remplace pas les charges 10/25/50 non répétées.

Les logs et rapports restent dans `tests/stress/results/network-benchmark-*`
de la copie distante ; `performance-final.json` conserve les chemins de chacun.

### Fichiers du lot et vérifications non effectuées

Code et API : `include/mc_block_state.h`, `include/mc_placement.h`,
`include/mc_nbt.h`, `src/world/nbt.c`, `src/world/block_state.c`, `src/gameplay/placement.c`,
`src/generated/generated_block_ids.c`, `src/generated/generated_block_ids.h`,
`src/protocol/handlers/play.c`.

Génération et déploiement : `Makefile`, `tools/generate_registry.py`,
`tools/gen_block_ids.py`, `tools/check_generated.sh`, `scripts/remote-build-stage.sh`.

Tests et preuves : `tests/test_block_registry.c`, `tests/test_chunk_network_codec.c`,
`tests/test_world_consistency.c`, `tests/test_anvil_roundtrip.c`,
`tests/test_nbt_arena.c`, `tests/test_registry_generator.py`,
`tests/stress/stress.py`, `tests/stress/benchmark_network.py`,
`tests/stress/test_block_persistence.py`, `tests/stress/test_block_placement.py`,
`tests/fixtures/vanilla-26.1.1-placement.json`, `tests/fixtures/README.md`,
`tests/stress/README.md` et le présent document.

Non effectués : tests Node/Mineflayer, essai manuel à deux vrais clients Java,
connexion Mojang positive sur le nouveau binaire, TSan fonctionnel, tests
complets des supports, des portes/trappes/escaliers/torches, formes d'entités et
attributs de portée, bordure dynamique, crash-consistance des chunks et charge
finale complète après la correction de DataVersion et de sa compatibilité de lecture. La phase 2A reste
ouverte au-delà de ce premier lot. Les déploiements principaux sont différés
sans altération de leurs sauvegardes.

## Phase 2A, lot 2 — états et voisinage des escaliers (27–29 octobre 2026)

Ce lot valide les scénarios d'escaliers décrits ci-dessous. La phase 2 et la
phase 2A restent ouvertes. Les résultats du lot 1 ci-dessus sont historiques ;
les mesures suivantes portent toutes sur le binaire final du lot 2, comprenant
déjà la correction DataVersion 4788 et la lecture des anciens chunks 4785.

### État initial et audit réutilisé

HEAD initial : `ac6b85a11e1aad3dee446c80065e8169e429edce`, après
`6ffedc7b66c8b8c7c83e66bdb7881fc27c310f29`. Aucun changement suivi au départ ;
`.vscode/`, `src/main.py` et `start-aider.sh` restent personnels et exclus.
Les références locales d'origin étaient encore au commit `86d615c8` ; un relevé
en lecture seule `git ls-remote origin` le 29 octobre confirme que main/HEAD
distant pointe toujours sur `86d615c86d80e8e7441fa9e54e6e582f91e8c11c`.
Aucun push, merge ou Git sur le serveur de test.

| Composant existant | Preuve / conformité connue avant le lot | Écart constaté | Réutilisation et correction prioritaire |
|---|---|---|---|
| Registre, propriétés, defaults, item→état | Rapports officiels, 1 168 blocs / 29 873 états ; tests du lot 1 | Aucun besoin de reconstruire le registre | Recherche des propriétés et états dans les tables existantes ; aucun ID d'escalier deviné |
| Décodage UseItemOn, portée, cible READY, inventaire | Tests C et 18 scénarios différentiels du lot 1 | Logique spécifique d'escalier dans play.c, facing inversé | Même décodage/validation ; délégation du calcul au module gameplay |
| Forme des escaliers | `straight` fixé au placement | Quatre angles absents ; pas de recalcul après pose/casse/rotation voisine | Règle locale pure, puis réconciliation autoritaire |
| Notifications des voisins | Connexions de certaines familles calculées dans play.c | Aucun chemin générique pour la forme des escaliers | Hook dans le monde, commun aux poses, commandes et destructions |
| Collision au placement | AABB du poseur, dalles partielles | Escalier traité comme cube plein | Huit voxels de demi-bloc pour les cinq formes et deux moitiés |
| Mutation, updates et cache chunk | mc_chunk_set_block, révision, dirty, updates_push ; tests C | Les nouvelles formes doivent suivre exactement ce chemin | Réutilisé pour chaque escalier modifié, sans changement du modèle de concurrence |
| Persistance Anvil/NBT | Tests de palette, roundtrip 4788 et lecture 4785 | Quatre attentes de facing incorrectes dans le test historique | Attentes corrigées ; angles et frontière négative ajoutés au roundtrip monde |

### Référence officielle et architecture

La référence est le JAR interne **officiel 26.1.1** installé sur l'hôte distant,
SHA-256 `3647564632a2f589be2a87bf544f628e5f67cb3780c86f7fa972ec36892aa081`.
`javap` du `StairBlock` de ce JAR a permis de vérifier `getStateForPlacement`,
`getStairsShape` et `canTakeShape`. Rapport blocks.json déjà vérifié au lot 1 :
SHA-256 `c4f6aafb57a085df217fcb93533de7c8baa5711ff6eecad07563b28813713cc0`.
Les observations réelles du serveur officiel confirment ensuite les résultats,
indépendamment du modèle C. La capture compacte est conservée dans
`tests/fixtures/vanilla-26.1.1-stairs.json`.

`src/gameplay/block_neighbors.c` et `include/mc_block_neighbors.h` fournissent
le calcul pur : orientation du joueur, moitié selon face et hitY, forme par
lecture du voisinage, sélection source/écoulement pour waterlogged, voxels.
L'interface de lecture distingue état disponible, chunk indisponible et erreur.
Le dispatcher ne traite actuellement que les escaliers ; il n'introduit aucun
scheduled tick, random tick, moteur de fluides ou redstone.

Le facing suit la direction horizontale du joueur : yaw 0=sud, 90=ouest,
180=nord, 270=est. La moitié est supérieure sur la face du dessous ou sur une
face latérale avec hitY strictement supérieur à 0,5. Un escalier perpendiculaire
de même moitié devant produit un angle extérieur ; derrière, un angle intérieur.
Un escalier de même facing et même moitié sur le côté pertinent bloque l'angle.
L'angle extérieur est prioritaire. Le matériau n'intervient pas. Les états des
voisins consultés utilisent facing/half, jamais leur shape : une passe sur le
bloc modifié et ses quatre voisins horizontaux suffit, sans boucle récursive.

`mc_world_resolve_block_state` résout l'état prospectif avant la collision et
la consommation. `mc_world_set_block` réconcilie les escaliers concernés après
mutation ; chaque changement utilise mc_chunk_set_block et updates_push,
invalide la révision de chunk et marque dirty. Le broadcast et la sauvegarde
existants restent utilisés. Une rotation par commande et une destruction ne
dépendent donc pas du seul handler UseItemOn. play.c délègue le calcul et la
collision ; il n'accueille pas l'algorithme des angles.

Les réconciliations appartiennent au tick. L'intégration d'un chunk READY
réexamine ses escaliers et ceux des quatre chunks voisins READY, avec filtrage
des sections par palette. Le chemin d'arrêt intègre aussi les résultats workers
restants avant sauvegarde. Un voisin indisponible ne vaut pas air : la pose est
refusée tant que les dépendances nécessaires manquent ; les formes chargées
sont conservées jusqu'à disponibilité du voisin. Aucun chargement synchrone,
nouveau thread ou nouvelle propriété de l'état partagé.

### Matrice différentielle exécutée

Même script, mêmes actions et positions sur GoatCore :25589 et Vanilla :25590,
deux mondes isolés sur **10.0.4.66**, opérateur offline synthétique Ofwood_ et
second bot observateur. Le scénario émet les vrais paquets 775 : rotation 32,
UseItemOn 66, PlayerAction 41, inventaire Creative 56 ; les codecs existants
restent réutilisés, y compris les séquences traversant 127/128.
Chaque observation vérifie le nom et toutes les propriétés, pas seulement
l'apparence. L'état autoritaire est lu via la commande QA GoatCore ou
`execute if block` sur Vanilla ; les poses réussies et transformations sont
aussi attendues sur les deux connexions. Les refus sont contrôlés dans le monde
et l'inventaire, sans attendre artificiellement une notification d'air Vanilla.
Le contrôle final des deux refus reconnecte le poseur sans réinitialiser sa
pile : l'inventaire autoritaire reçu doit encore contenir quatre escaliers,
ce qui évite de déduire une absence de consommation d'une liste vide d'updates.

| Scénarios | Observations nommées | Résultat GoatCore / Vanilla |
|---|---:|---|
| Quatre facings × deux moitiés, Survival 4→3 items | 8 | Identiques |
| Quatre faces latérales × hitY 0,25 / 0,5 / 0,75 | 12 | Identiques, seuil strict vérifié |
| Quatre facings × deux moitiés × intérieur/extérieur × gauche/droite ; pose initiale, pose voisine, rotation, restauration, destruction | 160 | Identiques sur les deux clients et dans le monde |
| Matériaux oak/stone, veto latéral, moitié opposée du veto | 6 | Identiques |
| Priorité extérieur et repli intérieur après veto | 2 | Identiques |
| Pose dans une source d'eau confinée | 1 | waterlogged=true identique |
| Cible occupée, collision du poseur, pose autorisée dans la partie vide | 3 | Résultats identiques ; refus sans consommation |
| Arrêt propre, redémarrage et reconnexion | 8 états supplémentaires | Quatre types d'angle et straight, deux moitiés, identiques après relecture |

Total : **192 observations identiques**, plus **8 états persistés identiques**.
Les fixtures et angles traversent les limites de chunks X=-16 et Z=16, avec
coordonnées X négatives. La matrice teste des placements oak_stairs et un voisin
stone_stairs ; elle ne prouve pas séparément toutes les essences du registre.
Les rotations de voisin sont préparées avec `/setblock` : il n'existe pas de
rotation générique d'un escalier placé par simple clic en Vanilla. La destruction
des 32 voisins est effectuée par le vrai paquet Creative PlayerAction.

La persistance ne se contente pas d'un ACK : les huit états finaux proviennent
de poses réelles, sont interrogés avant l'arrêt, puis relus depuis le même monde
après un nouveau processus et une reconnexion. Le test C vérifie en plus dirty,
révision, présence dans la queue de mises à jour et recalcul entre chunks chargés
séparément. Pas de validation de crash-consistance lors d'un arrêt brutal.

### Tests, sanitizers et erreurs rencontrées

Compilation et `GOATCORE_SOURCE_EXPORT=1 make -B -j4 precommit-check` réussis
dans `/opt/goatcore/phase2a-stage.7gYhw1` sur 10.0.4.66. La variable signale
l'export sans métadonnées Git ; elle ne lance ni Git ni serveur sur le PC.
Les tests existants de réseau à 1/2/4 workers, inventaire, combat, auth, NBT,
palettes, chunks et lecture 4785 restent passants. Tests Python : générateur
de registre (3) et infrastructure stress (12), réussis.

Tests C ajoutés/complétés : `test_stairs` couvre toutes les rotations, moitiés,
angles, veto, priorité, conservation waterlogged, eau source/écoulement,
seuil hitY, voxels et lecture indisponible ; `test_world_consistency` couvre
frontière négative, révision/cache, update, sauvegarde et chargement séparé ;
`test_anvil_roundtrip` corrige les quatre attentes d'orientation inversées.
Le test d'Anvil conserve ses cas de corruption volontaire : son message
`anvil load failed ... world_corrupt` est attendu.

Le runner `run_block_checks.py` lance, arrête et redémarre une instance jetable
avec un répertoire unique. Il interdit les ports 25565 et 25587, conserve logs,
rapports et snapshot, et garantit l'arrêt du sous-processus dans finally.
Le scénario historique `test_block_placement.py` a également réussi : 18 cas,
deux observateurs, Survival/Creative, blocs pleins, bûches, dalles et fusion,
refus, inventaire, BlockEvent shulker ID 677, puis quatre états et inventaire
restaurés après redémarrage. Pas de nouveau test différentiel exhaustif du minage.

ASan+UBSan : build distant `-O1 -g -fsanitize=address,undefined
-fno-omit-frame-pointer -fno-pie`, lien `-no-pie`,
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`, `UBSAN_OPTIONS=halt_on_error=1`.
`test_stairs`, `test_world_consistency` et `test_anvil_roundtrip` réussis, ainsi
que la matrice d'escaliers complète avec redémarrage et la suite historique de
placement avec redémarrage. Aucun diagnostic sanitizer dans ces passages.
Empreinte de ce binaire instrumenté :
`4f2a9befb1b9a76e30965e1a51dfdf8f98a46fe0ee8d5bd7d338fb9579ddbf9f`.
TSan : sonde minimale échouant avant tout test, `unexpected memory mapping`,
code 66. Aucune validation des races par TSan.

Deux problèmes de préparation ont été rencontrés et corrigés avant les passages
finaux : une attente de snapshot attribuait un angle à l'escalier terminal dont
le voisin était latéral (la forme correcte est straight) ; un arrêt Vanilla a
dépassé la borne initiale de 30 secondes et a été tué par le runner. La borne est
désormais de 60 secondes et les passages finaux edit/restart ont terminé
proprement. Les anciens logs restent conservés à distance ; ils ne sont pas
présentés comme des validations réussies.

### Performance du binaire final

Binaire normal testé, SHA-256 :
`8654181ee8d704efc0b72fe0fe76b4d6c768006bde65187fc193b9309a67b917`.
Les tests fonctionnels normaux et toutes les charges ci-dessous utilisent ce
même fichier `mc_server.normal`. Les services principaux n'ont pas reçu ce
binaire. Commande distante, exécutée séquentiellement pour N=1,10,25,50 :

```sh
python3 tests/stress/benchmark_network.py --server-binary ./mc_server.normal \
  --workers 1 --mode blocks --bots N --cycles-per-bot 100 \
  --duration 180 --block-cooldown 0.01
```

Un worker réseau, deux workers chunks, monde plat jetable frais, 100 cycles
pose+casse par bot. Les compteurs portent sur les BlockUpdate réellement reçus,
pas sur les actions envoyées. Les percentiles/TPS suivants proviennent de la
pire fenêtre active complète de 100 ticks retenue par le script (plus grand
P95 parmi les fenêtres proches du pic de joueurs), pas de tout le test.
P99=1000 ms signifie le dernier bucket plafonné, pas un maximum exact.

| Bots | Poses / casses confirmées | Manquants / erreurs / déconnexions | Durée s | TPS fenêtre | MSPT P50 / P95 / P99 ms | >50 ms fenêtre ; toutes fenêtres complètes |
|---:|---:|---:|---:|---:|---:|---|
| 1 | 100 / 100 | 0 / 0 / 0 | 6,613 | 19,52 | 32,1 / 46,9 / 51,9 | 3/100 ; 3/100 |
| 10 | 1000 / 1000 | 0 / 0 / 0 | 19,089 | 6,99 | 98,7 / 169,0 / 199,0 | 86/100 ; 86/100 |
| 25 | 2500 / 2500 | 0 / 0 / 0 | 39,765 | 3,81 | 152,0 / 424,4 / 1000,0 | 87/100 ; 87/100 |
| 50 | 5000 / 5000 | 0 / 0 / 0 | 100,974 | 5,24 | 25,1 / 421,6 / 670,1 | 38/100 ; 191/500 |

Les ticks de la dernière fenêtre incomplète ne sont pas inclus dans la dernière
colonne. Les taux ne forment donc pas une mesure exhaustive de tous les ticks.

| Bots | Latence update moyenne / max ms | CPU processus % | Max des moyennes sauvegarde périodique ms/tick | Max sauvegarde chunk / flush explicite ms |
|---:|---:|---:|---:|---:|
| 1 | 20,48 / 102,54 | 77,03 | 1,645 | 2,816 / 9,950 |
| 10 | 74,70 / 279,93 | 50,35 | 2,010 | 4,369 / 66,882 |
| 25 | 160,93 / 1299,10 | 41,66 | 1,758 | 3,972 / 380,244 |
| 50 | 421,02 / 3300,43 | 44,49 | 1,467 | 11,966 / 1918,179 |

CPU : somme des threads du serveur avant shutdown via /proc, 100 %=un cœur,
hors bots. Les latences incluent client/réseau/tick ; les temps de sauvegarde
sont ceux des fenêtres MC_PERF, pas une durée totale d'arrêt. Un échantillon
lent à 50 bots montre 2965 ms dans les items, dont 2924 ms dans leur ramassage ;
ce chemin préexiste et n'est pas refondu dans le lot.

Comparaison supplémentaire avec les sources exactes du HEAD initial ac6b85a1,
exportées par Git **local** puis compilées à distance sans Git. Même client,
10 bots × 100 cycles, chaque version reçoit sa propre copie du même monde :

| Binaire | Poses / casses | Manquants / erreurs / déconnexions | Durée s | TPS fenêtre | MSPT P50 / P95 / P99 ms |
|---|---:|---:|---:|---:|---:|
| ac6b85a1 | 1000 / 1000 | 0 / 0 / 0 | 17,593 | 8,25 | 84,8 / 151,4 / 170,3 |
| Lot 2 final | 1000 / 1000 | 0 / 0 / 0 | 18,613 | 8,37 | 86,3 / 163,2 / 219,6 |

Quantité d'actions égale, mais une seule paire, IO et charge hôte non contrôlés,
chargement réseau non identique (3707 contre 3811 chunks reçus). Le candidat
prend 5,8 % de temps global supplémentaire et sa queue MSPT est plus mauvaise
dans les fenêtres choisies. **Aucun gain ni absence de régression temporelle
n'est démontré ; les 20 TPS ne sont pas validés.** Aucune optimisation prématurée
pour masquer ce résultat.

Charge supplémentaire oak_stairs, ID item 442 lu dans le registre 26.1.1 :
10 bots × 100 cycles, 1000 poses + 1000 casses, zéro manquant/erreur/déconnexion,
19,632 s ; fenêtre 6,59 TPS, P50/P95/P99 103,7/177,4/244,1 ms, 86/100 ticks
au-delà de 50 ms ; latence moyenne/max 77,90/272,19 ms, CPU 55,77 %.
Ce passage du 29 octobre ne se compare pas directement à ceux du 27 octobre.
Un premier passage de 20 cycles par bot avait confirmé 200+200 opérations en
5,534 s, mais n'avait aucune fenêtre complète MC_PERF : ses TPS restent inconnus.
Ces charges ne couvrent pas un monde dense en escaliers ni les pires coûts de
réconciliation au chargement de sections directes.

Preuves brutes : `tests/stress/results/stairs-lot2-final/` dans la copie locale
(ignorées par Git), et `tests/stress/results/` dans la copie distante isolée.
Rapports : `stairs-final`, `vanilla-stairs-final`, `placement-regression`,
`asan-stairs`, `asan-placement`. Le contrôle renforcé de l'inventaire après refus
est répété le 29 octobre dans `stairs-checked`, `vanilla-stairs-checked` et
`asan-stairs-checked`, sur les mêmes binaires ; benchmarks `network-benchmark-20261027T174458Z`,
`174506Z`, `174525Z`, `174606Z`, `174747Z`, `174805Z`, `174824Z`, et
`network-benchmark-20261029T191447Z`. Les logs racine `performance-*.log`,
`precommit.log`, `asan-*.log` et `tsan-probe.log` donnent commandes et résultats.

### Limites et état de l'infrastructure

Conforme **dans les scénarios comparés** : facing, half, cinq shapes, veto,
priorité, mélange de matériaux, changements après pose/casse/rotation,
source-water placement, notification des deux joueurs et relecture sauvegardée.
Le calcul est commun aux familles `_stairs` du registre ; la couverture réelle
n'inclut pas toutes les essences ni toutes les géométries de placement.

Collision limitée à la forme de l'escalier contre l'AABB existante du poseur.
Pas de validation complète du mouvement sur les marches, step-height, postures,
autres joueurs, mobs ou autres entités. Les formes de voisinage peuvent changer
après le placement ; aucun moteur général de résolution physique n'est ajouté.
Waterlogged conserve sa propriété à travers un changement de forme ; seule la
pose en source confinée est testée en différentiel. L'exclusion des écoulements
est vérifiée en C et contre le prédicat du JAR ; leur simulation, restauration
de l'eau à la casse et ticks fluides ne sont pas validés. Les anciennes règles
waterlogged des autres familles sont préservées. Restent aussi les tests de
chunks extrêmement denses, clients Java réels, auth Mojang positive du candidat,
TSan fonctionnel et crash-consistance des chunks. Aucun de ces points n'est
présenté comme acquis.

Aucun remplacement du binaire principal, écrasement de monde, changement de
configuration, permissions, authentification, start/restart/stop de service
principal n'a été effectué par ce lot. Empreintes principales avant/après égales :

- `/opt/goatcore/app/mc_server` : `f840ed3f535d80d8b87293b1ada6fdf88cacd6c681740f1dcfa3b06bd1dc423c`.
- config online : `59a6b486122324c914563879fd8f43b2498695392498bc149858a7b25b423abd`.
- config offline : `5d95cf9989874b72faddfeb39fe1abc7f3a4d545bf34ad98fbd7d3ce757f4517`.

Écart d'exploitation constaté lors de la reprise le 29 octobre : au relevé
initial les deux services étaient actifs (PID 10634/10621), au relevé final
25565 est actif (PID 140) et **25587 est inactif**. Le journal indique un arrêt
propre de goatcore-test.service le 27 octobre à 21:04:07 ; son auteur n'est pas
identifié ici. L'hôte a redémarré depuis le premier relevé. La consigne de ne
pas intervenir sur ces services est respectée ; 25587 n'est pas relancé.
Les processus temporaires sont arrêtés et 25589/25590 libérés. Les permissions
des services principaux n'ont pas été changées ni retestées en jeu ; celles de
l'opérateur synthétique fonctionnent sur les instances temporaires uniquement.

Fichiers du lot : `Makefile`, `include/mc_block_neighbors.h`, `include/mc_world.h`,
`src/gameplay/block_neighbors.c`, `src/world/world.c`,
`src/protocol/handlers/play.c`, `tests/test_stairs.c`,
`tests/test_world_consistency.c`, `tests/test_anvil_roundtrip.c`,
`tests/stress/run_block_checks.py`, `tests/stress/test_stair_placement.py`,
`tests/stress/benchmark_network.py`, `tests/stress/stress.py`,
`tests/stress/README.md`, `tests/fixtures/vanilla-26.1.1-stairs.json`,
`tests/fixtures/README.md` et ce document.

Suites recommandées : étendre progressivement les tests de collision/placement
des escaliers, profiler la charge existante avant toute promesse de performance,
puis traiter les supports des torches et les portes/trappes dans les prochains
lots 2A. L'interface de voisinage pourra servir à la future 2B ; les collisions
générales restent à la future 2C. Ces phases ne sont pas commencées dans ce lot.

## Phase 2 — lot technique : tick, E/S et service 25587 (29 octobre 2026)

HEAD initial `477c2b6b18d6cc259f2900ee2377476b349d9ee9`, aucun changement suivi.
Les trois éléments personnels `.vscode/`, `src/main.py`, `start-aider.sh` sont
préservés et exclus. Référence de comparaison : `ac6b85a1`. Aucun push, merge,
réécriture d'historique ni Git sur l'hôte distant. Ce chantier n'ouvre aucune
autre phase de gameplay.

### Enquête en lecture seule : arrêt normal du conteneur de test

Au début de l'enquête : 25565 actif, PID 140 ; 25587 inactif, sans PID.
Le système distant s'identifie comme un **conteneur LXC**. Son journal précédent
établit cette séquence UTC le 27 octobre, avec précision à la microseconde :

- 21:04:07.867493 : systemd PID 1 reçoit `SIGRTMIN+3`.
- 21:04:07.888127 : activation de `halt.target`.
- 21:04:07.925338 : arrêt de goatcore-test.service ; à .926429, arrêt du service
  principal, avec les autres unités de la séquence générale d'arrêt.
- 21:04:07.964543 : goatcore-test.service désactivé avec succès ; à .965325,
  service arrêté, sans message de crash ni de dépassement du délai d'arrêt.
- 21:04:08.576609 : systemd-halt.service ; à .586321, `Shutting down`.

Il s'agit donc d'un arrêt ordonné du conteneur, et non d'un crash isolé du
serveur offline. Le journal ne fournit pas l'identité de l'émetteur du signal,
ni le motif de la demande d'arrêt de l'environnement. Une action d'orchestrateur
ou d'administrateur reste possible, sans preuve permettant de l'attribuer.
Il n'existe pas de preuve d'OOM dans le journal accessible du boot concerné ;
les événements noyau/hyperviseur hors du conteneur ne sont pas entièrement
observables ici, donc aucun diagnostic global d'absence d'OOM n'est revendiqué.

L'unité offline est **static**, sans `[Install]`, `Restart=no`, sans limite de
durée (`RuntimeMaxUSec=infinity`), `StopWhenUnneeded=no`, ni dépendance BindsTo
ou PartOf. Aucun timer d'arrêt GoatCore n'est listé. L'unité principale est
activée au démarrage. Après les boots suivants, dont celui du 27 à 22:04:34,
le service principal démarre à 22:04:35, mais aucune dépendance ne demande le
démarrage automatique du service offline. Cela explique son état actuel.

Les scripts du dépôt pouvant agir sur 25587 ont été lus : remote-test-server.sh
peut le démarrer/l'arrêter explicitement ; deploy-remote.sh ne le redémarre que
s'il était actif. Aucun journal ne relie leur invocation à cet arrêt ; la
séquence halt englobe tous les services. Aucun de ces scripts d'exploitation
n'a été exécuté pour modifier les services pendant ce chantier.

Restauration proposée, **uniquement sur autorisation ultérieure** : relire la
configuration offline, vérifier port et monde distincts, puis démarrer
goatcore-test.service avec son binaire existant. Les risques sont une reprise
des anciennes données de test et la charge de l'instance additionnelle ; cela
ne doit pas servir de prétexte pour remplacer le binaire partagé ou activer
l'unité au boot sans demande distincte. Aucune restauration n'a été effectuée.

### Méthode de comparaison et limites de mesure

Sources exportées depuis Git **local** vers une copie isolée créée par
scripts/remote-build-stage.sh : `/opt/goatcore/phase2a-stage.KTpn22`.
Les deux révisions utilisent les mêmes options C11/O2, bibliothèques, assets
26.1.1 et paramètres. src/net/server.c était identique dans ac6b85a1 et 477c2b6b ;
le même ajout de trace optionnelle a été appliqué à chacun pour la première
comparaison. Il ne s'agit donc pas des octets originaux de ces deux commits,
mais de leurs comportements avec une instrumentation commune, explicitement
identifiée par les SHA-256 des binaires.

Un worker réseau, deux workers chunks, zéro simulation worker ; affinité auto,
même cpuset disponible de 12 CPU logiques (`0-4,6-9,11,14-15`), sans exclusivité
sur l'hôte physique. Distances de vue et simulation 10, seed 0, mode offline,
skins offline désactivés. Le disque du conteneur est ext4 sur un volume LVM.
Les services principaux restent dans leur état initial pendant les mesures.
Le CPU, le stockage physique et d'éventuels voisins de l'hôte ne sont pas
réservés : les répétitions et la pression IO observée ne suppriment pas cette
limite d'attribution.

Le driver profile_tick.py réutilise les bots, codecs, cibles et block_cycle de
stress.py, y compris le même logger de paquets flushé. Il copie un unique monde
préparé, dont les fichiers sont identifiés par manifeste SHA-256 ; mêmes noms
offline et mêmes positions sur chaque copie. Les étapes sont séparées :
connexions/réception de **441 chunks par bot**, slot créatif et nettoyage de
cible ; échauffement 5 s ; repos initial 5 s ; **100 cycles pose+casse par bot** ;
repos final 5 s. Aucun bot ne part dès la fin de ses propres actions : le nombre
d'observateurs demeure constant jusqu'à la fin du repos final. Trois répétitions
par couple version/effectif, avec ordre des versions alterné entre répétitions.

Chaque action compte après le BlockUpdate attendu. Les préparations n'entrent
pas dans le nombre de poses/casses mesurées. Un échec ou une action manquante
fait échouer la passe, sans réduire silencieusement la quantité de travail.
Le driver ajoute une collecte des TakeItemEntity dans la comparaison après
correction pour distinguer le nombre de ramassages du nombre d'écritures disque.
Ce compteur supplémentaire est commun aux deux binaires de cette comparaison.

La trace CSV horodatée par CLOCK_MONOTONIC enregistre tous les ticks, sans
sélection de fenêtre de 100 ticks ni bucket MSPT plafonné. TPS = débuts de tick
dans la phase / durée réelle de la phase. Une incertitude de discrétisation
d'un tick peut produire un taux légèrement supérieur à 20 sur une courte durée.
MSPT = durée complète des ticks chevauchant la phase, avec compte séparé des
ticks de bord et inclusion du dernier tick lent. Les percentiles sont à rang
le plus proche. La période entre débuts de tick inclut attente, passes rapides,
retard et resynchronisation ; le MSPT du tick programmé exclut les passes rapides.
Ces deux grandeurs ne sont donc pas réciproques. Les durées sont conservées
dans les CSV pour recalculer les distributions et vérifier nombre de ticks/TPS.

L'instrumentation existante détaille handlers, flush chunks, phases du monde,
items, réseau et verrous. MC_PERF_TRACE_PATH ajoute seulement une sortie CSV
bufferisée lorsqu'elle est demandée avec MC_PERF=1. Sans cette option, aucune
écriture ni lecture d'horloge supplémentaire de ce point de trace n'a lieu.
La sonde de tests profile_fsync.c est préchargée uniquement dans l'instance
temporaire ; elle conserve résultat/errno et relève wall time, CPU et fichier
de chaque fsync. Les coûts imbriqués ne doivent pas être additionnés.

Le moniteur conserve des échantillons à 250 ms du CPU serveur et client, RSS,
high-water mark, IO du processus, schedstat/wchan des threads, pression
CPU/IO/mémoire et compteurs CPU globaux. Les files et attentes de verrous sont
présentes dans la trace continue. Les maxima mémoire échantillonnés et CPU sur
les bornes d'échantillonnage ne sont pas des intégrales exactes de la phase.
Les appels fsync chevauchant une frontière sont comptés entiers. Les fichiers
de trace ajoutent une petite charge de diagnostic, commune aux versions testées.

Le high-water mark de la file est cumulatif depuis le démarrage et peut inclure
la préparation ; task_depth mesure la profondeur observée au point de trace.
Le compteur de latence de file préexistant utilise l'heure de début du drain :
il ne mesure pas toute l'attente derrière les handlers du même batch. Les
latences client incluent aussi décodage et traitement Python. Leurs résumés sont
conservés, mais pas une trace horodatée exhaustive de chaque envoi réseau.

perf, strace et gdb ne sont pas disponibles ; gprof l'est. Le profil CPU dédié
est distinct des mesures de débit normales. Ses échantillons et compteurs ne
mesurent pas le temps bloqué dans les appels système ; les fonctions inlinées
et les bibliothèques non profilées limitent l'attribution. Les mesures murales
par phase et de fsync constituent les preuves principales des attentes.

### Correction unique : barrière de sauvegarde des ramassages

La référence et le lot 2 sauvegardent le fichier joueur **après chaque slot
ramassé**, depuis proto_play_try_pickup_ground_slot, au milieu de la boucle
d'items. Chaque écriture atomique compresse le NBT, synchronise le fichier
temporaire, renomme, puis synchronise le répertoire. À 10 bots, le premier
passage de référence montre 286 fsync représentant 6,378 s de temps mural pour
0,026 s CPU, sur 14,223 s de charge active. À 50 bots, son premier passage
montre 25,634 s dans fsync et un tick maximal de 1,878 s. Ce coût préexiste aux
escaliers et justifie de cibler les répétitions de commits de ramassage.

La correction conserve l'ordre des absorptions d'inventaire et de tous les
paquets d'inventaire, TakeItemEntity, suppression et métadonnées. Le chemin
de ramassage de la passe d'items marque seulement un flag tick-owned par
connexion ; à la fin de **cette même passe**, chaque destinataire modifié fait
une sauvegarde atomique complète. Le fsync du fichier **et** du répertoire
restent présents. Le chemin public immédiat reste disponible pour les appels
hors de la passe d'items. Aucune sauvegarde async, nouveau thread, modification
des règles de ramassage, validation, protocole, scheduler ou ordre des updates.
Les flushs de chunks restent inchangés : aucune seconde optimisation.

Un échec de sauvegarde ferme la connexion comme auparavant et conserve le flag
pour la sauvegarde habituelle de départ. Un socket fermé pendant la passe ne
fait pas abandonner les objets déjà acceptés. Le flag ne concerne que des
mutations déjà réalisées par le tick et ne traverse pas un worker.

**Compromis de durabilité explicite :** le commit disque intervient à la fin
de la passe d'items, au lieu d'entre deux ramassages. Un arrêt brutal avant
cette barrière peut perdre les ramassages de cette passe ; la borne est logique,
pas une promesse de quelques millisecondes. Le dernier fichier joueur reste
atomique, et la fin de passe est synchrone. Cela ne crée pas de transaction
atomique entre inventaire, items au sol et chunks, dont la crash-consistance
globale n'était déjà pas validée. Les tests ne prétendent pas garantir ce cas.
L'effet attendu se limite aux passes recevant plusieurs ramassages pour un
même joueur ; une passe avec un seul ramassage ne supprime aucun fsync.

### Révisions ac6b85a1 / 477c2b6b : mesures continues avant correction

Les 24 passes normales ont terminé avec tous leurs cycles confirmés, zéro
manquant, zéro échec et aucune déconnexion avant la fermeture volontaire.
Chaque ligne ci-dessous regroupe **trois répétitions** : durée médiane d'une
passe, TPS agrégés (somme des ticks / somme des durées), percentiles de tous les
ticks actifs regroupés, nombre total de dépassements et maximum de l'ensemble.
Il ne s'agit ni de moyennes de percentiles ni de la meilleure fenêtre.
Les actions indiquées sont confirmées **dans chacune des trois passes**.

| Version | Bots | Poses / casses par passe | Durée médiane s | TPS agrégés | MSPT P50 / P95 / P99 ms | >50 ms / ticks | Max ms |
|---|---:|---:|---:|---:|---|---:|---:|
| ac6b85a1 | 1 | 100 / 100 | 4,834 | 19,99 | 0,334 / 3,071 / 4,245 | 0 / 291 | 7,989 |
| 477c2b6b | 1 | 100 / 100 | 4,853 | 19,93 | 0,207 / 3,687 / 5,849 | 0 / 293 | 9,707 |
| ac6b85a1 | 10 | 1000 / 1000 | 15,399 | 13,29 | 50,810 / 117,240 / 153,164 | 306 / 606 | 399,070 |
| 477c2b6b | 10 | 1000 / 1000 | 14,780 | 14,65 | 46,118 / 105,766 / 181,057 | 291 / 644 | 250,054 |
| ac6b85a1 | 25 | 2500 / 2500 | 32,974 | 8,69 | 50,138 / 182,227 / 269,297 | 429 / 854 | 588,336 |
| 477c2b6b | 25 | 2500 / 2500 | 32,594 | 8,96 | 52,817 / 177,171 / 288,511 | 457 / 877 | 1026,731 |
| ac6b85a1 | 50 | 5000 / 5000 | 91,353 | 6,55 | 16,037 / 392,484 / 1121,647 | 540 / 1822 | 3657,326 |
| 477c2b6b | 50 | 5000 / 5000 | 94,841 | 6,53 | 15,411 / 384,416 / 1054,444 | 548 / 1883 | 5258,967 |

Les deux versions sont proches dans ce scénario, avec des variations de queue
de latence et du nombre de ramassages. Les résultats ne montrent pas une
régression systématique imputable aux escaliers ; ils ne prouvent pas non plus
un coût nul de tout voisinage ou de chargements denses en escaliers.
L'ancienne charge démarrait des actions pendant l'arrivée des chunks et faisait
partir chaque bot dès sa fin de travail. Ces deux différences interdisent une
comparaison numérique directe avec les anciens tableaux du lot 2.

Le P50 plus faible à 50 qu'à 25 ne signifie pas que 50 joueurs coûtent moins :
les longues rafales de travail alternent avec de nombreux ticks courts. La
médiane masque cette distribution ; P99, maximum et débit global se dégradent.
Dans cette campagne continue, le débit à 50 est inférieur à celui à 25. Les
anciens classements de fenêtres sélectionnées pouvaient aussi mélanger des
phases de chargement et des effectifs d'observateurs décroissants.

Au repos **avant** les actions, P95 du lot 2 : 0,130 / 0,209 / 0,985 / 3,069 ms
pour 1/10/25/50 bots. Après les actions, avec les items encore présents :
0,275 / 1,895 / 6,429 / 14,241 ms. Les fortes chutes se manifestent donc surtout
pendant les modifications et ramassages, et non dans le seul maintien de joueurs
connectés immobiles. Ces repos ne sont que des observations de cinq secondes,
pas une validation de stabilité sur plusieurs heures.

Les traces identifient précisément les deux pires ticks à 50 bots :

- Référence, passe r2, tick 811 : 3,657 s, dont 3,398 s items ; 176 fsync
  occupent 3,091 s, CPU total du tick 0,338 s.
- Lot 2, passe r1, tick 845 : 5,259 s, dont 5,081 s items ; 260 fsync occupent
  4,612 s, CPU total du tick 0,365 s. La passe rapide précédente dure 0,119 s.

Ces pauses viennent principalement de la répétition d'attentes de stockage,
pas d'un calcul d'angle d'escalier d'une durée de plusieurs secondes. À 50 bots,
les trois passes du lot 2 totalisent 81,364 s dans 4 254 fsync, pour 0,344 s CPU
dans ces appels. Les phases handlers et flush chunks restent aussi coûteuses :
elles constituent une seconde piste de travail, distincte du regroupement des
sauvegardes de joueurs.

| Bots, lot 2 avant correction | CPU serveur / client %¹ | RSS serveur min–max KiB² | Profondeur de file observée max | Attentes push/drain cumulées ms³ | Latence action moyenne / max ms |
|---:|---:|---:|---:|---:|---:|
| 1 | 9,6 / 6,8 | 24668–24760 | 1 | 0,707 | 13,22 / 50,50 |
| 10 | 31,6 / 32,5 | 27540–27952 | 18 | 2,677 | 58,75 / 252,51 |
| 25 | 47,5 / 69,4 | 31940–32688 | 31 | 4,907 | 128,91 / 1097,17 |
| 50 | 53,1 / 72,8 | 39500–40240 | 95 | 5,778 | 417,21 / 5337,36 |

¹ 100 %=un cœur ; moyenne pondérée par durée des trois périodes actives.
² Enveloppe des échantillons des trois processus indépendants, pas une croissance
continue entre passes. ³ Somme des attentes de verrous de la file sur les trois
périodes actives ; les attentes de traitement des tâches sont une autre métrique.
Les verrous items/fourneaux mesurés restent également très courts. Ces données
ne désignent pas une contention de ces mutex comme cause dominante ; elles
n'excluent pas toutes les autres attentes, notamment les buffers réseau.

Les compteurs CPU globaux accessibles indiquent environ 9–18 % occupés, sans
steal relevé dans ces échantillons. À 10–50 bots, l'IO wait global est de l'ordre
de 4–5 % et la pression IO full de 9 % en moyenne pendant les phases actives.
La pression mémoire reste nulle dans les relevés disponibles ; aucun indice
de manque de RAM n'explique les pauses observées. Les autres processus visibles
dans le conteneur sont peu actifs. Il reste impossible d'attribuer entièrement
les variations du disque et des compteurs globaux à des voisins physiques non
visibles. Une charge externe peut contribuer à la variance, mais elle ne remplace
pas la preuve directe des attentes fsync dans les fonctions GoatCore.

Le client Python approche lui-même un cœur à forte charge : sa sérialisation
des lectures, la diffusion quadratique des updates et son logger influencent
les latences et le débit de bout en bout. Les métriques ne constituent donc pas
une capacité maximale du serveur avec un client sans coût. Le ramassage est
réellement simulé et les items encore au sol sont comptés ; ils ne sont ni supprimés
ni désactivés pour améliorer les chiffres.

### Avant/après correction : même travail de blocs

Une seconde campagne de 24 passes alterne le binaire du lot 2 et le correctif,
avec les mêmes mondes, scripts, réglages et 100 cycles/bot. Trois répétitions
par ligne, mêmes règles d'agrégation que ci-dessus. Chaque passe confirme
100/100, 1000/1000, 2500/2500 ou 5000/5000 poses/casses, selon l'effectif ;
zéro manquant, échec ou déconnexion prématurée.

| Binaire | Bots | Durée médiane s | TPS agrégés | MSPT P50 / P95 / P99 ms | >50 ms / ticks | Max ms |
|---|---:|---:|---:|---|---:|---:|
| Avant | 1 | 4,937 | 19,97 | 0,258 / 3,294 / 4,384 | 0 / 296 | 29,824 |
| Corrigé | 1 | 4,973 | 19,96 | 0,276 / 3,268 / 3,900 | 0 / 297 | 4,021 |
| Avant | 10 | 14,991 | 14,29 | 46,407 / 104,018 / 172,878 | 296 / 636 | 230,102 |
| Corrigé | 10 | 14,120 | 16,10 | 44,057 / 90,448 / 118,594 | 287 / 680 | 254,319 |
| Avant | 25 | 36,700 | 8,72 | 46,769 / 190,037 / 309,729 | 450 / 932 | 1267,432 |
| Corrigé | 25 | 30,982 | 10,63 | 37,085 / 140,566 / 192,876 | 431 / 994 | 1123,890 |
| Avant | 50 | 98,635 | 6,31 | 16,454 / 374,884 / 1312,971 | 561 / 1885 | 6434,419 |
| Corrigé | 50 | 78,142 | 9,71 | 14,497 / 278,707 / 374,636 | 585 / 2257 | 741,737 |

Le débit de blocs est amélioré dans les passages 10/25/50, sans réduction du
nombre de modifications confirmées. Les 20 TPS ne sont **toujours pas tenus**
à ces effectifs. À 50 bots, davantage de ticks sont effectivement simulés en
moins de temps ; le nombre absolu de ticks >50 ms ne baisse pas (585 contre
561), même si leur proportion et les queues MSPT diminuent. Le maximum à
10 bots est légèrement plus élevé après correction : aucune garantie universelle
sur tous les maxima n'est déduite de ces échantillons.

| Bots | fsync avant / après, trois phases actives | Temps fsync avant / après s | CPU processus avant / après % | Latence action moyenne avant / après ms |
|---:|---:|---:|---:|---:|
| 1 | 0 / 0 | 0 / 0 | 9,7 / 9,7 | 13,37 / 13,45 |
| 10 | 854 / 732 | 20,119 / 17,721 | 32,7 / 35,0 | 59,09 / 55,12 |
| 25 | 1518 / 772 | 31,844 / 17,825 | 44,7 / 47,1 | 143,55 / 122,60 |
| 50 | 5488 / 812 | 100,286 / 17,323 | 48,5 / 69,7 | 432,27 / 331,41 |

Le CPU relatif peut monter après réduction des attentes : le processus travaille
davantage par seconde et simule plus de ticks. Ce n'est pas une augmentation
artificielle de la fréquence du scheduler. Les flushs de chunks restent coûteux
et inchangés. À 50 bots, les latences maximales client passent de 6712,32 à
1219,56 ms dans cette campagne ; le client atteint environ 83 % d'un cœur après
correction, ce qui limite encore la lecture de ce résultat.

**Limite de comparaison des items :** les blocs sont à quantité strictement
égale, mais l'entrelacement entre actions, physique et TTL produit un nombre
différent de ramassages. Les premiers observateurs totalisent 2 744 événements
de ramassage avant et 2 420 après à 50 bots. Les 5 488 fsync avant correspondent
à deux appels par ramassage ; après, 812 appels sont répartis sur 2 420
ramassages. Une partie du temps global dépend aussi de cette différence de
simulation. L'ensemble du gain de débit ne peut donc pas être attribué au seul
regroupement, et cette campagne ne remplace pas un contrôle ciblé à nombre de
ramassages identique.

### Profil CPU complémentaire et attribution des coûts

Une passe séparée de 50 bots × 100 cycles utilise le lot 2 instrumenté `-pg -g
-fno-omit-frame-pointer`, avec zlib statique pour rendre ses symboles visibles.
Elle confirme 5000 poses et 5000 casses. Sa durée active de 109,335 s **n'entre
pas dans les comparaisons normales** : les compteurs gprof coûtent du CPU et
modifient fortement le profil d'exécution.

| Fonction | Part du temps propre échantillonné attribué par gprof |
|---|---:|
| mc_paletted_container_get_block | 14,54 % |
| cmp_nbt_tag_ptr_name | 11,51 % |
| mc_chunk_get_block | 9,62 % |
| handle_client_write | 5,72 % |
| mc_chunk_destroy | 5,14 % |
| mc_be_store_remove | 4,27 % |
| mc_paletted_container_destroy | 3,04 % |
| broadcast_item_entity_teleport | 2,53 % |
| send_player_equipment | 2,17 % |
| proto_play_sync_remote_player | 2,03 % |
| deflate_slow | 2,03 % |

Ces parts portent sur les **13,82 s de temps propre attribuées**, pas sur tout
le CPU du processus, ni uniquement sur la période active. Le profil inclut
préparation, repos et arrêt ; bibliothèques dynamiques, code inliné, mcount et
limites de collecte multithread empêchent une attribution exhaustive et des
comptages d'appels exacts. Les proportions ne doivent pas être converties en
pourcentage du temps mural du serveur.
Les destructions de chunks et de conteneurs incluent notamment le teardown ;
leurs échantillons ne sont pas présentés comme un coût de la seule charge active.

La lecture du code relie les getters chauds à build_section_block_states dans
Anvil : reconstruction de 4096 valeurs pour chaque section lors d'une sauvegarde,
y compris les sections uniformes. Les placements/destructions appellent
mc_world_flush_block et reconstruisent ainsi fréquemment le NBT. Le tri des tags
et la compression complètent ce chemin. Cette seconde source de CPU est donc
étayée, sans être optimisée dans ce lot. Les getters servent aussi à d'autres
lectures ; leur coût total ne peut pas être imputé exclusivement à Anvil.

Les fonctions mc_stairs_is_state, mc_stairs_placement et mc_block_neighbor_state
n'ont aucun temps propre échantillonné attribué dans cette passe de pierre.
Cela signifie un coût non distingué à cette résolution et dans ce scénario,
pas une preuve de coût nul ni une borne sur les chargements denses en escaliers.

Une charge supplémentaire réellement composée d'escaliers (item oak_stairs 442)
compare ac6b85a1 et le lot 2, trois répétitions à 10 bots × 100 cycles, sans
correction de sauvegarde. Chaque passe confirme 1000 poses et 1000 destructions.
Durées médianes : 14,879 / 14,793 s ; TPS agrégés : 14,56 / 14,11 ; P95/P99 :
111,400/162,132 et 106,959/174,750 ms. Les temps fsync des trois passes sont
21,023 et 20,351 s. Ces différences ne distinguent pas un surcoût systématique
du calcul d'escalier du bruit d'E/S et de l'entrelacement des ramassages. La
référence ac6b85a1 conserve son orientation erronée : cette charge compare le
travail exécuté, pas la conformité de ses états.

Le chemin des blocs ordinaires quitte rapidement le dispatcher de voisinage ;
le recalcul d'angles ne s'applique qu'aux escaliers concernés et à quatre voisins,
sans propagation récursive. Les intégrations de chunks utilisent un filtrage
de palette. Des réexamens de chunks voisins peuvent se répéter en monde dense ;
ce pire cas n'est pas validé par ces mondes plats et reste à mesurer. Aucun
calcul de voisinage inutile dominant n'est établi dans les scénarios présents.

### Contrôle ciblé : exactement 100 ramassages avant et après

Pour isoler la correction de la variation du nombre d'items de la charge de
blocs, test_pickup_save.py prépare 100 objets unitaires à la même position,
orientés vers le sol, et attend les chunks. Deux piles sont réellement vidées,
la fin des sauvegardes de préparation est confirmée par une lecture QA, et
aucun objet ne doit encore avoir été ramassé au début de la période mesurée.
Les deux clients observent ensuite **100 objets récupérés**. Une seconde lecture
QA borne la fin de la passe et de son commit disque, avant toute déconnexion.
Inventaire total 100 à la reconnexion, puis après arrêt/redémarrage, sur chaque
version. Trois répétitions, ordre alterné, deux observateurs maintenus connectés.

| Version | Ramassages confirmés par passe | fsync par passe | Durée active médiane s | Temps fsync médian s | Plus long tick des trois passes ms |
|---|---:|---:|---:|---:|---:|
| Lot 2 avant correction | 100 | 200 | 5,436 | 3,368 | 4006,740 |
| Corrigé | 100 | 2 | 1,966 | 0,036 | 45,407 |

Les trois temps corrigés sont 1,964 / 1,991 / 1,966 s, contre
5,433 / 5,941 / 5,436 s avant. Le délai normal de ramassage est inclus dans ces
durées ; les écritures de préparation, de logout et de restart en sont exclues.
Les trois anciens ticks de ramassage dépassent 50 ms ; aucun des nouveaux ne
le dépasse. Les six passes ont zéro erreur fsync. Cette réduction ne provient
donc pas d'une diminution du nombre d'objets récupérés. Ce cas groupé ne promet
pas le même gain pour des ramassages isolés ni pour toute la simulation.

Le serveur officiel 26.1.1 passe aussi le scénario à 100 objets et retrouve
l'inventaire total après redémarrage. La somme des quantités TakeItemEntity est
comparée, car Vanilla peut fusionner les entités items. ASan+UBSan passe le même
cas groupé sur le binaire corrigé. Le scénario initial à deux objets reste
également passant sur GoatCore et Vanilla, y compris reconnexion et restart.

### Validation fonctionnelle, sanitizers et preuves

`GOATCORE_SOURCE_EXPORT=1 make -B precommit-check` a réussi, puis a été rejoué
sur les sources finales : tous les tests C, génération, framing, réseau à
1/2/4 workers, login et smoke distant passent. Le binaire reconstruit conserve
exactement son SHA-256. Les 12 tests Python du stress et les 4 nouveaux tests
de mesure continue passent. Ces derniers vérifient notamment le tick lent qui
traverse la fin d'une phase, un tick recouvrant toute une phase, l'absence de
plafonnement à 1000 ms et la détection d'une trace incomplète.

Le test PVP étendu utilise un vrai monde et le player store, avec interception
de fsync limitée au binaire de test. Trois absorptions produisent trois paquets
d'inventaire et zéro écriture avant la barrière, puis exactement deux fsync et
un fichier relu contenant les trois objets. Il vérifie aussi le chemin immédiat,
une fermeture de socket, le refus Spectator, l'échec de fsync conservant le
fichier précédent et le flag, puis une nouvelle tentative qui persiste le bon
inventaire. Aucune interception ne fait partie du binaire serveur normal.

Le binaire normal corrigé passe la matrice complète des escaliers et voisins
contre une nouvelle exécution du JAR officiel : **192 observations identiques**
et **8 états identiques après redémarrage**, également identiques à la fixture
versionnée du lot 2. Deux observateurs, collision de placement, faces/moitiés,
angles, source d'eau, coordonnées négatives et frontières restent couverts.
La suite historique des 18 placements passe aussi, avec les quatre états et
l'inventaire relus après restart : blocs pleins, bûches, dalles/fusion, refus,
collisions partielles et BlockEvent shulker. Aucune logique de ces familles
n'a été optimisée pour obtenir les chiffres.

ASan+UBSan, compilation distante O1/g, `-fno-omit-frame-pointer -fno-pie`, lien
`-no-pie`, `detect_leaks=1:halt_on_error=1`, `UBSAN_OPTIONS=halt_on_error=1` :
test_pvp, test_stairs, test_world_consistency, test_anvil_roundtrip et
test_player_nbt réussis. Les trois suites réseau pickup/escaliers/placements,
chacune avec arrêt et relecture, réussissent aussi, ainsi que le cas de
100 ramassages. Aucun diagnostic sanitizer relevé. Les logs d'échec disque
`user=PickupBatch` et de fichier Anvil `world_corrupt` sont des injections
volontaires validées par les assertions, pas des échecs de ces suites.

TSan : nouvelle sonde minimale non-PIE échouant avant les tests avec
`unexpected memory mapping`, code **66**. Aucune validation d'absence de races.
La sonde fsync a été vérifiée sur un appel valide et sur un appel libc donnant
EBADF, avec conservation du code de retour et d'errno. Un premier contrôle
`os.fsync(-1)` était rejeté par Python avant libc ; il a été corrigé via ctypes.
Les prototypes du profiler avaient aussi nécessité l'ajout de stdio.h et du
callback de log du Bot avant les campagnes finales. Ces erreurs de harnais sont
distinctes des résultats serveurs, et leurs essais ne sont pas comptés comme
des mesures validées.

Empreintes des binaires distants conservés :

- ac6b85a1 + trace commune : `2d23f30521fe8aee9bd49fe63d0a1cd9c66ddb6c997f4323d0d405f19ab03091`.
- 477c2b6b + trace commune : `8ce4a8944e929a69c2ad5f13615f0d197ea4697a16b16cf223f6ccc36ac5d344`.
- **Final corrigé normal testé** : `2b78862df41da8e23cc0581cf40f8bdb9b4dd14be49c9740ab89bae60e315c7a`.
- ASan+UBSan : `c8255cd4622742a0ecf4708a2c22ad27eb26c8ff6a6f9f4291fd22ef1fc6041b`.
- Profil gprof/zlib statique, diagnostic seulement : `ec5e282625aff344e6dc83ad3960aab20b67b3e4ec20f4a5ae1f2513f28f6b88`.

Résultats bruts dans tests/stress/results de la copie distante KTpn22, récupérés
dans `tests/stress/results/tick-diagnostic/` local, ignorés par Git :
profile-matrix, profile-correction, profile-stairs, profile-cpu, fixed-pickup-*,
stairs-corrected, stairs-vanilla-technical, placement-corrected et asan-*-technical.
Les CSV de tous les ticks et fsync, JSON des ressources/actions, snapshots,
journaux serveur/bots, manifeste de monde, gmon.out, gprof.txt, logs de build,
precommit et sanitizers permettent de vérifier les tableaux. analyze.py et les
JSON d'agrégation locaux conservent le calcul des regroupements ; les rapports
par passe restent disponibles sans sélection d'un meilleur essai.

### Conclusions d'attribution et travail restant

| Question | Conclusion étayée / limite |
|---|---|
| Ralentissements antérieurs au lot 2 ? | Oui, reproduits sur ac6b85a1, avec pauses multi-secondes dans les mêmes sauvegardes de ramassage. |
| Coût mesurable propre aux escaliers ? | Pas de régression systématique distinguée dans les charges pierre/escaliers à 10 bots ; couverture insuffisante pour le pire cas dense. |
| Voisinage calculé inutilement ? | Pas de propagation récursive ou de coût dominant observé ; réexamens de chunks denses encore à profiler. |
| CPU, verrous, allocations ou IO ? | IO synchrones dominantes dans les longues pauses ; reconstruction/sérialisation Anvil et diffusion ont un coût CPU secondaire confirmé. Les verrous mesurés ne dominent pas. Pas d'attribution exhaustive des allocations. |
| 25 moins bon que 50 sur certains indicateurs ? | Médianes et anciennes fenêtres trompeuses face aux rafales/queues ; le débit continu à 50 est inférieur à celui à 25. |
| Ticks proches/supérieurs à une seconde ? | Accumulation de nombreux fsync dans une seule passe ; d'autres ralentissements de flush restent possibles après correction. |
| Influence des autres processus ? | Client Python et infrastructure partagée influencent les résultats ; pas de manque CPU/RAM dominant visible, aucune maîtrise complète de l'hôte physique. |
| Joueurs inactifs également affectés ? | Repos frais beaucoup moins coûteux ; items restants ajoutent du travail, mais les fortes pauses se concentrent sur modifications et ramassages. Repos courts seulement. |

La correction n'est ni une validation globale de 20 TPS ni une suppression de
tous les blocages. Priorités techniques suivantes : profilage ciblé de la
reconstruction Anvil (notamment sections uniformes), coût et cadence des flushs
en préservant le contrat de sauvegarde, puis séparation du coût client et du
fanout réseau. Aucun de ces travaux supplémentaires, aucune porte/trappe,
redstone, simulation parallèle ou autre phase de gameplay n'est commencé ici.
La crash-consistance globale, les longs séjours, les clients Java réels et un
TSan fonctionnel restent hors de la validation apportée par ce lot.

### État final des services et fichiers du lot

Contrôle final le 29 octobre à 21:06 UTC : **25565 actif, PID 140 ; 25587 inactif,
sans PID**, comme au début de cette intervention. Aucun démarrage, arrêt,
redémarrage, changement d'authentification/permissions ou remplacement de binaire
principal n'a été effectué. Les SHA-256 du binaire partagé, des deux configs,
des deux setups CPU et des unités systemd sont identiques avant/après. Les
mondes principaux n'ont été ni utilisés par les tests, ni manipulés. Tous les
processus de tests temporaires sont arrêtés ; 25589/25590 et les ports éphémères
des campagnes sont libérés. Le binaire final reste dans la copie isolée.

Fichiers modifiés : `Makefile`, `include/mc_net.h`, `include/mc_protocol.h`,
`src/net/server.c`, `src/protocol/handlers/play.c`, `tests/test_pvp.c`,
`tests/stress/profile_tick.py`, `tests/stress/profile_fsync.c`,
`tests/stress/test_tick_profile.py`, `tests/stress/test_pickup_save.py`,
`tests/stress/run_block_checks.py`, `tests/stress/README.md` et ce document.
Les scripts Python sont invoqués avec python3 ; aucun changement de permission
de script n'est requis. Aucun fichier personnel ni résultat brut n'entre dans
le commit. Le contrat de persistance de fin de passe est explicitement exposé
ci-dessus pour la revue avant toute éventuelle promotion ultérieure.

## Phase 2 — profilage Anvil et persistance (29–30 octobre 2026)

### Périmètre, état initial et correction d'une hypothèse du diagnostic précédent

HEAD initial : `13a2b1c5b15872ff0168497d37464000a7622150`, sans changement
suivi. `.vscode/`, `src/main.py` et `start-aider.sh` sont préservés, hors commit.
Ce lot ne change ni le tick autoritaire, ni les workers, ni les règles Vanilla,
ni le protocole, ni le format/cadencement des sauvegardes. simulation_workers
reste désactivé. Aucun push et aucune opération Git sur l'hôte distant.

L'audit corrige une ambiguïté du rapport précédent : **reconstruire le NBT
Anvil d'un chunk ne signifie pas reconstruire un fichier de région `.mca`**.
Le chemin actif `world.c::save_chunk_to_store_with_entities` appelle
`mc_chunk_store_write`, qui écrit `WORLD/chunks/c_CX_CZ.bin` : un flux zlib
contenant du NBT de chunk. Le chargement actif appelle uniquement
`mc_chunk_store_read`, sans import automatique des régions `.mca`. Certains
messages historiques `source=anvil` ou `path=.../region/r.*.mca` sont trompeurs ;
les appels et les traces de fichiers effectifs font foi.

| Composant / événement | Comportement constaté avant et après ce lot |
|---|---|
| Pose/casse et certaines mutations de conteneurs | Flush explicite synchrone du chunk sale sur le tick. Deux actions successives peuvent donc produire deux réencodages complets du même chunk. |
| Sauvegarde de fond | Au plus une tentative de chunk sale par tick (`ANVIL_SAVE_ATTEMPTS_PER_TICK=1`). Un chunk propre n'est pas réécrit. |
| Éviction / arrêt normal | Sauvegarde des chunks sales ; les workers sont arrêtés et leurs résultats intégrés suivant le modèle existant. |
| Normalisation au chargement | Un worker peut sauvegarder son chunk privé normalisé avant publication. Il ne modifie pas le monde vivant du tick. |
| Store actif `.bin` | Construction de l'arbre NBT, sérialisation, compression, fopen `wb` avec troncature, fwrite, fflush/fclose vérifiés. Pas de fsync/fdatasync, fichier temporaire ou rename atomique. |
| API autonome `.mca` | Lecture de l'en-tête 8 192 octets, compression du seul chunk fourni, réutilisation/allocation de secteurs, écriture du payload et de l'en-tête. Pas de lecture/reconstruction de tous les payloads de la région. fflush/fclose non vérifiés dans cette API, sans fsync ni verrou de région. Elle n'est pas utilisée par le store actif du serveur. |
| Erreur de lecture `.bin` | Log d'erreur puis génération d'un nouveau chunk. Sa sauvegarde peut remplacer le fichier corrompu ; aucune récupération automatique de l'ancien état. |

Les allocations/copies existantes comprennent l'arbre NBT et ses chaînes,
le tampon NBT sérialisé, le tampon compressBound puis sa réduction par realloc.
Les sections mixtes reconstruisent toujours leur palette dans l'ordre de
première occurrence. Ce lot ne prétend pas mesurer séparément chaque malloc,
chaque copie ou les écritures physiques du journal ext4.

### Correction unique et instrumentation

Les premiers échantillons distants montraient environ 1,6 ms CPU par construction
NBT, et plusieurs ms murales dans fopen/O_TRUNC, avec peu de CPU dans cette
ouverture. La correction retenue est limitée à
`anvil.c::build_section_block_states` : une section `bits_per_block == 0`
fournit directement son unique `uniform_state`, sans relire ses 4 096 cellules.
Les autres représentations gardent exactement le parcours et l'ordre de palette
existants. Le NBT, la compression, les fichiers, les barrières, l'ordre des
mutations et les garanties de persistance restent identiques. Aucune écriture
n'est supprimée ou différée pour améliorer les mesures.

Le nouveau test de test_anvil_roundtrip construit le même contenu dans les
24 sections sous forme uniforme, à palette et directe, puis introduit une
section mixte : les NBT doivent être identiques **octet par octet**. Il complète
les tests existants de DataVersion 4788, lecture 4785, coordonnées négatives,
conteneurs et relecture des états. Le backend compare aussi les `.bin`
compressés des deux versions, puis toutes les cellules après relecture.

`MC_SAVE_TRACE_PATH` active uniquement à la demande les durées murales/CPU par
thread : construction/sérialisation NBT, compression, ouverture/troncature,
fwrite, fflush/fclose et opérations de région. Sans cette variable, la sonde
ne lit aucune horloge ; elle conserve errno dans tous les cas. Les invalidations
de cache ne sont chronométrées que si un cache non vide existe. Le test
concurrent de test_perf_config vérifie 400 lignes complètes, le mode désactivé
et la conservation d'errno. Les phases sont imbriquées et ne s'additionnent pas.
Le temps non couvert inclut notamment libération NBT, préparation des chemins
et coûts de la sonde ; la durée totale les contient.

Le traceur Linux x86_64 `mc_trace_io` ne s'attache qu'à l'enfant qu'il crée.
Il compte les appels fichiers et permet des interruptions reproductibles.
Les temps sous ptrace sont exclus des comparaisons de performance.
Les attentes internes de stdio, du noyau et du stockage ne sont pas toutes
séparables ; la différence mural/CPU établit une attente, pas son origine
physique exacte. Le store actif ne prend pas de mutex monde supplémentaire
pendant l'écriture ; les métriques existantes du tick couvrent ses autres
files et verrous. Aucun nouveau thread ou verrou de simulation n'est ajouté.

### Méthode et lieux de mesure

Une copie isolée distante a été créée avec remote-build-stage.sh :
`/opt/goatcore/phase2a-stage.qFGKq3`. Les deux binaires y ont les mêmes sondes,
options O2, configuration réseau 1 worker/chunks 2 workers et monde initial.
Le backend distant a terminé ses 42 passes chronométrées et 14 passes de comptes
système. La campagne bots a été lancée avant la perte d'accès à 10.0.4.66.

Le 30 octobre, l'utilisateur a explicitement autorisé les tests sur son PC
pendant l'indisponibilité du serveur. Les mesures locales sont distinctes :
Ryzen 7 7840HS, 16 CPU logiques, Linux 7.0, même scénario/configuration entre
référence et candidat. `/tmp` est un tmpfs : le premier backend local isole
ainsi surtout le CPU. La campagne disque utilise ensuite le volume NVMe du
dépôt, sous `tests/stress/results/anvil-technical/disk-stage`, ignoré par Git.
Aucune valeur tmpfs/NVMe locale n'est présentée comme une performance de la LVM
distante. Les en-têtes libcurl manquants localement ont été extraits dans le
répertoire temporaire ; aucune installation système n'a été faite.

Chaque scénario backend sépare préparation et phase active, fait trois
répétitions en alternant l'ordre des versions, puis une passe distincte de
comptage. A : 16 chunks chargés, repos pendant 40 ticks ; B : 200 modifications
d'un chunk ; C : 200 modifications réparties sur 16 chunks de la même région
logique ; D : 200 réparties sur 16 régions logiques ; E : relecture de toutes
les cellules. Le test `.mca` écrit 100 changements d'un seul chunk, avec
1, 16 ou 256 entrées déjà présentes. Tous les changements sont fixes, aucun
résultat n'est normalisé à une charge inférieure.

### Backend : coûts et volumes

Durée active médiane de trois passes, en ms :

| Scénario | Référence tmpfs | Candidat tmpfs | Référence NVMe | Candidat NVMe |
|---|---:|---:|---:|---:|
| B : 200 sauvegardes, 1 chunk | 76,056 | 30,102 | 85,620 | 39,596 |
| C : 200 sauvegardes, 16 chunks / même région logique | 80,864 | 31,363 | 82,914 | 38,249 |
| D : 200 sauvegardes / 16 régions logiques | 75,255 | 30,501 | 97,573 | 51,071 |
| API .mca : 100 changements, 1 entrée | 37,670 | 15,664 | 37,763 | 14,945 |
| API .mca : 100 changements, 16 entrées | 38,644 | 14,752 | 37,997 | 14,796 |
| API .mca : 100 changements, 256 entrées | 36,896 | 14,194 | 37,055 | 14,423 |

A : aucune sauvegarde/écriture dans la phase inactive chargée. B/C/D : exactement
200 openat, 200 write, 200 close par passe active, aucune lecture de chunk,
aucun pwrite/fsync/fdatasync. B écrit **70 200 octets** au noyau dans les deux
versions. Les `.bin` des deux versions sont identiques ; les fichiers de région
incluent un timestamp mural et ne sont pas comparés globalement par hash.

Pour chacune des occupations `.mca` et chaque version : **100 read, 819 200
octets lus ; 200 write, 1 228 800 octets écrits ; 100 ouvertures/fermetures,
400 lseek, zéro fsync/fdatasync/pwrite**. Soit 8 192 octets d'en-tête lus et
12 288 octets écrits par modification pour ces petits chunks (un secteur de
4 096 octets plus l'en-tête). Ce volume reste constant de 1 à 256 entrées :
l'hypothèse d'une reconstruction complète de région est écartée dans ces cas.
Les volumes sont les octets acceptés par les syscalls, sans amplification
physique des pages, du journal ou du contrôleur disque.

Sur NVMe, 600 sauvegardes du scénario B par version, distributions murales
P50/P95/P99 en **µs**, puis CPU moyen en µs :

| Phase | Référence P50/P95/P99 | Candidat P50/P95/P99 | CPU moyen référence → candidat |
|---|---:|---:|---:|
| Construction NBT | 277 / 304 / 357 | 54 / 66 / 78 | 281,09 → 54,92 |
| Sérialisation NBT | 6 / 8 / 11 | 6 / 7 / 11 | 6,45 → 6,39 |
| Compression | 60 / 75 / 97 | 57 / 73 / 89 | 61,72 → 58,92 |
| Ouverture/troncature | 16 / 22 / 33 | 16 / 21 / 34 | 17,17 → 16,56 |
| fwrite | 1 / 2 / 2 | 1 / 2 / 2 | 1,30 → 1,28 |
| fflush | 5 / 6 / 9 | 5 / 6 / 9 | 4,89 → 4,68 |
| fclose | 20 / 25 / 42 | 19 / 22 / 34 | 19,19 → 18,51 |
| Total | 418 / 479 / 622 | 191 / 228 / 283 | 426,29 → 195,23 |

Le CPU de construction baisse de 80,5 % dans cette fixture synthétique à deux
sections mixtes et 22 sections d'air uniformes. Le maximum total observé est
5 759 → 549 µs ; l'ouverture maximale 5 342 → 332 µs n'est **pas** attribuée à
la correction CPU : le timing du stockage varie. Une section logiquement
uniforme déjà représentée par une palette reste volontairement sur l'ancien
parcours. Le gain dépend donc de la représentation réelle des chunks.
Les phases NBT détaillées concernent le store actif ; l'API autonome `.mca`
construit aussi son NBT en amont du span region_total, sans span NBT distinct
sur ce second point d'entrée. Sa durée de scénario inclut ce travail.

### Interruptions : défaut préexistant confirmé, contrat inchangé

Deux matrices de douze instances locales jetables (tmpfs puis NVMe), six
interruptions pour chacune des deux versions, avec un ancien bloc de pierre
et une bûche témoin adjacente dans le
chunk négatif (-32,-32). Le traceur tue seulement le processus enfant ; les
fichiers originaux sont conservés avant toute récupération. Le rechargement
est tenté sur une copie, par les mêmes API world/worker que le serveur.

| Point d'interruption | Résultat avant ET après correction |
|---|---|
| Avant openat O_TRUNC | Ancien fichier de 332 octets inchangé, pierre et témoin conservés. |
| Après O_TRUNC | Fichier vide, lecture -1, régénération : pierre et témoin perdus. |
| Avant write | Même résultat : fichier vide et ancien chunk perdu. |
| Écriture partielle injectée, puis SIGKILL | 162 octets, flux zlib illisible, régénération et perte des deux blocs. |
| Après write complet | Nouveau fichier de 325 octets lisible, nouvelle bûche et témoin conservés. |
| Après close | Même nouveau contenu lisible. |

Il s'agit d'un SIGKILL avec cache noyau intact, **pas d'une panne de courant**.
Même après fflush/fclose ou un ACK réseau, l'absence de fsync ne prouve pas la
durabilité électrique. Le store actuel n'assure pas l'atomicité du remplacement
et peut détruire un ancien chunk pendant une nouvelle sauvegarde. L'optimisation
CPU ne corrige ni ne masque ce défaut. La résistance générale aux crashes n'est
**pas validée**. L'API `.mca` autonome n'apporte pas davantage de garantie globale
et ses interruptions ne sont pas couvertes par cette matrice `.bin`.

Les sauvegardes joueurs conservent leur propre protocole atomique et leurs
fsync, dont le regroupement de fin de passe de ramassage du lot précédent.
Il n'existe pas de transaction globale inventaire/monde/objets au sol ; les
entités items en mémoire ne sont pas sérialisées par le store de chunks. Une
fenêtre de crash peut donc perdre des objets au sol indépendamment du défaut
`.bin`. Aucun mécanisme de transaction ou nouveau format n'est introduit ici.

### Campagne bots locale NVMe : résultat continu à travail de blocs fixé

Référence = 13a2b1c5 avec les sondes communes ; candidat = même source plus le
raccourci des sections uniformes. Trois répétitions par niveau, ordre alterné,
100 poses ET 100 destructions confirmées par bot, même monde initial copié et
mêmes paramètres de workers/distances. Préparation, échauffement de 5 s, repos
de 5 s, charge entière, puis repos de 5 s sont séparés. Les 441 chunks annoncés
à chaque client ont été reçus avant la charge. Les deux versions exécutent
chacune **25 800 poses et 25 800 destructions**, zéro action manquante, erreur
ou déconnexion inattendue. Toutes les propositions, entrées de traitement
serveur et confirmations sont comptées ; il ne s'agit pas de paquets seulement
envoyés. Les étapes enqueue ont une résolution de 1 ms et leur décompte aux
bords de phase utilise le début de traitement non arrondi.

Distributions regroupant tous les ticks planifiés chevauchant les trois phases
actives, sans fenêtre sélectionnée ni écrêtage. TPS = somme des débuts de ticks
/ somme des durées réelles ; les valeurs légèrement supérieures à 20 reflètent
les bords de ces phases courtes et les rattrapages, pas une nouvelle fréquence.

| Bots | Version | Poses / casses | Durée totale active s | TPS observés | MSPT P50 / P95 / P99 | Ticks >50 ms | Maximum ms |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | Référence | 300 / 300 | 14,474 | 19,898 | 0,604 / 0,789 / 1,094 | 0 | 2,012 |
| 1 | Candidat | 300 / 300 | 14,636 | 19,882 | 0,366 / 0,611 / 3,302 | 0 | 6,121 |
| 10 | Référence | 3 000 / 3 000 | 25,663 | 19,951 | 2,617 / 3,514 / 12,433 | 0 | 17,076 |
| 10 | Candidat | 3 000 / 3 000 | 24,070 | 19,983 | 2,425 / 3,141 / 3,821 | 0 | 8,191 |
| 25 | Référence | 7 500 / 7 500 | 12,640 | 20,016 | 8,749 / 12,444 / 14,736 | 0 | 16,713 |
| 25 | Candidat | 7 500 / 7 500 | 12,124 | 20,126 | 8,396 / 11,775 / 12,714 | 0 | 14,576 |
| 50 | Référence | 15 000 / 15 000 | 57,586 | 20,005 | 17,766 / 39,227 / 49,598 | 9 | 76,247 |
| 50 | Candidat | 15 000 / 15 000 | 53,750 | 20,018 | 17,896 / 37,069 / 47,488 | 8 | 64,722 |

Ces MSPT ne cachent pas les passes rapides entre ticks : elles sont conservées
séparément dans chaque CSV. À 50 bots, coût planifié + passes rapides associé
à l'intervalle : P50/P95/P99 **37,061/67,365/80,489 ms** avant et
**35,500/63,068/71,515 ms** après, maximum **103,647/91,533 ms**, respectivement
327 et 239 intervalles dépassant 50 ms. Les coûts imbriqués du tick ne sont
pas additionnés une seconde fois. Débit proche de 20 sur ces courtes charges
locales ne signifie ni absence de latence, ni stabilité longue, ni 20 TPS sur
10.0.4.66, dont les conditions IO et CPU diffèrent.

| Bots | CPU processus % réf. → cand. | CPU client Python % réf. → cand. | Temps cumulé chunk_total s réf. → cand. | Sauvegardes réf. → cand. | fsync mesurés réf. → cand. |
|---:|---:|---:|---:|---:|---:|
| 1 | 2,6 → 1,7 | 2,0 → 1,8 | 0,543 → 0,447 | 458 → 451 | 0 → 0 |
| 10 | 11,2 → 9,0 | 7,1 → 8,0 | 1,773 → 1,055 | 3 464 → 3 432 | 672 → 630 |
| 25 | 75,1 → 62,6 | 64,6 → 65,7 | 3,634 → 1,995 | 7 717 → 7 705 | 482 → 462 |
| 50 | 114,6 → 108,9 | 99,4 → 99,5 | 7,378 → 4,207 | 15 874 → 15 853 | 800 → 778 |

CPU : moyennes par passe, 100 % représentant un cœur logique. RSS active à
50 bots : 41 268–42 800 KiB avant, 40 920–42 308 après ; pas de croissance
anormale démontrée dans ces essais courts. Profondeurs au point de mesure du tick : 1/10/18/50 avant,
1/7/15/50 après ; pics de file 2/12/27/54 avant, 2/14/30/54 après,
sans saturation de la limite 8 192. L'analyse des ressources
conserve aussi les compteurs IO, schedstat, wchan et pression hôte.

À 50 bots, chacun des trois essais propose, reçoit et confirme exactement
5 000 poses et 5 000 casses. Débit groupé par direction : 260,5 → 279,1 actions/s.
Les latences client P95 par passe sont 79,23/74,22/74,89 ms avant et
78,41/69,19/72,65 ms après. Attente de file des poses P99 :
48,259/44,238/42,379 ms avant, 46,091/42,130/44,396 après. Le client Python
proche de 100 % limite lui-même le rythme ; ce scénario fermé par confirmations
n'établit pas la capacité maximale d'admission indépendante du client. Aucun
chantier réseau n'a été entrepris pour changer ces chiffres.

Les effets secondaires temporels restent visibles : les quantités ramassées
par le premier observateur à 50 bots sont 673/407/409 avant, 303/560/482 après,
avec des nombres de lots et de fsync différents. Le calendrier des sauvegardes
sales produit également de petites variations de comptes et d'octets :
5 962 940 → 5 945 159 octets compressés sauvegardés à 50 bots. Le travail de
**blocs** est égal, mais le travail total de ramassage/synchronisation disque
pendant la phase n'est pas strictement identique. Le raccourcissement de charge
57,586 → 53,750 s n'est donc pas attribué entièrement au codec. La preuve
contrôlée du gain CPU est le backend à **200 sauvegardes et octets identiques**,
complétée par les coûts par appel en serveur. Aucune suppression de fsync ou
de validation n'est responsable du gain annoncé.

### Attribution et limites du profilage

À 50 bots sur les phases actives : construction NBT CPU **5,180 → 1,941 s**
pour 15 874 → 15 853 appels, soit environ **326 → 122 µs/appel**. Le total
sauvegarde CPU passe de 7,331 à 4,056 s ; compression et volume restent
comparables. L'optimisation du parcours uniforme a donc un effet mesurable sur
le serveur réel, moindre que dans la fixture synthétique. Le fanout et le coût
Python n'ont pas été isolés par une modification de code et ne sont pas déclarés
résolus.

Des attentes disque demeurent : ouverture/troncature maximale **18,155 ms avant,
29,126 ms après** dans ces essais à 50 bots. Des échantillons wchan montrent
`jbd2_log_wait_commit` et `folio_wait_bit_common`. Cela concorde avec les attentes
murales observées, sans identifier le contrôleur physique ni toutes les causes
hôte. Les temps de verrous mesurés ne dominent pas ces pauses ; les attentes
stdio/noyau non détaillées et les autres processus restent des limites.

Le plus long tick planifié avant correction, 76,247 ms, contient 38,963 ms de
tâches et 35,948 ms d'items ; seuls environ 4,225 ms de spans chunk_total le
chevauchent. Après correction, le maximum 64,722 ms contient 41,715 ms de tâches
et 21,886 ms d'items, avec environ 3,541 ms de sauvegardes de chunks chevauchantes.
Les sauvegardes joueurs des ramassages restent indépendantes des spans chunks.
Ces mesures réfutent l'attribution de **toutes** les pauses à Anvil.

Le défaut de troncature est plus grave pour l'intégrité que le coût CPU. La
prochaine intervention recommandée est un protocole de remplacement atomique
et durable des fichiers de chunks, avec récupération explicitement définie et
injections de crash/erreur, avant toute promotion du binaire. Il faudra mesurer
le coût des barrières nécessaires ; ce lot ne les retire ni ne les ajoute
silencieusement. La phase 2 reste ouverte.

### Non-régression et compatibilité effectivement vérifiées

`make -B precommit-check` réussit sur la copie locale NVMe : **23 suites C**,
smoke, réseau multi-epoll 1/2/4 workers et affinité sur un CPU, arrêt sous charge,
login offline/UUID/doublons/reconnexion/refus online, puis tests Python du
générateur, des bots et des bornes du profiler. Le registre des blocs et les
IDs de protocole sont régénérés et comparés aux rapports officiels présents.
Les sources brutes recettes/loot/composants absentes de cette copie limitent
les autres vérifications generated-check à la présence des fichiers ; aucune
régénération complète de tous les assets n'est revendiquée. L'export n'a pas de
Git ; l'hygiène Git est vérifiée dans le dépôt local. Le binaire reconstruit
par ce contrôle est identique par cmp au candidat des 24 passes.

| Suite | Binaire normal | ASan + UBSan | Référence / portée |
|---|---|---|---|
| Escaliers, formes/voisins, deux observateurs et lectures autoritaires | 192 observations, puis 8 états relus après chacun de 3 redémarrages | 192 observations + 8 après redémarrage | Nouvelle exécution officielle 26.1.1 et fixture versionnée : mêmes 192 lignes et mêmes 8 états. Vanilla relu après 3 redémarrages. |
| Blocs pleins, bûches, dalles/fusion, refus, collisions, inventaires | 18 scénarios, 4 états et slot 36 après chacun de 3 redémarrages | Même suite et relecture | Identique aux 18 scénarios de la fixture Vanilla existante ; shulker BlockEvent 775 conservé. Pas de nouvelle exécution Vanilla de cette seconde suite. |
| Ramassages | 100 drops et 100 objets ramassés vus par deux joueurs ; inventaire à la reconnexion puis après chacun de 3 redémarrages | Même cas 100, reconnexion et redémarrage | Quantités exactes, pas de duplication/perte dans ce scénario terminé normalement. Ne prouve pas une transaction globale en cas de crash. |
| Sérialisation et coordonnées | Backend : relecture de toutes les cellules B/C/D et `.mca` à 1/16/256 entrées ; fichiers `.bin` identiques entre versions | test_anvil_roundtrip, test_world_consistency et codec réseau | DataVersion 4788, lecture 4785, propriétés, coordonnées négatives et frontières, régions logiques multiples. |

ASan+UBSan locaux : O1/g, `-fsanitize=address,undefined`,
`-fno-omit-frame-pointer -fno-pie`, lien `-no-pie`,
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Dix suites C passent :
test_pvp, test_stairs, test_world_consistency, test_anvil_roundtrip,
test_player_nbt, test_chunk_network_codec, test_paletted_container,
test_perf_config, test_block_registry et test_nbt. Les trois suites réseau du
tableau passent avec arrêt normal et relecture. Aucun diagnostic sanitizer.
Les logs de fichiers corrompus et d'échecs disque des injections C sont attendus
et vérifiés ; les corruptions du scénario F sont des défauts connus reproduits,
pas une réussite de sûreté de persistance.

TSan : contrairement au runtime LXC distant précédemment arrêté avec
`unexpected memory mapping`, le runtime du **PC local** fonctionne. La sonde
minimale, test_perf_config et test_world_consistency non-PIE terminent avec
**code 0**, sans diagnostic. Ce résultat est limité à ces tests et cet hôte :
aucune validation TSan du serveur réseau complet ni du runtime distant actuel.

Les premiers pilotes distants avec une sonde de cache trop bavarde ont été
arrêtés et exclus des conclusions : près de 500 Mo/passe à l'initialisation.
La sonde a été corrigée dans les deux versions avant les campagnes retenues.
Le pilote bots local sur tmpfs a été arrêté pour déplacer la campagne disque
sur NVMe ; il n'entre pas dans le tableau des 24 passes. Une première compilation
locale a échoué faute de curl/curl.h, puis a réussi avec le SDK extrait. Une
lecture de fixture de placement utilisait initialement `cases` au lieu de
`scenarios` ; l'analyse corrigée compare bien les 18 lignes. Aucun de ces essais
de harnais n'est présenté comme une mesure finale ou une régression du serveur.

### Binaires, services, artefacts et fichiers du lot

Empreintes SHA-256 locales :

- Référence 13a2b1c5 + sondes communes : `0b3a961b97bd3254c728c02e8391ea0bd89fa94447d2c4b0a4a4c16f4d157e1e`.
- **Binaire normal final testé** : `c9eafc0e5f7616f584b35f8010be4a898dbe784c34bceea387051c72461d14c1`.
- ASan+UBSan : `3c0c2e167de6a64d6f4f92f8668902b38bca7b34a3e3e79eade25155b1cecf36`.
- JAR interne officiel 26.1.1 : `3647564632a2f589be2a87bf544f628e5f67cb3780c86f7fa972ec36892aa081`, version.json protocole 775 / DataVersion 4788. Le vieux JAR local 26.1 a été écarté. Le bundler 26.1.1 provient du manifeste Mojang, SHA-1 vérifié `49c8195703ad0ba4f0a4efbccfd85a4a8ca57431` ; Java local 25.0.4.1.

Aucun remplacement de binaire principal, changement de service, de monde,
d'authentification ou de permissions n'a été effectué sur 10.0.4.66.
**Dernière observation distante vérifiée, 29 octobre 22:34 UTC : 25565 actif,
PID 140 ; 25587 inactif, PID 0**, résultat systemd success. Binaire partagé
`f840ed3f535d80d8b87293b1ada6fdf88cacd6c681740f1dcfa3b06bd1dc423c`.
Les empreintes des configs, setups et unités sont conservées dans
services-before.txt. Le 30 octobre, les connexions SSH expirent ; le contrôle
final des deux services, la récupération de la campagne distante et la
vérification du nettoyage des processus temporaires distants restent
**impossibles tant que l'accès n'est pas rétabli**. Leur état courant n'est pas
inventé. 25587 n'a pas été redémarré, 25565 n'a pas été manipulé.

Tous les processus Minecraft temporaires **locaux** sont arrêtés et leurs ports
libérés, y compris 25589/25590/25591. Le binaire final reste dans la copie locale
isolée ; rien n'est promu sur les services. La validation distante finale reste
ouverte, et la persistance ne doit pas être qualifiée de crash-safe.

Artefacts bruts locaux, ignorés par Git, sous
`tests/stress/results/anvil-technical/` :

- `disk-stage/tests/stress/results/anvil-backend-nvme` : 42 passes, 14 comptes système, JSON de commandes, phases CPU/murales, syscalls, relectures et fichiers.
- `disk-stage/tests/stress/results/anvil-matrix-nvme` : les 24 passes complètes, ticks.csv, saves.csv, actions.json (instants individuels), resources.json, fsync.csv, snapshots de processus, monde initial/manifeste et résumés.
- `disk-stage/tests/stress/results/anvil-crashes-nvme` : 12 interruptions, fichiers interrompus préservés, copies récupérées et comptes système ; `stairs-normal`, `stairs-vanilla`, `placement-normal`, `pickup-normal` : captures et relectures.
- `tmpfs-results` : backend CPU, première matrice d'interruptions, suites ASan/UBSan et pilote tmpfs exclu des mesures disque. Les artefacts initialement en RAM ont été copiés sur le NVMe.
- `local-build-logs`, `disk-stage/precommit.log`, `binaries.sha256`, `local-host.txt`, `analysis-nvme.txt` et `analyze.py` : builds, sanitizers, méthode de regroupement et tentatives de contrôle distant. La copie distante qFGKq3 conserve ses propres résultats, non récupérés depuis la panne réseau.

Fichiers du commit : `.gitignore`, `Makefile`, `include/mc_util.h`,
`src/util/mc_util.c`, `src/net/server.c`, `src/world/anvil.c`,
`src/world/chunk.c`, `src/world/chunk_store.c`, `tests/test_anvil_roundtrip.c`,
`tests/test_perf_config.c`, `tests/stress/profile_tick.py`,
`tests/stress/profile_persistence.py`, `tests/stress/run_block_checks.py`,
`tests/stress/README.md`, `tools/mc_save_bench.c`, `tools/mc_trace_io.c`,
`tools/check_repo_hygiene.sh` et ce document. Les Python sont invoqués par
python3 ; aucun changement de permission n'est nécessaire. Un seul commit
local, sans push : `Profile and optimize Anvil persistence bottlenecks`.

## Phase 2 — sauvegardes atomiques et interruptions (30 octobre 2026)

### État initial et périmètre de la correction

HEAD initial : `8fd89fccc69a3ab0fde783d69c43ca0d6d068e89`, état suivi propre.
`.vscode/`, `src/main.py` et `start-aider.sh` restent personnels, non suivis.
L'exception de maintenance autorise explicitement les essais locaux de ce lot ;
les règles permanentes et les trois skills ne sont pas modifiés. Aucun accès
SSH, démarrage, reconfiguration ou déploiement sur 10.0.4.66 n'est tenté.

Le risque avait deux chemins :

- Le store **actif** `chunk_store.c` ouvrait `chunks/c_CX_CZ.bin` en `wb`, donc détruisait l'ancien flux zlib dès l'ouverture, avant toute nouvelle écriture. Il contient du NBT de chunk, pas un fichier de région.
- L'API autonome `anvil.c` ouvrait `.mca` en `r+b`, réécrivait en place les secteurs du chunk puis l'en-tête. Elle réessayait en `w+b` après n'importe quelle erreur d'ouverture. Ses fflush/fclose finaux n'étaient pas vérifiés. Elle n'est toujours pas le store utilisé par world.c.

La correction protège **les deux**, sans migrer les mondes ni modifier NBT,
DataVersion, registres, codec réseau, placement, voisins ou concurrence de la
simulation. Le raccourci des sections uniformes du lot précédent est conservé.

La reproduction locale avant correction utilise le backend du commit 8fd89fcc,
une région `r.-1.-1.mca` de trois chunks aux coordonnées distinctes et le traceur
mc_trace_io. Le premier write est limité à 32 octets, puis le processus est tué
avant toute reprise par libc. Avant : les trois chunks se lisent. Après : le
chunk modifié renvoie -1, les deux autres restent lisibles. L'image initiale,
l'image interrompue, les états et les syscalls sont conservés sous
`tests/stress/results/atomic-persistence/stage/tests/stress/results/baseline`.
Le binaire de référence serveur est exactement celui du lot précédent, pas une
réimplémentation approchée du défaut.

### Protocole d'écriture et acquittement

`src/util/atomic_file.c` fournit la séquence partagée :

1. Ouvrir et conserver un FD sur le répertoire parent. Vérifier le type du fichier existant sans suivre un lien symbolique final.
2. Créer un temporaire unique `NOM.tmp.PID.SEQUENCE` dans ce répertoire avec openat `O_CREAT|O_EXCL|O_CLOEXEC`. Les collisions ne sont jamais tronquées ou supprimées. Le nom et le FD appartiennent uniquement à cette transaction.
3. Préparer intégralement le nouveau contenu. write/pwrite/pread bouclent sur les transferts courts et EINTR ; un transfert nul prématuré devient EIO. Les modes Unix ordinaires et uid/gid existants sont conservés si l'opération est permise ; un échec préserve l'ancien fichier. Les nouveaux fichiers sont créés en 0600.
4. Effectuer fsync sur le temporaire, puis vérifier sa fermeture.
5. Effectuer renameat entre noms relatifs au **même FD de répertoire**, ce qui exclut un déplacement inter-filesystems et publie une seule image complète.
6. Effectuer fsync sur le répertoire, puis vérifier sa fermeture. Le succès 0 n'est retourné qu'après toutes ces étapes.

Le lien entre fsync du fichier et persistance de l'entrée de répertoire suit
[fsync(2)](https://man7.org/linux/man-pages/man2/fsync.2.html). Le remplacement
atomique et la stabilité des anciens FD ouverts suivent
[rename(2)](https://man7.org/linux/man-pages/man2/rename.2.html). close n'est pas
réessayé après EINTR sur Linux, pour ne pas fermer un FD déjà réutilisé par un
autre thread ; voir [close(2)](https://man7.org/linux/man-pages/man2/close.2.html).

| Résultat | Signification et traitement |
|---|---|
| 0 | fsync du fichier, fermeture, remplacement et fsync/fermeture du répertoire terminés. Le monde peut retirer le marquage sale. |
| -1 | Échec avant publication dans le contrat des systèmes de fichiers locaux utilisés. Le fichier cible n'est ni tronqué ni restauré. Le temporaire propre à l'opération est supprimé si possible ; un échec de nettoyage est journalisé. |
| `MC_SAVE_DURABILITY_UNCERTAIN` (-2) | Remplacement visible, mais confirmation finale en échec. Le log nomme explicitement l'incertitude ; aucun rollback et aucune suppression du nouveau fichier. Le monde reste sale. |

Les erreurs EIO, ENOSPC et EDQUOT ne sont pas transformées en succès, y compris
lors d'un fsync ou close. EINTR est réessayé pour les opérations où cela est sûr.
L'errno initial est conservé à travers les nettoyages. Une erreur secondaire de
nettoyage ne donne jamais un acquittement positif. Après un échec ou une
incertitude, le prochain flush repart de **l'état courant du tick**, pas d'un
ancien tampon gardé en attente.

La création des répertoires du monde synchronise leur hiérarchie avant le
démarrage des workers. Le store normal utilise alors ces répertoires préparés.
Les utilisateurs directs du store doivent préparer `world/chunks` avec
mc_persistence_mkdirs et réessayer cette préparation après un échec de création
ou de synchronisation. Les répertoires de stockage fournis par des acteurs
externes doivent eux-mêmes être durablement établis. Ce contrat ne couvre pas
la destruction ou le déplacement concurrent du répertoire par un autre outil.

### Régions, écrivains, lecteurs et versions

L'API `.mca` prend un flock exclusif sur un **fichier `.mca.lock` persistant**
avant de lire la région et le conserve jusqu'à la fin de la publication. Il ne
faut jamais supprimer ce fichier de verrou pendant que des écrivains peuvent
fonctionner. Chaque appel ouvre son propre FD de verrou : les utilisateurs du
même processus sont également sérialisés, conformément à
[flock(2)](https://man7.org/linux/man-pages/man2/flock.2.html).

Le writer valide taille, en-tête, bornes et chevauchements de secteurs, longueurs
et marqueurs de compression. Une erreur de lecture ou un format non pris en
charge interrompt la sauvegarde. Seul ENOENT autorise une création de région.
La région existante est copiée par blocs de 64 KiB dans le temporaire, puis le
secteur du chunk et ses seules entrées offset/timestamp sont modifiés. Les
payloads et tags NBT des **autres** chunks restent identiques octet par octet.
L'algorithme de secteurs existant est conservé ; son erreur d'allocation ne
peut plus être interprétée comme une autorisation d'écraser le secteur 2.
Aucun compactage ou changement de format n'est introduit.

Les lecteurs conservent soit leur ancien inode complet, soit ouvrent le nouveau
fichier complet. Les nouvelles régions et les agrandissements de chunks passent
par le même protocole. Deux writers de chunks distincts d'une même région ne
peuvent plus publier chacun une copie qui oublie la modification de l'autre.
Les tests synchronisent précisément un deuxième writer pendant la préparation
du premier, puis vérifient les deux modifications et l'ancien FD ; deux
processus distincts sont également exercés.

La propriété mémoire reste celle du projet : tick autoritaire pour les chunks
vivants, worker pour son chunk privé avant publication. Les sauvegardes restent
synchrones. Il n'existe donc pas de nouvelle file de snapshots dont un ancien
résultat pourrait arriver après un nouveau. Le test du monde vérifie qu'un
flush échoué/incertain garde le chunk sale, puis qu'une modification de revision
plus récente est bien sauvegardée au retry. L'API brute de région ne contient
pas de version logique permettant de reconnaître un **snapshot périmé fourni
volontairement par un appelant externe** : les appelants doivent posséder des
snapshots immuables et ordonner leurs mises à jour. Le verrou est coopératif ;
partager un monde avec un writer qui modifie les fichiers en place sans ce
verrou reste hors contrat.

### Références externes et protection à la lecture

La lecture `.mcc` n'était pas implémentée. Une région contenant un marqueur
externe 0x80 est désormais refusée **entièrement**, même si l'édition vise un
autre chunk. Aucun `.mcc` n'est créé, écrasé, déplacé ou supprimé. Les codecs
hors 1/2/3 et les nouveaux chunks dépassant les 255 secteurs sont aussi refusés.
Le test conserve les images `.mca` et les octets du fichier externe lors du
refus. Il ne prétend pas valider un import Vanilla externe complet. Une
transaction `.mca` + `.mcc` demandera un chantier distinct.

Une seconde dépendance indispensable concernait le chargeur actif : il
régénérait un chunk après toute erreur, puis pouvait écraser un **fichier valide
qui avait seulement subi un EIO de lecture transitoire**. Il ne génère désormais
que pour une absence. Après une erreur de lecture/décodage, il conserve le
fichier, détruit seulement son résultat privé incomplet, journalise l'erreur et
laisse le slot indisponible jusqu'au redémarrage après diagnostic/réparation.
Aucun état incomplet n'est remis au tick. Il n'y a pas de récupération ou retry
automatique dans ce lot. La suite historique qui attendait une régénération
après corruption a été modifiée pour exiger la conservation des quatre octets
endommagés. Un test distinct injecte EIO sur un fichier valide, vérifie son
intégrité après arrêt et le recharge correctement au redémarrage sans injection.

### Matrice d'interruptions et d'erreurs

Les wrappers de syscalls sont liés uniquement au nouveau test C ; le serveur
n'a aucun hook d'injection. Douze interruptions C (six par format) et six
interruptions supplémentaires d'une **vraie instance GoatCore** exercent les
mêmes étapes. Les fichiers interrompus sont conservés avant relecture.

| Étape | .bin actif | Région .mca de trois chunks | Relecture après SIGKILL |
|---|---|---|---|
| A : avant création du temporaire | C + serveur | C | Ancien fichier identique, tous les anciens états conservés. |
| B : pendant une écriture partielle du temporaire | C + serveur | C | Ancien fichier identique ; temporaire incomplet non publié. |
| C : après écriture, avant fsync du temporaire | C + serveur | C | Ancien fichier identique ; aucune promotion au redémarrage. |
| D : après fsync et fermeture, avant rename | C + serveur | C | Ancien fichier identique ; temporaire complet ignoré. |
| E : après rename, avant fsync du répertoire | C + serveur | C | Nouveau fichier complet dans ces essais ; états/NBT lisibles et voisins préservés. La durabilité du nom n'est pas encore acquittée. |
| F : après fsync du répertoire | C + serveur | C | Nouveau fichier complet relu ; les barrières du protocole ont réussi. |

G comprend 80 cas déterministes de transferts courts/nuls, EINTR, EIO, ENOSPC et
EDQUOT selon l'opération : création du temporaire, write/pwrite/pread, fsync
fichier/répertoire, rename, close fichier/répertoire, fstat et flock. S'y
ajoutent deux échecs calloc, dont le bitmap d'allocation de secteurs, et cinq
régions aux offsets/longueurs/chevauchements/codec ou marqueur externe invalides.
Les erreurs pré-publication conservent l'image précédente. Les erreurs après
publication retournent -2 et conservent le nouveau contenu cohérent. Les
écritures courtes et EINTR récupérables terminent normalement ; close(EINTR)
échoue avec le FD consommé. Les nombres de temporaires sont inchangés après
les erreurs ordinaires. Les opérations ne suppriment jamais un temporaire créé
par un autre écrivain.

Les tests couvrent aussi : région neuve, un ou plusieurs chunks, modifications
répétées, agrandissement de payload, champs NBT opaques, anciennes lectures par
FD, modes Unix, coordonnées négatives, frontière entre régions -1 et 0,
concurrence par threads/processus, dirty/revision au retry et échec de lecture.
Les temporaires laissés par SIGKILL sont délibérément ignorés et conservés,
jamais promus ni effacés automatiquement ; un nettoyage hors ligne devra
s'assurer qu'aucun writer ne les utilise.

La preuve porte sur les **interruptions de processus et erreurs injectées**.
SIGKILL conserve le cache noyau ; il ne simule pas une coupure électrique. La
durabilité après retour 0 repose sur les garanties du système de fichiers local
et sur un matériel qui respecte les commandes de synchronisation. Une panne
du contrôleur, un cache RAID non protégé ou qui ment, la perte du filesystem ou
la corruption physique ne sont pas exclus par ces tests. Aucun arrêt brutal du
PC n'est effectué. Les ACL/xattrs et métadonnées spéciales hors modes Unix et
uid/gid ne font pas partie de la validation apportée.

L'acquittement concerne un fichier. Les fichiers joueurs gardent leur protocole
existant ; aucune transaction globale joueur/inventaire/monde/objets au sol
n'est introduite et les items au sol restent non persistés. Les garanties du
présent lot ne doivent pas être étendues à ces ensembles multi-fichiers.

### Mesures avant/après sur le binaire final

Matériel **local uniquement** : Ryzen 7 7840HS, NVMe du volume du dépôt,
Linux 7.0, C11/O2, même configuration réseau 1 worker/chunks 2 workers,
simulation_workers inchangé/désactivé. Référence 8fd89fcc et candidat sont
alternés sur trois répétitions. Les mondes initiaux et le travail sont fixes ;
préparation, échauffement et phases actives sont séparés. Les temps ptrace sont
exclus des performances ; ils servent seulement aux comptes système et crashes.
Aucune comparaison directe au Xeon/HDD RAID du datacenter n'est faite.

Backend, durées actives médianes de trois passes, ms :

| Scénario | Référence | Candidat | Travail identique |
|---|---:|---:|---|
| B : un chunk | 40,951 | 267,553 | 200 sauvegardes, 70 200 octets compressés |
| C : 16 chunks, même région logique | 39,178 | 260,173 | 200 sauvegardes, 70 639 octets |
| D : 16 régions logiques | 39,637 | 270,086 | 200 sauvegardes, 70 437 octets |
| API .mca : 1 chunk présent | 15,425 | 135,802 | 100 modifications du même chunk |
| API .mca : 16 chunks présents | 14,955 | 143,204 | 100 modifications du même chunk |
| API .mca : 256 chunks présents | 14,513 | 223,305 | 100 modifications du même chunk |

A : aucun chunk sauvegardé et aucune écriture pendant le repos mesuré après
chargement ; les synchronisations de préparation sont hors de cette période.
Les `.bin` sont identiques octet par octet entre les deux versions. Les états
de tous les blocs des scénarios B/C/D et des régions sont relus et comparés.

B/C/D : toujours 200 write, mais désormais **400 fsync, 200 renameat et 400
openat/close**, contre zéro fsync/rename et 200 ouvertures/fermetures avant.
Chaque sauvegarde acquittée ajoute le fsync du fichier et celui du répertoire ;
les synchronisations d'initialisation des répertoires sont hors charge.

Pour 100 modifications `.mca`, lecture/écriture au noyau :

| Entrées présentes | Lecture avant | Lecture après | Écriture avant | Écriture après |
|---:|---:|---:|---:|---:|
| 1 | 819 200 | 2 048 500 | 1 228 800 | 2 457 600 |
| 16 | 819 200 | 8 200 000 | 1 228 800 | 8 601 600 |
| 256 | 819 200 | 106 624 000 | 1 228 800 | 106 905 600 |

Le candidat effectue 200 fsync et 100 renameat par passe `.mca`. Les lectures
incluent validation d'en-tête/enregistrements et copie ; les écritures incluent
copie complète, payload et nouvel en-tête. Les volumes sont les octets acceptés
par read/pread/write/pwrite, pas les écritures physiques amplifiées par ext4 ou
le contrôleur. La copie complète est un **nouveau coût de sécurité**, pas une
optimisation ; elle croît avec la taille du fichier, y compris ses secteurs
inutilisés. La mesure sur HDD RAID est indispensable avant un déploiement.

Scénario B, 600 sauvegardes par version ; temps muraux P50/P95/P99, en µs :

| Phase | Référence | Candidat | CPU moyen µs réf. → cand. |
|---|---:|---:|---:|
| Construction NBT | 54 / 62 / 72 | 54 / 69 / 79 | 54,5 → 57,7 |
| Compression | 57 / 70 / 88 | 63 / 89 / 104 | 58,8 → 66,7 |
| Total sauvegarde | 191 / 219 / 425 | 1 358 / 1 689 / 1 747 | 194,6 → 251,8 |
| fsync du temporaire | absent | 602 / 760 / 816 | 35,9 candidat |
| renameat | absent | 22 / 34 / 41 | 23,6 candidat |
| fsync du répertoire | absent | 552 / 709 / 752 | 5,3 candidat |

La construction NBT optimisée précédemment reste présente. Le maximum total
backend est 5 989 µs avant, 1 825 après dans ces échantillons : ce résultat
ponctuel ne signifie pas une amélioration des pauses, car les longues attentes
varient avec le stockage. La médiane de sauvegarde augmente nettement.

### Charge continue avec bots

100 poses et 100 casses confirmées par bot, trois répétitions à 1/10/25/50,
même monde copié, 5 s d'échauffement et 5 s de repos de chaque côté. Toutes les
441 réceptions de chunks par client sont terminées avant la charge. Chaque
version exécute **25 800 poses et 25 800 destructions** au total. Propositions,
entrées de traitement et confirmations concordent ; zéro opération manquante,
erreur ou déconnexion inattendue. Tous les ticks chevauchant la charge active
sont conservés, sans sélection d'une fenêtre de 100 ticks ni clipping des
percentiles. TPS = ticks réellement commencés / durée réelle regroupée.

| Bots | Version | Durée active totale s | TPS | MSPT P50 / P95 / P99 ms | Ticks >50 ms | Maximum ms |
|---:|---|---:|---:|---:|---:|---:|
| 1 | Référence | 12,964 | 19,978 | 0,329 / 0,516 / 0,539 | 0 | 0,666 |
| 1 | Candidat | 14,568 | 20,044 | 0,096 / 2,517 / 34,847 | 0 | 35,901 |
| 10 | Référence | 24,135 | 19,971 | 2,450 / 3,250 / 3,931 | 0 | 7,115 |
| 10 | Candidat | 27,616 | 19,989 | 3,958 / 5,321 / 6,407 | 0 | 35,229 |
| 25 | Référence | 14,698 | 20,002 | 8,486 / 12,448 / 15,404 | 0 | 18,504 |
| 25 | Candidat | 20,741 | 20,009 | 9,931 / 23,672 / 29,037 | 0 | 39,497 |
| 50 | Référence | 68,165 | 19,908 | 12,626 / 34,498 / 50,570 | 14 | 99,540 |
| 50 | Candidat | 65,565 | 18,257 | 24,193 / 69,429 / 92,838 | 203 | 133,415 |

Ces MSPT portent sur les ticks planifiés. Les passes rapides sur le même thread
restent mesurées séparément : à 50 bots, P99 du coût planifié + passes rapides
associées **75,889 → 134,928 ms**, maximum **112,706 → 163,866 ms**. La fréquence
légèrement supérieure à 20 dans des phases courtes n'est pas une nouvelle cible
du scheduler. **20 TPS ne sont pas maintenus à 50 bots dans cette campagne
locale**, et aucune stabilité distante n'est revendiquée.

| Bots | Chunks sauvegardés réf. → cand. | chunk_total mural cumulé s | fsync mesurés réf. → cand. | CPU serveur % réf. → cand. | CPU client % réf. → cand. |
|---:|---:|---:|---:|---:|---:|
| 1 | 433 → 437 | 0,288 → 1,913 | 0 → 874 | 1,8 → 2,0 | 1,7 → 1,6 |
| 10 | 3 447 → 3 540 | 1,121 → 5,992 | 672 → 7 814 | 9,3 → 8,6 | 8,3 → 6,4 |
| 25 | 7 708 → 7 877 | 2,710 → 13,245 | 564 → 16 408 | 65,5 → 53,1 | 79,9 → 57,0 |
| 50 | 16 027 → 15 905 | 5,644 → 27,276 | 752 → 32 674 | 85,4 → 85,2 | 99,0 → 93,2 |

CPU : moyenne par passe, 100 % = un cœur logique. RSS maximale à 50 bots :
42 020 → 41 980 KiB ; pas de croissance anormale démontrée sur ces essais
courts. Pics de file 54 → 58, sous la limite 8 192. Latences client P95 à
50 bots par passe : 105,06/89,37/101,98 ms avant et 99,46/99,11/119,52 après.
Volumes de chunks compressés à 50 : 6 010 256 → 5 983 400 octets. Les compteurs
IO de processus, schedstat, wchan, pression hôte et timings individuels restent
dans les JSON/CSV bruts.

Les petites variations du nombre de sauvegardes et de ramassages résultent de
la cadence des dirty flush et de la simulation pendant ces durées différentes.
Les fsync joueurs restent distincts des deux nouvelles barrières par chunk.
Le scénario est fermé par confirmations ; le client Python limite encore le
rythme, notamment avant correction. La durée plus courte à 50 bots ne représente
**aucun gain de capacité** : la cadence des ticks et leurs pauses se dégradent.
La comparaison contrôlée à 200 sauvegardes et octets identiques montre le
surcoût propre à la sécurité. Aucune garantie de durabilité n'est retirée pour
le réduire, et aucune deuxième optimisation n'est ajoutée.

### Tests fonctionnels, format natif et sanitizers

`make -B precommit-check` réussit sur la copie de sources locale finale :
**24 suites C**, démarrage isolé, réseau multi-epoll 1/2/4 workers, affinité sur
un CPU, arrêt sous charge, login/reconnexion/UUID/refus online et tests Python.
Le registre complet et les identifiants réseau des blocs sont régénérés et
comparés aux rapports officiels disponibles. Les autres sources brutes
recettes/loot/composants absentes limitent leur generated-check à la présence
des fichiers. Le binaire reconstruit est identique par cmp au candidat des
mesures. Aucune validation positive Mojang n'est annoncée.

| Scénario | Binaire normal | ASan+UBSan | Comparaison |
|---|---|---|---|
| Escaliers et voisins, collisions, orientations, deux observateurs | 192 observations, puis 8 états après chacun de 3 redémarrages | 192 + 8 après redémarrage | Nouvelle capture officielle 26.1.1 et fixture versionnée identiques. |
| Blocs pleins, bûches, dalles/fusion, refus, inventaires, shulker BlockEvent | 18 scénarios, 4 états et slot 36 après chacun de 3 redémarrages | Même suite + relecture | Identique à la fixture Vanilla existante ; comportement de placement inchangé. |
| Ramassage | 100 objets, observateurs concordants, inventaire après reconnexion et 3 redémarrages | 100 objets, reconnexion et redémarrage | Aucune duplication/perte dans les scénarios terminés normalement. |
| Anvil/NBT et monde | Versions 4788/4785, limites, états, dirty/revision, erreurs et concurrence | Suites C pertinentes et nouveau test atomique | NBT/chunks relus, pas uniquement fichiers présents. |

Une validation supplémentaire réécrit les **fichiers natifs d'un vrai monde
Vanilla** via mc_save_bench `rewrite-region`, entre arrêt et redémarrage du
serveur officiel. Le runner compare les SHA-256 des NBT décompressés de chaque
slot avant/après. Sur chacune des trois passes : **4 régions, 1 294 chunks,
empreintes toutes identiques**, 4 publications acquittées, puis relecture des
8 états par Vanilla. Les 192 observations de cette nouvelle exécution sont
également identiques à la fixture. Les fichiers entities/poi ne sont pas
réécrits. Le chemin 26.1.1 observé est
`world/dimensions/minecraft/overworld/region`, sans extrapolation d'un ancien
layout. Le marqueur externe 128 a été vérifié dans le bytecode RegionFile du
JAR officiel 26.1.1 ; son SHA-256 interne reste
`3647564632a2f589be2a87bf544f628e5f67cb3780c86f7fa972ec36892aa081`.
La validation structurelle du writer n'est pas un outil de réparation ni une
certification de tous les payloads déjà corrompus ; le contenu non modifié est
copié, et le test natif vérifie indépendamment la décompression et les hashes.

ASan+UBSan : compilation O1/g non-PIE, `-fsanitize=address,undefined`,
`-fno-omit-frame-pointer`, lien `-no-pie`,
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. **Onze suites C** passent :
test_pvp, test_stairs, test_world_consistency, test_anvil_roundtrip,
test_atomic_persistence, test_player_nbt, test_chunk_network_codec,
test_paletted_container, test_perf_config, test_block_registry et test_nbt.
Les trois suites réseau du tableau passent aussi. Aucun diagnostic mémoire ou
UB dans les passes finales. Les messages d'erreur des injections sont attendus
et leurs assertions vérifient la préservation des données.

TSan local : test_perf_config, test_world_consistency et
**test_atomic_persistence**, compilés non-PIE avec `-fsanitize=thread`, terminent
avec **code 0**, sans diagnostic. Cela concerne ces trois tests, pas l'ensemble
du serveur réseau, et ne valide pas le runtime distant précédemment en échec
`unexpected memory mapping`. Aucun test TSan distant n'est tenté pendant la
maintenance.

Incidents de harnais conservés et corrigés : une première compilation avait un
include avant `_GNU_SOURCE`, masquant sched_getaffinity/CPU_COUNT ; l'ordre a
été rétabli. Le premier passage ASan de l'injection EIO ne consommait pas la
panne, car glibc Fortify appelait `__fread_chk` : le wrapper de test couvre
maintenant fread **et** sa variante fortifiée, sans désactiver cette protection,
et les onze suites ont été rejouées. Le premier essai de réécriture native
cherchait `world/region` et échouait avant de modifier un fichier ; il a été
corrigé après inspection du layout exact 26.1.1, puis rejoué entièrement.
La découverte du risque de régénération sur erreur de lecture a entraîné une
correction supplémentaire de sécurité et une reprise des mesures sur le
binaire final. Les pilotes antérieurs ne figurent pas dans les tableaux.

### Livrables, limites opérationnelles et reprise distante

SHA-256 du binaire normal final testé :
`48f9830fe1271f165329ac884f78872cad5224f194a0cd351f5de426f3c876c0`.
Référence serveur 8fd89fcc :
`c9eafc0e5f7616f584b35f8010be4a898dbe784c34bceea387051c72461d14c1`.
Binaire ASan+UBSan :
`320744204ae88e382ec75495b3e2231bebee5b5003682c81bfdb189e5129967a`.

Les services **25565 et 25587 n'ont subi aucune modification** : aucun
redémarrage, changement de configuration/authentification/permissions, transfert
de binaire ou accès à leurs mondes. Leur état actuel n'est pas contrôlé durant
la maintenance. Aucun déploiement n'est réalisé. Les processus Minecraft locaux
temporaires sont arrêtés et leurs ports libérés à la fin des validations.

Artefacts bruts, ignorés par Git, sous
`tests/stress/results/atomic-persistence/` :

- `stage/tests/stress/results/baseline` : preuve `.mca` avant/après corruption historique, états et syscalls.
- `stage/tests/stress/results/final-atomic-unit` : images, temporaires interrompus, données NBT et régions testées ; `atomic-nvme-final.log` en décrit les assertions.
- `stage/tests/stress/results/final-server-crashes` : six SIGKILL de vrais serveurs temporaires, snapshots originaux et relectures.
- `stage/tests/stress/results/final-backend` : 42 passes chronométrées, 14 comptes syscall, relectures et images ; `final-bots` : 24 passes, tous les ticks/fsync/actions/ressources, manifestes et résumés.
- `stairs-final`, `vanilla-final`, `placement-final`, `pickup-final`, `vanilla-native-final` sous ce même répertoire : captures, hashes natifs et redémarrages.
- Logs de build/precommit, `analysis.txt`, sources de calcul et sortie javap ; les résultats ASan/UBSan/TSan initialement en RAM sont copiés dans `sanitizers` sur NVMe.

Fichiers du lot : `Makefile`, `include/mc_atomic_file.h`, `include/mc_anvil.h`,
`include/mc_chunk_store.h`, `src/util/atomic_file.c`, `src/world/anvil.c`,
`src/world/chunk_store.c`, `src/world/world.c`, `tests/test_atomic_persistence.c`,
`tests/test_anvil_roundtrip.c`, `tests/stress/profile_persistence.py`,
`tests/stress/run_block_checks.py`, `tests/stress/README.md`,
`tools/mc_save_bench.c`, `tools/mc_trace_io.c` et ce document. Les scripts restent
invoqués par python3 ; aucun changement de permission n'est nécessaire.
Un seul commit local est prévu, sans push :
`Implement atomic durable chunk and Anvil region saves`.

La condition d'intégrité est démontrée dans les scénarios couverts : aucune
interruption de processus pendant la préparation ne détruit l'ancienne image
valide, et aucune image partiellement écrite n'est publiée. L'incertitude après
remplacement est signalée ; les erreurs de lecture ne déclenchent plus un
effacement par régénération. Les limites de matériel, de versions externes,
d'ACL/xattrs, de fichiers `.mcc` et de transactions multi-fichiers restent
explicites. Il ne s'agit pas d'une garantie universelle contre toute panne.

Avant toute promotion, **répéter intégration, injections, sauvegardes et charges
sur le Xeon/HDD RAID après maintenance**. Le budget de tick et le coût des
barrières devront être examinés sans revenir à une écriture destructive ni
supprimer les fsync. La reprise des familles de blocs Vanilla peut suivre cette
validation de sécurité et de capacité ; aucune porte, trappe, redstone, phase 3
ou simulation parallèle n'est commencée ici. La phase 2 reste ouverte.

## Stockage Vanilla S1 — index natif et préparation de l'allocation (30 octobre 2026)

### Portée et état initial

HEAD initial vérifié : `4997ad9ef0a9642a1364dd3a358261933c7ee7cf`.
L'arbre suivi était propre ; `.vscode/`, `src/main.py` et `start-aider.sh`
étaient non suivis et restent exclus du lot. Aucun push. L'autorisation
explicite de maintenance permet les tests locaux dans des exports, ports et
mondes jetables. Aucun accès, déploiement ou changement des services distants
25565/25587 ; leur état actuel n'est pas vérifié pendant l'indisponibilité.
Les règles permanentes AGENTS/skills ne sont pas modifiées.

S1 ajoute une fondation de lecture, d'indexation et de **planification en
mémoire**. Le backend de remplacement atomique du lot précédent reste utilisé
par défaut. Aucune écriture incrémentale, migration de monde, nouvelle file de
sauvegarde, modification de gameplay, simulation parallèle ou refonte du tick.
La phase 2 et le chantier de stockage restent ouverts.

### Référence exacte : format, algorithme Vanilla, garanties GoatCore

Référence : JAR interne officiel 26.1.1, protocole 775, DataVersion 4788,
SHA-256 `3647564632a2f589be2a87bf544f628e5f67cb3780c86f7fa972ec36892aa081`.
Les sorties `javap -p -c` de RegionFile, RegionFileStorage, RegionBitmap,
IOWorker, RegionFileVersion et ChunkAccess sont conservées dans les artefacts
S1. Il s'agit du bytecode de cette version, pas d'un algorithme extrapolé.

- **Format** : secteurs de 4 096 octets ; deux secteurs d'en-tête ; 1 024
  emplacements big-endian (offset 24 bits, compte 8 bits), puis 1 024 timestamps.
  Un record contient une longueur big-endian, un octet de compression et son
  payload. La longueur comprend le codec, pas son préfixe de quatre octets.
  Codecs déclarés dans le JAR : 1 gzip, 2 deflate/zlib (défaut), 3 none,
  4 LZ4, 127 custom ; bit 128 pour un flux externe `.mcc`.
- **Algorithme Vanilla** : RegionBitmap utilise un BitSet et recherche une
  plage contiguë libre ; les secteurs d'en-tête sont réservés. RegionFile.write
  réserve de nouveaux secteurs, écrit le payload, actualise offsets/timestamp
  et écrit l'en-tête, exécute l'opération externe éventuelle, puis libère
  l'ancienne plage. Les records de 256 secteurs ou plus passent par un fichier
  externe et un stub interne. `flush` appelle FileChannel.force(true).
  RegionFileStorage conserve au plus 256 régions dans son cache, évince/ferme
  la dernière, et relaie lecture NBT/écriture et flush.
- **Ordonnancement Vanilla** : IOWorker utilise un PriorityConsecutiveExecutor,
  une SequencedMap de PendingStore et des CompletableFuture. Une nouvelle
  demande du même chunk actualise les données encore en attente ; la vidange
  retire la première entrée et programme la suivante, les lectures consultent
  les écritures en attente. ChunkAccess possède un indicateur unsaved ;
  markUnsaved le positionne et tryMarkSaved le teste/remet à zéro. Ces éléments
  sont une référence pour les lots ultérieurs, pas une file ajoutée à GoatCore.
- **Durabilité GoatCore** : la compatibilité du format n'exige pas de copier
  une mise à jour d'en-tête en place. Le writer actuel conserve lock coopératif,
  temporaire unique du même répertoire, écritures complètes avec EINTR,
  fsync du fichier, rename atomique et fsync du répertoire. Seul 0 acquitte la
  sauvegarde ; -1 indique l'échec avant publication et
  MC_SAVE_DURABILITY_UNCERTAIN la publication visible non acquittée. Aucune
  restauration hasardeuse. S1 ne supprime ni ne déplace ces barrières.

### Audit des chemins et du contenu des mondes

| Données | Vanilla 26.1.1 observé | GoatCore actuel |
|---|---|---|
| Chunks terrain | dimensions/minecraft/overworld/region/r.X.Z.mca | chunks/c_CX_CZ.bin, zlib + NBT de chunk ; writer .mca disponible séparément |
| Entités et POI | dimensions/minecraft/overworld/entities et poi | Pas d'équivalent complet ; pas d'import natif de ces régions |
| Joueurs | players/data/UUID.dat | Chemin actuel identique ; lecture de secours playerdata/UUID.dat |
| Progrès/statistiques | players/advancements et players/stats | Pas de producteurs équivalents complets |
| Métadonnées globales | level.dat, level.dat_old, session.lock | Pas de writer level.dat complet compatible avec un monde Vanilla |
| Conteneurs | NBT/block entities et stockage natif concerné | Données prises en charge + fichiers containers/*.mct spécifiques GoatCore |

Le monde officiel temporaire S1 confirme ces chemins, dont les sous-répertoires
region/entities/poi de l'overworld. GoatCore crée encore un répertoire region
historique, mais son chargeur de monde utilise les `.bin` et ne bascule pas
sur `.mca`. Les `.bin` ne sont ni des caches jetables ni des régions natives :
les supprimer détruirait le stockage actif. Les sauvegardes nouvelles restent
4788 et les anciens chunks GoatCore 4785 restent lisibles.

**Migration future non destructive** : inventorier hors ligne les sources,
versions, fichiers externes et empreintes ; conserver une sauvegarde immuable ;
produire un monde candidat séparé, avec level.dat/dimensions/seed et contenu
explicitement compatibles ; exporter les chunks sans perdre les champs opaques,
entités et données associées ; vérifier NBT, états, inventaires et relecture
Vanilla avant une bascule explicite réversible. Aucun renommage automatique ni
migration n'a lieu dans S1. La compatibilité des fichiers Anvil n'est pas une
compatibilité complète du monde ou de ses fonctionnalités.

### Architecture de la fondation

`mc_region_storage.h` / `region_storage.c` définissent trois objets opaques :

1. **Index immuable** : en-tête complet, 1 024 entrées et timestamps, longueur,
   codec/external/decodable, statistiques et bitmap d'occupation à un bit par
   secteur. Lecture par pread, validation de toute la table, offsets/comptes,
   longueurs, secteurs d'en-tête, chevauchements, bornes et troncatures.
   L'occupation inclut les deux secteurs d'en-tête. Les fichiers non réguliers,
   inférieurs à 8 Kio, non alignés ou dépassant les limites représentables sont
   refusés ; aucune réparation ou modification implicite.
2. **Lecteur indexé** : fd O_RDONLY/O_NOFOLLOW épinglé, lecture opaque des
   records, puis décodage gzip/zlib/brut via mc_anvil_read_chunk_indexed. Une
   ouverture peut servir tous les chunks. Les anciens appels Anvil passent
   désormais par cette validation complète ; lire un seul chunk via l'API par
   chemin reconstruit l'index, il faut réutiliser le lecteur pour l'amortir.
   fstat avant/après détecte les changements de taille/mtime en place et renvoie
   ESTALE. Un rename n'invalide pas le lecteur de l'ancien inode ; ctime/nlink
   ne sont donc pas utilisés comme identité de contenu. Cela suppose un monde
   exclusivement possédé ou des écrivains coopératifs, pas un writer hostile.
3. **Plan d'allocation sans I/O** : copie indépendante du bitmap et des entrées,
   recherche contiguë, réservation explicite, croissance virtuelle jusqu'aux
   limites du format. Tickets uniques au processus ; seule une réservation
   non publiée appartenant au plan peut être annulée. Les secteurs encore
   référencés ne deviennent jamais libres par `retire` : une liste conserve
   slot/plage/epoch. `protect` épingle les plages d'une future récupération
   avant toute réservation, même si elles paraissent libres dans l'en-tête.
   Aucun reclaim/commit/unpin ou write en place n'est exposé en S1.

Le snapshot expose device/inode/taille/mtime et CRC32 d'en-tête pour diagnostic
et future vérification de source. **Ce CRC32 n'est ni une génération durable,
ni une preuve cryptographique, ni un journal.** Les plans sont à propriétaire
unique, sans nouveau thread. Les coordonnées sont floor-div/mod 32, y compris
INT32_MIN/MAX. Les index ne contiennent aucun état mutable du monde vivant.

Le writer atomique réutilise le validateur natif, tout en gardant sa propre
allocation dans une **nouvelle image temporaire** et son protocole de
publication existant. Réutiliser des secteurs de l'ancien chunk dans cette
nouvelle image est sûr ; le futur writer incrémental ne bénéficie pas de cette
propriété et le plan S1 conserve donc les anciennes plages occupées.
Les échecs d'allocation du nouveau validateur et de l'ancien allocateur sont
injectés séparément dans le test atomique (trois allocations).

### Données opaques et restrictions

Les records et NBT décompressés sont retournés sans filtrer leurs champs. Le
writer atomique copie les chunks non modifiés octet pour octet. La comparaison
indépendante contrôle tous les octets NBT, y compris les champs non interprétés.
Cela ne promet pas que charger puis reconstruire un chunk via le modèle de
jeu conserve tout NBT Vanilla inconnu : un import natif complet devra définir
cette conservation explicitement avant activation.

LZ4 (4), custom (127), autres codecs et références `.mcc` sont indexés et
signalés. Leurs records internes restent lisibles comme octets opaques ; leur
décodage n'est pas simulé. Les plans et le writer atomique refusent toute région
contenant ces données non prises en charge, y compris une modification d'un
autre slot. Aucun `.mcc` n'est ouvert, écrasé ou supprimé. Une transaction
atomique multi-fichiers n'est pas implémentée. Une indexation réussie valide la
structure, pas le sens de tout NBT ni l'intégrité d'un codec inconnu.

### Tests exécutés et preuves conservées

Compilation C11/O2 avec les options habituelles, dans un export isolé sur le
NVMe local. Le SDK curl déjà extrait lors du lot précédent a été fourni via
C_INCLUDE_PATH/LIBRARY_PATH, sans installation système. Un premier lien sans
ces variables échouait sur `-lcurl` ; le build final les utilise correctement.
`make -B precommit-check` a été rejoué sur les sources finales après nettoyage
de l'export : **succès**, 25 suites C, smoke, multi-epoll, intégration login,
générateurs et tests Python existants. Vérification d'hygiène Git locale séparée
réussie. Aucune nouvelle règle de gameplay.

| Scénario | Résultat S1 |
|---|---|
| Index, bitmap et allocation | test_region_storage : **3 344 assertions** ; région vide, un/plusieurs chunks, 1 024 slots présents, trous, timestamps, bounds, en-têtes/records tronqués, chevauchements et longueurs invalides refusés |
| Coordonnées/allocation | Coordonnées négatives et INT32_MIN/MAX ; recherche contiguë, croissance virtuelle, limite d'offset 24 bits/255 secteurs ; pins conservés, retirement sans libération, tickets périmés/étrangers refusés |
| Inodes et erreurs | Lecteur de l'ancien inode après rename intact ; nouvelle ouverture lit la nouvelle image ; troncature en place détectée ESTALE ; lectures et plans non supportés refusés proprement |
| Compression et données opaques | Région synthétique gzip/zlib/brut + LZ4/custom/inconnu + stub externe ; égalité NBT intégrale et fichiers sources inchangés, entrées et bitmap contrôlés par Python indépendant |
| Quatre régions historiques Vanilla | **1 294 chunks** ; chaque offset/compte/timestamp/longueur/codec et chaque secteur vérifiés ; tous les NBT décodés identiques octet pour octet ; SHA-256 des sources inchangés |
| Backend atomique existant | **80 injections système**, **12 interruptions de processus**, **3 échecs calloc** ; lectures, durabilité incertaine, nettoyage et écrivains coopératifs threads/processus restent couverts |
| Monde/NBT/ancien GoatCore | Suites Anvil, NBT, chunks réseau, monde et persistance : 4788 en écriture, lecture 4785, états, négatifs/frontières, régions multiples, dirty/revision et refus de régénérer une donnée illisible |
| Escaliers/deux observateurs | **192 observations** finales identiques à une nouvelle capture officielle 26.1.1 et à la fixture versionnée ; **8 états** identiques après chacun des **3 redémarrages** |
| Régions réécrites puis lues par Vanilla | Nouveau monde officiel isolé : **4 régions, 1 328 chunks** à chacune des trois passes ; NBT décompressés inchangés, publication atomique acquittée, relecture officielle des états réussie |
| Placement/dalles/inventaires | **18 scénarios**, 4 états sauvegardés et inventaire après chacun des 3 redémarrages ; contrôlés contre la fixture Vanilla existante |
| Ramassage/reconnexion | Scénarios de **2 puis 100 objets**, observateur concordant, inventaire après reconnexion et trois redémarrages ; pas de perte/duplication dans ces scénarios |

Les 1 328 chunks du nouveau monde ne remplacent pas les 1 294 fichiers de
référence historiques : ce sont deux captures distinctes. La suite native
réécrit uniquement les fichiers terrain, pas entities/poi. Les 12 interruptions
C sont des crashes de processus contrôlés, **pas une simulation de coupure
électrique** ; les garanties matérielles et multi-fichiers restent celles du
lot atomique précédent. Aucun protocole incrémental à tester en crash n'est
activé en S1.

**ASan + UBSan** : douze suites C passent, dont le nouvel index, atomic
persistence, Anvil, NBT, monde, PvP/inventaires, escaliers, registre, palettes,
codec réseau, joueurs et configuration. Le lecteur natif relit également les
quatre régions/1 294 chunks sous sanitizers, avec comparaison Python.
Compilation O1/g non-PIE, address+undefined, frame pointers, leak detection et
halt_on_error. Les suites réseau escaliers (192 observations), placement
(18 cas), ramassage (2 puis 100 objets), reconnexion et un redémarrage passent
sur le serveur instrumenté. Aucun diagnostic mémoire/UB dans ces exécutions.

**TSan local** : test_region_storage, test_perf_config, test_world_consistency
et test_atomic_persistence terminent avec code 0, sans diagnostic, compilés
non-PIE avec thread sanitizer et halt_on_error. Cela ne valide pas toutes les
interactions réseau du serveur et ne constitue pas un résultat TSan distant.
Le runtime Xeon historiquement en échec `unexpected memory mapping` n'est pas
retenté pendant la maintenance.

### Microbenchmarks locaux — aucune amélioration de TPS annoncée

Matériel : Ryzen 7 7840HS, 16 CPU logiques, NVMe/ext4. Options O2 identiques,
processus de test et compilations terminés avant les **mesures finales**.
Cache chaud : le pilote hash/lit chaque région avant les chronomètres ; pas
de drop_caches. Trente répétitions par région, lookup de 1 000 000 entrées par
passe ; 29 échantillons de lecture complète sans dump (première passe avec
sortie NBT mesurée séparément). JSONL bruts, octets, opérations et quantiles
empiriques conservés. Les queues extrêmes ne sont pas caractérisées par
seulement trente répétitions.

| Région officielle | Chunks | Métadonnées index (octets) | Index complet P50/P95/P99 (µs) | Lecture + décodage région entière P50/P95/P99 (ms) |
|---|---:|---:|---|---|
| r.-1.-1 | 341 | 24 720 | 181,0 / 208,1 / 221,5 | 2,372 / 2,496 / 2,870 |
| r.-1.0 | 342 | 24 720 | 196,8 / 232,7 / 251,5 | 2,481 / 2,769 / 2,938 |
| r.0.-1 | 306 | 24 712 | 173,1 / 217,8 / 249,8 | 2,067 / 2,469 / 2,599 |
| r.0.0 | 305 | 24 712 | 159,0 / 203,4 / 286,1 | 2,141 / 2,469 / 2,712 |

Pour ces quatre régions, P50 : open seul **2,11–2,39 µs**, en-tête seul
**0,99–1,24 µs**, lookup dans l'index chaud environ **1–2 ns par recherche**
(boucle et checksum compris), lecture du premier record brut **1,34–1,43 µs**,
premier chunk décompressé **5,10–31,41 µs** selon son contenu. Le constructeur
d'index comprend la lecture d'en-tête, les 5 octets de chaque record, la
validation et le bitmap ; son coût n'est pas une simple allocation en RAM.
Mémoire affichée : structure + en-tête + entrées + bitmap, hors surcoût malloc,
fd et éventuel plan indépendant. À la limite du format le bitmap est d'environ
2 Mio ; il ne représente pas un cache des payloads.

La reconstruction actuelle est mesurée face au parent 4997ad9e, sans changer
le protocole d'écriture : **42 passes chronométrées**, trois répétitions par
cas et version, ordre alterné ; **14 passes ptrace séparées** pour les syscalls.
Les durées tracées ne servent pas à comparer la vitesse. Même nombre de
sauvegardes acquittées, tous les états relus ; sorties `.bin` identiques octet
pour octet. Référence serveur du parent :
`48f9830fe1271f165329ac884f78872cad5224f194a0cd351f5de426f3c876c0`.

| Travail fixé | Parent, durée médiane (ms) | S1, durée médiane (ms) | Sauvegardes / fsync mesurés par passe |
|---|---:|---:|---|
| Inactif, 16 chunks, 40 intervalles | 2 002,963 | 2 002,619 | 0 / 0 |
| `.bin`, 200 changements, un chunk | 253,059 | 268,108 | 200 / 400 |
| `.bin`, 200 changements, 16 chunks proches | 276,680 | 281,818 | 200 / 400 |
| `.bin`, 200 changements, plusieurs régions logiques | 265,058 | 266,749 | 200 / 400 |
| `.mca`, 100 sauvegardes, région d'un chunk | 139,771 | 142,823 | 100 / 200 |
| `.mca`, 100 sauvegardes, région de 16 chunks | 144,137 | 142,814 | 100 / 200 |
| `.mca`, 100 sauvegardes, région de 256 chunks | 230,803 | 235,948 | 100 / 200 |

Les variations ne constituent pas un gain de sauvegarde : notamment le cas
`.bin` à un chunk est **5,9 % plus lent** dans cette campagne, malgré un chemin
`.bin` inchangé. Trois répétitions sur un PC actif ne suffisent pas à attribuer
ce delta au nouveau code. Aucune réduction de fsync, de volume écrit ou du
nombre d'actions n'est revendiquée. Les distributions détaillées et chaque
répétition restent disponibles pour distinguer variation et régression future.

Pour 256 chunks, une passe réécrit toujours **105 676 800 octets par write +
1 228 800 par pwrite**, soit **106 905 600 octets** pour 100 sauvegardes :
1 700 write, 400 pwrite et 200 fsync. Les autres occupations réécrivent
2 457 600 octets (1 chunk) et 8 601 600 (16 chunks). Ce sont les octets soumis
aux syscalls, pas l'amplification matérielle du SSD. Même volume sur les deux
versions. S1 ne traite donc pas encore le coût de reconstruction.

Sur les **300 sauvegardes** du cas 256 chunks, temps du chemin `.mca` seul :

| Phase | Parent P50/P95/P99 (µs) | S1 P50/P95/P99 (µs) |
|---|---|---|
| Lecture/validation région | 118 / 153 / 171 | 125 / 164 / 180 |
| Copie complète | 242 / 324 / 358 | 242 / 318 / 356 |
| Total, barrières comprises | 2 222 / 2 520 / 2 632 | 2 221 / 2 662 / 2 962 |

Maxima total observés : 2,984 ms parent, 6,581 ms S1. Le total cumulé S1 est
679,114 ms murales contre 204,688 ms CPU pour ces 300 écritures : l'attente
reste importante. Ces durées de backend ne sont **pas des MSPT**. Aucun nouveau
benchmark de TPS/charge bots n'est présenté comme un gain : aucun writer
incrémental ni nouvel ordonnanceur n'est activé. Le NVMe local ne représente
pas le Xeon/HDD RAID ; mesures et durabilité devront être rejouées sur ce
matériel avant tout déploiement.

### Préparation de S2 et limites de sécurité

S2 devra définir un journal de récupération **versionné, borné et vérifiable**,
avec générations/identité durable de région, ancien et nouvel emplacement,
longueurs/empreintes, ordre des transactions et points d'acquittement. L'ordre
à démontrer est : verrou/propriété et validation de la source ; récupération
et protection de toutes les plages encore référencées ; réservation hors
plages protégées ; écriture complète et synchronisation des nouveaux payloads ;
journal/métadonnées récupérables durablement ordonnés ; publication ;
acquittement seulement après les barrières nécessaires. La libération des
anciens secteurs doit attendre l'expiration de **toutes** les références de
récupération. Un simple fsync d'un en-tête potentiellement déchiré ne suffit pas.
Les tickets S1 et epochs en RAM ne sont pas des identifiants de transaction
persistants. Revalider la source et sa génération avant toute publication.

Matrice minimale S2 à définir/tester avant activation : crash avant journal,
pendant payload, après sync des données, pendant en-tête/journal, après
publication avant acquittement, pendant checkpoint/libération et au replay
répété ; EIO/ENOSPC/EDQUOT/EINTR, journal tronqué/corrompu, séquences anciennes,
transactions successives même région, snapshots de chunks obsolètes, plusieurs
régions et références `.mcc`. Pas d'atomicité multi-régions ou
inventaire/objets/joueurs promise. Aucun journal ni mécanisme de récupération
incrémentale n'est livré dans S1, et les secteurs retirés ne sont jamais
réutilisés par ses plans.

Fichiers du lot : `.gitignore`, `Makefile`, `include/mc_region_storage.h`,
`include/mc_anvil.h`, `src/world/region_storage.c`, `src/world/anvil.c`,
`tests/test_region_storage.c`, `tests/test_atomic_persistence.c`,
`tools/mc_region_bench.c`, `tests/stress/check_region_storage.py`,
`tools/check_repo_hygiene.sh`, `tests/stress/README.md` et ce document.
Les scripts Python restent invoqués par python3, sans changement de permissions.

Artefacts bruts conservés et ignorés par Git sous
`tests/stress/results/storage-s1/` : sorties javap, manifest.json,
precommit-final.log, export stage et binaires, native-benchmark-final
(en-têtes, bitmap, JSONL, NBT décompressés, hashes), backend-final (42 mesures,
14 comptes de syscalls, CSV et mondes), analysis.json/analyze.py,
stairs-final/placement-final/pickup-final/pickup100-final, vanilla-final
(captures et hashes des trois réécritures), sanitizers et exécutions asan-*.
Les pilotes antérieurs ne sont pas utilisés dans les tableaux finaux.

SHA-256 du binaire normal final testé :
`349f1e3d41dd6361edce2d731c988e5a02a8283f0b73f7b0140e6fea613a0583`.
Binaire ASan+UBSan :
`24cc8ee8a76c80c7b3d0a79a0b77bb0b5ab6d7e734b93406f030222cf0cb5fc7`.
Outil de lecture/benchmark :
`2a1440945fb0596062e243e403d0e4ba934b69627547bb1bd207f86550379c0c`.
Un seul commit local pour S1, sans push :
`Introduce native Anvil region indexing and sector allocation`.

S1 fournit une fondation testée ; la réduction des réécritures et le coût
minimal sur le tick restent des objectifs des lots suivants. Priorité à S2 :
protocole de récupération et preuves de résistance aux interruptions, puis
revalidation Xeon/HDD RAID. Aucune porte, trappe, redstone ou phase 3 commencée.

## Stockage Vanilla S2 — écriture incrémentale et récupération (30 octobre 2026)

Parent audité : e9a2f61eb7d9fee820be5daace6ca2083f3ec24a. Arbre suivi propre ;
les trois fichiers/répertoires personnels non suivis sont préservés. Maintenance
et exception locale maintenues, aucun accès aux services principaux.

### Choix et garanties du protocole

Comparaison des stratégies : le remplacement atomique existant fournit une
image directement cohérente mais recopie toute la région ; un redo exige de
conserver durablement assez de nouvelles données pour reconstruire tout commit
incomplet ; un undo de données en place exigerait des copies de payloads anciens.
**Choix S2 : copy-on-write des secteurs et journal de métadonnées (undo avant commit, redo après commit).** Les
anciens payloads restent intacts et les nouveaux sont écrits ailleurs. Un batch
est trié par slot ; doublons et suppressions sont refusés. L'ancien en-tête
complet suffit à annuler le batch, même si un secteur d'en-tête est déchiré.
Aucune atomicité de pwrite, quatre octets ou secteur, n'est supposée.

Le journal est immuable et publié par le helper atomique existant avant toute
mutation de la région. Après fsync des payloads puis de l'en-tête, un petit
état auxiliaire par région est remplacé atomiquement et durablement : il porte
UUID, génération monotone, identité device/inode, issue COMMIT et SHA-256 du
journal. Cette publication valide la transaction. Le journal est ensuite
supprimé et le répertoire synchronisé ; le retour 0 exige aussi ce nettoyage.
Une erreur après publication peut donc laisser un commit durable non acquitté.

Un journal de génération G est admissible si l'état est G-1 (transaction en
attente : undo), ou G avec la même empreinte et une issue COMMIT (redo) ou
ABORT (undo idempotent). Tout autre cas bloque la région. Le rollback restaure
et synchronise l'ancien en-tête, puis publie l'issue ABORT de génération G
avant nettoyage. Un ancien journal ne peut ainsi ni annuler un commit ultérieur,
ni revenir sur son propre commit déjà durable. Aucune génération n'est avancée
avant que le journal soit prêt : seule une transaction peut être en attente.

Les deux formats sont versionnés et SHA-256 protégés. Une identité incohérente,
un fichier ambigu, tronqué ou corrompu bloque les écritures ; aucun journal
utile n'est effacé pour régénérer un chunk. Les empreintes détectent une
corruption, elles n'authentifient pas un écrivain malveillant.

Ordre : propriété exclusive du répertoire de test, verrou de région,
récupération préalable, index S1 et réservations sans toucher aux anciens
secteurs ; journal durable ; croissance/écriture COW ; fsync des données ;
écriture d'en-tête ; fsync de région ; état COMMIT durable ; unlink journal ;
fsync répertoire ; acquittement. Les anciens secteurs ne deviennent réutilisables
qu'à la reconstruction suivante du bitmap, après disparition durable du journal.
La récupération vérifie les anciens records et, pour COMMIT, les nouveaux
records ; elle restaure l'en-tête choisi, fsync, publie ABORT si nécessaire,
puis supprime/synchronise le journal. Une interruption à chaque étape reste
récupérable et idempotente. Pas d'hypothèse d'atomicité du pwrite d'en-tête.

Le backend est lié uniquement aux outils/tests explicitement expérimentaux. Un
répertoire doit porter un marqueur de tests jetables et son propriétaire
exclusif maintient un flock non bloquant pendant toute la session ; deux
processus S2 indépendants ne peuvent pas posséder ce même répertoire. Le tick,
les workers, `chunks/*.bin`, le writer atomique et les règles Minecraft restent
inchangés. Le lecteur S1 ordinaire ne connaît pas le journal ; un lecteur
concurrent non coordonné ou Vanilla ne doit pas ouvrir une région en cours de
transaction. Après fermeture propre ou récupération achevée, le `.mca` est
natif et n'a besoin ni du journal ni de l'état auxiliaire pour sa lecture par
Vanilla. Copier une transaction inachevée vers un autre inode n'est pas une
procédure de récupération : préserver les originaux et récupérer avant export.

Limites : données externes `.mcc`, LZ4, custom/inconnu refusées sans
modification ; aucune transaction multi-régions/inventaires ; aucun gain de TPS
revendiqué. Les tests SIGKILL conservent le cache noyau et ne reproduisent pas
une coupure électrique. Les garanties électriques sont conditionnelles au
respect des fsync et de l'ordre des écritures par le système de fichiers et
le matériel ; une corruption arbitraire du disque reste hors démonstration.

### Implémentation et formats S2 version 1

`region_txn.c` et `mc_region_txn.h` sont liés uniquement à `mc_region_txn` et au
test C. Aucun symbole mc_region_txn_* n'apparaît dans mc_server (contrôle nm).
Le changement commun à S1 est un constructeur d'index en lecture seule prenant
un en-tête sauvegardé et sa taille logique. Il permet de vérifier les anciens
payloads quand l'en-tête sur disque est déchiré ; le constructeur ordinaire garde
ses contrôles. Cette vue historique ne constitue pas une base d'allocation
courante hors du protocole de récupération.

L'outil exige `--experimental-disposable` et le marqueur exact
`.goatcore-s2-disposable`. `init` refuse un répertoire existant. Une session
conserve `.s2-owner.lock` sous flock exclusif/non bloquant ; chaque région prend
aussi le `.mca.lock` coopératif existant. Le thread ouvrant est l'unique
propriétaire du handle ; un appel depuis un autre thread est refusé EPERM.
Aucun worker, mutex de simulation ou choix de backend dans mc_server n'est
ajouté. Les verrous sont consultatifs : ils ne peuvent pas protéger contre un
outil externe non coopératif ou un lancement Vanilla simultané. Aucun partage
de monde avec ces outils n'est autorisé par le contrat expérimental.

Les updates sont des snapshots NBT immuables explicitement fournis par
l'appelant, triés par slot, sans doublons. La sérialisation du modèle de jeu et
les révisions des chunks vivants ne sont pas intégrées ici ; un appelant doit
ordonner ses snapshots. Il n'y a aucune file dont un ancien résultat pourrait
arriver tard. La génération interdit le rejeu d'anciennes transactions disque,
pas un utilisateur qui fournirait volontairement un vieux NBT comme nouvelle
mise à jour. S3 devra définir ce lien avec les revisions avant toute intégration.

Les records modifiés sont préparés/compressés en zlib, les autres chunks ne
sont pas reconstruits. Le plan S1 réserve des secteurs hors de toutes les plages
encore vivantes ; même pour un batch de 1 024 updates, aucun ancien secteur
n'est réutilisé. La région ne fait que grandir si nécessaire ; aucune troncature
réduisant son contenu et aucun compactage ne sont réalisés. Un seul timestamp
par batch est copié dans les entrées modifiées ; les autres timestamps restent
identiques. Limites : 1–1 024 updates, 1–255 secteurs par record, NBT individuel
au plus 64 Mio, total des secteurs nouveaux du batch au plus 64 Mio, offsets
24 bits. Les suppressions et codecs externes/non pris en charge sont refusés.

Les entiers des fichiers auxiliaires sont big-endian, sans dépendre du layout
d'une structure C. Aucun pointeur, ticket RAM ou CRC32 S1 n'est utilisé comme
identité transactionnelle durable.

**`r.X.Z.mca.s2-state` (160 octets)** :

| Offset | Contenu |
|---:|---|
| 0 / 8 / 12 | Magic GCS2STA1 (8), version 1 (u32), taille 160 (u32) |
| 16 | UUID aléatoire de lignée de région (16 octets, getrandom vérifié) |
| 32 / 40 / 48 | Dernière génération résolue, device, inode (u64) |
| 56 / 60 | Coordonnées de région X/Z (bits i32) |
| 64 / 68 | Issue : 0 initiale, 1 COMMIT, 2 ABORT (u32) ; 28 octets réservés nuls |
| 96 | SHA-256 du journal résolu ; nul pour génération initiale 0 |
| 128 | SHA-256 des 128 octets précédents |

**`r.X.Z.mca.s2-journal` (16 584 à 98 464 octets)** :

| Offset | Contenu |
|---:|---|
| 0 / 8 / 12 | Magic GCS2UND1 (8), version 1 (u32), longueur totale (u32) |
| 16 / 32 | UUID (16), génération de transaction G (u64) |
| 40 / 48 | Device/inode attendus de la région (u64) |
| 56 / 64 | Tailles logique ancienne et nouvelle (u64) |
| 72 / 76 | Coordonnées X/Z (bits i32) |
| 80 / 84 / 88 | Nombre d'anciens chunks et d'updates (u32), 40 octets réservés nuls |
| 128 | Ancien en-tête complet, 8 192 octets |
| 8 320 | Nouvel en-tête complet, 8 192 octets |
| 16 512 | Records triés : tous les anciens chunks puis tous les updates ; chacun slot u32, longueur u32, SHA-256 du record Anvil complet (40 octets) |
| Fin − 32 | SHA-256 de tous les octets précédents |

La longueur hashée d'un record comprend ses quatre octets de longueur, le
codec et le payload, mais pas le padding. Les plages protégées comprennent
bien tous les secteurs alloués, padding inclus. Des secteurs libres fragmentés
sont utilisables seulement s'ils n'appartiennent à aucun ancien chunk.

Le journal et l'état passent par le helper atomique (temporaire unique dans le
même répertoire, fsync/close, rename, fsync/close parent). Le répertoire est
épinglé par FD ; le helper est appelé via `/proc/self/fd/N/NOM`, ce qui suppose
le procfs Linux disponible. Les noms de région/journal sont canoniques ; les
coordonnées hors i32, doublons d'identité, liens symboliques finaux, hardlinks,
fichiers spéciaux, versions inconnues et tailles non bornées sont refusés.
Les lectures auxiliaires utilisent O_NONBLOCK avant leur contrôle de type.

Les contrats Linux utilisés sont ceux de [fsync(2)](https://man7.org/linux/man-pages/man2/fsync.2.html)
(synchronisation distincte du fichier et de son répertoire),
[rename(2)](https://man7.org/linux/man-pages/man2/rename.2.html) (remplacement atomique)
et [flock(2)](https://man7.org/linux/man-pages/man2/flock.2.html) (verrou consultatif
lié à une description de fichier ouverte). Ils ne constituent pas une preuve
que chaque contrôleur matériel respecte ces garanties.

### Ordre effectif et récupération

Pour une région déjà initialisée, un commit effectue **sept fsync** : deux pour
le journal, un pour les données COW, un pour l'en-tête de région, deux pour
l'état COMMIT et un pour la suppression du journal. L'initialisation d'une
nouvelle région vide et de son état demande des barrières supplémentaires.
Le bootstrap peut laisser une région vide valide en cas d'échec, jamais
remplacer un ancien chunk par un état par défaut.

1. Ouvrir le répertoire de tests et prendre sa propriété exclusive ; synchroniser le répertoire, scanner les journaux canoniques et terminer leur récupération avant d'admettre une écriture.
2. Préparer les records et les secteurs via S1 ; lire/hash tous les anciens records ; préparer ancien/nouvel en-tête. Initialiser durablement l'état de lignée si absent, uniquement sans journal en attente.
3. Publier le journal G = dernière génération résolue + 1 et synchroniser son répertoire. Rien dans la région n'est encore modifié.
4. Agrandir seulement si nécessaire, écrire complètement les nouveaux secteurs, puis fsync de région. Les anciens secteurs restent intacts.
5. Écrire les 8 Kio d'en-tête, fsync de région et fermer son FD en vérifiant le résultat. L'image nouvelle est complète, mais la transaction est encore annulable.
6. Publier l'état G/COMMIT et l'empreinte du journal par remplacement atomique durable. C'est le point de validation logique.
7. Supprimer le journal, fsync du répertoire et vérifier la fermeture du verrou ; seul le retour 0 acquitte l'opération. Une fermeture du verrou en échec ne devient pas un faux succès.

Résultats : -1 avant modification de chunks existants ; -2 signifie absence
d'acquittement et fermeture/récupération nécessaires. Ce dernier peut désigner
un commit déjà durable dont le nettoyage n'est pas terminé, ou une transaction
encore à annuler. Toutes les tentatives entrées dans le chemin d'écriture et
échouées rendent la session inutilisable jusqu'à réouverture ; les arguments
rejetés avant toute tentative ne modifient rien. Les logs donnent région,
journal/incident et errno. EINTR et transferts courts sont traités ; close
Linux n'est pas réessayé après EINTR. Les erreurs EIO/ENOSPC/EDQUOT/fsync/rename
ne sont jamais acquittées. Aucun rollback improvisé sur le chemin d'erreur.

À la récupération, vérifier intégralement journal, état, UUID/génération,
identité inode/device, coordonnées, checksums, longueurs, tri et cohérence des
deux tables. Le validateur S1 reçoit l'ancien en-tête sauvegardé sans faire
confiance à l'en-tête actuel. Les nouvelles allocations sont vérifiées hors
des anciens secteurs et dans les bornes. Toutes les empreintes anciennes sont
vérifiées avant mutation ; pour COMMIT, toutes les nouvelles sont aussi
vérifiées sur disque.

- État G−1 : restaurer l'ancien en-tête, fsync, publier G/ABORT durablement.
- État G/ABORT avec même journal : répéter la restauration ancienne.
- État G/COMMIT avec même journal : restaurer le nouvel en-tête vérifié, sans annuler un commit durable.
- Autre génération, autre empreinte, journal/état ambigu ou corrompu : bloquer l'ouverture expérimentale, conserver les fichiers et signaler l'incident.

Après restauration durable et résolution de l'état, unlink et fsync du parent.
La récupération ne raccourcit pas le fichier ; un tail non validé devient
simplement libre dans l'index suivant. Si une croissance partielle a laissé
une taille non alignée mais des anciens payloads complets, elle est seulement
alignée vers le haut. Les temporaires abandonnés restent conservés et ne sont
ni promus ni automatiquement effacés. Une seule transaction en attente par
région, aucune transaction globale entre régions.

### Compatibilité et procédure avant Vanilla

| Moment de l'interruption | Image directement observée après SIGKILL | Décision de récupération |
|---|---|---|
| Avant journal, pendant son temporaire | Ancien en-tête et anciens chunks intacts | Ancien ; temporaire ignoré |
| Journal durable, pendant/après nouveaux secteurs avant en-tête | Ancienne image cohérente, tail éventuellement inutilisé | Annulation de tout le batch |
| Pendant écriture d'en-tête | Image potentiellement déchirée ou batch partiel ; aucune garantie directe | Restaurer ancien en-tête via journal |
| En-tête écrit, avant fsync | Le cache peut montrer le nouvel en-tête ; aucune garantie électrique directe | Annulation si pas de COMMIT durable |
| En-tête fsync, avant COMMIT durable | Nouvelle image cohérente mais transaction non validée | Annulation complète |
| Pendant publication du COMMIT | Région nouvelle complète ; issue durable ancienne/nouvelle selon publication | Ancienne ou nouvelle selon état valide |
| Après COMMIT durable, pendant nettoyage | Nouvelle image durable dans le modèle fsync | Rejeu nouveau puis nettoyage |
| Pendant récupération | En-tête en restauration potentiellement partiel | Répéter la même décision grâce au journal conservé |

Une coupure électrique peut perdre ou déchirer les écritures non synchronisées ;
le cache conservé par SIGKILL ne prouve pas l'inverse. Ces invariants reposent
sur des fichiers locaux, des remplacements atomiques et un stockage respectant
les barrières. Les tests ne simulent ni panne physique du PC, ni tous les
comportements possibles d'un RAID/firmware.

**Procédure obligatoire** : arrêter tous les écrivains, conserver une sauvegarde
des originaux, lancer sur le répertoire expérimental d'origine :

```sh
./mc_region_txn --experimental-disposable recover /chemin/tests-jetables
```

Exiger le code 0, puis vérifier l'absence de `*.s2-journal` et relire les NBT
avec l'index/lecteur ou l'oracle indépendant. Fermer le backend avant Vanilla.
Seulement ensuite exporter les `.mca` propres ou ouvrir ce monde isolé avec
Minecraft 26.1.1. Ne pas armer un monde personnel en y ajoutant le marqueur.
Un journal ou un état corrompu n'est jamais supprimé pour forcer l'ouverture.
La récupération sur une copie d'inode différent est refusée : récupérer les
originaux avant export, ou conserver les copies pour une réparation experte
ultérieure ; S2 ne fournit pas de rebinding automatique.

Les `.mca` propres sont natifs et directement lisibles ; les `.s2-state` et
verrous ne sont pas nécessaires au lecteur Vanilla. Changer ensuite d'écrivain
peut changer l'inode : S2 refusera de réutiliser une ancienne lignée incohérente.
Les lecteurs S1 non coordonnés restent hors contrat pendant une transaction.
LZ4/custom/inconnu et `.mcc` restent refusés avant modification, sans ouverture
ou suppression de fichiers externes. Aucun import/migration de monde ni format
nouveau dans le `.mca`, aucune garantie multi-fichiers d'entités/inventaires.

### Validation exécutée

`make -B precommit-check` réussit dans l'export local isolé : **26 suites C**,
smoke, réseau multi-epoll, login/reconnexion/UUID, générateurs et tests Python.
Le SDK curl déjà extrait est utilisé par C_INCLUDE_PATH/LIBRARY_PATH, sans
installation système. Hygiène Git et diff vérifiés séparément. Le registre,
4788 en écriture et lecture historique 4785 sont couverts par les suites
existantes. Aucun changement de placement, collision, inventaire ou protocole.

Le harnais transactionnel lie les hooks et wrappers de syscall **uniquement au
test** ; ils sont absents de l'outil normal et du serveur. Les racines et images
avant récupération sont conservées sous MC_REGION_TXN_TEST_ROOT. Les données
modifiées dans ces tests comprennent une valeur NBT distincte de l'ancienne et
un champ opaque ; elles sont relues intégralement, pas seulement par existence
de fichier ou acquittement réseau.

| Matrice S2 | Résultat |
|---|---|
| 15 interruptions d'écriture nommées | Avant journal, temporaire partiel, journal durable, secteurs partiels/terminés/synchronisés, avant/pendant/après en-tête, région synchronisée, avant COMMIT, COMMIT durable, nettoyage et acquittement final : issue attendue et relecture répétée identiques |
| 14 interruptions de récupération | Sept étapes chacune pour undo et redo : début, en-tête partiel/terminé, fsync, état durable, unlink et fin ; même issue après nouvelle récupération |
| 68 injections système nommées | EINTR/EIO/ENOSPC/EDQUOT selon write/pwrite/fsync/ftruncate/unlink/rename/close et ENOMEM sur réservation ; échecs conservés, EINTR réessayé sauf close, aucun faux acquittement |
| 20 erreurs pendant récupération | Écriture d'en-tête, fsync, publication ABORT et nettoyage ; journal conservé tant qu'utile, nouvelle récupération réussie |
| Cas supplémentaires | Transferts courts/nuls, fsync du répertoire après rename de journal/COMMIT, en-tête entièrement remplacé par 8 Kio corrompus, journal/état tronqués ou altérés, ancien journal après commit plus récent, même journal COMMIT rejoué, données anciennes endommagées refusées |
| Allocation/intégrité | Régions vides/neuves, 1 024 emplacements, batch de 1 024 updates, trous fragmentés, croissance, transactions successives, plusieurs régions, coordonnées négatives/extrêmes, doublons et record >255 secteurs refusés |
| Propriété | Deux handles du même processus et deux processus concurrents ne peuvent pas posséder la racine ; appels depuis un autre thread refusés ; version récente non écrasée par ancien journal |
| Codecs | LZ4, custom, inconnu et externe : région inchangée octet pour octet après refus ; aucun chemin `.mcc` activé |

Les tests de corruption volontaire échouent **comme attendu** : ils exigent le
refus et la conservation, pas une réparation arbitraire. Les injections de
fermeture consomment réellement le FD avant de simuler l'erreur Linux. Les
quinze issues de crash vont à l'ancien batch avant COMMIT et au nouveau après
COMMIT durable ; après un rename d'état non encore acquitté, SIGKILL observe
la version visible, sans déduire sa persistance électrique. Les erreurs
initialement injectées dans le protocole préliminaire ne servent pas de
résultats finaux ; seules les passes du protocole décrit ici sont retenues.

**Compatibilité officielle après fermeture propre (A)** : nouvelle instance
Java 26.1.1 isolée, 192 observations d'escaliers identiques à la fixture et à
GoatCore ; avant chacun des **trois redémarrages**, quatre régions natives sont
réécrites avec l'outil S2 normal. **1 330 chunks par passe**, tous les SHA-256 des
NBT décompressés inchangés ; huit états observables relus par Vanilla.

**Compatibilité après récupération (B)** : seconde instance Vanilla isolée,
même matrice de **192 observations** ; avant chacun des trois redémarrages,
quatre écritures sont interrompues pendant le pwrite d'en-tête, puis récupérées
**deux fois** avant lancement officiel. **1 295 chunks par passe**, NBT tous
identiques, huit états relus. Cela fait douze interruptions de régions natives
et douze récupérations répétées ; les mondes sont distincts, leurs nombres de
chunks ne sont pas comparés comme une charge de performance. Il s'agit de
réécritures de NBT officiels inchangés ; les tests C distincts prouvent les
changements de contenu et l'annulation/rejeu de batches. Aucun test Vanilla
n'ouvre une transaction en attente sans récupération.

**Lecture directe (C)** : les limites du tableau précédent restent explicites.
S2 ne fournit aucune atomicité de lecture native d'un batch pendant la
réécriture d'en-tête. L'unité de garantie est la région après fermeture propre
ou récupération reconnue. Aucune simulation exhaustive de panne électrique
n'est annoncée.

**Non-régression du backend actif** : GoatCore final passe les 192 observations,
les 18 placements/dalles/fusions, deux observateurs, inventaires et reconnexion,
puis respectivement 8 et 4 états après trois redémarrages. Le ramassage de
100 objets est vu par deux joueurs et conserve exactement les quantités à la
reconnexion et après trois redémarrages. Les états d'escaliers concordent avec
les deux nouvelles exécutions officielles et la fixture. Ces sauvegardes restent
dans `.bin`, jamais dans le backend S2. Aucune transaction globale inventaire,
items et chunks n'est promise.

**ASan+UBSan** : treize suites C passent : test_region_txn, test_region_storage,
test_pvp, test_stairs, test_world_consistency, test_anvil_roundtrip,
test_atomic_persistence, test_player_nbt, test_chunk_network_codec,
test_paletted_container, test_perf_config, test_block_registry et test_nbt.
Le lecteur relit également les quatre régions historiques/1 294 chunks avec
l'oracle indépendant. Les trois suites réseau escaliers/placement/ramassage
100 objets passent avec reconnexion et un redémarrage. Aucun diagnostic dans
ces exécutions. Options O1/g, address+undefined, frame pointers, non-PIE,
detect_leaks et halt_on_error ; les logs d'injection EIO sont attendus.

**TSan local** : cinq suites terminent avec code 0, sans diagnostic :
test_region_txn (dont thread propriétaire et processus concurrents),
test_region_storage, test_perf_config, test_world_consistency et
test_atomic_persistence. Non-PIE, thread sanitizer, halt_on_error. Ce résultat
ne valide ni tout le serveur réseau ni le runtime distant historiquement en
échec `unexpected memory mapping` ; aucun test distant n'est tenté.

### Benchmarks locaux à travail égal

Ryzen 7 7840HS, 16 CPU logiques, NVMe/ext4. Tous les serveurs temporaires et les
compilations sont terminés avant les mesures. Un même outil O2 appelle soit
l'API atomique inchangée, soit S2 : mêmes NBT, régions de même taille initiale,
une plage libre d'un secteur, warmup d'une sauvegarde par région hors mesure.
Trois répétitions, ordre alterné. Chaque passe effectue **100 sauvegardes
acquittées**, puis relit tous les NBT ; les deux backends produisent les mêmes
payloads. Au total **54 passes / 5 400 sauvegardes**, dont 2 700 par backend.
Dix-huit passes ptrace séparées mesurent les syscalls ; leurs temps ne servent
pas à comparer les performances.

Les NBT de contrôle sont petits et comportent DataVersion, S2Value et un champ
opaque ; ils isolent le stockage, sans représenter un chunk jouable complet.
La sérialisation NBT est mesurée séparément dans le total ; les latences d'appel
ci-dessous comprennent préparation/compression et transaction, hors cette
sérialisation. La trace légère MC_SAVE_TRACE_PATH est activée sur les deux
backends pour les phases ; ses coûts et la variabilité du PC restent inclus.
Préparation, warmup, vérification et récupération sont hors période active.

Durées **médianes** des trois passes de 100 sauvegardes, en ms :

| Chunks par région | Répartition | Atomique | S2 |
|---:|---|---:|---:|
| 1 | Même chunk | 125,078 | 404,313 |
| 1 | Slots répartis (un seul disponible) | 128,779 | 406,773 |
| 1 | Quatre régions | 137,090 | 388,408 |
| 256 | Même chunk | 229,238 | 409,136 |
| 256 | Slots répartis | 229,169 | 455,594 |
| 256 | Quatre régions | 236,444 | 441,157 |
| 1 024 | Même chunk | 502,485 | 596,589 |
| 1 024 | Slots répartis | 495,083 | 598,893 |
| 1 024 | Quatre régions | 539,115 | 619,857 |

Latences des **300 appels regroupés** du cas « même chunk », P50/P95/P99 et
maximum, en ms :

| Chunks | Atomique P50/P95/P99 ; max | S2 P50/P95/P99 ; max |
|---:|---|---|
| 1 | 1,227 / 1,377 / 1,490 ; 1,539 | 4,042 / 4,572 / 4,817 ; 5,032 |
| 256 | 2,262 / 2,546 / 2,783 ; 9,169 | 4,087 / 4,868 / 5,063 ; 5,136 |
| 1 024 | 4,892 / 5,621 / 6,319 ; 12,292 | 6,000 / 6,985 / 7,333 ; 8,949 |

Volumes exacts acceptés par les syscalls pour 100 sauvegardes, identiques sur
les trois répartitions de chaque occupation :

| Chunks | Octets atomiques | Octets S2, journal/état compris | Secteurs `.mca` soumis A / S2 | fsync A / S2 |
|---:|---:|---:|---:|---:|
| 1 | 2 867 200 | 2 907 200 | 700 / 300 | 200 / 700 |
| 256 | 107 315 200 | 3 927 200 | 26 200 / 300 | 200 / 700 |
| 1 024 | 421 888 000 | 6 999 200 | 103 000 / 300 | 200 / 700 |

S2 écrit trois secteurs natifs par update dans ces fixtures (record et en-tête),
plus l'état de 160 octets et un journal de **16 624 / 26 824 / 57 544 octets**.
Les fichiers temporaires sont inclus ; l'amplification physique ext4/SSD ne
l'est pas. Baisse de volume total : **96,34 %** à 256 chunks, **98,34 %** à 1 024 ;
à un chunk le journal augmente au contraire le volume de **1,40 %**. Aucun
secteur encore utile à undo/redo n'est réutilisé.

La latence augmente dans ces passes : même chunk, **3,23× / 1,78× / 1,19×**
selon l'occupation. Les sept barrières, journal et état atomiques inclus,
coûtent davantage que les deux du backend de remplacement. Les P99/max ne
s'améliorent pas uniformément. **La réduction du volume n'est pas un gain de
TPS ni une baisse démontrée du temps de sauvegarde.** Le backend actif ne
change pas et ces mesures NVMe ne prédisent pas les résultats HDD RAID.

Pour le même chunk, CPU médian par passe : atomique **18,326 / 61,996 /
218,561 ms**, S2 **38,887 / 80,763 / 204,098 ms**. La différence mural−CPU est
conservée par passe dans les résultats ; elle mesure le temps non CPU, incluant
ordonnancement et attente, pas exclusivement le disque. Les médianes CPU et
murales peuvent appartenir à des répétitions différentes et ne s'additionnent
pas comme une décomposition d'une passe unique.

La sérialisation totale des 100 petits NBT représente environ **60–93 µs**
médians selon la passe. Compression/préparation record, P50 : atomique
**44 / 43 / 58 µs**, S2 **52 / 52 / 56 µs**. La préparation S2 index/allocations/
SHA de tous les anciens records monte de **18 à 426 puis 1 574 µs** P50
(P99 **36 / 527 / 2 053 µs**). Les publications durables du journal et du COMMIT
occupent chacune environ **1,1–1,3 ms** P50. Ces spans sont imbriqués avec les
spans atomic_file_* ; ne pas les additionner deux fois. Le volume réduit
supprime les copies complètes, mais les checksums et barrières demeurent
coûteux ; aucune optimisation spéculative supplémentaire n'est appliquée.

Après les passes, les régions des deux backends présentent une plage libre
d'un secteur, une seule plage libre et une plage libre maximale de longueur 1.
Les métadonnées et calculs indépendants de fragmentation après récupération
sont conservés. Cette charge avec un record d'un secteur ne mesure pas la
fragmentation de tailles variables ; les trous et allocations contiguës sont
couverts séparément dans les suites C.

**Récupération mesurée** : 27 interruptions supplémentaires sur les fichiers
originaux de la campagne S2, après la mesure active ; 18 avant COMMIT et 9 après
COMMIT durable. Même payload NBT réécrit, journal conservé avant récupération,
relecture de tous les NBT et second passage idempotent, tous réussis. Par
occupation, six échantillons undo et trois redo ; P95/P99 reflètent donc surtout
le maximum, sans caractérisation fiable des queues :

| Chunks | Undo P50 / max (ms) | Redo P50 / max (ms) |
|---:|---|---|
| 1 | 3,963 / 5,368 | 2,570 / 2,704 |
| 256 | 4,246 / 4,481 | 3,220 / 3,247 |
| 1 024 | 5,396 / 5,986 | 4,718 / 4,831 |

Ces temps comprennent ouverture/propriété, contrôle des empreintes, restauration,
barrières et nettoyage. Le second passage ne rejoue aucune modification.
Aucun benchmark bots/TPS du backend S2 n'est présenté puisqu'il n'est pas
intégré au monde actif. Les mesures HDD RAID après maintenance restent requises.

### Livrables, limites et étape suivante

Sources ajoutées : `include/mc_region_txn.h`, `src/world/region_txn.c`,
`tests/test_region_txn.c`, `tools/mc_region_txn.c`,
`tests/stress/profile_region_txn.py`. Modifications : `.gitignore`, `Makefile`,
`include/mc_region_storage.h`, `src/world/region_storage.c`,
`tools/check_repo_hygiene.sh`, `tests/stress/run_block_checks.py`,
`tests/stress/README.md` et ce document. Aucun changement de permission de script.

Artefacts ignorés, conservés sous `tests/stress/results/storage-s2/` : exports
stage/asan/tsan, precommit.log, journaux de build/sanitizers, matrices C et
images avant récupération, vanilla-clean/vanilla-recovered (hashes et captures),
stairs-final/placement-final/pickup-final et asan-*, bench-final (54 mesures,
18 traces, 27 récupérations, CSV et commandes), analysis.json/analyze.py,
manifest.json. Les pilotes et prototypes sont conservés séparément et exclus
des chiffres finaux. La mise au point du harnais a corrigé le repérage du parser
du runner et l'attribution des écritures de régions temporaires dans les
statistiques ; les volumes totaux bruts n'ont pas été remplacés par des estimations.

SHA-256 du serveur normal final testé (S2 non lié) :
`d1aad1c56b2d2e3524ff7f29cbb88cec7e7c423b91807404f53f06e677d334ad`.
SHA-256 de l'outil S2 normal réellement utilisé dans les mesures et Vanilla :
`b66dd4f10c4a4323d3cf7c3a8e8c04168904b1b3e560a7b272c963554769a669`.
SHA-256 du serveur ASan+UBSan :
`c9aa1d8f678a4f04c8d5d0fa2a20d031362c3fb809bca57f1e2a04769af49952`.

25565/25587 : aucune action, copie de binaire, modification de configuration,
authentification, permission ou données. Leur état distant n'est pas contrôlé
pendant la maintenance. Aucun déploiement et aucun push. Les processus
Minecraft locaux temporaires sont arrêtés. Les trois éléments personnels non
suivis restent exclus. Un seul commit local prévu :
`Implement experimental crash-recoverable incremental Anvil writes`.

L'activation réelle reste interdite : S2 est un outil expérimental sur racines
jetables. Restent hors démonstration : coupures électriques/firmware, matériel
Xeon/HDD RAID, NFS, transactions multi-régions/joueurs/entités/items, `.mcc`,
codecs non pris en charge, réparation d'une corruption physique ou adoption
automatique d'une lignée sur inode copié, lecteurs non coordonnés, migration
complète des mondes et gestion des snapshots vivants. La compatibilité de fichier
après fermeture/récupération est démontrée dans les cas officiels décrits,
pas une parité complète du contenu des mondes.

Prochain lot recommandé : revalidation Xeon/HDD RAID dès disponibilité et
étude mesurée des batches avec acquittements/révisions clairement définis,
avant toute intégration à l'ordonnanceur ou migration. Ne pas supprimer des
barrières pour masquer le surcoût. Le backend atomique et `.bin` restent les
chemins par défaut ; aucune autre phase Minecraft n'est commencée.

## Stockage Vanilla S3A — backend Anvil actif optionnel (30 octobre 2026)

### Périmètre et audit

État initial : HEAD `f7638ad323aa4848224d299b0e198f0880011ddb` (S2), arbre suivi
propre, seuls `.vscode/`, `src/main.py`, `start-aider.sh` personnels non suivis.
S1 `e9a2f61eb7d9fee820be5daace6ca2083f3ec24a` et S2 sont présents localement.
Aucun push, déploiement, migration ni nouvelle fonctionnalité Minecraft.
L'exception locale explicite pendant la maintenance de 10.0.4.66 s'applique ;
les règles permanentes de déploiement ne sont pas modifiées.

Avant S3A, `mc_server` utilisait exclusivement `chunks/c_<cx>_<cz>.bin`, zlib/NBT,
avec remplacement atomique durable. Les workers chargeaient/généraient des
chunks privés puis les publiaient par la done queue ; le tick intégrait les
modifications et effectuait flush, sauvegardes périodiques, éviction et arrêt.
Le lecteur Anvil historique était permissif (états inconnus remplacés par air,
indices invalides tolérés) et son encodeur construisait un NBT minimal,
insuffisant pour modifier un chunk officiel sans perdre ses champs opaques.
Il utilisait aussi un packing compact des palettes, conservé pour les `.bin`.
S1 validait/indexait les régions ; S2 restait un outil expérimental séparé.

Référence examinée : JAR officiel 26.1.1, protocole 775, DataVersion 4788,
SHA-256 interne `3647564632a2f589be2a87bf544f628e5f67cb3780c86f7fa972ec36892aa081`.
Les désassemblages de `SimpleBitStorage`, `SerializableChunkData` et
`DirectoryLock` sont conservés avec les résultats. La version cible range
`floor(64/bits)` valeurs par long, sans chevauchement. Elle recalcule les types
de heightmaps manquants d'après les blocs ; les tableaux de lumière doivent
également être invalidés après modification. Son verrou `session.lock` est un
verrou de fichier Java interprocessus, avec lequel S3A se coordonne par fcntl.

### Intégration et invariants

Une interface `mc_world_storage` centralise sélection, ouverture, lecture et
écriture. `chunk-storage=bin` est le défaut, y compris pour une configuration
existante sans cette clé. `chunk-storage=anvil-atomic-experimental` sélectionne
explicitement le chemin `dimensions/minecraft/overworld/region/r.RX.RZ.mca`.
Les coordonnées négatives sont calculées par les primitives S1. Aucun fallback
`.bin`, aucune conversion et aucun chemin S2 dans le serveur. La génération
plate existante n'est appelée que pour un slot/région réellement absent ; une
erreur ou un format non pris en charge laisse le chunk indisponible et produit
un diagnostic. Les mondes contenant des `.bin` ou des régions à l'ancien
emplacement ne sont pas adoptés silencieusement par le mode natif.

Un marqueur `.goatcore-storage` durable empêche une réouverture accidentelle
en mode `.bin`. Un verrou de monde empêche deux sessions natives simultanées ;
`session.lock` empêche l'ouverture concurrente par Vanilla. Un verrou partagé
sur le propriétaire S2 exclut son écrivain exclusif ; tout `.s2-journal` en
attente bloque le mode natif. Aucun journal n'est effacé ni récupéré
implicitement. Le marqueur ne sélectionne jamais le backend à la place de la
configuration.

Chaque chunk natif conserve son NBT source brut, possédé/libéré avec le chunk.
Les workers en sont propriétaires jusqu'au handoff ; le tick garde ensuite la
propriété des mutations et sauvegardes synchrones. Aucun nouveau worker,
snapshot différé, ordonnanceur ou changement du tick. Les mutations utilisent
l'invalidation de cache réseau et le marquage dirty existants. Une sauvegarde
ne retire dirty qu'après acquittement durable ; une erreur préserve le snapshot
récupérable et reste visible dans les logs.

Le décodeur natif strict est distinct du codec `.bin` inchangé. Il valide
coordonnées, version, statut, sections, palettes, longueurs et indices. Le NBT
source doit pouvoir être relu/réémis sans différence : doublons de noms,
chaînes tronquées par l'interprétation C, ambiguïtés et structures non
conservables sont refusés. Lors d'une modification, les entrées de palette
existantes, leur ordre et leurs champs opaques sont conservés ; les nouveaux
états sont ajoutés, et les longs alignés à la manière Vanilla. Les sections
inchangées restent intactes. Les six types connus de heightmaps sont invalidés,
ainsi que les lumières de sections et `isLightOn`, pour éviter des données
dérivées incohérentes lors de la réouverture officielle. Les autres champs de
heightmaps, biomes, structures et ticks restent conservés.

La publication réutilise le remplacement complet : copie de la région courante
vers temporaire unique, mises à jour, fsync fichier, rename atomique, fsync du
parent. Le NBT source attendu est comparé au chunk courant **sous le même verrou
de région que la publication** ; ESTALE interdit l'écrasement d'une version
plus récente. Un chunk généré exige un slot encore absent. Les sauvegardes de
chunks distincts d'une région recopient toujours sa version courante. Un retour
`MC_SAVE_DURABILITY_UNCERTAIN` avance uniquement le snapshot visible, conserve
dirty et permet de retenter sans rollback destructif. Les lecteurs S1 gardent
un descripteur d'inode cohérent à travers un rename.

Garanties héritées : avant rename l'ancienne région reste intacte ; après
rename le fichier visible est complet ; 0 acquitte les deux fsync. Une panne
d'électricité réelle, le firmware, un autre système de fichiers et le RAID
restent hors démonstration locale. Le remplacement de la région n'est pas une
transaction globale joueurs/items/entités/POI. S2 reste séparé et désactivé.

### Limites explicites de prise en charge

Le mode natif accepte uniquement les chunks Overworld `minecraft:full`,
DataVersion 4788, sections de blocs -4..19 (sections de lumière -5/20 admises),
palettes reconnues et sans block entities non vides. Protochunks, autres
versions/dimensions, états inconnus, encodages de palette non pris en charge et
NBT non conservable sont refusés. Les BEs ne sont pas interprétées puis
réécrites partiellement : leur présence interdit le chargement natif. Une
sauvegarde nécessitant des BEs est refusée ; les tests natifs n'utilisent pas
les conteneurs. Le stockage `.bin` conserve sa lecture des chunks GoatCore
4785 et ses comportements de conteneurs historiques.

Codecs 1/2/3 lus, écriture zlib 2. LZ4/personnalisés/inconnus et références `.mcc`
font refuser la région ; aucun fichier externe n'est écrasé ou supprimé. Les
palettes existantes trop remplies pour ajouter un état sont refusées plutôt
que compactées au prix d'une perte de métadonnées opaques.

Le terrain est natif ; ni génération Vanilla complète, ni gestion des entités,
POI, fluides, block entities et données globales n'est ajoutée. Les chemins
joueurs `players/data` et leur persistance restent ceux du serveur existant.
Un nouveau monde GoatCore ne constitue pas un monde Vanilla complet. Un essai
avec `level.dat` seul a été refusé par le serveur officiel : 26.1.1 requiert
notamment `data/minecraft/world_gen_settings.dat`. L'essai final utilise donc
une copie des métadonnées officielles complètes avec les régions neuves de
GoatCore. Aucun monde personnel n'est utilisé. Avant Vanilla : arrêter GoatCore,
exiger les acquittements, conserver une copie, vérifier l'absence de journaux S2
et ouvrir uniquement une copie propre. La récupération préalable S2 reste une
procédure séparée sur les inodes originaux, jamais une activation automatique.

### Validations fonctionnelles S3A

Toutes les instances sont temporaires et locales, ports 25593..25600. Les deux
services principaux ne sont ni contactés ni modifiés. Le binaire normal final
est une copie fixe, séparée des répertoires de compilation et des instances.

`make -B precommit-check` : **27 suites C**, générateurs, hygiène, smoke, réseau
multi-epoll, login offline/authentification négative, profils, scripts de stress
et calculs de traces passent. Aucun diagnostic de compilation nouveau. La
construction emploie le SDK curl local déjà utilisé par S1/S2, sans installation
système ni environnement Python supplémentaire.

`test_native_storage` ajoute **98 835 vérifications** : sélection explicite,
refus de mélanges, sessions simultanées/processus distincts, verrou fcntl de type
Vanilla, journal S2 en attente, 5/6/9 bits par palette, matérialisation d'une
section implicite, snapshots obsolètes, sauvegardes rapprochées, préservation
NBT opaque, palette/heightmaps/lumière, codec non supporté, coordonnées de chunk
incohérentes, DataVersion native refusée, BE non vide et NBT tronqué. L'injection
d'EIO du fsync parent vérifie le résultat incertain puis la reprise. Le test
utilise aussi les vrais workers de monde, flush/dirty/éviction/rechargement et
trois ouvertures successives aux coordonnées -513/-512/-17/-16/511/512. Aucun
`chunks/` n'est créé dans le monde natif.

Les suites antérieures Anvil, allocation S1, transactions/récupération S2,
interruptions et erreurs système du remplacement atomique, NBT, chunks,
inventaires et monde restent passantes. La lecture historique GoatCore 4785
reste couverte par les tests existants ; le mode natif strict n'adopte pas ces
anciens encodages. Les injections d'interruption portent sur le backend
atomique partagé, pas sur une coupure électrique réelle.

| Scénario exécuté | Résultat et preuve |
|---|---|
| Monde Anvil neuf, escaliers | 192 observations, deux observateurs, mutations/rotations/retrait/voisinage, puis 8 états conservés après redémarrages ; répété ASan+UBSan |
| Copie officielle 26.1.1 | 192 observations identiques ; trois redémarrages GoatCore, puis deux ouvertures Vanilla et assertions sur les 8 états |
| Nouvelle référence officielle | Suite complète de 192 observations rejouée sur le JAR officiel et comparée à la nouvelle exécution GoatCore |
| Blocs/dalles natifs | 18 cas : blocs pleins, logs X/Y/Z, dalles haut/bas, trois fusions, remplaçables, rejets et collision partielle ; deux observateurs, consommation Survival, 4 blocs et inventaire relus après trois redémarrages |
| Stockage `.bin` | Même suite de 18 cas, plus BlockEvent shulker, inventaire et trois redémarrages ; 483 fichiers de chunks identiques octet par octet au binaire S2 `d1aad1c...` sur le même scénario |
| Frontières de régions | Poses Survival à (-513,64,511), (-512,64,512), (511,64,-513), (512,64,-512) : pierre, log, dalle, escalier ; deux observateurs, état autoritaire et inventaire ; trois redémarrages GoatCore et deux Vanilla |
| Ramassage natif | 100 objets, inventaire et absence de duplication dans ce scénario, trois redémarrages ; exécuté avec ASan+UBSan |
| Régions neuves relues par Vanilla | Métadonnées officielles complètes + régions GoatCore : 8 états vérifiés, puis comparaison indépendante des 98 304 blocs de chacun des **483 chunks** d'origine après deux ouvertures officielles |

Dans la copie officielle initiale, **1 294 chunks** sur quatre régions sont
conservés : quatre chunks de fixtures sont modifiés, les **1 290 autres NBT
restent identiques octet par octet**. Le lecteur Python indépendant vérifie
l'égalité des NBT hors block states et données dérivées, les palettes originales
en préfixe, les biomes, structures, ticks et champs inconnus. `level.dat`, ses
métadonnées associées (`data/`, datapacks lorsqu'ils existent), POI et entités
ne sont pas réécrits par GoatCore et sont vérifiés. Les fichiers des joueurs
actifs évoluent normalement par le mécanisme existant `players/data`.

Après réouverture officielle de cette copie, les **98 304 blocs de chacun des
quatre chunks modifiés** restent identiques au snapshot GoatCore : comparaison
du nom et de toutes les propriétés, indépendamment des palettes/longs choisis
par le sérialiseur officiel. Aucun succès n'est déduit de la seule existence
d'un fichier ou du seul décodage NBT. Les traces officielles finales ne signalent
pas d'erreur de lecture ni de régénération des chunks testés. Les champs opaques
sont vérifiés avant Vanilla : son propre sérialiseur peut modifier des
métadonnées légitimes lors de l'ouverture et ne promet pas de garder tout tag
arbitraire.

Les snapshots des frontières de régions (états **et** dernier slot d'inventaire)
sont identiques entre la nouvelle exécution GoatCore et Vanilla. Les octets
réseau n'ont pas à avoir le même délai ; les deux clients observent les
mutations et les queries relisent l'état autoritaire. L'éviction/relecture est
vérifiée en C avec les workers existants, indépendamment des confirmations
réseau et des tests de redémarrage.

**ASan+UBSan** : 14 suites C terminent sans diagnostic, fuite détectée ni UB :
NBT, NBT arena, Anvil roundtrip, index régions, transactions S2, persistance
atomique, cohérence monde, stockage natif, registre, escaliers, palettes,
joueurs NBT, block entities et codec réseau de chunks. Les serveurs natifs
instrumentés exécutent également 192 observations d'escaliers et les 100
ramassages, avec redémarrages. Options `detect_leaks=1:halt_on_error=1:abort_on_error=1`
et `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`, O1/g non-PIE.

**TSan local** : six suites passent, code 0, sans diagnostic : régions,
transactions S2, persistance atomique, cohérence monde, stockage natif et
configuration perf. Cela couvre leurs exécutions, pas une preuve d'absence de
races de l'ensemble du serveur. Le défaut de runtime distant `unexpected memory
mapping` n'est ni réinterprété ni testé pendant la maintenance.

Mises au point du harnais conservées dans les artefacts : un premier essai
importé a été interrompu parce que le nettoyage de compilation supprimait son
binaire ; les essais finaux utilisent une copie fixe. L'export de sources a
initialement omis le nouvel oracle Python, corrigé avant le precommit final.
L'essai de métadonnées limité à `level.dat` a démontré l'absence nécessaire du
fichier de génération 26.1.1 ; la copie complète passe. Le nouveau scénario de
téléportation lointaine envoyait initialement seulement TeleportConfirm ; il
envoie maintenant aussi la position client, nécessaire pour que Vanilla
actualise l'envoi des chunks. Les essais finaux passent sur les deux serveurs.
La revue finale a corrigé la libération d'un tag Y en cas d'échec d'allocation
lors de l'ajout d'une section absente ; les validations et la campagne finale
portent sur le binaire ainsi corrigé. Aucun de ces essais de préparation n'est
substitué aux mesures finales.

Limites opérationnelles supplémentaires : les verrous sont consultatifs pour
les outils externes ; ceux qui les ignorent ne sont pas couverts. Le destructeur
historique journalise les échecs de sauvegarde sans retourner un statut global
au processus : un code d'arrêt 0 n'est donc pas, à lui seul, un acquittement de
persistance. Inspecter les erreurs et acquittements individuels ; aucune donnée
non acquittée ne doit être annoncée durable. Les biomes NBT sont conservés sur
disque, mais leur transmission réseau et la lumière côté client restent celles
du serveur existant, sans nouvelle parité des biomes/éclairage des mondes
importés. Le corpus officiel utilisé est un monde plat 26.1.1 ; il ne démontre
pas l'adoption sûre de tous les chunks d'un monde de survie peuplé de BEs,
entités et données de génération.

### Campagne locale finale : `.bin` contre Anvil atomique intégré

Binaire normal final identique pour les deux modes :
`14febb95c81cd402ac1b115f19c873c79f29e66aaa6c99e6b8a6b6d85ba2146b`.
Ryzen 7 7840HS, 16 CPU logiques, NVMe/ext4 ; aucun résultat Xeon/HDD n'est
extrapolé. O2, configuration existante `tests/stress/network/one.conf` :
un worker réseau, deux workers chunks, affinité auto, simulation_workers=0,
distances vue/simulation 10. Aucune compilation ni autre instance Minecraft
pendant la mesure. Les processus hôte, ressources, pression CPU/IO/mémoire et
files sont enregistrés, sans prétendre contrôler tous les processus du PC.

Deux templates préparés indépendamment, un par backend, sans migration :
**441 chunks, mêmes noms/propriétés de chacun des 98 304 blocs par chunk**,
vérifiés avant mesure par un lecteur Python indépendant. Copies neuves pour
chaque passe ; préparation/chargement, échauffement 5 s, idle 5 s, phase active
puis idle 5 s séparés. Trois répétitions à chaque palier, ordre des backends
alterné. Les traces couvrent toute la charge active, premier envoi à dernière
confirmation, y compris les ticks qui chevauchent les bornes ; aucun choix
d'une fenêtre favorable de 100 ticks.

Chaque bot effectue **100 poses + 100 destructions**. Dans chaque configuration,
les trois passes confirment respectivement 300/3 000/7 500/15 000 poses **et autant
de destructions**, aux paliers 1/10/25/50. Envois clients, handlers serveur et
confirmations concordent : **103 200 actions sur les 24 passes, aucune manquante,
aucune déconnexion ou erreur de sauvegarde/fsync**. Le maximum de file atteint
62 tâches dans le mode natif à 50 bots. L'état initial et le travail imposé aux
blocs sont égaux ; le nombre de sauvegardes coalescées peut varier.

TPS ci-dessous = somme des ticks commencés / somme des durées actives des trois
passes ; P50/P95/P99 regroupent tous les ticks actifs, et non les médianes des
percentiles de chaque passe. Les MSPT de cette première table concernent le
tick planifié ; le travail du chemin rapide exécuté entre ticks est présenté
séparément, car son omission masquerait une partie du coût réel.

| Bots | Mode | TPS observés | MSPT P50 / P95 / P99 (ms) | Ticks >50 ms | Tick maximal (ms) |
|---:|---|---:|---|---:|---:|
| 1 | bin | 19.980 | 1.995 / 2.683 / 8.069 | 0 | 8.577 |
| 1 | anvil | 19.959 | 3.110 / 4.097 / 8.106 | 0 | 34.576 |
| 10 | bin | 19.960 | 3.768 / 4.991 / 8.049 | 0 | 18.610 |
| 10 | anvil | 19.946 | 5.066 / 6.089 / 9.927 | 0 | 21.480 |
| 25 | bin | 19.973 | 9.220 / 19.534 / 22.659 | 0 | 23.274 |
| 25 | anvil | 19.985 | 18.319 / 33.988 / 39.634 | 0 | 47.431 |
| 50 | bin | 14.556 | 49.451 / 90.700 / 109.121 | 472 | 133.230 |
| 50 | anvil | 5.785 | 98.358 / 180.787 / 211.047 | 431 | 229.953 |

| Bots | Mode | Durée active totale / ticks | Tick + fast P99 / max (ms) | Latence action P50 / P95 / P99 (ms) |
|---:|---|---|---|---|
| 1 | bin | 15.466 s / 309 | 33.957 / 34.999 | 2.383 / 42.006 / 43.016 |
| 1 | anvil | 16.684 s / 333 | 35.256 / 48.626 | 3.736 / 41.842 / 43.923 |
| 10 | bin | 26.352 s / 526 | 25.531 / 33.504 | 38.021 / 41.360 / 42.955 |
| 10 | anvil | 28.577 s / 570 | 33.974 / 39.938 | 38.349 / 42.001 / 43.782 |
| 25 | bin | 16.823 s / 336 | 60.193 / 69.309 | 13.733 / 25.406 / 32.779 |
| 25 | anvil | 24.469 s / 489 | 80.242 / 100.285 | 26.464 / 44.946 / 52.421 |
| 50 | bin | 66.294 s / 965 | 147.931 / 186.843 | 56.023 / 114.011 / 143.992 |
| 50 | anvil | 86.435 s / 500 | 329.508 / 365.226 | 89.607 / 213.836 / 267.900 |

Les temps de sauvegarde suivants regroupent les appels actifs : un seul chunk
distinct à 1 bot, quatre chunks dans quatre régions aux autres paliers. Un
appel n'est donc pas un nouveau chunk. Le volume est celui soumis par le
backend aux écritures (copie de région et mises à jour comprises), pas une
mesure des octets NAND ni de l'amplification interne du SSD. Deux fsync par
sauvegarde de chunk ; le total inclut aussi les fichiers joueurs. Aucun coût de
création/échauffement n'est inclus dans ces volumes.

| Bots | Mode | Sauvegardes | Volume logique (Mo décimaux) | fsync chunks / total | Sauvegarde P50 / P95 / P99 / max (ms) |
|---:|---|---:|---:|---|---|
| 1 | bin | 464 | 0.164 | 928 / 928 | 2.148 / 8.187 / 32.807 / 34.848 |
| 1 | anvil | 480 | 226.099 | 960 / 960 | 3.316 / 9.080 / 34.539 / 48.445 |
| 10 | bin | 3515 | 1.259 | 7030 / 7638 | 1.513 / 2.302 / 2.686 / 18.576 |
| 10 | anvil | 3557 | 1692.946 | 7114 / 7926 | 2.675 / 3.551 / 4.173 / 23.156 |
| 25 | bin | 7801 | 2.893 | 15602 / 16210 | 1.400 / 1.665 / 2.112 / 16.299 |
| 25 | anvil | 7953 | 3781.186 | 15906 / 16596 | 2.394 / 2.843 / 3.484 / 19.921 |
| 50 | bin | 15739 | 5.920 | 31478 / 32344 | 1.450 / 1.998 / 2.310 / 22.659 |
| 50 | anvil | 15413 | 7340.786 | 30826 / 31520 | 2.438 / 2.992 / 3.709 / 27.926 |

Chargements mesurés séparément dans la préparation, en cache système chaud :
1 323 appels (3 × 441), sauf `.bin`/10 bots où un accès supplémentaire est
observé (1 324). Aucune erreur/absence lors de ces relectures. Les statistiques
sont par appel, pas une affirmation d'égalité des nombres d'accès. CPU = médiane
des pourcentages serveur par passe ; **100 % représente un cœur logique**.

| Bots | Mode | CPU serveur médian | RSS maximal (MiB) | Chargement P95 (ms) | CPU client Python médian |
|---:|---|---:|---:|---:|---:|
| 1 | bin | 2.3 % | 25.73 | 1.206 | 2.1 % |
| 1 | anvil | 5.2 % | 30.89 | 2.444 | 2.0 % |
| 10 | bin | 9.6 % | 29.11 | 1.216 | 7.4 % |
| 10 | anvil | 21.3 % | 33.77 | 1.551 | 6.6 % |
| 25 | bin | 46.6 % | 33.45 | 0.970 | 47.9 % |
| 25 | anvil | 64.0 % | 38.17 | 1.406 | 37.4 % |
| 50 | bin | 109.1 % | 41.14 | 0.980 | 93.0 % |
| 50 | anvil | 107.8 % | 45.57 | 1.330 | 81.0 % |

À 50 bots, les sauvegardes `.bin` cumulent **23,658 s murales / 4,905 s CPU** ;
les sauvegardes natives **38,947 s / 18,071 s CPU**. Dans ces dernières :
construction/conservation/sérialisation NBT hors `region_total` ≈ **9,935 s
murales / 9,928 s CPU** ; lecture/validation/contrôle du snapshot **2,565 s**,
compression **0,641 s**, copie de région **1,928 s**, fsync fichier **13,788 s**,
fsync répertoire **8,519 s**. Les phases imbriquées ne sont pas additionnées
aux totaux une seconde fois. Mur - CPU inclut aussi l'ordonnancement, ce n'est
pas un compteur pur d'attente disque. Les primitives de verrouillage ne sont
pas isolées par une nouvelle instrumentation dans ce lot.

L'amplification est confirmée : **7,341 Go contre 5,920 Mo** pour le même nombre
de poses/casses à 50 bots, malgré légèrement moins de sauvegardes natives
(15 413 contre 15 739). Les anciennes garanties de durabilité ne sont pas
retirées. Le coût n'est pas seulement disque : le traitement NBT qui préserve
les champs opaques et la validation de la région ajoutent du CPU mesuré. Aucun
gain de TPS n'est annoncé ; à 50 bots les TPS par passe varient de 13,47 à 16,25
en `.bin` et de 5,35 à 6,25 en natif. À 25 bots les TPS moyens proches de 20
coexistent avec une durée des actions plus longue et des coûts tick+fast qui
dépassent 50 ms. Cela ne démontre pas une stabilité générale à 20 TPS.

La simulation des items reste libre et dépend des ticks, donc la charge totale
incidente n'est pas strictement identique : le premier observateur voit
1 685 objets ramassés en `.bin` contre 898 en natif à 50 bots. Les paquets de
ramassage vus par plusieurs observateurs ne sont pas sommés comme s'ils
représentaient des objets distincts. Le générateur Python atteint environ
93 % / 81 % d'un cœur à ce palier ; il peut limiter le débit et le fanout n'est
pas isolé. Les traces démontrent le surcoût des sauvegardes, mais pas que tout
l'écart TPS provient du disque. La charge de fond et le stockage NVMe du PC
limitent la portée des chiffres. Rejouer sur Xeon/HDD RAID avant déploiement.

Une première campagne de 24 passes est conservée sous `bench-final` ; elle a
précédé le nettoyage d'un chemin d'échec d'allocation. Les chiffres ci-dessus
proviennent exclusivement de `bench-release`, sur le SHA final indiqué.

### Fichiers, artefacts et suite du stockage

Fichiers créés : `include/mc_world_storage.h`, `src/world/world_storage.c`,
`tests/test_native_storage.c`, `tests/stress/native_storage_oracle.py`.
Modifiés : `Makefile`, `include/mc_anvil.h`, `include/mc_chunk.h`,
`include/mc_server.h`, `include/mc_world.h`, `src/main.c`, `src/net/server.c`,
`src/world/anvil.c`, `src/world/chunk.c`, `src/world/world.c`,
`tests/stress/profile_tick.py`, `tests/stress/run_block_checks.py`,
`tests/stress/test_block_placement.py`, `tests/stress/README.md`, ce document.
Aucune modification de `play.c`, des registres, règles de placement, workers
de simulation ni des permissions de scripts.

Les résultats bruts sont conservés sous `tests/stress/results/storage-s3a/`
(ignorés par Git) : exports stage/asan/tsan, `precommit-release.log`, builds et
logs sanitizers `*-release.log`, `bin-release/mc_server`, copies de mondes et
rapports `stairs-import-release`, `placement-bin-release`,
`placement-native-release`, `boundaries-native-release`, `asan-stairs-release`,
`asan-pickup-release`, `vanilla-import-release`, `vanilla-new-release`,
`vanilla-stairs-reference` et `vanilla-boundaries-final`. Les assertions sur
NBT/blocs sont dans `import-preservation-release.json`,
`vanilla-import-blocks-release.json`, `vanilla-new-blocks-release.json` et
`bin-reference-equality-release.json`. `bench-release` contient les 24 passes,
manifestes des templates, actions proposées/confirmées, traces continues,
fsync, ressources, processus hôte et résumés. `analyze.py`,
`benchmark-analysis.json`, `benchmark-extra.json`, `analysis-release.txt` et
`manifest.json` permettent d'auditer les calculs. Les pilotes, erreurs de mise
au point et première campagne restent séparés des résultats finaux.

SHA-256 du binaire normal final effectivement utilisé en gameplay et dans les
24 mesures finales :
`14febb95c81cd402ac1b115f19c873c79f29e66aaa6c99e6b8a6b6d85ba2146b`.
SHA-256 du serveur ASan+UBSan final testé :
`a4b2f1f5ca55d76d87acf67e724d1b340082ade06a369166203c4caf53cfe658`.
S2 n'est pas lié dans `mc_server` (vérification des symboles) et reste un outil
expérimental séparé. Le backend atomique est conservé et `.bin` reste le défaut.
Aucune migration automatique, conversion ni dépendance aux `.bin` dans le
chemin de terrain natif. Aucun déploiement ni push. 25565/25587 : aucune action,
aucun binaire, monde, configuration d'authentification ou droit administrateur
modifié ; leur état distant n'est pas recontrôlé pendant la maintenance.
Les processus temporaires locaux sont arrêtés en fin de validation.

S3A valide la lecture/modification/sauvegarde/relecture native de terrain dans
les scénarios ci-dessus ; il ne valide ni un monde Vanilla complet ni 20 TPS
à 50 bots. S3B devrait élargir le corpus (mondes non plats, NBT opaques, BEs et
protochunks), définir la modification sans perte des données encore refusées
et renforcer le diagnostic d'échecs de fermeture. Le coût CPU de conservation
NBT et l'amplification des reconstructions doivent rester mesurés, sans activer
S2 ni retirer des fsync par défaut. La création des métadonnées globales et la
politique d'adoption doivent rester explicites et non destructives. S4 devra
traiter séparément snapshots/révisions et sauvegardes asynchrones ; aucun de
ces mécanismes n'est anticipé ici. Revalidation d'intégration, durabilité et
performances sur Xeon/HDD RAID nécessaire dès le retour du datacenter, avant
activation réelle ou déploiement.

## Stockage Vanilla S3B — conservation opaque et protections (30 octobre 2026)

### Audit et politique de propriété

Départ : `110a43e317714933c85a77c97ee131d5bc208c3b`, arbre suivi propre ; seuls
`.vscode/`, `src/main.py` et `start-aider.sh` personnels non suivis. Aucun push,
déploiement ou accès aux services principaux. Les tests locaux sont autorisés
pendant la maintenance ; les règles permanentes ne sont pas modifiées.

S3A conservait déjà l'arbre NBT source, mais refusait toute liste de block
entities non vide. Le modèle gameplay reconstruit les conteneurs à partir de
champs partiels : il ne doit pas recevoir les données opaques importées. Sans
protection, ouvrir un coffre pourrait créer un inventaire incomplet ; la
suppression pouvait fermer des vues, préparer des drops et retirer l'entité
avant que le stockage refuse sa sauvegarde. Les contrôles doivent donc précéder
ces effets, pas seulement protéger le writer.

Référence exacte : serveur officiel 26.1.1, protocole 775, DataVersion 4788,
JAR interne SHA-256
`3647564632a2f589be2a87bf544f628e5f67cb3780c86f7fa972ec36892aa081`.
`tools/audit_vanilla_storage.java` initialise ses registres et interroge
`BlockEntityType.isValid`, `PoiTypes.hasPoi` et les statuts de chunks. La fixture
versionnée et `generated_native_be_blocks.h` sont régénérables ; aucune table
de centaines de valeurs n'est déduite d'une ancienne version. Le générateur
exige le SHA du JAR cible lors du renouvellement de l'audit et le precommit
vérifie la table contre la fixture et les index internes du registre C.

L'audit identifie **49 types** officiels de block entities, leurs associations
aux blocs, les blocs POI et **12 statuts** : empty, structure_starts,
structure_references, biomes, noise, surface, carvers, features,
initialize_light, light, spawn, full (préfixe minecraft). Cela valide les
identités et formats, pas les comportements des 49 types.

Les données du chunk natif restent possédées par le snapshot NBT brut. Les
block entities ne sont **jamais** importées dans `mc_block_entity_store` ni
reconstruites via son encodeur. Le chargement valide id, types des coordonnées,
bornes, unicité, correspondance au bloc réel et enveloppe commune des
inventaires : Items = liste de compounds, id = chaîne, count = int positif,
Slot = byte unique, components = compound s'il existe. Les composants et tous
leurs sous-arbres sont conservés, sans conversion ni interprétation gameplay.
Ce contrôle ne prétend pas valider la sémantique de tous les composants, toutes
les capacités d'inventaire ni toutes les données métier arbitraires.

Pour les familles couvertes, un bitmap de **12 288 octets par chunk contenant
des BEs** protège leurs cellules et le voisinage immédiat. La mutation et les
interactions vérifient également les blocs sensibles voisins, y compris dans
un chunk adjacent. Les bordures dont les voisins sont inconnus sont refusées
jusqu'à leur chargement ; aucune hypothèse « voisin absent = air ». Les POI
sont protégés pour éviter une désynchronisation de leurs fichiers associés.
Les noms/index des blocs sensibles viennent de l'audit officiel et une table
constante évite une recherche textuelle par cellule dans le chemin courant.

Le thread de tick conserve la propriété des mutations. Le contrôle central
`mc_world_storage_action_allowed` est utilisé avant les suppressions,
interactions et mutations du monde. Les handlers ajoutent seulement les gardes
nécessaires avant les effets d'inventaire/drop et renvoient l'état autoritaire
avec l'ACK 775 ; ils n'hébergent pas un nouvel ensemble de règles de BEs.
Les sessions de minage refusées sont annulées. La création, modification ou
suppression d'une BE gameplay est refusée en mode natif. Un deuxième contrôle
dans l'encodeur empêche de contourner la protection en modifiant directement
les conteneurs de blocs du chunk avant la sauvegarde.

### Matrice de compatibilité des données

| Catégorie | Lecture | Conservation | Mutation permise | Persistance / validation | Limites et risque |
|---|---|---|---|---|---|
| A — NBT opaque, types/listes/structures imbriquées | Arbre NBT exact, borné | Types, valeurs et ordre des champs conservés | Blocs ordinaires sans relation protégée | Comparaison indépendante avant/après GoatCore | Un NBT qui ne peut pas faire un roundtrip exact est refusé |
| B — Coffre, four froid, panneau, bannière, shulker box, tonneau, pot décoré | Identité/enveloppe validées | Liste complète opaque, inventaires, composants et champs inconnus | Bloc ordinaire éloigné ; BE et voisinage immuables | Scénarios officiels, redémarrages et égalité NBT | Pas de GUI, drops, édition de texte, ouverture, cuisson ou autre simulation nouvelle |
| C — Autres types connus, dont piston/spawner/beehive | Lecture des chunks full | NBT opaque conservé | Chunk protégé en lecture seule | Validation des associations et refus d'écriture | Pas de garantie sur leur fonctionnement ; pas d'activation expérimentale implicite |
| C — Four actif | Lecture des chunks full | Minuteries conservées en Short | Chunk protégé | Contrôle du format officiel et test d'activité | Aucun tick de cuisson simulé |
| C — block_ticks/fluid_ticks non vides | Lecture opaque | Listes intactes | Chunk protégé | Test de refus après lecture | Aucun scheduler ni décalage des dates inventé |
| A/B — Structures, références, biomes | Conservés comme données opaques | Aucun remplacement par des valeurs GoatCore | Édition ordinaire sans modifier BE/POI | Comparaisons NBT et fichiers | Ne valide pas worldgen ni transmission réseau fidèle des biomes |
| B — Heightmaps/lumière | Politique S3A inchangée | Champs opaques conservés ; dérivés connus invalidés | Édition autorisée | Recalcul/réouverture Vanilla | Pas de nouvelle conformité du moteur d'éclairage C |
| C — Protochunks | Inspection NBT hors simulation | Originaux conservés | Aucune promotion/mutation | Statuts officiels réels + refus natif | Pipeline worldgen absent ; aucune régénération masquant le refus |
| C — BE inconnue/malformée ou association incohérente | Refus du chargement actif | Fichiers récupérables préservés | Aucune | Tests négatifs | Pas de reconstruction ou suppression silencieuse |
| C — .mcc/LZ4/custom/inconnu | Index/inspection de référence | Fichiers inchangés | Refus de la région dans le backend actif | Détection et empreintes | Pas de transaction multi-fichiers, pas de décodage .mcc dans GoatCore |
| Métadonnées globales, POI, entités | Pas d'adoption gameplay nouvelle | Fichiers existants inchangés par cette opération | Aucune nouvelle écriture | Empreintes des copies avant/après | Monde Vanilla complet toujours non démontré |

Les sept familles autorisées sont une liste explicite de **conservation**.
Les autres types reconnus deviennent lisibles mais protègent tout leur chunk.
Un four est protégé si sa propriété lit est true ou si lit_time_remaining /
cooking_time_spent indique une activité. Le JAR utilise `getShortOr/putShort`
pour ces minuteries. Un premier contrôle exigeait à tort des Int : il a refusé
le chargement, sans régénérer ni réécrire la région. Les tests sur les vraies
fixtures ont révélé cet écart ; le contrôle final et le test synthétique de
four actif utilisent le type Short exact. Les autres champs restent opaques.

Le backend atomique, sa comparaison de snapshot sous verrou, ses deux fsync,
le résultat de durabilité incertaine et ses garanties précédentes restent en
place. Les tests existants de crash, EINTR, écritures partielles, erreurs de
synchronisation et snapshots périmés sont rejoués. Aucun nouveau worker, aucun
ordonnanceur asynchrone et aucun chemin S2 de gameplay. Une panne physique,
le RAID et les transactions globales inventaires/items restent hors preuve.

### Corpus officiel et fixtures synthétiques

Trois captures indépendantes sont conservées, toutes issues du serveur officiel
26.1.1, seed 0, vue/simulation 4 et mondes jetables. Les BEs de contrôle sont
placées par les commandes du serveur officiel, puis sérialisées par lui ; elles
ne sont pas présentées comme des constructions naturellement générées.

| Capture | Régions | Chunks | Full / protochunks | BEs observées |
|---|---:|---:|---:|---|
| Monde plat + fixtures | 4 | 1 328 | 206 / 1 122 | 7, une par famille couverte |
| Monde plat dense | 4 | 1 327 | 205 / 1 122 | 160 : 154 coffres et les six autres familles |
| Monde normal | 4 | 1 348 | 194 / 1 154 | Les sept fixtures, plus coffres, beehive et mob_spawner naturels |

Soit **12 fichiers de régions / 4 003 chunks** catalogués avec empreintes.
Le corpus normal comprend 8 coffres au total, 5 spawners et une beehive. Les
NBT de débuts de structures identifient cinq mineshafts, une trial chamber et
un shipwreck_beached ; cela ne constitue pas une validation du pipeline worldgen
GoatCore, qui n'est pas développé. Sept statuts réellement présents sont
observés : structure_starts, biomes, noise, surface, carvers,
initialize_light, full. Les protochunks sont inspectés sans être promus.

Une tentative supplémentaire d'élargissement à un village via la commande
Vanilla locate a échoué deux fois avec « An unexpected error occurred trying
to execute that command », sans coordonnées exploitables. Les copies/logs sont
conservés ; ni auteur ni cause profonde n'est attribué. Aucun village ne figure
dans la validation finale et le code de cette tentative n'est pas conservé dans
le runner. Les références presque pleines restent les fixtures **synthétiques**
S1 à 1 024 slots, rejouées par la suite d'index/allocation ; aucune région
presque pleine naturellement générée n'est revendiquée.

`native_corpus.py` catalogue les statuts, types de BEs, codecs, régions et
empreintes sans écrire les mondes. `--compare-entities` compare types et valeurs
NBT, avec sélection des chunks pour distinguer les fixtures des entités
naturelles susceptibles d'évoluer sous Vanilla. Le writer de fixtures est
séparé et impose une copie marquée jetable.

La fixture externe est **synthétique, émise par l'API officielle RegionFile** :
un chunk officiel est enrichi d'un byte array déterministe de 1 200 000 octets,
seed 7754788. Le writer Java 26.1.1 produit un `.mca` de trois secteurs et
`c.-1.1.mcc` (1 202 186 octets), puis son propre lecteur vérifie le tableau
intégral. L'inspection GoatCore reconnaît le stub externe et le chemin attendu,
contrôle existence/type/taille pour diagnostic, mais ne décode pas le payload.
Les copies avec `.mcc` présent ou supprimé sont refusées sans modification du
`.mca` ni des données récupérables. Une vraie prise en charge exige une
transaction multi-fichiers ; elle n'est pas ajoutée.

Les variantes composées des benchmarks réutilisent le chunk dense officiel
dans une copie du monde plat pour stabiliser l'occupation de région. La variante
opaque ajoute explicitement des champs **synthétiques** : byte arrays
déterministes, long, chaîne et liste de compounds, à la racine et dans une BE.
Les composants custom_data des inventaires des fixtures restent, eux, des
valeurs acceptées et sérialisées par le serveur officiel. Les deux provenances
ne sont pas confondues.

### Tests différentiels, refus et non-régression

Le binaire final est testé sur copies plates et normales contenant les sept
fixtures officielles. Un joueur pose réellement une pierre en Survival loin
des BEs ; deux observateurs voient la mutation et une query relit le monde
autoritaire. Puis les requêtes de casse, interaction/ouverture, remplacement
administrateur et retrait du support des BEs sont refusées. L'inventaire du
joueur ne change pas pendant les refus et aucun paquet de création d'entité/drop
n'est apparu. Une pose d'un nouveau coffre est aussi refusée sans consommation.
Le chunk et ses BEs sont relus après **trois redémarrages GoatCore**.

Dans chaque monde, un seul chunk est modifié. Le contrôle indépendant valide
les NBT hors états de blocs/données dérivées : **1 327 chunks sur 1 328 restent
identiques octet par octet** dans la copie plate ; **1 347 sur 1 348** dans la
copie normale. Les BEs du chunk modifié, leurs inventaires, composants, listes,
champs inconnus, biomes, structures et ticks restent conservés. Les fichiers
globaux, POI et entités sont contrôlés séparément ; les fichiers des joueurs
actifs évoluent par le mécanisme déjà existant.

Deux réouvertures officielles suivent chaque scénario. Les sept BEs des
fixtures sont comparées par coordonnées : **types et valeurs NBT exactement
identiques**, y compris les count Int, Slot Byte, minuteries Short du four,
messages du panneau, motifs de bannière et composants custom_data imbriqués
(byte signé -17, IntArray, Long, liste de compounds). Le bloc placé est aussi
vérifié par une commande du serveur officiel, puis par comparaison indépendante
des 98 304 états du chunk modifié. Une ouverture sans erreur ne sert pas seule
de preuve.

Le cas dense subit **100 modifications/sauvegardes**, puis deux ouvertures
Vanilla : les **160 BEs** restent identiques, sans duplication, et les blocs du
chunk correspondent au résultat du backend C. Dans la variante opaque,
GoatCore conserve intégralement les champs synthétiques racine/BE avant
Vanilla. Le sérialiseur officiel élimine certains champs qu'il ne connaît pas :
un témoin de la même variante ouvert directement par Vanilla est donc comparé
au résultat GoatCore puis Vanilla. Les sept BEs finales sont identiques au
témoin. Cela distingue une normalisation officielle d'une perte par GoatCore ;
aucune promesse de conservation de tags arbitraires par Vanilla n'est faite.

**18 refus reproductibles** sont vérifiés sur copies marquées jetables :
BE inconnue, id manquant, coordonnées incohérentes, association id/bloc erronée,
Items mal typé, components mal typé, coordonnées dupliquées, statut proto
synthétique, ticks non vides, four actif Short, NBT invalide, codec custom
inconnu, en-tête pointant dans les secteurs réservés, fichier tronqué,
protochunk officiel, type officiel connu mais non couvert (lecture permise,
mutation refusée), référence externe présente et référence externe manquante.
Les `.mca`/`.mcc` récupérables sont inchangés après chaque refus ; aucun
`chunks/` de fallback n'apparaît. Les variants sont explicitement distingués
des vrais chunks du corpus. Erreurs de sauvegarde, durabilité incertaine et
snapshots obsolètes restent également couverts par les suites C ; une injection
d'EIO du fsync parent avec une BE opaque vérifie le snapshot visible et la
reprise sans destruction de la liste.

`make -B precommit-check` passe : **27 suites C**, générateurs, hygiène, smoke,
multi-epoll, login et tests Python existants. `test_native_storage` atteint
**100 727 vérifications**, dont chaque association officielle type/bloc,
protection des voisinages, minuteries Short, enveloppes, sauvegardes incertaines
et refus. Le stockage historique conserve sa lecture 4785 ; **483 fichiers
`.bin`** d'un scénario placement restent identiques octet par octet à S3A.

Les suites réseau finales repassent **192 observations d'escaliers** contre la
capture officielle existante, puis huit états après trois redémarrages ;
**18 cas placement/dalles/fusions** passent en natif et en `.bin`, avec deux
joueurs, inventaires et redémarrages. Le test shulker BlockEvent reste exécuté
en `.bin` ; il est volontairement omis en natif, où les BEs ne sont pas jouables.
Les collisions et protocoles existants ne sont pas modifiés.

ASan+UBSan : **14 suites C**, sans diagnostic, plus le scénario réseau BE
complet et deux redémarrages sur le serveur instrumenté. TSan local : **six
suites C**, code 0 sans diagnostic (régions, transactions S2, persistance
atomique, cohérence monde, stockage natif, configuration perf). Cela couvre
ces exécutions et ne prouve pas l'absence de toute race du serveur. Le runtime
Xeon historiquement indisponible n'est pas requalifié comme passant. Les
injections d'interruption du backend atomique/S2 sont rejouées, sans coupure
physique ni nouvelle garantie multi-fichiers.

### Coûts locaux des données conservées

NVMe/ext4 local, Ryzen 7 7840HS, O2 et mêmes options. Le probe C est compilé
contre S3A (`110a43e3`) et les sources S3B. Cinq configurations, trois répétitions
alternées, **100 modifications/sauvegardes réellement relues par passe** :
**1 500 sauvegardes**. Dix chargements sont chronométrés séparément par passe.
Les copies, vérifications et créations de marqueurs sont hors chronomètres ;
aucun autre serveur Minecraft ni compilation pendant les appels mesurés.
Le cache système est chaud. Tous les NBT non concernés sont comparés après
chaque passe, pas seulement le point modifié.

L'encodeur est échantillonné séparément avant l'appel de sauvegarde ; celui-ci
inclut sa propre sérialisation et sa durabilité. Ces deux mesures ne doivent
pas être additionnées comme des phases disjointes d'un même appel. Les
percentiles ci-dessous regroupent 30 lectures ou 300 encodages/sauvegardes par
configuration. La même région de départ stabilise le coût des copies ; la
variante opaque agrandit le record. Les jeux d'entrée diffèrent volontairement
par les données de BE/opaque, leur provenance est décrite plus haut.

| Configuration | Chargement P50 / P95 / P99 (ms) | Encodage P50 (ms) | Sauvegarde P50 / P95 / P99 (ms) | CPU sauvegardes, 300 appels (s) | VmHWM max (KiB) |
|---|---|---:|---|---:|---:|
| S3A-zero | 1.164 / 2.469 / 2.585 | 0.615 | 3.322 / 3.738 / 4.180 | 0.499 | 8604 |
| S3B-zero | 1.173 / 2.705 / 3.060 | 0.621 | 3.296 / 3.845 / 4.325 | 0.493 | 8912 |
| S3B-few | 1.292 / 2.610 / 2.707 | 0.735 | 3.450 / 3.843 / 4.179 | 0.545 | 11884 |
| S3B-many | 1.458 / 2.853 / 3.282 | 0.962 | 3.754 / 4.131 / 4.716 | 0.621 | 12032 |
| S3B-opaque | 1.413 / 3.074 / 3.347 | 0.771 | 4.631 / 5.114 / 5.503 | 0.881 | 12228 |

S3A-zero / S3B-zero utilisent un chunk officiel sans BE ; few en contient 7,
many 160 ; opaque ajoute 32 Kio de bytes déterministes à la racine **et** à une
BE, plus Long/chaîne/liste. Le bitmap par chunk BE consomme 12 Kio, indépendamment
de leur nombre. VmHWM est lu dans `/proc/self/status`, propre à l'espace mémoire
après exec ; le premier pilote utilisait ru_maxrss, qui conservait le pic du
parent Python, et n'est pas utilisé pour les chiffres finaux.

Par passe de 100 sauvegardes : **142 540 800 octets** soumis aux écritures pour
les quatre premières configurations, **156 057 600** pour opaque. **200 fsync
de région/parent par passe**, identiques aux barrières atomiques S3A ; les deux
fsync éventuels d'initialisation du marqueur de sélection sont hors appels
mesurés et distingués dans les traces. Les traces séparées MC_SAVE_TRACE_PATH
et LD_PRELOAD enregistrent les écritures et synchronisations ; aucun probe
coûteux n'est activé dans la configuration normale de mc_server.

Le cas sans BE reste proche de S3A (P50 sauvegarde 3,296 contre 3,322 ms) : ce
petit écart n'est pas déclaré comme un gain. Les coûts augmentent avec le nombre
de BEs et les données opaques : validation, arbre NBT et copie/compression
supplémentaires expliquent les écarts mesurés. L'occupation des régions et les
volumes sont explicites. La reconstruction reste entière : le cas many n'écrit
pas seulement la BE ou le bloc changé. Aucun fsync n'est supprimé et aucune
optimisation indépendante n'est introduite.

Il s'agit de coûts de chargement, sérialisation et stockage, **pas d'un nouveau
benchmark de capacité gameplay/TPS**. Le plafond S3A à 50 bots (14,56 TPS `.bin`,
5,79 TPS natif et forte amplification) n'est pas résolu ni masqué. Il doit guider
S4 : snapshots/révisions, coût de conservation NBT et regroupement sûr des
régions, après définition des acquittements. Aucune extrapolation vers le
Xeon/HDD RAID ; nouvelle validation distante obligatoire après maintenance.

### Traçabilité, fichiers et suite du chantier S3B

Résultats bruts conservés dans `tests/stress/results/storage-s3b/` (ignoré par
Git). Le manifeste versionné
`tests/fixtures/vanilla-26.1.1-s3b-corpus.json` contient les empreintes des
12 régions officielles, de la fixture externe synthétique, des rapports
finaux, des journaux de checks/sanitizers et des binaires. Il identifie les
conditions de génération et les captures ; il ne contient aucun JAR distribué
ni monde personnel. La génération multithread officielle peut changer l'ordre
et les empreintes d'une nouvelle capture : les empreintes identifient les
fichiers testés, pas une promesse de génération octet pour octet déterministe.

Les répertoires `validated-*` sont les campagnes finales de gameplay/restarts ;
`bench-complete/` est la campagne finale de coûts. Les pilotes antérieurs sont
conservés séparément. `preservation-*-final.json` vérifie les NBT non concernés ;
`vanilla-entities-*.json` et `vanilla-blocks-*.json` vérifient les réouvertures.
Les commandes reproductibles figurent dans `tests/stress/README.md`.

Binaire normal final testé (compilation O2) :
`tests/stress/results/storage-s3b/validated-bin/mc_server`, SHA-256
`f041766a37cefdd06e0fee34bfdfdba02e73065bc03fb1ec2e84cf9401b54099`.
Le binaire ASan+UBSan est distinct et son empreinte figure dans le manifeste.

Périmètre des fichiers modifiés : structures de chunk et garde de stockage,
décodeur/encodeur natifs, libération du bitmap, interface monde/stockage et
cinq gardes du handler ; fixture/table officielles et générateurs ; tests C,
runner bots, oracle de corpus, probe et campagne de coûts ; Makefile, hygiène,
documentation. Le codec `.bin`, le backend atomique et les transactions S2
ne sont pas modifiés. Aucun nouvel outil d'audit/probe n'est exécuté dans le
serveur normal.

**Services principaux : aucune intervention**, aucun accès distant, aucun
déploiement, aucun redémarrage de 25565/25587. Leur état courant n'a pas été
sondé pendant la maintenance et n'est pas présenté comme vérifié. Les instances
locales jetables sont arrêtées après validation. Le défaut reste `.bin` ;
le mode Anvil reste explicite et expérimental ; aucune migration et aucun
fallback après refus. S2 reste séparé du gameplay.

S3B valide la conservation dans les scénarios ci-dessus, pas les comportements
Minecraft des conteneurs ni la compatibilité mondiale complète. Pour le prochain
lot, traiter d'abord la politique de snapshots et de regroupement durable des
sauvegardes en S4, avec mesures à travail égal et conservation de ces protections.
L'adoption de nouveaux types de BEs exige un corpus officiel et des tests de
relations aux blocs/entités/ticks ; les protochunks et `.mcc` nécessitent des
travaux distincts. Rejouer l'intégration, les interruptions et les performances
sur Xeon/HDD RAID avant toute activation hors copies jetables.

## S4 — Snapshots, sauvegardes groupées et I/O asynchrones (31 octobre 2026)

### Point de départ et périmètre

HEAD initial vérifié : `c2b8824acff47001cd052bda5e1bd963538ba169` (S3B),
arbre suivi propre. `.vscode/`, `src/main.py` et `start-aider.sh` personnels
restent hors du commit. S1/S2/S3A/S3B sont réutilisés. Cette intervention ne
modifie ni le protocole 775, ni les règles des blocs/Survival, ni le scheduler
de simulation. Aucun `simulation_workers`, aucune migration, aucun accès SSH
ou déploiement. Les tests utilisent l'exception locale pendant la maintenance.

Avant S4, `mc_world_flush_block` était une barrière synchrone dans chaque
handler de casse, également utilisée pour les conteneurs et blocs multiparties.
Les poses ordinaires marquaient le chunk dirty ; `mc_world_tick_profiled`
sauvegardait au plus un chunk par passe, puis l'arrêt sauvait les autres un par
un. Chaque sauvegarde native sérialisait le NBT et reconstruisait toute la
région, même si la précédente sauvegarde venait de publier un autre chunk de
la même région. Les workers de chargement possédaient leurs chunks privés ;
les chunks READY et mutations appartenaient au tick.

Le profil P0 (`tests/stress/results/storage-s4/p0-baseline/`) reprend le binaire
S3B immuable et le scénario historique de 100 cycles par bot. Une répétition
1/10/25/50, aucune action manquante selon ses confirmations d'état :

| Bots | Poses / casses | TPS observés | MSPT P99 (ms) | Reconstructions actives | Octets logiques écrits |
|---:|---:|---:|---:|---:|---:|
| 1 | 100 / 100 | 19,993 | 4,723 | 149 | 70 184 960 |
| 10 | 1 000 / 1 000 | 20,059 | 8,901 | 1 170 | 556 773 376 |
| 25 | 2 500 / 2 500 | 19,992 | 40,922 | 2 653 | 1 260 883 968 |
| 50 | 5 000 / 5 000 | 5,543 | 230,999 | 5 139 | 2 447 699 968 |

À 50 bots, `region_total` représente 10,101 s murales / 2,855 s CPU ;
`atomic_file_sync` 4,726 s murales, et les flush des handlers 13,268 s. Ces
spans sont imbriqués et ne s'additionnent pas. `native_chunk_total` et les
coordonnées des traces permettent d'identifier les reconstructions répétées
par région. Sérialisation, copie, barrières et réseau contribuent ; le disque
n'explique pas seul la durée totale. Ce P0 utilise les confirmations d'état
historiques. La campagne finale renforce le client avec les ACK de séquence ;
les gains sont calculés entre versions de cette même campagne finale, pas en
mélangeant leurs méthodes avec P0 ou avec les anciens chiffres Xeon.

### Architecture et activation explicite

Le backend et la politique de sauvegarde sont distincts :

```ini
chunk-storage=bin
anvil-save-mode=immediate
```

C'est le défaut. Le chemin `.bin`, son packing, sa lecture 4785 et ses barrières
antérieures restent utilisés ; la nouvelle fonction de demande appelle la
barrière historique dans ce mode. Pour une copie Anvil jetable seulement :

```ini
chunk-storage=anvil-atomic-experimental
anvil-save-mode=batched
# ou explicitement : anvil-save-mode=async
```

`immediate` reste le comportement natif S3B. `batched` capture et écrit sur le
tick ; `async` capture sur le tick et confie les écritures à **un** worker I/O.
Une politique autre que immediate avec `.bin`, une valeur inconnue ou une
reconfiguration après le début du chargement est refusée. Le marqueur durable
de backend du monde reste inchangé ; aucune bascule ou récupération S2 cachée.
Revenir à immediate exige un redémarrage explicitement configuré, pas une
migration des régions. Le backend S2 n'est toujours pas lié à `mc_server`.

Le pipeline est : action validée → mutation/dirty → demande de persistance →
copie indépendante → regroupement par région → encodage NBT → remplacement
atomique → complétion → acquittement des générations sur le tick.
`play.c` ne contient aucun ordonnanceur : ses appels de persistance passent
par `mc_world_request_block_save`. `mc_world_flush_block` conserve son sens de
barrière, et `mc_world_flush_saves` fournit une barrière de monde chargé. Le
handler administrateur `save-all` appelle cette dernière et distingue succès
durable et échec. Une confirmation de bloc du protocole n'est jamais une
confirmation de sauvegarde disque.

`mc_save_pipeline` regroupe les copies, trie les coordonnées signées de région
et les slots, puis appelle `mc_storage_write_region`. Chaque région est une
transaction indépendante ; il n'y a pas de transaction globale entre régions.
Les contrôles de source de **tous** ses chunks précèdent la publication. Une
source périmée fait échouer tout le groupe de cette région, sans publication
partielle des autres entrées du groupe. Les autres régions d'un batch ont leurs
propres résultats. Le writer générique sert aussi le chemin singleton natif et
les outils atomiques existants.

### Propriété mémoire et générations

`mc_chunk_save_snapshot` produit une copie indépendante des palettes, mots
compactés, NBT source et bitmap de protection. Elle n'emprunte aucun buffer de
chunk vivant, cache réseau, store d'inventaires ou liste du monde. Le NBT source
inclut les biomes, données de génération et sous-arbres opaques préservés en
S3B ; aucun nouveau modèle incomplet de block entities n'est introduit.
La copie est publiée une fois, devient privée au writer, puis revient comme
complétion au tick. Celui-ci transfère son nouveau NBT source visible au chunk
vivant et détruit la copie une seule fois. Les block states vivants ne sont
jamais remplacés par ceux d'une complétion ancienne.

Les champs runtime `save_generation`, `durable_generation`,
`inflight_generation` et `save_pending` distinguent état courant, confirmation
et capture en cours. Les générations ne sont pas des tags Vanilla. Au chargement
natif, une nouvelle époque commence à zéro : les appels aux setters employés
pour décoder les palettes ne comptent pas comme mutations de gameplay. Cette
initialisation n'acquitte aucune opération disque incertaine d'un autre
processus. Un test ajouté pendant l'audit a révélé ce décalage de compteur ;
il est corrigé et rejoué avec le candidat final.

Une mutation après la capture G avance la génération courante ; la confirmation
G conserve alors dirty. Seul un résultat **0** fait avancer la génération
durable. Une erreur la laisse inchangée. Un résultat **-2** transfère le NBT
nouvellement visible pour permettre un retry sûr, mais ne nettoie pas dirty.
Une complétion sans propriétaire/génération correspondante est refusée ; un
flush sans progrès possible échoue au lieu de boucler. Le débordement d'une
génération ou révision est refusé avant de modifier un bloc.

Les sources attendues sont comparées sous le verrou de publication Anvil ;
l'exclusivité du monde, la file unique et l'interdiction de décharger un chunk
pending empêchent les publications hors ordre du pipeline. Le CAS de NBT brut
n'est pas présenté comme un numéro de génération global persistant ou une
protection ABA générale pour des appelants indépendants hors de ce protocole.
Les workers de chargement continuent de produire des chunks privés. Le chemin
existant de normalisation privée conserve ses verrous de région ; il ne reçoit
pas de chunk READY du tick.

### Bornes, saturation et erreurs

Un batch contient au plus **64 snapshots** et **64 Mio de copies**. Une seule
place existe dans la file asynchrone, y compris pour le travail en cours ou
terminé en attente de complétion. Si elle est occupée, aucune nouvelle copie
n'est allouée : les mutations restent dans l'état dirty du monde et coalescent
avant une capture ultérieure. Les limites portent sur la file et ses copies,
pas sur la mémoire totale du serveur, du parseur NBT ou du client Python.
Le worker a également besoin de buffers d'encodage/compression et des métadonnées
de région. Un snapshot trop grand ou un groupe dépassant la limite de NBT est
refusé explicitement ; le mode immediate reste disponible par choix explicite.

La politique groupée programme un flush à la passe monde suivante, sans timer
supplémentaire : normalement un tick de regroupement. L'attente réelle peut
croître si le tick ou le disque est lent. L'asynchrone n'offre pas une borne
de latence murale en cas de blocage du stockage. Les traces enregistrent
snapshots, octets en file, soumissions, complétions et backpressure. Les chunks
dirty/pending sont retenus lors des évictions ; leurs modifications récentes
ne disparaissent pas à l'acquittement d'une copie précédente.

Une erreur du writer est mémorisée, bloque les nouvelles mutations natives et
arrête les retries automatiques par tick. Les données dirty restent présentes
tant que le monde est ouvert ; un arrêt qui échoue signale les données non
sauvegardées, puis termine avec un statut d'erreur.
Une barrière explicite ou l'arrêt tente une reprise contrôlée après drainage,
puis signale l'échec s'il persiste. Aucun rollback destructif, fallback `.bin`,
regénération après une erreur de lecture ou suppression de `.mcc`. Les tests
simulent notamment ENOMEM, compression impossible, EIO, ENOSPC, EDQUOT, EINTR,
écritures courtes, erreurs de synchronisation/rename et sources périmées.

L'arrêt normal rejoint les loaders, intègre leurs derniers résultats, capture
les derniers états dirty, draine les sauvegardes et joint le worker avant de
libérer les chunks ou fermer le stockage. `mc_world_destroy_checked` et
`net_server_destroy_checked` remontent un échec jusqu'au code de sortie du
serveur. Les wrappers historiques void restent disponibles pour les appelants
existants ; ils ne masquent pas les diagnostics. Un échec de join conserve les
ressources que le worker pourrait encore utiliser, plutôt que de créer un UAF.

### Points de durabilité et interruptions

Chaque publication conserve : temporaire unique dans le même répertoire,
écritures vérifiées, fsync du fichier, rename atomique et fsync du parent.
Le fichier existant n'est jamais tronqué. Le regroupement économise les
**publications entières**, pas les barrières nécessaires d'une publication.
Les tables Anvil, timestamps, limites de secteurs et codecs restent Vanilla.
Les autres records, dont ceux des protochunks, sont recopiés sans interprétation.
Les régions externes/non prises en charge restent refusées conformément à S3B.

En mode expérimental groupé/asynchrone, une demande et un ACK réseau peuvent
précéder le flush durable. SIGKILL peut perdre ces changements encore en mémoire,
qui ne sont jamais annoncés durables. Les inventaires joueurs conservent leur
politique distincte : **aucune transaction globale chunks/inventaires/items**
n'est ajoutée. Une interruption dans cette fenêtre peut laisser ces sous-systèmes
décalés, y compris un risque de duplication/perte croisée. Ce lot ne démontre
pas l'exactly-once inter-fichiers après crash. Le mode immediate conserve les
points de synchronisation précédents ; les nouveaux modes restent réservés aux
copies jetables explicitement configurées.

Les tests d'interruption sont des SIGKILL contrôlés et injections système,
**pas des coupures électriques**. Avant rename, l'ancienne région reste intacte.
Après rename, le lecteur trouve une région cohérente ; avant le fsync du parent,
sa durabilité reste incertaine. Après succès des deux barrières, la nouvelle
version est acquittable dans les limites du FS et du matériel. Les temporaires
orphelins ne sont jamais promus automatiquement. Aucune récupération S2 n'est
nécessaire pour ces remplacements atomiques, et aucun journal ambigu n'est effacé.

### Validation fonctionnelle S4 sur le binaire final

Le binaire optimisé figé est
`tests/stress/results/storage-s4/release-bin/mc_server`, SHA-256
`51e05f50a2dc94edff99f472eeb2c960a42610a019966a751dc530cd8af46834`.
`release-production-sources.json` conserve les empreintes des sources de
production associées. Les binaires pilote précédents sont distincts et leurs
mesures ne constituent pas la campagne finale. La référence S3B est le commit
`c2b8824acff47001cd052bda5e1bd963538ba169`, binaire SHA-256
`f041766a37cefdd06e0fee34bfdfdba02e73065bc03fb1ec2e84cf9401b54099`.

Les 22 scénarios de `release_gameplay.py` passent, chacun avec trois
redémarrages propres. Les rapports/logs sont sous `results/storage-s4/release-*`.

| Mode/scénario | Observations réellement vérifiées | Résultat |
|---|---|---|
| `.bin` immediate | 18 cas de placement, orientations, dalles/fusion, refus, inventaire, deux observateurs, reconnexion ; test shulker historique | Réussi |
| Anvil immediate/batched/async | Les mêmes 18 cas ordinaires, plus quatre états aux frontières de régions et coordonnées négatives | Réussi pour chaque mode |
| Escaliers, quatre modes | 192 observations par scénario, formes/voisinage, état autoritaire et relecture | Réussi, référence Vanilla historique conservée |
| Ramassage, quatre modes | 100 objets, inventaire conservé après redémarrage | Réussi ; pas une transaction globale résistante au crash |
| Chunks officiels complexes, trois modes | Sept BE : coffre, four froid, panneau, bannière, shulker, tonneau, pot ; modification ordinaire éloignée | Données/inventaires/types NBT préservés |
| NBT opaque synthétique, async | Champs inconnus imbriqués, types et valeurs ; protection des positions BE | Conservés exactement par GoatCore |
| Serveur ASan/UBSan | `.bin` placement, async escaliers et chunk complexe | Réussi avec redémarrages |

Les suites de frontières et de placement ordinaire sont lancées séparément :
le mode frontières du runner retourne après ses quatre cas et ne couvre pas
implicitement les 18 cas ordinaires. La distance de vue du test de ramassage
reste 10, nécessaire à sa fixture ; une première tentative à 2 n'était pas une
validation et a été remplacée par la campagne correcte.

Les 483 fichiers `.bin` du scénario historique ont aussi été comparés aux
sorties S3B : identité octet par octet (`release-bin-equivalence.json`). Aucun
changement de format ni activation du pipeline natif dans ce mode.

### Relecture par le serveur officiel et conservation S3B

Le JAR serveur officiel interne 26.1.1 utilisé est SHA-256
`3647564632a2f589be2a87bf544f628e5f67cb3780c86f7fa972ec36892aa081`.
Onze scénarios Vanilla passent, chacun ouvert deux fois : trois chunks complexes
selon les politiques immediate/batched/async, une sortie opaque et son témoin
Vanilla sans mutation, puis les six mondes neufs batched/async de placement,
frontières et escaliers. Les mondes neufs de ce dernier groupe utilisent une
composition **jetable** de métadonnées globales officielles et régions GoatCore ;
les entités et POI sans rapport sont retirés de cette copie de test. Il ne
s'agit ni d'une migration, ni d'une preuve de compatibilité globale des mondes.

Les comparaisons indépendantes ne se limitent pas à l'ouverture du serveur :

- 310 chunks neufs sont comparés sur l'intégralité de leurs 98 304 block states
  après réouverture Vanilla : 35 escaliers + 85 frontières + 35 placement,
  pour chacun des deux modes groupés. Aucun état différent.
- Les trois chunks officiels complexes modifiés sont également comparés sur
  tous leurs blocs ; les sept BE gardent leurs types/valeurs/inventaires après
  sauvegarde GoatCore et réouverture Vanilla.
- Les corpus sources contiennent respectivement 1 328 et 1 348 chunks. Chez
  GoatCore, seul le chunk ciblé change ; les NBT bruts des autres chunks et les
  fichiers globaux/entités/POI restent identiques. Les preuves nomment les
  fichiers et les empreintes, sans prétendre que Vanilla ne les réécrit jamais.
- Les champs opaques synthétiques sont conservés exactement par GoatCore.
  Vanilla peut normaliser ou retirer des champs non enregistrés ; la comparaison
  après Vanilla utilise donc un témoin soumis à la même réouverture, pas une
  promesse de conservation arbitraire par Vanilla.

`release_compare.py`, `release-comparisons.log` et les
`release-proof-*.json` gardent les assertions et preuves. Le corpus S3B reste
identifié comme 12 régions officielles/4 003 chunks catalogués ; cela ne signifie
pas que ces 4 003 chunks ont tous été modifiés dans ce lot.

Les quatre cas du corpus de refus rejouent 18 opérations dangereuses, toutes
refusées : BE malformée/inconnue, positions incohérentes, voisinages protégés,
four actif, ticks, protochunk, NBT/codec invalide, externe manquant ou présent.
Les empreintes des originaux et `.mcc` sont inchangées ; aucun fallback `.bin`.
Les fixtures synthétiques restent distinctes des données générées officiellement.
La matrice de propriété et de refus S3B reste applicable : conservation opaque
ne signifie pas simulation des coffres, fours ou autres block entities.

### Tests de générations, erreurs et interruptions

`test_save_pipeline` étend les suites C existantes sans nouveau framework.
Il vérifie palettes et mots copiés, NBT source/guards indépendants, absence de
cache réseau partagé, budget/count/overflow, refus des doublons et sources
périmées. L'encodage du snapshot A est identique avant et après sa sauvegarde
pendant que l'état live B change. L'enveloppe privée de persistance peut recevoir
le nouveau NBT source ; les blocs capturés restent immuables.

Le test bloque réellement le writer dans fsync : le tick continue, modifie B,
conserve le dirty après l'ACK A, diffère l'éviction et recharge finalement B.
Une variante ajoute une mutation C pendant l'écriture B et une erreur du fsync
parent : le résultat -2 ne change pas la génération durable, B reste lisible,
et le retry publie C. Le shutdown draine aussi une tâche acceptée. Allocation,
compression, saturation et erreur persistante conduisent à un échec explicite,
sans boucle infinie ni abandon silencieux des snapshots.

| Checkpoint SIGKILL async (trois chunks d'une région) | Relecture répétée après arrêt |
|---|---|
| Avant capture | Ancien état complet |
| Temporaire écrit, avant fsync fichier | Ancien état complet |
| Fichier synchronisé, avant rename | Ancien état complet |
| Après rename, avant fsync parent | Snapshot A complet visible, durabilité non garantie |
| Après fsync parent, avant ACK tick | Snapshot A complet |
| A durable puis mutation B uniquement en mémoire | A complet ; B n'est jamais annoncé durable |

Chaque cas est relu deux fois, sans mélange entre chunks. Les processus de test
utilisent fork puis exec et des répertoires marqués jetables. Le test atomique
existant est étendu à deux chunks publiés ensemble et un troisième record opaque
inchangé : écritures courtes/nulles, EINTR/EIO/ENOSPC/EDQUOT, erreurs de lecture,
fsync/rename/close/lock et six points d'interruption. Les tests de génération,
capture, regroupement, arrêt et éviction sont complémentaires ; tous les
18 instants conceptuels de P6 ne disposent pas d'un SIGKILL distinct.

Une anomalie de comptabilité a été détectée pendant S4 : les setters employés
au décodage incrémentaient les générations comme des mutations gameplay.
Un chargement natif réussi commence maintenant une nouvelle époque runtime à
zéro ; un test le vérifie. Ce zéro n'acquitte aucune opération incertaine d'un
processus précédent. Les données Vanilla et le format disque restent inchangés.

### Méthode de mesure finale S4

Machine locale autorisée pendant maintenance : Ryzen 7 7840HS, 16 CPU logiques,
Linux 7.0.0-38/glibc 2.43, ext4 sur NVMe. Compilation C11 `-O2` identique pour
les binaires normaux. Un worker réseau, deux loaders de chunks, tick 20 Hz,
distances de vue/simulation 10 ; un seul worker de sauvegarde supplémentaire
dans le mode async. Aucun autre test, serveur de référence ou compilation
concurrent pendant les mesures. Les autres processus de l'utilisateur sont
laissés en place et capturés dans `processes-before.txt` pour chaque essai.
Il ne s'agit pas d'une machine isolée de laboratoire ni du Xeon/HDD distant.

La campagne finale emploie le même client renforcé dans les quatre modes :
`.bin` S4, Anvil S3B, S4 batched et S4 async. À chaque charge, trois répétitions
alternent l'ordre des versions. Chaque bot effectue 100 poses/casses à sa
position privée, attend l'état correspondant **et l'ACK de sa séquence** avant
de poursuivre. Les templates `.bin` et Anvil sont indépendants mais leurs blocs
complets sont comparés avant la charge. Les comptes proposé/reçu/traité/confirmé
sont contrôlés avec les traces client, enqueue et handlers serveur.

La période active entière est conservée ; aucune fenêtre privilégiée de
100 ticks. Les TPS sont le nombre réel de débuts de ticks divisé par la durée
active, sans déduire 20 TPS d'une simple moyenne MSPT. Les ticks qui croisent
les bornes restent inclus dans les distributions. Les passes rapides entre
ticks ont leur résumé séparé ; leurs agrégats de frontière peuvent contenir du
travail adjacent à la période, ce qui est explicitement compté. Les tableaux
de percentiles indiquent la médiane des percentiles des trois essais, pas un
percentile artificiel de leurs trois moyennes.

Préparation, échauffement de 3 s, repos de 3 s avant/après charge, queue,
validation finale et arrêt sont séparés. Une pose de pierre supplémentaire
par bot après la charge laisse un résultat positif à vérifier et est comptée
hors des 100 cycles. Chaque essai doit s'arrêter avec succès, relire ces blocs
depuis le NBT, puis redémarrer **une copie** avec le même binaire/backend et les
requêter par le protocole. La durée de ce redémarrage de contrôle est séparée
de la durée du processus mesuré. Le coût différé jusqu'à l'arrêt et le volume
total écrit ne sont jamais retirés du bilan.

`region_total.bytes` mesure les octets logiquement écrits dans les temporaires
de région, copies/header/records compris ; ce n'est pas une mesure physique du
NVMe. La valeur est sans objet pour `.bin`, qui n'écrit pas de région active.
Les compteurs finaux `/proc/self/io` comprennent aussi fichiers joueurs, logs,
traces, préparation et arrêt. Les deux mesures sont conservées, sans les rendre
interchangeables. Les fsync observés incluent les sauvegardes joueurs distinctes.
Les spans d'encodage, région et flush étant imbriqués, ils ne s'additionnent pas.
Le temps mural moins CPU indique de l'attente/ordonnancement, sans l'attribuer
entièrement au disque. Le pic mémoire est un maximum **observé** des samples et
du VmHWM final ; il n'est pas une preuve d'absence de tout pic bref.

Une première campagne complète a été interrompue par une erreur du collecteur
`/proc` : un processus terminé/zombie n'expose plus toujours `VmRSS`. Aucun
échec serveur n'en découle ; ces essais incomplets restent conservés à part.
Le collecteur distingue maintenant ce cas, sauvegarde ses samples même sur
erreur et possède un test de régression créant un vrai processus zombie.
Les neuf tests Python passent. Les données finales proviennent exclusivement
de `final-bots-complete`, sans fusion avec ces essais ni avec les pilotes P0/P8.

### Résultats finaux de non-régression et sanitizers

La dernière exécution `make -B precommit-check` sur export propre réussit :
**28 suites C**, démarrage isolé, multi-epoll/login, générateurs et Python
(3 tests générateurs, 12 stress, 9 profilage). `final-gate.log` conserve la
sortie complète. Les erreurs EIO/ENOMEM et l'arrêt en échec volontaire présents
dans ce log sont les assertions attendues des tests de panne, pas un échec du
gate. La reconstruction finale produit exactement le SHA-256 du binaire figé
utilisé par les campagnes Minecraft et de performance.

Quinze suites C passent sous **ASan + UBSan**, dont NBT, régions, transactions
S2 isolées, stockage natif, snapshots, monde, registre, palettes, escaliers,
inventaires et codec réseau. Sept passent sous **TSan local** : pipeline,
stockage natif, persistance atomique, index région, transactions S2, cohérence
monde et configuration de performance. Le test de pipeline final est rejoué
sous les deux instrumentations après sa dernière extension : environ 8 950
assertions selon les réveils de condition, dont les six SIGKILL à trois chunks.
Le serveur complet TSan passe aussi le scénario complexe async avec deux bots
et deux redémarrages (`release-tsan-async-complex`). Aucun diagnostic sanitizer
dans ces exécutions. Cela ne prouve pas l'absence de toute race possible et ne
réhabilite pas le runtime TSan du serveur distant, qui n'a pas été testé ici.

Quatre configurations dangereuses sont refusées avant création du monde ou
listener : `.bin` avec async/batched, valeur de politique inconnue, async sans
backend Anvil explicite. Le changement de politique après configuration est
également refusé par le test C. Les droits/authentifications des services
principaux n'ont fait l'objet d'aucune intervention.

### Benchmarks gameplay finaux : 48 essais

Tous les essais confirment exactement **100 cycles par bot**, donc respectivement
100/1 000/2 500/5 000 poses ET autant de destructions. Zéro action manquante,
échec ou déconnexion inattendue. Les comptes enqueue/handler/ACK sont identiques
aux propositions. Chaque essai ajoute hors charge une pose de validation par
bot, vérifie le NBT final et réussit son vrai redémarrage de contrôle.
Les valeurs suivantes sont les médianes et intervalles des trois répétitions.
`Max` est le maximum des essais, `>50` leur nombre total de ticks fixes dépassant
50 ms. Les distributions ne sont pas réduites à une fenêtre de 100 ticks.

| Bots | Mode | TPS médiane [min–max] | MSPT P50/P95/P99 (médianes des essais) | Max ms | >50 ms (somme) |
|---:|---|---:|---:|---:|---:|
| 1 | bin | 20.101 [19.925–20.112] | 0.131/2.971/3.360 | 18.637 | 0 |
| 1 | s3b | 19.976 [19.955–20.052] | 0.091/4.776/11.013 | 35.484 | 0 |
| 1 | batched | 20.004 [19.803–20.078] | 4.035/4.935/13.434 | 40.665 | 0 |
| 1 | async | 20.072 [19.989–20.111] | 0.094/0.218/0.377 | 0.517 | 0 |
| 10 | bin | 20.006 [19.911–20.033] | 4.513/5.794/6.180 | 11.533 | 0 |
| 10 | s3b | 19.958 [19.936–20.021] | 5.785/7.394/22.413 | 25.079 | 0 |
| 10 | batched | 19.935 [19.912–20.021] | 14.779/16.704/21.489 | 33.374 | 0 |
| 10 | async | 19.997 [19.894–20.123] | 3.670/4.529/4.758 | 5.026 | 0 |
| 25 | bin | 19.962 [19.928–19.970] | 10.090/22.364/27.374 | 34.454 | 0 |
| 25 | s3b | 19.975 [19.952–19.982] | 24.459/41.264/49.888 | 60.923 | 5 |
| 25 | batched | 19.952 [19.946–19.955] | 21.113/24.686/25.329 | 33.483 | 0 |
| 25 | async | 20.004 [19.945–20.020] | 10.167/13.487/18.517 | 19.684 | 0 |
| 50 | bin | 15.797 [15.544–16.396] | 44.668/80.369/94.396 | 123.338 | 442 |
| 50 | s3b | 5.172 [4.507–5.530] | 118.822/223.603/260.874 | 307.116 | 460 |
| 50 | batched | 19.158 [18.644–19.586] | 34.434/65.445/93.276 | 122.169 | 176 |
| 50 | async | 19.978 [19.868–19.990] | 15.218/37.528/52.741 | 81.272 | 19 |

Le volume et les synchronisations ci-dessous couvrent le processus entier,
préparation, repos, pose finale et arrêt compris. `0` octet de région pour `.bin`
signifie absence de fichier Anvil actif ; ses écritures figurent dans
`write_bytes`. La durée totale inclut le drainage mais pas le redémarrage de
contrôle indépendant. Les durées actives différentes sont mesurées pour le
même nombre d'actions, sans réduire silencieusement le travail.

| Bots | Mode | Durée active s [min–max] | Durée totale s [min–max] | Reconstructions (médiane) | fsync total (médiane) | Octets régions (médiane) | write_bytes processus (médiane) |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | bin | 4.873 [4.818–5.323] | 19.537 [19.445–20.009] | 0 | 430 | 0 | 864256 |
| 1 | s3b | 5.412 [5.336–5.907] | 20.065 [20.034–20.605] | 146 | 406 | 68771840 | 67272704 |
| 1 | batched | 4.599 [4.595–5.180] | 19.292 [19.257–19.835] | 92 | 298 | 43335680 | 42463232 |
| 1 | async | 4.783 [4.724–4.803] | 19.443 [19.382–19.467] | 96 | 306 | 45219840 | 44310528 |
| 10 | bin | 9.534 [9.497–9.593] | 24.390 [24.389–24.488] | 0 | 2808 | 0 | 6995968 |
| 10 | s3b | 9.340 [8.167–9.932] | 24.231 [23.017–24.796] | 1184 | 2794 | 564387840 | 552275968 |
| 10 | batched | 7.834 [7.692–7.976] | 22.733 [22.639–22.819] | 617 | 1632 | 291303424 | 285609984 |
| 10 | async | 6.585 [6.460–6.601] | 21.437 [21.386–21.490] | 516 | 1406 | 243666944 | 239017984 |
| 25 | bin | 6.473 [6.162–6.560] | 21.660 [21.346–21.792] | 0 | 5736 | 0 | 14729216 |
| 25 | s3b | 9.712 [9.473–9.959] | 24.953 [24.754–25.153] | 2679 | 5894 | 1273499648 | 1245298688 |
| 25 | batched | 5.362 [5.213–5.465] | 20.511 [20.390–20.648] | 429 | 1318 | 202600448 | 199536640 |
| 25 | async | 4.296 [4.262–4.299] | 19.494 [19.464–19.509] | 345 | 1116 | 162861056 | 160628736 |
| 50 | bin | 21.896 [21.460–23.160] | 37.808 [37.361–39.103] | 0 | 11254 | 0 | 29376512 |
| 50 | s3b | 36.738 [36.167–37.797] | 52.876 [52.278–53.879] | 5145 | 10934 | 2450186240 | 2395742208 |
| 50 | batched | 21.393 [21.244–22.688] | 37.382 [37.202–38.662] | 1400 | 3458 | 661102592 | 648744960 |
| 50 | async | 21.693 [20.973–23.362] | 37.620 [36.886–39.367] | 1309 | 3308 | 619851776 | 608505856 |

À 50 bots, les écritures logiques médianes de régions passent de
**2 450 186 240 octets** à **661 102 592** en groupé (-73,02 %) et
**619 851 776** en async (-74,70 %). Les reconstructions passent de 5 145 à
1 400/1 309. Les coûts ne sont pas laissés en attente à la fin de la mesure :
les 48 drains et redémarrages sont vérifiés. Ces politiques expérimentales
publient moins d'états intermédiaires ; elles ne promettent pas la même
confirmation durable individuelle que les anciennes barrières synchrones.

| Bots | Mode | CPU processus total s [min–max] | RSS observé max Kio | CPU client actif % (médiane) | Latence actions P50/P95/P99 ms (médianes) | Arrêt s [min–max] |
|---:|---|---:|---:|---:|---:|---:|
| 1 | bin | 2.523 [2.487–2.528] | 26236 | 2.43 | 2.586/41.907/43.543 | 0.064 [0.032–0.064] |
| 1 | s3b | 2.885 [2.871–2.903] | 31852 | 2.38 | 4.390/42.402/52.110 | 0.114 [0.064–0.114] |
| 1 | batched | 2.779 [2.766–2.807] | 31988 | 2.57 | 0.296/42.031/43.573 | 0.064 [0.032–0.064] |
| 1 | async | 2.796 [2.791–2.825] | 32072 | 2.65 | 0.319/41.766/42.271 | 0.064 [0.064–0.064] |
| 10 | bin | 3.735 [3.718–3.744] | 29616 | 8.69 | 38.274/42.330/44.344 | 0.114 [0.064–0.114] |
| 10 | s3b | 5.371 [5.292–5.377] | 34800 | 8.94 | 38.042/42.259/45.434 | 0.064 [0.064–0.064] |
| 10 | batched | 4.400 [4.369–4.469] | 34832 | 11.53 | 27.244/42.173/53.930 | 0.032 [0.032–0.064] |
| 10 | async | 4.259 [4.195–4.264] | 35104 | 13.51 | 18.765/40.400/42.795 | 0.064 [0.032–0.064] |
| 25 | bin | 6.445 [6.392–6.576] | 34192 | 53.09 | 16.738/32.115/39.540 | 0.064 [0.064–0.064] |
| 25 | s3b | 10.194 [10.075–10.277] | 39092 | 41.32 | 33.397/52.531/61.371 | 0.114 [0.064–0.114] |
| 25 | batched | 6.509 [6.206–6.655] | 39284 | 63.94 | 14.693/25.809/29.041 | 0.064 [0.064–0.064] |
| 25 | async | 6.292 [6.256–6.569] | 40132 | 77.53 | 6.263/14.111/18.410 | 0.064 [0.064–0.064] |
| 50 | bin | 26.724 [25.788–29.129] | 42340 | 95.03 | 57.194/109.102/137.374 | 0.114 [0.114–0.114] |
| 50 | s3b | 46.486 [45.282–46.849] | 46748 | 84.72 | 104.967/301.093/382.697 | 0.114 [0.114–0.114] |
| 50 | batched | 28.088 [27.707–29.981] | 46344 | 98.74 | 47.610/93.703/117.822 | 0.114 [0.114–0.114] |
| 50 | async | 28.945 [27.730–30.755] | 45708 | 99.81 | 37.800/93.220/125.098 | 0.114 [0.114–0.164] |

Le temps d'arrêt observé inclut la granularité d'attente du processus Python ;
ce n'est pas la latence exacte d'un seul fsync. Le span `world_shutdown_save`
et les barrières individuelles restent disponibles dans les traces. Le délai
après la dernière action inclut volontairement le repos de 3 s et la pose de
validation ; il n'est pas entièrement attribué au stockage.

| Bots | Mode | Tick + passes rapides P99 ms (médiane) | Max ms (tous essais) | Snapshots actifs (médiane) | CPU capture ms (médiane) | Queue max/budget observé octets | Backpressure total |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | bin | 11.977 | 22.535 | 0 | 0.000 | 0/0 | 0 |
| 1 | s3b | 16.505 | 38.040 | 0 | 0.000 | 0/0 | 0 |
| 1 | batched | 13.548 | 40.816 | 90 | 0.612 | 1/17480 | 0 |
| 1 | async | 0.477 | 0.684 | 95 | 0.594 | 1/17480 | 0 |
| 10 | bin | 25.472 | 30.818 | 0 | 0.000 | 0/0 | 0 |
| 10 | s3b | 38.372 | 44.591 | 0 | 0.000 | 0/0 | 0 |
| 10 | batched | 22.097 | 34.534 | 613 | 3.769 | 1/69920 | 0 |
| 10 | async | 6.070 | 6.466 | 512 | 3.028 | 1/69920 | 0 |
| 25 | bin | 66.996 | 77.752 | 0 | 0.000 | 0/0 | 0 |
| 25 | s3b | 87.611 | 95.793 | 0 | 0.000 | 0/0 | 0 |
| 25 | batched | 34.237 | 39.313 | 425 | 2.408 | 1/69920 | 0 |
| 25 | async | 26.581 | 29.207 | 341 | 1.816 | 1/69920 | 0 |
| 50 | bin | 141.396 | 162.114 | 0 | 0.000 | 0/0 | 0 |
| 50 | s3b | 360.312 | 399.072 | 0 | 0.000 | 0/0 | 0 |
| 50 | batched | 106.272 | 148.856 | 1396 | 7.865 | 1/69920 | 0 |
| 50 | async | 86.009 | 146.747 | 1304 | 6.983 | 1/69920 | 29 |

La colonne tick + passes rapides est un agrégat par intervalle entre ticks,
avec ses frontières exposées, pas la durée exacte de chaque invocation rapide.
Le `1` de la colonne queue en mode batched désigne un lot synchrone ; il n'y a
pas de thread I/O dans ce mode. En async, les traces confirment une seule place,
69 920 octets au plus observés et 29 passages de backpressure sur les trois
essais à 50 bots. Le test C force aussi la saturation et une écriture bloquée,
contrairement à cette petite occupation en charge réelle.

**20 TPS constants ne sont pas démontrés.** À 50 bots, async varie de
19,868 à 19,990 TPS ; P99 fixe médian 52,741 ms, maximum 81,272 ms et 19 ticks
fixes >50 ms sur trois essais. L'agrégat fixe + passes rapides atteint
146,747 ms. Le groupé synchrone améliore les TPS mais conserve des pauses disque
sur le tick. À faible charge, sa distribution fixe peut être plus coûteuse que
S3B, qui effectuait aussi des flush dans les passes rapides : comparer les deux
mesures plutôt qu'ignorer ce déplacement du travail.

### Profil des coûts et limites du générateur

À 50 bots, médianes de temps muraux actifs, en secondes :

| Span | S3B | S4 groupé | S4 async |
|---|---:|---:|---:|
| Sauvegardes natives complètes sur le tick, ancien chemin | 15,231 | sans objet | sans objet |
| Reconstruction/publication `region_total` | 10,702 | 3,356 | 3,445 sur I/O |
| fsync fichier atomique (y compris autres fichiers) | 4,793 | 1,691 | 1,642 |
| fsync parent atomique (y compris autres fichiers) | 2,973 | 0,816 | 0,814 |
| Copie de région | 0,827 | 0,240 | 0,233 |
| Encodage natif explicite | inclus ancien span | 1,291 | 1,306 sur I/O |
| Phase sauvegarde du monde sur tick | 0,491 | 4,676 | 0,015 |
| Phase items sur tick | 13,220 | 8,255 | 6,157 |
| Handler pose | 6,407 | 4,565 | 6,141 |
| Handler casse | 15,129 | 0,484 | 0,510 |

Les spans se recouvrent : **ne pas additionner les lignes**. Les anciens
handlers incluent leurs propres flush ; la phase monde ne les comptait pas.
Les fsync atomiques comprennent aussi les fichiers joueurs. Les captures
actives async coûtent environ 6,983 ms CPU cumulées pour la médiane de
1 304 snapshots à 50 bots. Les attentes de verrou de région sont faibles dans
ce scénario (environ 24 ms cumulées), sans démontrer qu'elles le seront pour
tous les mondes. Les threads des publications async sont distincts du thread
des handlers dans les 12 essais ; aucun writer n'a accès aux chunks vivants.

La cause principale démontrée est la **répétition de publications complètes**,
puis la sérialisation répétée des générations intermédiaires. Le regroupement
réduit réellement ces travaux ; async déplace ceux qui restent. Le disque ne
suffit pas à expliquer toutes les pauses : les items, sauvegardes joueurs,
validations, buffers et fanout restent sur leurs chemins existants. Le coût du
handler pose n'isole pas à lui seul chaque fonction réseau.

Le client Python atteint environ **99,81 % d'un CPU** actif en async à 50 bots
(95,03 % en `.bin`, 84,72 % en S3B). Les attentes enqueue sont des latences de
plusieurs paquets qui se recouvrent, pas un coût CPU additionnable. Les durées
actives ne croissent pas monotoniquement entre 10 et 25 bots et les latences
présentent des paliers proches de 40 ms : l'ordonnancement du client et les
échanges réseau n'ont pas été isolés de façon causale. Aucune optimisation
réseau spéculative n'est ajoutée. Les essais Creative historiques génèrent
aussi des items via leur chemin de casse préexistant ; les ticks et ramassages
peuvent différer selon le rythme, même avec des actions principales identiques.
Les microbenchmarks suivants isolent le stockage. Aucune parité Creative
complète ni capacité maximale de GoatCore n'est déduite de ces charges.

### Microbenchmarks : neuf cas, trois modes, trois répétitions

Les 81 essais comparent le même outil lié à S3B, S4 singleton et S4 groupé.
Les régions synthétiques sont clairement distinctes des fixtures officielles.
Le travail est une création dans le cas vide, 100 mutations ailleurs. Le mode
serial confirme chaque état intermédiaire ; le groupé confirme les derniers
états seulement. Chaque résultat final est relu ; les blocs sont comparés
entre variantes et les NBT/chunks non concernés préservés. Ce ne sont pas des
latences équivalentes de durabilité de chaque état intermédiaire.

| Cas | Mode | Travail | Durée ms médiane [min–max] | CPU ms médiane | Reconstructions | fsync fichier+parent | Octets régions | RSS max Kio |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| empty | S3B-serial | 1 | 10.478 [5.355–11.063] | 2.785 | 1 | 2 | 20480 | 8288 |
| empty | S4-serial | 1 | 6.346 [4.380–8.653] | 2.894 | 1 | 2 | 20480 | 8280 |
| empty | S4-grouped | 1 | 5.759 [4.964–5.879] | 2.557 | 1 | 2 | 20480 | 8380 |
| 256-same | S3B-serial | 100 | 516.720 [492.986–523.198] | 161.040 | 100 | 200 | 106905600 | 11764 |
| 256-same | S4-serial | 100 | 533.479 [364.116–545.471] | 163.253 | 100 | 200 | 106905600 | 11572 |
| 256-same | S4-grouped | 100 | 5.927 [5.332–6.770] | 1.925 | 1 | 2 | 1069056 | 9772 |
| 256-spread | S3B-serial | 100 | 290.769 [289.574–588.007] | 140.616 | 100 | 200 | 106905600 | 11436 |
| 256-spread | S4-serial | 100 | 584.837 [295.416–640.255] | 176.110 | 100 | 200 | 106905600 | 11836 |
| 256-spread | S4-grouped | 100 | 67.972 [67.405–75.418] | 60.515 | 2 | 4 | 2539520 | 10556 |
| 1024-same | S3B-serial | 100 | 637.376 [615.083–702.603] | 358.943 | 100 | 200 | 421478400 | 15572 |
| 1024-same | S4-serial | 100 | 630.218 [625.169–723.886] | 356.696 | 100 | 200 | 421478400 | 15588 |
| 1024-same | S4-grouped | 100 | 15.600 [14.592–26.071] | 3.723 | 1 | 2 | 4214784 | 13672 |
| 1024-spread | S3B-serial | 100 | 640.305 [628.354–651.311] | 357.774 | 100 | 200 | 421478400 | 15376 |
| 1024-spread | S4-serial | 100 | 638.948 [638.028–643.682] | 353.924 | 100 | 200 | 421478400 | 15360 |
| 1024-spread | S4-grouped | 100 | 75.400 [72.455–84.480] | 64.836 | 2 | 4 | 8830976 | 14376 |
| regions | S3B-serial | 100 | 200.641 [197.311–205.359] | 82.104 | 100 | 200 | 2457600 | 10324 |
| regions | S4-serial | 100 | 197.318 [193.708–202.746] | 81.625 | 100 | 200 | 2457600 | 10464 |
| regions | S4-grouped | 100 | 5.916 [5.795–6.441] | 2.503 | 3 | 6 | 73728 | 8564 |
| opaque-be | S3B-serial | 100 | 358.167 [351.743–359.305] | 186.531 | 100 | 200 | 142540800 | 9428 |
| opaque-be | S4-serial | 100 | 347.320 [345.738–358.665] | 185.477 | 100 | 200 | 142540800 | 9392 |
| opaque-be | S4-grouped | 100 | 4.290 [4.230–4.869] | 2.587 | 1 | 2 | 1425408 | 9572 |
| dense-be | S3B-serial | 100 | 374.427 [368.825–377.181] | 205.916 | 100 | 200 | 142131200 | 9544 |
| dense-be | S4-serial | 100 | 373.812 [368.230–376.004] | 205.467 | 100 | 200 | 142131200 | 9696 |
| dense-be | S4-grouped | 100 | 4.526 [4.340–4.570] | 2.750 | 1 | 2 | 1421312 | 9872 |
| opaque-fields | S3B-serial | 100 | 465.381 [456.611–469.817] | 293.067 | 100 | 200 | 156057600 | 11336 |
| opaque-fields | S4-serial | 100 | 465.209 [463.792–467.648] | 293.837 | 100 | 200 | 156057600 | 11308 |
| opaque-fields | S4-grouped | 100 | 5.204 [5.168–5.880] | 3.615 | 1 | 2 | 1560576 | 9736 |

Les lectures restent séparées des écritures : pour 256-spread, chargement
médian 270,293 ms S3B / 278,969 ms S4 groupé ; pour 1024-spread,
1 435,494 / 1 493,096 ms. Ce lot ne revendique pas d'amélioration de lecture.
Les traces conservent les latences de chaque publication, CPU, compression,
encodage, copie et synchronisations ; trois durées globales ne suffisent pas
à établir un P99 statistiquement robuste d'une campagne complète.

Pour 100 chunks distincts, 106 905 600 octets deviennent 2 539 520 dans la région
de 256 chunks, et 421 478 400 deviennent 8 830 976 dans celle de 1 024 chunks.
Le pipeline respecte sa borne de 64 : deux publications/quatre fsync. L'API
région bas niveau sait aussi publier les 100 chunks en une opération, vérifiée
par le test C sur 256 et 1 024 emplacements. Pour 100 mutations du même chunk,
une seule dernière génération suffit : 1 publication/2 fsync au lieu de 100/200.

Le nouveau chemin **singleton n'est pas une optimisation générale**. En
256-spread, sa médiane 584,837 ms contre 290,769 ms S3B est défavorable, avec
intervalles [295,416–640,255] et [289,574–588,007] ms. La variance NVMe/hôte
n'est pas causalement isolée ; les CPU et données brutes sont conservés. Aucun
gain de ce chemin n'est annoncé ni une seconde optimisation ajoutée pour
masquer ce résultat. Les modes groupés et les cas 1024 ont leurs propres
résultats, sans sélection des meilleures répétitions.

Le premier passage complexe du collecteur avait inclus le fsync initial du
marqueur backend : l'assertion comptait 101 barrières fichier pour 100 régions.
Le résultat de sauvegarde et sa relecture étaient corrects. Le collecteur
mesure désormais la fenêtre des publications, avec un test Python dédié.
Les 54 cas synthétiques valides sont conservés dans `final-micro`, les 27 cas
complexes refaits dans `final-micro-complex` ; aucune mesure défaillante n'est
fusionnée. L'analyse finale revalide les compteurs depuis les CSV des 81 essais.

Les sorties groupées à 7 BE, **160 BE** et champs opaques sont chacune rouvertes
deux fois par Vanilla. Les trois chunks modifiés sont comparés sur tous leurs
blocs ; les types/valeurs NBT et inventaires sont conservés. Pour les champs
non enregistrés, le même témoin Vanilla que précédemment est utilisé.
`final-micro-vanilla-proof.json` conserve les comparaisons indépendantes.

### S2 : évaluation secondaire isolée

S2 n'a subi aucun changement et n'est pas lié au serveur. Le runner existant
`profile_region_txn.py` compare 18 passes sur régions synthétiques de 256 chunks,
100 transactions chacune, trois motifs/trois répétitions. Six passes ptrace
séparées comptent les syscalls ; leurs durées perturbées ne sont pas utilisées
pour les latences. Les charges/payloads diffèrent de ceux des microbenchmarks
natifs : ne pas comparer leurs durées directement entre tableaux.

| S2 isolé, 256 chunks | Backend | Durée totale ms médiane [min–max] | CPU ms | Latence P50/P95/P99 ms (médianes) | fsync /100 transactions | Octets write/pwrite tracés |
|---|---|---:|---:|---:|---:|---:|
| single | atomic | 233.894 [226.759–236.699] | 64.546 | 2.263/2.573/2.895 | 200 | 107315200 |
| single | s2 | 436.711 [407.017–452.290] | 81.389 | 4.297/4.956/5.061 | 700 | 3927200 |
| spread | atomic | 224.494 [221.186–225.040] | 62.142 | 2.211/2.461/2.528 | 200 | 107315200 |
| spread | s2 | 442.009 [438.723–508.206] | 83.574 | 4.382/4.835/4.896 | 700 | 3927200 |
| regions | atomic | 235.202 [229.393–243.475] | 69.793 | 2.331/2.743/2.779 | 200 | 107315200 |
| regions | s2 | 437.131 [435.930–447.663] | 86.956 | 4.322/4.763/5.134 | 700 | 3927200 |

S2 réduit ici le volume tracé d'environ 96,34 %, mais reste environ deux fois
plus lent et effectue 700 fsync contre 200. Aucune barrière n'est supprimée.
Neuf interruptions supplémentaires (en-tête partiel ou transaction validée)
sont récupérées sur leurs inodes originaux, puis récupérées à nouveau : mêmes
NBT attendus, aucune corruption. Récupération mesurée entre 2,956 et 4,486 ms.
Les journaux ne sont jamais présentés directement à Vanilla sans récupération.
Aucune activation gameplay ni modification du protocole transactionnel S2.

### Charge bornée, reconnexion et fichiers de preuve

Le test async de 10 bots ×1 000 cycles confirme **10 000 poses et 10 000 casses**
en 67,094 s actives : 1 342 ticks, 20,002 TPS observés, MSPT P50/P95/P99
3,541/4,643/4,999 ms, maximum 5,965 ms et aucun tick fixe >50 ms. RSS maximal
observé 35 636 Kio ; `write_bytes` total 2 457 403 392 octets. La pose finale,
le drainage, la relecture NBT et le redémarrage passent. C'est une charge bornée
d'environ une minute active, **pas un test d'endurance de plusieurs heures** ni
une preuve générale d'absence de fuite. La réécriture de régions reste coûteuse
sur une charge soutenue et devra être mesurée sur HDD.

Le test de cycle de vie supplémentaire passe neuf déconnexions brutales
simultanées avec observation des retraits et reconnexion immédiate, puis dix
abandons pendant le streaming des chunks et reconnexion. L'instance Anvil
async s'arrête avec succès. Toutes les instances temporaires sont arrêtées.
Les copies de mondes/traces utiles sont conservées dans les résultats ignorés,
jamais intégrées en masse dans Git.

`tests/fixtures/vanilla-26.1.1-s4-validation.json` décrit les empreintes,
scénarios et résultats finaux. Les résultats bruts sont sous
`tests/stress/results/storage-s4/` : `final-bots-complete`, `final-derived.json`,
`final-micro*`, `final-s2`, `final-soak`, `final-lifecycle`, `final-gate.log`,
`release-proof-*`, rapports Vanilla et sanitizers. Les scripts/probes
reproductibles sont documentés dans le README de stress. Les essais pilotes
et erreurs de collecteur restent identifiés séparément.

Fichiers de code : nouvelle couche `mc_save_pipeline.h`/`save_pipeline.c` ;
interfaces et implémentations chunk, world, world_storage, Anvil ; configuration
`main.c`, initialisation/arrêt `net/server.c`, appels de persistance et barrière
administrateur dans `play.c`. Le Makefile lie le module et ajoute sa suite.
Tests/outils : `test_save_pipeline.c`, `test_atomic_persistence.c`,
`mc_native_bench.c`, `profile_save_batches.py`, `profile_tick.py`,
`test_tick_profile.py`, sonde `profile_fsync.c`, option de runner
`run_block_checks.py`. Documentation : présent rapport, audit de concurrence,
README et manifeste de validation. Aucun changement de logique des blocs.

### Conditions avant généralisation et prochaine intervention

Les modes groupé/asynchrone restent explicitement expérimentaux. Les limites
S3B sur ticks, fluides, éclairage dérivé, protochunks, `.mcc`, codecs et BE non
simulées restent en place. Aucun changement de règle de placement, simulation
parallèle, worker de région ou mécanisme de migration n'est livré. Une région
entière reste recopiée à chaque publication : le regroupement réduit la
fréquence, pas le coût asymptotique d'une publication. Un lot de 100 chunks
distincts dépasse la borne de 64 et nécessite au moins deux publications.

Avant activation hors copies jetables :

1. Reproduire la campagne sur Xeon/HDD RAID après maintenance, avec les binaires
   et sources figés, mêmes options et quantités de travail. Les deux services
   principaux doivent rester hors de la campagne ; employer des instances et
   ports temporaires, sans Git ni migration sur la machine distante.
2. Mesurer CPU/cpuset, charge des autres processus, mémoire, espace libre et
   configuration réelle. Utiliser les scripts existants de transfert/compilation
   après inspection, puis le protocole `profile_tick.py` documenté dans le README
   de stress : 1/10/25/50 bots, 100 cycles, trois répétitions alternées, mêmes
   workers et distances. Séparer préparation, charge, drainage, arrêt et relecture.
3. Conserver les CSV complets, empreintes et journaux, vérifier les comptes
   proposé/reçu/confirmé et les états non vides après redémarrage. Mesurer latences
   fichier/répertoire fsync, volume logique et `/proc`/outils hôte, queue,
   snapshots et durée d'arrêt. Ajouter `iostat`/pression I/O si disponibles,
   sans les confondre avec du CPU. Ne comparer que des charges équivalentes.
4. Rejouer sur copies les erreurs/interruption et sanitizers compatibles avec
   l'hôte. Ne pas annoncer TSan fonctionnel si son runtime échoue avant les tests.
   Une panne physique et les caches du RAID restent un domaine de validation
   distinct ; ne jamais couper volontairement le matériel de production.
5. Définir une politique d'autosave et de durabilité croisée avant de promettre
   des inventaires/items exactement cohérents après crash. Le présent pipeline
   ne fournit pas de transaction globale. Mesurer aussi un disque réellement
   lent et une durée de drainage bornée par des critères opérationnels, sans
   annuler un writer actif de façon dangereuse.

S3C nécessite encore un inventaire des mondes `.bin`/DataVersion 4785, validation
complète des identifiants 4788, une stratégie de sauvegarde hors place, manifeste
et comparaison des chunks/inventaires/données opaques, refus des cas ambigus,
espace disque prévu, réouverture Vanilla et rollback par conservation des
originaux. Aucun convertisseur, déplacement de `.bin` ou mélange de backends
n'est introduit ici. Compatibilité Anvil ne signifie toujours pas monde Vanilla
complet : entités, POI, fichiers globaux, biomes et moteur d'éclairage exigent
leurs validations propres.

S5 devrait d'abord étudier les goulets d'étranglement restant mesurés, le budget
de capture sur tick et la politique d'autosave, avant de complexifier les files
ou de promouvoir S2. La reprise des blocs de phase 2 pourra ensuite conserver
ces interfaces sans ajouter la logique de stockage au handler réseau. Les
services 25565/25587 n'ont été ni interrogés ni modifiés pendant la maintenance ;
leur état effectif n'est donc pas réaffirmé par ce rapport.

## S5 — Validation HDD RAID et politique de durabilité (31 octobre 2026)

### A. État initial

Départ vérifié : `1f05f9532cdbe1374761695790d4d1a68e014048`, arbre suivi
propre. Les trois éléments personnels `.vscode/`, `src/main.py`, `start-aider.sh`
restent non suivis et préservés. S1/S2/S3A/S3B/S4 ne sont pas réimplémentés.
Le stockage historique et les politiques expérimentales S4 restent disponibles.
S5 ne change ni protocole, ni gameplay, ni défaut de backend, ni nombre de
workers ; aucune migration et aucune activation S2.

### B. Environnement et accès distant

Deux connexions SSH de diagnostic espacées de plus d'une heure, BatchMode,
ConnectTimeout 6 secondes,
ConnectionAttempts 1, vers l'utilisateur/hôte des scripts existants échouent avec
`Connection timed out`, code 255. Les commandes distantes prévues étaient
uniquement des lectures. Aucun démarrage, redémarrage, transfert ou déploiement
n'a lieu. Les services 25565/25587 ne sont donc pas vérifiables dans ce lot.
Leur ancien état n'est pas présenté comme actuel. **La validation Xeon/HDD RAID
reste bloquée par l'accès distant** ; les exécutions utilisent l'autorisation
locale explicite de S5, sur copies jetables et ports temporaires.

Le PC de test reste Ryzen 7 7840HS, 16 CPU logiques, ext4/NVMe, Linux 7.0.
Les valeurs S4 et ses limites restent historiques, dans la section précédente.
La nouvelle instrumentation compte les `write`/`pwrite` du monde et leurs retours,
en plus des fsync, traces de tick et compteurs `/proc`. Elle est préchargée
uniquement dans les processus de test ; aucun hook de panne n'entre dans
`mc_server`. Le binaire runtime S4 reste inchangé : aucune correction de production n'est
justifiée par les défauts et mesures de cette intervention.

### C. Performances locales et limites de mesure

Les quatre politiques utilisent le **même binaire S4**, recompilé à l'identique,
SHA-256 `51e05f50a2dc94edff99f472eeb2c960a42610a019966a751dc530cd8af46834`.
Aucune correction de production n'est revendiquée. La comparaison est celle
des politiques actives, pas un gain de S5 contre S4. Mondes neufs équivalents,
view/simulation distance 10, workers identiques, 100 cycles par bot,
1/10/25/50 bots × quatre modes × trois répétitions, ordre alterné. Préparation,
échauffement de 3 s, repos de 3 s, charge intégrale, repos final, pose de témoins,
barrière, arrêt puis redémarrage contrôlé sont séparés.

Les **48 essais confirment 103 200 poses et 103 200 casses**, avec autant de
propositions, d'entrées en file et de traitements des handlers ; état et ACK
sont contrôlés, aucune action manquante. Les poses finales supplémentaires ne
sont pas comptées comme cycles de charge. Le temps total inclut le dernier
checkpoint et l'arrêt ; la vérification après redémarrage est mesurée à part.
Les actions ne sont pas déclarées terminées durablement sur leur seul ACK.

Un premier passage de 48 essais (`bench/`) a révélé un **effet de sonde** :
classer chaque `write` par `/proc/self/fd` instrumentait aussi des millions
de réveils eventfd réseau. Ces données sont conservées comme pilote intrusif,
**exclues des résultats représentatifs**. La sonde de test mémorise désormais
uniquement les eventfd connus et invalide ce classement lors de close,
remplacement et réutilisation par open/openat. Un test réel vérifie aussi
réutilisation après fermeture par syscall et dup2. Aucun cache de classement
des fichiers du monde ni changement du réseau GoatCore. Les 48 essais ont
été intégralement rejoués dans `bench-final/` avec la nouvelle sonde ; les
compteurs CSV et atomiques de sortie coïncident, sans événement perdu.

Un contrôle 50 bots ×100 cycles avec la sonde corrigée atteint 19,985 TPS,
contre le ralentissement artificiel du pilote. Cela mesure une correction
de l'observateur, **aucune optimisation du serveur**. Trois autres contrôles
async sans comptage détaillé des write/pwrite conservent les traces fsync/tick ;
ils servent à quantifier le coût résiduel, pas à choisir un meilleur essai.

Les TPS sont le nombre de ticks commencés divisé par toute la fenêtre active.
La table suivante décrit les ticks programmés ; les passes rapides entre ticks
sont comptabilisées séparément dans la seconde mesure de budget. Des TPS
légèrement supérieurs à 20 sur une fenêtre finie ne sont pas un nouveau rythme
cible. Toutes les répétitions et leurs dispersions sont conservées.

| Bots | Mode | TPS médiane [min–max] | MSPT P50/P95/P99 (médianes des essais) | Max ms | >50 ms (somme) |
|---:|---|---:|---:|---:|---:|
| 1 | bin | 19.978 [19.938–20.115] | 0.076/3.542/35.331 | 37.097 | 0 |
| 1 | immediate | 19.997 [19.961–20.135] | 0.085/5.126/36.369 | 39.405 | 0 |
| 1 | batched | 20.019 [19.954–20.036] | 3.677/5.311/12.080 | 12.766 | 0 |
| 1 | async | 20.009 [19.887–20.020] | 0.062/0.121/0.234 | 0.291 | 0 |
| 10 | bin | 19.982 [19.925–19.996] | 3.583/4.648/5.082 | 10.030 | 0 |
| 10 | immediate | 19.996 [19.929–20.094] | 4.675/5.784/7.973 | 14.345 | 0 |
| 10 | batched | 20.084 [19.981–20.085] | 12.708/14.085/20.017 | 23.843 | 0 |
| 10 | async | 20.000 [19.980–20.089] | 3.128/3.719/3.974 | 7.848 | 0 |
| 25 | bin | 19.948 [19.941–20.032] | 8.983/20.340/23.953 | 35.467 | 0 |
| 25 | immediate | 20.035 [20.010–20.048] | 17.875/33.689/40.836 | 48.676 | 0 |
| 25 | batched | 19.967 [19.898–20.030] | 17.524/20.736/30.054 | 32.935 | 0 |
| 25 | async | 20.001 [19.966–20.105] | 8.495/12.291/13.097 | 14.802 | 0 |
| 50 | bin | 10.966 [8.519–13.802] | 56.674/122.994/145.975 | 176.812 | 490 |
| 50 | immediate | 5.267 [5.061–5.645] | 106.387/176.536/216.481 | 229.335 | 412 |
| 50 | batched | 19.748 [10.887–19.980] | 31.930/56.795/78.375 | 208.616 | 234 |
| 50 | async | 19.972 [19.859–20.020] | 19.380/38.629/52.269 | 71.612 | 15 |

Les cycles incluant les passes rapides donnent à 50 bots des P99 médians de
210,615 / 290,175 / 94,194 / 75,653 ms (bin/immediate/batched/async), avec des
maxima respectifs 282,714 / 351,610 / 225,012 / 118,451 ms. **20 TPS constants
ne sont donc pas démontrés**, même lorsque le débit médian async approche 20.
À 25 bots aussi, les passes rapides peuvent excéder 50 ms alors que les ticks
programmés seuls respectent le budget.

| Bots | Mode | Durée active s [min–max] | Durée totale s [min–max] | Reconstructions (médiane) | fsync total (médiane) | Octets régions (médiane) | write_bytes processus (médiane) |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | bin | 4.915 [4.155–4.922] | 19.632 [18.896–19.659] | 0 | 418 | 0 | 856064 |
| 1 | immediate | 5.066 [5.001–5.260] | 19.880 [19.684–20.005] | 145 | 404 | 68300800 | 67170304 |
| 1 | batched | 3.658 [3.643–4.346] | 18.404 [18.388–19.036] | 74 | 262 | 34856960 | 34357248 |
| 1 | async | 3.946 [3.922–3.998] | 18.682 [18.635–18.736] | 80 | 274 | 37683200 | 37130240 |
| 10 | bin | 8.833 [8.602–9.058] | 23.713 [23.490–23.934] | 0 | 2752 | 0 | 7081984 |
| 10 | immediate | 9.406 [9.202–9.433] | 24.310 [24.083–24.326] | 1177 | 2734 | 559579136 | 550502400 |
| 10 | batched | 7.817 [7.817–8.158] | 22.703 [22.645–23.001] | 625 | 1632 | 295206912 | 290951168 |
| 10 | async | 6.969 [6.950–7.558] | 21.899 [21.835–22.481] | 551 | 1476 | 260071424 | 256405504 |
| 25 | bin | 5.617 [5.615–5.791] | 20.848 [20.746–20.982] | 0 | 5682 | 0 | 15060992 |
| 25 | immediate | 8.046 [7.836–8.081] | 23.181 [23.026–23.277] | 2654 | 5806 | 1261142016 | 1240137728 |
| 25 | batched | 4.558 [4.523–4.843] | 19.741 [19.688–19.983] | 368 | 1172 | 173719552 | 172195840 |
| 25 | async | 3.830 [3.800–3.856] | 19.046 [18.993–19.083] | 309 | 1026 | 145911808 | 144814080 |
| 50 | bin | 24.165 [21.156–26.882] | 40.218 [37.103–43.081] | 0 | 11078 | 0 | 29868032 |
| 50 | immediate | 28.851 [26.013–29.053] | 45.021 [42.152–45.167] | 5123 | 10892 | 2439847936 | 2398920704 |
| 50 | batched | 18.969 [18.838–27.464] | 34.839 [34.717–43.592] | 1309 | 3272 | 618504192 | 610373632 |
| 50 | async | 19.034 [18.125–19.531] | 35.005 [33.982–35.439] | 1277 | 3218 | 602808320 | 594898944 |

À 50 bots, les write/pwrite interceptés du monde acceptent respectivement
2,103 / 2 439,992 / 618,652 / 602,958 Mo (médianes), soit 5 475 / 58 153 /
15 009 / 14 640 appels. Les octets logiques de région, les retours des appels
et `write_bytes` processus sont des métriques différentes : ce dernier inclut
logs/traces et autres fichiers, le cache peut fusionner des écritures. Aucune
mesure ici ne décrit les plateaux du RAID distant. Toutes les synchronisations
observées de la campagne réussissent ; leurs nombres incluent aussi joueurs
et fichiers hors régions, sans assimiler cela à deux fsync par mutation.

Le regroupement et async réduisent ici le volume logique de régions d'environ
75 % face à immediate à 50 bots. Cela confirme l'effet de S4 sur ces scénarios ;
S5 ne l'attribue pas à une modification nouvelle. La durée active varie aussi
avec le coût de simulation et le générateur ; comparer les mêmes 5 000 cycles,
pas seulement le taux d'actions par seconde ou une fenêtre favorable.

| Bots | Mode | CPU processus total s [min–max] | RSS observé max Kio | CPU client actif % (médiane) | Latence actions P50/P95/P99 ms (médianes) | Arrêt s [min–max] |
|---:|---|---:|---:|---:|---:|---:|
| 1 | bin | 2.065 [2.049–2.079] | 26184 | 1.88 | 2.370/41.687/47.281 | 0.064 [0.064–0.114] |
| 1 | immediate | 2.352 [2.330–2.365] | 31372 | 1.79 | 3.584/42.047/74.845 | 0.065 [0.064–0.115] |
| 1 | batched | 2.255 [2.218–2.263] | 31504 | 2.27 | 0.308/41.194/42.108 | 0.064 [0.064–0.064] |
| 1 | async | 2.260 [2.245–2.263] | 31992 | 2.12 | 0.292/41.173/41.421 | 0.064 [0.032–0.114] |
| 10 | bin | 2.842 [2.838–2.843] | 28856 | 6.79 | 38.308/42.246/43.589 | 0.064 [0.064–0.114] |
| 10 | immediate | 4.132 [4.085–4.158] | 34336 | 6.41 | 38.406/43.226/44.881 | 0.064 [0.064–0.064] |
| 10 | batched | 3.440 [3.419–3.473] | 34500 | 7.70 | 30.186/42.099/45.108 | 0.032 [0.032–0.064] |
| 10 | async | 3.350 [3.345–3.364] | 35028 | 8.85 | 23.810/40.891/41.783 | 0.064 [0.032–0.114] |
| 25 | bin | 4.936 [4.934–4.957] | 33828 | 45.30 | 13.825/27.219/36.419 | 0.064 [0.064–0.114] |
| 25 | immediate | 7.560 [7.473–7.604] | 38804 | 35.97 | 26.040/44.291/53.070 | 0.064 [0.064–0.064] |
| 25 | batched | 4.802 [4.522–4.868] | 38824 | 53.64 | 9.834/22.952/26.510 | 0.064 [0.064–0.064] |
| 25 | async | 4.693 [4.674–4.769] | 37712 | 63.76 | 5.023/12.158/14.877 | 0.114 [0.064–0.114] |
| 50 | bin | 30.633 [26.088–34.965] | 41624 | 94.75 | 60.408/141.586/175.538 | 0.114 [0.114–0.114] |
| 50 | immediate | 35.143 [31.903–35.841] | 45884 | 80.10 | 89.586/214.734/270.428 | 0.114 [0.114–0.114] |
| 50 | batched | 25.325 [25.078–37.636] | 45788 | 98.77 | 43.626/85.822/105.815 | 0.114 [0.114–0.115] |
| 50 | async | 26.044 [24.400–26.363] | 47236 | 99.90 | 32.162/76.799/96.161 | 0.114 [0.064–0.114] |

#### Profil restant et stabilité des mesures

À 50 bots, le pilote Python consomme environ un cœur (médiane async 99,90 %).
Les propositions, files, handlers, états et ACK sont comptés séparément ; aucun
travail n'est abandonné pour relever les TPS. La cadence proposée peut pourtant
limiter la charge. Le client et le fanout réseau ne sont pas totalement isolés ;
aucune capacité maximale serveur ne se déduit de ces essais seuls.

Le tick async-50-r3 passe 6,774 s dans la passe items et 4,604 s dans les passes
rapides ; ses handlers totalisent 4,826 s. Ces périmètres se recouvrent : ne pas
les additionner comme un profil exclusif. Le pic d'items est 4 416 pendant la
charge, les tâches atteignent 54 en file et 29 ms d'attente maximale dans cet
essai. Le processus effectue encore des millions de petits appels, dont les
réveils réseau ; leur ventilation CPU exacte nécessite un prochain profil.
Les coûts items/traitement restent significatifs lorsque le disque est retiré
 du tick, sans justifier ici de refonte réseau ou gameplay.

Dans le même essai, le worker totalise 3,365 s de sauvegarde native dont
0,824 s de préparation NBT, 2,534 s de publication de régions, 1,200 s de fsync
fichier et 0,704 s de fsync parent. Compression 55 ms, copie des régions 202 ms,
attente du verrou 14 ms ; ces sous-périmètres se recouvrent également. Les
captures coûtent 3,559 ms CPU au total : ce n'est pas le goulet dominant observé.
Les 2,534 s de publication incluent seulement 0,753 s CPU : une grande part
est une attente, sans l'assimiler à une mesure physique HDD.

Le mode groupé garde ces publications sur le tick. Son essai 50-r2 ralentit
à 10,887 TPS malgré moins de reconstructions : fsync fichier actif 1,979 s
contre 1,261 s pour r1, CPU système total 26,419 s contre 16,188 s et 11,22
contre 6,79 millions d'appels write processus. Cela ne démontre pas une cause
unique. Les applications personnelles et la charge du PC sont enregistrées
sans être arrêtées ; ni interférence externe précise ni défaut S4 ne sont
attribués sans profil supplémentaire. La dispersion demeure dans la médiane
et l'intervalle, sans sélection du meilleur résultat.

| Bots | Mode | Tick + passes rapides P99 ms (médiane) | Max ms (tous essais) | Snapshots actifs (médiane) | CPU capture ms (médiane) | Queue max/budget observé octets | Backpressure total |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | bin | 36.947 | 37.228 | 0 | 0.000 | 0/0 | 0 |
| 1 | immediate | 37.774 | 39.423 | 0 | 0.000 | 0/0 | 0 |
| 1 | batched | 12.180 | 12.895 | 73 | 0.338 | 1/17480 | 0 |
| 1 | async | 0.375 | 0.391 | 79 | 0.326 | 1/17480 | 0 |
| 10 | bin | 21.369 | 28.036 | 0 | 0.000 | 0/0 | 0 |
| 10 | immediate | 33.036 | 37.433 | 0 | 0.000 | 0/0 | 0 |
| 10 | batched | 20.473 | 24.277 | 621 | 2.252 | 1/69920 | 0 |
| 10 | async | 4.763 | 8.595 | 547 | 1.919 | 1/69920 | 0 |
| 25 | bin | 60.190 | 68.427 | 0 | 0.000 | 0/0 | 0 |
| 25 | immediate | 78.439 | 90.445 | 0 | 0.000 | 0/0 | 0 |
| 25 | batched | 33.531 | 35.718 | 364 | 1.074 | 1/69920 | 0 |
| 25 | async | 22.354 | 23.348 | 305 | 0.904 | 1/69920 | 0 |
| 50 | bin | 210.615 | 282.714 | 0 | 0.000 | 0/0 | 0 |
| 50 | immediate | 290.175 | 351.610 | 0 | 0.000 | 0/0 | 0 |
| 50 | batched | 94.194 | 225.012 | 1304 | 3.913 | 1/69920 | 0 |
| 50 | async | 75.653 | 118.451 | 1273 | 3.793 | 1/69920 | 6 |


Le slot reste à un lot ; le maximum observé de copies acceptées est 69 920
octets dans ces charges, bien inférieur à la borne de 64 Mio/64 snapshots.
Six événements de backpressure async apparaissent à 50 bots, sans perte dirty.
Le délai submit→complétion traitée atteint des P99 par essai de 79,305 à
89,554 ms à 50 bots : c'est l'acquittement par le propriétaire, pas le temps
CPU du writer. Les barrières finales de ces essais async durent 128 à 155 ms ;
le shutdown mesuré est de 64 à 114 ms. Ces petites files NVMe ne valident pas
une borne de retard sur HDD ni une panne de disque qui ne répond plus.


Les trois contrôles async sans comptage détaillé confirment chacun les 5 000
poses et 5 000 casses : TPS 20,025 / 19,417 / 20,007, MSPT P99 programmés
42,874 / 79,347 / 49,047 ms, CPU total 24,836 / 26,387 / 23,658 s. Avec
comptage corrigé : médiane 19,972 TPS et 26,044 s CPU, contre 20,007 TPS et
24,836 s sans ce comptage. Le second contrôle fluctue aussi. Le surcoût résiduel
n'est donc ni nul ni isolé précisément par seulement trois répétitions ; ces
mesures n'établissent pas une régression serveur, dont les sources et le
binaire sont identiques. Les traces fsync/tick restent actives dans les témoins.

La charge principale utilise un worker réseau, deux loaders de chunks et
aucun worker de simulation. À 50 bots, les captures totales médianes sont
1 309 en batched et 1 277 en async, réparties en 348 et 339 lots (environ
3,8 snapshots par lot). Les chunks ciblent plusieurs régions : ce nombre
n'est pas celui des chunks fusionnés dans une seule publication. L'évitement
observé vient aussi de la coalescence dirty entre captures. Les tests C et
l'étude isolée multi-chunks complètent ce que cette charge couvre.

| 50 bots, trois essais | fsync P50/P95/P99 ms (médianes) | Publication région P50/P95/P99 ms (médianes) | CPU / temps mural cumulé régions ms (médianes) |
|---|---:|---:|---:|
| bin | 0,584 / 0,782 / 1,299 | n/a | n/a |
| immediate | 0,650 / 0,986 / 1,374 | 1,832 / 2,262 / 2,632 | 2 803 / 9 710 |
| batched | 0,643 / 1,412 / 1,965 | 1,912 / 2,737 / 7,799 | 789 / 2 945 |
| async | 0,637 / 1,358 / 1,552 | 1,930 / 2,703 / 3,181 | 803 / 2 809 |

Les fsync de cette table incluent toutes les catégories du processus ; les
publications sont celles des régions, toutes phases comprises. Les temps CPU
et muraux ne décrivent ni bande passante physique ni attente exclusivement
matérielle. Les CSV conservent les latences individuelles et les fichiers
concernés. Les chargements de chunks à 50 bots ont un maximum par essai allant
jusqu'à 2,743 ms dans ces mondes et caches locaux, sans conclusion cold-cache HDD.

#### Charge prolongée bornée

Après toutes les campagnes comparatives, un essai async isolé à 10 bots ×
1 000 cycles confirme **10 000 poses et 10 000 casses**, sans manque ni erreur,
en 74,963 s actives : 1 500 ticks, 20,010 TPS observés, MSPT programmés
P50/P95/P99 3,353/4,095/4,768 ms, maximum 5,665 ms et aucun >50 ms. Les cycles
incluant les passes rapides ont P99 5,490 ms et maximum 6,497 ms. Le checkpoint,
l'arrêt puis les dix témoins relus après redémarrage passent ; durée totale
89,891 s jusqu'à l'arrêt, vérification du redémarrage 0,189 s supplémentaire.

Le RSS maximal observé vaut 33 352 Kio, CPU processus total 14,810 s,
2 822 597 234 octets acceptés par les écritures monde interceptées contre
2 778 918 912 `write_bytes` processus. Les métriques ne sont pas interchangeables.
Aucun comportement de file non bornée n'est observé ; **ce test d'environ
75 secondes n'est pas un test d'endurance de plusieurs heures** ni une preuve
universelle d'absence de fuite. Il ne valide pas le matériel HDD distant.

### D. Politique de durabilité et tests d’interruption

| État | Signification | Preuve possible | Perte après SIGKILL |
|---|---|---|---|
| Appliqué | Mutation autoritaire en mémoire, génération courante avancée | État/ACK de séquence et query autoritaire | Possible tant qu'aucune barrière durable n'a réussi |
| Planifié/en cours | Dirty, copie indépendante, slot accepté ou writer actif | Snapshot, submit, traces I/O, génération in-flight | Possible ; un snapshot n'est pas un journal durable |
| Durable confirmé | Publication complète et toutes les barrières réussies | Retour 0 du backend puis acquittement de la génération/barrière explicite | Préservé dans les scénarios de processus testés, sous les garanties du FS/matériel |

**Le nom `immediate` ne rend pas chaque ACK réseau durable.** L'audit de
`play.c` montre qu'une pose ordinaire émet l'état et l'ACK puis laisse dirty au
flush de fond. Certains chemins, notamment une casse réussie passant par
`mc_world_request_block_save`, exécutent leur barrière synchrone avant leur
notification dans ce mode. Un ACK acquitte aussi une action refusée : lui seul
ne démontre ni mutation ni sauvegarde. Les tests utilisent état + séquence et
contrôlent séparément la relecture disque.

| Politique | Demande de persistance | Exécution | Confirmation exploitable |
|---|---|---|---|
| `.bin`/Anvil immediate | Les appels explicites conservés sont synchrones ; les autres mutations restent dirty | Thread propriétaire, plus normalisation privée historique des loaders | Succès de l'appel de sauvegarde concerné ; jamais une règle globale fondée sur l'ACK réseau |
| Anvil batched | Demande acceptée en mémoire, coalescence avant capture | Capture et publication par région sur le tick, normalement à la passe monde suivante | Résultat 0 de la publication et acquittement ; `save-all` est une barrière sur les chunks chargés |
| Anvil async | Capture sur tick, transfert de propriété au slot borné | Worker indépendant : NBT/compression/copie/temporaire/fsync/rename/fsync parent | Fin réussie du writer puis complétion traitée sur tick ; `save-all` attend et draine |

Les actions ne changent pas d'ordre autoritaire. Les générations de sauvegarde
sont runtime et ne sont pas sérialisées dans le NBT Vanilla. Si B arrive après
capture A, le succès A ne nettoie pas B. Une ancienne source est refusée sous
verrou de publication. Une confirmation durable de A n'est pas une confirmation
de B ; un redémarrage recommence une époque de compteurs, pas une réécriture
du contrat du processus précédent.

Ces générations sont les indicateurs du pipeline natif. Le chemin historique
`.bin` ne les exploite pas comme compteur universel de durabilité ; son contrat
repose sur le retour de sa barrière atomique. Une valeur runtime isolée ne doit
pas remplacer ce contrôle ni servir de preuve pour une future migration.

#### Ordre des opérations et gestion des incidents

Pour chaque région : nouvelle image sur temporaire unique du même répertoire,
écritures vérifiées avec reprises des courts transferts/EINTR, fsync du fichier,
fermeture vérifiée, rename atomique, fsync du répertoire et vérification des
erreurs de fermeture. Le répertoire synchronisé est nécessaire à la persistance
du changement de nom ; il ne rend pas durables des changements encore en RAM,
une autre région ou un inventaire séparé. Avant rename, l'ancienne image demeure.
Après rename, le fichier visible est cohérent ; avant confirmation du parent,
le résultat durable reste incertain. Le matériel/FS doit honorer les barrières.

EIO/ENOSPC avant publication laisse l'ancienne région intacte. Un échec après
rename produit le résultat incertain -2 : le NBT visible avance pour un retry
sûr, la génération durable reste inchangée et le chunk reste dirty. Le latch
d'erreur des modes groupés empêche les retries à chaque tick et les nouvelles
mutations natives dangereuses ; une barrière explicite tente une reprise
contrôlée. Aucun rollback destructif ou fallback vers `.bin`.

Quand le slot async est plein, aucune nouvelle copie n'est allouée : les états
dirty coalescent dans les chunks vivants. La borne protège les copies en file,
pas la mémoire totale du monde ou les allocations transitoires NBT/compression.
Le disque lent peut augmenter la fenêtre de perte, sans borne murale garantie.

SIGTERM propre arrête l'admission suivant le cycle de vie du serveur, rejoint
les loaders, intègre leurs résultats, sauvegarde les derniers états appliqués,
draine le writer puis joint le thread avant de libérer les données. Seul un
arrêt réussi avec code 0 confirme ce contrat ; l'échec de sauvegarde remonte au
code de sortie. Cela ne garantit pas le traitement de tout paquet simplement
arrivé sur une socket avant le signal.

Le worker n'a pas d'API d'arrêt isolé destinée au gameplay : la fermeture
normale finit le travail accepté et rend sa complétion au propriétaire.
Une sortie anormale d'un thread seul ou un FS définitivement bloqué ne sont
pas couverts par un watchdog de récupération. Un flush/shutdown peut attendre ;
aucune annulation arbitraire d'un writer actif n'est introduite pour masquer
ce risque. SIGKILL du processus se traite par l'image atomique, sans reprise
automatique d'un temporaire orphelin.

Il n'existe toujours **aucune transaction globale** chunks/joueurs/inventaires/
objets au sol/POI. Une interruption peut laisser des sous-systèmes décalés,
avec perte ou duplication croisée possible. Conserver les inventaires des BE
opaques ne démontre pas l'exactly-once de toutes les interactions Survival.
Les tests sont des interruptions de processus et erreurs simulées ; aucune
coupure physique, aucun arrêt de la machine ou test destructif du RAID.

#### Interruptions et erreurs : résultats locaux

Le nouveau runner S5 pilote un vrai `mc_server`, deux bots et des copies neuves
du corpus opaque S3B : 1 328 chunks, sept BE de types distincts et champs NBT
inconnus. Le preload exige un marqueur jetable, deux pipes hérités et un
armement après checkpoint initial. Il ne vise que les temporaires de régions
et leur répertoire ; aucun hook de test n'est lié au binaire serveur.

| Cas | Modes / nombre | Résultat attendu et observé |
|---|---|---|
| Barrière explicite puis arrêt | 3 | Nouveau bloc, sortie 0, relecture puis redémarrage |
| SIGTERM sans barrière de l'action | 3 | Drainage du dirty, nouveau bloc, sortie 0 |
| Succès save-all puis SIGKILL | 3 | Nouveau bloc conservé ; le succès précède le signal |
| SIGKILL avant première écriture ou après écriture partielle | 3 × 2 | Ancien état intact |
| SIGKILL avant/après fsync fichier ou avant rename | 3 × 3 | Ancien état intact |
| SIGKILL après rename, avant/après fsync parent | 3 × 3 | Nouvelle image cohérente visible ; la durabilité avant parent n'est pas revendiquée |
| SIGTERM durant écriture partielle, avant rename ou avant parent | 3 × 3 | Le processus attend le writer bloqué, puis draine et sort 0 |
| ENOSPC d'écriture, EIO fichier, EIO parent ponctuels | async × 3 | Erreur remontée, dirty conservé ; save-all explicite reprend la dernière génération |
| EIO fichier/parent permanents | async × 2 | Pas de faux succès durable, arrêt code 2 ; ancienne image ou nouvelle image visible cohérente selon publication |

Ces **47 scénarios passent**. En async, les SIGTERM contrôlés et les erreurs
acceptent un état B (terre) pendant que le snapshot A (pierre) est bloqué.
L'achèvement de A ne nettoie pas B. Les retries/arrêts réussis conservent B ;
l'erreur permanente fichier laisse l'ancien air, l'erreur permanente parent
laisse B visible sans confirmation de durabilité. Aucun rollback destructif.
Le runner compare les 98 304 états du chunk cible avant/après redémarrage,
avec une seule position autorisée à changer, les sept BE avec types et valeurs
NBT exacts, et les autres chunks/fichiers globaux. Pas de fallback `.bin`.

Le gate d'injection ne prétend pas interrompre le CPU au milieu de chaque
instruction d'enqueue : la publication du slot est protégée et les tests C
exercent saturation et transfert de propriété. SIGKILL exactement avant
capture/milieu d'enqueue n'est pas une couverture processus distincte dans
cette campagne. Les tests C réexécutés couvrent snapshots indépendants,
générations, source obsolète refusant tout le lot, seconde génération après
succès de la première, slot plein/EAGAIN, limites de copies, échec d'allocation,
erreur parent, éviction différée pendant I/O et drainage des dernières mutations.

Un SIGKILL conserve le cache du noyau : voir la nouvelle image après rename
ne démontre pas sa survie à une panne électrique. Un temporaire orphelin n'est
pas automatiquement adopté au redémarrage. Les durabilités multi-régions,
joueurs et objets ne deviennent pas atomiques parce que chaque région l'est.
Une sortie anormale isolée du worker et un disque éternellement bloqué restent
sans mécanisme de watchdog validé.

### E. Compatibilité Vanilla et non-régression

Les **28 suites C** du gate passent, ainsi que smoke, multi-epoll, login,
3 tests de génération, 12 tests du framework stress, 10 tests de profil et
le nouveau test réel du preload. Celui-ci vérifie 87 appels/87 octets acceptés,
une erreur EBADF, quatre threads, chemins exclus, errno et réutilisations de fd.
Les transferts partiels et erreurs ne sont pas comptés comme octets écrits
avec succès. Le dépôt reste exempt d'artefacts de compilation suivis.

Les tests Anvil conservent les écritures DataVersion 4788 et la lecture
historique GoatCore 4785. Le lecteur natif reste strict 4788 et refuse une
ancienne source ambiguë ; aucune conversion automatique n'est ajoutée.

Les builds instrumentés S4 sont rejoués après vérification des **101 fichiers
de production identiques** : 15 suites C sous ASan+UBSan et sept sous TSan
réussissent, avec arrêt sur diagnostic. Les trois scénarios gameplay ASan
(async stairs/complex, bin placement) et le scénario TSan async complex passent
chacun trois redémarrages. Les deux commandes nouvelles de l'outil batch,
atomic et S2, sont également reconstruites/testées sous ASan+UBSan. Aucun
diagnostic n'est observé ; ce n'est pas une preuve absolue d'absence de races.
Les suites locales ne valent pas une exécution TSan sur l'hôte distant.

| Domaine | Exécution S5 | Résultat / limite |
|---|---|---|
| Connexions, reconnexions, deux bots | 21 scénarios gameplay, trois redémarrages chacun | Tous passent ; authentification offline, pas de validation Mojang positive nouvelle |
| Blocs simples, bûches, dalles/fusion, refus et consommation | Les quatre politiques | États et inventaires contrôlés, trois relectures ; gameplay inchangé |
| Escaliers / voisins | Quatre politiques ×192 observations, référence Vanilla fraîche | Les tableaux des 192 observations sont identiques à Vanilla 26.1.1 |
| Ramassage / inventaire | Quatre politiques ×100 items | 100 ramassages/inventaire acteur, observation et trois redémarrages ; pas de transaction globale démontrée |
| Frontières / négatifs | Quatre positions autour de ±512, quatre politiques | États observés par deux bots puis retrouvés aux redémarrages |
| BE et NBT opaques | 47 copies d'interruption, scénario complexe normal/instrumenté, corpus C | Sept BE couvertes et champs/types inconnus conservés ; interactions dangereuses refusées |
| Corpus supplémentaire | Quatre cas acceptés, 18 refus réexécutés | Zéro/7/160 BE et données opaques ; corruptions, protochunks, ticks, codec, `.mcc` et structures non prises en charge protégés |
| Réouverture Vanilla des sorties d'interruption | Témoin opaque + quatre résultats, deux ouvertures chacun | Tous les blocs du chunk cible et sept BE comparés, sans régénération masquant une divergence |
| Régions neuves produites par async | Copie composée explicite, deux ouvertures Vanilla | Les **35 chunks**, chacun 98 304 blocs, ont les mêmes digests avant/après Vanilla |

Les six scénarios de réouverture Vanilla comprennent le témoin, une interruption
avant publication, une après rename, un arrêt avec génération plus récente,
une erreur parent permanente et les régions neuves d'escaliers. Pour les champs
synthétiques non enregistrés que Vanilla normalise lui-même, la comparaison BE
se fait avec un témoin ouvert par le même JAR. Leur conservation exacte par
GoatCore est vérifiée séparément ; aucune conservation par Vanilla d'un champ
qu'il supprime lui-même n'est promise. Le JAR interne reste celui de 26.1.1,
SHA-256 `3647564632a2f589be2a87bf544f628e5f67cb3780c86f7fa972ec36892aa081`.

La fixture neuve composée réutilise uniquement des métadonnées officielles
sur une copie jetable, avec régions/joueurs GoatCore et suppression des POI/
entités de cette copie sans rapport avec les régions. Sa provenance est
consignée : ce n'est ni une migration ni une preuve de monde complet. Les
inventaires BE couverts sont coffres, fours froids, panneaux, bannières,
shulker boxes, tonneaux et pots décorés ; leur simulation n'est pas implémentée.
Aucune extension protochunks/`.mcc`/scheduled ticks, aucun nouvel algorithme
de lumière/biomes/physique, aucune régression observée dans ces scénarios.

#### Vérification des sources générées

Le gate complet `make -B precommit-check` passe dans un export isolé : suites C,
smoke, multi-epoll, login et tests Python. Cet export contient les fichiers
générés, mais pas les sources brutes ignorées ; ce premier gate ne revendique
pas leur régénération complète. Le contrôle d'hygiène du dépôt local passe aussi.

Le contrôle strict supplémentaire révèle des entrées brutes locales préexistantes
vides/invalides (`acacia_logs.json` dans les tags, `acacia_boat.json` dans les
composants). Elles sont préservées, sans réécriture de fichiers locaux hors lot.
Une extraction séparée des 1 515 recettes/207 tags du JAR officiel et une
exécution officielle `net.minecraft.data.Main --reports` produisent des entrées
valides dans les résultats jetables S5. Avec ces entrées et le JAR bundlé,
`REQUIRE_RAW_DATA=1 tools/check_generated.sh` passe : registre de 1 168 blocs /
29 873 états, identifiants et **tous les fichiers générés identiques**. Les
échecs initiaux restent dans les logs ; aucune donnée n'est devinée pour les
contourner. La régénération travaille dans un temporaire et ne modifie pas src.

### F. Corrections et étude secondaire S2

**Aucun changement de production** n'est retenu : les mesures ne justifient
pas une optimisation supplémentaire sûre. S5 ajoute des tests déterministes,
une observabilité des écritures, une sonde corrigée pour ne pas profiler les
réveils réseau comme des fichiers, un délai de charge explicitement borné
pour HDD et une étude S2 multi-updates isolée. Les défauts d'entrées brutes
locales sont contournés par une génération officielle séparée, sans altérer
les fichiers locaux. Le mode `.bin`/immediate reste le défaut ; Anvil,
batched/async et S2 conservent leurs statuts expérimentaux distincts.

#### S2 : conditions préalables à une combinaison avec S4

L'étude S5 ne modifie pas le protocole transactionnel. Le nouvel outil groupe
plusieurs updates dans **l'API S2 existante**, face à la publication multi-chunks
atomique existante. Même NBT immuable, mêmes lectures des sources, mêmes 100
changements distincts, toutes les données finales relues. Régions synthétiques
de 256/1 024 chunks, lots de 1/16/64, trois répétitions alternant les backends.
Les 36 passes chronométrées ont seulement traces de phases/fsync ; les volumes
proviennent de 12 passes de comptage séparées, revérifiées par les mêmes hashes.
Ces NBT contiennent des valeurs et champs opaques, **pas des chunks jouables**.

Un adaptateur direct serait dangereux : `MC_REGION_TXN_RECOVERY_REQUIRED=-2`
S2 signifie session empoisonnée et récupération obligatoire, avec en-tête
potentiellement partiel. `MC_SAVE_DURABILITY_UNCERTAIN=-2` S4 signifie image
atomique cohérente visible mais durabilité parent non confirmée. L'égalité
numérique ne rend pas ces contrats interchangeables. Les loaders S1/Vanilla
ordinaires ne doivent pas lire une transaction S2 en cours, alors que les
lecteurs actuels peuvent ouvrir l'ancien/nouveau fichier atomique sans cela.

Un futur lot doit donc prévoir : coordination de tous les lecteurs et writers
par région, validation des sources attendues/générations avant publication,
traitement distinct des erreurs nécessitant récupération et des erreurs
parent, barrières d'acquittement après COMMIT durable et reprise idempotente.
Il doit mesurer les transactions de plusieurs chunks avant de réduire les
synchronisations ; aucune barrière n'est supprimée dans S5. Pas de nouveau
format, protocole, migration, activation ou worker dans le serveur. La piste
reste un outil isolé, pas un backend gameplay utilisable annoncé.

| Chunks | Lot | Backend | Durée ms médiane [min–max] | CPU ms | Transactions/fsync | Latence transaction P50/P95/P99 ms (médianes) | Octets write/pwrite (passe séparée) |
|---:|---:|---|---:|---:|---:|---:|---:|
| 256 | 1 | atomic | 250.285 [249.659–260.400] | 86.254 | 100/200 | 2.481/2.763/2.923 | 107315200 |
| 256 | 1 | s2 | 439.013 [432.649–484.674] | 97.452 | 100/700 | 4.397/4.732/4.844 | 3927200 |
| 256 | 16 | atomic | 22.419 [22.324–23.149] | 11.225 | 7/14 | 3.278/3.405/3.405 | 7892992 |
| 256 | 16 | s2 | 35.428 [34.832–35.851] | 11.133 | 7/49 | 4.987/5.562/5.562 | 659552 |
| 256 | 64 | atomic | 9.448 [9.420–9.599] | 6.216 | 2/4 | 4.132/5.312/5.312 | 2547712 |
| 256 | 64 | s2 | 13.959 [13.071–14.418] | 6.516 | 2/14 | 6.352/7.523/7.523 | 483872 |
| 1024 | 1 | atomic | 569.537 [554.569–569.552] | 290.272 | 100/200 | 5.592/6.187/6.502 | 421888000 |
| 1024 | 1 | s2 | 604.135 [602.977–624.291] | 242.351 | 100/700 | 6.002/6.502/6.689 | 6999200 |
| 1024 | 16 | atomic | 44.542 [44.428–47.949] | 24.670 | 7/14 | 6.404/6.686/6.686 | 29913088 |
| 1024 | 16 | s2 | 49.909 [46.837–53.605] | 21.695 | 7/49 | 7.289/7.470/7.470 | 874592 |
| 1024 | 64 | atomic | 15.677 [15.443–15.715] | 10.281 | 2/4 | 7.218/8.375/8.375 | 8839168 |
| 1024 | 64 | s2 | 16.233 [15.588–17.356] | 9.265 | 2/14 | 7.675/8.553/8.553 | 545312 |

S2 reste plus lent en médiane dans ces six paires locales, tout en réduisant
fortement les volumes. À 1 024 chunks/lot 64 : 16,233 contre 15,677 ms,
545 312 contre 8 839 168 octets interceptés, mais 14 contre 4 fsync. Il s'agit
d'un état final identique après 100 mutations, avec seulement deux checkpoints
pour cette largeur, contre 100 en largeur 1. Les garanties intermédiaires ne
sont pas équivalentes entre largeurs ; aucun gain TPS n'est annoncé.

Les P95/P99 des transactions sont peu significatifs lorsque chaque passe ne
contient que deux ou sept lots ; leurs échantillons et les trois répétitions
sont conservés. Les taux de fragmentation viennent de l'index logique, pas
d'une mesure d'extent physique. Le coût de récupération reste couvert par les
tests S2 rejoués ; aucune nouvelle distribution de latence de récupération
matérielle n'est revendiquée par ce benchmark sans interruption.

Dans les passes de comptage, les journaux préparés font au maximum 29 344
octets (256 chunks/lot 64) ou 60 064 octets (1 024/lot 64). Les deux journaux
de la charge représentent respectivement 57 568 et 119 008 octets ; aucun
journal temporaire/canonique ne reste après fermeture propre. Les métadonnées
de session S2 distinctes ne sont pas un prérequis de lecture Vanilla du `.mca`
propre. À largeur 64, l'index final S2 contient 64 secteurs libres en deux
plages (322/1 090 secteurs totaux), contre un secteur libre en atomic
(259/1 027). Cette amplification temporaire/résiduelle copy-on-write doit être
comptée avec les journaux, sans la confondre avec une reconstruction complète.

#### Fichiers et preuves du lot

| Usage | Fichiers |
|---|---|
| Instrumentation et mesures | `tests/stress/profile_fsync.c`, `profile_tick.py`, `test_tick_profile.py`, nouveau `test_io_probe.py` |
| Pannes contrôlées et relecture | Nouveaux `tests/stress/storage_fault.c`, `test_save_durability.py` |
| Étude S2 isolée | `tools/mc_region_txn.c`, nouveau `tests/stress/profile_region_batches.py` |
| Préparation distante | Nouveau `scripts/inspect-storage-host.sh` en lecture seule |
| Gate | `Makefile` : ajout du test réel de la sonde |
| Documentation | Présent rapport, `tests/stress/README.md`, nouveau `docs/storage-migration-s3c.md` |
| Résultats légers | Nouveau `tests/fixtures/vanilla-26.1.1-s5-validation.json` |

Les 47 résultats sont également comparés **après** redémarrage par l'oracle
indépendant de conservation de tous les NBT/fichiers non concernés. Les deux
replays ciblés (arrêt sans barrière d'action et kill avant fsync en async)
exécutent le runner final qui intègre cette assertion supplémentaire. Tous
passent ; aucune modification de production n'est nécessaire.

Les données brutes restent dans `tests/stress/results/storage-s5/` ignoré :
`bench-final`, `observer-control`, `durability-final`, contrôles post-redémarrage,
`game-*`, `vanilla-*`, `corpus-refusals`, `s2-batches`, `soak`, logs des gates et
sanitizers, traces et configurations. Le pilote intrusif `bench/` est exclu de
l'analyse représentative. Le manifeste brut couvre 1 820 fichiers de preuve,
SHA-256 `a3e6cf1f499ae7f0671fc9c8c048a5d51d5857a861908f3a94de489d527ff508`.
La fixture versionnée conserve les agrégats, empreintes et limites sans mondes
volumineux ni dumps de processus personnels. Les copies de tests sont conservées
pour reproduction ; tous les processus temporaires sont arrêtés.

### G. Risques restants

**Risques restants :** accès Xeon/HDD bloqué, variabilité du PC partagé,
coût résiduel des sondes, débit Python et fanout non isolés, simulation items,
reconstructions encore complètes et amplification Anvil, absence de borne
murale de la fenêtre dirty, absence de watchdog disque/worker, absence de
transaction chunks/joueurs/items/POI, aucune panne électrique matérielle testée.
Les interruptions de processus et erreurs injectées ne permettent pas une
certification globale du FS, du contrôleur RAID ou du matériel.

Le helper `scripts/inspect-storage-host.sh` reste en lecture seule. Le README
stress décrit la reprise via les scripts de staging existants, même compilation,
CPU/ressources bornés, priorité réduite, mondes et ports distincts, zéro
modification des services principaux. Les compteurs physiques HDD/RAID doivent
être relevés si visibles ; leur absence en LXC reste une limitation, pas une
preuve de bonne santé. Les mesures NVMe ne sont jamais étiquetées HDD.

### H. Prochaine phase et proposition S3C

La proposition **S3C** est dans `docs/storage-migration-s3c.md` : adoption pour
mondes neufs distincte d'une conversion ; inventaire `.bin` 4785/4788, packing
compact contre natif aligné, backup intégral vérifié, conversion hors place,
comparaisons indépendantes des blocs/BE/NBT/joueurs, ouvertures Vanilla,
checkpoints et rollback conservant les sources. Aucun convertisseur, déplacement,
bascule de défaut ni suppression des `.bin` n'est exécuté. L'activation d'une
destination ne peut intervenir qu'après validation et autorisation distinctes.

Priorité suivante : rejouer S5 sur Xeon/HDD dès son retour, avec la durée totale
jusqu'au checkpoint durable et l'arrêt, puis décider l'adoption des nouveaux
mondes et le périmètre S3C. Une combinaison S2/S4 nécessiterait un lot séparé
de coordination des lecteurs/récupération, avant toute activation. La reprise
des portes/trappes et autres blocs reste hors S5 ; ni phase 2 ni compatibilité
Vanilla complète ne sont déclarées terminées.

# S3C — Migration hors ligne GoatCore vers Anvil 26.1.1

Le lot part de `28b3c5e253dfec8da2d5303d7b861463afe8f638`. Les sources
et modes actifs S1–S5 ne sont pas réimplémentés. Les 101 fichiers de production
référencés par S4 restent identiques ; le module de traduction est lié uniquement
au nouvel outil hors ligne. Aucun changement de gameplay, de tick, de worker,
de configuration principale ou de défaut de stockage.

## Architecture et périmètre

`tools/mc_world_migrate.c` fournit analyse, conversion et vérification explicites.
`src/world/chunk_migration.c` et son en-tête traduisent le packing historique sans
reconstruire le NBT à partir d'un modèle incomplet. Le registre officiel, les
codecs NBT/historique/natif, l'index S1 et l'écrivain atomique durable sont réutilisés.
Le moteur S2 et le pipeline S4 restent inchangés.

Le contrat complet, les commandes, limites et codes de retour sont dans
[`storage-migration-s3c.md`](storage-migration-s3c.md). La destination doit être
indépendante et absente. L'analyse inventorie toutes les entrées et refuse le lot
entier si une donnée n'est pas convertible sans perte. Les sources sont ouvertes
en lecture seule, verrouillées lorsque les protocoles coopératifs le permettent,
empreintées et revérifiées à plusieurs barrières. Aucun `.bin` n'est supprimé.

La conversion transforme uniquement DataVersion et longs des états de blocs.
Les chunks doivent correspondre au schéma GoatCore connu 4785/4788, complets,
Overworld, avec biomes uniformes plains démontrés. Les noms, propriétés, palettes,
champs NBT opaques et fichiers annexes sont préservés. Les inventaires historiques
`slot`/count Byte, fichiers sidecars de conteneurs, protochunks, `.mcc`, formats
ambigus et cas S3B non modifiables sont refusés. Il n'existe pas de DataFixer
général ni de création fictive de block entity, biome, métadonnée mondiale ou POI.

Le manifeste contient les empreintes des sources et des cibles. Les régions sont
relues et comparées exactement au NBT transformé ; les deux codecs comparent les
98 304 états de chaque chunk. Un oracle Python indépendant compare aussi types,
valeurs, sections et états. Toutes les copies ordinaires sont vérifiées par SHA-256.
La conversion exige `accepted=converted=verified` avant publication.

## Publication et interruptions

Les lots de construction sont bornés à 64 chunks / 32 Mio de NBT par région.
Le staging privé est créé dans le parent de destination. Les régions utilisent
le remplacement atomique durable ; le manifeste est synchronisé, puis tous les
fichiers/répertoires du staging le sont de bas en haut. Après une dernière
vérification des sources, `renameat2(RENAME_NOREPLACE)` publie l'arbre, puis le
parent est synchronisé. Aucun répertoire existant n'est remplacé.

| Interruption contrôlée | Résultat exigé et observé dans la matrice CLI |
|---|---|
| Analyse, avant lecture, après lecture partielle | Source intacte ; destination absente ; relance sur destination neuve réussie |
| Conversion/préparation, avant et pendant écriture de région | Source intacte ; seulement staging éventuel ; aucune publication partielle |
| Avant/après fsync du fichier de région | Ancienne source intacte ; nouvelle destination finale encore absente |
| Vérification et lecture partielle de vérification | Destination finale absente ; source intacte ; relance possible |
| Avant/après synchronisation récursive, avant publication | Destination absente ; source intacte |
| Après publication, avant fsync du parent | Destination cohérente et vérifiable ; durabilité du nom non acquittée |
| Après fsync du parent | Destination cohérente et vérifiable ; source inchangée |
| EIO/ENOSPC lecture/création/écriture/fsync/rename | Échec explicite, aucune destination partielle annoncée valide |
| Erreur fsync du parent ou rename ayant déjà publié | Code 3, résultat visible cohérent mais durabilité incertaine ; aucun rollback destructeur |
| Écriture courte ou EINTR lecture/écriture/fsync | Boucles de complétion/reprise, conversion vérifiée |

La matrice contient **16 points SIGKILL**, huit erreurs système et quatre
reprises d'écritures courtes/EINTR. Elle vérifie aussi les relances, le refus
d'écraser une destination, une source modifiée et une course de création de
destination. Les hooks n'existent que dans `mc_migrate_fault` ; ils ne sont liés
ni à l'outil normal ni au serveur. Les stagings ne sont jamais effacés ou promus
automatiquement. SIGKILL ne reproduit pas une coupure électrique, et les fsync
ne démontrent pas la fiabilité physique des caches/contrôleurs de stockage.

## Matrice locale d'intégration

Tests sur copies privées, avant le retour du serveur distant. Toutes les
conversions sont suivies d'une vérification C et Python, de redémarrages GoatCore
natifs, de mutations observées par deux bots puis de deux ouvertures Vanilla.
Les métadonnées globales absentes de certains mondes BIN sont ajoutées uniquement
à une **composition de validation séparée**, depuis un modèle officiel déclaré.
Le convertisseur ne fabrique ni `level.dat` ni monde Vanilla complet.

| Corpus source | Chunks convertis et vérifiés | Chunks comparés après Vanilla | Particularité |
|---|---:|---:|---|
| Monde historique réel d'escaliers S5 | 483 | 483 | 192 observations identiques à la référence officielle S5 |
| Copie du précédent, DataVersion 4785 | 483 | 483 | Variante synthétique par changement du tag, pas une capture ancienne de production |
| Placement émis par le writer BIN actuel | 35 | 40 | Blocs, bûches, dalles et fusion ; nouveaux chunks créés pendant les essais |
| Frontières émises par le writer BIN actuel | 85 | 94 | Coordonnées négatives et plusieurs régions |
| Chunk officiel complexe reconditionné en BIN | 1 | 39 | Sept BE officielles, packing compact et champ racine opaque synthétique |
| Même fixture sous ASan/UBSan serveur | 1 | 34 | Relecture, mutation à deux observateurs, redémarrages |
| Même fixture sous TSan serveur | 1 | 34 | Même séquence et conservation des BE |

Les cinq corpus normaux représentent **1 087 conversions vérifiées**, dont des
copies/variantes d'un même monde, et non 1 087 chunks historiques uniques. Les
deux variantes de sanitizer ajoutent deux conversions. Les comparaisons Vanilla
portent sur chaque état des chunks provenant de GoatCore, y compris ceux créés
pendant les essais. Une ouverture réussie seule n'est pas le critère.

Les sept familles conservées sont coffre, four froid, panneau, bannière, shulker,
tonneau et pot décoré : types/valeurs NBT, inventaires et composants sont identiques
après sauvegarde GoatCore puis Vanilla dans ces scénarios. La fixture est composée
d'un chunk officiel S3B dont les longs ont été recompactés ; elle ne prétend pas
que l'ancien writer de conteneurs produisait déjà ces enveloppes natives. Ses
champs opaques synthétiques sont comparés exactement avant réécriture officielle.
Les anciennes enveloppes d'items non convertibles restent refusées.

Pour chaque scénario normal, la suite commence aussi par démarrer une copie BIN
et vérifier que le convertisseur refuse cette source réellement utilisée. La
source originale et la destination pristine sont encore vérifiées après les essais
sur copies. Les cycles du marqueur autoritaire comprennent une mutation et deux
redémarrages, avec observation par deux bots et barrière `save-all` à chaque cycle.

## Non-régression locale

`make -B precommit-check` a réussi dans un export propre : **29 suites C**, smoke,
multi-epoll, login réseau, tests Python existants et nouvelle suite CLI de
14 méthodes. Le codec ajoute **590 636 vérifications**. Après extension aux
16 points d'interruption, la suite CLI a été rejouée avec succès en normal,
ASan/UBSan et TSan. Le codec a également réussi sous ces sanitizers. Les mêmes
15 suites C ASan/UBSan et sept suites C TSan de S5 ont été rejouées avec succès,
leurs sources de production étant inchangées ; deux intégrations serveur
instrumentées complètent ces validations. Aucun diagnostic sanitizer observé
sur les chemins exécutés ; ceci ne prouve pas l'absence de races universelle.

Les tests de refus couvrent versions/états inconnus, coordonnées incohérentes,
NBT invalide ou dupliqué, compression/trailing bytes, données perdues, verrou
Vanilla POSIX, verrou BIN, liens, destination imbriquée/existante, rapport dirigé
vers la source, `.mcc` nécessaire, corruption du manifeste/région et source
modifiée. Le contrôle strict des données générées a réussi avec les rapports
et inputs officiels frais de S5 ; les JSON locaux antérieurs non fiables sont
préservés. Aucun comportement de placement n'est modifié.

## Benchmarks NVMe locaux

Ryzen 7 7840HS, stockage NVMe local. Trois répétitions par taille, travail fixé,
passes chronométrées sans observateur, puis une passe distincte par taille
comptant les opérations. Caches non purgés ; ces mesures ne décrivent pas un
chargement froid. Temps en millisecondes, médianes ; la conversion inclut audit,
construction, relecture, vérification, synchronisations et publication.

| Chunks | Analyse | Conversion complète | Vérification séparée | CPU conversion | Pic RSS conversion |
|---:|---:|---:|---:|---:|---:|
| 1 | 5,02 | 23,04 | 8,69 | 13,00 | 14 512 Kio |
| 100 | 241,95 | 779,90 | 477,51 | 715,79 | 14 840 Kio |
| 483 | 1 152,48 | 3 546,81 | 2 294,92 | 3 438,71 | 16 104 Kio |
| 1 000 | 2 384,02 | 7 325,34 | 4 723,51 | 7 137,51 | 15 952 Kio |

Débits médians de conversion : environ 43,4 / 128,2 / 136,2 / 136,5 chunks/s.
Les 1/100/483 chunks sont des sous-ensembles/copies du monde BIN réel. Le corpus
1 000 est une relocalisation **synthétique** d'un chunk sans BE dans une région
négative presque pleine. Ces mesures NVMe précèdent le durcissement final des alias de montage (outil
`b2c5aadc…`). Elles ne sont pas attribuées au binaire final ; la campagne finale
a été rejouée sur Xeon après son retour. Les 12 conversions chronométrées et quatre passes
comptées totalisent **6 336 chunks convertis et vérifiés** ; elles ne constituent
pas 6 336 chunks source distincts.

| Chunks | Appels write/pwrite ciblés | Octets écrits ciblés | fsync observés | Temps cumulé fsync, passe comptée |
|---:|---:|---:|---:|---:|
| 1 | 9 | 23 136 | 126 | 23,14 ms |
| 100 | 312 | 505 564 | 138 | 74,80 ms |
| 483 | 1 495 | 3 836 558 | 148 | 122,35 ms |
| 1 000 | 3 516 | 36 102 710 | 156 | 195,39 ms |

Les copies annexes et répertoires expliquent une partie des synchronisations
même pour un seul chunk. Les lots bornés peuvent reconstruire plusieurs fois une
région de grande taille ; S3C ne modifie pas le backend pour optimiser ce coût.
Les durées des passes comptées sont exclues du classement. Les compteurs
`source_read_bytes` et `copy_and_manifest_write_bytes` ont des périmètres limités ;
les traces de processus et write/pwrite complètent ces mesures sans prétendre
mesurer les écritures physiques du NVMe. Aucun gain de TPS n'est revendiqué.

## Durcissement démontré pendant la revue S3C

Un test dans un espace utilisateur/montage isolé a révélé un défaut de la première
version du convertisseur : un montage bind exposant la source sous un autre
chemin contournait la détection textuelle des destinations imbriquées. L'outil
refusait finalement la publication car la source avait changé, mais avait déjà
créé un staging dans cette source. Aucun `.bin` n'était tronqué ; la garantie
« aucune écriture source » n'était toutefois pas respectée.

La version finale compare l'identité dev/inode du parent de destination avec
celle de la racine source et de tous ses répertoires inventoriés, avant toute
création. Elle détecte aussi les fichiers de rapport stdout/stderr exposés par
un montage bind : aucune sortie n'est ajoutée à ces fichiers. Le test indépendant
`tests/test_migration_mounts.py` couvre sept refus : alias de source entière et
de sous-arbre pour analyse/conversion, deux rapports dans la source et un rapport
dans une destination existante. La source et la destination sont comparées avant/
après. Les sept contrôles passent localement en normal, ASan/UBSan et TSan.
La matrice CLI complète et les 590 636 contrôles C ont également été rejoués dans
les trois configurations sur ce code corrigé. Ce durcissement n'ajoute aucun
chemin de migration au serveur.

Le test de montage utilise `unshare -Urnm` et des répertoires temporaires privés,
puis démonte ses alias avant nettoyage. Il est explicitement ignoré lorsque les
namespaces utilisateur/montage ne sont pas disponibles ; cette absence ne vaut
pas validation. Les changements de montages concurrents par un administrateur
restent hors contrat. Une redirection shell peut tronquer avant le lancement de
l'outil : le contrat impose toujours des rapports extérieurs aux deux arbres.

## Validation distante finale — 4 novembre 2026

L'accès à `10.0.4.66` est revenu pendant S3C. Deux exports privés ont été créés
avec le script existant `remote-build-stage.sh`, sans Git distant et sans
remplacement d'un binaire principal. La campagne finale utilise
`/opt/goatcore/phase2a-stage.5lzj8b` sur Xeon E5-2637 v2 : ext4 dans le volume LVM
du conteneur, espace libre environ 49 Gio. Les disques visibles sont rotatifs ;
l'état du contrôleur et la redondance physique du RAID ne sont pas observables
à ce niveau. Les tâches utilisent `nice 10`, `ionice -c 2 -n 7`, CPU 8–11 ;
le processus principal reste sur CPU 1–3. Aucun service n'a été redémarré.

`make -B precommit-check` réussit dans l'export final propre, incluant les
29 suites C, le codec 590 636 contrôles, la suite CLI 14 méthodes et les sept
contrôles de montage bind réellement exécutés. Les 16 SIGKILL, huit erreurs et
quatre reprises courtes/EINTR sont également rejoués sur ce stockage. Les cinq
intégrations réussissent : 483 + 483 + 35 + 85 + 1 = **1 087 chunks convertis et
vérifiés**, sans action ou chunk manquant dans les assertions des scénarios.
Les nombres comparés après Vanilla sont respectivement 483/483/40/94/39 : tous
les blocs des chunks issus de GoatCore, avec les nouveaux chunks des essais.
Les sept BE conservent leurs types/valeurs, inventaires et composants après
GoatCore puis Vanilla. Les quatre corpus communs avant/après durcissement
(1 086 chunks) ont aussi des payloads NBT et manifestes **identiques octet par
octet** ; les timestamps physiques des en-têtes Anvil ne sont pas comparés.

### Conversion sur le Xeon et son volume HDD déclaré

Trois répétitions de chaque taille, puis quatre passes séparées de comptage.
Aucun compilateur ou autre campagne S3C n'est actif pendant les mesures finales.
Même travail et fixtures que la campagne NVMe initiale, avec le durcissement
final des chemins. Compilateur distant GCC 12.2 ; options C11/O2 du projet.
Les différences CPU, bibliothèque, stockage, priorités et caches interdisent
une attribution de l'écart NVMe/Xeon au seul disque.

| Chunks | Analyse médiane | Conversion médiane | Vérification séparée médiane | CPU conversion | Pic RSS médian |
|---:|---:|---:|---:|---:|---:|
| 1 | 32,27 ms | 458,96 ms | 57,83 ms | 85,64 ms | 12 772 Kio |
| 100 | 792,43 ms | 3 893,54 ms | 1 477,80 ms | 2 447,34 ms | 13 004 Kio |
| 483 | 3 390,08 ms | 11 420,99 ms | 6 526,50 ms | 10 483,43 ms | 14 084 Kio |
| 1 000 | 6 884,79 ms | 27 321,55 ms | 13 519,70 ms | 21 685,15 ms | 14 080 Kio |

Débits médians : **2,18 / 25,68 / 42,29 / 36,60 chunks/s**. Les répétitions
100 chunks vont de 3,03 à 5,74 s ; 483 chunks de 11,42 à 15,45 s ; 1 000 chunks
de 27,03 à 27,87 s. L'attente disque varie. Avant durcissement, le CPU médian
pour 1 000 chunks était 21,58 s contre 21,69 s après ; le temps mural passe de
23,00 à 27,32 s. Ces campagnes successives ne permettent pas d'attribuer tout
cet écart au contrôle des alias. Aucune optimisation ni réduction de durabilité
n'est appliquée pour masquer cette variabilité.

Les 6 336 conversions chronométrées/comptées finales sont toutes vérifiées.
Les volumes et nombres d'opérations ciblées sont les mêmes que dans le tableau
NVMe : 23 136 / 505 564 / 3 836 558 / 36 102 710 octets, avec 126/138/148/156
fsync. Les durées cumulées fsync des **passes comptées séparées** sont
2 457,83 / 2 699,25 / 3 710,33 / 3 696,27 ms ; elles ne sont pas utilisées pour
classer les latences des passes ordinaires. Les compteurs processus sont
conservés avec les write/pwrite ; ils ne mesurent pas les plateaux physiques du
RAID. Les reconstructions répétées dues aux lots bornés restent une limite de
l'outil atomique et ne justifient pas d'activer S2 dans ce lot.

Le premier manifeste du runner portait un libellé NVMe codé en dur, même sur le
distant. Ce défaut de **métadonnée de test** est corrigé par `--hardware-label`.
Les manifestes distants archivés conservent `manifest-original.json`, son SHA,
la valeur initiale et une correction explicite fondée sur les contrôles SSH.
Aucune durée, opération ou donnée de monde n'est modifiée par cette correction.

### Chargement et redémarrage

Trois cycles du marqueur par corpus, deux observateurs, barrière `save-all`,
puis arrêt propre. Les mesures ci-dessous sont des sondes sur caches non purgés,
pas des garanties de chargement froid. Les latences backend excluent les recherches
aboutissant à une absence ; ces absences, suivies d'une création attendue, sont
comptées séparément dans les résultats bruts.

| Corpus | Premier paquet de chunk après lancement, médiane | Chargements existants | P50/P95/P99 backend |
|---|---:|---:|---:|
| Escaliers 483 | 103,26 ms | 90 | 4,42 / 16,49 / 19,57 ms |
| Variante 4785 | 104,27 ms | 90 | 4,48 / 10,32 / 17,43 ms |
| Placement | 110,23 ms | 85 | 12,47 / 16,77 / 19,89 ms |
| Frontières | 209,77 ms | 100 | 11,12 / 16,71 / 17,75 ms |
| Complexe | 112,40 ms | 75 | 6,14 / 16,89 / 17,82 ms |
| Référence Anvil vierge, sans conversion | 213,20 ms | 50 | 11,24 / 15,60 / 16,52 ms |

Le monde vierge démarre trois fois, crée ses régions sans `.bin`, conserve le
marqueur et confirme les observations des deux bots. Les 44 arrêts GoatCore
chronométrés ont une médiane de 114,43 ms et un maximum de 416,78 ms, après les
barrières des scénarios. Ceci ne démontre pas un temps maximal d'arrêt à 50 bots
avec une file pleine. Les CSV contiennent chargement, sérialisation, compression,
écriture et synchronisations ; les phases à un seul échantillon ne constituent
pas des distributions statistiques représentatives.

### Échec préliminaire et reprise

Une première relecture Vanilla du corpus frontières a passé le contrôle du bloc
puis dépassé 60 secondes pendant l'arrêt et la sauvegarde ; le harnais a tué
uniquement cette JVM temporaire et déclaré l'essai échoué. Cette première
campagne n'est pas qualifiée complète. La compilation concurrente et la pression
E/S ont pu contribuer, sans cause unique démontrée.

Le harnais expose désormais `--shutdown-timeout` (défaut inchangé 60 s, borne
1–600 s), mesure l'arrêt et garde son échec explicite à expiration. Le runner
S3C choisit 180 s pour Vanilla. La campagne finale sans compilation concurrente
réussit ses **dix ouvertures Vanilla** ; tous les arrêts terminent en 3,33 à
41,69 s, dont 36,67 et 12,75 s pour frontières. Aucune région de cet essai tué
n'est utilisée comme résultat final ; les reprises repartent des copies pristine.

## Livrables, limites et passage suivant

Fichiers : `.gitignore`, `Makefile`, `include/mc_chunk_migration.h`,
`src/world/chunk_migration.c`, `tools/mc_world_migrate.c`,
`tools/check_repo_hygiene.sh`, `tests/test_chunk_migration.c`,
`tests/migration_fault.c`, `tests/test_world_migration.py`,
`tests/test_migration_mounts.py`, `tests/stress/profile_world_migration.py`,
`tests/stress/check_world_migration.py`, `tests/stress/run_block_checks.py`,
`tests/stress/README.md`, les deux documents S3C et le manifeste léger
`tests/fixtures/vanilla-26.1.1-s3c-validation.json`. Les mondes et exécutables
restent hors Git ; les preuves brutes sont sous `tests/stress/results/storage-s3c/`.

Empreintes des binaires finaux compilés et testés sur le distant :

- Convertisseur : `17c770061b45a048e0ea7ac46b23b7b490e44e614f456a5f3276f4ee529b7a21`.
- `mc_server` de test : `190d9d727f15d127ea5c13fb83759223e12ff9237bbb73e89ca592dbc4373878`.

Le 4 novembre à 12:23:58 UTC : 25565 actif, PID 139, même date d'activation ;
25587 inactif, non redémarré. Les SHA du binaire principal et des deux
configurations sont inchangés avant/après. Aucun processus Minecraft temporaire
ne subsiste ; aucun monde principal, service ou permission n'est modifié.
Aucun déploiement principal, aucune migration réelle et aucun push.

S3C est validé sur les scénarios couverts, localement puis sur le volume du
serveur Xeon. La santé du RAID, la résistance à une coupure électrique, les mondes
Vanilla complets et les anciennes enveloppes de conteneurs ne sont pas validés.
Les 4785 testés sont des fixtures/variantes déclarées, pas une collecte exhaustive
d'anciens mondes. Aucun retour implicite vers `.bin`, aucune activation automatique,
aucune fusion S2/S4 et aucun changement de défaut.

Avant toute utilisation principale : rejouer la charge S5 1/10/25/50 bots sur ce
matériel, qualifier le corpus réel et traiter ou refuser chaque inventaire/sidecar
historique incompatible. Suivre ensuite la procédure de sauvegarde, conversion
hors place, vérification, essais, autorisation de bascule et rollback décrite dans
`storage-migration-s3c.md`. Ne pas supprimer les anciens mondes. Aucun résultat
S3C ne prouve 20 TPS constants et aucune nouvelle phase gameplay n'est commencée.


# G1 — Prototype GoatWorld et comparaison native / Anvil S4 / SQLite

## Périmètre et état initial

Départ `9aed5c10cad48017906174e12042ea043ecfb893`. Ce lot répond au changement
explicite de stratégie : étude d'un stockage propriétaire, sans présumer son gain.
S1 à S5 et S3C sont réutilisés, pas réimplémentés. Le moteur normal de `mc_server`
n'est pas modifié. Le binaire serveur distant obtenu est identique à celui de S3C
(`190d9d…`), les modes par défaut, S2, S4, les règles gameplay et les mondes actifs
restent inchangés. Les nouvelles dépendances sont facultatives et limitées aux
outils. Le format détaillé, les commandes et limites sont dans
[goatworld-g1.md](goatworld-g1.md).

Livrables C11 : `mc_goatworld`, API `mc_goatworld.h`, modules expérimentaux natif,
codecs, packing réversible et SQLite. `mc_goatworld_fault` ajoute uniquement les
barrières de test. Aucun de ces modules n'entre dans SRC/`mc_server`. Les lecteurs
S1, l'écrivain atomique groupé S4, NBT et SimpleBitStorage existants sont réutilisés.
L'adaptateur Anvil appelle le même `mc_anvil_write_batch_expected` que S4, sur les
mêmes snapshots et batches. La comparaison ci-dessous **isole le stockage**, pas
l'exécution des workers S4, le réseau ou le gameplay. Aucun gain TPS n'est annoncé.

## Environnements et méthode

Local : Ryzen 7 7840HS, Linux, NVMe, outils hors ligne seulement. Distant : Xeon
E5-2637 v2, conteneur LXC, ext4 sur `/dev/mapper/LVM4-vm--125--disk--0`, support
rotatif observé ; le contrôleur et la santé physique du RAID ne sont pas visibles.
Les tests utilisent `/opt/goatcore/phase2a-stage.9XgBx2`, nice 10, ionice 2/7 et
CPU 8–11, distincts des CPU principaux. Pas de limite CPU cgroup ni de throttling
observé au contrôle ; les autres charges de l'hôte ne sont pas entièrement visibles.
Aucune purge globale des caches ni opération destructive sur les disques.

Versions locales : zlib 1.3.1, LZ4 1.10.0, Zstd 1.5.7, SQLite 3.46.1.
Versions distantes : zlib 1.2.13, LZ4 1.9.4, Zstd 1.5.4, SQLite 3.40.1.
Les différences de bibliothèques empêchent d'attribuer toute différence entre
hôtes au seul support physique. Les comparaisons d'architectures sont effectuées
**sur chaque hôte**, avec zlib6 commun, mêmes NBT, générations, préimages et actions.
Les en-têtes SDK ont été extraits dans des répertoires de test, sans installation
système ni modification des bibliothèques des services.

Chaque scénario : seed, fermeture durable/checkpoint, conseil d'éviction par
fichier, réouverture avec index applicatif vide, lectures séquentielles puis
aléatoires/voisines, prélecture, cache applicatif borné, 100 mises à jour isolées du
même chunk, 100 réparties par batches régionaux de 64 au maximum, fermeture,
relecture exhaustive, compactage, fermeture, relecture finale. Les mutations
portent sur le tag réservé `G1BenchRevision` ; elles ne prétendent pas mesurer la
simulation de blocs. Les empreintes finales sont identiques entre backends,
profils, répétitions et hôtes. Aucun travail différé n'est exclu du total complet.
La préparation du corpus avant l'ouverture est exclue et identique. Le total
diagnostique inclut aussi les inventaires d'allocation, le conseil d'éviction et
l'émission des échantillons déjà chronométrés ; il ne remplace pas les latences
de phases isolées ni une mesure du serveur en fonctionnement.

Trois répétitions, ordre des backends tournant, 200 modifications confirmées par
essai. Le runner limite à 1 Gio par processus/fichier et 600 s par essai ; le plus
gros corpus fait 58,8 Mo de NBT. Les sorties de benchmark jetables sont supprimées
seulement après vérification ; les preuves et corpus sources restent conservés.
Les mesures ordinaires n'ont aucun preload. Les passes comptées sont séparées et
ne classent pas les temps ordinaires. La sonde inclut désormais **fdatasync et
pwrite64**, nécessaires à SQLite ; les CSV sont rapprochés des compteurs sans perte.
Elle mesure les appels d'écriture observés, pas un inventaire de tous les syscalls.
`strace` n'est pas disponible sur le distant.

Le conseil fadvise(DONTNEED) est contrôlé par mincore : zéro page résidente
observée dans les passes qualifiées. Ceci qualifie le cache de pages du processus/
système, **pas** les caches du contrôleur, des HDD ou du RAID. Les P50/P95/P99
bruts et les temps CPU sont conservés. Les tableaux rapportent les médianes de
ces quantiles par essai ; une distribution de deux transactions groupées n'a pas
une queue P99 statistiquement robuste. Les ouvertures sont paresseuses : le
coût de chargement du premier index fait partie de la première lecture, pas du
seul appel d'ouverture de la racine.

La première campagne distante contient aussi des profils de codecs, dont quelques
passes ont chevauché un test de correction de trois secondes. **Ces passes ne
servent pas à choisir les profils** : une campagne propre `xeon-profiles-final`
rejoue les 60 passes de profils, seule et après la référence principale. Les
45 passes chronométrées zlib6 de référence ont précédé ce chevauchement.

## Corpus et conservation

| Corpus | Nature | Contenu |
|---|---|---|
| 1 / 100 / 483 | Sous-ensembles historiques réels, déjà convertis par S3C | Plusieurs régions, coordonnées négatives, dalles et escaliers ; pas de déplacement des chunks |
| 206 officiels complets | NBT brut du serveur officiel 26.1.1 | Quatre régions, sept BE couvertes ; import/export byte-for-byte natif et SQLite |
| 1 122 protochunks du même monde | Détectés et inventoriés | Originaux conservés, exclus explicitement, jamais promus en full |
| 1 000 | Synthétique déclaré | Région dense négative, 500 uniformes, 250 palettes de 256 blocs, 250 avec données opaques |
| 128 répartis | Synthétique déclaré | Une entrée par région ; expose le coût des index et fichiers supplémentaires |
| 32 mixtes et cas uniform/diverse/opaque | Synthétiques déclarés | Étude des douze profils, types NBT imbriqués, données compressibles et incompressibles |

Les empreintes des régions originales, du rapport officiel des blocs et des
corpus sont consignées. Aucun monde synthétique n'est présenté comme de la
génération officielle. Le corpus officiel est surtout plat et construit par
scénarios ; ce n'est pas un échantillon représentatif de tous les mondes Survival.
Les NBT complets sont conservés exactement : BE, inventaires/composants, biomes,
éclairage, structures, ticks et champs inconnus restent opaques à l'archive.
**Archiver n'autorise pas leur simulation ou leur modification par GoatCore.**
Aucune normalisation de ces champs, aucun état remplacé par l'air. Le prototype
ne remplace pas les validations gameplay S3B. Les DataVersion autres que 4788,
protochunks et références MCA externes/codecs inconnus sont refusés à l'import.
S3C reste le chemin explicite de conversion 4785 → 4788 pour ses données couvertes.

## Mesures NVMe locales

| Corpus | Backend | Total complet ms | 100 écritures isolées ms | 100 modifications groupées ms | Lecture aléatoire P50/P99 µs | Compactage ms |
|---|---|---:|---:|---:|---:|---:|
| 1 | anvil | 364.11 | 157.13 | 143.19 | 14.73 / 19.46 | 0.00 |
| 1 | native | 513.52 | 236.11 | 204.33 | 18.93 / 24.82 | 2.88 |
| 1 | sqlite | 298.37 | 97.17 | 81.27 | 23.84 / 36.16 | 2.39 |
| 100 | anvil | 261.41 | 157.73 | 21.81 | 11.32 / 17.13 | 0.00 |
| 100 | native | 346.93 | 210.72 | 22.18 | 14.30 / 18.86 | 16.99 |
| 100 | sqlite | 248.55 | 108.04 | 18.01 | 19.35 / 25.02 | 3.04 |
| 1000 | anvil | 3697.52 | 1793.44 | 138.89 | 19.22 / 155.92 | 0.00 |
| 1000 | native | 2402.39 | 226.30 | 99.72 | 17.47 / 188.01 | 327.17 |
| 1000 | sqlite | 2727.26 | 94.00 | 114.56 | 150.60 / 321.00 | 107.76 |
| 483 | anvil | 391.35 | 200.69 | 21.09 | 11.45 / 16.47 | 0.00 |
| 483 | native | 452.52 | 199.50 | 18.61 | 14.42 / 19.92 | 39.06 |
| 483 | sqlite | 325.78 | 78.93 | 17.06 | 19.64 / 26.81 | 4.23 |
| spread128 | anvil | 1069.04 | 161.21 | 249.74 | 117.63 / 175.54 | 0.00 |
| spread128 | native | 2185.69 | 224.53 | 287.38 | 151.78 / 244.60 | 406.90 |
| spread128 | sqlite | 1000.00 | 88.44 | 184.75 | 153.80 / 247.50 | 15.24 |

## Mesures sur le volume Xeon / support rotatif

| Corpus | Backend | Total complet ms | 100 écritures isolées ms | 100 modifications groupées ms | Lecture aléatoire P50/P99 µs | Compactage ms |
|---|---|---:|---:|---:|---:|---:|
| 1 | anvil | 483.17 | 99.96 | 230.38 | 23.09 / 80.25 | 0.01 |
| 1 | native | 801.57 | 182.96 | 348.55 | 140.07 / 202.81 | 2.54 |
| 1 | sqlite | 722.94 | 134.90 | 217.01 | 240.57 / 317.09 | 2.76 |
| 100 | anvil | 490.33 | 154.88 | 72.97 | 54.44 / 83.72 | 0.00 |
| 100 | native | 703.99 | 220.91 | 109.59 | 107.35 / 160.01 | 44.17 |
| 100 | sqlite | 511.16 | 69.53 | 40.23 | 72.53 / 249.97 | 1.49 |
| 1000 | anvil | 7065.67 | 3425.26 | 267.31 | 31.86 / 241.63 | 0.00 |
| 1000 | native | 5700.39 | 147.92 | 365.42 | 39.44 / 608.76 | 835.34 |
| 1000 | sqlite | 6021.92 | 61.11 | 258.62 | 167.62 / 792.86 | 139.57 |
| 483 | anvil | 759.82 | 330.42 | 86.15 | 18.91 / 29.93 | 0.00 |
| 483 | native | 1138.24 | 260.40 | 103.66 | 75.51 / 125.98 | 71.14 |
| 483 | sqlite | 766.42 | 68.86 | 38.25 | 72.12 / 92.13 | 2.47 |
| spread128 | anvil | 1519.78 | 122.53 | 418.76 | 206.74 / 376.76 | 0.00 |
| spread128 | native | 3942.61 | 164.80 | 490.57 | 541.65 / 731.66 | 359.57 |
| spread128 | sqlite | 1903.95 | 54.26 | 268.42 | 464.45 / 663.13 | 12.30 |

Les totaux comprennent lectures, contrôles d'intégrité, écritures durables et
compactage, avec quantité de travail identique. Anvil n'a pas de compactage
supplémentaire à faire : chaque reconstruction le compacte déjà. Les lectures
chaudes Anvil ne font pas les mêmes contrôles SHA-256 de conteneur que native/
SQLite ; les coûts d'intégrité supplémentaires font partie des architectures.
Les caches internes diffèrent et sont bornés : huit index pour native/Anvil,
4 Mio de pages SQLite ; cache de NBT optionnel commun de 8 Mio/64 entrées.
Le modèle de référence des tests entre dans la mémoire RSS des trois backends.
Sur 1 000 chunks, RSS Xeon médian : Anvil 83 292 Kio, native 79 196 Kio,
SQLite 80 372 Kio ; CPU total médian : 6 458 / 5 608 / 5 966 ms respectivement.

## Taille, amplification et synchronisations

| Corpus Xeon | Backend | Allocation initiale / après modifications / après compactage Kio | Octets demandés aux écritures Mio | fsync + fdatasync | Temps sync cumulé ms (passe comptée) |
|---|---|---:|---:|---:|---:|
| 483 | anvil | 1988 / 1988 / 1988 | 53.715 | 231 + 0 | 80.51 |
| 483 | native | 392 / 848 / 388 | 4.233 | 381 + 0 | 66.59 |
| 483 | sqlite | 240 / 252 / 224 | 2.092 | 121 + 136 | 40.62 |
| 1000 | anvil | 24032 / 24032 / 24032 | 2598.641 | 245 + 0 | 813.34 |
| 1000 | native | 21424 / 23964 / 21388 | 47.566 | 369 + 0 | 106.18 |
| 1000 | sqlite | 22512 / 22580 / 22448 | 96.622 | 128 + 143 | 84.42 |
| spread128 | anvil | 4128 / 4128 / 4128 | 13.438 | 665 + 0 | 163.65 |
| spread128 | native | 8216 / 10940 / 8216 | 25.731 | 2396 + 0 | 551.99 |
| spread128 | sqlite | 2896 / 2964 / 2896 | 19.836 | 338 + 353 | 125.63 |

Allocation = st_blocks des fichiers **et répertoires**, WAL/SHM distingués ouverts
et fermés. Les compteurs d'écriture comprennent seed, mutations, checkpoints et
compactage. Ce ne sont ni les seuls payloads utiles ni des octets physiques
certifiés sur les plateaux du RAID. `/proc/self/io/write_bytes` est aussi conservé,
mais demeure un indicateur processus/système de fichiers.

Dans la région dense de 1 000 chunks, native demande environ **54,6 fois moins
d'octets d'écriture** qu'Anvil (2 598,64 → 47,57 Mio), SQLite environ 26,9 fois
moins. Cela ne donne pas une accélération équivalente : le total complet Xeon
passe de 7 065,67 à 5 700,39 ms pour native, soit environ 19,3 % de moins ; le
compactage natif prend 835,34 ms. Les 100 mises à jour isolées passent de
3 425,26 à 147,92 ms, mais SQLite est encore plus rapide pour cette phase
(61,11 ms). Les données opaques incompressibles limitent naturellement la taille.

Sur 483 chunks réels, SQLite termine en 766,42 ms contre 759,82 pour Anvil et
1 138,24 pour native. Après compactage, allocation 224 / 1 988 / 388 Kio
respectivement. Native ne gagne donc pas systématiquement, même avec moins
d'octets écrits. Les lectures aléatoires chaudes et le coût des métadonnées/
contrôles restent importants sur ce Xeon ; dans une passe 483, les médianes CPU
et murales sont proches (Anvil environ 18,9 µs, native 85,9 µs, SQLite 71,8 µs),
ce qui exclut d'attribuer ces lectures chaudes au seul temps disque.

Le cas sparse est défavorable à native : 386 fichiers, 8 216 Kio après compactage
contre 258 / 4 128 pour Anvil et 3 / 2 896 pour SQLite. Son index de 32 Kio par
région et les barrières de première création sont coûteux. Les anciennes versions
ne disparaissent pas sans compactage : un seul chunk passe de 52 à 856 Kio après
200 changements puis revient à 52 Kio. **Aucune politique automatique de compactage
n'est livrée**, le prototype ne doit pas être adopté pour une session longue sans
borne et ordonnanceur de maintenance supplémentaires.

Les durées de sync parfois plus basses sur le distant que sur NVMe ne prouvent
pas que les HDD physiques sont plus rapides : contrôleur/cache/virtualisation et
bibliothèques diffèrent, les garanties matérielles n'ont pas été certifiées.

## Compression, granularité et palettes

Médianes CPU Xeon, compression puis décompression de chaque corpus entier,
passages réversibles et byte-for-byte contrôlés :

| Profil | 483 historiques : taille/raw, compression/décompression CPU ms | 1 000 synthétiques : taille/raw, compression/décompression CPU ms |
|---|---:|---:|
| none | 1.0000, 0.65 / 0.51 | 1.0000, 9.31 / 24.45 |
| zlib1 | 0.0565, 47.04 / 6.98 | 0.3850, 626.55 / 82.70 |
| zlib6 | 0.0510, 49.48 / 8.13 | 0.3699, 790.75 / 73.39 |
| lz4 | 0.0678, 1.24 / 0.60 | 0.3742, 17.39 / 25.03 |
| zstd1 | 0.0527, 6.71 / 3.21 | 0.3678, 47.85 / 10.35 |
| zstd3 | 0.0520, 7.35 / 3.34 | 0.6300, 56.31 / 10.61 |
| zstd9 | 0.0494, 22.31 / 3.61 | 0.3626, 310.76 / 12.01 |
| lz4-16k | 0.0701, 1.27 / 0.59 | 0.4167, 25.23 / 7.47 |
| zstd3-16k | 0.0543, 7.37 / 3.38 | 0.3989, 122.64 / 22.56 |
| none-pack | 0.5502, 52.22 / 51.14 | 1.0001, 314.65 / 308.79 |
| lz4-pack | 0.0678, 52.63 / 50.85 | 0.3744, 325.61 / 311.91 |
| zstd3-pack | 0.0535, 59.36 / 54.57 | 0.6301, 366.53 / 319.17 |

LZ4 et Zstd1 constituent les pistes `fast` et `balanced` pour G2. Zstd9 peut être
`compact` sur certains corpus, mais aucune monotonie taille/niveau n'est supposée :
le cas opaque synthétique compresse moins bien avec Zstd3 qu'avec Zstd1. Zstd9
diffère également entre bibliothèques 1.5.4 et 1.5.7. Les manifeste/version sont
nécessaires à la reproduction ; pas de règle « niveau supérieur = toujours mieux ».
Le packing réversible réduit le NBT brut historique à 55 %, mais n'améliore pas
la taille compressée LZ4 et ajoute environ 51 ms de décodage CPU sur Xeon ; il
n'est pas recommandé comme défaut. Le framing 16 Kio n'apporte pas ici de gain
constant. Aucun dictionnaire, codec opaque séparé ou lecture de section ciblée
n'est annoncé comme livré. **Aucun profil de production n'est choisi ni activé.**

## Durabilité, pannes et intégrité

Native : append sur un bloc 4 Kio neuf, fsync données, index temporaire complet,
fsync index, rename, fsync répertoire, puis succès. L'ancien contenu n'est jamais
écrasé par une transaction ordinaire. Le backend natif conserve trois barrières
établies ; Anvil deux, SQLite WAL FULL plus sync répertoire et checkpoints
contrôlés. Les coûts supplémentaires de création/compactage sont inclus dans les
compteurs. Aucune synchronisation n'a été retirée pour favoriser un résultat.
Générations natives/SQLite croissantes, hash de préimage, propriétaire pthread
unique et verrou interprocessus protègent les snapshots et écrivains coopératifs.
L'adaptateur Anvil ne persiste pas de génération G1 (lecture zéro) : il vérifie
la préimage ; les générations du pipeline S4 restent dans son amont inchangé.
Ce prototype ne prétend pas ajouter à Anvil une détection ABA par génération.

| Couverture | Résultat |
|---|---|
| Native, 14 points SIGKILL, avant append jusqu'au reçu durable | Ancien ou nouveau batch entier selon la publication ; double relecture identique |
| SQLite, 7 points SIGKILL, transaction/COMMIT/checkpoint | Annulation ou rejeu WAL automatique ; même état aux deux récupérations |
| Compactage native 9 points et SQLite 4 points | Données actives conservées, compactage répété idempotent |
| EIO/ENOSPC pwrite et fsync, huit cas comptabilisés | Échec signalé, ancien ou nouveau batch cohérent ; aucun faux succès |
| Écritures courtes, EINTR pwrite/fsync | Retry ou échec sûr selon backend, jamais succès d'un état perdu |
| fsync répertoire après rename échoué | Code CLI 3, nouvelle région cohérente visible, durabilité explicitement incertaine |
| Index altéré, mauvaise identité/version, longueur/offset/troncature/payload | Refus explicite ; pas de régénération ni fallback |
| SQLite métadonnées/générations altérées et identité d'une autre racine | Hash/identité refusés malgré une base SQL structurellement lisible |
| Source obsolète, doublon, batch multi-régions, autre pthread, autre écrivain | Mutation refusée avant publication dangereuse |
| LZ4 MCA/codec inconnu/.mcc, ancien DV, destination présente/nestée/liée | Refus non destructif, aucune sortie publiée comme succès |

Les 42 événements détaillés réunissent crashs/compactages et huit erreurs ; les
autres cas de refus/retry sont couverts dans les 12 méthodes CLI. Une création
initiale interrompue peut laisser `.owner` sans index : refus fermé, inspection
requise. Les temporaires d'index restent conservés et ignorés ; aucun n'est promu
pour « réparer » une ambiguïté. Le compactage ne supprime les anciens fichiers
qu'après publication durable du nouvel index. Les checksums détectent une
corruption ; ils ne sont ni redondance réparatrice ni transaction monde globale.
SIGKILL ne simule pas une coupure électrique. La cohérence globale entre joueurs,
chunks, inventaires et objets au sol reste une limite connue.

## Aller-retour Vanilla et non-régression

Quatre essais distants : historique 483 chunks via native, complexe un chunk via
native et SQLite, puis complexe native après SIGKILL au point `durable` (tag de
test ajouté, génération conservée). Pour chacun : import, relecture, export exact,
deux redémarrages GoatCore Anvil async, deux ouvertures Vanilla 26.1.1, assertions
sur **tous les 98 304 états de chaque chunk source**, pas seulement démarrage/NBT
lisible. Les trois scénarios complexes conservent exactement les sept BE : coffre,
four froid, panneau, bannière, shulker, tonneau et pot décoré, dont inventaires et
composants. Les champs opaques sont exacts avant la normalisation propre à Vanilla.

Les 206 chunks full officiels ont en plus été archivés/exportés byte-for-byte par
native et SQLite. Ils ne constituent pas un export complet du monde d'origine :
les 1 122 protochunks restent dans ses régions sources protégées. Les fichiers
globaux, entités, POI et joueurs ne sont pas encodés par GoatWorld. Dans les essais,
ils sont copiés indépendamment et les ajouts de métadonnées manquantes sont
déclarés. Aucun monde personnel, aucune transaction multi-fichiers .mcc.

La matrice complète d'escaliers est rejouée sur les copies exportées :
**192 observations identiques entre GoatCore et Vanilla**, deux observateurs et
vérification des huit états persistés après redémarrage. Ce contrôle est distinct
des simples relectures de huit points des premiers aller-retours.

`make -B precommit-check GOATWORLD_G1_CHECK=1` réussit sur le staging distant :
29 suites C existantes, suite G1 (107 389 vérifications), tests migration,
smoke-start, réseau multi-epoll, login et suites Python pertinentes. Les 12 tests
CLI finaux sont aussi rejoués séparément sur le distant. ASan+UBSan et TSan locaux
réussissent la suite C G1 et les 12 tests CLI avec injections ; aucun diagnostic
dans les chemins exécutés, sans prétendre prouver l'absence de toute race. Les
bibliothèques système externes ne sont pas toutes instrumentées. Sonde I/O et dix
tests du profileur passent. Le contrôle des générés vérifie les rapports présents
et signale explicitement les sources brutes optionnelles absentes du staging.

Un premier precommit distant a échoué au contrôle d'hygiène : le drapeau existant
`GOATCORE_SOURCE_EXPORT=1` manquait dans la commande, qui a tenté une lecture Git
hors dépôt. Aucun clone/pull/checkout/push n'a été effectué. La commande a été
corrigée puis le contrôle complet a réussi ; cet échec de harnais n'est pas masqué.
Lors des pilotes, les noms réels des temporaires atomiques sans point initial et
la création de répertoires sous descripteur épinglé ont aussi révélé des défauts
corrigés avant les campagnes qualifiées.

## Décision pour G2 et risques restants

| Architecture | Point fort démontré | Limite principale | Coût / compatibilité |
|---|---|---|---|
| Anvil S4 | Lecture historique chaude rapide, format natif Vanilla, peu de fichiers sparse | Réécriture de toute la région, 2,60 Gio demandés sur le dense | Deux sync établies ; aucun export requis, S4 reste opérationnel |
| Native v1 | Append dense : très faible amplification, accès direct, bonne mémoire | Index sparse et compactage, plus de sync, lecture pas systématiquement meilleure | Trois sync ; import/export exact couvert mais format à maintenir et recovery à qualifier davantage |
| SQLite WAL FULL | Petites écritures rapides, peu de fichiers, compacité historique/sparse | Lecture dense au-delà du cache et WAL/checkpoint, dépendance externe | Transactions éprouvées, FULL vérifié ; export MCA requis, version corrigée à vérifier avant concurrence G2 |

**Recommandation G2 : prendre SQLite WAL FULL comme architecture principale de
référence expérimentale**, avec profils de compression explicitement choisis,
sans l'intégrer au serveur par défaut. Il gagne plusieurs scénarios réels et sparse
sans le coût d'une maintenance d'index/compactage propriétaire. Conserver le natif
segmenté comme candidat pour les régions denses, où son amplification et ses
lectures battent SQLite ; aucune supériorité générale n'est démontrée. Un futur
choix natif doit résoudre et mesurer l'index compact/sparse, les allocations/
contrôles dans la lecture et un compactage borné avant adoption. Ne pas choisir
GoatWorld v1 uniquement parce qu'il est propriétaire.

G2 doit élargir le corpus à des terrains officiels variés, fixer un budget cache
comparable, comparer LZ4/Zstd1 sur ces données, qualifier une version SQLite corrigée,
et définir une intégration S4 sans changer les reçus de durabilité. Le format
propriétaire reste un prototype d'étude, pas une promesse de stockage définitif.
Une bascule réelle exige sauvegarde, vérification complète, autorisation explicite,
rollback et nouvelles validations de charge/durabilité sur le matériel cible.
Aucune migration ni intégration automatique, aucune nouvelle phase gameplay.

Les preuves brutes sont dans `tests/stress/results/goatworld-g1/` (non versionnées),
avec commandes, versions, JSONL, CSV et empreintes. Le manifeste léger versionné
est `tests/fixtures/goatworld-g1-validation.json`. Les scripts corpus, benchmarks,
aller-retour et tests de panne permettent de rejouer les cas ; aucun monde complet,
SDK, secret ou gros log n'entre dans Git. Les SHA binaires testés sont consignés
dans le manifeste ; le binaire serveur de test reste identique à S3C.

### Empreintes et état final G1

| Binaire | SHA-256 |
|---|---|
| Outil normal NVMe | `dfe01e59d2d697e203e2b6e3485b5016e6152459e7dfc814227332e317e0fff4` |
| Outil normal Xeon | `a1f0cea96e5047f43f0e44ed3c37fd1a0aad68175ba830658cc55930b93c6c07` |
| Outil d'injection Xeon | `5a573e0dc184f2027a14c407fc134f751beeb15fd3f068a812381d38537b3093` |
| Serveur de test Xeon, inchangé depuis S3C | `190d9d727f15d127ea5c13fb83759223e12ff9237bbb73e89ca592dbc4373878` |

Le manifeste fournit aussi les SHA des exécutables C unitaires et des trois
exécutables ASan/UBSan et TSan. JAR interne officiel 26.1.1 :
`3647564632a2f589be2a87bf544f628e5f67cb3780c86f7fa972ec36892aa081`.
Les systèmes de fichiers testés exposent des blocs de 4 096 octets ; les garanties
de non-recouvrement du prototype ne sont pas qualifiées sur des blocs plus grands.

210 passes chronométrées qualifiées (105 par hôte), 18 passes comptées séparées,
plus 60 passes de profils distants préliminaires conservées mais exclues du choix.
Les 228 passes qualifiées confirment chacune 200 mutations (45 600 au total).
Les douze profils sont aussi comparés trois fois sur sept corpus et deux hôtes.
Ces chiffres décrivent les outils, pas des poses/casses Minecraft sous charge.

Contrôle final du 4 novembre 2026 vers 17:13 UTC : 25565 actif, PID 147, activation
inchangée à 15:21:30 UTC ; 25587 inactif et non redémarré. Le binaire principal
conserve `f840ed3f…`, la configuration online `59a6b486…`, offline `5d95cf99…`
(empreintes intégrales dans le manifeste). Aucun processus Minecraft temporaire
ne subsiste. Aucun service, monde, binaire principal, permission ou configuration
principale n'a été modifié. Aucun déploiement, aucune migration automatique et
aucun push. La validation positive d'authentification Mojang n'a pas été rejouée
et n'est pas revendiquée par ce lot de stockage.
