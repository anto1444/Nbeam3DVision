# Nbeam 3D Vision 3.1.0

Mise à jour de la version 3.0.0 : un effet de balayage unique et sans frange de couleur (**Balayage BFI via MPRT**), choisi par un interrupteur vertical, une lumière et des couleurs fidèles en HDR, une gestion des profils simplifiée et les journaux rangés dans un dossier **log**. Vos profils et réglages sont conservés (les profils sont convertis tout seuls).

> **À savoir après la mise à jour** : le shader `Shaders\CRTBeamSimulator.hlsl` est remplacé par `Shaders\bfi.hlsl` (vous pouvez supprimer l'ancien) ; le réglage Gamma disparaît ; le bouton Supprimer de la gestion des profils est retiré ; les anciens fichiers `.log` à côté de l'exe ne servent plus.

## Nouveautés

### Effets
- **Deux effets seulement : BFI et Balayage BFI via MPRT**, choisis par un **interrupteur vertical** (haut = BFI, bas = balayage MPRT) à la place de la liste « Type ». Le balayage MPRT remplace le CRT Beam Simulator et le Faisceau MPRT. Les profils existants sont convertis : CRT Beam Simulator et Faisceau MPRT → balayage, BFI simple → BFI.
- **Pas de frange de couleur** : chaque pixel reçoit une seule impulsion de lumière, aussi longue que son canal le plus fort et partagée par ses trois canaux de couleur. Un bord saturé en mouvement (un rouge vif sur fond sombre) garde sa teinte, et chaque canal reçoit toujours exactement sa lumière.
- **Lumière et couleurs fidèles, y compris en HDR (float16)** : courbe sRGB standard exacte et fixe (le réglage **Gamma** est supprimé), pas de perte de luminosité sur les pixels très clairs (une impulsion plus longue qu'un cycle est raccourcie et rendue plus lumineuse sur les trois canaux : même énergie, même teinte).
- **Synchro BFI « Hybride » supprimée** : les profils qui l'utilisaient passent en **Calage de phase**.
- **Calage de phase sans micro-scintillement** : dans un cycle raccourci, le balayage MPRT s'étend sur toute l'image (avant, la dernière bande restait noire le temps d'un cycle), et la luminosité du BFI est corrigée aussi avec une image encodée en gamma et des images sombres partielles (erreur jusqu'à 23 % avant) ; cette correction se fait dans la même passe que l'effet, sans passe supplémentaire sur l'image.

### Profils et journaux
- **Gérer les profils** : « Profil à régler » passe en première ligne de **Test de mouvement** ; le bouton **Supprimer** est retiré ; le bouton **Explorateur** est à côté de **Enregistrer sous**.
- **Journaux** : tous les fichiers de suivi (`Nbeam_ls.log`, `Nbeam_desync.log`, `Nbeam_emitter_input.log`, rapport de plantage) sont rangés dans le dossier **`log`**, à côté de l'exe.

### Interface
- Info-bulles, avertissements et traductions (français / anglais) mis à jour pour les deux effets ; texte d'aide du sélecteur d'effet.
- Version 3.1.0 affichée dans le bandeau, la fenêtre « À propos » et les propriétés de l'exe. Nettoyage interne du code : le nom interne ShaderBeam devient Nbeam.

## Corrections

- Lumière et couleurs en HDR / float16 avec le balayage : la capture linéaire n'est plus traitée comme du sRGB (couleurs ternes, ombres écrasées), l'énergie est conservée pour les pixels très clairs.

## Problèmes connus

- **Désynchronisation temporaire quand la souris est visible.** Pour régler la 3D, préférez les raccourcis clavier ou le mini menu de l'émetteur.
- **Lossless Scaling (LSFG) capturé : le pointeur de la souris n'est pas visible dans le HUD.** Les clics marchent, mais il faut viser sans voir le pointeur ; pour taper du texte dans le HUD, arrêtez la mise à l'échelle.
- Les images en file à 0 (tearing) sont déconseillées avec le BFI et le balayage MPRT.

## Contenu de l'archive

- `Nbeam3DVision.exe` : le programme (il lance aussi la fenêtre du test des voitures)
- `Nbeam3DVision.pdb` : les symboles, pour que le journal de plantage indique les noms des fonctions (facultatif, à garder à côté de l'exe)
- `Shaders\bfi.hlsl` : le shader du balayage MPRT (à garder à côté de l'exe)
- `README.md` : le mode d'emploi complet ; `RELEASE.md` : ces notes
- `LICENSE`, `THIRD_PARTY_NOTICES.md` : licences

## Installation / mise à jour

1. Décompressez l'archive dans un dossier où Nbeam peut écrire.
2. Pour une mise à jour, quittez d'abord Nbeam par le menu de l'icône de notification, puis remplacez l'exe, le `.pdb` et le dossier `Shaders` (supprimez l'ancien `CRTBeamSimulator.hlsl`). Vos profils (`Profils\`) et votre configuration sont conservés.
3. Lancez `Nbeam3DVision.exe`.

**2D vers 3D (IA)** : `onnxruntime.dll`, `onnxruntime_providers_shared.dll` et le modèle `.onnx` ne sont **pas fournis** (licence et taille). Copiez-les vous-même à côté de l'exe (voir le README). Sans eux, tout le reste fonctionne.

## Prérequis

- Windows 10 (1903) ou 11, 64 bits, carte graphique DirectX 11 (RTX 3050 ou AMD équivalent recommandé).
- Processeur : 4 cœurs / 8 threads minimum (Intel Core i5 8e génération ou AMD Ryzen 3 3100), 6 cœurs / 12 threads recommandé (Intel Core i5-10400 ou AMD Ryzen 5 3600 et plus récents).
- Écran 80 Hz minimum (100 Hz recommandé pour la 3D ou le BFI), 240 Hz ou plus pour les deux ensemble.

> **Attention** — images qui clignotent très vite (prudence si vous êtes photosensible) ; risque de rémanence temporaire sur un écran lent avec le BFI ou le balayage MPRT. Utilisation à vos risques et périls.

Licence MIT. Basé sur [ShaderBeam](https://github.com/mausimus/ShaderBeam) de mausimus. Crédits complets dans le README.

**Full Changelog** : https://github.com/anto1444/Nbeam3DVision/compare/v3.0.0...v3.1.0
