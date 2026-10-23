# GOATCORE — DIRECTIVES PRINCIPALES

Ce fichier, à la racine du dépôt, est la référence persistante pour les nouveaux
chats Codex ouverts dans ce dépôt. Commencer par vérifier `git status`, puis
appliquer les règles ci-dessous et les skills pertinents de `.agents/skills/`.
Un chat ouvert hors du dépôt, ou un autre outil qui ne charge pas `AGENTS.md`,
n'est pas couvert par cette découverte automatique.

## 1. Objectif prioritaire

GoatCore est un serveur Minecraft Java 26.1.1 (protocole 775), développé entièrement en C11 sous Linux.

L'objectif principal est de reproduire le plus fidèlement possible le comportement du serveur officiel Minecraft Vanilla.

La compatibilité Vanilla est la priorité stratégique du projet.

## 2. Roadmap Vanilla

Respecter la roadmap suivante :

0. Tests de référence et comparaison avec le serveur officiel.
1. Login, authentification, gestion et persistance des joueurs.
2. Blocs, block states, interactions, collisions et physique.
3. Génération de l'Overworld, biomes, relief, grottes, minerais et structures.
4. Création d'un moteur d'entités extensible.
5. Mobs passifs, hostiles, intelligence artificielle, spawning et pathfinding.
6. Survival complet : inventaires, craft, combat, faim, expérience, enchantements et potions.
7. Redstone, fluides et mécaniques avancées.
8. Nether, End, dimensions, villages, boss et progression complète.
9. Parité Vanilla, tests différentiels et stabilisation.

Développer progressivement les fonctionnalités avec des tests de conformité à Minecraft 26.1.1.

## 3. Infrastructure de développement et de test

GoatCore dispose d'un serveur Linux distant dédié aux tests et aux benchmarks.

### Serveur distant

- Adresse : 10.0.4.66
- Système : Linux
- Fonction : compilation, déploiement, exécution et tests de GoatCore.
- Scripts d'automatisation existants : scripts/

### Instances Minecraft

Instance ONLINE :

- Hôte : 10.0.4.66
- Port : 25565
- Mode : online-mode=true
- Usage : tests avec le client Minecraft Java officiel.
- Administrateur : Ofwood_

Instance OFFLINE :

- Hôte : 10.0.4.66
- Port : 25587
- Mode : online-mode=false
- Usage : tests automatiques, bots, tests de charge et tests de protocole.
- Administrateur : Ofwood_

Les deux instances doivent fonctionner indépendamment, avec des ports, processus, configurations, logs et répertoires de mondes distincts.

Ne jamais considérer l'authentification online comme fonctionnelle sans l'avoir réellement validée.

Tant qu'un vrai client Java officiel n'a pas réussi une connexion complète sur
une instance temporaire isolée (actuellement 25588), ne pas basculer ni
redéployer 25565 avec `online-mode=true`. Un test de rejet `hasJoined` ne vaut
pas validation positive. Garder 25587 en mode offline pour les bots.

Ne jamais contourner l'authentification Microsoft/Mojang pour simuler un résultat positif en online-mode.

## 4. Politique obligatoire de tests distants

RÈGLE ABSOLUE :

Ne jamais démarrer GoatCore localement sur la machine de développement pour réaliser des tests d'intégration, des benchmarks ou des tests Minecraft.

Toutes les exécutions du serveur doivent être effectuées sur 10.0.4.66.

Workflow obligatoire :

1. Modifier le code source du projet.
2. Vérifier les changements.
3. Utiliser les scripts existants du dossier scripts/ pour transférer la version vers le serveur distant.
4. Compiler sur le serveur distant, sauf si le système de déploiement existant prévoit un autre mécanisme compatible.
5. Démarrer ou redémarrer uniquement les instances nécessaires.
6. Exécuter les tests sur le serveur distant.
7. Récupérer les résultats, logs, erreurs et métriques.
8. Corriger les problèmes.
9. Recommencer jusqu'à validation.

Privilégier une connexion SSH et les scripts de déploiement existants.

Examiner les scripts avant de les modifier ou de les remplacer.

Ne pas supposer un utilisateur SSH, un chemin de déploiement ou un nom de service non vérifié.

Les tests unitaires statiques ou ne nécessitant aucun lancement de serveur peuvent être exécutés localement si nécessaire.

## 5. Gestion des environnements

### ONLINE : port 25565

Utiliser cette instance pour :

- Connexions Minecraft Java officielles.
- Authentification online.
- Login et configuration.
- Tests manuels.
- Compatibilité Vanilla.
- Vérification des fonctionnalités avec de vrais clients.

Ne pas utiliser cette instance pour les tests de charge automatisés.

### OFFLINE : port 25587

Utiliser cette instance pour :

- Bots Minecraft.
- Tests E2E.
- Tests de charge.
- Tests réseau et protocole.
- Tests de gameplay automatisés.
- Tests de performance.
- Scénarios destructifs.

Les bots doivent cibler explicitement le port 25587.

Ne jamais envoyer les bots de test sur le port 25565.

## 6. Permissions administrateur

Le pseudo Minecraft Ofwood_ doit disposer des permissions administrateur sur les deux instances.

Lors du déploiement :

- Vérifier la configuration des permissions.
- Conserver les droits administrateur de Ofwood_.
- Restaurer ces droits après toute réinitialisation.
- Utiliser l'UUID authentifié pour l'instance online lorsque l'authentification sera opérationnelle.
- Utiliser l'identité offline déterministe pour l'instance offline.

Ne pas considérer une simple entrée dans un fichier de permissions comme suffisante : vérifier le fonctionnement réel des privilèges.

## 7. Mondes de test

Les mondes des deux instances sont jetables.

Codex est autorisé à :

- Réinitialiser les mondes de test.
- Supprimer les données de joueurs de test.
- Régénérer les mondes.
- Réinitialiser les inventaires.
- Recréer les configurations de test.

Ne pas supprimer les scripts, sources, outils de déploiement ou clés de connexion.

Limiter les suppressions aux répertoires de test explicitement identifiés sur 10.0.4.66.

## 8. Skills Codex

Utiliser les skills du dépôt :

- remote-deploy : déploiement et redémarrage des instances distantes.
- minecraft-testing : tests Minecraft, bots, E2E et benchmarks.
- vanilla-comparison : comparaison comportementale avec Vanilla.

Lorsqu'une modification nécessite des tests Minecraft, appliquer les procédures remote-deploy et minecraft-testing.

Pour les fonctionnalités Vanilla, privilégier également vanilla-comparison.

Ne jamais annoncer une fonctionnalité comme validée sans preuve de test.

## 9. Multithreading

GoatCore dispose déjà de :

- Réseau multi-epoll.
- Affinité CPU configurable.
- Workers de chunks.
- Tick autoritaire à 20 TPS.

Continuer les travaux suivants :

- Optimisation du thread de tick.
- Profiling du gameplay.
- Job system.
- Snapshots immuables.
- Simulation parallèle.
- Commit autoritaire.
- Physique parallèle.
- Sauvegardes asynchrones.
- Étude future des ticks par région.

Ne jamais sacrifier la cohérence Vanilla pour améliorer artificiellement les performances.

## 10. Architecture

Conserver :

- C11.
- Linux.
- epoll.
- pthread.
- zlib.
- Architecture réseau existante.
- Tick autoritaire.

Éviter les refontes inutiles.

Ne pas ajouter de multithreading sans modèle de propriété et de synchronisation sûr.

## 11. Règles de développement

- Codex réalise les modifications du code.
- ChatGPT fournit les analyses, plans et instructions de développement.
- Préserver les modifications locales existantes.
- Ne pas effectuer de push ou de merge sans instruction explicite.
- Ne pas créer de .venv inutile.
- Tester les fonctionnalités avant validation.
- Privilégier les tests C et Minecraft.
- Utiliser la branche locale la plus récente comme source de vérité.
- Ne pas modifier une fonctionnalité stable sans justification.
- Ne pas cacher les échecs de compilation ou de tests.

Les modifications de code doivent être vérifiées sur le serveur distant lorsqu'elles affectent l'exécution de GoatCore.

## 12. Rapport obligatoire après modification

À la fin d'une tâche de développement, fournir :

1. Fonctionnalités implémentées.
2. Fichiers modifiés.
3. Résultat de compilation.
4. Instance distante utilisée.
5. Tests réellement exécutés.
6. Résultats obtenus.
7. Erreurs rencontrées.
8. Limitations restantes.

Si un test n'a pas été exécuté, le signaler explicitement.

## 13. Objectif à long terme

Faire de GoatCore un serveur Minecraft Vanilla performant, multithread, stable à 20 TPS et entièrement développé en C.

La fidélité comportementale Vanilla est prioritaire.

Le multithreading est un chantier technique parallèle au service de cet objectif.



## 14. Commits Git obligatoires — Un prompt = Un commit

**RÈGLE ABSOLUE : CHAQUE PROMPT DE DÉVELOPPEMENT DOIT SE TERMINER PAR UN COMMIT GIT.**

L'objectif est de pouvoir revenir facilement à une version précédente en cas de bug majeur, de régression ou de problème technique.

### Règles obligatoires

- **1 prompt de développement = 1 commit Git minimum.**
- Chaque tâche doit se terminer par un commit contenant les modifications réalisées pour cette tâche.
- Toujours vérifier `git status` avant de commencer.
- Ne jamais écraser ou supprimer les modifications préexistantes.
- Ne jamais inclure dans un commit des modifications sans rapport avec le prompt.
- Effectuer les tests nécessaires avant le commit.
- Si les tests échouent et que la correction n'est pas possible, signaler clairement l'échec. Un commit de sauvegarde peut être créé, mais il doit être explicitement identifié comme non validé.
- Utiliser des messages de commit explicites et descriptifs.
- Ne jamais utiliser `git reset --hard` ou `git clean -fd` sans autorisation explicite.
- Ne jamais effectuer de `git push` ou de merge sans instruction explicite.
- Ne jamais modifier l'historique Git existant sans autorisation.

### Workflow obligatoire

1. Vérifier l'état Git avant toute modification.
2. Réaliser les modifications demandées.
3. Déployer et tester sur le serveur distant si nécessaire.
4. Corriger les erreurs détectées.
5. Créer un commit Git pour le prompt.
6. Vérifier que les modifications de la tâche sont bien enregistrées.
7. Fournir le hash du commit dans le rapport final.

### Rapport final obligatoire

À la fin de chaque prompt de développement, indiquer :

- Le message du commit.
- Le hash du commit.
- Les tests effectués et leurs résultats.
- Les éventuels problèmes restants.

**Ne jamais terminer une tâche de développement avec des modifications de la tâche non committées, sauf impossibilité technique explicitement signalée.**

L'objectif est de conserver un historique Git propre, traçable et permettant de revenir rapidement à n'importe quelle étape du développement.
