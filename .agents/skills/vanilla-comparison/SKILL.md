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
