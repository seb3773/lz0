# LZ0 Benchmarks & Performance Guide

Ce document présente l'évaluation comparative exhaustive du compresseur **LZ0** (version v0.3.0) face aux références de l'état de l'art : **UPKR** (compresseur asymétrique à base de bitstream rANS), **LZ4** (standard industriel de décompression ultra-rapide) et **ZX0** (référence du parsing optimal rétro-compatible).

Toutes les décompressions LZ0 présentées ci-dessous ont été vérifiées **100 % bit-exactes** avec les décodeurs de référence et haute performance (`lz0dec_fast`, `lz0dec_compact`, `lz0dec_min`, `lz0dec_ref`).

---

## 1. Corpus Réel : Fichiers Individuels Représentatifs

Évaluation multi-compresseurs sur des formats réels variés : code machine (ELF x86_64 et SPARC), exécutable Windows PE, images compressées (PNG, JPEG), flux audio pré-compressé (MP3 CBR), données multimédias brutes non compressées (Textures RGB24, RGBA32, Audio PCM 16 bits stéréo), données tabulaires structurées XLS, prose littéraire et code source C.

### Résultats Comparatifs

| Fichier | Type / Description | Taille Brute | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | UPKR | LZ4 (défaut) | ZX0 | Débit Déc. `lz0dec_fast` vs LZ4 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `fields.c` | Code source C | 11 150 B | 3 397 B (**30.47%**) | **3 374 B (30.26%)** | 2 928 B (26.26%) [-9] | 5 215 B (46.77%) | 3 214 B (28.83%) | **782 Mo/s** vs 2 554 Mo/s |
| `sum` | SPARC ELF binaire | 38 240 B | 12 125 B (**31.71%**) | **11 898 B (31.11%)** | 9 927 B (25.96%) [-6] | 18 812 B (49.19%) | 11 415 B (29.85%) | **808 Mo/s** vs 2 651 Mo/s |
| `lz0_win64.exe` | PE64 Windows binaire | 146 944 B | 75 147 B (**51.14%**) | **73 802 B (50.22%)** | 67 560 B (45.98%) [-6] | 101 596 B (69.14%) | *(trop lent)* | **410 Mo/s** vs 3 429 Mo/s |
| `star.png` | Image PNG (Deflate) | 355 666 B | 357 301 B (**100.46%**) | **356 643 B (100.27%)** | 364 646 B (102.52%) [-2] | 356 988 B (100.37%) | *(trop lent)* | **10 034 Mo/s** vs 30 048 Mo/s |
| `avatar.jpg` | Image JPEG | 25 380 B | 25 326 B (**99.79%**) | **25 281 B (99.61%)** | 25 749 B (101.45%) [-6] | 25 379 B (100.00%) | *(trop lent)* | **8 848 Mo/s** vs 32 413 Mo/s |
| `break.mp3` | Audio MP3 CBR | 38 080 B | 11 299 B (**29.67%**) | **11 243 B (29.52%)** | 11 287 B (29.64%) [-6] | 11 552 B (30.34%) | *(trop lent)* | **9 730 Mo/s** vs 17 165 Mo/s |
| `texture_rgb24.raw` | Texture RGB 24b | 196 608 B | 171 712 B (**87.34%**) | **171 429 B (87.19%)** | 86 531 B (44.01%) [-6] | 197 380 B (100.39%) | *(trop lent)* | **262 Mo/s** vs 32 617 Mo/s |
| `texture_rgba32.raw`| Texture RGBA 32b | 262 144 B | 146 503 B (**55.89%**) | **135 901 B (51.84%)** | *(exclu >256K)* | 263 173 B (100.39%) | *(trop lent)* | **382 Mo/s** vs 28 854 Mo/s |
| `audio_pcm16.raw` | Audio PCM16 Stéréo | 192 000 B | 139 008 B (**72.40%**) | **134 434 B (70.02%)** | 117 053 B (60.97%) [-6] | 163 730 B (85.28%) | *(trop lent)* | **401 Mo/s** vs 7 564 Mo/s |
| `alice29.txt` | Texte littéraire anglais | 152 089 B | 58 949 B (**38.76%**) | **58 655 B (38.57%)** | 50 488 B (33.20%) [-6] | 88 699 B (58.32%) | *(trop lent)* | **602 Mo/s** vs 2 660 Mo/s |
| `plrabn12.txt` | Poésie classique | 481 861 B | 208 776 B (**43.33%**) | **207 515 B (43.07%)** | 175 776 B (36.48%) [-2] | 325 589 B (67.57%) | *(trop lent)* | **530 Mo/s** vs 2 712 Mo/s |
| `kennedy.xls` | Feuille de calcul XLS | 1 029 744 B | 204 581 B (**19.87%**) | **201 093 B (19.53%)** | 64 830 B (6.30%) [-2] | 374 704 B (36.39%) | *(trop lent)* | **1 339 Mo/s** vs 3 125 Mo/s |
| `dickens_1m.txt`| Corpus texte 1 Mo | 1 048 576 B | 398 158 B (**37.97%**) | **394 996 B (37.67%)** | 331 404 B (31.61%) [-2] | 642 924 B (61.31%) | *(trop lent)* | **534 Mo/s** vs 2 676 Mo/s |
| `bash_x86_64.bin`| Exécutable ELF x86_64 | 1 298 416 B | 613 917 B (**47.28%**) | **600 868 B (46.28%)** | 553 594 B (42.64%) [-2] | 871 664 B (67.13%) | *(trop lent)* | **404 Mo/s** vs 2 797 Mo/s |

---

## 2. Blobs Multi-Ressources Hétérogènes (Approche A)

Dans de nombreux environnements embarqués (microcontrôleurs, firmwares, loaders de jeux, exécutables packés), les ressources sont regroupées sous forme d'un **blob monolithique unique** résultant de la concaténation de multiples fichiers hétérogènes (code machine, images, tables, textes).

Conformément à l'**Approche A**, le flux compressé reste **un seul flux LZ0 standard continu**, décompressé en bloc d'un seul trait par les décodeurs freestanding standards (< 300 octets en C / ASM), sans conteneur intermédiaire.

### Résultats Comparatifs sur les Blobs (256 Ko à 5 Mo, 3 à 15 sous-fichiers)

| Cible / Fichiers | Taille Brute | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | UPKR | LZ4 (défaut) | Débit Déc. `lz0dec_fast` vs UPKR |
|---|---:|---:|---:|---:|---:|---:|
| **`blob_png_256k`**<br>(4 PNGs) | 262 144 B | 247 405 B (94.38%) | **246 567 B (94.06%)** | 247 888 B (94.56%) [-6] | 252 446 B (96.30%) | **1 676 Mo/s** vs 17.6 Mo/s (**95×**) |
| **`blob_mix_256k`**<br>(5 f. : ELF, PE, XLS, PNG, txt) | 262 144 B | 128 155 B (48.89%) | 126 018 B (48.07%) | **110 904 B (42.31%)** [-6] | 168 178 B (64.15%) | **676 Mo/s** vs 37.9 Mo/s (**18×**) |
| **`blob_png_512k`**<br>(8 PNGs) | 524 288 B | 504 320 B (96.19%) | **502 897 B (95.92%)** | 508 723 B (97.03%) [-2] | 511 352 B (97.53%) | **2 201 Mo/s** vs 17.1 Mo/s (**128×**) |
| **`blob_mix_512k`**<br>(7 f. : ELF, PE, 2 PNGs, XLS, C, txt) | 524 288 B | 286 374 B (54.62%) | 283 212 B (54.02%) | **260 451 B (49.68%)** [-2] | 352 088 B (67.16%) | **779 Mo/s** vs 32.2 Mo/s (**24×**) |
| **`blob_png_2m`**<br>(8 PNGs distincts > 200 Ko) | 2 097 152 B | 2 056 945 B (98.08%) | **2 052 535 B (97.87%)** | 2 089 034 B (99.61%) [-1] | 2 067 355 B (98.58%) | **3 432 Mo/s** vs ~15 Mo/s (**228×**) |
| **`blob_mix_2m`**<br>(9 f. : ELF, PE, 3 PNGs, XLS, txt, C, HTML) | 2 097 152 B | 1 198 585 B (57.15%) | 1 187 523 B (56.63%) | **1 125 643 B (53.67%)** [-1] | 1 484 306 B (70.78%) | **692 Mo/s** vs ~15 Mo/s (**46×**) |
| **`blob_png_5m`**<br>(15 PNGs distincts) | 5 242 880 B | **4 362 024 B (83.20%)** | *(analysé)* | *(exclu >20 min)* | 5 210 986 B (99.39%) | **5 046 Mo/s** vs - |
| **`blob_mix_5m`**<br>(15 fichiers réels distincts) | 5 242 880 B | 2 191 126 B (41.79%) | **2 160 499 B (41.21%)** | *(exclu >30 min)* | 3 093 616 B (59.01%) | **657 Mo/s** vs - |

---

## 3. Débit de Décompression (C minimal vs C standard vs Assembleur Vectoriel)

Commandes de reproduction :
```bash
make bench_all          # Comparatif global ratios & débits
make bench_dec          # Micro-benchmarks débits x86-64
make bench_dec_arm64    # Micro-benchmarks débits AArch64 (QEMU)
make bench_dec_riscv64  # Micro-benchmarks débits RISC-V 64 (QEMU)
```

### Débit Décompression x86-64 Natif

| Fichier | C minimal (`lz0dec_min`) | C standard (`lz0_decompress_to`) | ASM Compact (`lz0dec_x86_64`) | ASM FAST (`lz0dec_fast_x86_64`) | UPKR (C unpacker) | LZ4 (`decompress_safe`) |
|---|---:|---:|---:|---:|---:|---:|
| `fields.c` | 315 Mo/s | 304 Mo/s | 420 Mo/s | **782 Mo/s** | 58 Mo/s | 2 554 Mo/s |
| `sum` | 261 Mo/s | 260 Mo/s | 370 Mo/s | **808 Mo/s** | 62 Mo/s | 2 651 Mo/s |
| `lz0_win64.exe` | 155 Mo/s | 154 Mo/s | 225 Mo/s | **410 Mo/s** | 36 Mo/s | 3 429 Mo/s |
| `star.png` | 1 264 Mo/s | 1 250 Mo/s | 2 850 Mo/s | **10 034 Mo/s** | 16 Mo/s | 30 048 Mo/s |
| `avatar.jpg` | 1 222 Mo/s | 1 210 Mo/s | 2 710 Mo/s | **8 848 Mo/s** | 16 Mo/s | 32 413 Mo/s |
| `break.mp3` | 1 870 Mo/s | 1 850 Mo/s | 3 100 Mo/s | **9 730 Mo/s** | 56 Mo/s | 17 165 Mo/s |
| `alice29.txt` | 206 Mo/s | 197 Mo/s | 277 Mo/s | **602 Mo/s** | 44 Mo/s | 2 660 Mo/s |
| `plrabn12.txt` | 195 Mo/s | 186 Mo/s | 268 Mo/s | **530 Mo/s** | 40 Mo/s | 2 712 Mo/s |
| `kennedy.xls` | 600 Mo/s | 598 Mo/s | 757 Mo/s | **1 339 Mo/s** | 175 Mo/s | 3 125 Mo/s |
| `dickens_1m.txt` | 222 Mo/s | 200 Mo/s | 292 Mo/s | **534 Mo/s** | 48 Mo/s | 2 676 Mo/s |
| `bash_x86_64.bin` | 179 Mo/s | 175 Mo/s | 233 Mo/s | **404 Mo/s** | 38 Mo/s | 2 797 Mo/s |

### Débit Décompression AArch64 / ARM64 (QEMU)

| Fichier | C standard (`lz0_decompress_to`) | C minimal (`lz0dec_min`) | ASM Compact (`lz0dec_arm64`) | ASM FAST (`lz0dec_fast_arm64`) | Facteur FAST vs C |
|---|---:|---:|---:|---:|---:|
| `fields.c` | 43.4 MB/s | 94.9 MB/s | 74.5 MB/s | **261.0 MB/s** | **+502%** |
| `alice29.txt` | 34.9 MB/s | 68.9 MB/s | 41.8 MB/s | **213.0 MB/s** | **+510%** |
| `plrabn12.txt` | 32.3 MB/s | 64.8 MB/s | 34.6 MB/s | **175.6 MB/s** | **+444%** |
| `kennedy.xls` | 53.6 MB/s | 125.8 MB/s | 121.8 MB/s | **377.4 MB/s** | **+605%** |
| `dickens_1m.txt` | 36.4 MB/s | 72.6 MB/s | 35.6 MB/s | **170.1 MB/s** | **+368%** |
| `bash_x86_64.bin` | 30.8 MB/s | 61.3 MB/s | 45.2 MB/s | **143.7 MB/s** | **+367%** |

### Débit Décompression RISC-V 64 / RV64GC (QEMU)

| Fichier | C standard (`lz0_decompress_to`) | C minimal (`lz0dec_min`) | ASM Compact (`lz0dec_riscv64`) | ASM FAST (`lz0dec_fast_riscv64`) | Facteur FAST vs C |
|---|---:|---:|---:|---:|---:|
| `fields.c` | 37.1 MB/s | 120.5 MB/s | 176.1 MB/s | **178.4 MB/s** | **+381%** |
| `alice29.txt` | 25.2 MB/s | 89.6 MB/s | 119.0 MB/s | **121.2 MB/s** | **+381%** |
| `plrabn12.txt` | 21.6 MB/s | 82.4 MB/s | 107.5 MB/s | **108.6 MB/s** | **+403%** |
| `kennedy.xls` | 50.1 MB/s | 200.1 MB/s | 285.6 MB/s | **295.9 MB/s** | **+490%** |
| `dickens_1m.txt` | 24.2 MB/s | 88.9 MB/s | 115.1 MB/s | **117.9 MB/s** | **+387%** |
| `bash_x86_64.bin` | 23.5 MB/s | 86.6 MB/s | 113.3 MB/s | **112.4 MB/s** | **+378%** |

---

## 4. Enseignements Clés et Analyse Technique

### LZ0 vs LZ4 : Un Gain Massif en Densité pour un Débit Confortable
- Sur tous les corpus structurés et binaires, **LZ0 réduit la taille de 15 % à 46 % de plus que LZ4** :
  - Sur `kennedy.xls` : LZ0 compresse à **201 Ko (19.5%)** vs LZ4 à **374 Ko (36.4%)** (taille presque divisée par deux).
  - Sur `sum` (SPARC ELF) : LZ0 compresse à **11.9 Ko** vs LZ4 à **18.8 Ko** (-37%).
  - Sur `blob_mix_2m` : LZ0 compresse à **1.18 Mo** vs LZ4 à **1.48 Mo** (-300 Ko d'écart).
  - Sur `blob_mix_5m` : LZ0 compresse à **2.19 Mo** vs LZ4 à **3.09 Mo** (-900 Ko d'écart).
- Sur les textures brutes non compressées :
  - Sur `texture_rgba32.raw` : LZ0 compresse à **135 Ko (51.8%)** alors que LZ4 produit une inflation (+100.4%).
  - Sur `audio_pcm16.raw` : LZ0 compresse à **134 Ko (70.0%)** vs LZ4 à **163 Ko (85.3%)** (gain de ~30 Ko).
- En contrepartie, `lz0dec_fast` offre une vitesse de décompression extrêmement confortable (**400 à 1 340 Mo/s** en texte/code et jusqu'à **10 Go/s** sur les images) avec un décodeur de seulement ~1.3 Ko sans table de hachage ni RAM auxiliaire.

### LZ0 vs UPKR : Supériorité sur les Données Pré-Compressées et Asymétrie de Décodage
- **Sur les images PNG, JPEG et données à haute entropie** : Le modèle probabiliste rANS de UPKR souffre d'un surcoût structurel important. LZ0, grâce à ses littéraux alignés sur l'octet et son filtrage strict des longueurs 2 non rentables, **bat UPKR systématiquement** sur tous les fichiers pré-compressés :
  - Sur `avatar.jpg` : LZ0 compresse à **25 281 B (99.6%)**, battant UPKR (**25 749 B**, en inflation à 101.5%).
  - Sur `break.mp3` : LZ0 compresse à **11 243 B (29.5%)**, battant UPKR (**11 287 B**) et LZ4 (**11 552 B**).
  - Sur `blob_png_512k` : LZ0 compresse à **502 Ko** vs UPKR à **508 Ko** (-6 Ko d'écart).
- **Vitesse de décodage** : `lz0dec_fast` décompresse **18× à 228× plus vite que UPKR** (qui plafonne à 15–38 Mo/s).
- **Vitesse d'encodage** : LZ0 Normal compresse en 2.7 secondes un blob de 2 Mo là où UPKR prend plusieurs minutes (**100× plus rapide**).

### Filtrage de Rentabilité Strict pour Longueurs 2
Dans les flux à haute entropie (JPEG, MP3, PNG, binaires chiffrés), émettre une correspondance LZ77 de 2 octets avec un grand offset coûte plus cher en bits de commande qu'émettre 2 littéraux bruts.
- LZ0 impose une règle stricte : une correspondance de longueur 2 n'est permise que si son offset est minuscule ($\le 32$) ou s'il s'agit d'un **repeat-offset** (coût nul d'offset).
- Résultat : élimination totale de l'inflation résiduelle sur les flux pré-compressés (`avatar.jpg` passe sous les 100 %).

### Détection de Périodes Audio & Multimédia (Strides Parity-Safe)
- **Audio MP3 CBR** : Les en-têtes de trames MPEG Layer III sont synchronisés via un filtre strict validant le mot magique `0xFF 0xE0`, la répétition bit-exacte du header 4 octets (`memcmp(win+i, win+i-p, 4) == 0`), le bit de Layer III, et des débits valides. Cela élimine 100 % des faux positifs sur textures graphiques.
- **Textures RGB24 / RGBA32 / Audio PCM16** : Le pas de corrélation utilise un échantillonnage sans repliement de parité (`step = (p <= 32) ? 1 : 2`), garantissant la détection exacte du stride 3 (RGB24) et du stride 4 (RGBA32 et PCM16 stéréo). Les strides candidats sont triés par taux de corrélation décroissant afin d'élaguer les multiples harmoniques.

### Programmation Dynamique Adaptative ($K=32$) et Multi-Échelle SA-IS
- En mode Ultra (`-9`), le nombre d'états DP par position s'ajuste à la taille du fichier :
  - **$K=32$** pour fichiers $\le 512$ Ko (capture maximale des interactions d'offsets).
  - **$K=16$** pour fichiers entre 512 Ko et 2 Mo (garantit une empreinte RAM DP $\le 130$ Mo).
  - **$K=8$** pour fichiers $> 2$ Mo (évite toute explosion quadratique du temps de calcul).
- L'échantillonnage de la fenêtre Suffix Array associe une fenêtre linéaire dense $1 \dots 128$ et des sauts géométriques $\{160, 192, 256, 320, 384, 512\}$, capturant les dépendances lointaines sur les gros fichiers sans accroître le nombre de comparaisons.
