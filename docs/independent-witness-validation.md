# Campagne du témoin indépendant — #115

Campagne locale du 2026-10-06, branche `115-retained-position`, base `develop`
`5573287cc8a7abba0b85c5b35e5a31f5e47662fa`. La révision livrée est celle de la PR
associée à ce rapport. Ce lot complète la campagne historique du checkpoint.

## Profil reproductible

Linux x86_64, noyau 7.0.0-34-generic, glibc 2.43, Clang/libc++ 21.1.8, CMake 4.2.3,
Ninja 1.13.2, util-linux 2.41.3 (`2.41.3-3ubuntu2.2`) et uidmap
`1:4.17.4-2ubuntu3` ; preset Release `ninja-clang`, exemples/tests/backend Linux activés.
SpecLab est figé dans CMake à `d85c1f74d95a25b2bf2733316149ba7936ed7df6`.
Les fichiers temporaires sont sur tmpfs : réouvertures et barrières exécutées,
aucune coupure électrique ni qualification de caches/matériel.

```bash
cmake --preset ninja-clang
cmake --build --preset ninja-clang --parallel 6
ctest --preset ninja-clang --parallel 6
python3 scripts/run-witness-deployment.py \
  --worker "$PWD/build-clang/tests/mddlog_witness_deployment" \
  --service "$PWD/build-clang/examples/mddlog_witness" --require-isolation
scripts/check-format.sh
python3 scripts/check-development-file.py
python3 -m unittest discover -s tests/documentation -p 'Test*.py'
```

Le wrapper utilise `unshare --user --map-auto` sans privilège hôte, avec une plage
subuid/subgid disponible. À défaut, le CTest retourne SKIP 77 explicitement ;
cela inclut une sonde bloquée au-delà de 10 secondes, dont le groupe de processus
est arrêté et récupéré. `--require-isolation` en fait un échec. La CI Clang exécute ce mode requis via sudo,
après avoir autorisé pour root les plages subuid/subgid `100000-100004` du runner
éphémère (`usermod --add-subuids 100000-100004 --add-subgids 100000-100004 root`).
`newuidmap`/`newgidmap` exigent cette autorisation même sous root. Le namespace mappe
ces cinq UID sans créer de comptes hôte. Les deux exécutables sont copiés dans un
répertoire temporaire traversable sous `/tmp`, car les UID mappés ne peuvent pas
traverser le home privé du runner ; la CI supprime ce répertoire en sortie.
Le même chemin privilégié est exercé localement
à partir d'un namespace parent, avec `--outer-uid-start 5` (plage disponible du parent).
Les tests ne qualifient pas la configuration des comptes d'un déploiement physique.

## Critères exercés

- `witness-durable-authority` : enrôlement explicite, absence, verrou exclusif,
  monotonie des counters après réouverture, retrait conservé, refus de revival et
  retrait répété, mauvaise identité, corruption et perte. Erreurs ENOSPC, barrière
  fichier et barrière répertoire injectées ; toutes les opérations sont arrêtées
  après échec, puis l'état visible est resynchronisé à la reprise.
- `witness-unix-transport` : vrai socket Unix, UID serveur épinglé, droits par flux
  et séparation avancement/retrait, les quatre opérations, service manquant, service
  silencieux avec délai de 20 ms, requête malformée/extra et tag 257 rejetés sans
  mutation. Réponses d'avancement et de retrait perdues après application : la
  lecture authentifiée retrouve l'état ; les nouvelles requêtes identiques ou
  conflictuelles gardent leurs refus ADR-004. Ces tests utilisent le même UID,
  et **ne prouvent pas l'indépendance**.
- `witness.deployment` : exécutable de référence dans un processus séparé, UID 1
  témoin, 2 rédacteur, 3 retrait, 4 lecteur dans un namespace isolé. Le controller
  UID 0 provisionne le fixture puis les acteurs abandonnent leur autorité UID.
  Le rédacteur reçoit EACCES sur les fichiers du témoin et le répertoire du lecteur.
  Flux non enrôlé, retrait par rédacteur et mutation par lecteur sont refusés.
  Deux demandes simultanées autorisées au même checkpoint donnent un seul stamp,
  l'autre reste `PositionNotIncreasing`. Le retrait autorisé survit au redémarrage
  indépendant du service.

Le rédacteur appelle `AuditBinding::record` et `AuditService` avec le backend réel de
#114, le client réel et des identités de démarrage/ledger enrôlées. Pour exercer le
chemin de confirmation de ce fixture, il déclare `QualifiedFsync` sur tmpfs ; cette
**déclaration de test n'est pas une qualification acceptée du support**. Le lecteur
vérifie une image privée hors ligne, sous son UID, et sauvegarde son checkpoint.
Le journal vivant garde en permanence son UID rédacteur.

Après le premier démarrage, le controller garde une ancienne image du journal et
une copie de l'état du témoin. Le service et le rédacteur redémarrent séparément,
le second démarrage ajoute un flux/ledger et le lecteur relève son état puis s'arrête.
Le controller restaure ensuite ancien journal et ancien témoin avec l'autorité
administrative du fixture. Le service redémarre ; un **nouveau processus lecteur**
charge sa position retenue intacte et rend `RolledBack`. Le rédacteur ne possède
pas l'autorité permettant cette restauration du témoin ; le test éprouve aussi
la détection de rollback combiné au-delà de sa propre frontière de confiance.

## Résultats

- Trois scénarios de témoin (deux SpecLab + déploiement) : réussis, aucun SKIP local.
- Déploiement avec `--require-isolation` : réussi sur les deux chemins de mapping.
- Compilation effective Release de la bibliothèque, exemples et tests : réussite.
- CTest complet : **199/199 réussis**, aucun SKIP ; consommateurs source/install et
  tests du cœur seul inclus.
- Formatage clang-format 21 : **100 fichiers vérifiés**, succès.
- Structure/références du dossier : succès ; **57 tests documentaires réussis**.
- ASan/UBSan Debug : les trois scénarios de restauration/autorité/transport réussissent,
  avec `ASAN_OPTIONS=detect_leaks=1` et `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`.
  La campagne multi-UID n'est pas incluse dans ce passage de sanitizer.
- Analyse clang-tidy 21 des **six nouvelles unités du témoin** : succès, aucun
  diagnostic du projet avec tous les avertissements promus en erreurs.

L'analyse utilise clang-tidy 21 avec tous les avertissements promus en erreurs sur
les trois nouveaux modules du témoin, ses deux unités de test et l'exécutable.
Les deux unités de checkpoint ont leur campagne antérieure dans le rapport dédié.
Le passage de sanitizer se reproduit avec :

```bash
cmake --preset ninja-clang-debug -DMDDLOG_BUILD_EXAMPLES=OFF \
  -DENABLE_SANITIZER_ADDRESS=ON -DENABLE_SANITIZER_UNDEFINED_BEHAVIOR=ON
cmake --build --preset ninja-clang-debug --target mddlog_specs --parallel 6
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --test-dir build-clang-debug \
  -R 'Witness|Unix witness|Persistent reader' --output-on-failure
```

## Vérifications après revue

Après revue, la compilation Release GCC 16.1 et ses **198/198 tests CTest** passent
également en local. Les trois scénarios ciblés passent sous ASan/UBSan GCC 16.1.
Deux contrôles négatifs du wrapper couvrent l'indisponibilité/expiration de la sonde,
la distinction SKIP/FAIL et l'arrêt de ses helpers ; les **59 tests documentaires** passent.

Les contrôles de revue supplémentaires couvrent les refus à 4096 flux, au counter
`UINT64_MAX` et à la taille maximale du fichier : aucun write/fsync n'est appelé,
`lastError` reste vide et les lectures restent disponibles. Au plafond de flux,
les mutations d'un flux existant restent possibles et le retrait ne libère pas sa place.
Un vrai backlog Unix saturé vérifie `Connect/EAGAIN` plutôt qu'`Authentication`.
Une écriture nulle du checkpoint, avec errno ENOSPC volontairement périmé, rend
`Write` avec erreur native zéro et conserve l'ancienne génération.

## Réserves

Résultats logiciels reproductibles, sans acceptance indépendante ni certification.
Le déploiement cible (comptes, UID non réaffectés, parents/ACL, filesystem, cache,
alimentation et supervision) reste à qualifier et accepter dans #115/#122. Une
restauration de l'ensemble du magasin du lecteur par son autorité sort du modèle.
Le témoignage distant, la signature et le cycle de vie des clés ne sont pas fournis.
Le protocole borne transport/taille, sans WCET de fsync ni politique complète sous
surcharge. Le lecteur actuel reçoit un inventaire complet, dans la limite déclarée.
La revue de PR et l'intégration finale ne sont pas présumées acceptées par ce rapport.
