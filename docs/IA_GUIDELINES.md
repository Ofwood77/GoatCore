# IA Guidelines

Ce fichier sert de base de travail pour les futures sessions IA sur ce projet.

## Regles absolues

1. Avant toute modification, consulter d'abord les fichiers dans `docs/`.
2. Si la reponse se trouve deja dans la documentation, ne pas explorer le code source inutilement.
3. Le code C doit rester oriente Data-Oriented Design (DOD) pour maximiser la localite memoire, le cache CPU et les performances brutes.

## Ordre de consultation recommande

Avant d'ouvrir des fichiers `src/`, lire dans cet ordre :

1. `docs/ARCHITECTURE_C_ACTUELLE.md`
2. `docs/REFERENCE_VANILLA_JAVA.md`
3. `docs/PROPOSAL_NEW_BLOCK_SYSTEM.md`
4. les autres fichiers `docs/` pertinents pour la tache

Le code source ne doit etre explore qu'apres cette lecture, et seulement pour verifier un point non deja documente.

## Philosophie de travail

- Toujours preferer une comprehension architecturale avant d'ecrire du code.
- Toujours distinguer le chemin chaud runtime du chemin froid debug/serialisation/outillage.
- Toujours traiter les conversions `Vanilla Java <-> runtime C` comme une couche d'adaptation, pas comme le modele interne principal.
- Toujours documenter les hypotheses importantes lorsqu'un comportement Vanilla n'est pas encore totalement confirme.

## Contraintes DOD obligatoires

Tout nouveau code gameplay, monde, chunk ou entite doit respecter les principes suivants :

- privilegier les donnees contigues et denses
- privilegier les `struct of arrays` ou les layouts hybrides qui gardent les champs chauds ensemble
- eviter les graphes d'objets disperses en memoire dans les boucles frequentes
- eviter les allocations fines dans les hot paths
- separer les metadonnees froides des donnees scannees a chaque tick
- conserver des compteurs et flags precomputes quand ils evitent des rescans couteux

## Regles specifiques aux blocs et chunks

- Ne plus propager des `block_state_id` Vanilla bruts comme representation centrale du runtime.
- Toute logique bloc doit raisonner en termes de :
  - type de bloc
  - etat de bloc
  - block entity eventuelle
- Les chunks doivent rester decoupes en sections `16 x 16 x 16`.
- Le stockage des voxels doit tendre vers une palette locale par section avec packing en bits.
- Les block entities ne doivent pas polluer le layout memoire chaud des voxels.

## Regles specifiques a la parite Vanilla

- Pour toute mecanique Vanilla sensible, verifier d'abord si `docs/REFERENCE_VANILLA_JAVA.md` suffit.
- Si la documentation ne suffit pas, explorer `mc_vania_asset` avant d'inferer.
- Si le comportement exact reste ambigu, noter explicitement :
  - ce qui est confirme
  - ce qui est deduit
  - ce qui doit encore etre verifie

## Regles de modification

- Ne pas lancer une grosse refonte transversale sans decrire d'abord l'impact sur le layout memoire.
- Ne pas introduire une API pratique mais opaque si elle masque un cout memoire important.
- Ne pas corriger un symptome gameplay en renforcant la dette du systeme actuel de `block_state_id` bruts.
- Si une modification change l'architecture, mettre a jour `docs/` dans la meme tache.

## Regles de verification

Apres chaque changement important :

- verifier les builds et tests disponibles
- verifier l'impact sur les structures de donnees chaudes
- verifier si la documentation reste exacte

## But a long terme

Construire un serveur Minecraft Vanilla en C capable de scaler tres loin, ce qui implique :

- un modele monde compact
- une execution previsible
- un cout memoire controle
- des hot paths simples a profiler et a optimiser

Si un choix favorise la facilite immediate mais detruit la localite memoire ou la clarte du runtime, ce choix doit etre reconsidere.
