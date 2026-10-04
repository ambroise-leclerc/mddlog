# mddlog

**Bibliothèque de logging C++23** pour les dispositifs médicaux critiques et les logiciels réglementés ou safety-critical.

🇫🇷 Français · [🇬🇧 English](README.en-GB.md) · [🇪🇸 Español](README.es.md)

```cpp
import std;
import mddlog;

int main() {
    // Configuration au point de composition de l’application.
    mddlog::SimpleLogger destination("pump", false);
    destination.addSink(mddlog::createConsoleSink());
    const auto context = mddlog::DiagnosticContext::create({
        .component = "pump", .operationId = "prime", .correlationId = "call-7"
    });
    if (!context) return 1; // Traiter le refus de construction avant de créer la liaison.
    mddlog::DiagnosticBinding logger(destination, *context);

    // Dans le code métier : le contexte n’est plus répété.
    logger.info("Pompe prête");
    logger.error("Occlusion détectée sur la voie A");
    logger.debugLazy([] { return std::format("state={}", 42); });
}
```

- **Logs de diagnostic** : niveaux classiques, sinks et façade `Log`, prêts à l'emploi.
- **Cœur borné, sans allocation** : capacité fixe, refus explicite plutôt qu'écrasement silencieux.
- **Journal d'audit** : événements distincts des logs, persistés, chaînés et vérifiables contre un ancrage indépendant.

[Décisions d'architecture](docs/adr/README.md) · [Validation de la persistance d'audit](docs/audit-persistence-validation.md) · [Guide d'admission d'audit](docs/migration/audit-admission.md) · [Guide du registre d'audit](docs/migration/audit-ledger.md) · [Preuves du cœur gouverné](docs/governed-evidence.md) · [Journal des modifications](CHANGELOG.md) · [Versions](https://github.com/ambroise-leclerc/mddlog/releases)

## Nouveautés de la version 0.3.0

La v0.3.0 livre l’API contextualisée de [#113](https://github.com/ambroise-leclerc/mddlog/issues/113), conçue dans [ADR-005](docs/adr/ADR-005-contextual-logging-api.md). La configuration reste au point de composition ; les fonctions métier reçoivent une liaison réutilisable.

- **Diagnostic** : `DiagnosticContext` possède composant, opération et corrélation ; `DiagnosticBinding` conserve la source du véritable appelant avec `SimpleLogger` ou `TextLogger`.
- **Messages coûteux** : `debugLazy(factory)` et `logLazy(level, factory)` construisent le message après le filtre de l’adaptateur.
- **Chemins gouvernés** : `GovernedBinding` conserve le temps explicite et `WriteResult` ; `AuditDescription`, `AuditContext` et `AuditBinding` séparent les invariants des phases et faits de chaque émission.
- **Preuves et migration** : cinq usages avant/après, composant stock local et consommateurs source/installés. L’intégration locale est acceptée ; l’application indépendante #122 et le gel 1.0 #121 restent ouverts.

Les API bas niveau et la façade globale `Log` restent disponibles. Voir le [guide des contextes](docs/migration/contextual-logging.md) et les [preuves d’intégration](docs/contextual-api-integration.md).

## Capacités livrées en 0.2.0

La v0.1.0 livrait un cœur de journalisation borné, sans allocation (ADR-001). La v0.2.0 y ajoute trois capacités complètes.

### 1. Des événements d'audit distincts des journaux de diagnostic — [ADR-002](docs/adr/ADR-002-regulatory-audit-event-model.md)

- **`AuditEvent`** porte des identifiants exacts (action, acteur, cible, références d'exigence et de risque, corrélation), une catégorie, une phase (demandé, confirmé, exécuté, échoué), une heure fournie par l'hôte et une identité de flux. Un identifiant invalide est refusé, jamais tronqué ; seul le texte `detail` peut être raccourci, avec un indicateur.
- **`AuditRing<N>`** admet un événement en mémoire bornée et lui attribue un numéro de séquence par flux. Plein, il refuse au lieu d'écraser, sans consommer de séquence.
- **`AuditSinkAdapter`** transmet les événements admis à un `AuditSink`, ne libère que ce qui a été accepté, garde le reste pour une nouvelle tentative, et publie sa propre santé : transmis, échecs, pertes après admission, événements en attente.
- **`logAudit(AuditInput)`** sur `SimpleLogger` et `Log` renvoie le résultat d'admission, quels que soient les filtres de diagnostic.

### 2. Les briques d'intégration dans une application — [ADR-003](docs/adr/ADR-003-application-integration-and-sink-ownership.md)

- **`SinkRegistry`** : inscription synchronisée de rappels par poignées explicites, avec retrait sûr, y compris depuis le rappel lui-même.
- **`TransportConsumer`** : consommation de files bornées sur un seul thread, santé indépendante, isolement d'un transport défaillant, et suppression des boucles de journalisation réentrantes.
- **`TextLogger`** : journal texte de diagnostic, groupes activables indépendamment, source de l'appelant et vidages séparés.
- WebFront les adopte et les vérifie dans son propre dépôt.

### 3. La persistance d'audit, avec détection d'altération par ancrage — [ADR-004](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md)

- **Format canonique et chaînage** : chaque événement est encodé selon un contrat d'octets versionné et chaîné par SHA-256 au sein de son flux. Des vecteurs de référence fixent chaque octet, et chaque chaîne d'outils de la CI les vérifie.
- **Stockage et confirmation durable** : `PersistingAuditSink` écrit des segments en ajout seul sur un `StorageMedium`. Un événement n'est déclaré « confirmé durablement » qu'après un `sync` que le support a confirmé, pour un préfixe complet du flux. Toute défaillance du stockage est visible dans la santé du sink, jamais présentée comme un événement d'audit.
- **Ancrage et vérification** : un `AnchorProvider`, extérieur au journal, conserve la position et le condensat atteints. `LogVerifier` relit tout le support et rend, pour chaque flux, un verdict explicite : *Anchored* avec sa plage, *Unanchored*, *anchor unavailable*, *Incomplete*, *Altered*, *Rolled back*, *Conflict*, *Retired*, *Cannot verify* ou *Inconsistent*. Jamais un simple « valide ».
- **Retour arrière détecté** : la position que le lecteur retient hors de l'appareil (`RetainedPosition`) révèle la restauration conjointe d'un ancien journal et de son ancien ancrage.
- **Redémarrage, rotation et rétention** : un registre consigne chaque démarrage, chaque flux ouvert ou fermé et chaque retrait. La reprise recalcule l'état de chaîne et le compare à l'ancrage. La rotation retire des segments entiers, jamais au-delà de l'ancrage, après avoir confirmé durablement sa trace. Le lecteur rapporte chaque redémarrage, chaque retrait et chaque reste d'une coupure comme une frontière, jamais comme une continuité.
- **Validation de bout en bout** : chaque scénario de validation d'ADR-004, de la réécriture avec recalcul des condensats à la coupure d'alimentation pendant un `sync`, est exécuté du producteur jusqu'au rapport du lecteur. Le [rapport de validation](docs/audit-persistence-validation.md) fait correspondre chaque critère à ses tests et énonce les limites.

### Changements incompatibles

- `LogLevel::Audit` et les anciennes surcharges positionnelles de `logAudit(...)` sont supprimés : utilisez `logAudit(AuditInput)` ([guide](docs/migration/audit-admission.md)).
- Les actions commençant par `mddlog.` sont réservées au registre de persistance : l'admission d'un producteur les refuse (`AuditRefusalReason::ReservedAction`).

## Ce que fournit chaque chemin

| Chemin | Ce qu'il fournit | Limite à connaître |
| --- | --- | --- |
| Cœur de diagnostic gouverné (`mddlog::core`) | `GovernedRecord` à capacité fixe et `RingLog` un producteur / un consommateur ; heure fournie par l'hôte ; résultats explicites d'admission et de troncature | Ni sink, ni persistance, ni garantie temporelle à l'échelle du système |
| Cœur d'audit (`mddlog::core`) | `AuditEvent` et `AuditRing` bornés ; identifiants exacts, catégorie et phase, identité de flux et séquence attribuée | Admis veut dire en mémoire ; un anneau plein refuse les nouveaux événements |
| Contextes et liaisons (`mddlog::core`, adaptateurs via `mddlog::mddlog`) | `DiagnosticContext`, `GovernedBinding`, `AuditDescription`, `AuditContext`, `AuditBinding` et `DiagnosticBinding` | Contextes possédés, destinations empruntées ; refus et temps gouvernés explicites ; un producteur par anneau |
| API d'audit publique (`mddlog::mddlog`) | `SimpleLogger::logAudit(AuditInput)` et `Log::logAudit(AuditInput)` renvoient le résultat d'admission après `setAuditRing()` | L'anneau lié doit survivre à sa liaison ; `Log::shutdown()` efface la liaison |
| Transmission d'audit (`mddlog::mddlog`) | `AuditSinkAdapter` transmet à un `AuditSink`, publie sa santé et acquitte ce qui a été accepté | Un seul thread consommateur ; l'acceptation par un sink n'est pas un stockage durable |
| Persistance d'audit (`mddlog::mddlog`) | `PersistingAuditSink` stocke, confirme durablement, tient un registre, fait tourner et retient ; `LogVerifier` rend un verdict par flux | La durabilité dépend du support ; la détection d'altération dépend d'un ancrage indépendant ; rien n'est signé |
| Intégration applicative (`mddlog::mddlog`) | `SinkRegistry`, `TransportConsumer` et `TextLogger` | `TextLogger` rend et rappelle de façon synchrone, avec allocation |
| Adaptateur de diagnostic (`mddlog::mddlog`) | `SimpleLogger`, `LogRecord`, `ConsoleSink` et la façade `Log` | Sa file asynchrone alloue et n'est pas bornée : ce n'est pas le chemin gouverné |

Chaque anneau a **un producteur et un consommateur**. L'hôte fournit une identité de flux distincte pour chaque producteur et chaque session de démarrage, et décide de sa réponse à un refus, à un échec de transmission ou à une coupure d'alimentation.

## Prise en main

### Admettre un événement d’audit avec un contexte réutilisable

```cpp
import mddlog.core.auditbinding;

using namespace mddlog::core;

AuditRing<64> audit{"device_789:boot_42:operator"};
const auto description = AuditDescription::create({
    .category = AuditCategory::RiskControl,
    .action = "EMERGENCY_SHUTDOWN",
    .riskRef = "RISK_17",
});
const auto context = AuditContext::create({
    .actor = "operator_42", .target = "pump_7",
    .correlationId = "device_789:boot_42:input:41",
});
if (!description || !context) {
    // Traiter les identifiants refusés avant toute émission.
    return 1;
}
AuditBinding events(audit, *description, *context);

const auto result = events.record(AuditPhase::Requested, RawTime::unavailable(),
                                 {.detail = "Shutdown requested", .sourceSequence = 41});
if (!result.wasAdmitted()) {
    // Appliquer la politique de refus de l’hôte ; consulter result.refusal().
    return 2;
}
```

La description et le contexte sont copiés ; la liaison emprunte l’anneau. Phase et heure restent explicites à chaque appel. `Requested` déclare une demande : la liaison n’exécute pas l’action et son destructeur n’émet rien. Une admission est une copie en mémoire, pas une confirmation durable.

### Persister et vérifier

Le support et le fournisseur d'ancrage sont fournis par l'intégrateur : le support doit respecter le contrat de stockage d'ADR-004 (décision 9), et le fournisseur doit être indépendant du journal (décision 7).

```cpp
import mddlog.adapter.auditdrain;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditlogverifier;

using namespace mddlog::adapter;

MyStorageMedium medium;    // implémente StorageMedium
MyAnchorProvider provider; // implémente AnchorProvider, hors du journal

StorageConfig config;
config.segmentSize        = 4096;
config.segmentCount       = 256;
config.maxProducerStreams = 4;
config.ledger             = LedgerConfig{.streamId = "device_789:boot_42:ledger"};
config.provider           = &provider;

auto sink = PersistingAuditSink::create(medium, std::move(config));
if (!sink) {
    // Configuration refusée : consulter sink.error().
}

AuditSinkAdapter adapter;
(void)adapter.addRing(audit); // l'AuditRing de l'exemple précédent
adapter.setSink(*sink);
(void)adapter.drainOnce();    // sur le thread consommateur
(void)(*sink)->advanceAnchor("device_789:boot_42:operator");

RetainedPosition retained; // conservée par le lecteur, hors de l'appareil
LogVerifier verifier{medium, provider, retained};
const LogReport report = verifier.verify();
```

Un consommateur CMake qui n'a besoin que du cœur se lie à `mddlog::core` ; pour l'audit transmis et persisté, la journalisation de diagnostic et l'intégration, à `mddlog::mddlog`. La cible complète dépend du cœur ; le cœur n'importe ni adaptateur ni sink.

```cmake
add_subdirectory(mddlog)
target_link_libraries(your_target PRIVATE mddlog::mddlog)
```

Pour aller plus loin : [admission et transmission d'audit](docs/migration/audit-admission.md), [registre, redémarrage, rotation et rétention](docs/migration/audit-ledger.md), [journal texte](docs/migration/text-logger.md), [consommateur de transport](docs/migration/transport-consumer.md), [cœur gouverné](docs/migration/governed-core.md), et l'exemple [basic_usage.cpp](examples/basic_usage.cpp).

## Ce que la persistance garantit, et ce qu'elle ne garantit pas

- **Détection d'altération, relative à un ancrage.** Une réécriture, une troncature ou une restauration ancienne est signalée lorsqu'elle touche des enregistrements que couvre un ancrage indépendant, ou une position que le lecteur a retenue. Le journal reste modifiable par quiconque peut écrire le support : mddlog ne l'empêche pas.
- **Au-delà du dernier ancrage, rien n'est détectable.** Les enregistrements postérieurs ne sont que cohérents entre eux, et le rapport le dit.
- **Aucune preuve d'auteur.** Rien n'est signé : le chaînage montre la cohérence, pas l'identité de l'auteur. Signature, gestion des clés et format d'export sont différés (ADR-004, décision 11).
- **La bibliothèque ne livre ni support ni fournisseur réels.** `InMemoryStorageMedium` et `InMemoryAnchorProvider` sont des doubles de test. Qualifier un support (fichier, flash, journal matériel) et un fournisseur adaptés revient à l'intégrateur.
- **Un seul thread consommateur** pour l'adaptateur d'audit, le consommateur de transport et le sink persistant.

Le [rapport de validation](docs/audit-persistence-validation.md) détaille la couverture démontrée et les limites.

## Contexte réglementaire et preuves

Ces normes décrivent des processus et des responsabilités du développement de dispositifs médicaux. mddlog fournit des briques et un matériau de revue ; l'utiliser n'établit la conformité à aucune norme ni réglementation, et la bibliothèque n'est ni certifiée ni validée pour un dispositif donné. Chaque fabricant l'évalue dans son système, sa gestion des risques, son cycle de vie logiciel et son système qualité.

| Référence | Matériau du projet | Ce qui reste au fabricant |
| --- | --- | --- |
| [IEC 62304:2006 + AMD1:2015](https://webstore.iec.ch/en/publication/22790), cycle de vie du logiciel de dispositif médical | [ADR](docs/adr/README.md), sources versionnées, [tests](tests/), [contrôles du cœur gouverné](docs/governed-evidence.md) et [validation de la persistance](docs/audit-persistence-validation.md) | Activités du cycle de vie, vérification et validation du système, gestion de configuration et résolution de problèmes |
| [ISO 14971:2019](https://www.iso.org/standard/72704.html), gestion des risques | Références `riskRef` et `requirementRef` sur `AuditEvent` ; résultats explicites de refus, de transmission et de vérification | Analyse des dangers, mesures de maîtrise des risques, vérification de leur efficacité et dossier de gestion des risques |
| [ISO 13485:2016](https://committee.iso.org/standard/59752.html), système de management de la qualité | Modifications revues et décisions de conception documentées | Système qualité, maîtrise des documents et conservation des enregistrements |

La [note de preuves](docs/governed-evidence.md) énonce ce que couvrent les contrôles du graphe de modules, des sources et des symboles d'allocation et d'exception. Les [preuves de scénarios](docs/audit-scenario-validation.md) décrivent les cas d'audit testés. Ce sont des contrôles d'ingénierie, pas un dossier de certification.

Le [dossier de développement logiciel](software_development_file/README.md) établit la base
documentaire initiale acceptée pour le jalon 0 de [#112](https://github.com/ambroise-leclerc/mddlog/issues/112)
et son préalable [#124](https://github.com/ambroise-leclerc/mddlog/issues/124). Il distingue les
contrats implémentés, la couverture existante et les lacunes de qualification, avec un registre
unique et des modèles en français pour les intégrateurs. L’acceptation de projet conserve des
limites explicites et les qualifications futures ouvertes ; les modèles doivent être complétés
pour le dispositif et son profil réel.

### Livraisons et feuille de route

- **Livré (v0.1.0) :** cœur de journalisation borné sans allocation ([ADR-001](docs/adr/ADR-001-allocation-free-governed-logging-core.md) ; épique [#8](https://github.com/ambroise-leclerc/mddlog/issues/8)).
- **Livré (v0.2.0) :** admission d'audit bornée, séquence par flux, refus explicite et transmission avec santé ([ADR-002](docs/adr/ADR-002-regulatory-audit-event-model.md) ; épique [#9](https://github.com/ambroise-leclerc/mddlog/issues/9)).
- **Livré (v0.2.0) :** registre de sinks synchronisé, consommateur de transport borné et journal texte, avec WebFront comme consommateur de référence ([ADR-003](docs/adr/ADR-003-application-integration-and-sink-ownership.md) ; épique [#10](https://github.com/ambroise-leclerc/mddlog/issues/10)).
- **Livré (v0.2.0), preuves en revue :** confirmation durable sur un support éligible, détection d'altération relative à un ancrage indépendant, redémarrage, rotation et rétention ([ADR-004](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md) ; épique [#11](https://github.com/ambroise-leclerc/mddlog/issues/11)). L'acceptation de ces preuves est une décision de revue distincte.
- **Livré (v0.3.0) :** contextes et liaisons de diagnostic/audit, filtrage paresseux et usages vérifiés ([ADR-005](docs/adr/ADR-005-contextual-logging-api.md) ; [#113](https://github.com/ambroise-leclerc/mddlog/issues/113)). L’intégration du composant local est acceptée ; application indépendante #122, budgets #117, robustesse #120 et gel #121 restent ouverts.
- **Différé :** signature et gestion des clés, format d'export ([ADR-004, décision 11](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md)).

## Compiler et vérifier

CMake fait foi pour les compilateurs pris en charge et les fichiers de modules. Les versions minimales admises sont GCC 16.1, Clang 20 amont ou MSVC 19.40 (Visual Studio 2022 17.10), avec CMake 4.0 à 4.3 et Ninja. Une version admise n'est pas forcément une configuration testée : la prise en charge de `import std` dépend de la combinaison exacte du compilateur, de la bibliothèque standard et de CMake.

```bash
cmake -S . -B build -G Ninja -DMDDLOG_BUILD_EXAMPLES=OFF -DMDDLOG_BUILD_TESTS=OFF
cmake --build build --parallel
```

Configurations de la CI :

| Plateforme | Compilateur et bibliothèque standard | CMake |
| --- | --- | --- |
| Windows x64 | MSVC 19.40+ | 4.1.1 |
| Linux x86_64 | GCC 16.1.0 / libstdc++ | 4.1.1 |
| Linux x86_64 | Clang 21 amont / libc++ | 4.3.1 |
| macOS arm64 | Clang 21.1.8 amont / libc++ | 4.3.1 |

Utilisez le preset correspondant de [CMakePresets.json](CMakePresets.json) pour compiler les exemples et lancer CTest, par exemple :

```bash
cmake --preset ninja-gcc
cmake --build --preset ninja-gcc
ctest --preset ninja-gcc --output-on-failure
```

Les tests récupèrent une révision figée de [SpecLab](https://github.com/ambroise-leclerc/SpecLab), seulement lorsqu'ils sont activés. `SourceTreeCoreConsumer` et `InstallTreeCoreConsumer` exercent séparément la cible du cœur seul. La CI exécute aussi les sanitizers et les contrôles du cœur gouverné ; voir [la note de preuves](docs/governed-evidence.md) pour leur portée. Une configuration réussie n'est ni une compilation ni un résultat de test.

## Licence et participation

mddlog est proposé sous la [Licence publique de l'Union européenne 1.2](LICENSE) ou sous conditions commerciales séparées ; voir [LICENSING.md](LICENSING.md). [CONTRIBUTING.md](CONTRIBUTING.md) décrit le processus de contribution sur invitation et les règles de revue. Pour une question ou un défaut, utilisez les [issues GitHub](https://github.com/ambroise-leclerc/mddlog/issues).
