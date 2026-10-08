# Validation locale du lot compatibilité et distribution (#121)

Ce relevé établit les preuves locales de CTRL-019 et VER-073–077. Il est livré pour revue ;
il ne constitue ni l’acceptation d’[ADR-007](adr/ADR-007-compatibility-and-distribution.md),
ni le gel 1.0, ni une qualification du profil de déploiement (#122). La CI de la PR fait foi
pour Windows, macOS et les jobs du dépôt ; elle n’est pas présumée par ce document.

## Révision et environnement

- Base : `develop` à `2b606df47d5bf2d7db5e021a8bf359173ee5490e`, avec les modifications de la branche
  `121-compatibility-commitments` (révision exacte : celle du commit qui contient ce document).
- Hôte : Ubuntu 26.04.1 LTS x86_64, CMake 4.2.3, Ninja 1.13.2, Python 3.14.6.
- Chaînes : GCC 16.1.0/libstdc++ ; Clang 21.1.8/libc++ via `cmake/toolchains/linux-clang21-libcxx.cmake`.
- Configuration des bibliothèques : Release, tests et exemples activés, `MDDLOG_BUILD_AUDIT_TOOLS=ON`
  (adaptateurs fichiers activés par défaut sous Linux).

## Résultats

Configuration, construction et tests sont relevés séparément.

| Contrôle | GCC 16.1 | Clang 21.1.8 |
| --- | --- | --- |
| Configuration | réussie | réussie |
| Construction complète | réussie | réussie |
| CTest complet | 241/241 | 242/242 (dont `build.cache.namedModuleIntegrity`) |
| `InstallTreeConsumer` (Release installé → consommateur Debug, composants core/full/file_storage/audit_tool) | réussi | réussi |
| `InstallTreeCoreConsumer` (composant core seul) | réussi | réussi |
| `SourceSubdirectoryConsumer` | réussi | réussi |
| `SourceArchiveConsumer` (`package-source.py --worktree`, extraction, sous-répertoire) | réussi | réussi |
| `build.package.refusals`, dont compilateur différent (`MDDLOG_TEST_OTHER_CXX_COMPILER`) | réussi (autre : clang++-21) | réussi (autre : g++) |
| `SourceTreeMigrationExamples` | réussi | réussi |
| `build.core.*` (frontière gouvernée inchangée) | réussis | réussis |

Contrôles sans compilation C++ :

- `python3 -B scripts/check-compatibility.py --git` : cohérent, y compris historique v0.1.0–v0.3.0
  (aucun nom ni module publié retiré, `since` conforme, archives v0.2.0/v0.3.0 présentes).
- `python3 scripts/check-development-file.py` : structure et références valides.
- `python3 -B -m unittest discover -s tests/documentation -p 'Test*.py'` : 95 tests réussis,
  dont 10 de `TestCompatibility.py` et 6 de `TestSourcePackage.py` avec contrôles négatifs.
- `scripts/check-format.sh` : 121 fichiers conformes à clang-format 21.
- Deux exécutions de `package-source.py --ref HEAD` produisent des octets identiques.

## Constats

1. **Fuites corrigées.** Avant ce lot, le paquet exportait `mddlog_warnings`. La reconstruction
   des BMIs installés recevait donc `-Wall … -Werror`, observé 90 fois dans le `build.ninja` d’un
   consommateur. Elle exportait aussi `/W4`, `/permissive-`, `NOMINMAX` et les macros de version
   vers les unités du consommateur. Après correction, seules les options de cible du consommateur
   portent `-Werror`. Les définitions de version atteignent les BMIs reconstruits par
   `IMPORTED_CXX_MODULES_COMPILE_DEFINITIONS` : `FullConsumer` vérifie la version.
2. **Exigence nouvelle, Clang.** Avec `CMAKE_CXX_EXTENSIONS OFF` après `project()`, le BMI `std`
   est créé en gnu++23 et Clang refuse de le charger : « GNU extensions was enabled in precompiled
   file ». Cette exigence est documentée dans [les exigences du consommateur](consumer-requirements.md).
3. **Contrainte d’avertissements.** `-Wextra` de GCC 16.1 et Clang 21 signale
   `-Wmissing-field-initializers` sur les initialiseurs désignés des structures d’options. Le
   consommateur de référence le désactive explicitement ; aucun changement d’API n’est fait.
4. **Configuration du paquet.** `mddlogConfig.cmake` ne modifie plus le consommateur ; un
   consommateur qui active lui-même C++23 et `import std` compile sans aucun réglage hérité.

## Limites

- Windows/MSVC et macOS n’ont pas été exécutés localement. Le retrait des options MSVC globales
  du paquet (`/experimental:module`, `/std:c++latest`) repose sur `cxx_std_23` et doit être
  confirmé par la CI Windows de la PR.
- Le cas « autre compilateur » de `build.package.refusals` n’existe que si
  `MDDLOG_TEST_OTHER_CXX_COMPILER` est fourni ; la CI ne le fournit pas.
- Les options ABI de la bibliothèque standard (`_GLIBCXX_DEBUG`, `/MT`…) ne sont pas détectées.
- Les sources consommateurs (`tests/consumer/`) restent hors du périmètre clang-tidy, comme avant.
- Les niveaux restent candidats ; les `reduction-candidate`, l’épinglage des jobs Clang Linux et
  MSVC et le signalement #147 sont ouverts (GAP-014).
