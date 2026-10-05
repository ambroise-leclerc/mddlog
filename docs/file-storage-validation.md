# Vérification locale du premier lot #114

État : résultats locaux, sans acceptation du profil physique. Révision : diff de la branche
`114-file-storage` fondée sur `e3635d0`, identifiable par Git lors de la revue.

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

Les journaux locaux de configuration, build, CTest et analyse sont conservés dans
`build-clang/verification-114/` (artefacts ignorés, sans archivage pérenne présumé).

Réserves : les erreurs de système d'exploitation injectées passent par `FileStorageCalls`
sur de vrais descripteurs, sans preuve de toutes les conditions physiques correspondantes.
Le mode `QualifiedFsync` des essais valide la décision de confirmation et l'ordre des appels,
pas l'éligibilité de cette machine. Aucun essai de coupure électrique ni d'arrêt brutal
n'est revendiqué. Intégration complète registre/reprise/ancrage et fournisseur indépendant
restent ouvertes. Le profil, les limites et les obligations sont dans
[le contrat](file-storage.md) ; GAP-007 conserve la qualification de #114.
