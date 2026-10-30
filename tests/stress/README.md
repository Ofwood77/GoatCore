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
id 1 par défaut, ou `--block-item-id` issu du registre cible) dans son hotbar via le paquet créatif 775, attend la confirmation
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

### Phase 2A : comparaison des placements (hôte distant uniquement)

`test_block_placement.py` attend une instance **temporaire offline** déjà
lancée, protocole 775, et un opérateur offline `Ofwood_`. Ce pseudo n'authentifie
aucun compte Microsoft. Utiliser un monde plat jetable et `MC_QA_MODE=1` /
`MC_ADMIN_USERNAME=Ofwood_` côté GoatCore ; sur Vanilla, un vrai `ops.json`
offline et `spawn-protection=0`. Les commandes administrateur préparent les
mêmes positions et états initiaux. Aucun bot au port 25565.

Exporter les sources depuis le PC avec `scripts/remote-build-stage.sh` (stdout :
chemin isolé créé sous `/opt/goatcore/`). Ce script réutilise le transfert tar
SSH des scripts de déploiement, exclut fichiers personnels, objets, résultats
et node_modules, et ne modifie aucun service principal. Compiler sur l'hôte
avec `GOATCORE_SOURCE_EXPORT=1 make -B precommit-check` dans ce répertoire.
Le rapport brut doit avoir été vérifié avec le datagen du JAR officiel 26.1.1 ;
le script le transfère s'il est disponible localement.

Exemples **depuis la copie distante**, une fois les serveurs temporaires prêts :

```sh
python3 tests/stress/test_block_placement.py --port 25589 --snapshot /tmp/goatcore-placement.json
python3 tests/stress/test_block_placement.py --port 25590 --vanilla --snapshot /tmp/vanilla-placement.json
```

Arrêter proprement chaque serveur puis le redémarrer sur son même monde,
sans réinitialiser les données joueur. Vérifier les blocs et le slot reçu :

```sh
python3 tests/stress/test_block_placement.py --port 25589 --snapshot /tmp/goatcore-placement.json --verify
python3 tests/stress/test_block_placement.py --port 25590 --vanilla --snapshot /tmp/vanilla-placement.json --verify
```

Les deux rapports JSON `cases` doivent être égaux. Les preuves de référence et
empreintes du JAR/rapport sont dans `tests/fixtures/vanilla-26.1.1-placement.json`.
Le scénario couvre un second observateur, Survival/Creative, axes, dalles,
remplacement d'herbe, placements refusés, seuil VarInt 127/128, coordonnées
négatives et frontières de chunks. Il ne prétend pas valider toutes les formes,
le voisinage ou l'ordre temporel exact des paquets Vanilla.

Pour des charges à travail fixé, réutiliser `benchmark_network.py --mode blocks
--bots N --workers 1 --cycles-per-bot 100 --duration 120 --block-cooldown 0.01`.
Le CPU du processus est calculé à partir de `/proc/PID/stat`, somme de tous ses
threads avant l'arrêt (100 % = un cœur), sur sa durée de vie avant shutdown.
Il exclut le bot Python ; les temps de sauvegarde détaillés restent dans les
logs `MC_PERF`. Toujours conserver aussi les essais en échec et leurs compteurs.

### Lot 2 : escaliers et voisinage (hôte distant uniquement)

Depuis la copie distante préparée avec le script d'export ci-dessus, le runner
gère une instance temporaire, son arrêt, son redémarrage et la relecture sur le
même monde. Les chemins de résultats doivent être nouveaux ; aucun fichier
des services principaux n'est utilisé comme destination. Les ports 25565 et
25587 sont interdits par le runner et le scénario d'escaliers.

```sh
python3 tests/stress/run_block_checks.py --kind goatcore --port 25589 \
  --server-binary ./mc_server.normal --result-dir tests/stress/results/stairs-new
python3 tests/stress/run_block_checks.py --kind vanilla --port 25590 \
  --vanilla-root /opt/goatcore/vanilla-2611 --result-dir tests/stress/results/vanilla-stairs-new
```

Le script renvoie un échec si le serveur, le scénario ou l'arrêt propre échoue.
Il conserve metadata.json (dont SHA du binaire GoatCore), report-edit.json,
report-verify.json, snapshot et journaux de chaque processus. Comparer les deux
report-edit.json complets : leurs `cases` et `saved` doivent être identiques ;
les deux report-verify.json doivent indiquer huit états restaurés.
La capture obtenue est versionnée dans `tests/fixtures/vanilla-26.1.1-stairs.json`.
`--suite placement` réutilise la suite du lot 1 avec la même procédure d'arrêt
et de relecture. Le runner se lance avec Python ; il ne requiert pas de bit
exécutable ni de modifications des scripts de déploiement existants.

La matrice des escaliers contient 192 observations : directions et moitiés,
faces latérales et seuil 0,5, quatre angles avec pose/rotation/casse du voisin,
veto latéraux, mélange oak/stone, eau source confinée, refus et forme partielle
contre le poseur. Les noms et propriétés sont interrogés dans le monde, en plus
des updates attendues sur deux connexions. Le scénario seul peut aussi cibler
un serveur temporaire déjà lancé :

```sh
python3 tests/stress/test_stair_placement.py --port 25589 --snapshot /tmp/stairs-snapshot.json
# Après arrêt propre et redémarrage du même monde :
python3 tests/stress/test_stair_placement.py --port 25589 --snapshot /tmp/stairs-snapshot.json --verify
```

Pour une charge d'escaliers, le benchmark accepte aussi `--block-item-id 442`
(oak_stairs du registre **26.1.1**, à revérifier lors d'un changement de version).
Le défaut reste stone=1. P50/P95/P99 sont rapportés pour les fenêtres complètes
MC_PERF de 100 ticks ; une charge trop courte peut n'en produire aucune.
`worst_active_100_tick_window` est sélectionnée par P95 parmi les fenêtres à
au moins 90 % du maximum de proto_ticks ; ce n'est pas un histogramme global.
Les logs stress stdout/stderr sont désormais conservés dans stress-one.log,
stress-two.log ou stress-four.log avec le rapport JSON et le journal serveur.
Une charge complétant ses actions ne démontre pas à elle seule le maintien
de 20 TPS. La section lot 2 de `docs/vanilla-compatibility.md` conserve les
mesures finales à 1/10/25/50 bots et leurs réserves.

### Profilage continu du tick et des attentes d'E/S (hôte distant uniquement)

`profile_tick.py` réutilise `Bot`, les cibles privées, les codecs 775 et le cycle
pose/casse de stress.py. Il attend **tous les 441 chunks** du rayon 10 de chaque
bot, prépare le slot et la cible, puis sépare échauffement (5 s), repos initial
(5 s), charge active à nombre fixe de cycles et repos final (5 s). Tous les bots
restent connectés pendant ces phases. Les connexions, lectures de chunks et
actions de préparation ne comptent pas comme actions mesurées.

Chaque exécution copie le même monde de référence immuable. Sans
`--world-template`, le script prépare d'abord ce monde avec un bot, puis conserve
son manifeste SHA-256. Les identités offline sont déterministes et identiques
entre copies. Les versions sont alternées à chaque répétition. Une action
incomplète, une déconnexion ou un arrêt non propre provoque un échec ; les
résultats bruts déjà écrits sont conservés. Les ports temporaires sont attribués
par le noyau ; 25565 et 25587 sont interdits.

Le binaire doit contenir le point de trace optionnel de src/net/server.c.
`MC_PERF=1 MC_PERF_TRACE_PATH=/chemin/frais/ticks.csv` enregistre **chaque tick**,
avec horodatages CLOCK_MONOTONIC, CPU du thread, passes rapides, phases du tick,
attentes de verrous, files et items. Le fichier est bufferisé et fermé à l'arrêt.
Sans cette variable, il n'y a ni écriture de trace ni lectures d'horloge ajoutées
par ce mécanisme. MC_PERF et ses logs préexistants restent indépendants.

Compiler la sonde optionnelle uniquement dans la copie de test distante :

```sh
cc -shared -fPIC -O2 -Wall -Wextra -o profile_fsync.so tests/stress/profile_fsync.c -ldl -pthread
python3 tests/stress/profile_tick.py \
  --binary reference=/chemin/reference/mc_server \
  --binary current=/chemin/avant-correction/mc_server \
  --bots 1 10 25 50 --repetitions 3 --cycles 100 \
  --fsync-probe ./profile_fsync.so --result-dir tests/stress/results/profile-comparison-new
```

La bibliothèque n'est préchargée que dans le processus enfant temporaire. Elle
mesure chaque fsync, son CPU, son résultat et son fichier sans changer la valeur
de retour ni errno. Elle ne doit jamais être injectée dans un service principal.
Pour comparer d'anciennes révisions, exporter celles-ci avec **Git local**, puis
appliquer seulement le même point de trace aux deux sources. Ne jamais utiliser
Git sur le serveur distant. Conserver les SHA des binaires et expliciter cet
ajout commun d'instrumentation ; ne pas comparer silencieusement une version
instrumentée à une autre sans instrumentation.

Les sorties comprennent ticks.csv, fsync.csv, resources.json (échantillonnage
250 ms : CPU serveur/client, mémoire, IO, schedstat/wchan des threads, pression
CPU/IO/mémoire), actions.json, bots.log, server.log et summary.json. comparison.json
conserve toutes les répétitions, pas seulement la meilleure. Le profil CPU gprof
doit être réalisé dans une passe séparée avec un binaire `-pg` ; ses temps ne
remplacent pas les mesures normales et ne représentent pas les attentes d'E/S.

TPS = nombre de débuts réels de tick dans la phase / durée monotone de la phase.
Une discrétisation d'un tick explique un petit dépassement de 20 sur une phase
courte. MSPT utilise les ticks complets qui chevauchent la phase, y compris un
éventuel dernier tick lent ; les ticks de bord sont comptés séparément. Les
percentiles sont à rang le plus proche et ne plafonnent pas à 1000 ms. Les passes
rapides sont distinctes du coût du tick programmé : tick_and_fast_ms et les
intervalles entre débuts de tick évitent de confondre MSPT et période réelle.
Les coûts imbriqués (handlers/flush, items/fsync) ne doivent pas être additionnés.
Les appels fsync chevauchant une frontière sont comptés entiers : leurs sommes
par phase ne sont pas strictement additives non plus. CPU/RSS sont échantillonnés,
pas une attribution exhaustive de tous les processus de l'hôte physique.

Tests purs des calculs : `python3 tests/stress/test_tick_profile.py`, sans serveur.
Pour vérifier un ramassage réel, deux observateurs et la sauvegarde/relecture :

```sh
python3 tests/stress/run_block_checks.py --kind goatcore --suite pickup --port 25589 \
  --server-binary ./mc_server.corrected --result-dir tests/stress/results/pickup-new
python3 tests/stress/run_block_checks.py --kind vanilla --suite pickup --port 25590 \
  --result-dir tests/stress/results/vanilla-pickup-new
```

Comparer les invariants des deux report-edit.json et report-verify.json : deux
objets lâchés/ramassés, quantité observée par les deux joueurs, pile de quatre
restaurée sans réinitialisation d'inventaire, puis après un nouveau processus.
Le regroupement éventuel des entités items par Vanilla n'impose pas un nombre
identique de TakeItemEntity ; c'est la somme des quantités qui est comparée.

Le même scénario accepte `--pickup-count 100` dans le runner : deux piles
préparées, 100 drops unitaires puis exactement 100 objets récupérés, vérification
de l'inventaire à la reconnexion et après redémarrage. Pour mesurer les attentes
du serveur GoatCore, ajouter `--fsync-probe ./profile_fsync.so`. Le runner écrit
ticks-edit.csv et fsync-edit.csv, puis des fichiers distincts pour verify.
Le scénario attend les chunks, confirme les piles vidées, utilise une lecture QA
comme barrière de préparation, vérifie qu'aucun ramassage n'a commencé, puis une
seconde barrière après les 100 ramassages. Les bornes sont dans `active_us` du
rapport edit. Le temps d'attente du délai normal de ramassage est inclus ; les
écritures de préparation et de déconnexion en sont exclues. Comparer les
invariants de quantité sur Vanilla ; les bornes temporelles ne doivent pas être
égalisées artificiellement entre serveurs.

Une sonde demandée mais absente ne vaut jamais zéro fsync : les chemins invalides
et l'absence de fichier de trace provoquent un échec. Sans sonde demandée,
profile_tick.py conserve `fsync: null` pour indiquer une métrique non mesurée.

### Profilage de persistance et interruptions contrôlées

Sur la copie de sources isolée de 10.0.4.66 uniquement :

```sh
make mc_save_bench mc_trace_io
python3 tests/stress/profile_persistence.py \
  --binary reference=./mc_save_bench.reference --binary candidate=./mc_save_bench.candidate \
  --repetitions 3 --result-dir tests/stress/results/persistence-new
python3 tests/stress/profile_persistence.py --crash \
  --binary reference=./mc_save_bench.reference --binary candidate=./mc_save_bench.candidate \
  --server reference=./mc_server.reference --server candidate=./mc_server.candidate \
  --result-dir tests/stress/results/crashes-new
```

Construire les deux révisions avec les mêmes options et sondes ; conserver les
copies de binaires avant toute recompilation. Le backend utilise le codec et le
store réels, sans socket. Il sépare la préparation, le chargement et la période
mesurée : monde inactif chargé, 200 modifications dans un chunk, 200 réparties
sur 16 chunks dans une même région logique, puis dans 16 régions distinctes.
L'API `.mca`, distincte du store actif du serveur, est exercée avec 1, 16 et
256 entrées présentes et 100 modifications du seul premier chunk. Les relectures
comparent toutes les cellules. Les fichiers `.bin` déterministes des deux
versions doivent être identiques octet par octet.

`MC_SAVE_TRACE_PATH=/chemin/trace.csv` active les phases murales/CPU par thread :
construction NBT, sérialisation, compression, ouverture/troncature, fwrite,
fflush/fclose, cache et opérations de région. Sans cette variable, aucun clock
n'est lu par la sonde. Elle préserve errno. Le CSV est tamponné et peut perdre sa
fin lors d'un SIGKILL ; il ne constitue aucune preuve de durabilité. La sonde de
cache mesure seulement les invalidations d'un cache non vide, pour exclure les
millions de cellules initialisées sans cache pendant le chargement.
Les durées imbriquées ne s'additionnent pas. Les erreurs sans phase terminée ne
doivent pas être comptées comme sauvegardes réussies.

`mc_trace_io` nécessite Linux x86_64 et ptrace autorisé pour **son enfant** ; il
ne s'attache jamais à un service existant. Il compte openat/read/write/pread/
pwrite/writev/close/lseek/fsync/fdatasync/ftruncate, avec fichiers et résultats.
Les répétitions sous ptrace sont réservées aux comptes et interruptions, car
leurs temps sont perturbés. Les volumes sont les octets remis au noyau ; les
écritures physiques et le journal ext4 ne sont pas mesurés par ce compteur.

Le mode crash crée six instances temporaires successives par version, ports
éphémères, mondes privés et opérateur offline de test. Il tue l'enfant avant
O_TRUNC, après O_TRUNC, avant write, après une écriture partielle injectée,
après write et après close. Il conserve le chunk interrompu, inspecte sa
lisibilité, puis teste la récupération sur **une copie**. Une lecture défaillante
peut provoquer une régénération et une perte des anciens blocs. Un fichier relu
après SIGKILL, même après close, ne prouve pas sa résistance à une panne de
courant : le cache du noyau est conservé dans cette expérience.

Pour la campagne bots, ajouter `--save-trace` à profile_tick.py. actions.json
conserve les instants individuels de poses/casses envoyées et confirmées ; les
phases enqueue_* et handler_* distinguent l'arrivée dans la file réseau, le
début et la fin du traitement. L'arrivée dans la file a une résolution de 1 ms
et suit le décodage réseau ; elle n'est pas l'instant exact d'arrivée sur la
carte réseau. Les latences client incluent ses propres attentes. Garder les
fenêtres continues et la même quantité d'actions pour les comparaisons.

run_block_checks.py accepte `--verify-restarts 3` pour relire le même monde
après trois arrêts/redémarrages successifs, sans refaire la préparation. Pour
une autre installation isolée du JAR officiel, préciser `--vanilla-root` et
`--java /chemin/java25`. Vérifier le version.json du JAR interne et son empreinte
avant de comparer : le même protocole 775 ne suffit pas à distinguer 26.1 de
26.1.1. Les résultats sur tmpfs, NVMe local et LVM distant doivent rester séparés.

### Remplacement atomique et erreurs de sauvegarde

Le nouveau `test_atomic_persistence` fait partie de `make test`. Les wrappers
système sont liés **uniquement** à ce test ; le serveur ne contient aucun
commutateur d'injection. Il vérifie les interruptions A–F sur les stores `.bin`
et `.mca`, EINTR/écritures courtes/EIO/ENOSPC/EDQUOT, les erreurs de fsync,
rename, close, lecture, allocation et verrouillage, les lecteurs avec ancien FD,
les écrivains concurrents, les données NBT opaques et les erreurs de région.
Pour conserver les fichiers de chaque cas sur un volume précis :

```sh
mkdir -p tests/stress/results/atomic-unit
MC_ATOMIC_TEST_ROOT="$PWD/tests/stress/results/atomic-unit" ./test_atomic_persistence
python3 tests/stress/profile_persistence.py --crash --atomic \
  --binary candidate=./mc_save_bench.candidate --server candidate=./mc_server.candidate \
  --result-dir tests/stress/results/atomic-server-new
```

Cette seconde commande crée six instances temporaires successives ; elle vérifie
les états autoritaires avant l'action et les états relus après chaque SIGKILL.
Les nouvelles étapes de mc_trace_io sont `temp-create-before`,
`temp-write-partial`, `temp-sync-before`, `rename-before`, `dir-sync-before` et
`dir-sync-after`. Les quatre premières doivent conserver l'ancien fichier
identique ; les deux dernières doivent exposer le nouveau fichier complet dans
ces essais de processus. Le cache noyau survit au SIGKILL : cela ne simule pas
une panne électrique, et l'étape avant fsync du répertoire n'acquitte pas la
durabilité. Les fichiers interrompus sont conservés avant la relecture.

Pour reproduire le défaut historique sur un exécutable du commit 8fd89fcc,
mc_save_bench accepte `region` (création de la fixture), `update-region`
(modification sans réinitialisation) et `inspect-region` (états rechargés).
Utiliser mc_trace_io `write-partial` sur `r.-1.-1.mca` : l'injection limite le
premier write à 32 octets puis tue l'enfant, avant que libc puisse réessayer.
Conserver l'image `.mca` initiale et comparer les trois chunks après le crash.
Cette reproduction ne doit jamais cibler le candidat corrigé comme s'il
utilisait encore l'ancien protocole d'écriture en place.

Une sauvegarde retourne 0 seulement après fsync du temporaire, fermeture
vérifiée, renameat et fsync du répertoire. Le statut
`MC_SAVE_DURABILITY_UNCERTAIN` (-2) indique un remplacement visible avec une
confirmation finale en échec. Le monde reste sale et le log identifie cette
incertitude ; il ne faut ni restaurer un ancien fichier ni effacer le nouveau.
Les retries utilisent le dernier état possédé par le tick. close(EINTR) n'est
pas réessayé sur Linux, pour éviter de fermer un FD réutilisé par un autre thread.

Les écrivains de régions utilisent un fichier `.mca.lock` persistant et flock,
qui couvre lecture, copie, modification et publication. Ne jamais supprimer ce
verrou pendant que des écrivains peuvent fonctionner. Les lecteurs continuent
sur leur ancien inode ou ouvrent le nouveau. Tous les écrivains doivent
coopérer : partager un même monde avec un autre serveur qui modifie les régions
en place reste hors contrat. Les snapshots en mémoire doivent être immuables
pendant leur sauvegarde et soumis dans l'ordre autoritaire ; aucun nouveau
worker de sauvegarde n'a été introduit.

Les `.tmp.PID.SEQUENCE` laissés par un SIGKILL sont ignorés, jamais promus ni
supprimés automatiquement. Les erreurs ordinaires tentent leur suppression et
signalent un échec de nettoyage. La copie utilise O_EXCL et ne réutilise aucun
fichier temporaire d'un autre écrivain. Une région contenant un marqueur `.mcc`
ou un codec non pris en charge est refusée avant publication ; aucun fichier
externe n'est modifié. Les en-têtes invalides ou secteurs en chevauchement sont
également refusés. Le NBT des autres chunks est copié octet par octet, sans
normalisation ni effacement des tags inconnus.

Pour les performances, garder les mêmes binaires/options, mondes initiaux,
charges et répétitions que profile_tick.py/profile_persistence.py. La nouvelle
API `.mca` copie **tout le fichier** à chaque remplacement ; son volume et son
coût croissent donc avec la région. Les nouveaux fsync ne doivent pas être
retirés pour retrouver l'ancien débit. La maintenance autorise exceptionnellement
les essais locaux de ce lot ; elle ne change pas les règles permanentes.
Reproduire les mesures sur Xeon/HDD RAID après maintenance avant tout déploiement.

Le chargement du store actif ne traite plus une erreur de lecture/décodage comme
une absence : il conserve le fichier, journalise l'échec et laisse le chunk
indisponible jusqu'au redémarrage après diagnostic/réparation. La génération
reste autorisée uniquement pour un fichier absent. test_anvil_roundtrip vérifie
qu'un fichier corrompu reste inchangé ; test_atomic_persistence injecte aussi
un EIO de lecture sur un fichier valide, vérifie sa conservation, puis le relit
correctement après redémarrage sans injection.

Pour vérifier les fichiers natifs du serveur officiel, sans importer son monde
dans GoatCore, le runner accepte `--anvil-rewriter ./mc_save_bench` avec
`--kind vanilla --suite stairs --verify-restarts 3`. Après l'arrêt du processus
et avant chaque redémarrage, il réécrit un slot par région via l'API atomique,
vérifie les SHA-256 des NBT décompressés de **tous** les chunks, puis fait relire
les états par Vanilla. En 26.1.1, le répertoire observé est
`world/dimensions/minecraft/overworld/region` ; les régions entities/poi sont
exclues. La fixture est toujours créée dans le résultat privé du runner.
mc_save_bench expose à cet effet `rewrite-region FICHIER LOCAL_X LOCAL_Z`.

## S1 : index de régions et allocation en mémoire

`make test_region_storage mc_region_bench` construit le test C et le lecteur de
régions. Le test utilise uniquement des fichiers temporaires. Le plan
`mc_region_plan_*` ne réalise aucune écriture : croissance et réservations sont
virtuelles, les anciens secteurs restent occupés après `retire`.

Le contrôle indépendant ci-dessous ne lance aucun serveur, ne modifie pas les
régions sources et refuse un répertoire de sortie déjà existant :

```sh
python3 tests/stress/check_region_storage.py \
  --binary ./mc_region_bench \
  --region-dir /chemin/monde-reference/dimensions/minecraft/overworld/region \
  --repetitions 30 \
  --result-dir tests/stress/results/region-index-neuf
```

Il génère une région synthétique (gzip, zlib, brut, LZ4/custom/inconnu opaques,
référence externe), compare chaque entrée et bit d'occupation, puis compare
**tous les octets NBT décompressés** avec un lecteur Python indépendant. Les
empreintes des fichiers sources doivent rester identiques. Aucun `.mcc` n'est
ouvert. Les NBT décodés et les mesures JSONL sont conservés dans la sortie.
Les erreurs structurelles et les invariants d'allocation sont couverts en C.

`mc_region_bench REGION REPETITIONS [REPERTOIRE_NBT_NEUF]` mesure séparément
open, lecture d'en-tête, index complet, lookup (un million par passe), lecture
brute/décodée d'un chunk et décodage de tous les chunks. Les durées JSONL sont
celles de la **phase entière** ; diviser uniquement le lookup par son nombre
d'opérations pour obtenir un coût par recherche. La première passe avec dump
est nommée séparément et exclue des statistiques de décodage sans sortie.
Le pilote lit/hash le fichier avant la mesure : cache chaud, aucun drop_caches.
Les P95/P99 sont des quantiles empiriques, pas des bornes de latence HDD.

Pour une nouvelle référence native, utiliser le runner Vanilla existant sur une
instance isolée autorisée, puis ces régions après arrêt propre. La comparaison
S1 a réutilisé les quatre régions historiques (1 294 chunks) et a également
réécrit les quatre régions d'une nouvelle exécution officielle entre trois
redémarrages. Le rapport détaillé est dans `docs/vanilla-compatibility.md`.
Le backend atomique reste celui utilisé par défaut ; aucun journal ni writer
incrémental n'est activé. Les règles habituelles d'exécution distante restent
applicables hors de l'exception de maintenance explicitement autorisée.

## S2 : backend incrémental expérimental et récupération

Le module transactionnel n'est **pas lié à mc_server**. Le stockage actif `.bin`
et le backend atomique restent inchangés. Construire explicitement :

```sh
make mc_region_txn test_region_txn mc_trace_io
./test_region_txn
./mc_region_txn --experimental-disposable init /chemin/tests-jetables-neuf
```

`init` refuse un répertoire existant ; ne jamais créer son marqueur dans un
monde personnel. Le backend exige ce marqueur et une propriété exclusive.
Les tests d'interruption conservent leurs fichiers dans MC_REGION_TXN_TEST_ROOT
(`/tmp` par défaut). Ils n'arrêtent aucune machine physique ni service existant.

Comparaison à travail fixé, trois répétitions et comptes système séparés :

```sh
python3 tests/stress/profile_region_txn.py \
  --tool ./mc_region_txn --fault-tool ./test_region_txn --tracer ./mc_trace_io \
  --result-dir tests/stress/results/s2-neuf --repetitions 3 --operations 100
```

Le pilote crée uniquement des répertoires neufs et teste 1/256/1024 chunks,
modifications d'un chunk, d'une région et de quatre régions. Les NBT de contrôle
contiennent DataVersion, S2Value et un champ opaque, pour isoler le stockage ;
ce ne sont pas des mondes jouables. Le pilote compare tous les NBT, mesure
CPU/mural, sérialisation, compression/phases, syscalls, volumes, fragmentation,
puis interrompt et récupère les fichiers originaux. Il ne lance aucun serveur.
Les durées sous ptrace ne servent pas à mesurer la vitesse. Aucun gain TPS
n'est déduit de ce backend encore séparé.

Le runner Vanilla existant accepte `--s2-experimental --anvil-rewriter
./mc_region_txn` pour les fermetures propres, ou `--anvil-rewriter
./test_region_txn` pour interrompre une écriture d'en-tête puis **récupérer avant
chaque redémarrage Vanilla**. Ce drapeau arme uniquement le répertoire fraîchement
créé par le runner. Les régions fournies de l'extérieur et services principaux
ne sont jamais armés par ce runner. La maintenance autorise temporairement les
instances locales ; les règles distantes s'appliquent de nouveau après celle-ci.

Avant toute ouverture Vanilla d'une région interrompue, arrêter les écrivains,
conserver les originaux, récupérer sur leurs inodes d'origine :

```sh
./mc_region_txn --experimental-disposable recover /chemin/tests-jetables
```

Exiger succès et absence de journaux en attente, puis vérifier les NBT avant
export. Le journal/état auxiliaire corrompu bloque les écritures et reste
conservé. Ne pas le supprimer ni copier une transaction inachevée vers un autre
inode pour contourner l'identité. Le rapport de compatibilité décrit le format,
les décisions undo/redo, les barrières et les garanties non démontrées.

### S3A : stockage actif Anvil optionnel

Le défaut reste `chunk-storage=bin` dans `server.config`. Pour une **copie de
monde jetable**, sélectionner explicitement :

```ini
chunk-storage=anvil-atomic-experimental
```

`MC_WORLD_PATH` désigne la racine du monde ; les régions utilisées sont
`dimensions/minecraft/overworld/region/r.<x>.<z>.mca`. Aucun import des `.bin`,
aucune migration ni bascule après erreur. Le mode Anvil crée un marqueur de
sélection durable et détient des verrous de monde/session/région. Ne pas ouvrir
simultanément ce monde dans Vanilla. Un journal S2 en attente interdit son
ouverture : appliquer d'abord la procédure S2 ci-dessus sur les fichiers
originaux, écrivains arrêtés ; S3A n'effectue aucune récupération S2 automatique.

Au terme de S3A, lecture native limitée aux chunks `minecraft:full`, DataVersion
4788, hauteur Overworld, palettes connues et sans block entities. Protochunks,
NBT ambigu/inconnu non conservable, codecs non pris en charge et références
`.mcc` provoquent un refus ; le chunk reste indisponible, sans régénération.
La présence d'un codec externe/non pris en charge dans la région interdit
également sa réécriture. Les sauvegardes conservent les NBT opaques et les
palettes existantes ; les heightmaps/lumières dérivées affectées sont invalidées
pour recalcul par Vanilla. Ne pas utiliser les conteneurs dans ce mode : une
sauvegarde nécessitant des block entities est refusée et reste dirty. S3B,
décrit plus bas, étend leur conservation opaque sans activer leurs interactions.

Le runner existant sait sélectionner ce backend et copier un monde de référence
sans modifier l'original :

```sh
python3 tests/stress/run_block_checks.py --kind goatcore --suite stairs \
  --chunk-storage anvil-atomic-experimental --port 25593 \
  --server-binary /chemin/binaire-fixe/mc_server --verify-restarts 3 \
  --result-dir tests/stress/results/s3a-stairs-neuf
```

Pour une copie officielle, ajouter `--world-template /copie/reference/world
--view-distance 2`. Les suites `placement` et `pickup` utilisent le même runner.
`placement --blocks-only` omet uniquement le test distinct de BlockEvent shulker ;
`placement --region-boundaries` vérifie deux observateurs, les inventaires
Survival, les coordonnées ±512 et plusieurs régions. Ce dernier scénario envoie
une position client après les téléportations, afin que Vanilla actualise son
suivi des chunks éloignés.

Réouverture officielle sans rejouer les modifications : `--kind vanilla
--verify-only --world-template /copie/modifiee/world --snapshot-template
/capture/snapshot.json --suite stairs --java /chemin/java25 --vanilla-root
/installation/26.1.1 --port 25594`. Exiger succès des assertions puis comparer les
blocs de tous les chunks modifiés, palettes et métadonnées. Le programme
`native_storage_oracle.py` fournit un lecteur NBT indépendant, un digest des
98 304 blocs par chunk (`--backend bin` choisit explicitement l'ancien packing
compact), et `--preserved-from` vérifie la conservation des champs non modifiés
et des fichiers globaux/POI/entités. Il ne modifie aucun monde.

Un monde nouvellement créé par GoatCore ne contient pas toutes les métadonnées
globales Vanilla. Pour tester ses régions avec Vanilla, utiliser une **copie
complète** des métadonnées d'un monde officiel : `level.dat` seul ne suffit pas
à 26.1.1, qui requiert notamment `data/minecraft/world_gen_settings.dat`.
Ne pas présenter ces essais de chunks comme une compatibilité mondiale complète.

Comparaison à travail fixé, avec le même binaire et des templates indépendants
vérifiés bloc par bloc avant mesure :

```sh
python3 tests/stress/profile_tick.py \
  --binary bin=/chemin/mc_server --binary anvil=/chemin/mc_server \
  --storage-backend anvil=anvil-atomic-experimental \
  --bots 1 10 25 50 --repetitions 3 --cycles 100 --save-trace \
  --fsync-probe /chemin/profile_fsync.so \
  --result-dir tests/stress/results/s3a-charge-neuve
```

Les fichiers de trace couvrent toute la période active, préparation et idle
séparés ; les ressources et compteurs d'E/S sont conservés. Le chemin du binaire
est immuable pendant toute la campagne. S2 n'est pas lié dans `mc_server` et
n'intervient dans aucune de ces sauvegardes. L'exception locale n'est valable
que pendant la maintenance ; répéter ces validations sur Xeon/HDD RAID avant
tout déploiement. Aucun chiffre local ne démontre un gain sur ce matériel.

Pour comparer tous les blocs après une réouverture officielle, indépendamment
du réencodage des palettes, utiliser `native_storage_oracle.py APRES_WORLD
--same-blocks-as AVANT_WORLD --output preuve.json`. Les options répétées
`--chunk=-2,0 --chunk=-1,0 --chunk=-2,1 --chunk=-1,1` limitent cette comparaison
aux chunks modifiés d'une référence officielle ; sans sélection, tous les
chunks de la copie source sont vérifiés.

### S3B : conservation opaque des block entities

Appliquer les mêmes règles d'isolement que S3A. L'autorisation locale est une
exception temporaire pendant la maintenance, pas une modification des skills.
Les ports 25565 et 25587 ne sont pas utilisés. Les fixtures officielles se
préparent avec le runner existant, Java 25 et le serveur exact 26.1.1 :

```sh
python3 tests/stress/run_block_checks.py --kind vanilla --suite complex \
  --java /chemin/java25 --vanilla-root /installation/26.1.1 \
  --level-type minecraft:normal --level-seed 0 --port 25593 \
  --result-dir tests/stress/results/s3b-reference
python3 tests/stress/run_block_checks.py --kind goatcore --suite complex \
  --server-binary /chemin/binaire-fixe/mc_server \
  --chunk-storage anvil-atomic-experimental \
  --world-template tests/stress/results/s3b-reference/runtime/world \
  --view-distance 2 --verify-restarts 3 --port 25594 \
  --result-dir tests/stress/results/s3b-modifie
```

Le scénario crée sept block entities par commandes **Vanilla**, puis vérifie
une pose Survival éloignée, deux observateurs, l'inventaire et les refus de
casse/interactions/création de BE. `--complex-dense` prépare 160 BEs ; le défaut
de génération reste plat, `--level-type minecraft:normal` élargit le corpus.
Reprendre ensuite la procédure `--kind vanilla --verify-only` de S3A avec
`--suite complex`, la copie modifiée et son `snapshot.json`. La conservation
ne doit pas être déduite de ce seul test de présence du bloc :

```sh
python3 tests/stress/native_corpus.py /copie/apres/world \
  --compare-entities /copie/avant/world --chunk=-1,1 --output be-proof.json
python3 tests/stress/native_storage_oracle.py /copie/modifiee/world \
  --preserved-from /copie/reference/world --output preservation.json
```

La comparaison BE vérifie types et valeurs NBT exacts, indépendamment de l'ordre
des clés des compounds. Les autres chunks doivent garder leurs NBT bruts.
Pour les tags arbitraires synthétiques, comparer aussi un témoin directement
réouvert dans Vanilla : son sérialiseur peut les normaliser ou les supprimer.
Ne pas attribuer cette normalisation à GoatCore.

`tools/audit_vanilla_storage.java` interroge les registres du JAR officiel,
avec son classpath de bibliothèques. Conserver la sortie, puis régénérer via
`python3 tools/generate_native_storage.py --audit audit.txt --server-jar
/chemin/server-26.1.1.jar`. Le SHA exact est exigé. `--check` vérifie la table
sans Java, à partir de la fixture versionnée.

`make mc_native_bench` construit un probe réservé aux copies contenant le
marqueur `.goatcore-s3b-disposable`. Aucun lancement serveur n'est nécessaire.
`check_native_corpus.py --probe PROBE --reference WORLD7 --dense WORLD160
--normal WORLDNORMAL --external WORLDMCC --result-dir NOUVEAU` prépare quatre
cas et vérifie 18 refus sur copies. Les variantes opaques/malformées sont
**synthétiques**. La fixture externe se prépare séparément avec
`tools/write_external_fixture.java`, l'API RegionFile officielle et un dossier
jetable ; elle ne représente pas une référence naturellement générée.

Pour mesurer 100 sauvegardes par cas, trois répétitions alternées :

```sh
python3 tests/stress/profile_native_storage.py \
  --probe /chemin/S3B/mc_native_bench --baseline-probe /chemin/S3A/mc_native_bench \
  --cases /resultats/corpus/cases.json --writes 100 --repetitions 3 \
  --fsync-probe /chemin/profile_fsync.so --result-dir /resultats/bench-nouveau
```

Le même probe de test est lié aux deux révisions ; S3A n'accepte que le cas
sans BE. Chaque sauvegarde est relue, les NBT non concernés comparés et les
traces brutes conservées. Arrêter les autres instances de test et compilations
pendant les mesures. Les encodages séparés et sauvegardes complètes se
recouvrent : ne pas additionner leurs durées. VmHWM vient de `/proc/self/status`
après exec ; ru_maxrss peut hériter du pic du processus parent.

La fixture `tests/fixtures/vanilla-26.1.1-s3b-corpus.json` référence les captures,
empreintes des régions, rapports et binaires réellement testés. Les mondes,
traces et journaux volumineux restent dans `results/storage-s3b/`, ignoré par
Git. Protochunks, types BE non couverts, ticks actifs et `.mcc` restent protégés
ou refusés. Aucune simulation de BE, migration ou activation S2 ; `.bin` reste
le défaut. Voir le rapport S3B pour les garanties et les limites exactes.
