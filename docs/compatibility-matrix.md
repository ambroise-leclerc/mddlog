# Matrice de compatibilité, profil de référence et distribution (#121)

La matrice **qualifiée** est l’ensemble des combinaisons exactes qu’une révision fait construire,
tester et consommer par sa CI. Les planchers de `CMakeLists.txt` (GCC 16.1, Clang 20 amont,
MSVC 19.40, CMake 4.0–4.3, Ninja) sont des conditions d’admission plus larges : une combinaison
admise n’est pas qualifiée. Politique : [ADR-007](adr/ADR-007-compatibility-and-distribution.md),
candidate jusqu’au gel 1.0 après #122.

## Combinaisons exercées par la CI

| Plateforme | Compilateur / bibliothèque standard | CMake | Configurations | Consommateurs indépendants | Épinglage |
| --- | --- | --- | --- | --- | --- |
| Linux x86_64 | GCC 16.1.0 / libstdc++ (conteneur `gcc:16.1.0`) | 4.1.1 | Release (suite complète) ; Debug ASan/UBSan et TSan (`sanitizers.yml`) | installés complet et cœur, sous-répertoire, archive | exact |
| Linux x86_64 — **profil de référence** | Clang 21 amont / libc++ (apt.llvm.org) | 4.3.1 | Release avec outil CLI ; Debug ASan/UBSan ; fuzzing ; campagnes | installés complet et cœur, sous-répertoire, archive | majeure 21 contrôlée, correctif flottant |
| macOS 15 arm64 | Clang 21.1.8 amont / libc++ (Homebrew `llvm@21`) | 4.3.1 | Release ; Debug ASan/UBSan | installés complet et cœur, sous-répertoire, archive | exact (refus sinon) |
| Windows x64 | MSVC de l’image `windows-latest` | 4.1.1 | Release | installés complet et cœur, sous-répertoire, archive | version de l’image flottante |

Les deux combinaisons à correctif ou image flottants sont **exercées**, non figées. Avant le
gel 1.0, il faut soit épingler leur version exacte, soit conserver par exécution le
`mddlog-build-info.json` du paquet testé, que `InstallTreeConsumer` imprime. Le signalement
TSan de Clang/libc++ ([#147](https://github.com/ambroise-leclerc/mddlog/issues/147),
[source conservée](validation/audit-robustness/tsan-libcxx/README.md)) reste ouvert : ce tuple
n’est pas qualifié pour TSan ; le TSan qualifié est GCC/libstdc++.

Le profil de référence est celui qui active tout : adaptateurs fichiers Linux, outil
`mddlog-audit`, sanitizers, fuzzing et campagnes de robustesse. Il ne remplace pas le profil
de déploiement réel, qui relève de #122.

## Debug, Release et options du consommateur

- La suite complète tourne en Release sur les quatre combinaisons ; Debug est exercé avec les
  sanitizers sur Linux et macOS. Windows Debug n’est pas exercé en CI : preset disponible, non qualifié.
- Une bibliothèque installée en Release sert un consommateur Debug avec GCC et Clang
  (`InstallTreeConsumer`, `InstallTreeCoreConsumer`) ; avec MSVC, même configuration.
- Le consommateur applique ses propres avertissements stricts (`-Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion -Werror`, ou `/W4 /WX`). Les contraintes constatées
  sont listées dans [les exigences du consommateur](consumer-requirements.md#reconstruction-des-bmis).
- Les jobs sanitizers excluent les consommateurs indépendants, qui reconstruisent la bibliothèque
  et n’apportent pas de signal d’instrumentation supplémentaire.

## Qualifier une nouvelle version de CMake ou de compilateur

Les bornes ne s’élargissent qu’après qualification, sur une branche d’issue :

1. Lire les notes de version : porte `import std`, format P1689, flags de modules.
2. Ajouter ou modifier le job CI correspondant, sans retirer l’ancien tant que la nouvelle
   combinaison n’a pas réussi.
3. Exiger sur la nouvelle combinaison la configuration, la construction, la suite complète,
   `build.core.*` et les cinq tests `build;consumer`. Relever séparément configuration,
   construction et tests.
4. Mettre à jour `CMakeLists.txt`, `cmake/mddlogConfig.cmake.in` et la porte des consommateurs.
   Mettre à jour cette matrice, le registre (DEP-004/DEP-005) et le changelog.
5. Garder un refus explicite pour ce qui reste non qualifié. Exemple : CMake 4.4+ est refusé
   jusqu’à revue de sa porte ; GCC 16.2 l’est pour ses BMIs corrompus. Le message nomme
   l’alternative supportée.

Un refus est un résultat attendu, testé par `build.package.refusals` pour le paquet installé :
version hors politique, composant inconnu, générateur non Ninja et compilateur différent.
La configuration de la bibliothèque refuse elle-même les compilateurs, générateurs et versions
CMake non admis.

## Archive source reproductible

```bash
python3 scripts/package-source.py --ref vX.Y.Z --output dist
```

L’archive `mddlog-X.Y.Z-src.tar.gz` contient tous les fichiers suivis du commit et un
`SOURCE_REVISION`, avec métadonnées normalisées et horodatage du commit. Le manifeste
`mddlog-X.Y.Z-src.manifest.json` liste les empreintes SHA-256 de chaque fichier, du flux tar et
de l’archive compressée, ainsi que les dépendances épinglées (SpecLab par commit, CPM.cmake
intégré, bornes CMake/Ninja). Le flux tar est reproductible à partir du commit ; les octets gzip
dépendent aussi de zlib. `--worktree` empaquette les fichiers suivis de la copie de travail pour
les tests (`SourceArchiveConsumer`) ; une telle archive est marquée et n’est pas une livraison.

## Gestionnaires de paquets

Aucune intégration n’est retenue. Aucun gestionnaire n’a démontré, sur cette matrice,
l’installation de modules C++ nommés consommés avec `import std`. Une intégration sera proposée
avec un consommateur indépendant qui le prouve, selon la procédure ci-dessus.
