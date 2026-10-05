# Vérification locale du premier lot #114

État : résultats locaux, sans acceptation du profil physique. Campagne initiale :
`f34d50b` sur la branche `114-file-storage`, fondée sur `e3635d0`. Les corrections
de revue de PR #134 sont vérifiées séparément ci-dessous.

Campagne du 2026-10-05 : Linux 7.0.0-34-generic, glibc 2.43, Clang/libc++ 21.1.8, CMake 4.2.3, Ninja, preset
`ninja-clang` Release, exemples/tests/backend activés. Les fichiers de tests vivent dans
un répertoire temporaire propre à chaque scénario. Première campagne sur `/tmp` (tmpfs),
puis rejeu des six scénarios du backend et des deux consommateurs sur le volume XFS du
workspace via `TMPDIR=.../build-clang/file-storage-tests`, puis campagne complète 179/179
sur ce même volume après les dernières corrections. Options du montage observées :
`rw,noatime,inode64,logbufs=8,logbsize=32k,sunit=1024,swidth=2048,noquota`.
La pile matérielle/caches n'est pas caractérisée ; aucun matériel n'est qualifié.

Commandes reproductibles :

```bash
cmake --preset ninja-clang
cmake --build --preset ninja-clang --parallel 4
ctest --test-dir build-clang --output-on-failure
scripts/check-format.sh
JOBS=4 scripts/run-clang-tidy.sh build-clang
python3 scripts/check-development-file.py
python3 -m unittest discover -s tests/documentation -p 'Test*.py'
```

Résultats effectifs :

- Configuration, compilation et liens du preset : réussis, backend, exemple et consommateur inclus.
- CTest complet : **179/179 réussis**, dont `InstallTreeConsumer`, `InstallTreeCoreConsumer`
  et `SourceTreeFileStorageConsumer` ; le consommateur installé importe directement le backend.
- Rejeu sur XFS : **8/8 réussis** (six scénarios, consommateur source et consommateur installé).
- Exemple autonome : code retour 0 ; une admission/écriture, position durable 0,
  un enregistrement retrouvé et chaîné après réouverture en lecture seule.
- Build neuf `build-114-without-files` : configuration, compilation et liens de `mddlog`
  et `mddlog-core` réussis avec backend/tests/exemples désactivés.
- Format LLVM 21 : **84 fichiers conformes**. Dossier : structure et références conformes,
  **30 tests documentaires réussis**.
- Analyse statique LLVM 21 : **75/75 unités conformes, dont 36/36 interfaces de modules**,
  aucun échec et aucun `clang-diagnostic-error`. Le nouveau module a aussi été revérifié
  séparément après les dernières corrections.

Rejeu XFS reproductible après création d'un répertoire privé dans le build :

```bash
TMPDIR="$PWD/build-clang/file-storage-tests" ctest --test-dir build-clang \
    -R 'File storage|Real file storage|Only declared|FileStorageConsumer|InstallTreeConsumer' \
    --output-on-failure --parallel 4
cmake --preset ninja-clang -B build-114-without-files \
    -DMDDLOG_BUILD_FILE_STORAGE=OFF -DMDDLOG_BUILD_TESTS=OFF -DMDDLOG_BUILD_EXAMPLES=OFF
cmake --build build-114-without-files --parallel 4
```

| Scénario | Couverture |
| --- | --- |
| `file-storage-conformance` | Invariants partagés sur mémoire et fichiers |
| `file-storage-restart` | Sink réel, fermeture/réouverture, chaînage, lecture seule, aucune confirmation en mode non qualifié |
| `file-storage-barriers` | Transferts courts, interruptions, contrôle d'offset et ordre fichier/répertoire ; échec de chaque barrière |
| `file-storage-failures` | Écritures interrompues, octets partiels conservés, ENOSPC/EACCES/EIO injectés, zéro progression, lecture en erreur, limites, retrait non confirmé |
| `file-storage-health` | Échec de chaque barrière publié par la santé du sink, aucune position/claim durable avancé |
| `file-storage-ownership` | Verrous concurrents, ressources libérées, noms/liens/permissions et configuration refusés |
| `file-storage-reopening-protection` | Substitutions par lien/FIFO après create, délai de surveillance, fork/exec réel, inode conservé remplacé |
| `file-storage-reference-allocation` | Références retirées jamais réutilisées après redémarrage même sans segments, transferts de métadonnées courts/interrompus, chaque barrière, rename et zéro progression en échec, compteur régressé/absent/épuisé |
| `file-storage-inventory-diagnostics` | Nom/type/permissions/capacité rejetés avec errno nul malgré errno périmé, erreur système de lecture conservée |

Les journaux locaux de configuration, build, CTest et analyse sont conservés dans
`build-clang/verification-114/` (artefacts ignorés, sans archivage pérenne présumé).

## Corrections de revue de PR #134

Campagne complémentaire du 2026-10-05 sur le même environnement et volume XFS,
appliquée au diff suivant `f34d50b` :

- Compilation et liens : réussis.
- CTest complet : **182/182 réussis**, dont les neuf scénarios du backend et les consommateurs
  de modules en source et après installation.
- Analyse statique LLVM 21 : les deux unités modifiées (`FileStorageMedium.cppm` et
  `FileStorageSpec.cpp`) sont conformes avec diagnostics promus en erreurs. La preuve
  globale des 75 unités ci-dessus concerne la révision initiale `f34d50b`.
- Format : **84 fichiers conformes**. Dossier : structure et références conformes,
  **30 tests documentaires réussis**.

```bash
cmake --build --preset ninja-clang --parallel 4
TMPDIR="$PWD/build-clang/file-storage-tests" ctest --test-dir build-clang --output-on-failure --parallel 4
clang-tidy-21 -p build-clang --warnings-as-errors='*' include/mddlog/adapter/FileStorageMedium.cppm
clang-tidy-21 -p build-clang --warnings-as-errors='*' tests/spec/FileStorageSpec.cpp
scripts/check-format.sh
python3 scripts/check-development-file.py
python3 -m unittest discover -s tests/documentation -p 'Test*.py'
```

Le FIFO est testé dans un enfant surveillé pendant deux secondes pour qu'une régression
bloquante échoue sans suspendre la suite. Le contrôle d'exec vérifie effectivement
l'absence du descripteur dans `/proc/self/fd` après lancement de `/bin/sh`.
Ces processus ne constituent pas une campagne d'arrêt brutal de l'écriture.
Les pannes du compteur sont injectées séparément des barrières d'audit : aucun segment
n'est créé avant la réservation confirmée ; un temporaire résiduel bloque le redémarrage.
Les journaux sont conservés dans `build-clang/verification-134/` (artefacts ignorés).

Réserves : les erreurs de système d'exploitation injectées passent par `FileStorageCalls`
sur de vrais descripteurs, sans preuve de toutes les conditions physiques correspondantes.
Le mode `QualifiedFsync` des essais valide la décision de confirmation et l'ordre des appels,
pas l'éligibilité de cette machine. Ces campagnes initiales ne revendiquent aucun essai de coupure électrique
ni d'arrêt brutal. Le [protocole logiciel/physique](file-storage-test-plan.md) et
[la campagne suivante](file-storage-campaign-results.md) ajoutent les arrêts SIGKILL réels. Intégration complète registre/reprise/ancrage et fournisseur indépendant
restent ouvertes. Le profil, les limites et les obligations sont dans
[le contrat](file-storage.md) ; GAP-007 conserve la qualification de #114.
