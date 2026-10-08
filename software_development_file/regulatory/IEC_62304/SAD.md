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

Le [premier lot de #115](../../../docs/retained-position.md) ajoute une sauvegarde du
lecteur Linux à la zone adaptateur. Le magasin est lié à une seule identité fournisseur,
hors droits du rédacteur selon le déploiement. Aucun appel métier ni dépendance du cœur
n'est ajouté. Le stockage du lecteur, celui du journal et l'autorité du futur témoin sont
trois responsabilités séparées ; un fichier sous les mêmes droits ne démontre pas cette
architecture. Le lot témoin ci-dessous complète cette sauvegarde ; acceptation et qualification
demeurent dans GAP-008.

Le [lot complet #115](../../../docs/independent-witness.md) livre l'autorité fichiers,
le service sous UID propre et le client Unix authentifié dans l'adaptateur. Le noyau
atteste les UID ; le policy point de composition sépare avancement, retrait et lecture
par flux/autorité. La propriété privée du state du service et du lecteur s'exerce dans
VER-044 ; l'interface du producteur et la séparation du cœur ne changent pas. La garde
persiste au-delà des redémarrages indépendants ; la qualification/acceptation du profil
cible demeure dans GAP-008/#122.

Le lot #117 borne les lecteurs avec REQ-017/CTRL-016 et VER-054–059 : inventaire avant
lectures, chunks, volumes et cardinalités finis, budget fournisseur global et checkpoint
candidat. Le [profil Linux x86_64](../../../docs/audit-resource-budgets.md) conserve
une image plafonnée du journal ; il ne garantit pas une mémoire constante ou un WCET.
Les autres plateformes doivent mesurer leur propre profil avant de revendiquer ces
budgets. La qualification indépendante reste en GAP-015.

## Livraison diagnostique #118

SimpleLogger possède désormais un tableau circulaire borné et une santé indépendante,
avec configuration centralisée également accessible via Log. TextLogger reste synchrone et
allouant. Aucun module, composant tiers, dépendance de déploiement ni chemin d’audit nouveau.
CTRL-017 et VER-064/065 relient sources, conception et [preuves](../../../docs/diagnostic-validation.md).
Les limites de durée des sinks, mémoire propre hôte et qualification du profil restent explicites.
