# mddlog — C++23-Protokollierung für Medizinprodukte-Software

[🇫🇷 Français](README.md) · [🇬🇧 English](README.en-GB.md) · 🇩🇪 Deutsch · [🇪🇸 Español](README.es.md)

**mddlog** ist eine C++23-Modulbibliothek für Diagnoseprotokollierung und Audit-Trails in Medizinprodukte-Software und anderen regulierten eingebetteten Systemen. Sie hält drei Dinge auseinander, die leicht verwechselt werden: eine Diagnosemeldung, ein im Speicher zugelassenes Audit-Ereignis und einen dauerhaft und überprüfbar aufbewahrten Audit-Datensatz. Jeder Schritt liefert ein ausdrückliches Ergebnis, und jede Zusicherung ist schriftlich begrenzt.

[Architekturentscheidungen](docs/adr/README.md) · [Validierung der Audit-Persistenz](docs/audit-persistence-validation.md) · [Leitfaden zur Audit-Zulassung](docs/migration/audit-admission.md) · [Leitfaden zum Audit-Register](docs/migration/audit-ledger.md) · [Nachweise zum geregelten Kern](docs/governed-evidence.md) · [Änderungsprotokoll](CHANGELOG.md) · [Versionen](https://github.com/ambroise-leclerc/mddlog/releases)

Maßgeblich ist die französische Fassung ([README](README.md)); die verlinkten Dokumente sind auf Englisch.

## Neu in Version 0.2.0

v0.1.0 lieferte einen begrenzten, allokationsfreien Protokollierungskern (ADR-001). v0.2.0 ergänzt drei vollständige Fähigkeiten.

### 1. Audit-Ereignisse, getrennt von Diagnoseprotokollen — [ADR-002](docs/adr/ADR-002-regulatory-audit-event-model.md)

- **`AuditEvent`** trägt exakte Bezeichner (Aktion, Akteur, Ziel, Anforderungs- und Risikoreferenzen, Korrelation), eine Kategorie, eine Phase (angefordert, bestätigt, ausgeführt, fehlgeschlagen), eine vom Host gelieferte Zeit und eine Stream-Identität. Ein ungültiger Bezeichner wird abgewiesen, nie gekürzt; nur der Text `detail` darf gekürzt werden, mit einer Markierung.
- **`AuditRing<N>`** lässt ein Ereignis in begrenzten Speicher zu und vergibt eine Sequenznummer je Stream. Ist er voll, weist er ab, statt zu überschreiben, ohne eine Sequenz zu verbrauchen.
- **`AuditSinkAdapter`** übergibt zugelassene Ereignisse an einen `AuditSink`, gibt nur Angenommenes frei, behält den Rest für einen neuen Versuch und veröffentlicht seinen eigenen Zustand: übergeben, Fehler, Verluste nach der Zulassung, wartende Ereignisse.
- **`logAudit(AuditInput)`** an `SimpleLogger` und `Log` liefert das Zulassungsergebnis, unabhängig von Diagnosefiltern.

### 2. Bausteine für die Integration in eine Anwendung — [ADR-003](docs/adr/ADR-003-application-integration-and-sink-ownership.md)

- **`SinkRegistry`**: synchronisierte Registrierung von Rückrufen über ausdrückliche Handles, mit sicherem Entfernen, auch aus dem Rückruf selbst.
- **`TransportConsumer`**: begrenzte Ringe, auf einem Thread verarbeitet, mit unabhängigem Zustand, Isolierung eines fehlerhaften Transports und Unterdrückung reentranter Protokollierungsschleifen.
- **`TextLogger`**: ein Diagnose-Textprotokoll mit unabhängig aktivierbaren Gruppen, Aufruferquelle und getrennten Speicherauszügen.
- WebFront übernimmt und prüft sie in seinem eigenen Repository.

### 3. Audit-Persistenz mit Manipulationserkennung durch Verankerung — [ADR-004](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md)

- **Kanonisches Format und Verkettung**: Jedes Ereignis wird nach einem versionierten Byte-Vertrag kodiert und innerhalb seines Streams mit SHA-256 verkettet. Referenzvektoren legen jedes Byte fest, und jede Toolchain der CI prüft sie.
- **Speicherung und dauerhafte Bestätigung**: `PersistingAuditSink` schreibt Segmente nur anhängend auf ein `StorageMedium`. Ein Ereignis gilt erst als „dauerhaft bestätigt“, nachdem das Medium einen `sync` bestätigt hat, für ein vollständiges Präfix des Streams. Jeder Speicherfehler ist im Zustand des Sinks sichtbar und wird nie als Audit-Ereignis dargestellt.
- **Verankerung und Prüfung**: Ein `AnchorProvider` außerhalb des Protokolls bewahrt die erreichte Position und den Hashwert. `LogVerifier` liest das ganze Medium zurück und liefert für jeden Stream ein ausdrückliches Urteil: *Anchored* mit seinem Bereich, *Unanchored*, *anchor unavailable*, *Incomplete*, *Altered*, *Rolled back*, *Conflict*, *Retired*, *Cannot verify* oder *Inconsistent*. Nie ein bloßes „gültig“.
- **Erkannte Rücksetzung**: Die Position, die der Leser außerhalb des Geräts festhält (`RetainedPosition`), deckt auf, wenn ein altes Protokoll zusammen mit seinem alten Anker wiederhergestellt wird.
- **Neustart, Rotation und Aufbewahrung**: Ein Register hält jeden Start, jeden geöffneten oder geschlossenen Stream und jede Entfernung fest. Die Wiederaufnahme berechnet den Kettenzustand neu und vergleicht ihn mit dem Anker. Die Rotation entfernt ganze Segmente, nie über den Anker hinaus, nachdem sie ihren Eintrag dauerhaft bestätigt hat. Der Leser meldet jeden Neustart, jede Entfernung und jeden Rest einer Stromunterbrechung als Grenze, nie als Kontinuität.
- **Durchgängige Validierung**: Jedes Validierungsszenario von ADR-004, vom Umschreiben mit neu berechneten Hashwerten bis zur Stromunterbrechung während eines `sync`, läuft vom Erzeuger bis zum Bericht des Lesers. Der [Validierungsbericht](docs/audit-persistence-validation.md) ordnet jedem Kriterium seine Tests zu und nennt die Grenzen.

### Inkompatible Änderungen

- `LogLevel::Audit` und die alten positionalen Überladungen von `logAudit(...)` sind entfernt: Verwenden Sie `logAudit(AuditInput)` ([Leitfaden](docs/migration/audit-admission.md)).
- Aktionen, die mit `mddlog.` beginnen, sind dem Persistenzregister vorbehalten: Die Zulassung eines Erzeugers weist sie ab (`AuditRefusalReason::ReservedAction`).

## Was jeder Pfad bietet

| Pfad | Was er bietet | Zu beachtende Grenze |
| --- | --- | --- |
| Geregelter Diagnosekern (`mddlog::core`) | `GovernedRecord` mit fester Kapazität und `RingLog` mit einem Erzeuger und einem Verbraucher; vom Host gelieferte Zeit; ausdrückliche Zulassungs- und Kürzungsergebnisse | Kein Sink, keine Persistenz, keine systemweite Zeitzusicherung |
| Audit-Kern (`mddlog::core`) | Begrenzte `AuditEvent` und `AuditRing`; exakte Bezeichner, Kategorie und Phase, Stream-Identität und vergebene Sequenz | Zugelassen heißt im Speicher; ein voller Ring weist neue Ereignisse ab |
| Öffentliche Audit-API (`mddlog::mddlog`) | `SimpleLogger::logAudit(AuditInput)` und `Log::logAudit(AuditInput)` liefern nach `setAuditRing()` das Zulassungsergebnis | Der gebundene Ring muss seine Bindung überdauern; `Log::shutdown()` löst die Bindung |
| Audit-Übergabe (`mddlog::mddlog`) | `AuditSinkAdapter` übergibt an einen `AuditSink`, veröffentlicht seinen Zustand und quittiert Angenommenes | Ein Verbraucher-Thread; die Annahme durch einen Sink ist keine dauerhafte Speicherung |
| Audit-Persistenz (`mddlog::mddlog`) | `PersistingAuditSink` speichert, bestätigt dauerhaft, führt ein Register, rotiert und bewahrt auf; `LogVerifier` liefert ein Urteil je Stream | Die Dauerhaftigkeit hängt vom Medium ab; die Manipulationserkennung von einem unabhängigen Anker; nichts ist signiert |
| Anwendungsintegration (`mddlog::mddlog`) | `SinkRegistry`, `TransportConsumer` und `TextLogger` | `TextLogger` rendert und ruft synchron zurück, mit Allokation |
| Diagnoseadapter (`mddlog::mddlog`) | `SimpleLogger`, `LogRecord`, `ConsoleSink` und die Fassade `Log` | Seine asynchrone Warteschlange alloziert und ist unbegrenzt: nicht der geregelte Pfad |

Jeder Ring hat **einen Erzeuger und einen Verbraucher**. Der Host liefert für jeden Erzeuger und jede Startsitzung eine eigene Stream-Identität und entscheidet, wie er auf eine Abweisung, einen Übergabefehler oder eine Stromunterbrechung reagiert.

## Erste Schritte

### Ein Audit-Ereignis zulassen

```cpp
import mddlog.core.auditring;

using namespace mddlog::core;

AuditRing<64> audit{"device_789:boot_42:operator"};
AuditInput input{
    .category = AuditCategory::RiskControl,
    .phase = AuditPhase::Requested,
    .time = RawTime::unavailable(),
    .action = "EMERGENCY_SHUTDOWN",
    .actor = "operator_42",
    .target = "pump_7",
    .riskRef = "RISK_17",
    .correlationId = "device_789:boot_42:input:41",
    .sourceSequence = 41,
    .detail = "Shutdown requested",
};

auto result = audit.tryRecord(input);
if (!result.wasAdmitted()) {
    // Abweisungsstrategie des Hosts anwenden; result.refusal() prüfen.
}
```

### Persistieren und prüfen

Medium und Ankeranbieter stellt der Integrator bereit: Das Medium muss den Speichervertrag von ADR-004 erfüllen (Entscheidung 9), und der Anbieter muss vom Protokoll unabhängig sein (Entscheidung 7).

```cpp
import mddlog.adapter.auditdrain;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditlogverifier;

using namespace mddlog::adapter;

MyStorageMedium medium;    // implementiert StorageMedium
MyAnchorProvider provider; // implementiert AnchorProvider, außerhalb des Protokolls

StorageConfig config;
config.segmentSize        = 4096;
config.segmentCount       = 256;
config.maxProducerStreams = 4;
config.ledger             = LedgerConfig{.streamId = "device_789:boot_42:ledger"};
config.provider           = &provider;

auto sink = PersistingAuditSink::create(medium, std::move(config));
if (!sink) {
    // Konfiguration abgewiesen: sink.error() prüfen.
}

AuditSinkAdapter adapter;
(void)adapter.addRing(audit); // der AuditRing aus dem vorigen Beispiel
adapter.setSink(*sink);
(void)adapter.drainOnce();    // auf dem Verbraucher-Thread
(void)(*sink)->advanceAnchor("device_789:boot_42:operator");

RetainedPosition retained; // vom Leser außerhalb des Geräts aufbewahrt
LogVerifier verifier{medium, provider, retained};
const LogReport report = verifier.verify();
```

Ein CMake-Verbraucher, der nur den Kern braucht, bindet `mddlog::core`; für Audit-Übergabe und -Persistenz, Diagnoseprotokollierung und Integration bindet er `mddlog::mddlog`. Das vollständige Ziel hängt vom Kern ab; der Kern importiert weder Adapter noch Sink.

```cmake
add_subdirectory(mddlog)
target_link_libraries(your_target PRIVATE mddlog::mddlog)
```

Weiterführend: [Audit-Zulassung und -Übergabe](docs/migration/audit-admission.md), [Register, Neustart, Rotation und Aufbewahrung](docs/migration/audit-ledger.md), [Textprotokoll](docs/migration/text-logger.md), [Transportverbraucher](docs/migration/transport-consumer.md), [geregelter Kern](docs/migration/governed-core.md) und das Beispiel [basic_usage.cpp](examples/basic_usage.cpp).

## Was die Persistenz zusichert und was nicht

- **Manipulationserkennung relativ zu einem Anker.** Ein Umschreiben, eine Kürzung oder ein wiederhergestellter alter Zustand wird gemeldet, wenn er Datensätze betrifft, die ein unabhängiger Anker abdeckt, oder eine Position, die der Leser festgehalten hat. Das Protokoll bleibt für jeden änderbar, der das Medium beschreiben kann: mddlog verhindert das nicht.
- **Nach dem letzten Anker ist nichts erkennbar.** Spätere Datensätze sind nur untereinander konsistent, und der Bericht sagt das.
- **Kein Urheberschaftsnachweis.** Nichts ist signiert: Die Verkettung zeigt Konsistenz, nicht, wer einen Datensatz geschrieben hat. Signatur, Schlüsselverwaltung und ein Exportformat sind zurückgestellt (ADR-004, Entscheidung 11).
- **Die Bibliothek liefert weder ein echtes Medium noch einen echten Anbieter.** `InMemoryStorageMedium` und `InMemoryAnchorProvider` sind Test-Doubles. Ein geeignetes Medium (Datei, Flash, Geräteprotokoll) und einen geeigneten Anbieter zu qualifizieren, ist Aufgabe des Integrators.
- **Ein Verbraucher-Thread** für den Audit-Adapter, den Transportverbraucher und den persistierenden Sink.

Der [Validierungsbericht](docs/audit-persistence-validation.md) beschreibt die nachgewiesene Abdeckung und ihre Grenzen.

## Regulatorischer Kontext und Nachweise

Diese Normen beschreiben Prozesse und Verantwortlichkeiten der Medizinprodukte-Entwicklung. mddlog liefert Bausteine und Prüfmaterial; seine Verwendung begründet keine Konformität mit einer Norm oder Vorschrift, und die Bibliothek ist für kein bestimmtes Produkt zertifiziert oder validiert. Jeder Hersteller bewertet sie in seinem eigenen System, Risikomanagement, Software-Lebenszyklus und Qualitätsmanagement.

| Referenz | Projektmaterial | Was beim Hersteller bleibt |
| --- | --- | --- |
| [IEC 62304:2006 + AMD1:2015](https://webstore.iec.ch/en/publication/22790), Software-Lebenszyklus-Prozesse für Medizinprodukte | [ADRs](docs/adr/README.md), versionierte Quellen, [Tests](tests/), [Prüfungen des geregelten Kerns](docs/governed-evidence.md) und [Persistenzvalidierung](docs/audit-persistence-validation.md) | Lebenszyklusaktivitäten, System-Verifizierung und -Validierung, Konfigurationsmanagement und Problemlösung |
| [ISO 14971:2019](https://www.iso.org/standard/72704.html), Risikomanagement | `riskRef` und `requirementRef` an `AuditEvent`; ausdrückliche Ergebnisse für Abweisung, Übergabe und Prüfung | Gefährdungsanalyse, Risikobeherrschungsmaßnahmen, Prüfung ihrer Wirksamkeit und Risikomanagementakte |
| [ISO 13485:2016](https://committee.iso.org/standard/59752.html), Qualitätsmanagementsysteme | Geprüfte Änderungen und dokumentierte Designentscheidungen | Qualitätsmanagementsystem, Lenkung von Dokumenten und Aufbewahrung von Aufzeichnungen |

Die [Nachweisnotiz](docs/governed-evidence.md) beschreibt, was die Prüfungen des Modulgraphen, der Quellen sowie der Allokations- und Ausnahmesymbole abdecken. Die [Szenario-Nachweise](docs/audit-scenario-validation.md) beschreiben die geprüften Audit-Fälle. Es sind technische Prüfungen, kein Zertifizierungsdossier.

### Lieferungen und Fahrplan

- **Geliefert (v0.1.0):** allokationsfreier, begrenzter Protokollierungskern ([ADR-001](docs/adr/ADR-001-allocation-free-governed-logging-core.md); Epic [#8](https://github.com/ambroise-leclerc/mddlog/issues/8)).
- **Geliefert (v0.2.0):** begrenzte Audit-Zulassung, Sequenz je Stream, ausdrückliche Abweisung und Übergabe mit Zustandsmeldung ([ADR-002](docs/adr/ADR-002-regulatory-audit-event-model.md); Epic [#9](https://github.com/ambroise-leclerc/mddlog/issues/9)).
- **Geliefert (v0.2.0):** synchronisiertes Sink-Register, begrenzter Transportverbraucher und Textprotokoll, mit WebFront als Referenzverbraucher ([ADR-003](docs/adr/ADR-003-application-integration-and-sink-ownership.md); Epic [#10](https://github.com/ambroise-leclerc/mddlog/issues/10)).
- **Geliefert (v0.2.0), Nachweise in Prüfung:** dauerhafte Bestätigung auf einem geeigneten Medium, Manipulationserkennung relativ zu einem unabhängigen Anker, Neustart, Rotation und Aufbewahrung ([ADR-004](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md); Epic [#11](https://github.com/ambroise-leclerc/mddlog/issues/11)). Die Annahme dieser Nachweise ist eine gesonderte Prüfentscheidung.
- **Zurückgestellt:** Signatur und Schlüsselverwaltung, Exportformat ([ADR-004, Entscheidung 11](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md)).

## Bauen und prüfen

CMake ist maßgeblich für unterstützte Compiler und Moduldateien. Die zugelassenen Mindestversionen sind GCC 16.1, Upstream-Clang 20 oder MSVC 19.40 (Visual Studio 2022 17.10), mit CMake 4.0 bis 4.3 und Ninja. Eine zugelassene Version ist nicht unbedingt eine getestete Konfiguration: Die Unterstützung von `import std` hängt von der genauen Kombination aus Compiler, Standardbibliothek und CMake ab.

```bash
cmake -S . -B build -G Ninja -DMDDLOG_BUILD_EXAMPLES=OFF -DMDDLOG_BUILD_TESTS=OFF
cmake --build build --parallel
```

CI-Konfigurationen:

| Plattform | Compiler und Standardbibliothek | CMake |
| --- | --- | --- |
| Windows x64 | MSVC 19.40+ | 4.1.1 |
| Linux x86_64 | GCC 16.1.0 / libstdc++ | 4.1.1 |
| Linux x86_64 | Upstream-Clang 21 / libc++ | 4.3.1 |
| macOS arm64 | Upstream-Clang 21.1.8 / libc++ | 4.3.1 |

Verwenden Sie das passende Preset aus [CMakePresets.json](CMakePresets.json), um die Beispiele zu bauen und CTest auszuführen, zum Beispiel:

```bash
cmake --preset ninja-gcc
cmake --build --preset ninja-gcc
ctest --preset ninja-gcc --output-on-failure
```

Die Tests laden eine festgelegte Revision von [SpecLab](https://github.com/ambroise-leclerc/SpecLab), nur wenn sie aktiviert sind. `SourceTreeCoreConsumer` und `InstallTreeCoreConsumer` prüfen getrennt das reine Kernziel. Die CI führt außerdem Sanitizer und die Prüfungen des geregelten Kerns aus; ihr Umfang steht in [der Nachweisnotiz](docs/governed-evidence.md). Eine erfolgreiche Konfiguration ist weder ein Build noch ein Testergebnis.

## Lizenz und Mitwirkung

mddlog wird unter der [Europäischen Union Public Licence 1.2](LICENSE) oder gesonderten kommerziellen Bedingungen angeboten; siehe [LICENSING.md](LICENSING.md). [CONTRIBUTING.md](CONTRIBUTING.md) beschreibt den Beitragsprozess auf Einladung und die Prüfregeln. Für Fragen oder Fehler nutzen Sie die [GitHub-Issues](https://github.com/ambroise-leclerc/mddlog/issues).
