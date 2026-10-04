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

## Proposition de contextes pour la 1.0 (#113/#127)

[ADR-005](../../../docs/adr/ADR-005-contextual-logging-api.md) est **proposée**, sans
modification des modules publics ou des formats. Elle précise capture possédée, contexte
d'opération indépendant, origine diagnostic, temps gouverné par événement, filtre avant
fabrique de message et phases d'audit explicites. Les liaisons empruntent un anneau par
producteur ; elles ne modifient pas SPSC et leurs destructeurs n'émettent rien.

L'[étude](../../../docs/contextual-api-study.md) décrit l'exécutable et ses limites.
GAP-006 suit l'acceptation #127 puis réalisation diagnostic #128, audit #129 et preuves
#130. REQ-009/REQ-011 restent prévues ; RISK-008/009/010 ne sont pas clos par cette
proposition. Les mesures envisagées sont la copie exacte des identifiants, des contextes
indépendants et une politique d'admission visible ; aucune qualification de ces mesures
n'est revendiquée. Aucun composant tiers nouveau : la chaîne DEP existante reste utilisée.
