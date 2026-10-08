# Diagnostic borné et santé (#118)

Statut : conception et implémentation livrées pour revue ; acceptation du lot et qualification
applicative distinctes. Les quatre contrats ADR-001 à ADR-004 restent inchangés. Ce document
précise le contrat de `SimpleLogger`, adaptateur allouant ; il ne transforme pas `TextLogger`
ou les callbacks de `SinkRegistry` en producteurs gouvernés.

## Configuration et admission

`DiagnosticConfig` est immutable après construction. Valeurs par défaut : 1024 messages,
8 flush, 4096 octets cumulés de champs texte par message, 16 sinks. Les capacités doivent être
positives ; une somme messages + flush non représentable est refusée par exception de construction.
La file est un tableau circulaire de slots réservé au démarrage. Les budgets incluent le travail
en cours : le retrait d'un slot par le worker ne libère pas son admission avant traitement.
L'arrêt dispose d'un état réservé unique hors file, partagé par tous ses appelants ; il ne
consomme pas un slot de flush et n'alloue aucune nouvelle commande.

Tous les niveaux, Trace à Fatal, refusent immédiatement le nouveau message quand la capacité
est atteinte. Aucune priorité, éviction d'un message admis, tentative de blocage sur un sink,
troncature ou nouvelle tentative automatique. Le refus est compté une fois par événement,
indépendamment du nombre de sinks. `tryLog` expose le résultat ; `log`, les raccourcis par niveau,
`logMedical` et les appels contextuels conservent leurs signatures `void` et leur localisation.

Le filtre précède allocation et admission. Les appels désactivés ne changent pas les compteurs ;
les arguments ordinaires restent évalués par C++, et `DiagnosticBinding::logLazy` conserve son
filtrage avant fabrique. L'admission fermée, la taille excessive et la saturation sont vérifiées,
dans cet ordre, avant construction du record. Message, composant/catégorie, identifiants médicaux,
opération et corrélation entrent dans le budget d'octets. Les champs vides, la source statique,
l'horodatage et les objets internes ont un coût fixe distinct. Aucun record personnalisé ou
metadata de taille arbitraire n'est injectable dans cette file. Une erreur de construction du
record, notamment `bad_alloc`, renvoie `InternalFailure`, incrémente `internalFailures` et
ne modifie pas les admissions ; les raccourcis `void` laissent ce résultat dans la santé.
La construction initiale du logger et les allocations des arguments effectuées par l'appelant
restent susceptibles de lever une exception.

Les émissions asynchrones admises sont FIFO selon leur acquisition du verrou d'admission ; aucun
ordre total des instants d'appel concurrents n'est promis. Le mode synchrone vérifie les refus
avant l'attente sur `deliveryMutex`, puis sérialise émission et flush et revérifie l'admission
pour observer une fermeture survenue pendant l'attente. Un appel admissible peut attendre un
sink actif ; la saturation est possible si sa capacité message est déjà occupée. Aucun sink
n'est appelé sous verrou d'admission ou de registre ; les callbacks synchrones s'exécutent
sous `deliveryMutex`, ce qui impose leur sérialisation.

## Flush, arrêt et erreurs

`flushChecked()` renvoie `DiagnosticFlushResult`, contenant le statut global et l'identité
possédée (`SinkPtr`) et la réussite de chaque sink du snapshot engagé. `flush()` conserve `void` ;
ses erreurs sont dans `health()` et `Sink::getStatistics().flushFailures`, distinctes des pertes
d'écriture. Les sinks désactivés sont également flushés. Aucun sink présent est un succès vide.
Une exception, ou `recordFlushFailure()` appelé par le sink, rend son résultat faux et le statut
`SinkFailure` ; les autres sinks sont toujours tentés. Une absence de signalement d’erreur par
un sink ne peut pas être détectée par l’adaptateur. ConsoleSink signale aussi les états
d’erreur de ses streams lorsque les exceptions iostream sont désactivées ; son destructeur
ne laisse pas échapper une exception de flush. `failbit`/`badbit` persistent jusqu'à une
récupération explicite du flux par l'hôte : chaque tentative suivante reste en échec. Le sink
ne fait pas de `clear()` automatique, qui masquerait une erreur sans réparer le support.
Un retour réussi n'est ni une garantie de
persistance, ni une affirmation que chaque écriture antérieure a réussi : celles-ci sont des
tentatives terminées, avec leurs échecs dans la santé.

Un flush asynchrone occupe une commande FIFO : les messages admis avant sont traités avant lui,
ceux admis après sont traités après. Un flux continu n'affame donc pas sa barrière. `flushFor`
borne l'attente sur la condition de complétion et renvoie `Timeout` sans annuler la commande.
Son budget reste occupé jusqu'à son traitement, même après expiration. `Saturated`, `Stopped`
et `Reentrant` refusent la commande sans allocation. Une expiration n'est jamais un succès ;
le résultat final d'une commande expirée reste observable dans les compteurs, sans nouvelle
file de résultats conservés. Les délais dépendent de l'ordonnanceur et de l'acquisition des
verrous courts ; aucun plafond temps réel du temps total de l'appel n'est revendiqué.

`shutdown()` ferme l’admission diagnostique atomiquement, attend tous les événements et flush admis puis
exécute exactement un flush final. Ses appels concurrents partagent le résultat final par sink.
`shutdownFor` ferme de la même façon et borne seulement l'attente asynchrone ; un timeout laisse
le worker vivant et fermé aux admissions. Le destructeur attend la complétion et joint le thread.
Il n'existe pas de détachement, abandon d'événements admis, interruption d'un sink hôte ou destruction
sûre d'un logger depuis son callback. Les autres appels doivent être terminés avant destruction.
En mode synchrone, les opérations avec délai renvoient `Unsupported` sans appeler de sink ; les
opérations bloquantes restent disponibles.

Les échecs d'écriture sont signalés par `recordsDropped` ou exception. Une exception incrémente
la statistique du sink si celui-ci n'a pas déjà signalé la perte ; la différence du compteur
alimente `writeFailures`. Le sink reste inscrit pour les tentatives suivantes ; il peut être
désactivé ou retiré par le superviseur. Les erreurs ne sont jamais écrites dans un autre diagnostic.
Les erreurs internes de préparation d'un record, d'une complétion, d'un snapshot ou de copie
d'un résultat sont comptées dans `internalFailures` ; un
flush affecté renvoie `InternalFailure`, sans inventer des résultats de sinks non appelés.

## Sinks, rappels et durée de vie

Le nombre de sinks et les duplications de la même identité sont bornés/refusés (`tryAddSink`,
`sinkRefused`). Les invocations utilisent une copie du registre avec propriété partagée. Ajout,
retrait et clear sont autorisés concurremment et depuis un callback ; les noms sont lus hors
verrou de registre, puis les identités correspondantes sont retirées. Un ajout postérieur au
snapshot n'est pas engagé. Un retrait retire des snapshots futurs, sans attendre la quiescence
des snapshots déjà engagés ; il conserve leur propriété et ne détruit pas les ressources hôte
prématurément. Les ressources libérées par retrait/clear sont détruites hors verrou de registre.
Ce contrat est propre à l'interface de sinks objets ; `SinkRegistry::remove` conserve intégralement
son contrat de quiescence ADR-003 pour les callbacks empruntant des ressources externes.

Un garde par thread empêche une émission ou commande de flush/arrêt depuis une invocation de
sink vers n'importe quel `SimpleLogger` : résultat `Reentrant`, émission comptée ou flush refusé.
`Log::shutdownChecked()` renvoie `Reentrant` depuis un callback, conserve le logger et sa liaison
audit, et compte le refus dans `shutdownRefused` pour éviter la destruction sur le worker.
`Log::shutdown()` conserve sa signature `void` et délègue à cette opération ;
l'arrêt global appartient au superviseur. Les flush `Log::flushChecked()` / `Log::flushFor()`
et l'arrêt vérifié n'initialisent jamais une instance absente : ils renvoient `Stopped`.
Les émissions et le flush historique `void` conservent leur initialisation implicite. La garde ne couvre pas un rappel différé sur un autre
thread : le sink/transport hôte doit rompre ce cycle avant de réémettre, conformément à ADR-003.
Les autres getters, la santé, les filtres et les opérations de registre restent utilisables.
Les ajouts après fermeture sont refusés et comptés ; les noms de sinks restent des vues stables,
et `getName()` ne modifie pas le logger.

Un sink doit documenter sa durée maximale de `write`, `flush` et de libération, et revenir même
en erreur. Un sink lent retarde les suivants, mais ne fait croître aucune des deux files. Un sink
bloqué indéfiniment empêche drainage et destruction ; aucun délai de l'adaptateur ne peut modifier
ce contrat hôte. Un sink partagé par plusieurs loggers doit fournir sa propre synchronisation ;
les deltas de statistiques du logger ne sont exacts que si les écritures du sink sont exclusivement
pilotées par lui et ses compteurs ne sont pas remis à zéro pendant le traitement.

## Mémoire et supervision

`health()` est une copie cohérente sous le verrou d'admission, sans appel de sink. `messages` et
`flushes` incluent le travail engagé ; hauts niveaux, pertes, admissions, traitements, refus de
commande, expirations, échecs d'écriture et de flush sont séparés. `processed` compte les records
traités, même si aucun sink n'a réussi. Une fois arrêté : `admitted == processed`, files vides,
`flushAdmitted == flushCompleted`. Les compteurs uint64 peuvent reboucler après leur capacité.

La mémoire retenue par le logger dépend de M, C, B, S configurés, pas du nombre de tentatives :
M+C slots fixes, au plus M records de B octets de contenu (plus objets et overhead d'allocation),
au plus C complétions de flush, une complétion d'arrêt, et snapshots/résultats d'au plus S sinks.
Un résultat conservé par l'appelant et la mémoire propre des sinks relèvent de l'hôte. La construction
est sérialisée sous admission : les producteurs concurrents n'accumulent pas de records alloués
hors budget. La complétion d'une commande et sa libération peuvent conserver transitoirement
un objet supplémentaire dans le worker ; c'est un coût constant. Le tableau reste réservé après
arrêt. Aucun nombre exact d'octets malloc, aucune absence générale d'allocation ou latence temps
réel n'est déduit de ces bornes structurelles.

Pour les producteurs critiques, utiliser `GovernedBinding` / `RingLog<N>` par producteur et un
drain contrôlé ([migration](migration/governed-core.md)). L'audit utilise exclusivement sa propre
admission gouvernée ; ses événements ne passent jamais dans cette file de diagnostics.
`TextLogger` conserve ses allocations et callbacks synchrones ; il n'a pas de flush asynchrone
ni les garanties de ce profil. Aucun nouveau module ou composant tiers n'est ajouté par #118.
