# Nbeam 3D Vision 2.4.1

Mise à jour de la version 2.4.0 : la fréquence de l'émetteur se choisit dans une liste, le mini menu s'enrichit, Nbeam exige un écran d'au moins 80 Hz, et le lissage de la profondeur compense son retard. Vos profils et réglages sont conservés.

## Nouveautés

### Écran 80 Hz minimum
- Nbeam **refuse de démarrer** si l'écran de sortie est sous 79 Hz (message : taux de rafraîchissement incompatible, 80 Hz minimum), et **se ferme** avec ce message si l'écran passe sous 79 Hz pendant l'exécution. Rien n'est affiché si un autre écran connecté est compatible.
- Les profils ne proposent plus d'écran ni de fréquence sous 80 Hz.

### Émetteur 3D
- **Fréquence de synchro** : le champ libre devient une liste déroulante des fréquences possibles (Hz de l'écran ÷ n, de 80 à 165 Hz), affichées avec 3 décimales, enregistrées avec 6.
- Le message « 3D indisponible à cause de l'émetteur » disparaît.
- **Lissage** de la profondeur jusqu'à 100 % : au-delà de 50 %, l'image du jeu est retenue (une carte de retard, davantage plus haut) pour rester en phase avec la carte. Nécessite le tampon de carte.

### Mini menu 3D
- Nouvelle ligne **Fréquence de l'émetteur** (sous Mode 3D, confirmée en rappuyant sur le bouton) ; **Convergence** passe au-dessus de Inverser les yeux.
- **Ajouter le programme aux profils** (tant qu'il n'y est pas) et **Profil du programme** (liste des profils), pour le programme capturé ou au premier plan.

### Interface
- Case **Afficher les statistiques** (mémoire, latence avec ou sans tampon, images en retard) dans Apparence.
- Bannière Nbeam pour le BFI et le CRT avec la fréquence de l'écran ; la cellule et la fréquence 3D sont masquées quand la 3D n'est pas utilisée (la bannière 3D reste prioritaire).
- La sortie d'affichage ne liste que les écrans connectés.

### Corrections
- Valeur de réglage hors plage : remplacée par la plus proche existante ; un profil qui pointe vers un shader supprimé ne plante plus.
- Nettoyage du code (champs et textes inutilisés).

## Problèmes connus
- **Désynchronisation temporaire quand la souris est visible.** Pour régler la 3D, préférez les raccourcis clavier ou le mini menu de l'émetteur.
- **Lossless Scaling (LSFG) capturé : le pointeur de la souris n'est pas visible dans le HUD.** Les clics marchent, mais il faut viser sans voir le pointeur ; pour taper du texte dans le HUD, arrêtez la mise à l'échelle.

## Contenu de l'archive

- `Nbeam3DVision.exe` : le programme (il lance aussi la fenêtre du test des voitures)
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
- Écran 80 Hz minimum (100 Hz recommandé pour la 3D ou le BFI), 240 Hz ou plus pour les deux ensemble.

> **Attention** — images qui clignotent très vite (prudence si vous êtes photosensible) ; risque de rémanence temporaire sur un écran lent avec le BFI ou la simulation CRT. Utilisation à vos risques et périls.

Licence MIT. Basé sur [ShaderBeam](https://github.com/mausimus/ShaderBeam) de mausimus. Crédits complets dans le README.
