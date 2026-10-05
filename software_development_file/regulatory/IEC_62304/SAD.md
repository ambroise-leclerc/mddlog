# Architecture logicielle

Maîtrise et statut : [index](../../README.md). Sources :
[CMake](../../../CMakeLists.txt) et [ADR](../../../docs/adr/README.md).

## Décomposition effective de la v0.2.0

`mddlog::core` fournit InlineString, LogLevel, WriteResult, GovernedRecord, RingLog,
AuditEvent et AuditRing. Les producteurs copient les données dans des valeurs à capacité fixe,
fournissent le temps et utilisent un anneau SPSC par producteur. Le cœur ne réalise ni formatage,
ni accès disque, ni ancrage, ni gestion de clés. Les contrôles de frontière sont décrits par
[la note de preuves](../../../docs/governed-evidence.md), reliés par CTRL-001/VER-001.

`mddlog::mddlog` dépend du cœur ; il fournit SimpleLogger/Log, TextLogger, SinkRegistry,
RingSinkAdapter, TransportConsumer et AuditSinkAdapter, les sinks et la chaîne de persistance.
Canonical/Chain/Sha256 produisent les octets et digests ; Medium/Layout/Store structurent et
synchronisent les segments ; Ledger/Log/LogVerifier assurent reprise, rétention et lecture ;
Anchor/Verifier gèrent l'ancrage et la position retenue. La liste exhaustive des modules reste
le FILE_SET CMake. Le logger diagnostique alloue et sa file asynchrone n'est pas bornée.

## Interfaces, responsabilités et confiance

Producteur → anneau : résultat d'admission explicite, données possédées, ordre par flux.
Anneau → consommateur : vue puis acquittement des éléments réellement transmis.
Consommateur → sink : acceptation de transmission ; elle ne confirme pas la durabilité.
Sink persistant → support : append puis sync selon le contrat du support ; la confirmation durable
n'est possible qu'après un sync conforme. Le journal modifiable et le ledger ne sont pas une
racine de confiance. Le fournisseur d'ancrage doit être indépendant du journal ; le lecteur doit
protéger et persister sa position hors de l'autorité du rédacteur pour exclure le rollback combiné.

L'hôte compose objets et durées de vie, fournit identités uniques par producteur/démarrage,
pilote le consommateur unique et définit sa politique d'échec. Les primitives de persistance
sont implémentées avec un support fichiers Linux optionnel et des doubles en mémoire. Le
fournisseur indépendant reste à livrer ; qualification physique, orchestration centralisée
et budgets du lecteur restent les lacunes des #114–#117.

## Budget et ségrégation

La capacité N des anneaux et les capacités des champs sont fixes ; le temps est fourni par l'hôte.
Les détails chiffrés et leur définition restent dans les sources référencées par CTRL-002/003.
Les allocations, I/O, reprises et formatages restent dans les adaptateurs. Les bornes du cœur
ne bornent pas la mémoire du lecteur ni les temps du système entier. Les mesures de #117 et
la maîtrise de surcharge #118 sont prévues. Les dépendances de construction et qualification
sont inventoriées par [SOUP](SOUP.md).

Le premier lot #114 ajoute un adaptateur fichiers Linux optionnel, sans dépendance du cœur.
[Son contrat](../../../docs/file-storage.md) garde l'éligibilité, les E/S et les erreurs de
synchronisation au point de composition. La pile physique et les campagnes de coupure
restent à qualifier ; ni fournisseur ni orchestration complète ne sont présumés livrés.
