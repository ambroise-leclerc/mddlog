# Vérification locale de la position retenue — premier lot de #115

Ce rapport conserve la campagne de 196 tests effectuée avant ajout du service témoin.
Le lot complet et ses preuves supplémentaires sont dans
[la campagne du témoin](independent-witness-validation.md).

Campagne du 2026-10-06, branche `115-retained-position`, base `develop`
`5573287` (PR #139). Le lot est identifié par le diff et les sources de cette branche ;
les résultats ci-dessous concernent ce lot, sans constituer l'acceptation de #115.

## Profil et commandes

Linux x86_64, noyau `7.0.0-34-generic`, glibc 2.43, Clang/libc++ 21.1.8,
CMake 4.2.3 et Ninja 1.13.2. Preset `ninja-clang` en Release ; exemples, tests et
backend fichiers activés, fuzzers désactivés. SpecLab à la révision figée par CMake.
Les répertoires temporaires des checkpoints sont sur **tmpfs** : les appels de
synchronisation et la réouverture sont testés, aucune persistance électrique n'est prouvée.

```bash
cmake --preset ninja-clang
cmake --build --preset ninja-clang --parallel 6
ctest --preset ninja-clang --parallel 6
scripts/check-format.sh
clang-tidy-21 -p build-clang --warnings-as-errors='*' \
  include/mddlog/adapter/FileRetainedPosition.cppm
clang-tidy-21 -p build-clang --warnings-as-errors='*' \
  tests/spec/FileRetainedPositionSpec.cpp
python3 scripts/check-development-file.py
python3 -m unittest discover -s tests/documentation -p 'Test*.py'
```

## Résultats

- Compilation effective de la bibliothèque, des exemples et des tests réussie.
- CTest : **196/196 réussis**, dont le scénario `audit-retained-file`,
  `SourceTreeFileStorageConsumer`, `InstallTreeConsumer`, `SourceTreeCoreConsumer`
  et `InstallTreeCoreConsumer`. Le consommateur fichiers importe et utilise le
  nouveau module depuis les sources et le package installé.
- Formatage LLVM 21 : 94 fichiers vérifiés, succès.
- Analyse LLVM 21 des deux nouvelles unités : succès, aucun diagnostic du projet,
  avec tous les avertissements promus en erreurs.
- Registre et références du dossier : succès ; 57 tests documentaires réussis.
  Ces contrôles n'acceptent pas le contenu ni le profil de déploiement.

Le scénario de rollback construit et vérifie deux événements, sauvegarde la position du
lecteur, détruit son magasin, restaure le fournisseur en mémoire et ne présente plus que
le premier événement. Le lecteur recréé charge le checkpoint et conclut `RolledBack`.
La réouverture couvre aussi digest, counter et retrait. Une autre identité fournisseur
est refusée. Un état perdu ne déclenche aucune initialisation implicite.

Deux lecteurs partent de la même génération et tentent des sauvegardes simultanées :
une seule réussit, l'autre reçoit `Busy` ou `Conflict`. Une tentative séquentielle avec
ancienne génération est refusée, ainsi qu'une baisse de head/position/counter,
la disparition d'un flux, le changement de digest à position égale et l'annulation
d'un retrait. Tous les préfixes tronqués du fichier témoin, un fichier trop grand,
un checksum corrompu, une version inconnue, un lien symbolique et un répertoire
partagé sont refusés.

Les erreurs ENOSPC après écriture partielle, fsync fichier, rename et fsync répertoire
sont injectées par la seam existante `FileStorageCalls`. Avant rename, l'ancienne
génération reste lisible ; après rename et échec de barrière répertoire, la nouvelle
génération est visible mais l'opération retourne `Sync`. Le lecteur doit la réconcilier
et confirmer une nouvelle sauvegarde. EINTR et écritures courtes sont repris.

## Réserves

Le témoin est un double en mémoire ; aucune séparation effective des comptes ni
qualification du service n'est revendiquée. Les droits croisés du rédacteur, les ACL et
parents, les redémarrages de processus indépendants et les coupures électriques du
lecteur restent à éprouver sur le profil complet. Cette campagne ne teste pas un
transport authentifié ni la migration opérée du fournisseur. L'analyse statique est
limitée aux deux nouvelles unités ; elle n'est pas une nouvelle campagne globale.
Les preuves externes antérieures du stockage gardent leur propre portée.
GAP-008/#115 et le programme #112 restent ouverts.

Empreintes SHA-256 des deux nouvelles unités analysées :

```text
e2be17dd48fdb56afe43203ffc8b32ee48b3577107e3ff4ef5718dade4ee0610  FileRetainedPosition.cppm
14078b170e1d825cbdf45f5c44bbc18d26a0454172966e9b7eef215c17544f45  FileRetainedPositionSpec.cpp
```
