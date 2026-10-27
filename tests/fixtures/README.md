# Référence joueur Vanilla 26.1.1

`vanilla-26.1.1-player.dat` vient du serveur Java officiel 26.1.1, protocole
775, exécuté sur 10.0.4.66:25590. Le joueur est un bot offline synthétique,
`StressVE00000`, UUID `3c6aa7c1-7854-35ee-93a8-eb4d69355d2d`.
Le fichier ne contient aucune identité Mojang ni donnée d'un joueur humain.

Pour reproduire l'échantillon sur un monde de référence temporaire :

1. Démarrer le serveur officiel en offline, en Creative.
2. Connecter ce bot avec le protocole 775.
3. Envoyer les actions Creative d'inventaire : slot réseau 36 = pierre
   (ID 1), quantité 64 ; slot 5 = casque diamant (971), quantité 1 ;
   slot 45 = épée diamant (937), quantité 1.
4. Déconnecter le bot. Copier son fichier dans `world/players/data/`.
5. Redémarrer la référence et reconnecter la même identité. Vérifier les
   trois piles reçues dans le paquet d'inventaire.

Le NBT observé contient `DataVersion=4788`, `Inventory[].Slot` Byte,
`Inventory[].count` Int et `equipment.head` / `equipment.offhand`.
`test_player_nbt` lit cet échantillon, le sauvegarde via GoatCore et le
recharge. Il vérifie les objets et la conservation des champs non interprétés.
