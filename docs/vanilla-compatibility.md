# GoatCore 26.1.1 — compatibilité Vanilla (phase 1)

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
| Hello S→C `0x01`, Key C→S `0x01` | Login | Non | Non : chiffrement/authentification online absents | Non |
| Login finished S→C `0x02` | Login | Oui | Propriété `textures` signée facultative en mode offline ; rendu client Java non vérifié | Unitaire + intégration + API réelle |
| Login acknowledged C→S `0x03` | Login | Oui | Oui pour paquet vide et transition unique | Unitaire |
| Login compression S→C `0x03` | Login | Non émis | Non testé avec clients officiels ; compression désactivée | Test de framing seul |
| Select known packs S→C `0x0e`, C→S `0x07` | Configuration | Oui | Partiel : liste fixe, réponse bornée et non répétable | Client Python |
| Registry data S→C `0x07` | Configuration | Oui, blobs 26.1.1 | Non validé contre capture Vanilla | Client Python |
| Update tags S→C `0x0d` | Configuration | Oui, blob 26.1.1 | Non validé contre capture Vanilla | Client Python |
| Finish configuration `0x03` dans les deux sens | Configuration | Oui | Partiel : ordre contrôlé, payload C→S vide | Client Python |
| Autres paquets Configuration | Configuration | Partiel/ignorés | Non validé | Non |
| Join game et synchronisation initiale | Play | Partiel | Non validé avec client officiel | Stress Python |

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
- `online-mode=true` est reconnu dans `server.config`, mais **refusé au
  démarrage**. Le mode online n'est donc pas implémenté ; il n'y a aucun repli
  offline. `USE_OPENSSL=1` ne suffit pas : le flux doit être déchiffré avant
  le décodage des trames, et la session Mojang doit être vérifiée avant Login
  Success. L'implémentation crypto actuelle chiffre seulement des portions de
  paquet et appelle `EVP_*Final_ex` à chaque paquet ; elle n'est pas utilisable
  pour le flux CFB8 Minecraft.

## Restes de la phase 1

À valider/corriger par étapes : comparaison binaire avec serveur et client
Java 26.1.1 officiels, authentification online (RSA/AES CFB8, contrôle de
session et profil), déconnexions explicites pendant Login, validation complète
de Configuration, cycle de vie/reconnexion/sauvegarde du joueur, spawn,
mouvements, visibilité multijoueur, mort/respawn, propriétés du profil. Les
tests Python ne couvrent que l'API protocolaire partielle de GoatCore. Les
mondes actifs 25565/25587 ne sont pas redéployés par cette phase.

Références externes primaires : [Minecraft Java 26.1.1](https://www.minecraft.net/ja-jp/article/minecraft-java-edition-26-1-1),
[Java `UUID.nameUUIDFromBytes`](https://docs.oracle.com/en/java/javase/26/docs/api/java.base/java/util/UUID.html).
