# Nbeam 3D Vision 1.1.0

Mise à jour de la version 1.0.0. Surcouche Windows : BFI, CRT Beam Simulator et 3D active (lunettes NVIDIA 3D Vision ou émetteur Arduino), sur n'importe quel écran.

## Nouveautés

- **Tampon de carte (2D vers 3D par IA)** : nouvelle case dans le panneau 3D. Nbeam mesure le retard de la carte de profondeur et retient chaque image jusqu'à ce que sa propre carte soit prête : **la profondeur colle à l'image**, même sur un PC lent ou surchargé (le tampon peut alors dépasser 5 images). Le délai mesuré est affiché. Il se cumule avec le tampon d'image, qui fonctionne désormais normalement en 2D vers 3D.
- **Tampon d'image sur les diviseurs** : avec les sous-images sur une valeur fixe, un jeu qui tourne à un diviseur de la cadence du contenu (60, 40, 30 ou 15 images/s pour 120) voit chacune de ses images affichée le même nombre de fois, régulièrement, au lieu d'une alternance irrégulière. Un jeu simplement plus lent n'est plus pris pour une saccade.
- **Tampon d'image Auto plus réactif** : il monte d'un cran toutes les 100 ms (au lieu de 250 ms) et redescend après 2 s de calme (au lieu de 6 s), et de 1 à 0 après 4 s (au lieu de 10 s).
- **Raccourci « Ajouter le programme au premier plan »** (Ctrl+Maj+P par défaut, personnalisable, manette possible) : depuis un jeu, il ajoute son `.exe` aux détections de profil, avec un bandeau de confirmation.
- **2D vers 3D : bords nets** : les bords gauche et droit sont remplacés par des bandes noires droites, identiques dans les deux yeux, au lieu de pixels étirés qui faisaient des bords irréguliers.
- **2D vers 3D : anti-halo** : plage portée à 300 % (valeur par défaut 60 %).
- **Beam racing (expérimental)** : avec les images en file à 0, l'envoi de l'image est calé sur le balayage réel de l'écran pour que la déchirure tombe là où l'effet la cache (retour vertical pour le BFI et la 3D, au-dessus des bandes pour le CRT Beam Simulator), avec un réglage d'avance.
- **2D vers 3D : « Inverser la profondeur » supprimé** : l'inversion de la profondeur suit maintenant « Inverser les yeux », ce qui évite que les deux réglages se contredisent.

## Contenu de l'archive

- `Nbeam3DVision.exe` : le programme
- `Shaders\CRTBeamSimulator.hlsl` : le shader du CRT Beam Simulator (à garder à côté de l'exe)
- `README.md` : le mode d'emploi complet
- `LICENSE`, `THIRD_PARTY_NOTICES.md` : licences

## Installation / mise à jour

1. Décompressez l'archive dans un dossier où Nbeam peut écrire.
2. Pour une mise à jour, quittez d'abord Nbeam par le menu de l'icône de notification, puis remplacez l'exe et le dossier `Shaders`. Vos profils (`Profils\`) et votre configuration sont conservés.
3. Lancez `Nbeam3DVision.exe`.

**2D vers 3D (IA)** : `onnxruntime.dll`, `onnxruntime_providers_shared.dll` et le modèle `.onnx` ne sont **pas fournis** (licence et taille). Copiez-les vous-même à côté de l'exe (voir le README). Sans eux, tout le reste fonctionne.

## Prérequis

- Windows 10 (1903) ou 11, 64 bits, carte graphique DirectX 11 (RTX 3050 ou AMD équivalent recommandé).
- Écran 100 Hz minimum pour la 3D ou le BFI, 240 Hz ou plus pour les deux ensemble.

> **Attention** — images qui clignotent très vite (prudence si vous êtes photosensible) ; risque de rémanence temporaire sur un écran lent avec le BFI ou la simulation CRT. Utilisation à vos risques et périls.

Licence MIT. Basé sur [ShaderBeam](https://github.com/mausimus/ShaderBeam) de mausimus. Crédits complets dans le README.
