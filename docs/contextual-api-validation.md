# Vérification des liaisons contextualisées — #128/#129

Statut : **implémenté et vérifié localement**, preuves à revoir dans #130 ; aucun gel 1.0
ou profil de dispositif accepté. La conception [ADR-005](adr/ADR-005-contextual-logging-api.md)
a été acceptée par Ambroise Leclerc le 2026-10-04 sur `f3063bc4122cad41f23352721824db9094066b3d`.
La présente campagne vise la révision logicielle `8fe4fb10bb65e92064529e48e44327f0e556c99e`.
Les mises à jour documentaires ultérieures ne constituent pas une nouvelle campagne C++.

## Ce qui est livré

Quatre modules gouvernés possèdent les identifiants et empruntent les anneaux existants :
[DiagnosticContext](../include/mddlog/core/DiagnosticContext.cppm),
[GovernedBinding](../include/mddlog/core/GovernedBinding.cppm),
[AuditContext](../include/mddlog/core/AuditContext.cppm) (incluant `AuditDescription`) et
[AuditBinding](../include/mddlog/core/AuditBinding.cppm).
Le [module d'adaptateur](../include/mddlog/adapter/DiagnosticBinding.cppm) fournit une seule
liaison injectée pour `TextLogger` et `SimpleLogger`. La référence et la migration sont dans
[le guide](migration/contextual-logging.md).

`AuditEvent::validateIdentifier` est partagé par descriptions, contextes et admission ;
les règles et l'ordre d'erreur de l'admission existante restent inchangés. La description
producteur refuse le vocabulaire réservé ; la voie du ledger via `AuditEvent::assign` reste
celle d'ADR-004. Les formats et les valeurs de `AuditWriteResult`/`WriteResult` ne changent pas.
Les factories `expected` utilisent des valeurs possédées sans accesseur levant dans le cœur.
La surcharge contextualisée de `SimpleLogger` préserve aussi les appels à catégorie vide `{}`.

## Profil et résultats

Campagne du 2026-10-04 : Linux x86_64, Clang/libc++ 21.1.8, CMake 4.2.3, Ninja, preset
`ninja-clang` Release, exemples/tests activés et SpecLab épinglé par CMake.

```sh
cmake --build --preset ninja-clang --parallel 8
ctest --preset ninja-clang --parallel 8
scripts/check-format.sh
JOBS=8 scripts/run-clang-tidy.sh build-clang
python3 scripts/check-development-file.py
python3 -m unittest discover -s tests/documentation -p 'Test*.py'
```

Construction effective réussie, puis **166/166 tests CTest réussis**. Ce total inclut
les neuf nouveaux scénarios SpecLab, l'étude avant/après convertie vers l'API publique,
`SourceTreeContextConsumer`, `SourceTreeCoreConsumer`, `InstallTreeConsumer` et
`InstallTreeCoreConsumer`. Le consommateur complet installé compile la nouvelle API via
`import mddlog;`; le consommateur gouverné installé n'importe et ne lie que le cœur.
Les cinq contrôles `build.core.*` réussissent avec **11 modules gouvernés** et leurs imports
revus, en incluant les nouveaux corps de templates et factories dans le probe d'objets.

Analyse complète **clang-tidy 21 : 70/70 unités réussies**, dont **35/35 interfaces `.cppm`**,
zéro unité en échec et zéro occurrence `clang-diagnostic-error`. Formatage réussi sur
77 fichiers avec clang-format 21. Contrôle structure/références du dossier réussi et
**30/30 tests documentaires réussis** ; `git diff --check` réussi. La présence d'un module
ou d'un scénario dans le registre n'est pas une acceptation de ses preuves.

## Couverture et budgets

Les références de contrôle/vérification restent dans le [registre unique](../software_development_file/register.json),
CTRL-011–013 et VER-011–019. Les scénarios exercés sont dans
[DiagnosticContextSpec](../tests/spec/DiagnosticContextSpec.cpp),
[DiagnosticBindingSpec](../tests/spec/DiagnosticBindingSpec.cpp) et
[AuditContextSpec](../tests/spec/AuditContextSpec.cpp).
Ils vérifient identifiants exacts et invalides, copies de temporaires, contextes imbriqués et
indépendants, origine du véritable appelant, temps variable par événement, saturation et
troncature UTF-8, filtres et fabrique non évaluée, champs structurés de `SimpleLogger`,
exception de fabrique sans émission, phases explicites et absence de phases à la destruction.
Le cas de reconfiguration du filtre pendant la fabrique démontre l'absence de transaction.
Deux producteurs concurrents avec des contextes dérivés et deux anneaux vérifient l'isolation ;
les tests SPSC existants continuent de couvrir publication/acquittement avec consommateur.

Tailles mesurées par le [consommateur gouverné](../tests/consumer/CoreConsumer.cpp), sur ce profil :

| Valeur | Octets |
| --- | ---: |
| `DiagnosticContext` | 110 |
| `GovernedBinding<N>` | 120 |
| `AuditDescription` | 200 |
| `AuditContext` | 284 |
| `AuditBinding<N>` | 496 |

Ces tailles ne dépendent pas de N ; les anneaux possédés par l'hôte gardent leur propre budget.
Les assertions statiques bornent les contextes par les capacités existantes et les liaisons
par leurs valeurs possédées plus marge d'alignement/pointeur. Aucun enregistrement complet n'est
conservé en contexte, contrairement au prototype initial. Le budget du `GovernedRecord` reste
384 octets au maximum et ses champs restent ceux d'ADR-001.

Le chemin d'émission prépare des vues en nombre fixe puis délègue à `tryWrite`/`tryRecord`.
Validation et copies sont bornées par les capacités existantes ; aucune attente, allocation,
horloge ou nouveau verrou n'entre dans le cœur. Le scan lexical et les scans d'objets
allocation/exception contrôlent les modules et les instanciations. Ils ne prouvent pas un WCET,
une absence de tout défaut du runtime ou une qualification de chaque ABI. Les budgets temporels
mesurés du profil et la campagne de robustesse complète restent #117/#120.

## Lisibilité et limites de clôture

L'[exécutable](../examples/ContextualUsage.cpp) utilise directement les nouvelles valeurs et
liaisons. Ses cinq paires avant/après restent compilées et exécutées ; les fonctions métier
portent messages, phases, temps et politique de refus, avec contexte/destination à la composition.
Le [rapport initial](contextual-api-study.md) conserve la comparaison acceptée et sa révision.
La revue sur un composant applicatif réel, les preuves complètes de durée de vie et de concurrence,
la clôture de l'expérience #113 et la migration intégrée relèvent encore de #130.

REQ-009/REQ-011 restent au statut prévu pour l'expérience complète jusqu'à cette revue ;
leurs contrôles et les primitives de #128/#129 sont ici implémentés. GAP-006 reste ouvert pour
#130. Ni ce rapport ni l'acceptation de conception ne qualifient stockage, fournisseur indépendant,
réponse hôte à un refus critique ou application médicale. La bibliothèque ne fournit pas de
politique de confidentialité ; l'hôte minimise et protège les identifiants.
