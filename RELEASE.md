# Nbeam 3D Vision 1.2.0

Mise à jour de la version 1.1.0. Surcouche Windows : BFI, CRT Beam Simulator et 3D active (lunettes NVIDIA 3D Vision ou émetteur Arduino), sur n'importe quel écran.

> **À faire une fois après la mise à jour** : réglez à nouveau le **Sync Offset** des lunettes. Il est maintenant compté depuis l'affichage réel de l'image et non plus depuis son envoi ; ensuite il ne se dérègle plus.

## Nouveautés

### 3D
- **Synchro des lunettes automatique** : les lunettes suivent les vrais rafraîchissements de l'écran (statistiques DXGI) dès que Windows les fournit, sinon la synchronisation classique prend le relais. Rien à cocher ; une ligne sous « Inverser les yeux » indique le mode utilisé.
- **Sync Offset stable** : changer le nombre d'images en file ou passer d'une synchro à l'autre ne le dérègle plus (le délai entre l'envoi et l'affichage est mesuré puis compensé automatiquement).
- **Moins de décalages des lunettes sous charge** : une image envoyée en retard est remise sur le rythme régulier de l'écran, au lieu d'un décalage qui revenait progressivement (surtout visible à haute fréquence).
- **Chargements et compilation de shaders du jeu** : Nbeam passe en priorité haute et ses threads de rendu et de synchro des lunettes sont confiés au planificateur multimédia de Windows (MMCSS). Le jeu ne leur prend plus leur temps de calcul : moins d'images ratées.

### Profils
- **Profil par défaut** : nouvelle case à côté du choix du profil. Nbeam démarre sur ce profil, et c'est lui qui revient quand un programme détecté se ferme (sans profil par défaut, le profil précédent revient, comme avant).
- **Paramètres globaux** dans la détection de profil : une ligne qui règle d'un coup tous les programmes (case activée, délai), puis une ligne par écran branché (**Écran 1, 2, 3**) qui donne un profil et une fréquence de rafraîchissement à tous les programmes sur cet écran. Chaque programme reste modifiable sur sa ligne ; « (mixte) » indique des valeurs différentes.
- **Sauvegarde automatique** : tout réglage modifié dans le HUD ou par un raccourci est enregistré dans le profil actif dans la seconde, y compris dans le profil d'un programme détecté pendant qu'il tourne.
- **Ajouter depuis la barre des tâches** : nouveau bouton à côté de « Ajouter une détection ». Il liste les programmes ouverts sur la barre des tâches ; un clic ajoute le programme choisi aux détections.
- **Raccourcis globaux** : ils sont enregistrés dans `Nbeam.app.cfg` et ne dépendent plus du profil actif.

### Effets et HUD
- **Afficher / Masquer les effets** : le bouton du HUD (ex « Comparer / Suspendre »), un nouveau raccourci (**Ctrl+Maj+D** par défaut) et le menu de l'icône de notification coupent **tous les effets et l'émetteur 3D**, jusqu'à un appui sur « Afficher ». Charger un autre profil que le profil par défaut réaffiche les effets.
- **Ouvrir le HUD ne coupe plus les effets** : on règle en voyant le résultat.
- **HUD plus léger** : il est dessiné à part 30 fois par seconde (à pleine cadence seulement pendant qu'on le déplace) puis posé sur l'image ; le temps de rendu reste régulier quand il est affiché, ce qui aide la 3D et le BFI.
- **Journal de plantage** : si Nbeam se ferme brutalement, `Nbeam_crash.txt` (à côté de l'exe) indique l'endroit exact du problème. Envoyez-le avec votre rapport de bug.

### Renommages
- « Nom du nouveau profil » devient **« Créer un profil »**.
- Le raccourci « Ajouter le programme au premier plan » devient **« Ajouter le programme aux profils »**.

## Correctifs

- **Fermetures inattendues** :
  - erreur de la capture Windows quand un jeu se ferme ou change de fenêtre au mauvais moment ;
  - HUD utilisé en même temps par la souris et par le rendu ;
  - démarrage alors qu'un programme détecté tourne déjà ;
  - écran éteint ou débranché (une TV, par exemple) : Nbeam attend et réessaie toutes les 2 s ;
  - « Quitter » reçu deux fois.
- **Perte du GPU** (mise à jour ou plantage du pilote) : Nbeam ne reste plus figé à 100 % de processeur, il redémarre tout seul.
- **Fenêtre du jeu fermée** : un seul redémarrage au lieu de centaines par seconde (saccades, clignotements).
- **Raccourcis tous inactifs** après une capture de raccourci laissée ouverte (HUD masqué ou autre onglet) : la capture est maintenant annulée.
- **Beam racing** : il ne s'applique plus en 3D (avec les images en file à 0, il décalait l'émetteur).
- **Changement d'écran** (écran branché, débranché ou changement de mode) : Nbeam redémarre réduit, le HUD ne s'ouvre plus tout seul.
- Les raccourcis « Afficher/masquer HUD » et « Afficher/masquer les effets » ne peuvent plus être vides (on ne peut plus se retrouver sans moyen d'afficher le HUD) ; un raccourci vide s'affiche « Aucun ».

## Supprimé

- L'option « Effets toujours désactivés (capture du bureau) » et son raccourci, remplacés par Afficher / Masquer les effets.
- L'option « Synchro sur le balayage (DXGI) » : la synchro est maintenant automatique.
- Le code inutilisé du plafonnement d'images (RTSS / profil du pilote NVIDIA) et les en-têtes NVAPI.

> Si vous aviez personnalisé le raccourci « Ajouter le programme au premier plan », il revient à Ctrl+Maj+P (le raccourci a changé de nom). Ctrl+Maj+D, qui servait à « Effets toujours désactivés », sert maintenant à Afficher / Masquer les effets.

## Contenu de l'archive

- `Nbeam3DVision.exe` : le programme
- `Nbeam3DVision.pdb` : les symboles, pour que le journal de plantage indique les noms des fonctions (facultatif, à garder à côté de l'exe)
- `Shaders\CRTBeamSimulator.hlsl` : le shader du CRT Beam Simulator (à garder à côté de l'exe)
- `README.md` : le mode d'emploi complet ; `RELEASE.md` : ces notes
- `LICENSE`, `THIRD_PARTY_NOTICES.md` : licences

## Installation / mise à jour

1. Décompressez l'archive dans un dossier où Nbeam peut écrire.
2. Pour une mise à jour, quittez d'abord Nbeam par le menu de l'icône de notification, puis remplacez l'exe, le `.pdb` et le dossier `Shaders`. Vos profils (`Profils\`) et votre configuration sont conservés.
3. Lancez `Nbeam3DVision.exe`.

**2D vers 3D (IA)** : `onnxruntime.dll`, `onnxruntime_providers_shared.dll` et le modèle `.onnx` ne sont **pas fournis** (licence et taille). Copiez-les vous-même à côté de l'exe (voir le README). Sans eux, tout le reste fonctionne.

## Prérequis

- Windows 10 (1903) ou 11, 64 bits, carte graphique DirectX 11 (RTX 3050 ou AMD équivalent recommandé).
- Processeur : 4 cœurs / 8 threads minimum (Intel Core i5 8e génération ou AMD Ryzen 3 3100), 6 cœurs / 12 threads recommandé (Intel Core i5-10400 ou AMD Ryzen 5 3600 et plus récents).
- Écran 100 Hz minimum pour la 3D ou le BFI, 240 Hz ou plus pour les deux ensemble.

> **Attention** — images qui clignotent très vite (prudence si vous êtes photosensible) ; risque de rémanence temporaire sur un écran lent avec le BFI ou la simulation CRT. Utilisation à vos risques et périls.

Licence MIT. Basé sur [ShaderBeam](https://github.com/mausimus/ShaderBeam) de mausimus. Crédits complets dans le README.
