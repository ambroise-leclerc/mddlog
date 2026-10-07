# Ressources du lecteur et du consommateur — #117

Le profil logiciel de référence est **Linux x86_64, Clang/libc++ 21, Release**.
Sa configuration et ses enveloppes de régression sont dans
[linux-x86_64.json](resource-budgets/linux-x86_64.json). Il couvre les doubles en mémoire
et les appels Linux du backend fichiers. Il n’ajoute aucune qualification physique,
aucune promesse WCET ni aucun portage MCU/RTOS. Les autres tuples de la matrice
compilent les lecteurs et passent leurs scénarios ; leurs temps ne sont pas déduits
de cette campagne. La qualification applicative finale reste dans #122.

## Profil et configuration

| Dimension | Profil mesuré | Défaut compatible fini |
| --- | ---: | ---: |
| Segments | 512 | 4096 |
| Octets par segment | 32 Kio | 1 Mio |
| Volume inventorié | 16 Mio | 64 Mio |
| Chunk demandé au support | 4 Kio | 64 Kio |
| Octets demandés par lecture | 32 Mio | 128 Mio |
| Identités présentes ou citées, historiques incluses | 256 | 4096 |
| Enregistrements, registres inclus | 65 536 | 262 144 |
| Entrées du fournisseur, retraits inclus | 2048 | 4096 |
| Octets par identifiant de fournisseur/checkpoint | 1024 | 1024 |
| Appels de lecture au fournisseur | 8192 | 32 768 |
| Fautes d’intégrité conservées | 2048 | 4096 |
| RSS du processus de mesure | 256 Mio | aucune promesse hors profil |
| Descripteurs du processus fichiers isolé | 520 (fixture 32 segments : 40) | dépend de l’hôte |

`AuditResourceLimits` centralise les cardinalités. Le même objet se copie dans
`StorageConfig::resources` et `VerifierConfig::resources` ; l’appel simple au service
le reçoit via `AuditServiceConfig::storage`. Le profil des benchmarks l’applique à
la création, à l’analyse, à la reprise, à la rétention et à la vérification.
Le backend fichiers doit déclarer ses propres limites concordantes :
`maxSegments`, `maxSegmentBytes`, et `maxReadBytes >= readChunkBytes` (ce dernier
est une borne **par requête**). Les magasins du témoin et du checkpoint ont déjà
leurs plafonds d’entrées/fichiers ; le profil lecteur peut être plus restrictif.
Le backend conserve un descripteur par segment accédé jusqu’à réclamation ou
 destruction ; prévoir `maxSegments + 8` pour le processus isolé avec son répertoire,
les trois descripteurs standard et les ouvertures temporaires. La fixture fichiers
mesurée déclare 32 segments et vérifie un plafond de 40 descripteurs.
Aucune modification du format canonique ou de l’archive v0.2 n’est nécessaire.

Chaque limite est positive. La capacité déclarée du sink (`segmentCount * segmentSize`)
doit tenir dans le volume du profil ; la validation emploie une division pour éviter
le débordement. Les limites sont inclusives. Le nombre de producteurs simultanés
reste distinct du nombre d’identités historiques : fermer un flux ne permet pas
son recyclage et ne libère pas sa place dans l’inventaire d’identités. Les registres
et les flux seulement cités consomment aussi cette place. Une longue exploitation
doit prévoir export/archivage et nouvelle composition explicite ; augmenter la
limite constitue un nouveau profil à mesurer.

## Lecture et résultats

`AuditReadSession` contrôle **tout l’inventaire avant le premier read de données**,
y compris tailles individuelles et somme sans débordement. Chaque requête est
bornée par `readChunkBytes`, et la somme des octets demandés par `maxReadBytes`.
L’analyse conserve encore les octets du journal, afin de garder valides ses spans :
il s’agit de lecture découpée avec **volume plafonné**, pas d’un vérificateur à mémoire
constante. Les descriptions de frames et les identités sont plafonnées avant leur
insertion dans les inventaires correspondants. Une limite de records peut donc
refuser un journal avant sa limite en octets.

Les interfaces `StorageMedium::segments()` et `AnchorProvider::streams()` retournent
des conteneurs possédés. Leur allocation interne appartient au backend : le lecteur
ne peut pas empêcher une implémentation arbitraire d’allouer avant de répondre.
Les backends de référence contrôlent leurs plafonds avant croissance de l’inventaire
ou décodage du fichier. Un backend personnalisé doit fournir ce contrat et borner
les tailles de ses réponses. Le lecteur borne aussi les identifiants reçus avant
de les recopier dans ses propres structures. Les checkpoints fournis directement
par l’hôte sont vérifiés avant copie par `LogVerifier`.

`resourceIssue` distingue segments, tailles, volume, travail de lecture, identités,
records, inventaire/appels/textes du fournisseur, fautes et `MemoryUnavailable`.
Les lectures qui dépassent un budget sont explicitement incomplètes. `LogReport`
écarte alors ses verdicts partiels, frontières et régions résiduelles et **ne modifie
pas le checkpoint**. Un rapport vide avec `resourceIssue != None` n’est jamais la
preuve d’un journal vide ou vérifié. `AnchorVerifier` répond `CannotVerify` /
`ResourceLimit` et expose le diagnostic précis. Les lecteurs bas niveau demandent
à l’appelant d’examiner `resourceIssue`, `unreadable()` ou `complete()` selon leur API.
La reprise ne transforme pas un refus en absence d’historique ; la création refuse
le budget avant un nouvel origin. La rétention refusée pour ce motif ne démarre
aucune suppression. Les défaillances d’allocation au cours d’I/O/mutations gardent
les contrats de reprise existants ; elles ne rendent pas l’opération transactionnelle.

`LogVerifier` utilise un checkpoint candidat, publié uniquement après la fin d’une
vérification sans refus de ressources. Les appels du fournisseur sont comptés pour
l’ensemble du rapport, frontières et retraits compris ; chaque `verify()` réinitialise
ce budget. Pour un `AnchorVerifier` autonome, il couvre la durée de vie de l’objet.
Une erreur d’I/O classique, un verdict sémantique défavorable et un refus de ressources
restent des diagnostics distincts. Un défaut conservateur peut donc encore refuser
un profil trop petit ; il ne promeut aucune admission à durabilité.

## Coûts et limites temporelles

Avec V octets, R records, S identités et P entrées du témoin, les octets possédés
sont O(V), les descripteurs et copies de records/registre O(R), les inventaires O(S+P).
Le tampon temporaire supplémentaire est borné par un chunk ; les réservations de
segment sont vérifiées avant allocation. Ce n’est pas un allocateur à quota RAM :
les cardinalités bornent la croissance, la RSS est une enveloppe **mesurée** avec
bibliothèque standard, capacité des conteneurs et copies du checkpoint incluses.
La fixture en mémoire conserve en outre sa propre copie du support.

Les relations entre registres, citations, trims et listes fournisseur font encore
plusieurs parcours et recherches ; on ne promet pas une vérification O(V) ou une
reprise linéaire en sessions. Chaque rétention analyse le journal ; des retraits
successifs sans cache répètent ce travail. Le témoin réécrit et synchronise son
inventaire à chaque mutation. Les I/O synchrones, `fsync`, l’ordonnancement et les
callbacks hôte ne sont pas bornés par les budgets en nombre de tentatives.
Les enveloppes CI détectent de grosses régressions sur le tuple, pas un temps maximal
système. Le deadline du service reste souple.

Le producteur gouverné garde ses buffers fixes et des atomiques `uint64_t` déclarées
lock-free par le compilateur ; les scénarios d’allocation et d’exception et leurs
contrôles négatifs restent exécutés. `DiagnosticContext` est possédé et borné par
ses champs. L’adaptateur `DiagnosticBinding`/`TextLogger` formate et alloue ; une
chaîne diagnostique arbitrairement longue n’est pas un appel gouverné. Le profil
mesure des messages de taille déclarée, avec un sink qui consomme synchroniquement
sans les accumuler. L’application doit limiter ses entrées et callbacks. Ni ces
faits ni les mesures d’admission ne constituent une preuve de pire cas sur MCU.

## Reproduction et couverture

```sh
cmake --preset ninja-clang
cmake --build --preset ninja-clang
python3 scripts/run-resource-bench.py build-clang/tests/mddlog_resource_bench \
  --count 49152 --runs 3 --host-binary build-clang/tests/mddlog_host_resource_bench \
  --service-binary build-clang/tests/mddlog_service_resource_bench \
  --output build-clang/resources.json
ctest --test-dir build-clang --output-on-failure
```

Le benchmark principal se compile **sans changement de source sur v0.2.0** ;
`--baseline` enregistre ses mesures sans appliquer des limites qui n’y existaient
pas. Corpus : 32 producteurs, 64 reprises supplémentaires, 1024 retraits du témoin,
SHA-256 de 8 Mio, 100 000 admissions et 100 000 refus sur anneau plein ; rotation et suppression réelles de segments
après vérification. Le benchmark hôte ajoute contexte/formatage, appels fichiers,
barrières et descripteurs. Le benchmark du service ajoute 32 producteurs saturés,
les observateurs autorisés, un témoin indisponible puis rétabli, `poll` et l’arrêt
quiescent. Les latences maximales **observées** de passe/observation/arrêt sont
comparées à une enveloppe d’une seconde, le traitement complet à 30 secondes ;
ces seuils ne bornent ni un appel externe arbitraire ni l’ordonnanceur. Les rapports générés restent hors contrôle de version
et sont conservés comme artefacts CI. Le [relevé](audit-resource-validation.md)
donne le matériel, les résultats et leurs limites.

Les tests vérifient tailles malformées jusqu’à `UINT64_MAX`, sommes débordantes,
limites exactes et dépassements, chunks minuscules, refus de mémoire, compteurs
réserve sans wrap, historique fermé, fautes et préservation du checkpoint.
Le harness de rétention/reprise compare les rapports entiers avec des chunks de
17 octets ; les 64 graines de coupure #120 ajoutent ceux de 11 octets. Verdicts,
causes, plages, notes de frontières et régions résiduelles doivent rester égaux.
Le fuzzing et l’oracle indépendant du programme #120 restent complémentaires.


## Budget du fournisseur et durée de vie des vérificateurs

`maxProviderCalls` compte uniquement les lectures `latest()` et `streams()`.
`advance()` et `retire()` sont délégués sans consommer ni appliquer ce budget ;
leurs politiques et délais relèvent du consommateur et du contrat du fournisseur.

Un `LogVerifier::verify()` remet son budget à zéro puis le partage avec les
vérifications de streams, l’inventaire non présent et les frontières : une seule
enveloppe compte les appels effectifs au fournisseur. `LogReport::providerCalls`
rapporte ce total, y compris avant un refus.

Un `AnchorVerifier` autonome conserve un budget cumulé pour le lot composé de
`verify()` et `verifyUnlisted()`. Appeler `resetBudget()` avant un nouveau lot
indépendant ; cette opération efface aussi le refus de ressources et l’état
transitoire du lot. `withSharedBudget()` emprunte explicitement un budget externe
dont les limites correspondent à `config.resources` ; ce budget doit lui survivre.
La copie et le déplacement des deux vérificateurs sont explicitement interdits,
comme ils l’étaient déjà implicitement via `AnchorProvider`. Ils ne conservent
aucun pointeur vers un de leurs propres membres. Une construction en place dans
un `std::optional` reste possible. Les références au support, au fournisseur et
au checkpoint demeurent empruntées et doivent leur survivre.
