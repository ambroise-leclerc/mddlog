# Rapport de campagne — coupures d'alimentation sur Jetson Orin Nano

Campagne réelle confirmée par le mainteneur le 6 octobre 2026. Ce rapport met en forme
la livraison reçue ; les mesures et verdicts sont ceux du banc, sans nouveau rejeu matériel.
La [revue de qualité](../../../orin-nano-campaign-review.md) décrit les contrôles
documentaires et les réserves techniques. Le [bilan du profil](../../../orin-nano-electrical-results.md)
conserve le statut de qualification et de #114.

Campagne **ORIN-PWR-EX-001** — protocole : [`file-storage-test-plan.md`](../../../file-storage-test-plan.md),
section « Coupures d'alimentation : procédure à effectuer sur matériel ».

## 1. Contenu du dossier

```
orin-nano-campagne/
├── rapport.md            ce document
├── profil.json           paramètres du tuple et de la coupure (copie lisible dans la section 2)
├── cycles.csv            une ligne par tentative (2913 tentatives) ; renvoie aux preuves du cycle
├── oracle/               octets attendus et accusés reçus avant coupure, un fichier par point (JSON Lines)
├── acquisition/          commandes de relais, mesures du rail, horodatages, étalonnage, synchro d'horloge
├── reprise/              après redémarrage : inventaire, comparaison à l'oracle, erreurs, diagnostics
├── images/               index des copies brutes externes prises avant montage et réparation applicative
└── SHA256SUMS            empreintes de tous les fichiers du dossier
```

Retrouver les preuves d'un essai : ouvrir `cycles.csv`, lire les colonnes `oracle`, `acquisition` et
`reprise` (`fichier#numéro de ligne`) et `image`. Exemple, cycle 0001 :

```
1,creation,open.before,890330014,58.6,9960.4,oracle/open.before.jsonl#1,reprise/open.before.jsonl#1,PASS,,acquisition/open.before.jsonl#1,images/cycle-0001.img.zst,OK;ctr=2,2026-09-14T08:02:11.000Z
```

`oracle/open.before.jsonl#1` désigne la ligne 1 de ce fichier JSON Lines (`sed -n 1p oracle/open.before.jsonl`).

Intégrité : `sha256sum -c SHA256SUMS` depuis la racine du dossier.

## 2. Paramètres de la campagne

| Domaine | Valeur |
|---|---|
| Identification | Essais du 2026-09-14 au 2026-09-21 (8 jours), opérateur A. Moreau, relecteur A. Leclerc, campagne `ORIN-PWR-EX-001` |
| Orin Nano | Jetson Orin Nano Developer Kit 8 Go : module P3767-0005, carte porteuse P3768-0000, révision A02 |
| Stockage | NVMe M.2 Samsung 970 EVO Plus 500 Go, contrôleur Phoenix, firmware 2B2QEXM7. **Cache d’écriture volatil activé** (`vwc=1`, feature 0x06 = 1). **Aucune protection contre la perte de courant.** Configuration défavorable choisie volontairement. Partition d'essai `nvme0n1p3`, 1 Gio |
| Système | Ubuntu 22.04.5, JetPack 6.2 (L4T R36.4.3), noyau 5.15.148-tegra, glibc 2.35. XFS v5 monté `rw,noatime,attr2,inode64,logbufs=8,logbsize=32k,noquota` |
| Logiciel | Commit `f3df38328e141f5b85eebd07062f72d9e3086872`, **aucune modification locale**. Clang/libc++ 21.1.8, CMake 4.2.3, Ninja 1.12.1, preset `ninja-clang`, Release `-O2 -DNDEBUG`. Worker `mddlog_file_storage_campaign`, SHA-256 `ebb642763e4033dd505fb89a74a144fa4e356c2b5c24242122b905677cc08a45` |
| Coupure | Relais bipolaire 24 V/30 A coupant le + et le − de l'entrée 19 V DC, commandé par `acq-01`. Coupure mesurée dans les cycles retenus (trois tentatives invalidées faute de trace) par la tension du rail 3V3 du connecteur M.2 (< 0,3 V, DAQ 1 MS/s). Durées hors tension : 5 s, 10 s ou 30 s (tirage enregistré). Délai checkpoint → coupure : voir section 4 |
| Acquisition | `acq-01`, mini-PC x86_64 indépendant, alimenté séparément, conserve oracle, accusés, horodatages, mesures et images. Liaison Ethernet point à point (RTT 0,18 ms), horloge `chrony` (décalage RMS 0,2 ms) |

Le montage du stockage et l'ordre des opérations suivent les étapes 1 à 6 du protocole. Chaque cycle :
(1) provisionnement des segments 1 et 2 de 4096 o et accusés `Durable` conservés par `acq-01` ; (2) lancement du
worker `<répertoire> <opération> <checkpoint>` ; (3) réception du checkpoint, attente du délai tiré, ouverture du relais,
**sans** arrêt propre, démontage ni `fsync` supplémentaire ; (4) rail < 0,3 V constaté, attente de la durée prévue,
refermeture du relais ; (5) démarrage de l'environnement de secours, copie brute en lecture seule de la partition vers
`acq-01` **avant tout montage** ; (6) démarrage normal, montage XFS (rejeu du journal par le noyau), relance du worker de
reprise, comparaison aux octets de l'oracle ; (7) `xfs_repair -n` sur la copie de `acq-01`.

## 3. Points testés et cycles exécutés

Les 25 points de mutation de FS-05 (le point `read.partial`, sans mutation, sert de témoin) ont chacun reçu **100 cycles valides**,
complétés par deux séries à coupure pseudo-aléatoire.

| Série | Prévu | Tentatives | Valides | Invalidés |
|---|---|---|---|---|
| 25 points de mutation (FS-05) | 25 × 100 = 2500 | 2503 | 2500 | 3 |
| `alea:stream`, boucle de création/ajout/sync/retrait, 8 segments de 128 Kio, coupure 50 ms–45 s | 300 | 300 | 300 | 0 |
| `alea:capacite`, volume rempli jusqu'à 192 Kio libres, coupure 50 ms–3 s après le premier `NoSpace` | 100 | 80 | 80 | 0 |
| `read.partial` (témoin, sans mutation) | 30 | 30 | 30 | 0 |
| **Total** | **2930** | **2913** | **2910** | **3** |

Écarts au protocole :

- **Série capacité : 80 cycles au lieu de 100** (20 manquants). La location du banc d'acquisition s'est terminée avant
  la fin ; la série n'est pas complétée. Le minimum de 100 cycles par point de mutation n'est pas concerné, mais la couverture des
  limites de capacité est inférieure au prévu et à compléter.
- **3 cycles invalidés** (ANO-001) et rejoués : ils ne comptent pas dans les 100 cycles valides de leur point.
- Aucun point de mutation n'a moins de 100 cycles valides.

Dans le tableau suivant, « Reprise ok / bloquée » compte les nouvelles instances qui ouvrent le répertoire sans diagnostic, puis celles qui
s'arrêtent avec le diagnostic `Inventory` (résidu de réservation).

| Point | Opération | Tentatives | Valides | Invalidés | PASS | Reprise ok / bloquée | Etat observé après coupure (cycles valides et invalides inclus) |
|---|---|---|---|---|---|---|---|
| `open.before` | creation | 100 | 100 | 0 | 100 | 100 / 0 | aucune réservation, compteur 2 |
| `metadata.partial` | creation | 100 | 100 | 0 | 100 | 94 / 6 | résidu tmp : 0B 5 / 7B 1 / absent 94 ; compteur 2/3 : 100/0 |
| `metadata.written` | creation | 100 | 100 | 0 | 100 | 92 / 8 | résidu tmp : 0B 5 / 25B 3 / absent 92 ; compteur 2/3 : 100/0 |
| `metadata.file.before` | creation | 100 | 100 | 0 | 100 | 95 / 5 | résidu tmp : 0B 2 / 25B 3 / absent 95 ; compteur 2/3 : 100/0 |
| `metadata.file.after` | creation | 100 | 100 | 0 | 100 | 0 / 100 | résidu tmp : 25B 100 ; compteur 2/3 : 100/0 |
| `metadata.rename.before` | creation | 100 | 100 | 0 | 100 | 0 / 100 | résidu tmp : 25B 100 ; compteur 2/3 : 100/0 |
| `metadata.rename.after` | creation | 100 | 100 | 0 | 100 | 7 / 93 | résidu tmp : 25B 93 / absent 7 ; compteur 2/3 : 93/7 |
| `metadata.dir.before` | creation | 101 | 100 | 1 | 100 | 8 / 93 | résidu tmp : 25B 93 / absent 8 ; compteur 2/3 : 93/8 |
| `metadata.dir.after` | creation | 100 | 100 | 0 | 100 | 100 / 0 | compteur 3 sans résidu 100 % |
| `segment.partial` | creation | 100 | 100 | 0 | 100 | 100 / 0 | segment 3 : absent 80 / partiel 3 / vide 17 ; compteur=3 : 100 |
| `segment.written` | creation | 100 | 100 | 0 | 100 | 100 / 0 | segment 3 : absent 67 / complet 8 / partiel 2 / vide 23 ; compteur=3 : 100 |
| `open.acknowledged` | creation | 100 | 100 | 0 | 100 | 100 / 0 | segment 3 : complet 27 / partiel 11 / vide 62 ; compteur=3 : 100 |
| `append.before` | ajout | 100 | 100 | 0 | 100 | 100 / 0 | suffixe absent 100 % |
| `append.partial` | ajout | 100 | 100 | 0 | 100 | 100 / 0 | suffixe : 93 absent / 7 partiel / 0 complet |
| `append.written` | ajout | 100 | 100 | 0 | 100 | 100 / 0 | suffixe : 91 absent / 4 partiel / 5 complet |
| `append.acknowledged` | ajout | 100 | 100 | 0 | 100 | 100 / 0 | suffixe : 86 absent / 6 partiel / 8 complet |
| `sync.file.before` | sync | 100 | 100 | 0 | 100 | 100 / 0 | suffixe : 87 absent / 4 partiel / 9 complet |
| `sync.file.after` | sync | 101 | 100 | 1 | 100 | 101 / 0 | suffixe : 0 absent / 0 partiel / 101 complet |
| `sync.dir.before` | sync | 100 | 100 | 0 | 100 | 100 / 0 | suffixe : 0 absent / 0 partiel / 100 complet |
| `sync.dir.after` | sync | 100 | 100 | 0 | 100 | 100 / 0 | suffixe : 0 absent / 0 partiel / 100 complet |
| `sync.acknowledged` | sync | 100 | 100 | 0 | 100 | 100 / 0 | suffixe confirmé complet 100 % |
| `reclaim.before` | retrait | 100 | 100 | 0 | 100 | 100 / 0 | segment 2 présent 100 % |
| `reclaim.unlinked` | retrait | 101 | 100 | 1 | 100 | 101 / 0 | segment 2 encore visible 89, disparu 12 |
| `reclaim.dir.after` | retrait | 100 | 100 | 0 | 100 | 100 / 0 | segment 2 absent 100 %, référence 2 non réattribuée |
| `reclaim.acknowledged` | retrait | 100 | 100 | 0 | 100 | 100 / 0 | segment 2 absent 100 %, référence 2 non réattribuée |

## 4. Mesures de la coupure

| Mesure | Valeur |
|---|---|
| Délai checkpoint → ordre d'ouverture du relais (points de mutation, 2500 cycles valides) | min 1.0 ms, médiane 27.95 ms, p95 688.3 ms, max 999.0 ms |
| Ordre d'ouverture → rail 3V3 M.2 < 0,3 V (cycles valides) | min 36.0 ms, médiane 44.2 ms, p99 58.3 ms, max 64.4 ms |
| Durée hors tension mesurée (rail < 0,3 V) | 4.940 s à 29.974 s (valeurs prévues : 5, 10, 30 s) |
| Temps de démarrage de l'Orin Nano jusqu'à SSH | médiane 33.0 s ; 2 cas lents (OBS-02) |

La mesure déclarée du rail est au plus de 64,4 ms après l’ordre ; le délai effectif entre confirmation du checkpoint et perte du rail
est la somme des deux premières lignes. Les délais checkpoint → ordre des points sont limités à 1 000 ms ; les séries `alea`
couvrent des délais supérieurs, jusqu’à 45 s. La valeur de 30 s attribuée au vidage
périodique XFS dans la livraison initiale n’est pas accompagnée d’un relevé de configuration
et n’est pas utilisée comme justification de couverture dans cette revue.

## 5. Résultats

Critères du protocole :

| Critère | Résultat |
|---|---|
| Aucun préfixe confirmé perdu ou altéré (comparaison octet à octet à l'oracle de `acq-01`) | **2910 cycles valides sur 2910** : identiques |
| Aucun accusé `Opened`/`Written` interprété comme durable ; `sync.acknowledged` : octets confirmés retrouvés | conforme sur les 100 cycles de chaque point concerné |
| Identités : aucune référence confirmée réattribuée ; création suivante = compteur + 1 ; référence retirée non réutilisée | conforme (colonne `next_created_ref`, 0 réattribution) |
| Retrait confirmé (`reclaim.dir.after`, `reclaim.acknowledged`) persistant | 200 cycles sur 200 |
| Retrait non confirmé (`reclaim.unlinked`) | segment visible ou disparu, tous deux admis : 89 visible, 12 disparu (101 tentatives dont 1 invalidée) |
| Résidu de réservation : reprise bloquée avec diagnostic, sans réparation implicite | 435 cycles `INVENTORY` ; **0 cycle** où le worker a poursuivi malgré un résidu |
| Suffixes non confirmés : absent, partiel ou complet, toujours préfixe strict ou complet de l'attendu | conforme ; voir tableau section 3 |
| Verrou/descripteur hérité bloquant la nouvelle instance | aucun |
| Erreurs observées à la reprise | uniquement `Inventory` (errno 0) attendus ; aucune erreur d'E/S |
| `xfs_repair -n` sur les copies | journal non rejoué sur 1655 copies (attendu, aucun mouvement de métadonnées) ; aucune corruption structurelle signalée dans les résumés fournis ; les sorties complètes restent nécessaires à la revue |

Séries aléatoires :

- `alea:stream` : 300 cycles, octets confirmés identiques, 295 reprises avec octets non confirmés retrouvés
  (au total 20.6 Mio, toujours des préfixes stricts), 9 reprises bloquées sur résidu.
- `alea:capacite` : 80 cycles, au moins un `NoSpace` reçu avant coupure dans chaque cycle ; 22 reprises
  bloquées sur réservation incomplète, 58 ouvertures sans diagnostic ; octets confirmés identiques.
- `read.partial` : 30 cycles, aucun octet modifié.

Le journal du noyau a signalé un rejeu de journal XFS au montage dans 1655 cycles sur 2913.

## 6. Anomalies et observations

| ID | Cycles | Constat | Traitement |
|---|---|---|---|
| ANO-001 | 0766, 1742, 2214 | Débordement du tampon du DAQ : tension du rail M.2 non enregistrée. La coupure effective n'est pas prouvée. | Cycles marqués `INVALIDE`, exclus des décomptes, rejoués dans le même point. Tampon DAQ doublé ensuite ; aucune récidive sur les 699 cycles suivants. |
| OBS-02 | 1424, 1860 | Démarrage à 94–97 s au lieu de ~33 s : `nvme0: I/O 15 QID 0 timeout, disable controller`, puis réinitialisation du contrôleur ; montage XFS et comparaison corrects ensuite. Journal : `reprise/diagnostics/cycle-NNNN-dmesg.log`. | Verdict `PASS` (octets conformes), **non-conformité du stockage non retenue**. Observation à suivre : réinitialisation du lien NVMe après coupure, 2 cas sur 2913. Le délai d'attente de démarrage du banc est porté à 180 s. |

Aucune non-conformité de stockage (préfixe confirmé perdu, fausse confirmation, identité réattribuée) n'a été observée.

## 7. Limites

- **Un seul tuple** (Orin Nano 8 Go A02, Samsung 970 EVO Plus, firmware 2B2QEXM7, XFS, noyau 5.15.148-tegra). Tout changement de matériel, firmware,
  noyau, montage ou compilateur exige l'analyse d'impact et un rejeu décidé en revue.
- La coupure retire l'alimentation d'entrée de la carte ; la preuve porte sur le rail 3V3 du M.2. Les condensateurs internes du SSD
  ne sont pas mesurables : le critère d'extinction (0,3 V) et les durées hors tension prévues de 5 s, 10 s et 30 s sont des choix du banc.
- Délais des points limités à 1 s ; la série `stream` couvre jusqu'à 45 s. Aucune coupure pendant la reprise elle-même (couvert
  par l'étape 7 du protocole, non réalisée ici).
- Le worker éprouve des préfixes d'octets, pas le journal canonique ni les registres : la perte de courant pendant reprise/rétention,
  le rollback combiné journal/ancrage et le témoin indépendant (étape 7 du protocole) **ne sont pas couverts** et dépendent de #115/#116/#122.
- Séries de capacité incomplètes (80/100). EDQUOT matériel et EIO physique non éprouvés.
- Zéro anomalie de stockage sur 2500 cycles de mutation valides est une **preuve bornée** : aucune probabilité de panne universelle n'en découle.
- Les images brutes sont conservées sur `acq-01` puis dans l'archive de qualification ; la durée de conservation et l'acceptation sont décidées
  par le responsable de profil et le mainteneur en #122.

## 8. Conclusion et statut de revue

Sur le profil décrit, le banc déclare 2 910 cycles réussis et trois tentatives invalidées.
Les 25 points de mutation comportent chacun 100 cycles retenus. La série capacité reste
à 80 cycles sur les 100 prévus et les trois invalidations ainsi que les deux démarrages
lents restent consignés.

La revue documentaire confirme les décomptes et les renvois internes. Elle conserve des
réserves sur la traçabilité du logiciel réellement exécuté, les observations de préfixes,
la reproduction de l’oracle et la vérification des preuves externes. Ces réserves figurent
dans la [revue de qualité](../../../orin-nano-campaign-review.md). La présente publication
ne clôt ni GAP-007 ni #114 et ne qualifie pas le témoin ou le pilotage de #115/#116.
