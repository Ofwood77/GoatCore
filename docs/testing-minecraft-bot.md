# Bot de test Minecraft E2E

Ce dossier ajoute un bot QA Mineflayer isole pour tester GoatCore comme un vrai client Minecraft.

## Installation

```bash
cd tests/minecraft-bot
npm install
```

Node.js 22 ou plus est requis par les versions recentes de Mineflayer et `minecraft-protocol`.
Le depot C ne depend pas de Node pour compiler.

## Lancement

Par securite, les tests reels ne se lancent que si `RUN_MINECRAFT_E2E_TESTS=1` est defini.

```bash
RUN_MINECRAFT_E2E_TESTS=1 MC_HOST=127.0.0.1 MC_PORT=25565 ./scripts/minecraft-e2e-test.sh
RUN_MINECRAFT_E2E_TESTS=1 MC_TEST_MODE=movement ./scripts/minecraft-e2e-test.sh
RUN_MINECRAFT_E2E_TESTS=1 MC_TEST_MODE=inventory ./scripts/minecraft-e2e-test.sh
RUN_MINECRAFT_E2E_TESTS=1 RUN_DESTRUCTIVE_TESTS=1 MC_TEST_MODE=blocks ./scripts/minecraft-e2e-test.sh
RUN_MINECRAFT_E2E_TESTS=1 RUN_DESTRUCTIVE_TESTS=1 MC_TEST_MODE=full ./scripts/minecraft-e2e-test.sh
```

Variables utiles :

- `MC_HOST`, defaut `127.0.0.1`
- `MC_PORT`, defaut `25565`
- `MC_VERSION`, optionnel
- `MC_USERNAME`, defaut `TestBot`
- `MC_AUTH`, defaut `offline`
- `MC_TEST_MODE`, valeurs `smoke`, `movement`, `inventory`, `blocks`, `full`
- `MC_TEST_TIMEOUT`, defaut `60000`
- `MC_SERVER_LOG`, optionnel, chemin d'un log serveur a scanner
- `MC_ALLOW_UNSUPPORTED_MINEFLAYER=1`, optionnel, force une tentative meme si Mineflayer ne sait pas charger les donnees du protocole detecte

## Modes

- `smoke` : connexion, spawn, message chat, attente courte, deconnexion.
- `movement` : marche avant/arriere/gauche/droite, saut, sneak, sprint, pathfinder proche si disponible.
- `inventory` : observation inventaire, selection hotbar et equipement si un item existe.
- `blocks` : placement/cassage dans une zone de test, uniquement avec `RUN_DESTRUCTIVE_TESTS=1`.
- `full` : smoke, movement, inventory, puis blocks si autorise.

## Rapports

Chaque lancement cree un dossier :

```text
test-results/minecraft-e2e/<date>/
```

Fichiers produits :

- `summary.txt` : resume lisible
- `report.json` : resultats structures
- `bot.log` : lignes console du bot
- `server-errors.log` : lignes suspectes si `MC_SERVER_LOG` est fourni

## Securite

Utilisez de preference un serveur temporaire de test avec `online-mode=false`, whitelist desactivee et monde separe.
Ne lancez pas `blocks` ou `full` avec `RUN_DESTRUCTIVE_TESTS=1` sur un vrai monde public.
Le bot tente toujours une deconnexion propre en fin de test.

## Compatibilite GoatCore 26.1.1

`minecraft-data` `3.111.0` connait le protocole `775`, mais ne fournit pas encore de dossier de donnees chargeable pour `26.1.x`.
Le bot installe donc au demarrage un shim local, limite au processus Node, qui:

- declare `26.1.1` comme version Mineflayer experimentale;
- reutilise les donnees statiques proches de `1.21.11`;
- remappe les IDs de paquets depuis `data/26.1.1/reports/packets.json`;
- garde le major technique `1.21` pour reutiliser les implementations Prismarine existantes;
- desactive les plugins avances en mode `smoke`.

Le mode `smoke` est valide pour tester connexion, spawn, chat et deconnexion sur GoatCore `26.1.1`.
Les modes `movement`, `inventory`, `blocks` et `full` restent experimentaux tant que les structures completes des paquets play `26.1.1` ne sont pas generees.

## Limites actuelles

Le bot ne pilote pas encore RCON ni l'API d'une plateforme web. Les tests d'inventaire avances dependent donc d'un monde ou d'un inventaire deja prepare. Le protocole cible GoatCore `26.1.1` peut necessiter de laisser `MC_VERSION` vide si Mineflayer ne connait pas encore ce nom de version.

Si le shim local ne suffit pas pour un scenario avance, utilisez le test protocolaire minimal du depot pour verifier que le serveur accepte deja un client:

```bash
python3 tools/mc_ping.py status 127.0.0.1 25565
python3 tools/mc_ping.py login 127.0.0.1 25565 --username TestBot --timeout 5
```

`MC_ALLOW_UNSUPPORTED_MINEFLAYER=1` existe seulement pour tenter un debug experimental. Il peut echouer plus loin sur les registres, les dimensions ou le parsing des paquets.
