# Serveurs GoatCore sur 10.0.4.66

Depuis la racine du dépôt local, **uniquement après une connexion réussie avec un client Java officiel sur le port temporaire 25588** :

```sh
./scripts/deploy-remote.sh
./scripts/remote-test-server.sh start
./scripts/remote-bots.sh 5 movement 30
./scripts/remote-test-server.sh stop
```

Le déploiement compile sur la machine distante et relance le serveur normal
`10.0.4.66:25565` en `online-mode=true`. Son monde est dans
`/var/lib/goatcore/world`.
Il installe aussi le service de test, mais ne le démarre pas. Si celui-ci était
déjà actif, il est redémarré avec le nouveau binaire.

Le serveur de test utilise `10.0.4.66:25587` en `online-mode=false` et un monde séparé, persistant
dans `/var/lib/goatcore/test-world`. `start` ne détruit pas ce monde ; `stop`
ne touche pas au serveur normal. Voir son état ou ses logs :

```sh
./scripts/remote-test-server.sh status
./scripts/remote-test-server.sh logs
```

Les services chargent respectivement `/etc/goatcore/server1.setup.conf` et
`/etc/goatcore/server2.setup.conf`. Le déploiement crée ces fichiers s'ils
n'existent pas, puis préserve vos modifications. Sur `10.0.4.66`, le premier
utilise les CPU logiques `0-3` avec 2 workers chunks ; le second utilise `6-7`
avec 1 worker chunk. Les CPU `4-5` ne sont pas autorisés par le masque Linux
actuel de cette machine. Modifier un fichier puis redémarrer le service voulu :

```sh
ssh root@10.0.4.66 'systemctl restart goatcore-test.service'
```

Les deux processus utilisent le même binaire compilé, mais des répertoires de
travail distincts (`/opt/goatcore/app/instances/online` et `offline`) et donc
des `server.config` distincts. Le déploiement initialise ces fichiers depuis
l'ancienne configuration si elle existe, force le mode voulu lors de cette
initialisation, puis préserve les réglages des déploiements suivants. Les
modèles sont dans `config/server-online.config` et `config/server-offline.config`.
Les journaux restent séparés par unité systemd. Il faut vérifier les deux
services et les commandes d'Ofwood_ après une bascule ; la présence de
`MC_ADMIN_USERNAME` ne prouve pas à elle seule le fonctionnement des droits.

Les bots tournent sur la machine distante et ciblent uniquement `127.0.0.1:25587`.
Leur lancement est toujours manuel. Arguments : nombre (1 à 1000), mode
(`connect`, `movement`, `blocks`, `mixed`, `fastblocks`), durée en secondes (1 à 3600).
Tous les modes exigent que le serveur de test soit actif. Les modes `blocks`
et `mixed` modifient uniquement ce monde de test et restent limités à 50 bots
pour garder une cible distincte à portée de chacun. Le serveur de test accepte
au maximum 1000 connexions (`MC_MAX_CONNECTIONS=1000`), contre 128 par défaut
sur le serveur normal. Exemple de montée en charge :

```sh
./scripts/remote-bots.sh 100 connect 60
./scripts/remote-bots.sh 250 connect 90
./scripts/remote-bots.sh 1000 connect 180
```

Le lanceur échelonne les connexions de 20 ms. 1000 est une limite de test,
pas une capacité validée : surveiller les erreurs et la mémoire, et monter
progressivement. Les modes mouvement et blocs à forte charge coûtent bien
plus que les simples connexions.

Les deux services configurent les commandes d'administration **implémentées par
GoatCore** pour le pseudo exact `Ofwood_` via `MC_ADMIN_USERNAME`. Ce n'est pas
un OP vanilla complet : seules les commandes présentes dans GoatCore existent.
Sur 25565, cette attribution n'est sûre qu'après vérification réelle de la
session Mojang et de l'UUID officiel. Sur 25587, le pseudo peut être usurpé
par tout client ayant accès au port offline. `MC_ADMIN_COMMANDS=1`, qui
accorderait ces droits à tous, reste désactivé.

Pour le test rapide de blocs, le mode `fastblocks` n'attend pas une confirmation
avant l'action suivante. Il plafonne séparément les envois de poses et de
casses à 400/s **au total pour tous les bots**. Le rapport distingue les pics
de paquets envoyés des poses/casses confirmées dans une fenêtre d'une seconde ;
un envoi ne prouve pas que le serveur a appliqué l'action.

Mesures du 2026-10-19 sur le serveur de test : 150 bots en mode `connect`
restaient connectés sans échec ni déconnexion pendant 20 s. Le test
`fastblocks` avec 50 bots a atteint au mieux 252 poses/s et 250 casses/s
envoyées, mais seulement 168 poses/s et 163 casses/s confirmées sur une
fenêtre glissante d'une seconde. Le plafond de 400/s est donc disponible pour
le générateur de charge, mais **400 changements/s réellement appliqués ne sont
pas démontrés**. 1000 bots simultanés n'ont pas été validés.

```sh
./scripts/remote-bots.sh 50 fastblocks 30
```

Les résultats restent sous `/opt/goatcore/app/tests/stress/results/` sur la
machine distante. Aucun `.venv` n'est utilisé. Pour une autre machine :

```sh
./scripts/deploy-remote.sh root@adresse
GOATCORE_REMOTE=root@adresse ./scripts/remote-test-server.sh start
GOATCORE_REMOTE=root@adresse ./scripts/remote-bots.sh 5 connect 30
```
