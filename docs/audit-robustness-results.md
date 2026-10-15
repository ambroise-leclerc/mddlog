# Résultats complémentaires de stockage et robustesse — #114 / #120

Le [complément du 8 octobre 2026](#complément-de-chaîne-réelle-du-8-octobre-2026) étend
ces résultats historiques au pilotage, au témoin/checkpoint réels et aux archives v0.2/v0.3.
Les résultats ci-dessous du 5 octobre restent attachés à leur propre révision et profil.

Campagne locale du 5 octobre 2026, workspace fondé sur `f3df383` avec les modifications
de ce lot. La PR #136 a été fusionnée à `4757e6e` ; son contenu documentaire est présent
dans cette base locale. Références : [plan de robustesse](audit-robustness-test-plan.md),
[protocole fichiers](file-storage-test-plan.md) et [résultat électrique Orin Nano](orin-nano-electrical-results.md).
La revue des compléments et la traçabilité du profil physique restent ouvertes.

## Résultats exécutés

Profil logiciel : Linux x86_64 7.0.0-34-generic, Clang/libc++ 21.1.8, GCC/libstdc++ 16.1,
CMake 4.2.3 et Python 3.14.6. Builds Release des deux compilateurs, Debug Clang ASan/UBSan
et Debug GCC TSan ; SpecLab reste figé à `d85c1f74d95a25b2bf2733316149ba7936ed7df6`.

| Vérification | Résultat |
| --- | --- |
| Modèle d’admission | 32 seeds × 2 000 décisions, file abstraite, refus sans consommation d’identité |
| Reprise générative | 64 seeds, trois opérations et trois positions de coupure, quatre politiques de suffixe, réponse du témoin perdue ; préfixes confirmés comparés aux copies indépendantes |
| Audit concurrent | Trois producteurs SPSC, un consommateur et un observateur ; 1 200 événements vérifiés, rejets et exceptions réessayés |
| Répétitions des scénarios ciblés | Quatre scénarios × 100 passages réussis ; le test d’arrêt exerce 2 000 cycles par passage |
| Stockage à 100 répétitions | 2 609 PASS, zéro FAIL, un SKIP pour le volume ; statut global PARTIAL |
| SHA-256 indépendant | 268 entrées concordent avec Python hashlib ; limites du padding et des blocs, jusqu’à 64 Kio |
| Fuzzing Clang ASan/UBSan | 100 000 entrées, seed 120, aucun crash/UB/timeout ; RSS maximal 262 Mio pour un budget de 512 Mio |
| TSan GCC | 26/26 scénarios RingLog, AuditRing, observateurs et SinkRegistry réussis, sans fichier de suppression |
| Contrôles documentaires Python | 42 tests, dont quatre contrôles négatifs du superviseur de robustesse |
| CTest Release final | 188/188 Clang, 187/187 GCC, consommateurs source/installé inclus |
| CTest Debug ASan/UBSan final | 182/182 réussis au rejeu complet ; consommateurs installés exclus |
| Style et analyse statique | 90 fichiers conformes à clang-format ; six unités C++ modifiées sans diagnostic clang-tidy restant |

Les journaux distinguent les passages initiaux, les corrections et les rejeux.

Le SKIP du volume est dû au refus des namespaces dans l’environnement local. Les essais
ENOSPC réels de la campagne précédente restent identifiés dans son rapport et sa CI ;
ce rejeu à 100 passages ne revendique pas leur nouvelle exécution. La CI longue ajoutée
impose une exécution distincte du volume plein en montage root privé.

LSan ne peut pas fonctionner sous le ptrace de l’environnement local : le premier essai
du probe instrumenté a échoué avec ce diagnostic, malgré 268 digests produits. Le fuzzing
réussi utilise explicitement `--disable-leak-detection` ; ASan et UBSan restent actifs.
La CI conserve LSan activé ; aucun résultat nouveau de cette CI n’est revendiqué ici.

## Couverture mesurée du fuzzing

Rapport LLVM du rejeu à 100 000 entrées, incluant l’instrumentation des corps inline
importés dans le harness. Les six lecteurs principaux possèdent une couverture exécutée.

| Unité | Branches couvertes |
| --- | --- |
| AuditCanonical | 76,88 % |
| AuditLayout | 88,46 % |
| AuditLedger | 38,95 % |
| AuditLog | 28,31 % |
| AuditLogVerifier | 25,84 % |
| AuditStore | 6,88 % |
| SHA-256 | 100 % |

Ces chiffres mesurent le harness des lecteurs, pas l’union avec les scénarios fonctionnels.
Les branches d’écriture, de rétention, de fournisseur et les ordres complexes de registres
ne sont pas entièrement couvertes par ce corpus. Les scénarios SpecLab les exercent séparément.
Aucune couverture exhaustive ni probabilité universelle de défaut n’est déduite de ce passage.

## Anomalies et corrections

L’examen du blocage initial du sink asynchrone a identifié une course possible dans
`SimpleLogger::shutdown()` : modification du prédicat sans le mutex de la condition,
avec notification perdue entre le test du prédicat et l’entrée en attente. La modification
utilise ce mutex et les 100 rejeux des deux scénarios concernés passent. Cela établit la
correction de la course identifiée ; sans trace de pile initiale, son lien avec l’ancien
blocage n’est pas affirmé comme une reproduction déterministe de sa cause.

Le premier rejeu GCC avec un délai global de 30 secondes a dépassé ce délai dans
`InstallTreeConsumer`. Son rejeu isolé à 120 secondes passe en environ 48 secondes.
Les journaux conservent le passage initial et le rejeu, sans transformer le timeout en succès.

Le premier passage ASan/UBSan comptait 180/182 réussites : le build de fuzzing propageait
ses options de lien au cœur seul et les contrôles de graphe et leurs contrôles négatifs
l’ont refusé. Les options appartiennent désormais à l’interface de l’adaptateur ; les
182 tests passent au rejeu complet après correction. Le SHA-256
du fuzzer reste identique à celui de la campagne de 100 000 entrées après ce déplacement
des options, donc les preuves de cette campagne correspondent au binaire final.

## Preuves et statut des issues

### Revue technique d’usage du backend

L’exemple `FileAudit.cpp` centralise répertoire, limites, durée de vie du support et
configuration du sink. `emit()` reçoit seulement un `AuditBinding` et conserve l’appel
d’admission `record()` ; il n’ouvre pas de fichier. Description et contexte sont créés
avant l’émission, la phase est `Requested` et l’heure indisponible reste explicitement
indisponible. Le consommateur réalise drainage et flush hors de ce point d’émission.
La fermeture du rédacteur précède la réouverture en lecture seule.

Les binaires Clang et GCC de l’exemple ont été exécutés : argument manquant refusé,
un événement écrit puis relu avec chaînage vérifié, répertoire non vide refusé lors
d’un second lancement. La position durable annoncée reste zéro en `Unqualified` ;
la lecture n’est pas présentée comme un ancrage indépendant. Le refus d’admission
interrompt cette démonstration via son résultat booléen ; une application doit exploiter
la raison structurée et appliquer sa propre politique de refus.

Cette revue technique couvre la composition autonome disponible. L’acceptation du
mainteneur et la revue du futur pilotage de #116 restent distinctes ; son cycle de vie,
le redémarrage applicatif et les décisions sur les refus ne sont pas qualifiés ici.

### Conservation des preuves

Les artefacts locaux restent ignorés par Git :

- `build-clang/storage-campaign-100/run-nkd_43r9/report.json` : 100 répétitions et SKIP explicite.
- `build-robustness-fuzz/campaign/run-n3hacx65/report.json` : fuzzing, hashes des workers/seeds,
  oracle SHA-256, corpus final, logs, profils LLVM et couverture.
- `build-clang/verification-audit-robustness/` : journaux `mddlog-robustness-*`, patch
  du lot et manifeste de révision, des sources modifiées et des binaires vérifiés.

Les métadonnées du build ont été ajoutées au rapport du fuzzing après l’exécution,
depuis le cache du build correspondant ; le hash du binaire a été contrôlé avant cet ajout.

Le workflow `Audit Robustness` ajoute campagnes courtes/longues, collecte de couverture
et archivage ; le workflow TSan élargit sa sélection à AuditRing et aux observateurs.
Ces modifications de CI sont configurées et doivent être exécutées à la révision soumise.

### Compléments après revue de la PR #137 — 6 octobre 2026

La tête initiale `352a2b3` a passé les 18 contrôles Actions et le statut de revue
CodeRabbit. Le [workflow court de robustesse](https://github.com/ambroise-leclerc/mddlog/actions/runs/37427941939)
et les autres builds/sanitizers sont donc exécutés ; cela ne constitue pas un passage
de la campagne manuelle/hebdomadaire à un million d’entrées.

La revue a relevé deux corrections : les probes de versions absentes ou trop longues
sont désormais consignées comme indisponibles sans invalider l’oracle, et VER-035 est
rattachée au contrôle d’arrêt diagnostic CTRL-015, distinct de CTRL-010/REQ-015.
Les 44 tests Python passent après ces corrections, dont une campagne SHA-256 avec
CMake, Ninja et compilateur absents et un probe de version dépassant son délai.
Le probe C++ SHA-256 concorde encore sur 268 entrées avec le superviseur corrigé.
Ces corrections relancent la CI ; elles ne reprennent pas à leur compte les résultats
de la tête initiale comme s’ils avaient déjà été exécutés sur le nouveau commit.

### Suite des observations de revue — 6 octobre 2026

Les métadonnées Git sont facultatives et collectées dans la portée protégée : absence
de Git ou timeout n’empêchent plus le rapport ni la comparaison SHA-256. Les 48 tests
Python passent, incluant ces deux cas, le nettoyage de deux essais éphémères réussis,
la conservation d’un essai en échec et le refus du nettoyage avec un fuzzer.

CTest active ce nettoyage pour l’oracle. Les inventaires avant/après trois répétitions
des cinq scénarios ciblés ne croissent ni pour les profils à la racine, ni pour les
dossiers SHA-256 réussis, en Release et en ASan/UBSan. Les profils sont jetés par défaut ;
un nouveau rejeu de 10 000 entrées conserve bien les profils choisis par le superviseur
et couvre les six lecteurs. Son rapport est
`build-robustness-fuzz/review-campaign/run-djyd9weu/report.json` ; le hash du fuzzer est
`a1b25d1d498d0a1b05f52ce6a000e8f3be8fd311f3460afc03f92f55128266d1`.
Ce nouveau binaire ne reprend pas le hash de la campagne initiale de 100 000 entrées.

La CI sépare désormais les scénarios ASan/UBSan du build des adaptateurs instrumentés
pour le fuzzer et déclenche le contrôle aussi pour une PR documentaire. Le runtime
Linux mixte et les imports préalables au header sont explicités dans les sources/docs.
La cible manuelle `campaign=volume` a été exécutée sur `20daa0f` :
[run 37455927395](https://github.com/ambroise-leclerc/mddlog/actions/runs/37455927395),
job distinct `File storage ENOSPC (Clang 21)` réussi. Le rapport téléchargé contient
un cas agrégé PASS et les trois chemins ENOSPC attendus (`append-data`, `reserve`,
`open-data`), zéro FAIL et zéro SKIP. Les erreurs portent `errno = 28`, arrêtent les
mutations et ne confirment aucune position ; les préfixes et reprises sont vérifiés.

Profil : Linux x86_64, Clang/libc++ 21.1.8, Debug ASan/UBSan, UID 0 dans un namespace
de montage privé, tmpfs de 1 Mio avec 256 inodes. Le build du worker a le fuzzing désactivé.
La restauration de propriété et l’archivage réussissent. L’artefact
`audit-robustness-20daa0fb44490ecec7a9c231f0c09f4f8b0f723c` porte l’identifiant
`11408613765` ; le SHA-256 du rapport est
`ea3839e942824c933f3458a087761c9dcfa81c3670120874c816c1d56332a4ec` et celui du worker
`b47b43517e0b2ba7a82fdb87adf3687b025945ed35c0e9f036f9b29f9041c3be`.
La copie et sa vérification restent dans
`build-clang/verification-audit-robustness/volume-ci-37455927395/`.

Ce passage apporte la preuve du volume plein réel obligatoire dans le nouveau workflow.
Les étapes de concurrence et de fuzzing y sont SKIPPED, et les 100 interruptions ne sont
pas demandées en mode volume ; aucune campagne longue omise n’est annoncée réussie.
Ce tmpfs teste les erreurs système ; il ne qualifie pas la persistance contre coupure
électrique, les caches ni le stockage de l’Orin Nano.

| Issue | Travaux démontrés par ce lot | Éléments restant nécessaires à la clôture actuelle |
| --- | --- | --- |
| #114 | Conformité, erreurs réelles précédentes, reprises SIGKILL à 100 passages ; succès électrique déclaré sur Orin Nano ; revue technique de l’exemple | Paramètres et preuves du profil électrique, acceptation des revues du profil et de l’ergonomie, raccordement au pilotage de #116 |
| #120 | Modèles génératifs, fuzzing/corpus, oracle indépendant, concurrence/TSan, campagnes longues et rapports | Revue des preuves, CI effective du lot, témoin réel et position retenue de #115, campagnes du pilotage de #116 et archives interversions |

Le maintien ou le transfert explicite des critères d’intégration vers #115/#116/#121
reste à décider. Aucun critère n’est supprimé par la réussite des tests logiciels.
GAP-003 et GAP-007 restent ouverts jusqu’aux décisions et preuves correspondantes ; GAP-013 est
clos depuis la décision sur #148 consignée plus bas.

## Dossier électrique reçu le 6 octobre 2026

Après la fusion de #137, le mainteneur fournit et confirme la provenance réelle de la
campagne Orin Nano. Le [bilan actualisé](orin-nano-electrical-results.md) et la
[revue documentaire](orin-nano-campaign-review.md) publient le profil, 2 913 tentatives
et leurs événements avec les mentions du template retirées. Les réserves de traçabilité,
d’oracle et de preuves externes sont explicites ; la qualification reste ouverte.
Cette réception n’ajoute aucun nouveau résultat aux campagnes logicielles ci-dessus.

## Complément de chaîne réelle du 8 octobre 2026

Base `197574f2ceefb72a7e0aaebe6ad322cce2e8fe7d`, workspace du lot #120 non encore
accepté. [Modèle, traçabilité, protocole et réserves](audit-chain-campaign.md) ; VER-070–072.
Aucune modification des quatre contrats ni des modules déployés. Tuple local : Linux
x86_64 7.0.0-34-generic, Clang/libc++ 21.1.8, GCC/libstdc++ 16.1.0, CMake 4.2.3,
Python 3.14.6 ; SpecLab inchangé au SHA épinglé. Les rapports JSON conservent options,
versions, révision/worktree, commandes et empreintes des workers/sources/traces/corpus.

| Vérification exécutée | Résultat et borne |
| --- | --- |
| CTest Clang Release final | 238/238 réussis, y compris consommateurs source/installé, witness.deployment et les deux archives |
| Nouveau banc GCC Release | Compilation/lien réussis ; audit.chain-campaign passe, sans attribuer un nouveau passage à toute la suite GCC |
| Profil réel prolongé Clang Release | 32 seeds 120–151 × 24 flux × 64 événements ; 49 152 événements de référence, 768 fautes terminales, 1 000 refus puis 1 000 polls sans fournisseur par flux ; reprise supplémentaire de chaque dernier flux |
| Isolation réelle répétée | 32 invocations de VER-044 avec --require-isolation, toutes réussies ; autorités writer/witness/reader/retirement sous UID distincts |
| Profil Clang Debug ASan/UBSan | 8 seeds × 6 flux × 64 événements, 3 072 événements de référence et 928 records relus après rétention/reprise ; quatre invocations multi-UID et relecture des deux archives réussies |
| Fuzzing avec archives réelles | 100 000 entrées seed 120, douze seeds initiaux incluant les quatre segments originaux v0.2/v0.3 ; aucun crash, UB ou timeout ; couverture des six lecteurs collectée |
| Oracle SHA-256 | 268 inputs concordent avec hashlib ; nouvel oracle de chaîne reconstruit aussi les octets depuis les requêtes, pas depuis decodeCanonical |
| Contrôles documentaires | 64 tests réussis et registre/références vérifiés ; contrôles négatifs du modèle et fidélité byte-for-byte des seeds aux archives inclus |
| Format et analyse | 119 fichiers conformes à clang-format 21 ; nouvelle unité AuditChainCampaign analysée par clang-tidy 21 sans diagnostic utilisateur |

Le profil ASan/UBSan utilise `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1` localement,
avec UBSan halt-on-error ; le fuzzing utilise explicitement --disable-leak-detection.
LSan n’est donc pas vérifié localement, conformément à la limite ptrace déjà consignée.
La CI conserve LSan et impose le namespace multi-UID ; sa nouvelle configuration n’est
pas annoncée comme une exécution réussie. Le nouveau banc est séquentiel ; aucun nouveau
résultat TSan n’est revendiqué et le suivi antérieur libc++/TSan reste ouvert.

La campagne de fuzzing finale est `build-robustness-fuzz/120-releases/run-ftawngvp/report.json`
(184,07 s). Le profil ASan/UBSan final est
`build-120-asan/120-delivery/run-fg8vjs8a/report.json` (43,79 s), worker SHA-256
`14861a326fbb32f01b43ee2933d4d7427d836c9ccc9e41fca84164f9462eafb6`.
Le profil prolongé final est `build-clang/120-final/run-6xlfo1p7/report.json`
(209,84 s), worker SHA-256
`99219f32744cd66d1e143ab90eddbca7f073d1c4b14f92b36042559becc3c8e1` ; 14 848 records
relus après rétention/reprise. Les intégrations multi-UID et les deux corpus sont PASS.
Ces captures locales restaient hors Git lors de ce relevé. La campagne CI de référence
et sa conservation versionnée sont consignées dans le complément ci-dessous.

La nouvelle archive v0.3 a été réellement produite par une reconstruction séparée de
`073761b7a6d5ed29ed87bc37c85967db72386d3c` ; protocole, hash et distinction entre
production d’octets par la release et durabilité physique sont dans le
[README du corpus](../tests/archives/audit-export/README.md). La v0.2 est conservée
intacte. Le support des futurs formats/releases et sa durée restent dus à #121.

Anomalies du banc corrigées puis rejouées : l’injection du reclaim a d’abord atteint
une barrière de rotation du ledger ; elle est maintenant armée au vrai reclaim.
Le contrôle terminal a ensuite additionné takenUnacknowledged à pendingInRings : le
premier est une partie du second, donc le modèle comptait deux fois un refus de sink.
La relation correcte transmis + pending = admis et le reporting des pertes après
transmission sont vérifiés dans le modèle et ses contrôles négatifs. Les premiers FAIL
restent dans leurs répertoires de campagne, distincts des passages finaux. Les nouveaux
contrôles CLI d’ordre ont aussi été rejoués après correction du chemin fields.sequence
selon le schéma de projection.

Livraison proposée à la revue du mainteneur. GAP-003/013, les qualifications physiques,
la politique de futures archives #121 et l’acceptation explicite #122 restent ouverts.
Aucun succès fini n’est présenté comme une preuve exhaustive ni une certification.

Couverture LLVM du fuzzing final (branche exécutée / branches totales) :
AuditCanonical 123/160, AuditLayout 47/54, AuditLedger 96/172, AuditLog 152/506,
AuditLogVerifier 58/192, AuditStore 55/650. Les chemins non couverts restent présents
et visibles dans coverage.json/coverage.txt ; en particulier le fuzzing de lecteurs ne
prouve pas les branches de mutation et de rétention du sink. Les campagnes de chaîne
les exercent séparément sans additionner leurs résultats à ces pourcentages.

### Complément après revue de PR #146

La revue a révélé un faux positif du superviseur : une borne `through == records`
initialisait le compteur de reprise à sa valeur attendue sans exiger de record relu.
Le cas a été reproduit sur un flux initial et le flux final ; la régression couvre les
trois producteurs du profil court. L’oracle exige maintenant `recovery_started` pour
chaque flux, sans exception finale : le worker exécute déjà un boot de récupération
supplémentaire pour ce dernier. La ponctuation du passage décrivant cette reprise est
également corrigée.

65 tests documentaires et les trois CTests de chaîne/outils passent après ce correctif.
Les 32 histoires Release et les huit histoires ASan/UBSan précédentes ont été revalidées
par le nouvel oracle : leurs hashes de trace sont inchangés, les 49 152 / 3 072 records
de référence et 14 848 / 928 records relus restent concordants. Ce contrôle revalide les
captures existantes ; il ne constitue pas une nouvelle campagne du worker inchangé.
Rapport local : `build-clang/120-review-oracle.json`, SHA-256 de l’oracle corrigé
`890fd3673250496e61aedf4d93535ac51c4dac63a084b29f3a8c49a3177b26a6`.

### Revue du périmètre CI et de la provenance (#120)

Le générateur v0.3 émet désormais lui-même producerRevision, producerStream et
eventCount ; la procédure exacte du README a été rejouée sur la release figée
`073761b7a6d5ed29ed87bc37c85967db72386d3c`. La reconstruction et le générateur
compilés/liés avec Clang/libc++ 21.1.8 produisent le même JSON octet pour octet et
la même empreinte `0852b676669ab7570753c61b139d16524b7e4ec1ce8c721116c73585052761e2`.

Le profil CI élève seulement run-witness-deployment.py, conserve le superviseur,
les workers de chaîne et les CLI sous l’utilisateur du runner, et relève le budget
de l’étape à 110 minutes et celui du job à 300 minutes. Le calcul des plafonds et
la limite des admissions observées via la trace figurent dans audit-chain-campaign.md.
Les bornes worker/superviseur exigent maintenant toutes deux au moins trois boots.
Les empreintes des sources incluent les scripts d’intégration et les deux générateurs.

Validation locale de ce correctif : trois CTests de chaîne/archives, formatage des
119 fichiers et contrôle du dossier réussis. Un nouveau contrôle négatif exige
FAIL si l’élévation du déploiement est refusée ; il vérifie aussi que l’histoire
précédente reste sans sudo et que les sources supplémentaires sont hachées.
L’exécution CI prolongée sur la branche sera consignée dans la PR avec son SHA
et ses artefacts ; cette configuration ne constitue pas encore sa réussite.

### Conservation Git et disposition de clôture de #120

La PR #146 a été approuvée par AM-L le 2026-10-08 sur
`6bf13770bd7732ab2640fc6fe28a06ccf5d38d32`, puis fusionnée à
`b73ab46acb7e6d008535b3871c1980e1f7b94fc3`. Son workflow manuel prolongé
37810541676 réussit aussi sur la révision testée. Les
[preuves conservées dans Git](validation/audit-robustness/2026-10-08/README.md)
contiennent l’archive originale de 7 482 376 octets, les quatre rapports originaux
lisibles, l’inventaire SHA-256 des 8 109 membres et les logs sélectionnés de build,
sanitizers et campagne. L’expiration GitHub n’affecte plus ces octets.

La campagne CI avec LSan actif confirme 32 histoires × 24 flux × 64 événements,
49 152 événements de référence, 14 848 relus et 768 fautes terminales ; 32
déploiements isolés requis et les deux archives passent. Durée : 264,30 s. Le
fuzzing exécute 1 000 000 entrées et l’oracle SHA-256 268 entrées. Stockage :
100 répétitions, 2 609 PASS, zéro FAIL et un SKIP ENOSPC sans privilèges ; le
volume dédié sous sudo produit ensuite PASS sans SKIP. Le statut PARTIAL du
rapport des 100 répétitions est conservé, sans le transformer en PASS.

Le vérificateur `scripts/verify-audit-evidence.py` contrôle hors ligne toutes les
empreintes, les copies des rapports, leur provenance, les comptes et intégrations,
puis rejoue les 32 traces avec l’oracle Python. Ce contrôle n’exécute ni worker
C++, ni sudo et ne constitue pas une nouvelle campagne. La CI documentaire
vérifie ce jeu et les contrôles de corruption. Conservation avec l’historique du
projet, sans expiration automatique, suppression ni remplacement par d’autres
campagnes ; les nouveaux jeux prennent un nouveau répertoire daté.

Disposition acceptée : clôture logicielle de #120/GAP-013 pour les profils bornés
réalisés et revue/fusion #146, par la décision nominative AM-L : PR #148 approuvée le 2026-10-08 à 21:58:47 UTC sur `f5b015c08a0fb9bda55ee792f039309d86debc5e`, fusionnée à `2b606df47d5bf2d7db5e021a8bf359173ee5490e`. GAP-003 reste ouvert sous #122 pour la
traçabilité du déploiement réel et l’acceptation finale. Le suivi autonome libc++
est transféré explicitement à #147, avec sa source et son observation conservées
dans Git ; il n’est ni résolu ni déclaré faux positif et reste à évaluer pour la
matrice #121 et la candidate #122. La conservation demandée ne qualifie ni le
stockage physique, ni le témoin de production, ni le dossier final.
