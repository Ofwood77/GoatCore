# Attribution Git — 5 novembre 2026

À la demande explicite du propriétaire, les attributions historiques de ce projet
ont été regroupées sous son compte GitHub vérifié **Ofwood77** (ID 124205937).
L'ancien compte `commandant-x` a été confirmé comme appartenant au propriétaire.
L'identité automatique `Codex <codex@local>` n'était pas associée à ce compte.
Une ancienne mention `Co-authored-by: aider … <aider@aider.chat>` expliquait
l'attribution Aider ; elle ne constituait pas une preuve de droits sur le dépôt.

Après autorisation explicite de réécriture et de push protégé :

- audit de toutes les branches et des deux tags publiés, plus des branches locales ;
- sauvegarde complète vérifiée dans `.local/identity-audit.03FrvZ/before-attribution-all.bundle` ;
- réécriture isolée de 84 objets commit (dont les références de PR présentes dans
  la copie d'audit), avec auteur/committer `Ofwood77` et adresse
  `124205937+Ofwood77@users.noreply.github.com` ;
- retrait de la mention Aider et des signatures rendues obsolètes ;
- conservation exacte de **tous les arbres de fichiers** et des dates ; les
  relations de parents sont remappées sans changer les structures de fusion ;
- publication atomique des **7 branches et 2 tags** avec une protection
  `--force-with-lease=ref:ancien_hash` spécifique à chaque référence ;
- vérification des hashes distants et synchronisation des branches locales sans
  checkout, reset ou écrasement du travail non committé.

L’API GitHub a ensuite confirmé les **75 commits uniques accessibles depuis les
7 branches et 2 tags publiés** : auteur et committer associés à `Ofwood77`, sans
mention de co-auteur Aider/Codex. Les huit commits supplémentaires des branches
locales sont aussi normalisés ; le 84e objet audité appartient à une référence
de fusion de PR conservée dans la copie d’audit.

La table `git-attribution-map.json` permet de retrouver les nouveaux hashes depuis
les références historiques citées dans les rapports de validation. En particulier,
G2 `92dff9c7c55dddc392a52630855083425d67748c` devient
`a83e2b580733e1579099071da69e635aa526ec7c`, avec le même arbre de fichiers. Les
mesures des binaires restent identifiées par SHA-256 indépendamment des hashes Git.

Douze fichiers Windows `*:Zone.Identifier` contenant uniquement `[ZoneTransfer]`
étaient présents sous `.git/refs` et empêchaient les lectures `--all` et certains
fetchs. Ils ont été déplacés dans la copie d'audit, avec leurs empreintes, sans
supprimer de vraie référence ni modifier les fichiers personnels du projet.

Les références de pull requests sont en lecture seule sur GitHub : elles ne sont
pas forcées ni supprimées. Les anciennes pages de commits, signatures, références
internes ou caches de contributeurs peuvent rester accessibles ; ce travail ne
prétend pas effacer les objets des serveurs GitHub. Un autre clone doit être
sauvegardé puis resynchronisé avec le nouvel historique ; ne pas fusionner les
anciens et nouveaux historiques pour résoudre la divergence.

La configuration **locale** du dépôt et les trois skills imposent désormais
l'identité Ofwood77 avant chaque commit et un push normal vérifié après chaque
commit. Aucun co-auteur bot n'est ajouté. Cette règle permanente ne couvre
pas les futures réécritures ou déploiements, qui gardent leur autorisation propre.
Les services Minecraft et leurs données ne sont pas concernés par cet audit.

## Nettoyage des branches après publication sur main

À la demande explicite du propriétaire, `main` a été avancée sans réécriture
jusqu’au commit validé `053d605807d46f714189ac5891d14ffc41391978`.
Les six autres branches GitHub et les trois autres branches locales ont ensuite
été supprimées. Les deux tags existants sont conservés.

Avant suppression, une sauvegarde complète des références concernées a été créée
et vérifiée : `.local/branch-cleanup.9a4sa6uz/before-branch-cleanup.bundle`.
Elle conserve aussi les commits propres aux anciennes branches. Les suppressions
distantes ont été regroupées atomiquement avec un contrôle du hash attendu de
chaque branche ; les suppressions locales utilisaient également le hash capturé.
`main`, les fichiers de travail et les fichiers personnels sont préservés.

Le contrôle de tous les commits accessibles depuis `main` confirme l’auteur et
le committer Ofwood77, sans co-auteur bot. L’API des contributeurs GitHub ne
retourne que Ofwood77 ; ses compteurs et l’encart du site peuvent être mis en
cache. Les anciennes références de pull requests restent gérées par GitHub.
Aucun changement de droits d’accès n’est déduit de la liste des contributeurs.

Les instructions et les trois skills imposent désormais `main` comme unique
branche de travail et de publication, avec un push normal vers `origin/main`
après chaque commit. Aucun serveur Minecraft ni donnée de monde n’est modifié.
