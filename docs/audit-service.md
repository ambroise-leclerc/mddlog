# Pilotage centralisé — #116

`mddlog.adapter.auditservice` compose un consommateur unique et un sink de persistance.
La boucle de l’hôte appelle `poll()`, y compris quand aucun producteur n’émet. Aucun
thread ni attente n’est ajouté au producteur gouverné. La continuation de l’action
métier après un refus reste une décision explicite de l’application.

## Composition, identités et durées de vie

`AuditService::create(medium, config)` valide les budgets positifs, les périodes,
la politique de rétention et les paramètres du sink. Les erreurs distinguent
`InvalidRecordBudget` (`maxRecordsPerRing`), `InvalidAttemptBudget`,
`InvalidAnchorStreamBudget`, `InvalidAnchorPeriod`, `InvalidAnchorAgeBound` et
`InvalidAnchorRecordBound`, puis
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
L’adaptateur détient seul les inscriptions : le service consulte
`hasRegisteredStream()` et `registeredRingCount()` pour les doublons et la limite.
Ces requêtes de métadonnées appartiennent au consommateur unique.

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
Le défaut `maxRecordsPerRing = 1` fait un premier tour peu coûteux de tous les
producteurs. Les deux budgets globaux sont illimités par défaut pour conserver
le comportement existant ; augmenter le budget par anneau selon le débit attendu.

`tick()` applique `StorageConfig::sync.recordBound` et `ageBound`. Seul un retour
`Durable` du support avance la position confirmée. L’ancrage est éligible dès
`anchorRecordBound` nouvelles positions confirmées ou `anchorAgeBound` depuis la
première confirmation non ancrée. `anchorPeriod` borne la fréquence de reprise par
flux. `maxAnchorStreamsPerPoll` limite les flux tentés, registre inclus, avec rotation.
Un succès libère l’échéancier de reprise : une nouvelle revendication peut satisfaire
son seuil en nombre sans attendre une période de retry. Une tentative peut réconcilier `latest()` puis appeler `advance()` : ce budget ne
compte pas chaque appel réseau. Une position déjà ancrée n’est pas reproposée.
Quand `unanchoredSince` est absent et qu’un écart confirmé existe, son âge est
inconnu : le flux est éligible immédiatement, même sous le seuil en nombre.

Après une réponse perdue ou indisponible, le sink interroge `latest()` avant tout
rejeu. Une ancre utilisable correspondant exactement à la revendication incertaine
confirme l’ancrage ; une absence ou une position antérieure autorise une nouvelle
tentative. Une divergence ou un retrait demeure une faute d’intégrité.
La position d’une ancre plus ancienne n’est pas importée en santé : son digest n’a
pas été rapproché du préfixe local. L’exposition reste donc conservatrice jusqu’à
un rapprochement exact ou une nouvelle acceptation. `LatestAnswer` comporte
actuellement quatre variants : ancre, retraite, absence et indisponibilité.
Seules une ancre utilisable plus ancienne ou une `AnchorAbsent` explicite autorisent
le rejeu ; une réponse non reconnue conserverait la revendication incertaine.
La date `unanchoredSince` reste la plus ancienne date d’exposition connue tant
qu’un écart subsiste, même après rapprochement partiel. Elle peut donc provoquer
un ancrage anticipé ; elle n’est pas une date exacte du premier événement encore
non ancré. La santé n’expose plus cette date quand l’écart est entièrement couvert.
Un refus explicite de `advance()` conserve son sens ADR-004 et ne devient pas un succès.
Une divergence bloque les tentatives suivantes pour cette instance, y compris à
la fermeture ; `anchorBlocked` et la faute d’intégrité restent publiés. Une reprise
exige une réconciliation explicite de l’hôte et une nouvelle session. Les fautes
identiques étaient déjà dédupliquées ; ce blocage évite aussi les appels répétés.
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
Les écarts par flux sont bornés à zéro si les positions sont inversées.
`positionOrderViolations` signale cette anomalie d’intégrité, registre inclus, sans
la masquer par un compteur d’exposition non signé immense. Le sink publie les
positions qu’il a confirmées et ancrées ; une ancre distante divergente ne les remplace pas.

Après cessation et quiescence des producteurs, `stop(maxDrainPasses)` ou
`stop(AuditStopOptions)` réalise les passages autorisés puis demande la fermeture
au sink quand les anneaux sont vides. Le sink synchronise les flux ; avec un registre,
il tente aussi leurs ancrages et celui du registre. Sans registre, sa fermeture réalise
un flush ; les positions confirmées à ce moment peuvent donc rester non ancrées,
même avec un fournisseur configuré. Le rapport distingue `Completed`, `Pending`,
`DeadlineExceeded`, `Degraded` et `InvalidBudget`. `closed` signifie fin du cycle de
vie, indépendamment de la durabilité et de l’ancrage. Le backlog reste explicite et
un arrêt partiel peut être réessayé. Une fermeture dégradée n’efface aucune perte. Son statut tient aussi compte des
positions non confirmées ou non ancrées du registre, même sans événement producteur.
Sans fournisseur d’ancrage, les positions durables non ancrées restent exposées et
une fermeture propre avec de telles positions est `Degraded`. Ce statut exprime
une preuve d’audit incomplète ; il ne signifie pas nécessairement une panne du support.
`unconfirmed`, les pertes et les fautes permettent de distinguer ces situations.
Un service vide sans registre ni données peut se fermer avec `Completed` sans témoin.
Un appel sur un service fermé ne réexécute pas les I/O.
`DeadlineExceeded` s’applique seulement à un arrêt encore ouvert. Une fermeture
terminée conserve `Completed` ou `Degraded`, même si une phase synchrone lente
dépasse le délai ; `elapsed` permet à l’hôte de constater ce dépassement.
Si le délai expire après la dernière passe, même avec des anneaux désormais vides,
la fermeture n’est pas commencée. Elle peut encore exiger une synchronisation et
des appels au témoin ; un nouvel appel `stop()` sans ce délai peut la terminer.
`stop(0)` sélectionne la surcharge en nombre de passages et ne tente aucun drain.
Des événements en attente donnent `Pending` ; avec des anneaux vides, la fermeture
est tentée. Zéro passage est autorisé, contrairement à un budget temporel nul.

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
