# GoatCore 26.1.1 — compatibilité Vanilla (phase 2A : lots 1 et 2)

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
