---
name: minecraft-testing
description: Exécuter et interpréter les tests Minecraft GoatCore avec bots Python, E2E, scénarios de blocs ou benchmarks. Utiliser pour toute validation serveur ou charge réseau/gameplay.
---

# Tests Minecraft GoatCore

Appliquer `AGENTS.md` et `remote-deploy` : toute exécution de `mc_server`,
des bots et des tests d'intégration se fait sur 10.0.4.66. Ne jamais utiliser
le port 25565 pour la charge automatisée.

Examiner le test et ses options avant exécution. `scripts/remote-bots.sh`
cible explicitement `127.0.0.1:25587` depuis le serveur distant et accepte
1 à 1000 bots, les modes `connect`, `movement`, `blocks`, `mixed`, `fastblocks`,
et une durée de 1 à 3600 s. `scripts/remote-test-server.sh` gère son service.
Les tests de blocs modifient le monde offline jetable ; les modes de blocs sont
limités à 50 bots par le script. `scripts/minecraft-e2e-test.sh` lance les
tests Node seulement avec `RUN_MINECRAFT_E2E_TESTS=1` et des dépendances
présentes ; l'exécuter sur le serveur distant, jamais localement.

Choisir une charge proportionnée et augmenter progressivement. Pour un test
destructif ou une réinitialisation, résoudre le monde ciblé avant toute
suppression. Ne pas interpréter un paquet de pose/casse envoyé comme un bloc
réellement modifié : comparer les confirmations ou l'état du monde. Capturer
les échecs, déconnexions, TPS/MSPT, ressources CPU/RAM et journaux, et
rapporter les commandes exactes et résultats observés. Un test non exécuté
doit être signalé comme tel ; 1000 bots n'est pas une capacité démontrée.

Pour l'authentification online, utiliser un vrai client Java officiel sur
25588 avant toute promotion de 25565. Les tests négatifs automatiques ne
prouvent pas une connexion positive ni les privilèges d'Ofwood_.
