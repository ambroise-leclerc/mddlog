# Conception détaillée et contrats d'erreur

Maîtrise et statut : [index](../../README.md). Les CTRL et VER du [registre](../../register.json)
relient ces contrats aux sources et tests ; les ADR restent la définition de conception.

## Diagnostic gouverné

InlineString possède ses octets. GovernedRecord tronque le message à une frontière UTF-8
avec résultat explicite ; il refuse les identifiants trop longs. RawTime ne consulte pas
l'horloge. RingLog<N> accepte ou refuse sans attendre un sink ; `drain()` expose deux spans,
à traiter avant l'acquittement qui permet de réutiliser les slots. Un producteur et un consommateur
par anneau ; ni ordre global entre anneaux ni permission de plusieurs producteurs sur le même.

## Audit

AuditInput sépare catégorie et phase. AuditEvent exige les identifiants conformes aux champs
et conserve une identité de flux et une séquence. `AuditEvent::assign` autorise les actions
préfixées par `mddlog.` pour permettre à l'adaptateur de construire les enregistrements du ledger.
`AuditRing::tryRecord` refuse ce préfixe réservé lors de l'admission producteur et attribue une
séquence à l'admission. Une admission ne signifie ni exécution de l'action,
ni effet clinique, ni durabilité. L'hôte vérifie le résultat et pilote sa réponse au refus.
Une portée détruite ne prouve pas la réussite d'une action ; `Requested`, `Confirmed`, `Executed` et `Failed`
sont des déclarations explicites. AuditSinkAdapter rend refus et reprise de transmission visibles.

## Adaptation et persistance

SinkRegistry protège inscription et retrait ; TransportConsumer découple producteur et transport
avec une file bornée. Cela ne borne pas automatiquement la file de SimpleLogger. TextLogger
fournit groupes et callbacks synchrones ; le contexte réutilisable unifié de #113 reste prévu.
REQ-015 relie la file bornée du transport à CTRL-010/VER-010. REQ-012 ne revendique aucune
couverture de ces tests pour la mémoire de SimpleLogger ou la lecture d'archives ; ces budgets
restent à qualifier dans GAP-010/GAP-011. Les champs bornés de REQ-002 conservent leur couverture
unitaire existante et une qualification du profil explicitement ouverte dans GAP-018.

Le contrat canonique versionné et le chaînage déterministe sont contrôlés par vecteurs de
référence. PersistingAuditSink distingue append, synchronisation, état durable et santé.
Le ledger décrit continuité, fermeture, rotation et retrait. La vérification signale couverture
d'ancrage, incohérence, suffixe absent et rollback selon les informations disponibles ; sans
position retenue indépendante, un ancien journal et son ancien ancrage ne permettent pas
l'exclusion du rollback. Les règles exactes restent dans ADR-004 et les sources de CTRL-005–008.

## Limites de conception ouvertes

Aucun backend fichier/flash ni fournisseur indépendant réel n'est livré. Aucune signature ne
prouve l'auteur. Le lecteur matérialise des données dans des conteneurs allouants : ses budgets
restent à établir. L'hôte appelle aujourd'hui les primitives de pilotage ; #116 doit les regrouper
hors code métier. Les suites de tests et guides de migration liés par les VER décrivent ce qui
est exercé ; leur couverture n'équivaut pas à une validation applicative réelle.

## Contextes et liaisons pour la 1.0 (#113/#127–129)

[ADR-005](../../../docs/adr/ADR-005-contextual-logging-api.md) est acceptée le 2026-10-04
sur `f3063bc`. CTRL-011–013 sont implémentés : valeurs possédées et bornées, factories à
refus typé, contextes dérivés indépendants, liaison diagnostic et description/contexte audit
séparés. Les liaisons empruntent un anneau par producteur sans changer SPSC ; les destructeurs
n'émettent rien. Le temps gouverné et les phases restent explicites par événement, le canal
et les résultats d'admission restent ceux d'ADR-001/002. Les formats d'ADR-004 ne changent pas.

[La référence/migration](../../../docs/migration/contextual-logging.md) précise signatures,
origines, durée de vie des vues et filtres avant fabrique. SimpleLogger expose son filtre
et reçoit les champs structurés ; TextLogger reçoit une projection texte. Aucune allocation
ou horloge de ces adaptateurs ne remonte dans le cœur. std::expected est utilisé seulement
pour la construction gouvernée sans accesseur levant ; aucun composant tiers nouveau.

[Les preuves locales](../../../docs/contextual-api-validation.md) couvrent VER-011–019,
les consommateurs sources/installés, la frontière et les tailles sur un tuple précis.
GAP-006 reste ouvert pour la revue d'intégration #130 ; REQ-009/REQ-011 restent prévues
pour l'expérience complète. RISK-008/010 bénéficient des contrôles de copies, de séparation
et d'admission visible ; les risques restent ouverts et l'hôte maîtrise toujours SPSC,
durée de vie des destinations et réponse aux refus. Aucune confidentialité ou qualification
de dispositif n'est fournie ; RISK-009 conserve sa limite.
