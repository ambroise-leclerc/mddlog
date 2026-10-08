# Vérification du diagnostic borné (#118)

Livraison locale du lot, le 2026-10-08, sur la branche `118-bounded-diagnostics`.
La révision exacte est celle du changement Git contenant ce rapport ; aucune acceptation
mainteneur de l'épique ou qualification de dispositif n'est enregistrée ici. Le
[contrat](diagnostic-budgets.md) et la [migration](migration/bounded-diagnostics.md) sont les
entrées de revue de conception et d'ergonomie.

## Profil et commandes

Linux x86_64, Clang 21.1.8 / libc++ 21, CMake 4.2.3, Ninja 1.13.2, Release,
`-stdlib=libc++`, exemples/tests et backend fichiers activés. Aucune nouvelle dépendance.
SpecLab garde son pin existant ; les nouveaux consumers sont autonomes.

```sh
cmake --build build-clang --parallel 4
ctest --test-dir build-clang --output-on-failure --parallel 4
build-clang/tests/mddlog_diagnostic_memory_consumer
scripts/check-format.sh
scripts/run-clang-tidy.sh build-clang
python3 scripts/check-development-file.py
python3 -m unittest discover -s tests/documentation -p 'Test*.py'
```

La première campagne construit effectivement bibliothèque, modules, exemples et consumers ;
191/191 CTest passent, y compris consommateurs source/installation et un sous-ensemble initial
de huit scénarios diagnostiques. Les douze scénarios de la livraison finale incluent les ajouts
après cette campagne. Cette campagne initiale précédait le réalignement sur `develop` : la campagne finale
ci-dessous porte sur la base `9e9f1b30a383f07c497cb03470fdf0f966ff8f1d`, après #116/#117. Les résultats de la campagne finale après revue des tests sont consignés ci-dessous.
Les autres tuples publiés restent à la CI ; aucune réussite GCC, MSVC ou macOS n'est présumée.

## Couverture

`tests/consumer/BoundedDiagnosticConsumer.cpp`, CTest `diagnostic.bounded.*` :

| Scénario | Preuve |
|---|---|
| overload | Worker arrêté dans un sink à durée maximale 5 s ; capacité 3 messages incluant celui engagé, 2 flush ; 100000 refus Fatal exacts, taille excessive, filtre Debug, expiration et saturation des commandes, arrêt avec file pleine, admissions fermées, FIFO 0/1/2 et drainage. Deux flush admis et exactement un flush final. |
| failures-sync / failures-async | Sinks à exception et refus signalé plus sink sain ; pertes préservées, échecs de flush séparés, résultats par identité, flush void observable, arrêt en échec sans succès implicite. |
| callbacks-sync / callbacks-async | Diagnostic, flush et arrêt réentrants refusés ; retrait du sink courant et ajout d'un autre sans deadlock ; prochain snapshot, santé après clear. |
| concurrent | Quatre producteurs (8000 tentatives), flush avec délai et churn du registre, arrêt concurrent après un premier lot ; admissions/pertes/fermetures exhaustives, traitement des admissions, absence de duplications, hauts niveaux et commandes bornés. |
| order | Message avant barrière, message après barrière, FIFO vérifié ; sink retiré après engagement du snapshot reçoit uniquement le record engagé. |
| active-flush | Capacité 1 gardée par une commande retirée de la file mais bloquée dans le sink ; arrêt concurrent, tous les événements admis traités, un seul flush final. |
| configuration | Capacité zéro refusée ; limite exacte et limite +1 octet, cumul médical ; délais synchrones refusés explicitement. |
| console | Streambuf refusant écriture et flush, avec et sans masque d’exception : aucun succès d’écriture inventé, flush en échec dans la santé/statistiques, restauration puis arrêt réussi. |
| global | Configuration de façade, santé sans auto-initialisation, dépassement d'octets observable, fermeture de handles retenus. |

`tests/consumer/DiagnosticMemoryConsumer.cpp`, CTest `diagnostic.bounded.memory` : drain
arrêté dans un sink hôte borné, deux records de 50 octets admis sous budget 64, une commande
expirée conservée. Un million de cycles effectuent chacun un Fatal saturé, un Debug désactivé,
et un flush saturé. Le remplacement de `operator new/new[]` compte les allocations C++ ordinaires
utilisées par records/conteneurs/complétions ; pas les allocations propres d'un sink ni les
allocateurs C d'un composant tiers ou suralignés. Les entrées sont préconstruites et le sink est
immobile pendant la mesure.

Résultat local direct : `attempts=1000000 allocations=0 allocated_bytes=0`,
`messages=2 flushes=1`, 1000000 pertes de message et 1000000 refus de commande. Une exécution
directe dure 129550162 ns pour ces trois millions d'appels. Cette durée est une observation,
sans budget temps réel ; la preuve de plateau est l'absence d'allocations supplémentaires,
pas une mesure du pic RSS ni un plafond exact d'octets pour une application. Le coût initial
réservé et le contenu maximal admis sont expliqués dans le contrat. Les autres scénarios
vérifient libération du budget après traitement et refus sans sink disponible.

## Revue et limites

L'exemple `examples/BoundedDiagnostic.cpp` configure et surveille dans `main` ; la fonction
métier garde une unique ligne `Log::info`. L'admission gouvernée et l'audit ne changent pas.
La revue locale a identifié la nécessité de sérialiser avant admission en mode synchrone pour
que le flush ne dépasse pas un record admis, de garder la propriété des snapshots après retrait,
et de protéger l'arrêt global depuis le worker. Ces protections sont présentes ; elles ne
remplacent pas une revue mainteneur indépendante.

Le sink hôte doit revenir et borner lui-même ses appels. Les tests bloquent dans une limite
finie ; ils ne prouvent ni interruption de sink ni arrêt destructeur avec un sink infini.
La mémoire conservée par l'appelant dans ses résultats, les allocations des sinks et les rappels
différés sur un autre thread restent sous contrat hôte. Des allocations défaillantes sont aussi injectées avant construction du record, préparation
du snapshot de flush et flush final : aucun faux succès, compteurs conservés, arrêt complété
et `InternalFailure` séparé des échecs de sinks. La comparaison des statistiques de
sink suppose sa propriété exclusive et aucun reset concurrent. Les interleavings ne sont pas
exhaustifs. Le reproducteur TSan libc++ promise/future de #120 demeure distinct : ce nouveau
worker n'utilise pas promise/future, mais une condition de complétion ; aucune validation TSan
n'est déduite des tests CTest ordinaires. REQ-012 conserve sa qualification globale du profil
ouverte, GAP-011 suit la revue et l'acceptation des budgets et des obligations de sinks.

## Campagne finale

Base à jour `9e9f1b30a383f07c497cb03470fdf0f966ff8f1d` ; bibliothèque, exemples et
consumers construits réellement en Release sous Clang 21.1.8/libc++ et GCC 16.1.0/libstdc++.

| Vérification | Résultat |
|---|---|
| Clang, CTest hors `witness.deployment` | 229/229 PASS, dont 12 diagnostics, consommateurs source/installés complets et cœur seul |
| GCC, CTest hors `witness.deployment` | 228/228 PASS, dont les mêmes 12 diagnostics et consommateurs |
| Témoin isolé, exécutables Clang et GCC copiés sous `/tmp` | PASS dans les deux cas avec `--require-isolation` |
| Format LLVM 21 | PASS, 111 fichiers |
| clang-tidy LLVM 21, scope officiel | 97/97 unités sans findings, 42/42 interfaces `.cppm`, zéro `clang-diagnostic-error` |
| Dossier : structure/références et tests documentaires | PASS ; 59/59 tests Python |
| Exemple de supervision | Exécution PASS ; un événement admis, aucun refus ni échec |
| Plateau et injections d’allocation, Clang et GCC | PASS ; un million de cycles, zéro nouvelle allocation ; les trois injections ne fabriquent aucun succès |

La première invocation CTest complète sur la base à jour avait un échec de
`witness.deployment` : le namespace multi-UID ne pouvait pas traverser le home privé.
Le cas n’a pas été masqué par SKIP : les mêmes exécutables ont été copiés dans un répertoire
`/tmp` traversable, comme décrit dans la campagne du témoin, et le wrapper exigé a passé
avec les deux compilateurs. Les campagnes CTest finales excluent uniquement cette invocation
inadaptée au chemin du checkout et sont complétées par ces deux exécutions isolées.
Les nombres CTest diffèrent par le contrôle d’intégrité du cache de modules, spécifique à Clang.

```sh
ctest --test-dir build-clang --output-on-failure --parallel 4 -E '^witness.deployment$'
ctest --test-dir build-gcc --output-on-failure --parallel 4 -E '^witness.deployment$'
# Pour chacun des deux compilateurs :
task_witness_dir=$(mktemp -d /tmp/mddlog-witness.XXXXXX)
chmod 755 "$task_witness_dir"
cp build-clang/tests/mddlog_witness_deployment build-clang/examples/mddlog_witness "$task_witness_dir/"
python3 scripts/run-witness-deployment.py \
  --worker "$task_witness_dir/mddlog_witness_deployment" \
  --service "$task_witness_dir/mddlog_witness" --require-isolation
```

Les tentatives d’analyse pendant régénération des BMIs ont été invalidées après plantage
d’outil ; la campagne statique finale ci-dessus a été relancée sur les modules figés.
Aucun résultat de ces tentatives n’est utilisé comme réussite. Aucune nouvelle campagne
TSan/ASan/UBSan, macOS ou MSVC n’est revendiquée ; ces profils restent à la CI et à #120.
La revue et l’acceptation de GAP-011 restent à consigner par le mainteneur ; les preuves
locales n’acceptent pas les budgets et les obligations hôte d’un dispositif.
