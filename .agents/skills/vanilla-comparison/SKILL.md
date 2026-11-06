---
name: vanilla-comparison
description: Comparer une fonctionnalité GoatCore au comportement du serveur Minecraft Java Vanilla 26.1.1, protocole 775, lors d'un travail de compatibilité ou d'une régression.
---

# Comparaison Vanilla 26.1.1

Pour une fonctionnalité de protocole ou de gameplay, définir d'abord un
scénario observable identique : version du client, état initial, actions et
paquets ou effets attendus. Vérifier les identifiants et structures du
protocole 775 contre une source de référence adaptée à 26.1.1 ; ne pas
transposer aveuglément un codec 1.21.x. Utiliser les assets et tests du dépôt
comme référence locale, en distinguant clairement une hypothèse d'une
observation Vanilla.

Si une comparaison exige de lancer GoatCore ou un serveur Vanilla, faire ces
exécutions sur 10.0.4.66 avec ports et mondes isolés, jamais sur le laptop.
Ne pas envoyer de bots au 25565 online. Comparer les résultats côté client et
côté serveur, y compris erreurs, ordre des paquets et état persistant quand
c'est pertinent. Une différence documentée n'est pas une parité validée.

Pour l'authentification, seule une connexion réelle d'un client Java officiel
peut valider le chemin positif `hasJoined` ; ne jamais fabriquer une session
Mojang réussie pour déclarer le mode online fonctionnel. Rapporter ce qui a
été testé, la référence utilisée et les écarts restants.

## Optimisations mémoire et persistance

Les optimisations RAM, caches et sauvegardes différées ne doivent pas modifier
les mutations autoritaires, les états ou l'ordre observable des actions Vanilla.
Comparer séparément action appliquée, snapshot accepté et génération durable.
Vérifier les inventaires et états après reconnexion/redémarrage et après les
interruptions couvertes par le contrat ; un état en RAM n'est pas une preuve de
durabilité. La cible mémoire 4–100 Go n'autorise aucune approximation gameplay.

## Scalability without semantic shortcuts

Scaling, caching and workload optimizations must preserve authoritative behavior
and observable ordering. Dropping essential Minecraft simulation to improve TPS
is not conformance. Keep differential regression corpora during scaling work;
report absent gameplay and synthetic workload limits. Functional comparisons
against Vanilla do not establish a direct performance advantage. Publish direct
TPS comparisons only with genuinely equivalent measured workloads and resources.

## Sauvegarde GitHub après commit

Après chaque commit, effectuer un push normal depuis le dépôt local vers
le remote GitHub vérifié, sur `main`. Le propriétaire autorise ce
push de façon permanente ; aucune nouvelle confirmation n’est nécessaire.
Vérifier que le hash distant correspond au commit poussé et rapporter le résultat.
Ne pas utiliser `--force`, changer de branche cible, fusionner ou réécrire
l’historique pour résoudre un refus. En cas de rejet, conserver le commit local
et expliquer le blocage. Cette règle n’autorise aucun déploiement ni aucune
commande Git sur le serveur de test.

Vérifier avant chaque commit l’auteur et le committer `Ofwood77`, avec l’adresse
GitHub noreply définie dans `AGENTS.md`. Ne pas ajouter de co-auteur bot.

`main` est l’unique branche de travail et de publication. Ne pas créer ou
publier d’autre branche sans nouvelle instruction explicite du propriétaire.
