# Contextes et liaisons — API de développement 1.0 et migration v0.2

La conception [ADR-005](../adr/ADR-005-contextual-logging-api.md) est acceptée. Les modules
ci-dessous sont implémentés par #128/#129 sur la branche de développement ; le gel public
final relève de #121. Le [rapport de vérification](../contextual-api-validation.md) distingue
ces résultats du profil de déploiement restant à qualifier.

## Choisir le chemin

| Besoin | Objet injecté | Module direct | Cible CMake |
| --- | --- | --- | --- |
| Diagnostic à budget gouverné | `core::GovernedBinding<N>` | `mddlog.core.governedbinding` | `mddlog::core` |
| Audit à admission explicite | `core::AuditBinding<N>` | `mddlog.core.auditbinding` | `mddlog::core` |
| Diagnostic à callbacks synchrones | `adapter::DiagnosticBinding<TextLogger>` | `mddlog.adapter.diagnosticbinding` | `mddlog::mddlog` |
| Diagnostic avec sinks objets | `adapter::DiagnosticBinding<core::SimpleLogger>` | `mddlog.adapter.diagnosticbinding` | `mddlog::mddlog` |

`import mddlog;` expose également les contextes et liaisons dans `mddlog`, ainsi que
`Refusal`, `RefusalReason`, `IdentifierField`, `RingLog`, `WriteResult`, `Admission` et
`TruncatedFields` pour nommer les refus et les résultats du diagnostic gouverné.
Le consommateur `tests/consumer/ContextAdapterConsumer.cpp` vérifie ce chemin sans import
direct du cœur, dans les arbres source et installé. Une application
qui importe directement un module `mddlog.core.*` et utilise aussi les adaptateurs lie
**les deux cibles** explicitement, comme le précise le [guide du cœur](governed-core.md).
Les chemins `Log`, `SimpleLogger`, `TextLogger`, `RecordInput` et `AuditInput` restent utilisables.

## Référence des valeurs et factories

| Valeur possédée | Options copiées | Création et résultat |
| --- | --- | --- |
| `DiagnosticContext` | `DiagnosticContextOptions{component, operationId, correlationId}` | `create(options)` → `expected<DiagnosticContext, Refusal>` |
| Opération dérivée | `OperationContextOptions{operationId, correlationId}` | `parent.withOperation(options)` → même résultat ; composant conservé |
| `AuditDescription` | `AuditDescriptionOptions{category, action, requirementRef, riskRef}` | `create(options)` → `expected<AuditDescription, AuditRefusal>` |
| `AuditContext` | `AuditContextOptions{actor, target, correlationId}` | `create(options)` → `expected<AuditContext, AuditRefusal>` |

Ces factories sont `constexpr`, `noexcept` et `[[nodiscard]]`. Le résultat doit être testé
avant `*result` ou `result.error()`. Le cœur utilise `std::expected` pour **la construction**,
avec valeurs inline et sans appel à ses accesseurs levants ; les résultats d'émission restent
`WriteResult` et `AuditWriteResult`. Un hôte gouverné n'utilise pas `value()` pour contourner
le contrôle d'erreur. Les capacités restent celles de `Record` et `AuditEvent`.

Les accesseurs portent les noms des champs et retournent des vues vers les octets possédés.
Ces vues exigent que la valeur existe et ne soit pas remplacée. Toute copie possède ses
propres octets ; les factories ne retiennent pas les vues entrantes, y compris vers des
chaînes temporaires. Les valeurs n'ont pas de constructeur public par défaut.

Le diagnostic refuse les identifiants trop longs dans l'ordre composant, opération, corrélation.
Les champs d'audit suivent exactement les règles ASCII d'`AuditEvent::validateIdentifier` :
action et cible obligatoires ; acteur, corrélation et références facultatifs. Une description
pour producteur refuse aussi le préfixe réservé `mddlog.`. La validation de la description
porte sur ces identifiants : `category` est copiée sans contrôle de plage, comme dans
`AuditEvent`/`AuditRing`. L'hôte fournit une valeur nommée d'`AuditCategory` et valide toute
conversion depuis une entrée externe ; une valeur forcée hors plage n'est pas refusée
par cette factory. Cette limite est héritée de l'admission existante.
L'hôte conserve son vocabulaire,
la minimisation des données et l'unicité des identités de flux entre producteurs/démarrages.

## Diagnostic : préparer une fois, émettre au véritable appelant

```cpp
import std;
import mddlog.core.governedbinding;
using namespace mddlog::core;

const auto component = DiagnosticContext::create({.component = "pump"});
// À la composition : traiter l'erreur avant de construire la liaison.
if (!component) return 1;
const auto operation = component->withOperation({.operationId = "prime", .correlationId = "call-7"});
if (!operation) return 2;
RingLog<8> ring;
GovernedBinding logger(ring, *operation);

const auto result = logger.info(RawTime::unavailable(), "Ready");
// L'hôte décide quoi faire d'un refus ou d'une troncature.
if (result.admission() == Admission::Refused) return 3;
```

`GovernedBinding` possède son contexte et emprunte l'anneau. `log(level, time, message, location)`
et `trace/debug/info/warn/error/fatal(time, message, location)` renvoient `WriteResult`.
Chaque méthode courte reçoit une `source_location::current()` au véritable appelant et la
transmet à l'anneau. Le temps est explicite **à chaque événement** ; aucune horloge ni filtre
implicite n'est ajouté au producteur gouverné. Le message est copié puis éventuellement
tronqué comme dans `RingLog`; contexte et résultats conservent leurs contrats existants.

Pour le diagnostic d'adaptateur, `DiagnosticBinding(destination, context)` déduit le type du
logger. Les méthodes courtes prennent seulement le message et une localisation facultative.
`log(level, message, location)` est le chemin générique. `SimpleLogger` reçoit catégorie,
opération et corrélation dans ses champs structurés ; `TextLogger` rend le préfixe
`[component:operation:correlation]`, y compris les champs vides : un contexte vide rend
`[::]`. Ce format fixe est volontaire et ne supprime pas sélectivement les séparateurs.
Les adaptateurs gardent leur horloge et leurs coûts
allouants. La liaison ne change pas les politiques des sinks ni la file de `SimpleLogger`.

La surcharge avancée est `SimpleLogger::log(level, context, message, location)` : le contexte
occupe un paramètre distinct pour préserver l'ancien `log(level, message, {})`. Le contexte
n'est pas projeté dans `userId/sessionId/deviceId` ; `logMedical` garde ses champs spécifiques.

## Construire les messages après le filtre

```cpp
// Avec une liaison d'adaptateur préparée hors de la fonction métier :
logger.debugLazy([&] { return std::format("state={}", readState()); });
logger.logLazy(LogLevel::Info, [&] { return makeDetailedMessage(); });
```

`is(level)` expose un instantané du filtre existant : groupes de `TextLogger`, activation et
seuil de `SimpleLogger`. Rendre `SimpleLogger::is` public est un ajout d'API volontaire,
pour consulter ce filtre avant la construction du message ; il n'ajoute aucune garantie
transactionnelle. `debugLazy` est le seul raccourci paresseux par niveau, pour le cas usuel
du diagnostic coûteux. Les autres niveaux utilisent `logLazy(level, factory)` : cette
asymétrie est volontaire pour garder une seule entrée générique sans six synonymes publics.
`logLazy`/`debugLazy` invoquent la fabrique synchrone au plus une fois,
après cet instantané, et conservent son résultat jusqu'à la copie/rendu. Une vue retournée
par la fabrique doit rester valide pendant cette copie ; retourner une `std::string` temporaire
est sûr. Une désactivation concurrente peut supprimer la livraison après construction.
Les exceptions de la fabrique sont propagées, sans créer d'enregistrement.

Un argument ordinaire, `logger.info(expensive())`, et les expressions de capture de la lambda
sont évalués avant le filtre. Le producteur gouverné ne propose pas de fabrique allouante ;
il reçoit le texte préparé selon la politique de l'hôte. Aucun filtre diagnostique ne bloque l'audit.

## Audit : description et contexte distincts des faits de l'événement

```cpp
import std;
import mddlog.core.auditbinding;
using namespace mddlog::core;

const auto description = AuditDescription::create({.category = AuditCategory::Operator,
                                                   .action = "pump.prime"});
const auto context = AuditContext::create({.actor = "operator-1", .target = "pump-1",
                                           .correlationId = "call-7"});
if (!description || !context) return 1;
AuditRing<8> ring("pump:boot-1");
AuditBinding audit(ring, *description, *context);

const auto request = audit.record(AuditPhase::Requested, RawTime::unavailable());
if (!request.wasAdmitted()) return 2; // La politique de l'hôte bloque ici l'action.
// L'hôte exécute l'action et observe réellement son résultat.
const auto outcome = audit.record(AuditPhase::Executed, RawTime::unavailable(),
                                  {.detail = "host-observed outcome", .sourceSequence = request.sequence()});
if (!outcome.wasAdmitted()) return 3; // L'action a déjà eu lieu ; organiser la réponse à ce refus.
```

`AuditBinding::record(phase, time, AuditEventOptions{detail, sourceSequence})` renvoie
`AuditWriteResult`, toujours `[[nodiscard]]`. Phase et temps n'ont pas de valeur par défaut.
Les données propres à l'émission sont copiées synchroniquement par l'anneau ; seul le détail
peut être tronqué, avec indicateur dans le résultat. Un anneau à identité invalide retourne
`InvalidStream` à l'émission. La construction de la liaison ne consomme aucune séquence.

`Requested`, `Confirmed`, `Executed` et `Failed` restent des déclarations explicites de l'hôte.
La liaison n'exécute pas l'action et son destructeur n'émet rien. Une exception ou un retour
anticipé n'invente aucune issue. L'admission est seulement une copie en mémoire, distincte de
transmission, ancrage et durabilité. L'audit n'acquiert pas une localisation C++ ou un nouveau
format canonique. Les [exemples exécutables](../../examples/ContextualUsage.cpp) montrent
une politique hôte avec refus de demande et refus de résultat après exécution.

## Durée de vie et plusieurs producteurs

Arrêter les producteurs, terminer les consommateurs, puis détruire liaisons et anneaux.
Les destinations doivent survivre à **toutes les copies** des liaisons. La liaison à une
destination temporaire est refusée au typage ; elle ne prévient pas une destruction prématurée
par l'hôte. Les contextes peuvent être partagés en lecture ou copiés, mais les liaisons
n'autorisent pas plusieurs producteurs concurrents sur le même anneau : un anneau par
producteur et canal, sans ordre global entre anneaux. Drains et persistance restent hors métier.

`Log` est une commodité globale facultative. Si l'hôte injecte une liaison vers `*Log::getLogger()`,
il conserve le `shared_ptr` aussi longtemps que la liaison : celle-ci n'acquiert aucune propriété
partagée de la destination. Le chemin audit gouverné conseille la liaison directe à `AuditRing`,
plutôt que la sérialisation par mutex de `SimpleLogger::logAudit`.

## Intégrer un composant et consommer hors métier

[InventoryWorker](../../examples/InventoryWorker.hpp) et sa [composition exécutable](../../examples/ContextualInventory.cpp)
montrent une migration complète d'une modification de stock. `adjustBefore` assemble les champs
à chaque événement ; `apply` reçoit delta et temps, puis conserve demande, mutation et refus
d'issue comme faits distincts. Le composant possède deux liaisons, avec des capacités de
diagnostic et d'audit indépendantes ; les anneaux restent possédés par la composition.

L'application consommatrice copie les enregistrements avant d'acquitter les vues et ne relit
pas celles-ci ensuite. Pour plusieurs producteurs, elle crée un couple d'anneaux et une
identité de flux par producteur. Elle arrête et joint les threads, termine les derniers
drains, puis détruit composants/liaisons et enfin anneaux. Les copies de liaisons utilisées
par les threads exigent elles aussi cette durée de vie. La saturation d'une demande audit
bloque ici la mutation ; celle du résultat après mutation exige une réaction hôte distincte.

Reproduction : `ctest --preset ninja-clang -R 'examples.contextual' --output-on-failure`.
Le [rapport d'intégration](../contextual-api-integration.md) mesure les usages, énonce chaque
critère de #113 et distingue la démonstration locale de l'application indépendante #122.
