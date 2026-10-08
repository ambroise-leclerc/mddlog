# Migration du diagnostic v0.2 vers les budgets (#118)

Les méthodes `info`, `warn`, `error`, `log`, `logMedical`, `flush`, les contextes et la source
appelante conservent leurs signatures. Les constructeurs `(nom, bool async)` fonctionnent avec
les budgets par défaut. Le changement observable est le refus mesuré sous saturation, plutôt
qu'une croissance sans limite ; les messages trop longs sont également refusés.

Avant, la composition utilisait `Log::initialize("pump")`. Après :

```cpp
import mddlog;
import mddlog.log;
Log::initialize("pump", DiagnosticConfig{
    .messageCapacity = 64, .flushCapacity = 2,
    .maxRecordBytes = 256, .sinkCapacity = 4});
```

Le code métier conserve exactement `Log::info("Operation started", "pump")`. La configuration
précède le premier usage : une instance existante n'est pas reconfigurée. Pour une injection
locale : `SimpleLogger destination("pump", DiagnosticConfig{...});`, puis le même
`DiagnosticBinding<SimpleLogger>`. [L'exemple compilé](../../examples/BoundedDiagnostic.cpp)
regroupe configuration, flush avec délai, santé et arrêt hors fonction métier.

Le superviseur consulte `destination.health()` ou `Log::health()` (optionnel, sans initialisation
implicite), et traite `flushChecked` / `flushFor` selon le statut. Il choisit ses budgets et sa
réaction aux pertes ; `Fatal` ne contourne pas la capacité. `tryLog` permet une admission explicite
sans modifier tous les appels métier. `flush()` seul ne retourne toujours aucun résultat.
Un `Timeout` ne libère pas la commande et ne signifie pas que le sink est interrompu.

`SimpleLogger::shutdown` ferme les handles retenus ; `Log::shutdown` détache l'instance et ferme
également leurs admissions. Comme auparavant, un appel ultérieur à la façade peut initialiser
une nouvelle instance : arrêter les producteurs avant l'arrêt global. Un appel global concurrent
peut déjà avoir obtenu un handle ou créer la nouvelle instance après détachement. Les handles
retiennent la propriété mais ne permettent plus de diagnostiquer après la fermeture de l'ancien
logger. La liaison audit est effacée par l'arrêt global, conformément au contrat existant.

Les sinks objets sont appelés sans verrou de registre. Retrait/clear n'attendent pas la quiescence
d'un snapshot engagé ; leur shared_ptr conserve la durée de vie. Utiliser les handles quiescents
de `SinkRegistry` pour des callbacks empruntant des ressources nécessitant ce contrat. Les
émissions et flush/arrêt réentrants sont refusés ; la santé reste disponible dans le callback.
L'arrêt global depuis un callback est sans effet : le demander au superviseur.

Le [contrat détaillé](../diagnostic-budgets.md) précise plafonds, ordre, erreurs, limites des délais
et obligation de retour des sinks hôte. Pour le diagnostic critique sans allocation, migrer
vers le [cœur gouverné](governed-core.md). L'audit conserve son anneau et sa supervision distincts.
