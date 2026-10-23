# GoatCore 26.1.1 — compatibilité Vanilla (phase 1B)

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
tests Python ne couvrent que l'API protocolaire partielle de GoatCore. Les
Les mondes de test restent jetables.

Références externes primaires : [Minecraft Java 26.1.1](https://www.minecraft.net/ja-jp/article/minecraft-java-edition-26-1-1),
[Java `UUID.nameUUIDFromBytes`](https://docs.oracle.com/en/java/javase/26/docs/api/java.base/java/util/UUID.html).
