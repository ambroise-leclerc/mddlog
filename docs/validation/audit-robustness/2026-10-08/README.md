# Preuves conservées de #120 — 8 octobre 2026

Ce répertoire conserve dans Git les preuves sélectionnées de la campagne logicielle
livrée par [PR #146](https://github.com/ambroise-leclerc/mddlog/pull/146).
L'issue et les liens CI servent au suivi ; leur disponibilité n'est pas nécessaire
pour vérifier les octets conservés. Ce lot de conservation et la disposition de
clôture logicielle de #120 sont soumis à revue.

## Provenance

- Révision exécutée : `6bf13770bd7732ab2640fc6fe28a06ccf5d38d32`.
- Fusion sur `develop` : `b73ab46acb7e6d008535b3871c1980e1f7b94fc3` ; même arbre source.
- Revue AM-L approuvée le 2026-10-08 à 19:08:07 UTC ; fusion à 19:08:25 UTC.
- Workflow manuel `campaign=long` : run `37810541676`, tentative 1, réussi.
- Artefact `11566830079` : copie originale ZIP, sans recompression ni modification.
- SHA-256 du ZIP : `9a98a554725bd4d9d8b6d5cc31fa23a86402fd2855fbbe7531ad33fa8ced2855`.
- Taille : 7 482 376 octets ; 8 109 fichiers, 148 178 779 octets décompressés.

`manifest.json` conserve dates, identifiants, commandes/profils via les rapports,
empreintes et liens d'origine. L'expiration GitHub de l'artefact, le 6 janvier 2027,
n'affecte pas cette copie Git. Les versions effectives, options sanitizers, sources
et binaires sont identifiées dans les rapports originaux ; les binaires compilés ne
sont pas embarqués. Les journaux sont synthétiques, sans données applicatives réelles.

## Contenu et résultats

| Fichier | Preuve et portée |
| --- | --- |
| `audit-robustness.zip` | Artefact original complet : traces, journaux fichiers/témoin/checkpoint, corpus final, résultats SHA-256, logs et couverture LLVM, dix campagnes courtes et campagnes prolongées |
| `files.sha256` | Inventaire trié et SHA-256 de chacun des 8 109 fichiers du ZIP |
| `chain-report.json` | Copie exacte du rapport : 32 seeds × 24 flux × 64 événements, 49 152 événements de référence, 14 848 relus, 768 fautes terminales, 32 déploiements isolés requis et deux archives CLI ; PASS en 264,30 s |
| `readers-report.json` | Copie exacte : 1 000 000 entrées de fuzzing, oracle SHA-256 sur 268 entrées, couverture des six lecteurs ciblés et autres unités instrumentées ; PASS, LSan actif |
| `storage-report.json` | Copie exacte : 100 répétitions, 2 609 PASS, zéro FAIL, un SKIP pour ENOSPC sans privilèges ; statut original PARTIAL conservé |
| `volume-report.json` | Copie exacte : contrôle ENOSPC dédié sous sudo, `--require-volume`, PASS sans SKIP |
| `long-workflow.log.gz` | Logs des jobs du workflow prolongé, avec préparation, compilation, étapes et archivage |
| `sanitizers.log.gz` | Logs du run `37810539016`, ASan/UBSan GCC et TSan GCC : commandes, versions et scénarios réellement exécutés |
| `clang-build.log.gz` | Logs du run `37810539053`, build/tests Clang Release, ASan/UBSan et six lots clang-tidy |

Les logs des jobs sont réunis dans l'ordre de leur archive GitHub, précédés du nom
du job, puis compressés avec gzip et `mtime=0`. Le manifeste hache les versions
compressée et décompressée. Les quatre rapports sont des copies exactes des membres
du ZIP : aucun chemin absolu, statut ou chiffre n'a été réécrit pour les publier.

## Vérification hors ligne

Le vérificateur est propre au jeu du **2026-10-08** et à son profil. `--evidence`
permet de vérifier une copie déplacée de ce jeu, pas une nouvelle campagne. Son
ancrage de confiance comprend le digest du ZIP et les empreintes compressées et
décompressées des trois logs supplémentaires, figés dans le code hors manifeste.
Un futur jeu daté devra disposer d’un vérificateur/profil et d’un ancrage revus ;
la politique d’ajout ne promet pas leur prise en charge par ce script.

Depuis la racine du dépôt, le vérificateur utilise Python standard uniquement ;
les tests de protection du checkout utilisent aussi Git :

```bash
python3 scripts/verify-audit-evidence.py
python3 -m unittest discover -s tests/documentation -p 'TestAuditEvidence.py'
```

Le vérificateur contrôle le digest original, l'inventaire complet, les tailles, les
rapports et logs, le profil et les résultats requis. Le digest original est aussi
figé dans le vérificateur, indépendamment d’un manifeste fourni. Les trois logs
supplémentaires sont obligatoires, sans doublon, avec leurs runs et empreintes
originaux ; recalculer le manifeste ne permet pas de les remplacer. Les fichiers en
entrée et chaque log décompressé sont plafonnés à 16 Mio ; les logs sont hachés
par morceaux bornés. Les membres ZIP sont également lus par morceaux ; les
plafonds de 16 Mio par membre et 200 Mio au total portent sur les octets
effectivement lus, en plus du contrôle des tailles déclarées. Il refuse les chemins
sortant du répertoire, les membres dupliqués et les archives hors budget. Il ne déploie pas
le témoin et n'exécute pas de worker C++ ni sudo : il rejoue les 32 traces par
l'oracle Python de file, d'octets canoniques et de digests. Ce rejeu charge et
exécute `scripts/run-audit-chain-campaign.py` du dépôt via `exec_module` ; il suppose
donc une révision du dépôt de confiance, au-delà des seuls octets de l’archive.
Les tests négatifs couvrent corruption et provenance, ainsi que profil, isolation,
comptes du stockage, fuzz/couverture et divergence de rejeu avec des fixtures
sémantiques dont les empreintes sont recalculées. La CI documentaire les exécute.
Les erreurs de vérification sont rendues en JSON FAIL à la frontière CLI, y compris
les types mal formés et erreurs des codecs ; les interruptions du processus et
erreurs d’usage argparse gardent leur comportement habituel.

Pour reproduire les campagnes C++, reconstruire la révision fusionnée citée avec
son tuple, puis suivre [le profil prolongé](../../../audit-chain-campaign.md).
Les chemins `/home/runner/...` sont ceux de l'exécution originale, pas des chemins
à réutiliser localement. Une nouvelle campagne produit de nouvelles preuves ; elle
ne remplace pas les octets de ce répertoire.

## Conservation et réserves

Durée : conserver ces preuves avec l'historique du projet, y compris après la
clôture de #120 et l'expiration des artefacts CI. Pas d'expiration automatique,
de remplacement par une campagne ultérieure, ni de Git LFS externe nécessaire.
Ce jeu représente environ 11 Mo conservés dans l’historique, dont 7,5 Mo de ZIP
et un rapport de stockage de 81 941 lignes. Ce poids est assumé sans LFS. Les
attributs Git désactivent la conversion de fins de ligne pour `docs/validation/**`
et traitent ZIP/gzip comme binaires ; les copies exactes restent identiques avec
`core.autocrlf=true` ou `core.eol=crlf`.
Les nouveaux jeux acceptés prennent un nouveau répertoire daté ; une correction
de lecture ajoute une note de revue sans altérer les fichiers originaux. Les
répertoires de build restent hors Git ; ce ZIP est une sélection de résultats de
vérification dont la conservation a été demandée par le mainteneur.

La revue/fusion de #146 accepte le lot logiciel avec ses limites. Cette conservation
n'est pas une nouvelle campagne C++ ni l'acceptation finale du dossier. L'oracle
observe admissions, detail et compteurs via la trace du worker : une trace et des
octets mensongers mais cohérents pourraient lui échapper. La chaîne fonctionnelle
partage un UID ; les déploiements multi-UID sont des essais distincts, pas la
qualification d'un témoin de production. Les interruptions sont logicielles, sans
preuve de coupure électrique ou d'endurance physique.

Le suivi du reproducteur autonome TSan/libc++ est explicitement porté par
[#147](https://github.com/ambroise-leclerc/mddlog/issues/147), avec une copie de
sa source et de son observation dans [le dossier de suivi](../tsan-libcxx/README.md).
Il reste ouvert et doit être évalué pour la matrice #121 et la candidate #122 ;
aucun faux positif ni défaut de bibliothèque n'est déclaré établi. La politique
des futures archives relève de #121 ; le profil réel et l'acceptation finale du
dossier, de #122. La clôture de GAP-013 pour les profils logiciels livrés est acceptée par la
décision nominative AM-L : PR #148 approuvée le 2026-10-08 à 21:58:47 UTC sur `f5b015c08a0fb9bda55ee792f039309d86debc5e`, fusionnée à `2b606df47d5bf2d7db5e021a8bf359173ee5490e` ; le registre la consigne depuis #121.
GAP-003 conserve son acceptation finale sous #122.
