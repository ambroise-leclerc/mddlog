# Composants tiers et outils

Maîtrise et statut : [index](../../README.md). Inventaire unique : section `dependencies` du
[registre](../../register.json). Chaque DEP indique portée, provenance, contrainte/version,
licence, anomalies et surveillance ; les versions exactes de campagne restent dans ses preuves.

La bibliothèque n'importe pas MduX ni SpecLab à l'exécution. Le standard C++, la bibliothèque
standard et le runtime de threads sont néanmoins des éléments déployés à évaluer dans le
profil de l'hôte ; « sans dépendance externe » ne les retire pas de l'analyse. Leur classement
SOUP est une proposition à confirmer selon le dossier de développement disponible et la
classification système du fabricant (GAP-005), pas une conséquence automatique de leur licence.

SpecLab est réservé aux tests ; CPM est un outil de construction dont une copie est intégrée au dépôt. CMake, Ninja,
compilateurs, analyseurs, Python, Git et runners CI influencent la construction ou les preuves
sans être liés au composant. Le premier backend Linux de #114 ajoute la pile système DEP-009 (appels Linux et libc) ; matériel, système de fichiers et caches restent à qualifier. Le témoin Linux de #115 réutilise cette pile pour les sockets Unix/SO_PEERCRED et fsync ;
aucune nouvelle bibliothèque tierce n’est liée. DEP-011 inventorie le banc multi-UID.
Les versions supportées et exclusions restent gouvernées par CMake ; en particulier GCC 16.2
est exclu pour corruption des BMI, sans généraliser une garantie aux versions ultérieures.

Les sources de surveillance propres à chaque DEP et leur cadence figurent dans le registre.
Elles définissent un plan ; aucun examen complet des avis ni suivi opérationnel déjà réalisé
n’est revendiqué. Les résultats et anomalies de chaque examen restent à conserver.

Avant une mise à jour : vérifier licence et provenance, examiner défauts et avis de sécurité,
reconstruire modules et consommateurs, exécuter les vérifications impactées et faire accepter
le changement. Les rôles et la conservation sont définis dans [les plans](../Plans.md).


DEP-010 inventorie le CLI Zstandard du [contrôle des archives externes](../../../docs/file-storage-closure.md).
Il n'est lié ni déployé avec la bibliothèque ; son chemin et son empreinte sont consignés
par le rapport. La campagne locale utilise zstd 1.5.7. Les notices de la distribution
et la maîtrise de l'outil restent à examiner dans le profil de revue (GAP-005) ; les
comparaisons d'empreintes ne qualifient pas la persistance matérielle (GAP-007).

DEP-011 couvre util-linux (`unshare`) et les helpers uidmap de la campagne du témoin,
outils de vérification hors bibliothèque déployée. Leurs versions, mappings et conditions
de SKIP/échec sont consignés dans [la campagne](../../../docs/independent-witness-validation.md).
L’exécution CI requiert l’isolation ; sa réussite ne qualifie pas les comptes d’un dispositif.

## Dépendances du lot diagnostic #118

Aucun composant tiers ni changement de pin n’est ajouté. La bibliothèque standard et le
runtime threads existants assurent tableau circulaire, snapshots possédés, `jthread` et
conditions de complétion ; promise/future n’intervient plus dans ce worker. L’injection
d’allocations défaillantes et le plateau sous surcharge sont des consumers de vérification,
sans instrumentation dans la bibliothèque déployée. La durée et la mémoire propres des
sinks hôte restent à qualifier ; [le contrat](../../../docs/diagnostic-budgets.md)
et VER-064/065 explicitent ces limites, sans modifier la qualification des dépendances.

## Lot outils et export #119

Le lot #119 n’ajoute aucune bibliothèque tierce déployée : C++23/libc++, SHA-256/codec existants, adaptateurs POSIX existants et Python pour le banc externe. Le codec témoin portable reste une dépendance interne ; la CLI est optionnelle Linux. Versions du profil local et corpus original v0.2 sont identifiés dans les preuves ; la matrice finale reste #121.

Voir [contrat et usage](../../../docs/audit-tools.md) et [preuves](../../../docs/audit-tools-validation.md).

## Outillage du complément #120

Le nouveau superviseur utilise Python standard (DEP-006), notamment hashlib pour l’oracle indépendant, et partage les limites de processus du runner existant. Aucune nouvelle dépendance liée à la bibliothèque. Le profil multi-UID reprend DEP-011 et les formats/lecteurs DEP-009 ; versions et empreintes sont collectées par campagne. Conservation et qualification des outils restent dans GAP-003/005.
