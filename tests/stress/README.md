# Stress multijoueur GoatCore (26.1.1 / protocole 775)

Client Python indépendant du serveur C. Il reprend le cadrage des paquets de
`tools/mc_ping.py` et les identifiants de `src/generated/generated_minecraft_ids.h`.
Mineflayer et son shim dans `tests/minecraft-bot/` ont été examinés ; ils restent
adaptés aux tests QA, mais nécessitent Node 22 et ne sont pas requis ici.

## Installation et lancement

Python 3.10+ et sa bibliothèque standard suffisent. Aucune dépendance, aucun
`.venv` et aucune installation globale ne sont nécessaires.

Sur un **monde temporaire** et un port dédié :

```sh
MC_BIND_PORT=25587 sh scripts/start-flat-test-world.sh
```

Ce lanceur crée à chaque fois un monde plat neuf sous `/tmp`, avec un sol à
Y=63 et un spawn à Y=64. Il évite les anciens chunks ou positions joueur
sauvegardés dans le vide, sans effacer le monde `world/` du dépôt.

Dans un autre terminal :

```sh
python3 tests/stress/stress.py --bots 5 --duration 30 --port 25587 --mode movement
python3 tests/stress/stress.py --bots 5 --duration 30 --port 25587 --mode blocks --allow-block-changes
python3 tests/stress/stress.py --bots 50 --duration 120 --port 25587 --mode connect
```

Le test peut aussi coexister avec un vrai client sur le même serveur. Pour
confirmer les déplacements au moyen de l'état autoritaire (facultatif), démarrer
le serveur de test avec `MC_QA_MODE=1 MC_ADMIN_COMMANDS=1` et ajouter
`--qa-verify-movement`. Sans cela, `movements_confirmed` vaut `null`, car un
envoi de paquet ne prouve pas que le serveur l'a traité.

Modes : `connect` (présence), `movement` (marche à 5 blocs/s maximum autour du
spawn), `blocks` et `mixed` (cycle pierre sur place, avec vérification QA si
demandée). Chaque exécution donne aux bots une identité neuve pour éviter de
restaurer une ancienne position joueur : ils arrivent tous au spawn du monde.
En mode mouvement, ils décrivent des trajectoires déphasées autour du spawn ;
`--radius` règle leur rayon. En mode blocs, ils restent au spawn et reçoivent
chacun une cible distincte à portée (50 maximum), sans se téléporter.
`--block-cooldown` laisse par défaut **1 seconde après chaque pose et chaque
casse** pour que l'alternance soit visible. `--interval` ajoute une pause entre
les cycles complets.

Pour un test de débit, le mode `fastblocks` plafonne les envois à 400 poses/s
et 400 casses/s au total, sans attendre la confirmation après chaque action.
Les rapports distinguent les pics envoyés des pics réellement confirmés sur
une seconde. La commande distante `./scripts/remote-bots.sh 50 fastblocks 30`
applique ces réglages.

`--bots` est limité par `--max-bots` (1000 par défaut) en modes connexion et
mouvement ; les modes blocs restent limités à 50 cibles distinctes près du
spawn. `--spawn-delay`,
`--interval`, `--block-cooldown`, `--block-rate`, `--timeout`, `--host`, `--port`, `--duration`, `--report` et
`--server-log` permettent d'ajuster et d'analyser le scénario. Les modes blocs
requièrent `--allow-block-changes`. Pour une adresse autre que loopback, il faut
**en plus** `--allow-remote-destructive`. `--shared-zone` est facultatif et
introduit volontairement des conflits ; les changements observés dans cette zone
ne sont alors **pas** attribués à un bot précis et ne sont pas comptés comme
poses/casses vérifiées.

Les modes blocs demandent un monde créatif : chaque bot place de la pierre (item
id 1) dans son hotbar via le paquet créatif 775, attend la confirmation
d'inventaire, pose puis casse à partir d'un bloc support. Une action n'est comptée
qu'après une mise à jour `BlockUpdate` portant l'état attendu. Le monde sera
modifié pendant le test : utilisez un monde jetable ou une copie. La préparation
de la zone peut également casser un bloc préexistant.

Chaque exécution écrit `results/<horodatage>/report.json`, `summary.txt` et
`bots.log`. Le chemin `--report` ajoute une copie JSON. Les fichiers de résultats
sont ignorés par Git. `--server-log` analyse les lignes `MC_PERF` déjà présentes
dans un journal du serveur : les ticks détaillés sont échantillonnés et ne
constituent pas un histogramme exhaustif. Sans journal, ces statistiques sont
`null` ; elles ne sont jamais inventées.

## Validation

```sh
python3 -m unittest discover -s tests/stress -p 'test_*.py'
```

Monter progressivement à 1, 5, 10, 25, 50, puis 100, 250 et au-delà en modes
connexion/mouvement. Vérifier les rapports,
les déconnexions, la réactivité d'un vrai client (contrôle manuel), et le journal
serveur. Ne pas lancer directement plusieurs centaines de bots. Le protocole
775 est pris en charge seulement pour les paquets nécessaires à ces scénarios ;
ce client ne remplace pas un client Java complet (authentification online-mode,
resource packs et mécaniques de jeu générales non implémentés).
