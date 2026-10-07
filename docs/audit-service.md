# Pilotage centralisé — #116

`mddlog.adapter.auditservice` compose un consommateur unique et un sink de persistance.
La boucle de l’hôte appelle `poll()`, y compris quand aucun producteur n’émet. Aucun
thread ni attente n’est ajouté au producteur gouverné. La continuation de l’action
métier après un refus reste une décision explicite de l’application.

## Composition, identités et durées de vie

`AuditService::create(medium, config)` valide les budgets positifs, les périodes,
la politique de rétention et les paramètres du sink. Les erreurs distinguent
`InvalidStorage` (avec cause), `InvalidRetention` et `StartupFailed` (avec
`startupCause` pour un registre initial en panne). Un démarrage peut avoir laissé
un registre incomplet : corriger le support puis recomposer avec de nouvelles identités.

Le service possède le consommateur et le sink ; le support, le fournisseur et les
anneaux sont liés et doivent lui survivre. L’hôte attribue centralement une identité
nouvelle par producteur et par démarrage, registre compris. Une chaîne générée localement
ne garantit aucune unicité entre processus. `addRing()` refuse les identités invalides,
dupliquées, égales à celle du registre, l’excès de `maxProducerStreams` et toute
inscription après le premier appel d’exploitation. Terminer les inscriptions avant
les observations concurrentes. Aucune désinscription dynamique n’est proposée.
Les refus d’identité invalide, dupliquée ou réservée au registre, ainsi que les refus
de capacité, incrémentent `delivery.configurationErrors` et publient leur raison dans
`lastIssue`. Ils n’inscrivent pas l’anneau et ne consomment pas de place producteur.

Le callback de pertes s’exécute sur le consommateur. Il ne doit pas réentrer dans le
service ; ses captures doivent lui survivre. Ses exceptions sont isolées et comptées
par `callbackFailures`, après publication indépendante de la perte. La capture faible
du consommateur évite un cycle. Les clocks doivent être monotones et ne pas lancer
d’exception. L’hôte cesse les producteurs et joint les observateurs avant destruction.
Le destructeur détache le sink, sans I/O, fermeture implicite ni événement `Executed`.

## Ordonnancement et politiques

Chaque passage tente au plus `maxRecordsPerRing` événements par anneau et
`maxAttemptsPerPoll` au total. Un refus ou une exception consomme une tentative et
conserve l’événement. L’anneau suivant est visité en premier au passage suivant si
le budget global interrompt le tour : un producteur plein ou défaillant ne monopolise
pas les tentatives. Les budgets maximaux par défaut conservent les appels existants ;
un profil contraint doit donner ses propres valeurs positives.

`tick()` applique `StorageConfig::sync.recordBound` et `ageBound`. Seul un retour
`Durable` du support avance la position confirmée. L’ancrage est éligible dès
`anchorRecordBound` nouvelles positions confirmées ou `anchorAgeBound` depuis la
première confirmation non ancrée. `anchorPeriod` borne la fréquence de reprise par
flux. `maxAnchorStreamsPerPoll` limite les flux tentés, registre inclus, avec rotation.
Un succès libère l’échéancier de reprise : une nouvelle revendication peut satisfaire
son seuil en nombre sans attendre une période de retry. Une tentative peut réconcilier `latest()` puis appeler `advance()` : ce budget ne
compte pas chaque appel réseau. Une position déjà ancrée n’est pas reproposée.

Après une réponse perdue ou indisponible, le sink interroge `latest()` avant tout
rejeu. Une ancre utilisable correspondant exactement à la revendication incertaine
confirme l’ancrage ; une absence ou une position antérieure autorise une nouvelle
tentative. Une divergence ou un retrait demeure une faute d’intégrité. Un refus
explicite de `advance()` conserve son sens ADR-004 et ne devient pas un succès.
L’authentification et l’identité du fournisseur relèvent du provider injecté ; le
profil réel utilise `UnixAnchorProvider` avec UID et identité épinglés.

`KeepPending` est le profil par défaut : aucune suppression automatique. Avec
`RelieveDeclared`, `relieve()` précède le drain et n’exerce que les permissions
`storage.retention.rotate` / `removeEnded`. Ce profil exige un registre et au moins
une permission. Les preuves d’ancrage et les trims durables restent obligatoires
avant reclamation. Si le support est plein, le backlog reste dans les anneaux et
les admissions suivantes peuvent être refusées. La politique ne promet pas une
capacité infinie ni une disponibilité du témoin.

Une instance `Failed` ne redémarre jamais après retour du support. L’hôte conserve
son rapport et ses événements en attente, répare/réouvre le support, puis compose
une nouvelle session avec des identités nouvelles. Le transfert éventuel du backlog
ancien exige une décision explicite de réconciliation ; il n’est pas fait en secret.
`storageSink()` conserve les opérations bas niveau pour le consommateur unique,
pendant la durée de vie du service ; elles ne sont pas nécessaires au code métier.

## Santé et arrêt

`health()` peut être observé concurremment après inscription. Il agrège admissions,
refus pour anneau plein (`ringFullRefusals`, pas les erreurs d’entrée), transmissions,
backlog, pertes signalées, positions écrites/durables/ancrées et compteur du témoin.
`unconfirmed` et `unanchored` somment les événements producteurs, hors registre.
Chaque flux expose `unconfirmedSince` / `unanchoredSince` sur l’horloge monotone du
support ; l’observateur calcule l’âge avec la même base. La capacité, la réserve,
les échecs et les fautes d’intégrité restent dans le snapshot de stockage. Ces
observations ne sont pas une transaction entre producteur et consommateur.

Après cessation et quiescence des producteurs, `stop(maxDrainPasses)` ou
`stop(AuditStopOptions)` réalise les passages autorisés puis sync/close et ancrage
quand les anneaux sont vides. Le rapport distingue `Completed`, `Pending`,
`DeadlineExceeded`, `Degraded` et `InvalidBudget`. `closed` signifie fin du cycle de
vie, indépendamment de la durabilité et de l’ancrage. Le backlog reste explicite et
un arrêt partiel peut être réessayé. Une fermeture dégradée n’efface aucune perte. Son statut tient aussi compte des
positions non confirmées ou non ancrées du registre, même sans événement producteur.
Un appel sur un service fermé ne réexécute pas les I/O.

La limite temporelle est **souple**, vérifiée entre passages et avant fermeture.
Un `sync`, une rétention ou un appel fournisseur synchrone peut la dépasser ; le
rapport le constate au retour. Le provider Unix impose son propre timeout de
transport. Les systèmes de fichiers ne fournissent pas ici de délai maximal pour
`fsync`. Les bornes matérielles, budgets de récupération, allocation et WCET restent
à qualifier dans #117 et #122 ; aucun test logiciel ne vaut qualification électrique.

## Exemples et preuves

[AuditApplication.cpp](../examples/AuditApplication.cpp) compile sur la matrice
portable et regroupe l’exploitation au démarrage et à l’arrêt. `inspect` ne reçoit
qu’un `AuditBinding`. Ses doubles démontrent l’API sans promettre d’indépendance.
[FileAudit.cpp](../examples/FileAudit.cpp) compose le backend réel non qualifié et
vérifie les octets après réouverture. Le test Linux
[WitnessDeployment.cpp](../tests/spec/WitnessDeployment.cpp) compose deux producteurs,
le backend fichiers et le témoin réel, avec des UID distincts et des budgets globaux.
Son `QualifiedFsync` est une déclaration de fixture sur tmpfs, pas une qualification
physique du déploiement.

[AuditServiceSpec.cpp](../tests/spec/AuditServiceSpec.cpp) couvre budgets, saturation,
flux inactif, témoin indisponible/réponse perdue, retour explicite après panne du
support, callbacks, arrêt avec backlog et délai dépassé, santé concurrente et absence
de promotion d’admission à durabilité. Voir [le relevé de validation](audit-service-validation.md)
pour les résultats réellement exécutés et les réserves d’acceptation.
