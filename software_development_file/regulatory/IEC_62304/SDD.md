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

Le backend fichiers Linux et le témoin sous UID séparé sont implémentés ; leur profil
physique reste à qualifier. Aucune signature ne prouve l'auteur. Le lecteur matérialise des données dans des conteneurs allouants : ses budgets
restent à établir. AuditService regroupe les primitives de pilotage hors code métier ;
#117 doit borner leur exploitation sur le profil retenu. Les suites de tests et guides de migration liés par les VER décrivent ce qui
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
[L’intégration #130](../../../docs/contextual-api-integration.md) fournit CTRL-014/VER-020–022 :
composant stock, équivalence avant/après et producteurs/consommateur avec arrêt explicite.
La décision de clôture du rapport d'intégration enregistre revue AM-L et confirmation
explicite d'Ambroise Leclerc, pour les lots livrés sur `develop` à `2588124`. GAP-006 est clos ;
REQ-009/REQ-011 sont implémentées avec la réserve d'intégration locale. L'application indépendante reste #122. RISK-008/010 bénéficient des contrôles de copies, de séparation
et d'admission visible ; les risques restent ouverts et l'hôte maîtrise toujours SPSC,
durée de vie des destinations et réponse aux refus. Aucune confidentialité ou qualification
de dispositif n'est fournie ; RISK-009 conserve sa limite.

## Premier backend fichiers Linux (#114)

CTRL-007 inclut `FileStorageMedium`, facultatif dans l'adaptateur. Le
[contrat](../../../docs/file-storage.md) fixe propriété privée, verrou exclusif ou lecteurs
partagés hors ligne, identité opaque, limites, transferts complets et barrière fichier puis
répertoire. Éligibilité non inférée : `Unqualified` répond `Unsupported`. Les erreurs de
mutation arrêtent le rédacteur ; les octets partiels restent pour la reprise existante.
Le compteur privé `.mddlog-refs` réserve chaque référence avant création par écriture
complète, fsync du temporaire, remplacement atomique puis fsync du répertoire, y compris
en mode non qualifié sans confirmation d'audit. Le retrait ne réduit jamais ce compteur ;
un état incomplet ou régressé est refusé sans réparation. Les réouvertures empêchent suivi
de liens, blocage FIFO et fuite à exec ; nom et inode conservé doivent correspondre.
Les rejets de validation portent un errno nul, distinct des erreurs système.
VER-023 à VER-026 et [la campagne locale](../../../docs/file-storage-validation.md) distinguent
réouverture et erreurs injectées de la qualification matérielle encore ouverte en GAP-007.
Les formats, le cœur, les appels métier et les politiques de reprise/rétention ne changent pas.

Le [protocole de campagne](../../../docs/file-storage-test-plan.md) couvre VER-027–029 :
worker Linux séparé, checkpoints observés avant SIGKILL, comparaison directe aux octets
attendus puis aux lectures du backend, refus EACCES sans privilèges et ENOSPC sur tmpfs
privé borné. Le banc vérifie aussi descripteurs et allocation monotone pendant 64 cycles.
Il ne modifie pas le backend ni ne qualifie les caches/alimentation. Les verdicts PARTIAL
et SKIP sont distincts d’une campagne complète ; le job hôte exige le volume réel.

VER-030–035 complètent cette preuve par un modèle abstrait d’admission, trois producteurs
SPSC et les observateurs autorisés, des reprises génératives après réponse du témoin perdue,
libFuzzer borné sur les lecteurs et une comparaison SHA-256 avec hashlib. Le build de fuzzing
est facultatif et instrumente les adaptateurs ; ses options ne se propagent pas au cœur seul.
Les budgets, le corpus et les limites sont dans le
[plan de robustesse](../../../docs/audit-robustness-test-plan.md). L’arrêt de SimpleLogger
change son prédicat sous le mutex de la condition pour ne pas perdre une notification.
Cette synchronisation est CTRL-015, vérifiée par VER-035. Elle ne vérifie ni le transport
borné de CTRL-010/REQ-015 ni les budgets de surcharge de SimpleLogger encore ouverts.
Le [résultat Orin Nano](../../../docs/orin-nano-electrical-results.md) consigne la réussite
électrique déclarée, sans inventer le tuple ni les artefacts nécessaires à sa revue.


## Pilotage centralisé (#116)

CTRL-004/CTRL-007 incluent [AuditService](../../../docs/audit-service.md), propriétaire
du consommateur et du sink avec liaisons explicites. VER-036–040 et VER-045–053 couvrent
budgets et rotation, synchronisation/ancrage inactif, réconciliation des réponses perdues,
santé (dont refus de capacité et d’identité du registre sans inscription), callback
défaillant et arrêt dégradé. La rétention n’agit que sur déclaration ;
les inversions de positions sont signalées sans sous-dépassement des compteurs,
une divergence bloque l’ancrage de la session, et le délai ne masque pas la qualité
d’une fermeture terminée.
L’adaptateur est la source des inscriptions ; le service consulte son compteur et
ses identités. Sans témoin, une fermeture durable peut rester dégradée par absence
d’ancrage. Le rapprochement partiel conserve une couverture et un âge prudents.
une instance échouée ne redémarre pas silencieusement. Le délai d’arrêt est souple et
ne borne pas un appel externe synchrone. VER-044 intègre deux producteurs, le backend
réel et un témoin sous UID séparé. Le [relevé](../../../docs/audit-service-validation.md)
sépare résultats logiciels, CI, budgets du profil (#117) et qualification physique (#122).
Aucun nouveau thread producteur ni composant tiers de déploiement n’est ajouté.

## Position du lecteur persistante (#115, premier lot)

CTRL-006 inclut `FileRetainedPosition`, module Linux facultatif dans l'adaptateur.
Le [contrat](../../../docs/retained-position.md) lie un magasin à un fournisseur,
valide version, checksum, tailles et invariants avant toute restauration, puis sauvegarde
avec génération attendue, verrou non bloquant, fsync fichier, rename et fsync répertoire.
Absence, corruption, migration implicite, conflit et régression ont des refus distincts ;
un échec après rename requiert réconciliation sans affirmation de durabilité. VER-041
exerce ces chemins et le rollback après redémarrage du lecteur avec témoin en mémoire.
GAP-008 demeure ouvert pour acceptation et qualification ; le service authentifié
et la séparation des autorités sont complétés par le lot témoin ci-dessous.

## Témoin indépendant livré (#115)

CTRL-006 comprend désormais l'[autorité fichiers et le service Unix](../../../docs/independent-witness.md).
`FileAnchorAuthority` valide entièrement l'état, tient le verrou du répertoire,
confirme fichier/rename/répertoire avant stamp et arrête toutes les opérations après
échec. `UnixAnchorProvider` épingle UID/identité, borne le transport et garde les
refus ADR-004 lors des retries ; une réponse perdue nécessite réconciliation.
Le service applique les permissions UID/flux/opération. VER-042/043 vérifient persistance,
retraits, corruption, droits, timeout et réponses perdues ; VER-044 utilise quatre
UID et des processus distincts avec AuditService, backend réel et checkpoint du lecteur.
Le protocole v1 borne les frames/inventaires, sans budget temporel de fsync ; le
profil et ses réserves sont dans la campagne. GAP-008 reste ouvert pour acceptation
et qualification physique, pas pour une implémentation du service encore absente.

## Ressources déclarées (#117)

REQ-017 et CTRL-016 ajoutent `AuditResourceLimits` aux configurations de stockage
et de vérification. Le [profil et contrat](../../../docs/audit-resource-budgets.md)
distingue volume, cardinalités, chunks, travail par lecture et nombre d’appels au témoin.
Les métadonnées sont contrôlées avant la première lecture de données ; le fournisseur
et le checkpoint sont contrôlés avant croissance des inventaires du lecteur.
Le rapport interrompu expose son motif et écarte les verdicts partiels ; le checkpoint
candidat n’est publié qu’après une passe sans refus de ressources. La reprise ne
transforme pas un historique hors profil en origin, et la rétention ne supprime rien
sur cette base. Les défauts demeurent finis et configurables dans l’adaptateur.
VER-054–059 portent limites, mémoire, historique et équivalence du corpus existant.
Le [relevé](../../../docs/audit-resource-validation.md) sépare croissance bornée,
RSS et temps mesurés ; pas d’allocateur à quota ni de preuve WCET. Les backends
personnalisés bornent leurs allocations internes selon leur contrat. La politique
diagnostique sous surcharge de REQ-012 reste dans GAP-011 ; aucune revendication MCU.


Le lot de clôture #116 complète VER-059/VER-060 : observateurs permis sous profils
explicites, mesure des passes/arrêt sous saturation et retour du témoin, et refus
de ressources conservé dans `Degraded` après fermeture. Le dernier critère logiciel
de GAP-009 dispose ainsi de sa preuve de budget ; revue/fusion restent distinctes
de la qualification et de l’acceptation système de GAP-015.
