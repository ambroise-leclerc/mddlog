# Témoin indépendant Linux — #115 / #112

Ce lot implémente un témoin persistant dans un processus et sous une autorité système
séparés du rédacteur. Il complète la [position retenue](retained-position.md).
Les quatre opérations d'ADR-004 restent `advance`, `retire`, `latest` et `streams` ;
aucun appel d'émission ni résultat d'admission ne change.

## Frontière de confiance et enrôlement

Le profil de référence distingue quatre autorités :

| Autorité | Droits |
| --- | --- |
| Service témoin | Répertoire de state privé, identité fournisseur, socket ; applique les transactions |
| Rédacteur | Journal et avancement des flux explicitement enrôlés ; lecture pour réconciliation |
| Autorité de retrait | Retrait des flux explicitement enrôlés ; lecture pour réconciliation |
| Lecteur | Lecture du témoin, images hors ligne du journal et magasin de position retenue privé |

Le service s'exécute sous un UID différent des trois autres rôles. Le rédacteur ne peut
modifier ni lire les fichiers du témoin ni le checkpoint du lecteur. Avancement et retrait
peuvent être affectés à des UID distincts ; les permissions sont des correspondances exactes
`UID + streamId + opération`. Aucun préfixe ou wildcard implicite. Un UID absent de la politique
est refusé avant réception du corps. Les lecteurs autorisés peuvent lire l'ensemble de
l'inventaire, y compris les identifiants des flux retirés ; aucune confidentialité entre eux
n'est fournie. Le profil doit définir les données autorisées dans ces identifiants.

Le client authentifie le service avec `SO_PEERCRED` et un UID épinglé avant d'envoyer sa
requête. Le service utilise les credentials fournis par le noyau pour authentifier le client.
L'identité fournisseur est enrôlée explicitement, présente dans chaque requête, sauvegardée
avec l'état et vérifiée dans les stamps/inventaires. Changer cette identité bloque la
restauration. La réaffectation des UID ou le remplacement du fournisseur impose une migration
administrée : arrêt, export/revue, nouvelle politique et nouvelle référence du lecteur,
sans effacer silencieusement ses checkpoints. Il n'existe pas de migration automatique.

Ces credentials se réfèrent au namespace utilisateur partagé par service et clients.
La protection repose sur le noyau et la maîtrise des UID, parents, ACL, montages et comptes
privilégiés. Ni root ni un témoin de confiance compromis ne sont dans la résistance annoncée.
Une somme SHA-256 n'authentifie pas un fichier. Ce profil local ne fournit ni TLS distant
ni signature autonome (#123). L'API de service permet le même UID pour les tests de protocole ;
un tel déploiement ne constitue pas un témoin indépendant. L'exécutable de référence refuse
cette coïncidence pour le service et les trois rôles.

## Composition centrale et déploiement

Modules Linux facultatifs, dans `mddlog::mddlog`, avec `MDDLOG_BUILD_FILE_STORAGE=ON` :

- `mddlog.adapter.fileanchorauthority` : autorité durable exclusivement possédée par le service.
- `mddlog.adapter.unixanchorprovider` : `UnixAnchorProvider` et `UnixWitnessService`.
- `mddlog.adapter.witnesscodec` : encodage borné des échanges et de l'état.

`examples/WitnessService.cpp` construit `mddlog_witness` quand les exemples sont activés.
Avec `MDDLOG_INSTALL_EXAMPLES=ON`, cet exécutable est installé dans `bin`. La bibliothèque
et ses imports restent utilisables depuis le package installé sans installer l'exécutable.

L'administrateur provisionne et rend durables les répertoires du service, du journal et du
lecteur sous leurs UID, avant l'enrôlement. State du témoin et position du lecteur : mode
0700, fichiers 0600, aucun droit d'écriture du rédacteur sur les parents. Parent du socket :
propriété du service, mode 0711 (ou accès de traversée équivalent), aucune écriture groupe/autres.
La socket est 0666 pour permettre la connexion aux UID enrôlés ; les credentials et la
politique contrôlent les opérations. Le service refuse un parent final symbolique, mal
possédé ou inscriptible par d'autres. Ces contrôles ne vérifient pas toutes les ACL/ascendances.
`O_NOFOLLOW` ne porte que sur le dernier composant du parent : un lien symbolique dans
un ancêtre n'est pas détecté. Le profil doit contrôler toute l'ascendance, ses liens,
ACL et montages, et empêcher sa substitution pendant la vie du service.

Exemple, après création des comptes et des répertoires par l'administrateur :

```bash
# UID illustratifs : témoin 2001, rédacteur 2002, retrait 2003, lecteur 2004.
# Exécuter les deux commandes sous le compte du témoin, jamais sous celui du rédacteur.
mddlog_witness init /var/lib/mddlog-witness site-a-witness
mddlog_witness serve /var/lib/mddlog-witness /run/mddlog-witness/socket \
  site-a-witness 2002 2003 2004 device/boot-1 ledger/boot-1
```

`init` est une décision explicite d'enrôlement. Une sauvegarde présente, même corrompue,
est refusée. `serve` exige l'état existant et validé ; une absence ne recrée jamais un fournisseur
vide. Après arrêt du service, l'administrateur retire la socket orpheline avant le prochain
`serve`, ou utilise un répertoire runtime géré par son superviseur. Aucun endpoint existant
n'est supprimé automatiquement par la bibliothèque. SIGINT/SIGTERM arrêtent la boucle après
l'opération en cours ; aucune acceptation n'est créée à la destruction. Un échec de stockage
fait sortir l'exécutable. La reprise nécessite une restauration complète réussie.

Au point de composition de l'application :

```cpp
UnixAnchorProvider witness({
    .socketPath = "/run/mddlog-witness/socket",
    .providerId = "site-a-witness",
    .serverUid = 2001,
    .timeout = std::chrono::milliseconds{1000},
});
StorageConfig storage = /* configuration du journal et du ledger de cet hôte */;
storage.provider = &witness;
auto service = AuditService::create(medium, {.storage = storage});
// witness et medium survivent au service. L'émission reste audit.record(...).
```

L'hôte enrôle aussi les identités de ledger et les nouvelles sessions de producteurs.
Il conserve les tombstones des anciennes identités dans le fournisseur ; une nouvelle
session n'est pas une permission de réutiliser un flux retiré. Les appels de diagnostic
et d'audit n'incluent aucune donnée d'accès au service.

## Persistance de l'autorité

`FileAnchorAuthority::initialize` crée un état vide explicitement ; `open` ne l'initialise
jamais. Le répertoire privé est verrouillé exclusivement pendant toute la vie de l'autorité.
Un deuxième service reçoit `Busy`. Lectures relatives au descripteur, CLOEXEC, NOFOLLOW,
fichiers réguliers privés avec un seul lien : les substitutions finales ne sont pas suivies.
Un fichier manquant, tronqué, corrompu, d'une version inconnue ou d'une autre identité bloque
l'ouverture. Le fichier et le répertoire sont resynchronisés avant exposition de l'état
restauré, pour confirmer un rename visible issu d'une transaction incertaine précédente.

Une mutation construit un état candidat sans modifier l'état en mémoire servi. Elle écrit
entièrement `witness.tmp` (reprises EINTR/écritures courtes), fait fsync du fichier,
rename atomique vers `witness.bin`, puis fsync du répertoire. L'état servi et la réponse
positive ne changent qu'après succès de toutes les étapes. Toute erreur d'écriture/barrière
rend les quatre opérations indisponibles jusqu'à réouverture. Le temporaire orphelin est
ignoré à la restauration, puis éliminé sous le verrou avant la prochaine transaction.
Un refus de capacité avant toute écriture renvoie `ProviderUnavailable` pour la mutation,
sans positionner `lastError` ni arrêter le service. `latest` et `streams` restent disponibles.

Le head global augmente à chaque avancement ou retrait accepté, sans débordement. La
position doit croître ; même position/même digest est `PositionNotIncreasing`, même
position/autre digest est `Conflict`. Un retrait exige la dernière position acceptée,
garde l'anchor final et reçoit son propre counter supérieur. Les retraits ne sont ni
oubliés ni réactivés, même après redémarrage. Un retrait répété reste un conflit.
Un avancement à une position supérieure sur un flux retiré renvoie `Conflict` ;
un avancement à sa position finale ou en dessous renvoie `PositionNotIncreasing`.

Le format d'état v1 contient une chaîne magic `mddlog-witness`, la version uint64 1,
un inventaire complet, puis SHA-256 de tous les octets précédents. Entiers uint64 little
endian, textes de 1 à 1024 octets avec longueur uint64, aucun NUL. L'inventaire contient
providerId, head, nombre d'entrées, puis pour chaque entrée : tag de retrait uint64 0/1,
anchor et, si retirée, counter/time de retrait. Anchor : formats uint16 encodés en uint64,
streamId, position, digest de 32 octets, providerId, counter, time. Time : disponibilité
uint64 0/1, puis représentation uint64 des nanosecondes signées depuis l'époque Unix ;
indisponible exige zéro. La clock murale du service donne une date, pas l'ordre : le counter
reste la valeur monotone. Les identités, positions/counters, unicité des flux/counters,
head maximal et ordre counter d'anchor/retrait sont validés avant restitution.

`acceptedTime` et `retiredTime` proviennent de `system_clock` et peuvent reculer après
une correction d'horloge : ces heures ne prouvent ni ordre ni durée écoulée. Pour établir
l'ordre des opérations, utiliser exclusivement le counter monotone.

Limites : 4096 flux conservés, fichiers et trames de 1 Mio, identifiants de flux conformes
aux 96 octets d'`AuditEvent`. Un inventaire volumineux peut atteindre 1 Mio avant 4096 flux,
notamment avec une identité fournisseur longue : le magasin refuse alors la mutation et
conserve l'état lisible. Au plafond de flux, avancement et retrait d'un flux existant
restent possibles dans les limites de taille et de compteur. Au head `UINT64_MAX`,
toute nouvelle mutation est refusée ; les lectures restent possibles. Reprendre les
mutations exige une migration administrée du profil, pas un simple redémarrage.
Aucune éviction n'efface un retrait. Ces limites ne sont pas des
budgets WCET. Le matériel, filesystem, caches et alimentation restent à qualifier.

Chaque mutation acceptée encode et réécrit tout l'inventaire, puis synchronise le
fichier et son répertoire. Le volume écrit est proportionnel au nombre d'entrées et
à la taille de leurs identités, jusqu'à 1 Mio par mutation ; la latence inclut ces deux
fsync synchrones. Aucun coût constant, débit minimal ni WCET n'est garanti.

## Transport, délais et réponses perdues

Protocole v1 : connexion neuve par opération, taille uint64 puis trame bornée. Requête :
version, opération (advance 1, retire 2, latest 3, streams 4), identité fournisseur et champs
de l'opération. Réponse : version, résultat typé et stamp ou inventaire. Les champs
manquants/excédentaires, tags/tailles inconnus et incohérences de l'inventaire sont refusés.
`latest` transporte l'inventaire complet avec son head authentifié, puis sélectionne le flux,
pour vérifier les mêmes invariants que `streams` ; la lecture ciblée bornée est une évolution
possible de #117, sans changement de cette version.

Une échéance `steady_clock` unique couvre connexion, envoi et réception. Socket non bloquante,
poll avec temps restant, pas de SIGPIPE ; les erreurs EINTR ne renouvellent pas le délai.
Le service borne aussi l'attente d'un corps et d'un lecteur lent. Sa boucle est un consommateur
unique : un client autorisé lent peut occuper une échéance avant les autres.
Un UID enrôlé qui se reconnecte sans transmettre peut répéter cette occupation
(1 s par connexion par défaut) et saturer la disponibilité : aucun quota par UID,
limiteur de débit ni ordonnancement équitable n'est implémenté.
Les fsync restent synchrones et ne sont pas interrompus par le délai réseau.
Le client peut donc expirer pendant
une transaction qui sera ensuite appliquée. Cadence, capacité, supervision et indisponibilité
sont des responsabilités du profil ; aucun temps maximum de persistance n'est revendiqué.

`lastError` du client distingue configuration, connexion, authentification, timeout,
transport, protocole, refus d'autorité, indisponibilité et refus métier. Le magasin expose
séparément sa dernière erreur native. Aucun secret ou chemin de configuration n'est repris
dans ces structures de santé. Le service rend ses erreurs par résultat typé à son superviseur.

Un backlog Unix plein (`connect` renvoyant `EAGAIN`) est classé `Connect`, avec l'erreur
native conservée ; il ne déclenche pas de vérification d'authentification sur un socket
non connecté, ni de retry silencieux.
Voir le contrat Linux de [`connect(2)`](https://man7.org/linux/man-pages/man2/connect.2.html).

`ProviderUnavailable` signifie **absence d'acceptation confirmée**, pas preuve de non-exécution.
Après une réponse perdue, l'hôte interroge `latest`/`streams` sous la même authentification :
si l'anchor ou le retrait attendu est présent, la réconciliation constate cette application.
Si une observation plus haute/conflictuelle apparaît, l'hôte traite son conflit. Il ne fabrique
pas un stamp et ne transforme pas une nouvelle requête refusée en acceptation. Aucun retry
silencieux de mutation ne contourne les refus d'ADR-004. `AuditService`/la reprise existante
consomment ces opérations via la même interface `AnchorProvider`.

En particulier, après perte de la réponse d'un `advance`, rejouer même position/même
digest donne `PositionNotIncreasing`, jamais une nouvelle acceptation. Consulter
`latest` pour réconcilier ; un stamp ne peut pas être déduit du refus seul.

## Preuves et portée

[La campagne](independent-witness-validation.md) distingue les tests de transport avec le
même UID (sans indépendance) du profil à quatre UID/processus séparés. Le profil utilise
le backend fichiers réel, `AuditService`, les appels d'audit contextualisés, le client réel
et la position retenue du lecteur. Les copies du journal destinées au lecteur sont hors ligne
et privées ; il ne prend pas possession du répertoire vivant du rédacteur.

Les sources et le profil sont dans ce dépôt ; SpecLab reste figé par CMake. Les consommateurs
source/install vérifient les imports du magasin et du client sans nouvelle dépendance du cœur.
La CI Clang exige la campagne d'autorités séparées ; ailleurs, un SKIP explicite peut indiquer
que le namespace multi-UID n'est pas disponible. Une réouverture sur tmpfs n'est pas une
qualification électrique. L'acceptation de #115 et du profil complet reste une revue de
mainteneur ; les preuves physiques et finales de #122 ne sont pas acquises par ce lot.
