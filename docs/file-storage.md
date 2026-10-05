# Premier lot du support fichiers — #114 / programme #112

État : backend Linux implémenté, contrat soumis à revue ; qualification d'un déploiement
et clôture de #114 ouvertes. Ce lot suit #113 et conserve ADR-001 à ADR-004. Il ne décide
pas le fournisseur indépendant #115, le pilotage #116 ni les budgets globaux du lecteur #117.

## Composition

Le module `mddlog.adapter.filestoragemedium` est inclus dans `mddlog::mddlog` avec
`MDDLOG_BUILD_FILE_STORAGE=ON` (défaut sous Linux uniquement). Il n'est pas réexporté
par la façade générale. `OFF` retire le backend et ses exemples/tests ; `mddlog::core`
ne l'importe ni ne le lie. Demander `ON` ailleurs que sous Linux échoue à la configuration.
Le package installé expose le module lorsque cette option a été activée.

```cpp
import mddlog.adapter.filestoragemedium;

using namespace mddlog::adapter;
auto medium = FileStorageMedium::create({
    .directory = "/var/lib/my-device/audit",
    .maxSegments = 64,
    .maxSegmentBytes = 65536,
    .maxReadBytes = 65536,
    .durability = FileDurability::Unqualified
});
```

La factory retourne un `expected<unique_ptr<FileStorageMedium>, FileStorageError>`.
Le support doit vivre plus longtemps que le sink. L'hôte fournit des limites compatibles
avec `StorageConfig`, notamment taille et nombre de segments ; tous les fichiers existants
comptent dans ces limites, y compris une création interrompue. Les limites ne réservent pas
de blocs physiques. La capacité réelle peut manquer avant la limite configurée.

Le [programme autonome](../examples/FileAudit.cpp) centralise configuration, admission,
drainage et vidage, puis ferme le rédacteur et vérifie le chaînage en lecture seule :

```bash
mkdir -m 700 /tmp/my-audit-demo
build-clang/examples/mddlog_file_audit /tmp/my-audit-demo device:unique-boot-1
```

Il exige un répertoire vide et une identité nouvelle. Il conserve `Unqualified`, annonce
une position durable nulle et ne présente pas la survie après fermeture comme une preuve
contre coupure électrique. `emit()` reste un appel d'admission court. Le pilote complet
avec registre, ancrage, rétention et redémarrage applicatif sera livré en #116.

## Profil et éligibilité

Premier profil d'implémentation : Linux, processus à consommateur unique, fichiers réguliers
locaux dans un répertoire privé déjà provisionné et persisté par l'hôte. Le profil candidat
pour qualification est un système de fichiers XFS local avec barrières actives, sans montage
réseau/FUSE, sans restauration concurrente ni dispositif qui ignore les commandes de vidage.
Ce choix candidat n'est pas une déclaration de qualification.

| Élément du déploiement | Exigence / état |
| --- | --- |
| Système, noyau, libc, montage | Versions et options exactes à archiver pour la campagne physique |
| Support, contrôleur, firmware | Modèles et versions à choisir et à archiver ; aucun matériel qualifié par ce lot |
| Cache volatile | Vidage correctement honoré, ou cache désactivé/protégé ; à prouver sur le matériel choisi |
| Primitives | `openat`, `write`, `pread`, `fstat`, `fsync` du fichier et du répertoire, `unlinkat`, `flock` |
| Limites du banc fonctionnel | 8 segments de 2048 octets ; lecture par appel limitée à 2048 octets |
| Budget de déploiement | À fixer/mesurer avec #117 ; pas de borne temporelle des appels système |
| Éligibilité livrée | `Unqualified` par défaut ; aucune détection automatique de durabilité |

`QualifiedFsync` est une **déclaration de l'intégrateur** après qualification de toute
la pile. Dans ce mode seulement, `sync` peut retourner `Durable`, après réussite de toutes
les barrières requises. Une écriture réussie, la fermeture d'un descripteur ou le seul choix
de XFS ne rendent pas une configuration éligible. Les essais qui sélectionnent ce mode
vérifient les branches de code et les appels ; ils ne qualifient pas leur machine.

## Opérations et interruptions

- **Création** : nom canonique de 16 chiffres hexadécimaux minuscules suivis de `.mdl`,
  identité opaque non nulle, `O_EXCL`, mode `0600`. Les identifiants de flux, indices et
  séquences du caller ne deviennent jamais des chemins ; le contenu fourni reste inchangé.
  La référence est stable tant que le segment existe et après réouverture ; elle peut être
  réutilisée après récupération de tout l'inventaire. Sa portée reste celle du support.
- **Ajout** : `O_APPEND`, boucle sur écritures courtes, reprise de `EINTR`, échec sur zéro
  progression. Aucun succès partiel. Les limites sont contrôlées avant écriture ; `ENOSPC`
  et `EDQUOT` à la création donnent `NoSpace`, les erreurs d'ajout donnent `Failed`.
- **Barrière** : contrôle que l'offset demandé est dans le fichier, `fsync` du fichier
  puis du répertoire, y compris pour une nouvelle existence et un préfixe vide.
  `Unqualified` retourne `Unsupported` sans barrière ni confirmation. En mode qualifié,
  un succès couvre tous les octets avant l'offset et l'existence du segment. Au-delà,
  les octets non confirmés peuvent être absents, partiels ou présents après coupure.
- **Échecs de mutation** : une écriture incomplète, une barrière échouée ou une suppression
  échouée arrête les mutations de l'objet. Une nouvelle tentative ne transforme jamais
  un échec de writeback en confirmation. Inspection possible, puis destruction, réouverture
  et reprise par les mécanismes existants ; pas de troncature ni réparation silencieuse.
  Un manque de capacité détecté avant mutation laisse l'objet utilisable.
- **Lecture** : `pread` par offset, gestion de lectures courtes et `EINTR`, plage limitée
  par `maxReadBytes`. Fin de fichier : vecteur vide ; segment absent, illisible ou erreur :
  `nullopt`. Une fin prématurée après observation de la taille est une erreur, pas un succès.
- **Inventaire** : balayage frais du répertoire, ordre croissant des références, tailles
  physiques. Noms non canoniques, fichiers non réguliers, liens symboliques ou multiples,
  propriétaire différent, permissions publiques et tailles/capacité hors limites sont refusés.
  Les octets malformés restent inventoriés : `checkMediumAtStart` et le lecteur existants
  établissent préambule absent, version inconnue, discontinuité et suffixe incomplet.
- **Suppression** : validation, `unlinkat`, puis `fsync` du répertoire avant `true`.
  Une réponse `false` peut correspondre à un retrait déjà visible mais non confirmé ;
  après coupure le segment peut réapparaître. Le registre et la reprise d'ADR-004 restent
  l'autorité pour expliquer/reprendre les retraits. Un segment déjà absent retourne `false`.
  Aucune suppression de segment n'est autorisée directement depuis le code métier.

Les descripteurs des segments utilisés restent ouverts jusqu'au retrait ou à la destruction,
notamment pour observer les erreurs de writeback sur le descripteur d'écriture. Leur nombre
est borné par `maxSegments`, en plus du répertoire et d'un descripteur temporaire de balayage.
La lecture et l'inventaire allouent dans l'adaptateur ; leurs limites locales ne qualifient
pas le coût global de `readStoredStream`. La destruction ne synchronise ni n'accepte d'audit.

## Propriété et santé

Le répertoire final appartient à l'utilisateur effectif, sans permission groupe/autres,
ainsi que les fichiers. Il est préexistant : sa création et la persistance de ses parents
appartiennent au provisionnement de l'hôte. Les parents et les chemins sont de confiance ;
le backend ne sécurise pas une traversée de parents contrôlés par un adversaire.

Un rédacteur prend un verrou exclusif non bloquant sur le répertoire. `ReadOnly` prend un
verrou partagé : lecteurs multiples hors ligne, refus pendant la vie d'un rédacteur. Le
verrou est consultatif et tous les participants doivent le respecter. Il ne protège ni
contre un utilisateur privilégié ni contre la réécriture/restauration par le propriétaire.
Confidentialité, sauvegarde et effacement sécurisé restent des obligations de l'intégrateur.

`lastError()` conserve la dernière cause et son `errno` éventuel ; succès sans effacement.
`mutationsStopped()` expose l'arrêt du rédacteur. Ces deux accès appartiennent au consommateur.
Les réponses de `StorageMedium` alimentent les compteurs/états existants de
`PersistingAuditSink::health()` ; elles ne génèrent aucun événement sur le support défaillant.
L'hôte publie la santé par un canal indépendant. Le seam `FileStorageCalls` est destiné
aux essais déterministes et doit vivre plus longtemps que le support si injecté.

## Vérification et travaux restant ouverts

[La suite commune](../tests/framework/StorageMediumConformance.hpp) vérifie création,
ajout, plages, EOF, inventaire, identités et retrait sur mémoire et fichiers.
[Les scénarios Linux](../tests/spec/FileStorageSpec.cpp) couvrent réouverture réelle avec
chaînage, accès exclusif/lecture seule, permissions/noms/liens, limites et injection de
transferts courts, `EINTR`, `ENOSPC`, `EACCES`, `EIO`, lecture défaillante et chacune des
barrières. L'exemple reste utilisable sans SpecLab.

La campagne locale est décrite dans [le rapport](file-storage-validation.md). Les injections
ne constituent ni un disque physiquement plein ni une permission refusée par le noyau sur
chaque primitive. Campagnes sur volume contraint, arrêt brutal du processus avec points
observés, et coupures électriques sur matériel choisi restent distinctes et ouvertes en
#114/#120. Position durable, ancrage et position retenue indépendants restent à éprouver
avec #115/#116/#122. GAP-007 reste ouvert ; aucune acceptation de preuves matérielles,
certification ou validation de dispositif ne découle de ce lot.

Références de primitives : [write(2)](https://man7.org/linux/man-pages/man2/write.2.html),
[fsync(2)](https://man7.org/linux/man-pages/man2/fsync.2.html),
[flock(2)](https://man7.org/linux/man-pages/man2/flock.2.html),
[open(2)](https://man7.org/linux/man-pages/man2/open.2.html),
[close(2)](https://man7.org/linux/man-pages/man2/close.2.html).
