# Exigences du consommateur et reconstruction des BMIs (#121)

Ce document décrit ce qu’un projet doit faire pour consommer mddlog depuis le paquet installé
ou depuis les sources, et ce que le paquet lui garantit ou lui refuse. La politique est celle
d’[ADR-007](adr/ADR-007-compatibility-and-distribution.md), candidate jusqu’au gel 1.0 ; les
combinaisons qualifiées sont dans [la matrice](compatibility-matrix.md).

## Projet consommateur minimal

```cmake
cmake_minimum_required(VERSION 4.0.0)
# 1. La porte import std de la série CMake utilisée, AVANT project().
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
else()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
endif()
# 2. C++23 sans extensions, AVANT project() : CMake crée la cible du module std à ce moment.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
project(Application LANGUAGES CXX)
# 3. Les sources du consommateur qui écrivent `import std;`.
set(CMAKE_CXX_MODULE_STD ON)

find_package(mddlog 0.3 CONFIG REQUIRED COMPONENTS core full)
add_executable(application main.cpp)
target_link_libraries(application PRIVATE mddlog::mddlog)
```

Le projet de référence exécuté par CTest est [tests/consumer/package](../tests/consumer/package/CMakeLists.txt) ;
il compile aussi en mode source avec `-DMDDLOG_CONSUMER_MODE=source -DMDDLOG_SOURCE_DIR=<arbre>`.

| Exigence | Raison | Refus si absente |
| --- | --- | --- |
| Générateur Ninja (ou Ninja Multi-Config) | CMake ne gère les modules C++ qu’avec ces générateurs ici | `find_package` échoue : « need a Ninja generator » |
| CMake 4.0 à 4.3 et porte `import std` avant `project()` | La porte dépend de la série CMake et ne peut plus agir après l’activation de CXX | Paquet refusé pour CMake 4.4+ ; « does not offer C++23 import std » sinon |
| `CMAKE_CXX_EXTENSIONS OFF` avant `project()` | Clang refuse un BMI `std` compilé en gnu++23 dans les compilations `-std=c++23` de mddlog | Erreur Clang « GNU extensions was enabled in precompiled file » |
| Même compilateur, même version et même bibliothèque standard que la construction | BMIs reconstruits et objets liés ; aucune ABI/BMI n’est promise entre compilateurs | « build mddlog from source with the consumer's toolchain » ; contournement non qualifié : `MDDLOG_ACCEPT_UNQUALIFIED_TOOLCHAIN=ON` |
| Mêmes options affectant l’ABI de la bibliothèque standard (`-stdlib=libc++`, `_GLIBCXX_DEBUG`, `_LIBCPP_ABI_*`, `/MD` vs `/MT`) | Les objets installés supposent ces choix | Non détecté par le paquet : erreur d’édition de liens ou comportement indéfini |
| Version demandée de la même mineure avant 1.0 | `SameMinorVersion` tant que la majeure vaut 0 | `find_package(mddlog 0.4)` refuse un paquet 0.3 |
| Composants optionnels demandés explicitement | `file_storage` et `audit_tool` n’existent que si la construction les a activés (Linux) | « component ... was not enabled » ou « Unknown mddlog component » |

`mddlog-build-info.json`, installé à côté de `mddlogConfig.cmake`, consigne la révision source
(relevée à chaque construction),
la chaîne exacte (compilateur, CMake, générateur, `CMAKE_CXX_FLAGS` et ceux de la configuration
construite, runtime MSVC sous MSVC, manifeste des modules std), la configuration, les options et
les dépendances liées. Le runtime MSVC y est consigné mais pas comparé à celui du consommateur. Les variables
`mddlog_BUILD_CXX_COMPILER_ID`, `mddlog_BUILD_CXX_COMPILER_VERSION`, `mddlog_BUILD_INFO_FILE`,
`mddlog_FILE_STORAGE` et `mddlog_AUDIT_TOOL` en exposent l’essentiel après `find_package`.

## Ce que le paquet ne modifie plus

Depuis ce lot, `mddlogConfig.cmake` ne change ni `CMAKE_CXX_STANDARD`, ni `CMAKE_CXX_MODULE_STD`,
ni la porte `import std`, ni les options de compilation du répertoire du consommateur. Les cibles
importées portent leurs exigences d’usage : `cxx_std_23` et `Threads::Threads` ; et, pour une
construction instrumentée, les options d’édition de liens des sanitizers. Les avertissements de
mddlog, `/EHsc`, `/permissive-`, `-fconcepts-diagnostics-depth` et les définitions de version
sont privés. Ils n’atteignent pas les unités du consommateur ; `MDDLOG_VERSION_*` et
`MDDLOG_PLATFORM_*` ne sont plus définies chez lui.

## Reconstruction des BMIs

Un BMI n’est pas distribué : le paquet installe les unités d’interface (`include/modules/mddlog/...`)
et CMake les recompile dans le consommateur, avec les définitions et options enregistrées pour
ces modules (`IMPORTED_CXX_MODULES_COMPILE_*`). Ainsi `mddlog::getVersion()` rend la version du
paquet. Les drapeaux globaux du consommateur (`CMAKE_CXX_FLAGS`) s’appliquent aussi à cette
reconstruction ; ses options de cible (`target_compile_options`) ne s’y appliquent pas.

Contraintes constatées par les consommateurs indépendants de ce lot :

- Avec `-Wextra`, GCC 16.1 et Clang 21 signalent `-Wmissing-field-initializers` sur les
  initialiseurs désignés qui omettent des membres par défaut. C’est l’idiome des structures
  d’options de mddlog (`{.time = ..., .message = ...}`). Un consommateur en `-Werror` ajoute
  `-Wno-missing-field-initializers` ; le consommateur de référence le fait explicitement.
- Avec Clang, CMake passe un `-c` redondant à la précompilation des BMIs ; Clang émet
  « argument unused during compilation: '-c' ». Un `-Werror` global dans `CMAKE_CXX_FLAGS`
  ferait donc échouer la reconstruction : préférez des avertissements limités aux cibles.
- Une bibliothèque Release installée sert un consommateur Debug avec GCC et Clang, et
  réciproquement ; avec MSVC, le consommateur garde la configuration de la bibliothèque, qui
  choisit la bibliothèque d’exécution (`/MD` ou `/MDd`).
- Une unité qui importe directement un module gouverné (`import mddlog.core.ring;`) lie
  `mddlog::core` explicitement : avec GCC, CMake 4.1 ne place pas les modules liés
  transitivement dans son mapper.

## Consommation depuis les sources

`add_subdirectory(<mddlog>)` ou `FetchContent` d’un arbre ou d’une
[archive source](compatibility-matrix.md#archive-source-reproductible) fournit les mêmes cibles
`mddlog::core` et `mddlog::mddlog`. Le consommateur choisit `MDDLOG_BUILD_TESTS`,
`MDDLOG_BUILD_EXAMPLES`, `MDDLOG_BUILD_FILE_STORAGE` et `MDDLOG_BUILD_AUDIT_TOOLS`. L’exécutable
de l’outil s’appelle alors `mddlog_audit` dans l’arbre et `mddlog::audit_tool` une fois installé.
Les exigences de générateur, de porte `import std` et d’extensions sont identiques, et les
refus de compilateur de `CMakeLists.txt` s’appliquent au moment de la configuration.
