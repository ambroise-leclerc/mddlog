# Pilotage de référence, premier lot — #114 / #116

`mddlog.adapter.auditservice` fournit `AuditService`, appelé depuis la boucle de l'hôte.
Le support est injecté : `FileStorageMedium` reste facultatif, sans dépendance du cœur
gouverné au backend. Le service ne change ni les points d'émission ni les formats d'audit.
Ce premier lot raccorde l'exemple fichiers au pilotage ; il ne termine pas toute #116.

## Composition et contrat de durée de vie

L'hôte ouvre le support, configure `StorageConfig` et crée le service avec
`AuditService::create`. Les erreurs distinguent budget nul, période d'ancrage invalide
et erreur de configuration du sink, avec conservation de la cause d'origine.
Le service possède le consommateur et le sink. Support, fournisseur et anneaux doivent
survivre au service. La configuration conserve les règles de validation du sink ; un
sink créé peut publier une panne au démarrage, à examiner dans `health().storage`.

Les anneaux sont inscrits avant le premier `poll` et avant toute observation concurrente.
Une identité invalide ou dupliquée est refusée. L'inscription après démarrage ou demande
d'arrêt est refusée. L'hôte attribue des identités nouvelles à chaque producteur et
chaque démarrage, registre compris ; aucun identifiant prétendument unique n'est généré.
Le canal de pertes du service est raccordé au sink et le callback éventuel de l'hôte
est conservé. La capture du consommateur est faible, sans cycle de propriété.
La destruction détache le sink sans entrée-sortie implicite.

## Boucle de l'hôte

`poll()` visite tous les anneaux dans leur ordre d'inscription, avec au plus
`maxRecordsPerRing` événements par anneau. Chaque anneau reçoit le même budget : un
premier producteur saturé ne consomme pas celui des suivants. `AuditSinkAdapter::drainOnce`
conserve son comportement historique quand aucun budget n'est fourni. Refus et exceptions
conservent l'événement dans son anneau selon son contrat existant.

Le service appelle ensuite `tick()` et tente les ancrages à `anchorPeriod`, registre
compris. Les refus d'intégrité et l'indisponibilité restent observables dans la santé du
sink. Une position déjà ancrée n'est pas proposée à nouveau ; l'indisponibilité permet
une tentative ultérieure au prochain échéancier. Le service n'offre aucune position
supérieure à celle confirmée par le support.

L'hôte doit appeler `poll` aussi en période inactive et choisir une cadence compatible
avec les bornes d'âge. Les opérations externes sont synchrones ; le budget en événements
ne borne pas leur durée. Aucun thread, délai forcé, politique de rétention ni décision
médicale n'est ajouté. Le clock de stockage est partagé avec l'échéancier d'ancrage.

## Arrêt et santé

L'hôte cesse les producteurs et établit leur quiescence avant `stop(maxDrainPasses)`.
Le service réalise au plus ce nombre de passages et ferme le sink seulement quand les
anneaux sont vides. Si le backlog subsiste, le rapport porte `closed = false` ; la
boucle peut poursuivre ou l'hôte peut réessayer l'arrêt. Une instance échouée reste
échouée : le service ne change pas son identité ni ne rejoue silencieusement ses événements.

`closed = true` signifie fin du cycle de vie, pas qualification, durabilité ou ancrage.
Le rapport contient les événements transmis, les événements encore dans les anneaux,
les pertes signalées, les états de stockage, les positions écrites/confirmées et les
compteurs d'ancrage. Ces observations ne forment pas un instantané transactionnel entre
producteurs et consommateur. Les observateurs respectent le contrat existant des deux
canaux de santé. Les refus d'admission restent à lire sur les anneaux producteurs.

## Exemple fichiers et revue d'ergonomie

[FileAudit.cpp](../examples/FileAudit.cpp) regroupe ouverture, configuration, inscription,
`poll` et `stop` au point de composition. `emit` ne reçoit qu'un `AuditBinding` : aucun
réglage du support ni entretien n'est ajouté au code métier. Après fermeture, un lecteur
rouvre le répertoire en lecture seule et vérifie la chaîne. Une nouvelle invocation,
avec de nouvelles identités de producteur et registre, ouvre l'historique existant ; les
identités déjà employées sont refusées. Les budgets de l'exemple sont propres à cette
composition et ne remplacent pas ceux du profil électrique Orin Nano.

```sh
mkdir -m 700 /tmp/mddlog-audit-demo
build-clang/examples/mddlog_file_audit /tmp/mddlog-audit-demo demo:boot-1
build-clang/examples/mddlog_file_audit /tmp/mddlog-audit-demo demo:boot-2
```

Le support de l'exemple reste `Unqualified` : les octets relus sont vérifiés mais la
position durable publiée vaut zéro. L'indépendance réelle du témoin, la position retenue
persistante et l'essai intégré sur le matériel restent dus en #115/#116/#122.
Cette revue technique décrit la séparation des responsabilités ; elle n'est pas une
acceptation d'aptitude à l'utilisation d'un dispositif.

## Vérification et limites restantes

[AuditServiceSpec.cpp](../tests/spec/AuditServiceSpec.cpp) couvre les configurations
invalides, les deux producteurs avec budget, l'arrêt borné et répété, le flux inactif,
la reprise des tentatives d'ancrage, un support non éligible, le signal de pertes et
les observations de santé pendant émission/drain concurrents.
Les doubles en mémoire vérifient l'orchestration ; ils ne qualifient pas l'électricité.
La [préparation de clôture](file-storage-closure.md) distingue les résultats exécutés
et les preuves encore manquantes. Le choix du thread, les bornes temporelles externes,
l'agrégation des refus d'admission, les fenêtres d'exposition et les politiques de
rétention/reprise complètes restent à traiter en #116 et #117.
