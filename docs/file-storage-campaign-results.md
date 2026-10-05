# Résultats du banc logiciel de stockage — #114 / #120

Campagne du 2026-10-05, diff de `114-storage-campaign` fondé sur `3da48c0`.
Le [protocole](file-storage-test-plan.md) définit les critères avant campagne.
État : résultats locaux soumis à revue, sans qualification matérielle ni clôture de GAP-007.

Le [bilan succinct](file-storage-success-report.md) consigne la réussite du lot fusionné
par la PR #135 et les preuves CI, séparément de cette campagne locale.

## Environnement et commandes

Linux 7.0.0-34-generic, glibc 2.43, Clang/libc++ 21.1.8, GCC/libstdc++ 16.1.0,
CMake 4.2.3, Ninja, Python 3.14.6. Builds Release des presets `ninja-clang` et `ninja-gcc`,
tests/exemples/backend activés. Interruptions sur XFS du workspace, options observées
`rw,noatime,inode64,logbufs=8,logbsize=32k,sunit=1024,swidth=2048,noquota`.
Les trois essais ENOSPC montent un tmpfs privé de 1 Mio/256 inodes dans un namespace
utilisateur/montage isolé ; les refus EACCES sont exercés avec l’UID 1000 sans privilèges.

```bash
cmake --preset ninja-clang
cmake --build --preset ninja-clang --parallel 4
TMPDIR="$PWD/build-clang/file-storage-tests" ctest --test-dir build-clang --output-on-failure --parallel 4 --timeout 30
cmake --preset ninja-gcc
cmake --build --preset ninja-gcc --parallel 4
TMPDIR="$PWD/build-clang/file-storage-tests" ctest --test-dir build-gcc --output-on-failure --parallel 4
python3 scripts/run-file-storage-campaign.py \
  --worker build-clang/tests/mddlog_file_storage_campaign \
  --output build-clang/campaign-expanded --iterations 10 --require-volume
clang-tidy-21 -p build-clang --warnings-as-errors='*' tests/spec/FileStorageCampaign.cpp
clang-tidy-21 -p build-clang --warnings-as-errors='*' tests/spec/FileStorageSpec.cpp
scripts/check-format.sh
python3 scripts/check-development-file.py
python3 -m unittest discover -s tests/documentation -p 'Test*.py'
```

## Résultats

| Vérification | Résultat |
| --- | --- |
| Configuration, compilation, liens Clang et GCC | Réussis, worker séparé inclus |
| CTest Clang / GCC | 183/183 et 182/182 réussis ; consommateurs source/installé inclus |
| `file-storage.campaign` sur chaque compilateur | 36/36 cas PASS, aucun FAIL/SKIP ; les trois sous-cas ENOSPC sont regroupés sous `real-enospc` |
| Campagne répétée Clang | 270/270 cas PASS, aucun FAIL/SKIP : 26 points × 10, quatre refus de droits, quatre compteurs corrompus, ressources et volume |
| ENOSPC réel | Ajout refusé, réservation incomplète refusée au redémarrage, ouverture partielle conservée sans référence retournée |
| Refus EACCES réels | Ouverture, ajout, lecture et retrait refusés ; préfixes inchangés et erreur native conservée |
| Ressources | 64 créations/barrières/retraits et 64 factories refusées sans croissance des descripteurs du worker ; références finales à 66 |
| Superviseur | Cinq descripteurs avant/après chaque invocation locale ; enfants tués et récoltés |
| Contrôles négatifs Python et dossier | 38 tests réussis ; structure et références conformes |
| Format et analyse statique | 85 fichiers au format LLVM 21 ; les deux unités C++ modifiées conformes sous clang-tidy 21 |

La comparaison à l’oracle inclut préfixes confirmés, suffixes partiels, fichiers nouveaux,
retraits observés, compteur consommé et réouverture dans un autre processus. Les checkpoints
de réservation interrompue attendent un rejet explicite ; le banc ne les répare pas.
Le scénario existant d’écriture partielle inclut maintenant EDQUOT injecté, distinct du
volume tmpfs réellement plein.

## Anomalie observée lors du premier passage

La première suite Clang a terminé 182 cas, mais le test existant
`A throwing sink does not crash the asynchronous worker thread` est resté bloqué
plus de sept minutes. Il a été interrompu par SIGTERM ; ce passage n’est pas annoncé
comme réussi. Le rejeu complet avec `--timeout 30` passe 183/183 en environ 32 secondes.
Le journal initial est conservé à côté du rejeu. Le test et `SimpleLogger` ne sont pas
modifiés par ce lot ; ce blocage intermittent doit être examiné avec #118/#120.

## Artefacts et portée

Rapports générés : `build-clang/tests/file-storage-campaign/run-*/report.json`,
`build-gcc/tests/file-storage-campaign/run-*/report.json` et
`build-clang/campaign-expanded/run-*/report.json`. Ils enregistrent SHA-256 du worker,
révision/diff, environnement/build/montage, événements, erreurs et durée de chaque cas.
Les données des cas d’interruption et de corruption restent à côté du rapport.
Journaux de build, CTest et analyse dans `build-clang/verification-storage-campaign/`.
Ces artefacts locaux sont ignorés par Git ; aucune conservation pérenne implicite.

La CI ajoutée répète les points dix fois, exige un volume plein réel sur l’hôte et archive
les rapports. Son exécution et ses résultats sont à citer avec le run et le SHA de la PR,
séparément des résultats locaux ci-dessus. Aucun résultat de workflow non exécuté n’est revendiqué.

Non réalisés : campagne de 100 répétitions par point, coupure électrique, qualification
du matériel/contrôleur/caches, quota réel, EIO matériel et chaîne complète avec témoin et
position retenue indépendants. Les redémarrages sont ceux des processus ; noyau et caches
restent actifs. `QualifiedFsync` ne sert qu’à tester les branches ; tmpfs ne fournit aucune
preuve de survie au redémarrage de la machine. Les obligations et la procédure physique
figurent dans le protocole ; #114/#115/#116/#120/#122 restent ouverts.
