# Nbeam 3D Vision 2.4.0

Mise à jour de la version 2.3.0. Surcouche Windows : BFI, CRT Beam Simulator et 3D active (lunettes NVIDIA 3D Vision ou émetteur Arduino), sur n'importe quel écran. Cette version apporte surtout un **mini menu 3D piloté par la molette et le bouton de l'émetteur NVIDIA**.

> **À savoir après la mise à jour** : le curseur **Cadence 3D (images/s)** et la case **Utiliser le GPU (DirectML)** disparaissent (voir ci-dessous). Vos profils et réglages sont conservés.

## Nouveautés

### Mini menu 3D (émetteur NVIDIA)
- Un mini menu, **uniquement piloté par l'émetteur**, s'affiche à côté de la bannière 3D (au-dessus si la bannière est en bas de l'écran, en dessous si elle est en haut), pendant la même durée que la bannière.
- **Molette** : ouvre le menu puis monte / descend dans la liste (3 crans par ligne). **Bouton maintenu + molette** : change la valeur de la ligne en surbrillance. **Clic** : ferme le menu. Un rappel d'utilisation est affiché sous le menu pour les débutants.
- Réglages proposés : **mode 3D**, convergence, résolution IA, force de la profondeur, lissage, anti-halo (2D vers 3D IA), décalage de synchro, durée d'ouverture, inverser les yeux. Les réglages sont enregistrés dans le profil.
- Le **mode 3D** et la **résolution IA** sont lourds : le nouveau choix s'affiche avec un « > » et n'est appliqué qu'en **rappuyant sur le bouton** pour confirmer.
- Le menu fonctionne aussi quand la 3D est coupée (mode 3D sur Off) : l'émetteur est lu en permanence.

### 3D par IA
- **Cadence de la profondeur automatique** : une carte de profondeur par rafraîchissement 3D, dans la limite de ce que le PC peut faire (80 par seconde au maximum). Le curseur « Cadence 3D (images/s) » est supprimé.
- **L'IA utilise toujours le GPU (DirectML)** ; le processeur ne sert que de secours. La case est supprimée.

### Synchro BFI (mode Hybride)
- Moins d'à-coups quand le tampon d'une image entre et sort : il n'est retiré qu'après **3 secondes de calme** (au lieu d'1), au moment où une image est de toute façon plus longue que d'habitude, et il reste en place 30 secondes s'il est entré 3 fois en 30 secondes.

### Interface
- **Calage de phase** et **BFI auto** : l'intervalle de mesure (calage de phase) et l'intervalle de détection (BFI auto) vont de 0,1 à 2 secondes, par pas de 0,1, avec une décimale.
- **Configuration du profil** : deux onglets, **Paramètres du programme** et **Paramètres globaux** (le même tableau appliqué à tous les programmes d'un coup).
- **Gérer les profils** : une section au-dessus de la configuration du profil (réglages actifs, profil à régler, renommer, supprimer vers la corbeille, ouvrir le dossier, créer un profil).
- **Raccourcis** : une seule case par action pour le clavier et la manette (ce qui est utilisé remplace l'ancien raccourci) ; une touche déjà utilisée par une autre action est **échangée** entre les deux actions ; un clic ailleurs ou Échap annule la saisie.
- **À propos** : crédits centrés, **notes de version**, croix rouge, bouton **Nouvelle version** qui ouvre la page des versions sur GitHub. 
- **Redémarrer** dans le menu de l'icône de notification.

### Corrections
- La description du programme (propriétés du fichier) affiche maintenant « Nbeam 3D Vision » et la bonne version.

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
- Écran 100 Hz minimum pour la 3D ou le BFI, 240 Hz ou plus pour les deux ensemble.

> **Attention** — images qui clignotent très vite (prudence si vous êtes photosensible) ; risque de rémanence temporaire sur un écran lent avec le BFI ou la simulation CRT. Utilisation à vos risques et périls.

Licence MIT. Basé sur [ShaderBeam](https://github.com/mausimus/ShaderBeam) de mausimus. Crédits complets dans le README.
