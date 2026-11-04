# Nbeam 3D Vision 2.3.0

Mise à jour de la version 2.2.0. Surcouche Windows : BFI, CRT Beam Simulator et 3D active (lunettes NVIDIA 3D Vision ou émetteur Arduino), sur n'importe quel écran — et maintenant aussi par-dessus d'autres surcouches de capture comme Lossless Scaling.

> **À savoir après la mise à jour** : le réglage **Type d'écran** disparaît (c'est la case Anti-rétention LCD qui décide) et le **test de mouvement** devient un bouton dans la barre du bas. Vos profils et réglages sont conservés.

## Nouveautés

### Lossless Scaling et autres surcouches de capture
- Nbeam peut **capturer la couche de Lossless Scaling** (ajoutez `LosslessScaling.exe` dans la configuration du profil) : le BFI, le CRT ou la 3D s'ajoutent à l'image déjà mise à l'échelle ou générée.
- Nbeam reste **au-dessus** de la couche de Lossless Scaling.
- Le **HUD s'affiche par-dessus sans voler le focus du jeu** (sinon Lossless Scaling s'arrêtait) : il s'utilise à la souris.
- Le menu de l'icône de notification passe au-dessus de la barre des tâches.

### Interface
- **Test de mouvement** : un bouton dans la barre du bas, entre « Benchmark » et le bouton des effets (**Test de mouvement** / **Arrêter le test**), à la place de la case.
- **Configuration du profil** (ancienne Détection de profil) : elle regroupe désormais **tous les réglages des profils** (réglages actifs, profil à régler, créer un profil, puis les programmes et leurs profils). Plus simple, avec **Processus à régler** (liste par ordre alphabétique), **Écrire un nouveau processus**, **Ajouter un nouveau processus** et une case **Activé** ; le bouton Actualiser disparaît (la liste des profils se met à jour toute seule).
- **Aides « ? » plus courtes et plus simples.**
- **Paramètres du shader** : une décimale (1.0, 2.2) et les boutons − / + avancent de 0,1.
- **Type d'écran supprimé** : c'est la case Anti-rétention LCD qui décide ; la compensation d'inversion LCD n'apparaît que si elle est cochée.
- **Mode fenêtre** : la fenêtre retient sa position exacte, sa taille et si elle était agrandie quand on quitte Nbeam.
- **Émetteur non reconnu** : le message s'affiche avant le démarrage ; OK démarre Nbeam, la croix le ferme.

### 3D
- **Synchro BFI masquée en 3D** : le tampon d'image est toujours utilisé ; **BFI auto** est masqué aussi.
- **Images en file : 3 au minimum en 3D** (0, 1 et 2 seulement en 2D).
- **Synchro Auto de l'émetteur** : la fréquence la plus haute entre 80 et 165 Hz (480 Hz → 160 Hz, 240 Hz → 120 Hz, 100 Hz → 100 Hz), d'après la fréquence d'écran utilisée, y compris celle de la configuration du profil.

### Corrections
- Plantage possible du rendu quand un réglage reconstruisait les images d'entrée en direct (changement de shader ou de source 3D).

## Problème connu
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
