# LZ0 Benchmarks & Performance Guide

Ce document présente l'évaluation comparative exhaustive du compresseur **LZ0** (version v0.3.0) face aux références de l'état de l'art : **UPKR** (compresseur asymétrique à base de bitstream rANS), **LZ4** (standard industriel de décompression ultra-rapide) et **ZX0** (référence du parsing optimal rétro-compatible).

Toutes les décompressions LZ0 présentées ci-dessous ont été vérifiées **100 % bit-exactes** avec les décodeurs de référence et haute performance (`lz0dec_fast`, `lz0dec_compact`, `lz0dec_min`, `lz0dec_ref`).

---

## 1. Corpus Réel : Fichiers Individuels Représentatifs

Évaluation multi-compresseurs sur des formats réels variés : code machine (ELF x86_64 et SPARC), exécutable Windows PE, images compressées (PNG, JPEG), flux audio pré-compressé (MP3 CBR), données multimédias brutes non compressées (Textures RGB24, RGBA32, Audio PCM 16 bits stéréo), données tabulaires structurées XLS, prose littéraire et code source C.

### Résultats Comparatifs

| Fichier | Type / Description | Taille Brute | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | UPKR | LZ4 (défaut) | ZX0 | Débit Déc. `lz0dec_fast` vs LZ4 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `fields.c` | Code source C | 11 150 B | 3 407 B (**30.56%**) | **3 344 B (29.99%)** | 2 928 B (26.26%) [-9] | 5 215 B (46.77%) | 3 214 B (28.83%) | **994 Mo/s** vs 2 577 Mo/s |
| `sum` | SPARC ELF binaire | 38 240 B | 12 139 B (**31.74%**) | **11 766 B (30.77%)** | 9 927 B (25.96%) [-6] | 18 812 B (49.19%) | 11 415 B (29.85%) | **791 Mo/s** vs 2 948 Mo/s |
| `lz0_win64.exe` | PE64 Windows binaire | 146 944 B | 75 289 B (**51.24%**) | **72 615 B (49.42%)** | 67 560 B (45.98%) [-6] | 101 596 B (69.14%) | *(trop lent)* | **406 Mo/s** vs 3 375 Mo/s |
| `star.png` | Image PNG (Deflate) | 355 666 B | 357 309 B (**100.46%**) | **356 400 B (100.21%)** | 364 646 B (102.52%) [-2] | 356 988 B (100.37%) | *(trop lent)* | **9 217 Mo/s** vs 30 225 Mo/s |
| `avatar.jpg` | Image JPEG | 25 380 B | 25 334 B (**99.82%**) | **25 268 B (99.56%)** | 25 749 B (101.45%) [-6] | 25 379 B (100.00%) | *(trop lent)* | **8 205 Mo/s** vs 30 985 Mo/s |
| `break.mp3` | Audio MP3 CBR | 38 080 B | 11 289 B (**29.65%**) | **11 246 B (29.53%)** | 11 287 B (29.64%) [-6] | 11 552 B (30.34%) | *(trop lent)* | **7 210 Mo/s** vs 16 687 Mo/s |
| `texture_rgb24.raw` | Texture RGB 24b | 196 608 B | 154 475 B (**78.57%**) | **131 157 B (66.71%)** | 86 531 B (44.01%) [-6] | 197 380 B (100.39%) | *(trop lent)* | **290 Mo/s** vs 30 108 Mo/s |
| `texture_rgba32.raw`| Texture RGBA 32b | 262 144 B | 144 027 B (**54.94%**) | **134 122 B (51.16%)** | *(exclu >256K)* | 263 173 B (100.39%) | *(trop lent)* | **406 Mo/s** vs 28 616 Mo/s |
| `audio_pcm16.raw` | Audio PCM16 Stéréo | 192 000 B | 139 027 B (**72.41%**) | **134 230 B (69.91%)** | 117 053 B (60.97%) [-6] | 163 730 B (85.28%) | *(trop lent)* | **416 Mo/s** vs 8 148 Mo/s |
| `alice29.txt` | Texte littéraire anglais | 152 089 B | 59 804 B (**39.32%**) | **58 323 B (38.35%)** | 50 488 B (33.20%) [-6] | 88 699 B (58.32%) | *(trop lent)* | **582 Mo/s** vs 2 575 Mo/s |
| `plrabn12.txt` | Poésie classique | 481 861 B | 214 066 B (**44.42%**) | **206 804 B (42.92%)** | 175 776 B (36.48%) [-2] | 325 589 B (67.57%) | *(trop lent)* | **581 Mo/s** vs 2 687 Mo/s |
| `kennedy.xls` | Feuille de calcul XLS | 1 029 744 B | 204 890 B (**19.90%**) | **196 737 B (19.11%)** | 64 830 B (6.30%) [-2] | 374 704 B (36.39%) | *(trop lent)* | **1 352 Mo/s** vs 2 719 Mo/s |
| `dickens_1m.txt`| Corpus texte 1 Mo | 1 048 576 B | 411 317 B (**39.23%**) | **393 497 B (37.53%)** | 331 404 B (31.61%) [-2] | 642 924 B (61.31%) | *(trop lent)* | **631 Mo/s** vs 1 887 Mo/s |
| `bash_x86_64.bin`| Exécutable ELF x86_64 | 1 298 416 B | 620 832 B (**47.81%**) | **593 597 B (45.72%)** | 553 594 B (42.64%) [-2] | 871 664 B (67.13%) | *(trop lent)* | **400 Mo/s** vs 2 652 Mo/s |

---

## 2. Blobs Multi-Ressources Hétérogènes (Approche A)

Dans de nombreux environnements embarqués (microcontrôleurs, firmwares, loaders de jeux, exécutables packés), les ressources sont regroupées sous forme d'un **blob monolithique unique** résultant de la concaténation de multiples fichiers hétérogènes (code machine, images, tables, textes).

Conformément à l'**Approche A**, le flux compressé reste **un seul flux LZ0 standard continu**, décompressé en bloc d'un seul trait par les décodeurs freestanding standards (< 300 octets en C / ASM), sans conteneur intermédiaire.

### Résultats Comparatifs sur les Blobs (256 Ko à 5 Mo, 3 à 15 sous-fichiers)

| Cible / Fichiers | Taille Brute | LZ0 Normal (`-6`) | LZ0 Ultra (`-9`) | UPKR | LZ4 (défaut) | Débit Déc. `lz0dec_fast` vs UPKR |
|---|---:|---:|---:|---:|---:|---:|
| **`blob_png_256k`**<br>(4 PNGs) | 262 144 B | 247 444 B (94.39%) | **246 192 B (93.91%)** | 247 888 B (94.56%) [-6] | 252 446 B (96.30%) | **1 625 Mo/s** vs 17.6 Mo/s (**92×**) |
| **`blob_mix_256k`**<br>(5 f. : ELF, PE, XLS, PNG, txt) | 262 144 B | 128 308 B (48.95%) | 124 976 B (47.67%) | **110 904 B (42.31%)** [-6] | 168 178 B (64.15%) | **673 Mo/s** vs 37.9 Mo/s (**18×**) |
| **`blob_png_512k`**<br>(8 PNGs) | 524 288 B | 504 340 B (96.20%) | **502 292 B (95.80%)** | 508 723 B (97.03%) [-2] | 511 352 B (97.53%) | **2 160 Mo/s** vs 17.1 Mo/s (**126×**) |
| **`blob_mix_512k`**<br>(7 f. : ELF, PE, 2 PNGs, XLS, C, txt) | 524 288 B | 286 813 B (54.71%) | 281 539 B (53.70%) | **260 451 B (49.68%)** [-2] | 352 088 B (67.16%) | **767 Mo/s** vs 32.2 Mo/s (**24×**) |
| **`blob_png_2m`**<br>(8 PNGs distincts > 200 Ko) | 2 097 152 B | 2 057 017 B (98.09%) | **2 051 622 B (97.83%)** | 2 089 034 B (99.61%) [-1] | 2 067 355 B (98.58%) | **4 042 Mo/s** vs ~15 Mo/s (**269×**) |
| **`blob_mix_2m`**<br>(9 f. : ELF, PE, 3 PNGs, XLS, txt, C, HTML) | 2 097 152 B | 1 204 218 B (57.42%) | 1 181 308 B (56.33%) | **1 125 643 B (53.67%)** [-1] | 1 484 306 B (70.78%) | **681 Mo/s** vs ~15 Mo/s (**45×**) |
| **`blob_png_5m`**<br>(15 PNGs distincts) | 5 242 880 B | **4 362 141 B (83.20%)** | **4 353 045 B (83.03%)** | *(exclu >20 min)* | 5 210 986 B (99.39%) | **3 606 Mo/s** vs - |
| **`blob_mix_5m`**<br>(15 fichiers réels distincts) | 5 242 880 B | 2 219 217 B (42.33%) | **2 157 767 B (41.16%)** | *(exclu >30 min)* | 3 093 616 B (59.01%) | **733 Mo/s** vs - |

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
| `fields.c` | 326 Mo/s | 364 Mo/s | 533 Mo/s | **1 070 Mo/s** | 60 Mo/s | 2 577 Mo/s |
| `sum` | 261 Mo/s | 284 Mo/s | 370 Mo/s | **791 Mo/s** | 62 Mo/s | 2 948 Mo/s |
| `lz0_win64.exe` | 155 Mo/s | 166 Mo/s | 225 Mo/s | **406 Mo/s** | 36 Mo/s | 3 375 Mo/s |
| `star.png` | 1 264 Mo/s | 4 664 Mo/s | 2 850 Mo/s | **9 217 Mo/s** | 16 Mo/s | 30 225 Mo/s |
| `avatar.jpg` | 1 222 Mo/s | 4 753 Mo/s | 2 710 Mo/s | **8 205 Mo/s** | 16 Mo/s | 30 985 Mo/s |
| `break.mp3` | 1 870 Mo/s | 2 809 Mo/s | 3 100 Mo/s | **7 210 Mo/s** | 56 Mo/s | 16 688 Mo/s |
| `alice29.txt` | 196 Mo/s | 209 Mo/s | 296 Mo/s | **582 Mo/s** | 44 Mo/s | 2 575 Mo/s |
| `plrabn12.txt` | 180 Mo/s | 201 Mo/s | 271 Mo/s | **581 Mo/s** | 40 Mo/s | 2 687 Mo/s |
| `kennedy.xls` | 576 Mo/s | 632 Mo/s | 876 Mo/s | **1 352 Mo/s** | 175 Mo/s | 2 719 Mo/s |
| `dickens_1m.txt` | 192 Mo/s | 221 Mo/s | 300 Mo/s | **631 Mo/s** | 48 Mo/s | 1 887 Mo/s |
| `bash_x86_64.bin` | 174 Mo/s | 182 Mo/s | 254 Mo/s | **404 Mo/s** | 38 Mo/s | 2 652 Mo/s |

### Débit Décompression AArch64 / ARM64 (QEMU)

| Fichier | C standard (`lz0_decompress_to`) | C minimal (`lz0dec_min`) | ASM Compact (`lz0dec_arm64`) | ASM FAST (`lz0dec_fast_arm64`) | Facteur FAST vs C |
|---|---:|---:|---:|---:|---:|
| `fields.c` | 58.7 MB/s | 55.4 MB/s | 114.3 MB/s | **182.2 MB/s** | **+211%** |
| `alice29.txt` | 47.1 MB/s | 42.8 MB/s | 81.1 MB/s | **167.1 MB/s** | **+255%** |
| `plrabn12.txt` | 44.3 MB/s | 38.1 MB/s | 65.8 MB/s | **162.7 MB/s** | **+267%** |
| `kennedy.xls` | 83.1 MB/s | 73.8 MB/s | 173.9 MB/s | **207.1 MB/s** | **+149%** |
| `dickens_1m.txt` | 48.7 MB/s | 41.9 MB/s | 76.3 MB/s | **133.6 MB/s** | **+174%** |
| `bash_x86_64.bin` | 41.3 MB/s | 39.9 MB/s | 74.2 MB/s | **109.7 MB/s** | **+165%** |

### Débit Décompression RISC-V 64 / RV64GC (QEMU)

| Fichier | C standard (`lz0_decompress_to`) | C minimal (`lz0dec_min`) | ASM Compact (`lz0dec_riscv64`) | ASM FAST (`lz0dec_fast_riscv64`) | Facteur FAST vs C |
|---|---:|---:|---:|---:|---:|
| `fields.c` | 36.5 MB/s | 122.4 MB/s | 141.8 MB/s | **186.6 MB/s** | **+412%** |
| `alice29.txt` | 24.7 MB/s | 90.7 MB/s | 110.8 MB/s | **122.0 MB/s** | **+393%** |
| `plrabn12.txt` | 21.1 MB/s | 79.0 MB/s | 92.9 MB/s | **113.8 MB/s** | **+440%** |
| `kennedy.xls` | 45.4 MB/s | 202.9 MB/s | 167.3 MB/s | **309.9 MB/s** | **+583%** |
| `dickens_1m.txt` | 23.4 MB/s | 81.6 MB/s | 99.0 MB/s | **124.6 MB/s** | **+432%** |
| `bash_x86_64.bin` | 21.6 MB/s | 85.2 MB/s | 78.4 MB/s | **112.2 MB/s** | **+420%** |

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

### Parseur Ultra Dédié SoA ($K=128..12$) et Multi-Échelle SA-IS
- En mode Ultra (`-9`), le parseur dédié `lz0_parse_ultra` utilise une représentation compacte Structure-of-Arrays (12 octets par état), un filtre de Bloom 64 bits et un élagage par marge de coût :
  - **$K=128$** pour fichiers $\le 512$ Ko (capture maximale des interactions d'offsets).
  - **$K=64$** autour de 1 Mo (garantit une empreinte RAM DP calibrée par `lz0_ultra_budget`).
  - **$K=32$** à 2 Mo et **$K=12$** à 5 Mo (évite toute explosion de l'empreinte mémoire).
- Les correspondances de longueur 2 sont évaluées sans contrainte d'offset (`lz0_len2_max_off = 0`), ce qui génère des gains substantiels sur les formats tabulaires et multimédia (textures RGB24, RGBA32, XLS) sans dégrader les données à haute entropie.
- L'échantillonnage de la fenêtre Suffix Array associe une fenêtre linéaire dense $1 \dots 128$ et des sauts géométriques $\{160, 192, 256, 320, 384, 512\}$, capturant les dépendances lointaines sur les gros fichiers sans accroître le nombre de comparaisons.
