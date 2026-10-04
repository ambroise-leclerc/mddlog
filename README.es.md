# mddlog

**Biblioteca de logging C++23** para productos sanitarios críticos y software regulado o safety-critical.

[🇫🇷 Français](README.md) · [🇬🇧 English](README.en-GB.md) · 🇪🇸 Español

```cpp
import mddlog.log;

mddlog::Log::info("Bomba lista");
mddlog::Log::error("Oclusión detectada en la vía A");
```

- **Logs de diagnóstico**: los niveles habituales, sinks y una fachada `Log` lista para usar.
- **Núcleo acotado, sin asignación de memoria**: capacidad fija, rechazo explícito en lugar de sobrescritura silenciosa.
- **Registro de auditoría**: eventos separados de los logs, persistidos, encadenados y verificables frente a un anclaje independiente.

[Decisiones de arquitectura](docs/adr/README.md) · [Validación de la persistencia de auditoría](docs/audit-persistence-validation.md) · [Guía de admisión de auditoría](docs/migration/audit-admission.md) · [Guía del libro de registro de auditoría](docs/migration/audit-ledger.md) · [Evidencias del núcleo gobernado](docs/governed-evidence.md) · [Registro de cambios](CHANGELOG.md) · [Versiones](https://github.com/ambroise-leclerc/mddlog/releases)

La versión de referencia es la francesa ([README](README.md)); los documentos enlazados están en inglés.

## Novedades de la versión 0.2.0

La v0.1.0 ofrecía un núcleo de registro acotado y sin asignación de memoria (ADR-001). La v0.2.0 añade tres capacidades completas.

### 1. Eventos de auditoría separados de los registros de diagnóstico — [ADR-002](docs/adr/ADR-002-regulatory-audit-event-model.md)

- **`AuditEvent`** lleva identificadores exactos (acción, actor, objetivo, referencias de requisito y de riesgo, correlación), una categoría, una fase (solicitada, confirmada, ejecutada, fallida), una hora proporcionada por el anfitrión y una identidad de flujo. Un identificador no válido se rechaza, nunca se trunca; solo el texto `detail` puede acortarse, con un indicador.
- **`AuditRing<N>`** admite un evento en memoria acotada y le asigna un número de secuencia por flujo. Lleno, rechaza en lugar de sobrescribir, sin consumir secuencia.
- **`AuditSinkAdapter`** entrega los eventos admitidos a un `AuditSink`, libera solo lo aceptado, conserva el resto para un nuevo intento y publica su propio estado: entregados, fallos, pérdidas tras la admisión, eventos pendientes.
- **`logAudit(AuditInput)`** en `SimpleLogger` y `Log` devuelve el resultado de admisión, sean cuales sean los filtros de diagnóstico.

### 2. Piezas para integrar mddlog en una aplicación — [ADR-003](docs/adr/ADR-003-application-integration-and-sink-ownership.md)

- **`SinkRegistry`**: registro sincronizado de retrollamadas mediante manejadores explícitos, con retirada segura, incluso desde la propia retrollamada.
- **`TransportConsumer`**: anillos acotados consumidos en un solo hilo, estado independiente, aislamiento de un transporte que falla y supresión de los bucles de registro reentrantes.
- **`TextLogger`**: registro de texto de diagnóstico con grupos activables de forma independiente, origen del llamante y volcados separados.
- WebFront las adopta y las verifica en su propio repositorio.

### 3. Persistencia de auditoría, con detección de alteraciones mediante anclaje — [ADR-004](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md)

- **Formato canónico y encadenamiento**: cada evento se codifica según un contrato de bytes versionado y se encadena con SHA-256 dentro de su flujo. Unos vectores de referencia fijan cada byte, y cada cadena de herramientas de la CI los comprueba.
- **Almacenamiento y confirmación duradera**: `PersistingAuditSink` escribe segmentos de solo adición en un `StorageMedium`. Un evento solo se declara «confirmado de forma duradera» después de un `sync` que el soporte ha confirmado, para un prefijo completo del flujo. Todo fallo del almacenamiento es visible en el estado del sink y nunca se presenta como un evento de auditoría.
- **Anclaje y verificación**: un `AnchorProvider`, externo al registro, conserva la posición y el resumen criptográfico alcanzados. `LogVerifier` relee todo el soporte y devuelve, para cada flujo, un veredicto explícito: *Anchored* con su rango, *Unanchored*, *anchor unavailable*, *Incomplete*, *Altered*, *Rolled back*, *Conflict*, *Retired*, *Cannot verify* o *Inconsistent*. Nunca un simple «válido».
- **Retroceso detectado**: la posición que el lector conserva fuera del dispositivo (`RetainedPosition`) revela la restauración conjunta de un registro antiguo y de su anclaje antiguo.
- **Reinicio, rotación y retención**: un libro de registro consigna cada arranque, cada flujo abierto o cerrado y cada retirada. La reanudación recalcula el estado de la cadena y lo compara con el anclaje. La rotación retira segmentos completos, nunca más allá del anclaje, después de confirmar de forma duradera su rastro. El lector informa de cada reinicio, cada retirada y cada resto de un corte de alimentación como una frontera, nunca como una continuidad.
- **Validación de extremo a extremo**: cada escenario de validación de ADR-004, desde la reescritura con resúmenes recalculados hasta el corte de alimentación durante un `sync`, se ejecuta desde el productor hasta el informe del lector. El [informe de validación](docs/audit-persistence-validation.md) relaciona cada criterio con sus pruebas y enuncia los límites.

### Cambios incompatibles

- Se eliminan `LogLevel::Audit` y las antiguas sobrecargas posicionales de `logAudit(...)`: use `logAudit(AuditInput)` ([guía](docs/migration/audit-admission.md)).
- Las acciones que empiezan por `mddlog.` están reservadas al libro de registro de persistencia: la admisión de un productor las rechaza (`AuditRefusalReason::ReservedAction`).

## Qué ofrece cada ruta

| Ruta | Qué ofrece | Límite que conviene conocer |
| --- | --- | --- |
| Núcleo de diagnóstico gobernado (`mddlog::core`) | `GovernedRecord` de capacidad fija y `RingLog` de un productor y un consumidor; hora proporcionada por el anfitrión; resultados explícitos de admisión y truncamiento | Sin sink, sin persistencia, sin garantía temporal a escala del sistema |
| Núcleo de auditoría (`mddlog::core`) | `AuditEvent` y `AuditRing` acotados; identificadores exactos, categoría y fase, identidad de flujo y secuencia asignada | Admitido significa en memoria; un anillo lleno rechaza los nuevos eventos |
| API de auditoría pública (`mddlog::mddlog`) | `SimpleLogger::logAudit(AuditInput)` y `Log::logAudit(AuditInput)` devuelven el resultado de admisión tras `setAuditRing()` | El anillo vinculado debe sobrevivir a su vínculo; `Log::shutdown()` borra el vínculo |
| Entrega de auditoría (`mddlog::mddlog`) | `AuditSinkAdapter` entrega a un `AuditSink`, publica su estado y confirma lo aceptado | Un solo hilo consumidor; la aceptación por un sink no es un almacenamiento duradero |
| Persistencia de auditoría (`mddlog::mddlog`) | `PersistingAuditSink` almacena, confirma de forma duradera, lleva un libro de registro, rota y retiene; `LogVerifier` devuelve un veredicto por flujo | La durabilidad depende del soporte; la detección de alteraciones, de un anclaje independiente; nada está firmado |
| Integración en aplicaciones (`mddlog::mddlog`) | `SinkRegistry`, `TransportConsumer` y `TextLogger` | `TextLogger` representa y llama de forma síncrona, con asignación de memoria |
| Adaptador de diagnóstico (`mddlog::mddlog`) | `SimpleLogger`, `LogRecord`, `ConsoleSink` y la fachada `Log` | Su cola asíncrona asigna memoria y no está acotada: no es la ruta gobernada |

Cada anillo tiene **un productor y un consumidor**. El anfitrión proporciona una identidad de flujo distinta para cada productor y cada sesión de arranque, y decide cómo responder a un rechazo, a un fallo de entrega o a un corte de alimentación.

## Primeros pasos

### Admitir un evento de auditoría

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
    // Aplicar la política de rechazo del anfitrión; consultar result.refusal().
}
```

### Persistir y verificar

El soporte y el proveedor de anclaje los aporta el integrador: el soporte debe cumplir el contrato de almacenamiento de ADR-004 (decisión 9), y el proveedor debe ser independiente del registro (decisión 7).

```cpp
import mddlog.adapter.auditdrain;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditlogverifier;

using namespace mddlog::adapter;

MyStorageMedium medium;    // implementa StorageMedium
MyAnchorProvider provider; // implementa AnchorProvider, fuera del registro

StorageConfig config;
config.segmentSize        = 4096;
config.segmentCount       = 256;
config.maxProducerStreams = 4;
config.ledger             = LedgerConfig{.streamId = "device_789:boot_42:ledger"};
config.provider           = &provider;

auto sink = PersistingAuditSink::create(medium, std::move(config));
if (!sink) {
    // Configuración rechazada: consultar sink.error().
}

AuditSinkAdapter adapter;
(void)adapter.addRing(audit); // el AuditRing del ejemplo anterior
adapter.setSink(*sink);
(void)adapter.drainOnce();    // en el hilo consumidor
(void)(*sink)->advanceAnchor("device_789:boot_42:operator");

RetainedPosition retained; // conservada por el lector, fuera del dispositivo
LogVerifier verifier{medium, provider, retained};
const LogReport report = verifier.verify();
```

Un consumidor CMake que solo necesita el núcleo se enlaza con `mddlog::core`; para la entrega y la persistencia de auditoría, el registro de diagnóstico y la integración, con `mddlog::mddlog`. El objetivo completo depende del núcleo; el núcleo no importa ni adaptadores ni sinks.

```cmake
add_subdirectory(mddlog)
target_link_libraries(your_target PRIVATE mddlog::mddlog)
```

Para saber más: [admisión y entrega de auditoría](docs/migration/audit-admission.md), [libro de registro, reinicio, rotación y retención](docs/migration/audit-ledger.md), [registro de texto](docs/migration/text-logger.md), [consumidor de transporte](docs/migration/transport-consumer.md), [núcleo gobernado](docs/migration/governed-core.md) y el ejemplo [basic_usage.cpp](examples/basic_usage.cpp).

## Qué garantiza la persistencia y qué no

- **Detección de alteraciones, relativa a un anclaje.** Una reescritura, un truncamiento o un estado antiguo restaurado se señala cuando afecta a registros que cubre un anclaje independiente, o a una posición que el lector ha conservado. El registro sigue siendo modificable por cualquiera que pueda escribir el soporte: mddlog no lo impide.
- **Más allá del último anclaje, nada es detectable.** Los registros posteriores solo son coherentes entre sí, y el informe lo dice.
- **Ninguna prueba de autoría.** Nada está firmado: el encadenamiento muestra coherencia, no quién escribió un registro. La firma, la gestión de claves y un formato de exportación quedan aplazados (ADR-004, decisión 11).
- **La biblioteca no incluye ni soporte ni proveedor reales.** `InMemoryStorageMedium` e `InMemoryAnchorProvider` son dobles de prueba. Cualificar un soporte (archivo, flash, registro de dispositivo) y un proveedor adecuados corresponde al integrador.
- **Un solo hilo consumidor** para el adaptador de auditoría, el consumidor de transporte y el sink persistente.

El [informe de validación](docs/audit-persistence-validation.md) detalla la cobertura demostrada y sus límites.

## Contexto regulatorio y evidencias

Estas normas describen procesos y responsabilidades del desarrollo de productos sanitarios. mddlog aporta piezas y material de revisión; su uso no establece la conformidad con ninguna norma ni reglamento, y la biblioteca no está certificada ni validada para ningún producto concreto. Cada fabricante la evalúa en su propio sistema, gestión de riesgos, ciclo de vida del software y sistema de calidad.

| Referencia | Material del proyecto | Lo que queda a cargo del fabricante |
| --- | --- | --- |
| [IEC 62304:2006 + AMD1:2015](https://webstore.iec.ch/en/publication/22790), procesos del ciclo de vida del software de productos sanitarios | [ADR](docs/adr/README.md), código versionado, [pruebas](tests/), [controles del núcleo gobernado](docs/governed-evidence.md) y [validación de la persistencia](docs/audit-persistence-validation.md) | Actividades del ciclo de vida, verificación y validación del sistema, gestión de la configuración y resolución de problemas |
| [ISO 14971:2019](https://www.iso.org/standard/72704.html), gestión de riesgos | `riskRef` y `requirementRef` en `AuditEvent`; resultados explícitos de rechazo, entrega y verificación | Análisis de peligros, medidas de control del riesgo, verificación de su eficacia y expediente de gestión de riesgos |
| [ISO 13485:2016](https://committee.iso.org/standard/59752.html), sistemas de gestión de la calidad | Cambios revisados y decisiones de diseño documentadas | Sistema de calidad, control de documentos y conservación de registros |

La [nota de evidencias](docs/governed-evidence.md) indica qué cubren los controles del grafo de módulos, de las fuentes y de los símbolos de asignación y de excepción. Las [evidencias de escenarios](docs/audit-scenario-validation.md) describen los casos de auditoría probados. Son controles de ingeniería, no un expediente de certificación.

### Entregas y hoja de ruta

- **Entregado (v0.1.0):** núcleo de registro acotado sin asignación de memoria ([ADR-001](docs/adr/ADR-001-allocation-free-governed-logging-core.md); épica [#8](https://github.com/ambroise-leclerc/mddlog/issues/8)).
- **Entregado (v0.2.0):** admisión de auditoría acotada, secuencia por flujo, rechazo explícito y entrega con estado ([ADR-002](docs/adr/ADR-002-regulatory-audit-event-model.md); épica [#9](https://github.com/ambroise-leclerc/mddlog/issues/9)).
- **Entregado (v0.2.0):** registro de sinks sincronizado, consumidor de transporte acotado y registro de texto, con WebFront como consumidor de referencia ([ADR-003](docs/adr/ADR-003-application-integration-and-sink-ownership.md); épica [#10](https://github.com/ambroise-leclerc/mddlog/issues/10)).
- **Entregado (v0.2.0), evidencias en revisión:** confirmación duradera en un soporte apto, detección de alteraciones relativa a un anclaje independiente, reinicio, rotación y retención ([ADR-004](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md); épica [#11](https://github.com/ambroise-leclerc/mddlog/issues/11)). La aceptación de estas evidencias es una decisión de revisión aparte.
- **Aplazado:** firma y gestión de claves, formato de exportación ([ADR-004, decisión 11](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md)).

## Compilar y verificar

CMake es la referencia para los compiladores admitidos y los archivos de módulos. Las versiones mínimas admitidas son GCC 16.1, Clang 20 de origen o MSVC 19.40 (Visual Studio 2022 17.10), con CMake 4.0 a 4.3 y Ninja. Una versión admitida no es necesariamente una configuración probada: la compatibilidad con `import std` depende de la combinación exacta de compilador, biblioteca estándar y CMake.

```bash
cmake -S . -B build -G Ninja -DMDDLOG_BUILD_EXAMPLES=OFF -DMDDLOG_BUILD_TESTS=OFF
cmake --build build --parallel
```

Configuraciones de la CI:

| Plataforma | Compilador y biblioteca estándar | CMake |
| --- | --- | --- |
| Windows x64 | MSVC 19.40+ | 4.1.1 |
| Linux x86_64 | GCC 16.1.0 / libstdc++ | 4.1.1 |
| Linux x86_64 | Clang 21 de origen / libc++ | 4.3.1 |
| macOS arm64 | Clang 21.1.8 de origen / libc++ | 4.3.1 |

Use el preset correspondiente de [CMakePresets.json](CMakePresets.json) para compilar los ejemplos y ejecutar CTest, por ejemplo:

```bash
cmake --preset ninja-gcc
cmake --build --preset ninja-gcc
ctest --preset ninja-gcc --output-on-failure
```

Las pruebas descargan una revisión fijada de [SpecLab](https://github.com/ambroise-leclerc/SpecLab), solo cuando están activadas. `SourceTreeCoreConsumer` e `InstallTreeCoreConsumer` comprueban por separado el objetivo del núcleo solo. La CI ejecuta además los sanitizadores y los controles del núcleo gobernado; su alcance figura en [la nota de evidencias](docs/governed-evidence.md). Una configuración correcta no es ni una compilación ni un resultado de prueba.

## Licencia y participación

mddlog se ofrece bajo la [Licencia Pública de la Unión Europea 1.2](LICENSE) o bajo condiciones comerciales separadas; véase [LICENSING.md](LICENSING.md). [CONTRIBUTING.md](CONTRIBUTING.md) describe el proceso de contribución por invitación y las reglas de revisión. Para preguntas o defectos, utilice las [issues de GitHub](https://github.com/ambroise-leclerc/mddlog/issues).
