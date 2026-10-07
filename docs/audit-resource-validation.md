# Validation des ressources — #117

Campagne du 2026-10-07 sur Linux x86_64, AMD Ryzen 9 3900X (12 cœurs / 24 threads),
Clang/libc++ 21.1.8, CMake 4.2.3, Ninja, Release. Révision de départ :
`7db8b8e26aff5e3127f6603335186024db5cde8a`. La tête et le diff de la PR identifient
les ajouts vérifiés ; la comparaison v0.2 utilise le tag immuable
`v0.2.0`, `e7012f299b3c976b37b43b53add43e5e6e4636b8`. Aucune acceptation de la base #124 ou du système n’est modifiée.

## Corpus et méthode

Le [profil](resource-budgets/linux-x86_64.json) plafonne le volume à 16 Mio,
les identités à 256 et le fournisseur à 2048 entrées. Le benchmark commun se compile
sur la référence v0.2 et le lecteur borné. Trois processus par version,
`steady_clock` pour les opérations, GNU time pour la RSS maximale. Le support en
mémoire conserve sa copie de l’archive dans la RSS ; des caches chauds, charges de
l’hôte et optimisations du compilateur peuvent influer sur les distributions.
Les médianes ne sont pas une preuve de gain causal ni une borne de pire cas.

49 152 événements, 32 producteurs, détail de 200 octets, 64 reprises supplémentaires :
13 214 070 octets, 481 segments, 65 registres avant maintenance. Le fournisseur
contient ensuite 1024 retraits supplémentaires. La vérification couvre 97 flux
présents et 1024 retraits sans journal. Rotation : 12 segments ; suppression : 13.
La fixture fichiers utilise 4096 événements, 32 segments autorisés et sync toutes
les 128 admissions ; `QualifiedFsync` y est une hypothèse de fixture logicielle,
pas une qualification de coupure électrique. Les formats/contextes mesurés utilisent
des identifiants valides et un message fixe de 25 caractères, sans accumulation
dans le sink. Le contexte inclut la préparation d’une corrélation décimale variable.

## Vérification des contrats

- VER-054 : inventaire, tailles `UINT64_MAX` et somme hors budget refusés avant read.
- VER-055 : limite exacte de records/volume/lecture, chunks de 7 octets, dépassement
  et `bad_alloc` explicites sans rapport complet.
- VER-056 : appels, inventaire de retraits, texte fournisseur et croissance des heads
  du checkpoint plafonnés ; aucun checkpoint publié sur interruption.
- VER-057 : historique fermé conservé, limite à la reprise avant nouvel origin,
  invalidité des budgets nuls et calcul de réserve sans wrap.
- VER-058 : limite exacte des fautes, dépassement explicitement non exploitable.
- VER-059 : reproduction v0.2/profil, lectures/copies mesurées par le support,
  tailles du producteur/contexte, RSS, barrières fichiers et enveloppes CI.

Le harness `AuditLogRig::readLog` compare chaque rapport à une lecture de chunks
de 17 octets : verdict, cause, chaîne, positions, couverture, exposition, ancre,
statut d’âge/checkpoint, registre, disposition, frontières et région résiduelle.
Il couvre reprise, fork/cycle, trims et retraits du corpus existant. Les 64 graines
de `AuditGeneratedRecoverySpec` comparent aussi les rapports après coupure avec
des chunks de 11 octets. Il s’agit de la même référence sémantique et du même corpus,
complétés par les oracles de confirmed-prefix/SHA et campagnes #120 ; aucune preuve
d’exhaustivité des entrées hostiles n’est revendiquée.

VER-061 injecte un `bad_alloc` après la confirmation durable du trim, avant
l’ancrage du registre et toute suppression. Le refus conserve `trimmedThrough` ;
la nouvelle tentative reprend le trim confirmé sans écrire un second trim.
Le slot de rapprochement est réservé avant l’écriture.

## Réserves

La croissance est bornée par les limites de volume/cardinalité et les contrats
des backends. L’allocateur ne possède pas de quota RAM ; les images, descriptions
de records, évaluations, listes fournisseur et checkpoint candidat ont des copies.
Les mesures de RSS et les enveloppes CI sont des preuves logicielles sur le tuple
déclaré. Les appels synchrones externes restent sous contrat hôte ; aucun WCET,
support MCU, système temps réel ou nouvelle qualification physique n’est déduit.
La saturation diagnostique de #118, les campagnes complémentaires #120 et
l’application indépendante/acceptation finale de #122 restent distinctes.

## Mesures obtenues

Trois répétitions par version ; médianes en millisecondes. Une charge de compilation/
analyse était présente sur cet hôte partagé : ces valeurs permettent d’examiner
le profil et ses enveloppes, sans isoler un gain ou surcoût algorithmique.

| Opération | v0.2 | Lecteur borné |
| --- | ---: | ---: |
| Création à vide | 0.088 | 0.107 |
| 49 152 événements et syncs | 209.957 | 318.090 |
| Fermeture | 0.152 | 0.197 |
| 64 reprises | 11893.230 | 14420.359 |
| Analyse du journal | 197.976 | 224.129 |
| Rapprochement du témoin | 0.790 | 0.835 |
| Vérification complète | 777.913 | 728.753 |
| Rotation (12 segments) | 154.641 | 260.483 |
| Suppression (13 segments) | 166.930 | 204.081 |
| 100 000 admissions/acks | 17.902 | 18.187 |
| SHA-256, 8 Mio | 58.098 | 74.506 |

RSS maximale : **57124 Kio** (v0.2), **56860 Kio** (borné), sous 256 Mio.
Sur le corpus entier, maintenance incluse, 956243915 octets sont
demandés au support : les reprises et deux rétentions répètent les lectures.
La référence demande 64509 requêtes, de 32736 octets
au plus ; le lecteur borné en demande 267449, plafonnées à **4096 octets**.
Les deux corpus réalisent 646 syncs. L’archive elle-même reste identique.

Le benchmark hôte mesure les médianes suivantes :

- 100 000 préparations de contexte : 1.925 ms.
- 10 000 messages avec contexte : 28.673 ms.
- Création du backend/sink fichiers : 0.204 ms.
- 4096 événements avec barrières fichiers : 76.511 ms.
- Fermeture fichiers : 0.013 ms.

RSS hôte maximale 3592 Kio ; 20 descripteurs sous le plafond 40.
`AuditEvent` : 800 octets, `AuditRing<8>` : 6536 octets, `DiagnosticContext` : 110 octets
sur ce tuple. Ces tailles ne constituent pas des ABI universelles.

Toutes les enveloppes du profil passent sur les trois répétitions. Leur marge
absorbe les variations des runners CI ; le workflow conserve le relevé du tuple,
le corpus et les mesures comme artefact. Les autres plateformes n’utilisent pas
ces seuils temporels par extrapolation.

## Suites exécutées

- Clang Release : **217/217 CTest**, benchmark du service inclus.
- GCC 16.1.0 / libstdc++ Release : **216/216 CTest** (test de cache Clang absent).
- ASan/UBSan Clang Debug : **196/196 tests (194 scénarios, benchmark du service et injection de rétention)**, fuites activées et arrêt sur erreur.
- TSan Clang Debug : **12/12 tests** sélectionnés de concurrence/observateurs.

Les scénarios d’allocation/exception du cœur et leurs contrôles négatifs passent
dans les suites complètes. Le dossier est contrôlé séparément de la compilation.
L’analyse clang-tidy 21 et les contrôles format/documentation sont requis ;
les checks de PR restent l’autorité pour les autres tuples.

## Pilotage et observateurs sous charge

Le benchmark du service applique le même profil de cardinalité/volume à 32 anneaux
SPSC de huit places, 512 événements par producteur. Il impose une saturation avant
le premier drain, puis maintient un témoin indisponible jusqu’à la moitié des
transmissions. Les budgets déclarés sont quatre records par anneau, 64 tentatives
par poll, quatre flux d’ancrage par poll et une période de retry de 1 ms.
La réconciliation conserve la borne d’âge d’une seconde. Les producteurs sont
rejoints avant l’arrêt ; les observateurs s’arrêtent aussi avant destruction.

L’observateur appelle `AuditService::health`, `PersistingAuditSink::health`,
`durableClaim`, `durablePosition` et les compteurs atomiques des anneaux. Il vérifie
la monotonie, les plafonds d’inventaire et l’ordre des positions sans exiger une
transaction entre snapshots indépendants. Chaque répétition termine avec 16 384
événements transmis, aucun backlog et `Completed`, confirmation et ancrage inclus.

Trois répétitions :

| Mesure | Médiane des répétitions | Maximum des répétitions |
| --- | ---: | ---: |
| Traitement et retour du témoin | 998.907 ms | 998.938 ms |
| Réconciliation | 932.992 ms | 936.952 ms |
| Passe la plus lente observée | 0.356 ms | 0.586 ms |
| Observation complète la plus lente | 0.066 ms | 0.072 ms |
| Arrêt quiescent | 0.160 ms | 0.161 ms |

RSS service maximale : **6320 Kio**, sous 256 Mio.
Les 33 flux publiés (32 producteurs et registre) restent sous le plafond déclaré ;
les refus d’anneaux pleins sont visibles sans consommer de séquence. L’observation
mesurée comprend deux snapshots et tous les claims/compteurs, pas seulement un
chargement atomique. Les maxima observés ne sont pas des garanties de deadline.

Le cas est exécuté comme CTest `audit.service.resourceProfile` sur Linux et avec
les enveloppes du workflow ressources. Le critère logiciel d’observateurs/budgets
de #116 peut être examiné avec cette preuve ; la décision de revue reste distincte
des résultats, et #122 conserve la qualification de l’application indépendante.

La campagne complémentaire mesure aussi le démarrage le plus lent des 64 reprises
(638.688 ms, médiane des maxima), le démarrage avec
1024 retraits (191.008 ms) et 100 000 refus sur anneau
plein (6.052 ms). Les enveloppes de deux secondes
par démarrage et d’une seconde pour le lot de refus passent sur les trois répétitions.

Analyse locale clang-tidy 21 : **15 unités propres** (huit interfaces modifiées,
trois unités couvrant les scénarios/headers, trois benchmarks et l’exemple fichiers).
Les corrections de l’analyse ont été reprises sur les unités concernées ;
les shards CI couvrent le périmètre entier, benchmarks inclus. Format LLVM 21 :
107 fichiers contrôlés ; vérificateur du dossier et **59 tests documentaires** passés.


## Revue des vérificateurs et des refus de session

VER-062 interdit explicitement les quatre opérations de copie/déplacement sur
`AnchorVerifier` et `LogVerifier`, avec assertions de type. Elles étaient déjà
implicitement supprimées par `AnchorProvider` ; les vérificateurs n’utilisent
plus de pointeur vers leur propre enveloppe. La construction en place dans un
`optional` et le retour prvalue du vérificateur à budget partagé sont exercés.
Les lots autonomes d’`AnchorVerifier` sont remis à zéro explicitement ; les lots
complets de `LogVerifier` le sont automatiquement. Les tests comparent les cinq
lectures réellement reçues par un fournisseur à `providerCalls`, puis refusent
la cinquième avec un budget de quatre sans publier le checkpoint candidat.

VER-063 dépasse le budget de lecture dans la passe des préfixes puis dans celle
des derniers segments. Le contrôle s’arrête à la première erreur de session,
sans finding `UnreadableSegment`. Un préambule réellement invalide découvert
avant le refus reste dans les findings, distinct du refus de ressources.
La seconde passe de `readStoredStream` ne rescanne que les buffers immuables déjà
validés et plafonnés dans la première passe. Le budget fournisseur est en lecture
seulement ; `advance`/`retire` conservent leur contrat d’exploitation.
