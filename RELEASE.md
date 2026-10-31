# Nbeam 3D Vision 2.0.0

Mise à jour de la version 1.2.0. Surcouche Windows : BFI, CRT Beam Simulator et 3D active (lunettes NVIDIA 3D Vision ou émetteur Arduino), sur n'importe quel écran — et désormais aussi un vrai logiciel en fenêtre.

> **À savoir après la mise à jour** : Nbeam s'ouvre maintenant **dans une fenêtre** (mode fenêtre, coché par défaut). Pour retrouver l'ancien fonctionnement (HUD par-dessus l'écran uniquement), décochez **Mode fenêtre** dans Application. Le **Sync Offset** ne se règle plus qu'en négatif : une valeur positive d'un ancien profil revient à 0, réglez-le une fois.

## Nouveautés

### Mode fenêtre (comme Lossless Scaling)
- Nbeam s'ouvre dans une **fenêtre Windows normale** (déplaçable, redimensionnable) avec la même présentation que le HUD. On y règle tout **effets coupés** : rien n'est capturé tant que les effets ne tournent pas.
- **Activer les effets** : bouton en bas de la fenêtre, raccourci Afficher / masquer les effets, menu de l'icône de notification, ou automatiquement quand un **programme détecté** se lance. La fenêtre disparaît et l'overlay reprend par-dessus l'écran.
- **Pendant les effets**, le HUD par-dessus l'écran fonctionne comme avant (son raccourci) ; son bouton devient **Désactiver les effets**.
- **Arrêt** à la fermeture du programme détecté, avec Désactiver les effets ou avec le raccourci : la fenêtre revient (sans voler le focus du jeu quand c'est le raccourci).
- La croix range la fenêtre dans la zone de notification (si l'option est cochée, les détections continuent), sinon elle quitte Nbeam.
- **Taille de l'interface séparée** entre la fenêtre et le HUD par-dessus l'écran (Apparence → Taille du HUD, dans le mode concerné).

### Effets
- **Luminosité uniforme** (sous-images Auto, BFI simple) : case au-dessus du seuil de FPS bas. L'image garde toujours la luminosité qu'a le BFI au seuil de FPS bas, quelle que soit la cadence du jeu (y compris BFI doublé ou coupé sous le seuil) ; plus le seuil est bas, plus l'image est sombre.

### 3D
- **Sync Offset négatif uniquement** : de 0 à moins une période d'œil (curseur et raccourcis), pour ne plus inverser les yeux par erreur avec une valeur positive.
- **Anti-halo de -300 à 300 %** : une valeur négative fait la même chose à l'envers, pour une profondeur inversée (Inverser les yeux coché en 2D vers 3D).

### Raccourcis
- **Écoute directe du clavier** : les raccourcis passent maintenant aussi dans les jeux qui s'approprient le clavier (DirectInput exclusif, Raw Input), comme Mirror's Edge. Les touches ne sont jamais bloquées pour le jeu. Si un jeu tourne en administrateur, lancez Nbeam en administrateur.

## Correctifs

- La fenêtre de Nbeam n'apparaît plus dans la liste des fenêtres à capturer.

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
