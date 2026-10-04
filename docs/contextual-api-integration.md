# Intégration des contextes et revue de lisibilité — #130

Statut : **réalisation et revue technique locale disponibles, revue mainteneur à effectuer**.
La conception ADR-005 est acceptée ; l'acceptation du prototype n'est pas celle de ce lot.
Campagne du 2026-10-04 sur `c80790fee0e3fda26abc32a44d30504117105046`, branche
`130-contextual-integration`, après les liaisons vérifiées sur `8fe4fb1`.
Les changements documentaires suivants ne changent pas la révision C++ testée.

## Composant applicatif examiné

[InventoryWorker](../examples/InventoryWorker.hpp) est un composant local de gestion de stock,
intégré par [ContextualInventory](../examples/ContextualInventory.cpp). Il possède son compteur,
reçoit deux liaisons préparées par l'hôte et vérifie une modification avant de la réaliser.
La somme est calculée en `int64_t` puis vérifiée avant conversion vers le compteur `int`.
Sous-débordement et dépassement produisent explicitement `Failed`, sans mutation.
Les temps de demande et d'observation sont deux paramètres distincts fournis par l'hôte.

La politique de ce composant est observable : `Requested` doit être admis avant mutation ;
si l'issue est refusée après mutation, le résultat conserve `changed=true`, l'admission de
la demande et le refus de l'issue. Un refus de demande laisse l'issue et le diagnostic absents.
Le diagnostic est séparé : sa saturation ne bloque ni l'audit ni la mutation. Cette politique
appartient au composant hôte ; elle n'est pas imposée par la bibliothèque. La liaison n'exécute
aucune action et aucun destructeur ne publie une phase.

`adjustBefore` fournit exactement la même politique et les mêmes faits avec `tryRecord`/
`tryWrite`. La comparaison réutilise les mêmes nouvelles valeurs de capture pour isoler
le changement au point d’émission ; elle ne prétend pas compiler ces factories avec la v0.2. Le scénario `equivalence` compare mutations, phases, liens `sourceSequence`,
identifiants et temps. Les identités de flux diffèrent intentionnellement entre les deux
producteurs comparés ; chaque flux conserve ses propres séquences. La source du diagnostic
se trouve dans la méthode métier `apply`, pas dans une méthode interne de liaison.

Il s'agit d'une application exécutable locale avec état métier et consommateurs, **pas d'une
application indépendante ou d'un dispositif**. #122 garde l'intégration indépendante sur une
chaîne réelle ; #117/#120 gardent budgets du profil et robustesse étendue. Aucun support
persistant, ancrage ni politique de rétention n'est qualifié ici ; leur pilotage reste #116.

## Mesure et appréciation de lisibilité

Commande reproductible, sur les sources formatées :

```sh
python3 scripts/measure-contextual-usage.py
```

Le script compte les lignes non vides dans les corps (politique de refus et contrôle inclus,
commentaires exclus), ainsi que les occurrences de projections invariantes : affectations,
accès aux champs nommés et préfixes texte. Ce ne sont ni des mesures de performance, ni un
score automatique d'ergonomie. Le helper avant de l'action auditée est inclus : déplacer la
répétition dans un helper ne la fait pas disparaître du décompte. Les signatures et la
composition ne sont pas incluses dans ces lignes ; leur coût est examiné séparément ci-dessous.

| Usage | Lignes avant | Lignes après | Projections invariantes avant/après |
| --- | ---: | ---: | ---: |
| Composant | 2 | 2 | 2/0 |
| Opération | 2 | 2 | 2/0 |
| Producteur gouverné | 1 | 1 | 1/0 |
| Action auditée (helper inclus) | 13 | 6 | 5/0 |
| Plusieurs producteurs | 12 | 2 | 4/0 |
| Composant stock concret | 32 | 10 | 34/0 |

Les cinq usages de [ContextualUsage](../examples/ContextualUsage.cpp) utilisent les modules
publics. Les deux fonctions de producteurs sont désormais une paire avant/après distincte,
compilée et exécutée. Les mesures du [prototype accepté](contextual-api-study.md) restent
historiques ; cette campagne mesure l'API réalisée.

La revue technique locale constate que les appels diagnostic sont une instruction et que
les appels audit conservent phase, temps et admission au point d'action. Dans le composant
stock, cinq variables locales subsistent avant et après : demande, somme, validité, issue et
diagnostic. La validation et les branches de refus restent visibles. Le chemin bas niveau
reçoit neuf arguments ; `apply` reçoit delta et deux temps, soit trois arguments. Le stock
passe d'une référence externe à un membre possédé par le composant.

Coût de composition, en valeurs à gérer (résultats transitoires des factories exclus ; les
liaisons sont comptées même lorsqu'elles sont directement construites dans un composant) :

| Usage | Avant | Après |
| --- | --- | --- |
| Diagnostic composant | logger | logger, contexte, liaison |
| Diagnostic opération | logger | logger, contexte composant, contexte dérivé, liaison |
| Producteur gouverné | anneau | anneau, contexte, liaison |
| Audit d'action | anneau | anneau, description, contexte, liaison |
| Deux producteurs diagnostic | deux anneaux | deux anneaux, deux contextes, deux liaisons |
| Stock concret | deux anneaux, trois valeurs de contexte/description, compteur | mêmes deux anneaux et trois valeurs, composant avec compteur et deux liaisons |

La simplification déplace donc la capture et sa gestion vers la composition ; elle ne réduit
pas systématiquement le nombre total d'objets. Elle remplace l'assemblage par événement par
des valeurs bornées réutilisées. Une référence empruntée n'assure pas à elle seule la durée
de vie : la responsabilité de l'hôte est décrite et exercée, pas éliminée par le typage.

## Producteurs, consommateurs et arrêt

Le scénario `lifecycle` crée deux composants, chacun avec un anneau diagnostic et un anneau
audit de capacité 4, et des identités de flux distinctes. Des chaînes temporaires alimentent
les factories avant le démarrage ; les liaisons et composants sont copiés dans les producteurs.
Deux `jthread` produisent pendant qu'un seul consommateur d'application draine les quatre
anneaux. Un acquittement atomique par producteur organise la prochaine commande à l'extérieur
du métier ; la méthode d'émission n'attend pas. Le délai de garde arrête l'essai en cas de défaut.
Les tokens d'arrêt permettent aussi aux producteurs de sortir pendant une exception de l'hôte.

200 mutations par producteur donnent 400 diagnostics et 800 événements d'audit. Les multiples
réutilisations des slots vérifient les copies avant acquittement, les séquences propres à
chaque flux, l'alternance des phases et la corrélation. Aucun ordre global entre flux n'est
supposé. Les producteurs sont joints, les derniers drains sont terminés, puis composants,
liaisons et destinations sortent de portée dans cet ordre. Les copies conservées par le
consommateur sont examinées après réutilisation des slots ; aucune vue acquittée n'est utilisée.
Ce scénario exerce une ordonnance particulière et ne remplace pas #120.

## Critères C de #113

| Critère | Preuve et limite |
| --- | --- |
| Diagnostic en une instruction | Les paires composant/opération/gouverné et `InventoryWorker::apply` ; composition séparée. |
| Audit préparé, sémantique et admission visibles | `apply` et `auditAfter`, résultat distinguant mutation et refus ; deux temps et phases explicites. |
| Aucun stockage/ancrage/drain/rétention à l'émission | Les deux méthodes stock et les cinq usages ; drains dans la composition/consommation uniquement. Pilotage de persistance #116 distinct. |
| Avant/après compilés et revue sur composant concret | Six paires mesurées ci-dessus, scénario `equivalence`. Revue technique locale fournie ; application indépendante #122 et revue mainteneur restent dues. |
| Contextes, source, temporaires, refus, troncature, filtres | VER-011–019 et rapport #128/#129 ; scénarios stock et producteurs/consommateur ci-dessus. |
| Aucun résultat inventé à la sortie de portée | `context-integration-interrupted-scope`, destruction après refus d'issue, phases explicitement déclarées par l'hôte. |
| Migration/référence et frontière du cœur | Guide contextualisé, consommateurs source/installés, cinq contrôles `build.core.*` ; aucun changement de module du cœur par ce lot. |

Ces preuves permettent la revue ; elles ne constituent pas une acceptation tacite. GAP-006
reste ouvert jusqu'à la décision mainteneur sur cette révision et la réserve de portée
locale/#122. REQ-009/REQ-011 restent prévues pour l'expérience acceptée complète. Ne pas clore
#113 ou #130, ni réutiliser l'acceptation initiale du dossier comme acceptation de ce lot.

## Campagne et reproduction

Linux x86_64, Clang/libc++ 21.1.8, CMake 4.2.3, Ninja, preset `ninja-clang` Release,
exemples/tests activés, SpecLab épinglé par CMake ; tuple identique à la campagne #128/#129.

```sh
cmake --build --preset ninja-clang --parallel 8
ctest --preset ninja-clang --parallel 8
scripts/check-format.sh
clang-format-21 --dry-run --Werror examples/InventoryWorker.hpp
clang-tidy-21 -p build-clang --warnings-as-errors='*' --quiet examples/ContextualInventory.cpp
clang-tidy-21 -p build-clang --warnings-as-errors='*' --quiet examples/ContextualUsage.cpp
clang-tidy-21 -p build-clang --warnings-as-errors='*' --quiet tests/spec/ContextIntegrationSpec.cpp
python3 scripts/measure-contextual-usage.py
python3 scripts/check-development-file.py
python3 -m unittest discover -s tests/documentation -p 'Test*.py'
git diff --check
```

Construction effective réussie ; **172/172 tests CTest réussis**. Les six ajouts
sont trois scénarios SpecLab et les trois scénarios du composant. `InstallTreeConsumer` et
`InstallTreeCoreConsumer` copient le composant et son header dans leur projet extérieur,
le construisent avec **seulement `mddlog::core`** et exécutent les trois scénarios. Il s'agit
encore de la même application locale, sans indépendance organisationnelle revendiquée.
Les consommateurs précédents et les cinq contrôles du cœur restent réussis.

Formatage réussi sur 79 fichiers du périmètre du script et contrôle explicite du header
applicatif réussi. Analyse ciblée clang-tidy 21 réussie sur **3/3 unités nouvelles/modifiées**, avec le header
analysé via ses inclusions, zéro diagnostic de projet restant. Contrôle structure/références
du dossier réussi, **30/30 tests documentaires réussis** et `git diff --check` réussi. La campagne
complète 70/70 unités de #128/#129 reste historique ; aucune nouvelle analyse complète des
72 unités actuelles n'est déduite de cette analyse ciblée.
