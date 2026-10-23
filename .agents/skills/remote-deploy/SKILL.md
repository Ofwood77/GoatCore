---
name: remote-deploy
description: Compiler, déployer ou diagnostiquer GoatCore sur 10.0.4.66, avec services 25565/25587 et instance d'authentification temporaire 25588. Utiliser pour tout lancement de serveur ou test d'intégration distant.
---

# GoatCore Remote Deploy

## Objectif

Déployer et tester GoatCore exclusivement sur le serveur distant 10.0.4.66.

Ne jamais lancer une instance du serveur Minecraft sur la machine locale pour
des tests d'intégration, Minecraft ou de performance. Les tests unitaires
sans serveur peuvent rester locaux.

## Infrastructure

Serveur : 10.0.4.66

Instances :

- ONLINE : 25565
- OFFLINE : 25587
- AUTH STAGE : 25588, temporaire et isolé

Administrateur Minecraft : Ofwood_

## Procédure

1. Inspecter les scripts concernés dans `scripts/` avant de les lancer.
   `deploy-remote.sh` transfère, compile et redémarre 25565 ; il redémarre
   25587 seulement s'il était actif. `remote-test-server.sh` gère 25587,
   `remote-auth-stage.sh` gère 25588.
2. Vérifier par SSH les unités, chemins, configurations et processus effectifs.
3. Transférer le code et compiler sur 10.0.4.66 seulement pour l'instance
   nécessaire. Ne jamais redéployer 25565 pour diagnostiquer 25588.
4. Vérifier compilation, état du service, port, logs et comportement réel.
5. Tester les droits d'Ofwood_ en jeu, pas seulement la variable d'environnement.

## Séparation des instances

Les instances online et offline doivent posséder :

- Configurations distinctes.
- Processus indépendants.
- Ports distincts.
- Mondes distincts.
- Logs distincts.

Elles partagent le binaire `/opt/goatcore/app/mc_server`, mais ont des
répertoires de travail `instances/online` et `instances/offline` sous
`/opt/goatcore/app`, chacun avec son `server.config` et ses assets liés.
Les mondes sont `/var/lib/goatcore/world` et `/var/lib/goatcore/test-world`.
Les unités sont `goatcore.service` et `goatcore-test.service` ; les journaux
systemd sont séparés. Les fichiers d'affinité CPU sont respectivement
`/etc/goatcore/server1.setup.conf` et `server2.setup.conf`.

Ne jamais écraser les configurations online avec les configurations offline.

Ne promouvoir `online-mode=true` sur 25565 qu'après connexion complète d'un
vrai client Java officiel sur 25588, vérification de l'UUID Mojang et des
droits d'Ofwood_. Un échange RSA suivi d'un refus `hasJoined` ne suffit pas.
Le nom exact `Ofwood_` est configuré par `MC_ADMIN_USERNAME` sur les deux
unités ; sur 25587 offline, ce nom reste usurpable par tout client autorisé
à accéder au port.

## Gestion des mondes

Les mondes sont jetables et peuvent être réinitialisés après vérification
du chemin exact.

Ne pas supprimer les fichiers hors des répertoires de test identifiés.

## Gestion des erreurs

En cas d'échec :

- Récupérer les logs.
- Identifier le problème.
- Corriger la cause.
- Redéployer.
- Retester.

Ne jamais présenter un déploiement comme réussi sans validation.

Ne pas effectuer de push ou de merge sans autorisation explicite.
