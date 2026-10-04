# ADR-005 : Contextes explicites et liaisons de producteurs

## Status

**Accepted**, 2026-10-04 — lot [#127](https://github.com/ambroise-leclerc/mddlog/issues/127)
de [#113](https://github.com/ambroise-leclerc/mddlog/issues/113), programme
[#112](https://github.com/ambroise-leclerc/mddlog/issues/112).
La base initiale #124 est acceptée sur `develop` (`67a15e8`). Ambroise Leclerc accepte
cette conception et ses exemples à la révision `f3063bc4122cad41f23352721824db9094066b3d`
le 2026-10-04. Les noms d'étude restent ajustables lors de la réalisation #128/#129 ;
l'acceptation de conception ne vaut pas livraison ou qualification de l'API.

## Context

### Inventaire de la v0.2

| Chemin existant | Usage conseillé aujourd'hui | Contexte et coût |
| --- | --- | --- |
| `Log::info` et autres niveaux | Diagnostic global facultatif d'une petite application | Origine transmise depuis l'appelant ; catégorie répétée ; singleton protégé par mutex, initialisation et diagnostic allouants |
| `core::SimpleLogger` (`adapter.logger`) | Diagnostic injecté, sinks objets, mode synchrone ou asynchrone | Nom du logger invariant, catégorie par appel ; `logMedical` reçoit utilisateur/session/dispositif ; sa file asynchrone reste non bornée |
| `adapter::TextLogger` | Diagnostic synchrone à callbacks et groupes de niveaux indépendants | Groupes initialement désactivés ; origine facultative à transmettre ; formatage allouant ; retrait suivant `SinkRegistry` |
| `core::RingLog<N>::tryWrite` | Diagnostic gouverné avec budgets et résultat d'admission | `RecordInput` complet ; temps explicite ; champs possédés bornés ; un producteur par anneau |
| `core::AuditRing<N>::tryRecord` | Audit gouverné, canal distinct | `AuditInput` complet ; identité du flux dans l'anneau ; séquence attribuée à l'admission ; un producteur par anneau |
| `SimpleLogger::logAudit` / `Log::logAudit` | Compatibilité des adaptateurs existants | Liaison empruntée configurée/retirée explicitement, appels sérialisés par mutex ; ne fournit pas un chemin gouverné sans verrou |

Les drains, sinks et primitives de persistance restent des objets de composition. Leur
orchestration relève de #116 ; ces liaisons ne les introduisent pas dans les fonctions métier.
`Log` n'est pas le point d'entrée conseillé pour un producteur soumis aux budgets gouvernés.

### Usages qui motivent la proposition

L'[étude exécutable](../../examples/ContextualUsage.cpp) contient des paires avant/après
pour un composant, une opération corrélée, un producteur gouverné et une action auditée,
puis compose plusieurs producteurs avec des anneaux distincts. Les classes de `study`
sont locales à cet exécutable : elles explorent la forme d'appel, sans étendre la bibliothèque.
Le [rapport](../contextual-api-study.md) précise mesures, vérifications et limites.

## Medical Device Considerations

Un contexte erroné peut attribuer un événement à la mauvaise action ou personne. Un résultat
ignoré peut autoriser une action critique après refus ; un résultat d'issue refusé ne peut pas
annuler une action déjà exécutée. Réduire les répétitions ne doit pas supprimer ces décisions
visibles. L'hôte minimise les identifiants sensibles et détermine sa politique d'échec.
La revue de lisibilité est une revue d'API de composant, sans validation d'aptitude à
l'utilisation d'un dispositif. La conception ne modifie pas les quatre contrats existants.

## Decision

### 1. Injection explicite, contextes possédés

Préparer le contexte et la liaison au point de composition, puis injecter la liaison dans le
composant. Un contexte diagnostic contient `component`, `operationId` et `correlationId`.
`withOperation` produit une nouvelle valeur validée : aucune mutation du parent ni pile globale
ou `thread_local`. Les contextes immuables peuvent être copiés entre threads ; une liaison de
producteur reste réservée à l'unique producteur de son anneau, même si sa valeur est copiable.
Une liaison ne rend pas un anneau SPSC multiproducteur.

La construction copie les identifiants dans du stockage possédé et borné. Elle refuse toute
valeur trop longue, sans troncature d'identité. Les capacités sont celles de `Record` et
`AuditEvent`, sans seconde liste de constantes. L'API finale doit retourner une erreur typée
avec champ et raison ; l'`optional` diagnostic du prototype n'est qu'un raccourci d'étude.
Les vues de paramètres sont lues pendant l'appel ; aucune vue sur un temporaire n'est retenue.
Les vues obtenues sur un contexte sont invalidées à sa destruction ou son remplacement.

### 2. Une liaison par destination, sans synonymes

Le producteur gouverné emprunte directement `RingLog<N>` ou `AuditRing<N>` et possède son
contexte. La liaison ne possède pas l'anneau, le sink ou le consommateur. Interdire la liaison
à un anneau temporaire ; documenter l'ordre de destruction : arrêter le producteur, terminer
le consommateur, détruire les liaisons puis les anneaux. Aucun destructeur n'émet, n'acquitte,
ni ne synchronise. Une référence ne protège pas contre une destruction prématurée par l'hôte.

La construction et l'émission gouvernées sont bornées, sans allocation, exception ni verrou.
Les résultats existants `WriteResult` et `AuditWriteResult` restent visibles et `[[nodiscard]]`.
Éviter un nouvel effacement de type allouant dans le cœur. Un anneau par producteur et canal ;
aucun ordre global entre anneaux. #128/#129 devront mesurer taille et coût des nouvelles
valeurs et passer les contrôles de frontière avec un consommateur limité à `mddlog::core`.
L'exécutable d'étude, qui lie l'adaptateur, ne prouve pas cette frontière.

Pour le diagnostic allouant, une liaison injectée peut utiliser `TextLogger` ou `SimpleLogger`.
Le premier essai utilise `TextLogger` car son filtre `is` est public. `SimpleLogger` conserve
son rôle et ses paramètres ; l'API finale doit préciser une requête de filtre cohérente avant
d'ajouter une fabrique paresseuse (son `shouldLog` est actuellement privé).
Ne pas multiplier les façades globales ; `Log` reste la commodité historique facultative.

### 3. Données par événement et véritable origine

Le temps gouverné reste un `RawTime` obligatoire **par émission**, jamais une heure capturée
avec le contexte. L'hôte prépare son horloge ; aucune lecture d'horloge dans le cœur. Le
prototype accepte explicitement `RawTime::unavailable()` ; il ne prétend pas dater l'action.
Les adaptateurs gardent leur horloge et leur politique existantes.

Pour le diagnostic, le message et le niveau appartiennent à l'événement ; le paramètre
`source_location::current()` est défini sur la méthode publique appelée dans le métier, puis
transmis à chaque couche. Ne pas réutiliser l'origine de construction du contexte. Une
surcouche de l'hôte doit elle aussi recevoir et transmettre la localisation de son appelant.

L'audit v0.2 ne possède **pas** de champ de localisation C++ ou de composant diagnostic. Ses
identités sémantiques sont acteur/cible/corrélation et identité du flux. Cette proposition ne
modifie ni `AuditInput` ni le format canonique pour leur ajouter une source. Si une origine
C++ d'audit devient nécessaire, elle exige une décision de format séparée dans #121.

### 4. Descriptions d'audit de l'hôte et phases explicites

Une description réutilisable appartient à l'hôte : catégorie, action et références facultatives
`requirementRef`/`riskRef`. Le contexte d'opération fournit acteur, cible et corrélation.
La liaison copie les deux ensembles et vérifie exactement les identifiants au moyen du contrat
d'`AuditEvent`, dont cible/action non vides, et refuse le préfixe réservé `mddlog.`. L'identité
de flux appartient toujours à `AuditRing`, distincte à chaque instance/session de démarrage.
Une identité valide syntaxiquement n'en prouve pas l'unicité.

Dans le prototype, une description est un `AuditInput` de l'hôte complété à la composition.
Seuls catégorie/action/acteur/cible/références/corrélation sont retenus par la liaison ; les
champs phase/temps/détail/sourceSequence de cet agrégat ne sont pas des invariants. #129 doit
leur donner des options nommées distinctes pour éviter cette ambiguïté. Sa validation doit
rester accessible sans dépendre d'un adaptateur. Aucun vocabulaire métier universel n'est ajouté.

Chaque appel d'audit porte phase, temps, détail éventuel et `sourceSequence` éventuelle.
`Requested`, `Confirmed`, `Executed` et `Failed` sont toujours explicites. La bibliothèque
n'exécute pas l'action ni n'impose une machine d'états universelle. L'hôte peut partager une
politique de refus, mais la décision sur le refus critique reste visible au point d'action.
L'admission garantit uniquement la copie en mémoire : elle ne confirme pas l'exécution, la
transmission ou la durabilité. Une exception ou une sortie de portée ne fabrique aucune phase.
Le détail seul peut être tronqué, avec le résultat existant.

### 5. Filtre avant fabrique de message

Un argument ordinaire tel que `info(expensive())` est évalué avant l'entrée dans la méthode,
même si le niveau est désactivé. Pour le diagnostic d'adaptateur, proposer un appel paresseux
explicite, illustré par `debugLazy(factory)`. La fabrique n'est appelée qu'après une première
lecture du filtre et au plus une fois ; la liaison conserve le résultat jusqu'à sa copie ou
son rendu. Les exceptions de la fabrique suivent le contrat allouant de l'adaptateur.

Une désactivation concurrente peut survenir après cette lecture : une fabrique peut alors
être exécutée sans livraison finale. Il n'existe pas de transaction sur la reconfiguration.
Un argument permettant de créer la fabrique reste évalué avant le filtre : utiliser une lambda
à captures peu coûteuses. Aucun filtre de sévérité ne s'applique à l'admission d'audit.

Le diagnostic gouverné conserve pour ce lot son appel à texte déjà préparé. Une éventuelle
fabrique gouvernée doit fournir du stockage borné et être `noexcept`, sans transférer un
`std::format` ou une fabrique allouante dans le cœur ; elle ne constitue pas une exigence de
formatage sans allocation dans tous les adaptateurs.

## Alternatives Considered

- **Tout passer par `Log`** : appels courts, mais contexte partagé implicite et mutex ; ne
  satisfait pas la séparation et l'indépendance requises pour le producteur gouverné.
- **Portée globale/thread-local** : réduit quelques arguments mais introduit propagation
  cachée, confusion entre opérations et restauration ; le contexte par valeur est plus explicite.
- **Constructeur fluide et macros** : complexité d'un second langage, origine difficile à
  conserver ; agrégats nommés et petites méthodes suffisent aux cinq usages.
- **Un logger unique pour audit et diagnostic** : masque canaux et résultats distincts ; les
  liaisons partagent des règles de capture, pas une opération d'émission universelle.
- **RAII d'action auditée** : la destruction ne révèle pas l'issue métier ; les phases explicites
  restent nécessaires même si leur séquence prend davantage de lignes.
- **Modifier SPSC pour partager une liaison** : nécessite un autre contrat et d'autres preuves ;
  la composition de plusieurs anneaux suffit à ce lot.
- **Tout implémenter dans #113 sans sous-issues** : une seule revue mélangerait décision,
  diagnostic allouant, nouveaux budgets gouvernés et preuves. La revue préalable requise par
  #113 impose déjà une frontière de livraison ; quatre lots permettent des critères vérifiables.

## Consequences

| Lot | Dépendance | Livrable et limite |
| --- | --- | --- |
| [#127](https://github.com/ambroise-leclerc/mddlog/issues/127) | #124 accepté | Cette conception et les usages compilables, puis décision nominative |
| [#128](https://github.com/ambroise-leclerc/mddlog/issues/128) | Conception #127 acceptée | Diagnostic contextualisé, filtrage et migration ; tests et budget gouverné |
| [#129](https://github.com/ambroise-leclerc/mddlog/issues/129) | Conception #127 acceptée | Descriptions/contextes d'audit ; refus, temps, durées de vie et phases explicites |
| [#130](https://github.com/ambroise-leclerc/mddlog/issues/130) | #128 et #129 | Intégration, revue de lisibilité, consommateurs sources/installés et preuves de clôture |

Chaque lot actualise le dossier et ses tests. #116 conserve le pilotage ; #117/#118 les budgets
d'exploitation ; #120 la robustesse ; #121 le gel final ; #122 la qualification de l'application
indépendante. Les prototypes utilisent des enregistrements complets pour posséder leurs champs :
une implémentation finale peut réduire cette taille sans changer la forme d'appel proposée.

## References

[ADR-001](ADR-001-allocation-free-governed-logging-core.md),
[ADR-002](ADR-002-regulatory-audit-event-model.md),
[ADR-003](ADR-003-application-integration-and-sink-ownership.md),
[ADR-004](ADR-004-audit-persistence-and-tamper-evidence.md),
[registre unique](../../software_development_file/register.json) : REQ-009, REQ-011,
GAP-006, RISK-008/009/010. Aucune dépendance nouvelle n'est introduite.

## Approval

Ambroise Leclerc accepte explicitement la conception le 2026-10-04 (réponse : « oui »
à la demande d'acceptation de l'ADR-005 pour engager #128 et #129). Révision examinée :
`f3063bc4122cad41f23352721824db9094066b3d`, incluant l'étude et les résultats locaux.
Aucune réserve supplémentaire n'est exprimée ; les limites de l'étude et les qualifications
futures restent celles du rapport. Cette décision porte sur la conception du composant,
sans revue indépendante revendiquée ni acceptation de risques de dispositif. #128/#129
peuvent démarrer ; le gel final reste #121 et l'acceptation des preuves de publication #122.

## Réalisation #128/#129

Les [modules et preuves](../contextual-api-validation.md) et la
[référence/migration](../migration/contextual-logging.md) concrétisent les signatures.
Les factories renvoient `std::expected` avec erreurs typées ; cette décision de construction
est distincte des résultats d'émission, conservés sous `WriteResult`/`AuditWriteResult`.
Le cœur ne fait appel à aucun accesseur levant d'`expected`. DiagnosticContext ne stocke
que composant/opération/corrélation ; AuditDescription et AuditContext possèdent des options
séparées. GovernedBinding et AuditBinding empruntent leurs anneaux. DiagnosticBinding,
unique template d'adaptateur, projette vers les deux loggers historiques. L'étude actuelle
utilise ces modules ; la révision de conception approuvée reste le SHA indiqué dans Approval.
