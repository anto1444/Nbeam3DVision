# Nbeam 3D Vision 2.2.0

Mise à jour de la version 2.1.0. Surcouche Windows : BFI, CRT Beam Simulator et 3D active (lunettes NVIDIA 3D Vision ou émetteur Arduino), sur n'importe quel écran.

> **À savoir après la mise à jour** : le réglage **Sous-images : Auto** devient la case **BFI auto**, et le tampon d'image rejoint le nouveau menu **Synchro BFI** (visible quand un programme est capturé). Vos profils sont convertis tout seuls : un tampon réglé reste en Tampon d'image (l'ancien Auto devient un maximum de 5), un calage de phase reste en Calage de phase.

## Nouveautés

### Synchro BFI (nouveau menu, programme capturé)
- **Aucune (déconseillé)** : une image du jeu est prise au début de chaque cycle BFI, comme avant.
- **Calage de phase (meilleure latence)** (2D) : Nbeam mesure l'arrivée des images du jeu, avec leurs micro-variations, et décale son cycle pour qu'elles arrivent juste avant la sous-image visible : attente plus courte et stable. **Intervalle de mesure** de 0,5 à 3 s (1 s par défaut). Les décalages sont **étalés** (jamais plus d'un rafraîchissement par cycle) et, en BFI simple, la **luminosité des cycles décalés est compensée** pour éviter les petits flashs. La valeur de plafond idéale du jeu est affichée.
- **Tampon d'image (meilleure stabilité)** : la valeur choisie (1 à 5) est désormais un **maximum** ; le tampon utilise 1 image et ne monte que si le jeu en a besoin, une image à la fois, avec **2 secondes de transition** entre deux changements ; la taille utilisée est affichée.
- **Hybride** (2D) : calage de phase tant que le jeu est stable ; à chaque à-coup, un tampon d'**une image** arrive aussitôt, puis repart après **1 seconde calme**.
- **BFI auto** (case à cocher, 2D) : les sous-images suivent la cadence du jeu (ancien Sous-images : Auto), avec l'intervalle de détection, le seuil de FPS bas et la luminosité uniforme. Compatible avec chaque choix du menu : le calage de phase suit les sous-images choisies.

### Affichage
- **Présentation directe (expérimental)**, sous « Images en file » : l'overlay n'est plus une fenêtre transparente aux clics, Windows peut l'afficher directement (flip indépendant ou plan matériel) : environ un rafraîchissement de latence en moins et pas d'image BFI retardée par la composition. Pas de clic à travers tant que le HUD est caché. La présentation obtenue et les images en retard par minute sont affichées pour comparer.

### Capture
- L'âge de chaque image capturée est mesuré avec l'horodatage de Windows, pour la synchro BFI.

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
