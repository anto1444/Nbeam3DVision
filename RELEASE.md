# Nbeam 3D Vision 2.1.0

Mise à jour de la version 2.0.0. Surcouche Windows : BFI, CRT Beam Simulator et 3D active (lunettes NVIDIA 3D Vision ou émetteur Arduino), sur n'importe quel écran.

> **À savoir après la mise à jour** : les **profils ne s'appliquent plus que quand leur programme (.exe) est détecté**. Sans programme détecté, Nbeam utilise les **réglages du bureau** (`Profils\Bureau.cfg`), créés au premier lancement à partir de votre ancien profil par défaut (ou du profil qui était actif) : vous retrouvez vos réglages. La case « Profil par défaut » et la liste des profils du menu de l'icône de notification disparaissent. Le mode fenêtre est toujours actif.

## Nouveautés

### Profils
- **Réglages du bureau** séparés des profils : un profil ne s'applique que pendant que son programme tourne, puis le bureau revient.
- **Profil à régler** : choisissez un profil pour le régler sans lancer son jeu. Le test des voitures s'ouvre avec ce profil chargé et le HUD par-dessus ; en quittant le test (touche Windows ou Échap), le profil est enregistré et on revient au bureau.
- **Créer un profil / Enregistrer sous** ne change plus les réglages actifs.
- **Changement instantané** : changer le profil du programme en cours dans la détection l'applique tout de suite.
- Tant qu'un programme détecté tourne, les réglages des profils sont masqués et un message indique le programme et le profil utilisé.

### Test (nouvelle catégorie, icône de compteur de vitesse)
- **Test de mouvement** : des voitures détaillées, de la couleur d'accent, défilent dans une **fenêtre DirectX à part** que Nbeam capture comme un jeu, à travers tous les effets (BFI, CRT, 3D) : on compare le flou de mouvement d'un réglage à l'autre.
- **En 3D** (côte à côte, haut/bas, damier) : les voitures et leur grille sortent de l'écran et portent la lettre de leur œil (G / D) de chaque côté de « NBEAM » : si l'on voit l'autre lettre, les yeux sont inversés.
- **Vitesse des voitures** (0 à 3840 px/s), **taille des voitures** (25 à 200 %) et **couleur du fond**.
- **Écran partagé** déplacé dans Test, avec la case **Inverser** (l'effet sur l'autre moitié).
- La catégorie Test est masquée tant qu'un programme détecté tourne.

### 3D
- **Convergence pour tous les formats 3D** (côte à côte, haut/bas, damier, plus seulement la 2D vers 3D), réglée **de -100 à +100** : négatif = dans la profondeur de l'écran, positif = sort de l'écran, 0 = profondeur par défaut.
- **Avertissement au démarrage** si aucun émetteur 3D n'est reconnu, avec le lien cliquable du pilote NVIDIA, et rappel en rouge en bas de la fenêtre tant qu'il n'est pas branché.

### Application
- **Réduire au lieu de fermer** remplace l'option Mode fenêtre (toujours actif) : la croix réduit Nbeam (dans la zone de notification si l'option est cochée) au lieu de le quitter.

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
