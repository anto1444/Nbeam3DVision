# Nbeam 3D Vision

**Version 3.0.0 — par anto1444, 2026**

<p align="center"><img src="docs/nbeam.jpg" alt="Nbeam 3D Vision" width="420"></p>

**La résurrection de l'émetteur NVIDIA 3D Vision** : Nbeam pilote directement l'émetteur USB, sans pilote d'affichage « 3D Vision » ni écran certifié. La 3D active fonctionne donc **partout sur le bureau, sur n'importe quel écran, même en fenêtré**, en capturant un programme précis. Les **changements de profil et les réglages des lunettes sont instantanés** : on règle en direct, et chaque jeu retrouve tout seul ses propres réglages.

Nbeam 3D Vision est une surcouche (overlay) Windows qui s'affiche par-dessus un jeu ou une vidéo et réunit, sur **n'importe quel écran** :

- le **BFI** et le **CRT Beam Simulator** pour supprimer le flou de mouvement d'un écran LCD/OLED rapide ;
- la **3D active** (lunettes NVIDIA 3D Vision ou émetteur Arduino) à partir d'un contenu 3D en côte à côte, haut/bas, damier ou entrelacé (lignes ou colonnes), avec ou sans les effets ci-dessus, ou depuis une image 2D grâce à l'IA ;
- des **profils** par jeu, chargés automatiquement.

> **Attention** — ce logiciel produit des images qui clignotent très vite. Prudence si vous êtes photosensible.

> **Attention — risque pour l'écran** : un écran trop lent peut **marquer temporairement la dalle** (rémanence) avec le BFI ou la simulation CRT. Utilisation **à vos risques et périls**.

## Avertissement au premier démarrage

Au tout premier lancement (quand le dossier `Profils` est créé), Nbeam affiche un avertissement.

## Prérequis

- Windows 10 (1903) ou 11, 64 bits, carte graphique DirectX 11 (RTX 3050 ou AMD équivalent recommandé).
- Processeur : **4 cœurs / 8 threads minimum** (Intel Core i5 de 8e génération ou AMD Ryzen 3 3100), **6 cœurs / 12 threads recommandé** (Intel Core i5-10400 ou AMD Ryzen 5 3600 et plus récents). Nbeam garde un thread à haute priorité pour le rendu et un autre pour la synchro des lunettes, et le beam racing occupe un cœur pendant l'attente ; le jeu doit garder assez de cœurs libres. La 2D vers 3D (IA) tourne sur la carte graphique (DirectML) ; sur le processeur seul, elle demande un processeur bien plus puissant.
- Écran : **100 Hz minimum** pour la 3D **ou** le BFI, **240 Hz ou plus** pour les deux ensemble. Idéal : un écran **OLED**, ou un LCD à temps de réponse rapide.
- 3D NVIDIA : l'émetteur USB et son pilote. 3D Arduino : un émetteur compatible sur un port COM.

## Démarrage rapide

1. Placez `Nbeam3DVision.exe` dans un dossier où il peut écrire.
2. Lancez-le : le dossier `Profils\` et le profil d'exemple `Defaut.ini` sont créés.
3. Dans le HUD, choisissez l'**écran de sortie** (votre écran rapide) et le nombre de **sous-images**, puis lancez votre jeu.

Principe : Nbeam **capture** l'écran ou la fenêtre d'un programme, applique l'effet puis **réaffiche** l'image en plein écran au rythme de l'écran de sortie. Les **sous-images** sont le nombre d'images affichées pour chaque image du contenu : sur 240 Hz, 4 sous-images = un contenu à 60 images/s.

## BFI simple

Insère des images noires entre les images du jeu.

- **Images claires** : nombre de sous-images qui montrent l'image (jamais plus que le nombre de sous-images). Moins d'images claires = mouvement plus net, mais moins lumineux et plus de scintillement.
- **Gain vs flou** : noirceur des images insérées. 1 = noir complet (BFI classique) ; plus bas, une partie de l'image reste visible (plus lumineux, moins de scintillement, un peu plus de flou).

## CRT Beam Simulator

Reproduit le balayage d'un tube cathodique : l'image est « dessinée » ligne par ligne sur les sous-images, avec une fin de persistance du phosphore. Le mouvement est net et plus doux que le BFI classique.

- **Gain vs flou** : compromis luminosité / flou. 1 = luminosité inchangée ; plus bas = moins de flou sur les zones sombres, au prix de la luminosité.
- **Gamma** : le gamma de votre écran (évite les bandes horizontales).
- **Direction du balayage**, **Mode ralenti** (pour observer la simulation), **Anti-rétention LCD** (évite la rétention d'image sur LCD), **Niveau de noir** (ce qui est plus sombre devient un noir pur ; utile sur OLED et en HDR).

> **Images en file à 0 (tearing) : déconseillé avec le BFI et le CRT.** La déchirure peut couper une image claire ou noire en deux et provoquer des bandes ou un scintillement irrégulier. Gardez une valeur de file de 1 ou plus, ou placez la ligne de tearing tout en haut de l'image (masquée) avec le contrôle du tearing.

### BFI adaptatif (Sous-images : Auto)

Quand un programme est capturé, le curseur **Sous-images** propose **Auto** (avant le 1). C'est le **BFI adaptatif** : Nbeam mesure en continu la cadence réelle du jeu et ajuste tout seul le nombre de sous-images (120 images/s sur 480 Hz → 4, 240 images/s → 2). Ses atouts :

- l'effet **reste net et régulier** même quand le jeu change de cadence (cinématique en 30 images/s, menu, chute d'images) : plus besoin de choisir un réglage par jeu ni de le corriger à la main ;
- au-dessus d'un **seuil de sécurité** réglable en sous-images (10 sur un écran 480 Hz = 48 Hz ; indépendant du BFI auto, il marche aussi en réglage manuel et sur le bureau), le BFI est doublé ou, avec l'interrupteur, coupé pour éviter un scintillement lent et fatigant ;
- la réactivité se règle avec l'**intervalle de détection**.

> **Attention** — pour un meilleur résultat, privilégiez toujours une cadence de jeu qui **divise exactement la fréquence de l'écran** (60, 80, 120 ou 240 images/s sur 480 Hz, par exemple). Sinon (73 images/s sur 480 Hz…), le nombre de sous-images ne tombe pas juste et le jeu paraîtra saccadé.

Auto n'est pas disponible avec la 3D (les sous-images suivent alors l'émetteur).

### Tampon d'image

Le **tampon d'image** garde quelques images du jeu d'avance pour **lisser un jeu à cadence irrégulière**, au prix d'un peu de latence (uniquement avec la capture d'un programme). 0 = désactivé, 1 à 5 = nombre d'images d'avance. En **Auto**, Nbeam adapte lui-même la taille (0 à 5) : elle augmente tout de suite après un à-coup et redescend doucement ; le tampon ne repasse à 0 (sans latence ajoutée) qu'après 4 secondes de jeu stable.

Avec les **sous-images sur une valeur fixe** (pas en Auto), le tampon gère aussi un jeu qui tourne à un **diviseur de la cadence du contenu** : pour un contenu à 120 fps et un jeu à 60, 40, 30 ou 15 fps, chaque image du jeu est affichée exactement 2, 3, 4 ou 8 fois, régulièrement, au lieu d'une alternance irrégulière.

Avec la **2D vers 3D (IA)**, le tampon d'image fonctionne comme ailleurs. La synchronisation des cartes de profondeur est une option à part, le **Tampon de carte** (panneau 3D, sous « Afficher la carte de profondeur ») : le calcul de la profondeur prend du temps et la carte arrive en retard sur l'image ; coché, Nbeam mesure ce délai (affiché dans le panneau 3D) et retient chaque image jusqu'à ce que sa propre carte soit prête, pour que **la profondeur colle à l'image**. Il s'ajoute au tampon d'image (les deux délais se cumulent). Décoché, aucune synchronisation : pas de latence ajoutée, mais la profondeur peut être en retard sur les mouvements rapides.

## 3D active

- **Formats** : côte à côte (SBS), haut/bas (TB), damier, entrelacé horizontal (œil gauche sur les lignes paires, droit sur les impaires), entrelacé vertical (idem avec les colonnes), ou **2D vers 3D (IA)**.
- **Source auto** (interrupteur au-dessus de « Format de la source 3D », qui est masqué tant qu'elle est activée) : Nbeam utilise une technique de détection automatique par algorithme sur l'image capturée et passe tout seul sur le format 3D détecté (SBS, TB, damier, entrelacé horizontal ou vertical), comme si vous l'aviez choisi. Rien ne change tant qu'aucune source 3D n'est trouvée ; pour changer le format à la main, désactivez-la.
- **SBS complet** (expérimental) : Nbeam redimensionne la fenêtre du jeu à deux fois la largeur de l'écran pour que chaque moitié soit un œil entier, sans déformation (capture de jeu uniquement).
- **Mini menu 3D** (bouton de l'émetteur) : la molette navigue, un clic sélectionne un réglage, la molette le règle, un clic valide ; le mini menu ne s'ouvre qu'en capture de jeu ; en mode bureau (effets activés ou non), le bouton sert uniquement à ajouter le programme aux profils, exactement comme le raccourci « Ajouter le programme aux profils », et la notification s'affiche dans le coin de la bannière même quand les effets sont désactivés. Le HUD et le mini menu ne sont jamais affichés ensemble (afficher le HUD ferme le mini menu), et la fréquence de l'émetteur ne se change pas depuis le mini menu avec la synchro Auto ou la source auto. Pendant un réglage, un curseur arrondi aux couleurs d'accent remplace le menu. La dernière ligne, « Désactiver les effets », coupe les effets.
- La bascule des yeux se fait à Hz de l'écran ÷ sous-images ; chaque œil reçoit la moitié. En 3D, les sous-images sont **liées à la fréquence de l'émetteur** (480 Hz et émetteur à 120 Hz → 4). La case **Auto** de la synchronisation détecte la fréquence de l'émetteur entre 80 et 165 Hz (hors de cette plage, rien ne se passe et l'émetteur reste éteint).
- **Synchro des lunettes automatique** : les lunettes suivent les vrais rafraîchissements de l'écran (statistiques DXGI) dès que Windows les fournit, sinon la synchronisation classique prend le relais ; une ligne sous « Inverser les yeux » indique le mode utilisé. Le **Sync Offset** est toujours compté depuis l'affichage réel de l'image : changer les images en file ou de mode de synchro ne le dérègle pas. Une image envoyée en retard (surcharge, chargement du jeu) est remise sur le rythme régulier de l'écran, les lunettes ne se décalent plus.
- Réglez d'abord **Sync Offset**, puis **Open Duration**, en regardant l'image à travers les lunettes, **avec les raccourcis clavier** (le HUD affiché fausse le rythme).
- **Correction de l'offset** (au-dessus de « Inverser les yeux », désactivée par défaut) : à basse file d'images, l'écran décale parfois l'image d'un rafraîchissement (micro-désynchronisation des lunettes). Avec cette option, Nbeam règle alors la durée d'ouverture 1 µs en dessous de la valeur que vous avez réglée, puis de nouveau sur cette valeur à la fois suivante ; l'émetteur redémarre ainsi son offset. Seule la valeur que vous avez réglée est enregistrée. Avec des sous-images noires (BFI / CRT), le changement part dans la première image noire. Au plus deux fois par seconde. **Attention : ce paramètre peut créer de légers scintillements ; si vous y êtes sensible, désactivez-le et augmentez les images en file.**
- **Suivi d'événements** : la touche O (3D active) enregistre les 3 dernières secondes du timing des lunettes dans `Nbeam_desync.log`, à presser juste après une désynchronisation pour voir quand elle s'est produite. L'option « Enregistrer le journal d'événements » (section Application, activée par défaut) coupe tout le suivi d'événements quand elle est décochée.
- Le **tearing** (option des images en file, 0 = tearing autorisé) donne la latence la plus basse en 2D ; le **contrôle du tearing** place la ligne de déchirure (0 = tout en haut, masquée).
- **Beam racing** (avec les images en file à 0) : Nbeam lit la position réelle du balayage de l'écran et n'envoie l'image que là où la déchirure ne se voit pas. BFI et image simple : juste avant le retour vertical (aucune déchirure visible). CRT Beam Simulator (balayage haut-bas ou bas-haut) : aussi au-dessus des bandes allumées, pour une latence plus basse. L'**avance du tearing** compense le délai d'envoi (augmentez-la si une déchirure apparaît en haut de l'image). Occupe un cœur du processeur ; nécessite un écran dont la position de balayage est lisible et rien par-dessus la fenêtre de Nbeam. Fonction expérimentale. **Inactif en 3D** : les lunettes gardent la synchronisation classique.

**2D vers 3D (IA)** : un modèle de profondeur estime la profondeur de chaque pixel pour créer les deux vues. Il faut placer à côté de l'exe `onnxruntime.dll`, `onnxruntime_providers_shared.dll` et un modèle `.onnx` (ex. `model_fp16.onnx`) ; ces fichiers ne sont exigés que pour cette source. Les bords gauche et droit sont masqués par des bandes noires droites (à la profondeur de l'écran) ; **Inverser les yeux** inverse aussi la profondeur ; l'**anti-halo** (jusqu'à 300 %) limite le débordement de la profondeur d'un objet sur le fond ; le **Tampon de carte** cale la profondeur sur l'image (voir Tampon d'image).

> **Ces fichiers ne sont pas fournis avec Nbeam** (pour des raisons de licence et de taille). `onnxruntime.dll` et `onnxruntime_providers_shared.dll` proviennent d'une version DirectML d'[ONNX Runtime](https://github.com/microsoft/onnxruntime/releases) (Microsoft, MIT) ; le modèle `.onnx` est un modèle d'estimation de profondeur à fournir vous-même, en respectant sa licence. Copiez ces trois fichiers à côté de `Nbeam3DVision.exe`. Sans eux, Nbeam fonctionne normalement : seule la source « 2D vers 3D (IA) » est indisponible.

## Raccourcis clavier

Tous les raccourcis sont **entièrement personnalisables** dans la section **Raccourcis** du HUD :

- **Clavier** : cliquez sur un raccourci, puis appuyez sur la nouvelle combinaison (au moins Ctrl, Alt, Maj ou Windows ; Échap annule). Ils fonctionnent partout, même dans un jeu en plein écran.
- Les actions couvrent l'affichage du HUD, le redémarrage, la sortie, l'inversion des yeux en 3D, les sous-images, les images claires, le Sync Offset et l'Open Duration, l'**affichage / masquage des effets** (Ctrl+Maj+D par défaut), l'**ajout du programme aux profils** (voir Profils). Les raccourcis du HUD et des effets ne peuvent pas être vides.

Les raccourcis sont les mêmes pour tous les profils : changer de profil ne les modifie jamais.

**Afficher / Masquer les effets** : le bouton du bas du HUD, le raccourci et le menu de l'icône de notification coupent **tous les effets et l'émetteur 3D** (l'image s'affiche telle quelle, pour comparer). Ils restent coupés, HUD ouvert ou fermé, jusqu'à « Afficher ». Ouvrir le HUD ne coupe plus les effets.

## Profils

Un **profil** est un fichier `.ini` du dossier `Profils\` qui mémorise tous les réglages de rendu, d'affichage et de 3D. On en change depuis le HUD ou le menu de l'icône de notification.

Une **détection de profil** associe un programme (`.exe`) à un profil. Pour ajouter un programme, saisissez son nom d'exe, ou, depuis le programme lui-même, appuyez sur le raccourci **Ajouter le programme aux profils** (Ctrl+Maj+P par défaut) : son exe est ajouté à la liste. Ensuite :

- quand le programme est détecté (fenêtre visible dans la barre des tâches, après un délai réglable), son profil est chargé ; il n'est capturé qu'une fois qu'il montre du mouvement (deux contrôles à 1 seconde d'écart : pas de capture sur une bannière de lancement) et le HUD se réduit. Tant que la fenêtre est illisible (masquée ou toute blanche) ou immobile (menu fixe, fenêtre de configuration graphique du jeu), rien n'est appliqué ;
- dès que le programme disparaît de la barre des tâches, Nbeam repasse en mode bureau et le **profil par défaut** revient (case « Profil par défaut » à côté du choix du profil), ou le profil précédent si aucun n'est défini ; tout autre profil que celui par défaut réaffiche les effets s'ils avaient été masqués ;
- un programme a **un seul profil**, le même pour tous les écrans ; chaque écran mémorisé (3 au maximum) a ses propres réglages : case **Activé**, **taux de rafraîchissement** (appliqué à la détection active puis rétabli), **plafond d'images auto**, **limite d'images** et **délai d'activation**. Les réglages d'un écran non connecté sont ignorés jusqu'à sa reconnexion ;
- **Limite d'images** (RTSS, Display Commander ou ReLimiter) : avec Display Commander / ReLimiter, Nbeam écrit la limite de chaque programme dans le `DisplayCommander.ini` / `relimiter.ini` à côté du jeu, pour tous les programmes à la fois, au démarrage de Nbeam, au changement d'écran principal de Windows et après la fermeture d'un jeu : elle est prête avant le lancement. Un jeu sans ce fichier est ignoré ;
- une détection peut être désactivée sans être supprimée ;
- la ligne **Paramètres globaux** règle une valeur pour tous les programmes d'un coup (case, fréquence, plafond, limite, délai) ; l'interrupteur « Activé » reste à moitié quand certains programmes sont activés et d'autres non (un clic les active tous). Chaque programme reste modifiable sur sa ligne ensuite. Sous Profil, **Profil à l'ajout d'un programme** (Aucun ou un profil) est donné automatiquement à chaque programme ajouté (HUD, raccourci ou bouton de l'émetteur).

**Écran de sortie « Automatique »** : un jeu en plein écran ou sans bordure maximisé met la sortie sur l'écran principal de Windows ; un jeu dans une fenêtre plus petite que l'écran la met par-dessus le jeu (sortie « Processus »).

**Dolby Atmos** : Nbeam joue en continu un son inaudible (environ -90 dB) sur le périphérique audio par défaut pour que Windows garde l'audio spatial actif (interrupteur « Garder le Dolby Atmos actif », section Application).

**Taille auto du HUD** : l'interface suit la mise à l'échelle Windows de l'écran où elle se trouve ; décochée, le curseur « Taille du HUD » revient.

## Tutos d'usage

**Un jeu 60 images/s sur un écran 240 Hz, sans flou** : créez un profil, choisissez le CRT Beam Simulator ou le BFI simple, 4 sous-images, plafonnez le jeu à 60 images/s, puis liez le `.exe` du jeu à ce profil dans « Détection de profil ».

**Un jeu en 3D (côte à côte)** : choisissez le format source SBS, l'émetteur de vos lunettes, puis réglez Sync Offset et Open Duration. En 3D, chaque œil reçoit la moitié des sous-images : visez 240 Hz ou plus pour garder du BFI/CRT en plus.

**3D sans crosstalk (fantômes entre les yeux)** : cumulez la 3D avec le BFI ou le CRT Beam Simulator. Choisissez le format source (ex. SBS) et l'émetteur, puis activez le CRT Beam Simulator ou le BFI simple avec au moins 2 sous-images par œil (visez 240 Hz ou plus : 4 sous-images donnent 2 images pour chaque œil). Les images noires ou le balayage CRT effacent l'image de l'œil précédent avant que l'autre œil s'ouvre, ce qui réduit fortement les doubles images. Ajustez ensuite Open Duration (plus court = moins de fantômes) et Sync Offset en regardant à travers les lunettes.

**Regarder une vidéo 2D en 3D** : choisissez « 2D vers 3D (IA) », puis réglez la force de la profondeur (2 à 4 % est confortable) et la convergence. Laissez **Tampon de carte** coché pour que la profondeur colle à l'image (la latence ajoutée n'a pas d'importance pour une vidéo) ; pour un jeu rapide, décochez-le si la latence gêne.

**Plusieurs écrans** : ajoutez une paire profil/écran par écran dans la détection ; Nbeam choisit celle de l'écran où se trouve le jeu. Quand un écran est branché, débranché ou change de mode, Nbeam redémarre tout seul, réduit (le HUD ne s'ouvre pas). « Désactiver les effets » réduit Nbeam au lieu de le faire réapparaître en plein jeu.

**Latence minimale** : mettez les images en file à « 0 (tearing autorisé) » et réglez le contrôle du tearing pour cacher la déchirure en haut de l'image.

## Conseils

- Branchez un seul écran, ou faites tourner Nbeam sur le plus rapide.
- Plafonnez le jeu à la valeur « FPS contenu » du HUD.
- Si l'image saccade, augmentez la file d'images ou mettez le tampon d'image sur Auto.

## Crédits et remerciements

Nbeam 3D Vision repose sur le travail d'autres auteurs, un grand merci à eux :

- **[ShaderBeam](https://github.com/mausimus/ShaderBeam)** par **mausimus** (licence MIT) : la base de tout ce projet (capture, rendu, BFI, structure du logiciel).
- **[CRT Beam Simulator](https://github.com/blurbusters/crt-beam-simulator)** par **Mark Rejhon (Blur Busters)** et **Timothy Lottes** : l'algorithme et les shaders de simulation CRT ([article](https://blurbusters.com/crt)).
- **[Dear ImGui](https://github.com/ocornut/imgui)** par Omar Cornut et ses contributeurs : l'interface (HUD).
- **[Inter](https://rsms.me/inter/)** par The Inter Project Authors (licence SIL OFL 1.1) : la police du HUD.
- **[ONNX Runtime](https://github.com/microsoft/onnxruntime)** (Microsoft, MIT) et **[DirectML](https://github.com/microsoft/DirectML)** : l'inférence IA.
- **[Depth Anything V2](https://github.com/DepthAnything/Depth-Anything-V2)** : le modèle de profondeur pour la 2D vers 3D. Le modèle n'est **pas distribué** avec Nbeam : sa licence dépend de sa taille (la version Small est en Apache-2.0, les plus grandes sont en usage non commercial), vérifiez celle du modèle que vous utilisez.
- **[WibbleWobble](https://github.com/PHARTGAMES/WibbleWobbleCore)** (PHARTGAMES, GPL-3.0) : l'inspiration de la 3D active sur écran quelconque et de ses réglages (synchronisation, durée d'ouverture, inversion des yeux).
- **[mINI](https://github.com/metayeti/mINI)** de Danijel Durakovic (licence MIT) : la lecture et l'écriture des fichiers `.ini` des profils.
- **[stb](https://github.com/nothings/stb)** de Sean Barrett (domaine public / MIT), livré avec Dear ImGui : rendu des polices.

Ajouts de Nbeam 3D Vision (3D active avec lunettes, profils et détection par programme, capture de processus, sortie « Processus », tampon d'image et tampon de carte, tearing, 2D vers 3D) : anto1444, 2026.

Nbeam 3D Vision est distribué sous licence MIT, comme ShaderBeam.

*Développé avec l'aide de Claude (Anthropic).*

*NVIDIA et 3D Vision sont des marques de NVIDIA Corporation, citées uniquement pour indiquer la compatibilité. Nbeam 3D Vision n'est ni affilié à NVIDIA, à Blur Busters, à WibbleWobble ni à ShaderBeam, ni approuvé par eux.*
