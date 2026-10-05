# Protocole d’essais du stockage fichiers — #114 / #120

Statut : protocole et banc logiciel soumis à revue. La qualification d’un déploiement
et l’acceptation finale restent ouvertes en GAP-007 et #122. Ce document complète
[le contrat](file-storage.md), [les vérifications initiales](file-storage-validation.md)
et [les règles de conservation des preuves](../software_development_file/regulatory/Plans.md).

## Objectif, frontières et oracle

Éprouver création, réservation d’identité, ajout, lecture, inventaire, synchronisation
et retrait, avec reprise dans un nouveau processus. Distinguer trois preuves : erreurs
injectées déterministes, erreurs réelles du noyau et arrêt réel du processus. Une quatrième
campagne, avec extinction du matériel, est requise pour revendiquer un profil durable.
Un arrêt par SIGKILL conserve noyau, caches et alimentation : il ne simule pas leur perte.

Le banc initialise deux segments dont les octets attendus sont fixés hors de l’implémentation.
Il obtient les barrières du préfixe initial avant interruption. Le superviseur conserve les
accusés et événements hors du répertoire du journal. Après SIGKILL, il compare directement
les fichiers à ces octets, puis compare l’inventaire et les lectures de `FileStorageMedium`
à cette observation indépendante. Il n’utilise pas le résultat du lecteur comme seul oracle.

Le worker sélectionne `QualifiedFsync` pour exercer les branches de confirmation ; cela
n’attribue aucune éligibilité au banc. Les préfixes du worker sont des octets de test,
pas des événements canoniques. Le scénario `file-storage-restart` couvre séparément
le sink réel, le décodage et le chaînage ; l’acceptation applicative relève de #116/#122.

Invariants attendus :

- Un préfixe initial confirmé reste identique, sauf retrait explicitement testé de son segment.
- `Opened` et `Written` ne sont jamais interprétés comme une confirmation durable.
- Une opération échouée ne retourne aucun succès durable ; les erreurs de mutation arrêtent
  le rédacteur et conservent la cause observée.
- Un suffixe incomplet reste un préfixe des octets demandés ; aucune réparation implicite.
- Une réservation incomplète avec `.mddlog-refs.tmp` entraîne `Inventory`, sans reprise silencieuse.
- Une réservation remplacée consomme l’identité même si le segment n’existe pas encore.
  Une nouvelle création utilise exactement compteur + 1 ; les références retirées ne reviennent pas.
- Après mort du processus, aucun verrou/descripteur ne bloque la nouvelle instance.
- Une entrée invalide entraîne un rejet explicite ; un rejet sémantique porte errno nul.

## Préparation et limites du banc

Linux, Python 3, `unshare`, `mount`, compilateur/CMake compatibles avec le projet.
Compiler avant exécution : configuration, compilation, liens et tests sont des résultats
distincts. Le backend doit être activé. Conserver le SHA logiciel, le diff éventuel, le SHA
du worker, le compilateur/bibliothèque standard/CMake/Ninja, les options et le montage.

Utiliser un répertoire d’artefacts dans le build, sur le système de fichiers du profil pour
les interruptions. Il est privé, réservé aux essais et ne contient jamais une archive réelle.
Les jeux de données sont indépendants ; les cas échoués et les journaux sont conservés.
Le banc ne supprime pas un résidu de réservation pour transformer un rejet en succès.

Budgets du banc livré : huit segments de 128 Kio maximum, lecture par appel de 128 Kio,
checkpoint attendu sous dix secondes, collecte après mort sous cinq secondes, appel normal
sous quinze secondes, sous-campagne de volume sous trente secondes. CTest impose 120 secondes.
Une invocation accepte 1 à 10 000 répétitions ; le nombre de processus et d’artefacts est
linéaire en ce nombre, sans parallélisme interne. Ne pas extrapoler ces délais à la bibliothèque.

Le volume plein est un tmpfs de **1 Mio et 256 inodes**, monté dans un espace de montage
privé. Avant remplissage, le banc contrôle changement de périphérique et capacité ≤ 2 Mio.
Le remplissage est plafonné à 2 Mio. Aucun volume existant de l’hôte n’est rempli.
Ce tmpfs établit ENOSPC réel, aucune persistance après redémarrage de la machine.

Les essais EACCES sont exécutés sans droits root. Si le superviseur est root, le worker
abandonne groupes et UID/GID pour 65534 dans un répertoire temporaire qui lui appartient.
Les autres essais conservent l’utilisateur du profil. Aucun changement de droits de l’hôte
n’est appliqué hors de ces répertoires d’essai.

## Matrice exécutable

| ID | Stimulus et exécution | Critère / preuve |
| --- | --- | --- |
| FS-01 | Suite commune `file-storage-conformance` sur mémoire/fichiers | Création, ajout, plages/EOF, inventaire, références et retrait conformes |
| FS-02 | `file-storage-restart`, `file-storage-health` | Sink réel relu et chaîné ; santé observable, position non qualifiée non promue |
| FS-03 | `file-storage-barriers`, `file-storage-failures`, `file-storage-reference-allocation` | Transferts courts/EINTR, ENOSPC/EDQUOT/EACCES/EIO injectés, zéro progression, chaque barrière et rename en erreur ; aucun faux succès |
| FS-04 | `file-storage-ownership`, `file-storage-reopening-protection`, `file-storage-inventory-diagnostics` | Propriété, verrou, liens/FIFO/inodes substitués, exec et diagnostics exacts |
| FS-05 | `kill-<répétition>-<point>` dans le rapport du superviseur | Point demandé observé, retour du worker égal à −SIGKILL, préfixes inchangés, reprise/identité selon oracle ci-dessus |
| FS-06 | `permission-open/append/read/reclaim` | EACCES réellement renvoyé par le noyau ; octets inchangés, refus explicite, arrêt sur mutation échouée |
| FS-07 | `real-enospc` : ajout, réservation puis création de données | ENOSPC réel sur volume privé plein ; ajout échoué, réservation interrompue bloquée, création partielle conservée sans référence exposée ; `NoSpace` vérifié |
| FS-08 | `counter-0..3` | Compteur vide, version inconnue, régressée ou hexadécimal invalide refusés à errno nul |
| FS-09 | `resources` | 64 créations/syncs/retraits et 64 démarrages refusés dans le même processus : nombre de descripteurs stable et références strictement croissantes |
| FS-10 | Tests Python `TestFileStorageCampaign.py` | Un worker qui sort sans checkpoint, un mauvais checkpoint, un préfixe perdu une fausse confirmation, une sortie excessive ou un volume obligatoire indisponible font échouer le banc |

Les 26 points FS-05 sont :

| Opération | Points observés |
| --- | --- |
| Création | `open.before`, `metadata.partial`, `metadata.written`, `metadata.file.before/after`, `metadata.rename.before/after`, `metadata.dir.before/after`, `segment.partial`, `segment.written`, `open.acknowledged` |
| Ajout | `append.before`, `append.partial`, `append.written`, `append.acknowledged` |
| Sync | `sync.file.before/after`, `sync.dir.before/after`, `sync.acknowledged` |
| Retrait | `reclaim.before`, `reclaim.unlinked`, `reclaim.dir.after`, `reclaim.acknowledged` |
| Lecture | `read.partial` |

Un checkpoint est émis et vidé sur stdout **après** le syscall lorsqu’il porte `.after`,
`.written`, `.partial` ou `.unlinked`. Le worker s’arrête par SIGSTOP ; le superviseur lui
envoie SIGKILL seulement après réception du point demandé. Les accusés d’opération sont
recueillis avant les checkpoints `.acknowledged` et doivent annoncer un succès effectif
(`Durable` pour sync, référence attendue pour open). Un timeout ou une sortie prématurée échoue,
il ne devient jamais une interruption réussie. Le seam limite les transferts aux checkpoints
partiels ; le syscall écrit/lit réellement les octets. Les erreurs de FS-06/07 ne sont pas injectées.

## Commandes et cadence

Campagne courte, exécutée par CTest lorsque le backend est activé :

```bash
cmake --preset ninja-clang
cmake --build --preset ninja-clang --parallel 4
ctest --test-dir build-clang --output-on-failure --parallel 4
python3 -m unittest discover -s tests/documentation -p 'Test*.py'
```

Rejeu explicite complet, volume obligatoire, dix répétitions de chaque point :

```bash
python3 scripts/run-file-storage-campaign.py \
  --worker build-clang/tests/mddlog_file_storage_campaign \
  --output build-clang/storage-campaign --iterations 10 --require-volume
```

Si les namespaces utilisateur sont interdits, le CTest court publie `SKIP` pour le volume
et un rapport global `PARTIAL`, tout en exerçant les autres cas. Ce n’est pas une preuve
ENOSPC. `--require-volume` transforme ce manque de prérequis en échec. La CI Clang impose
un essai distinct sur l’hôte, dans un namespace de montage root privé :

```bash
sudo python3 scripts/run-file-storage-campaign.py \
  --worker "$PWD/build-clang/tests/mddlog_file_storage_campaign" \
  --output "$PWD/build-clang/storage-full-volume" --volume-only --require-volume
```

Cette commande n’est destinée qu’à un banc Linux dédié où `sudo` est autorisé. Le CTest
normal ne demande ni n’obtient une élévation. La CI répète aussi FS-05 dix fois et archive
les répertoires de rapports avec `actions/upload-artifact`. Les rapports ne contiennent
aucun secret de configuration du déploiement.

Avant candidate et après changement du backend, compilateur, noyau, montage ou matériel :
100 répétitions par point sur chaque profil candidat, en plus de la CI courte et des sanitizers.
Exécuter `--iterations 100 --require-volume` dans un répertoire d’artefacts nouveau. Archiver
les résultats, y compris les échecs ; les campagnes à 100 répétitions ne sont pas revendiquées
par une exécution à dix. Enregistrer les budgets de temps/espace mesurés pour #117.

## Coupures d’alimentation : procédure à effectuer sur matériel

Le matériel n’est pas choisi par ce lot. Pour chaque tuple à qualifier, fixer avant campagne :
disque/support, contrôleur, firmware, caches, alimentation, noyau/libc, XFS et options/barrières,
état initial provisionné et persisté, versions du logiciel et moyen d’acquisition indépendant.
Changer l’un de ces éléments nécessite une analyse d’impact et un rejeu décidé en revue.

1. Réserver un dispositif et un volume d’essai ; identifier le moyen de coupure réellement
   capable de retirer l’alimentation du stockage et de ses caches. Consigner aussi l’absence
   de caches alimentés par une autre source. Un reboot ou SIGKILL n’est pas ce stimulus.
2. Conserver oracle et accusés sur une autre machine, hors du volume et de l’alimentation coupés.
   Initialiser les préfixes puis capturer les confirmations réelles du profil candidat.
3. Lancer le worker avec `<répertoire> <opération> <checkpoint>` ; sa sortie JSON est le point
   d’observation pour l’acquisition indépendante. Le SIGSTOP permet d’attendre le contrôleur.
   Le superviseur logiciel livré utilise SIGKILL : il ne pilote pas ce contrôleur physique.
4. Couper après réception du point demandé, sans shutdown/unmount/fsync supplémentaire.
   Mesurer et archiver le délai signal/coupure. Après restauration de l’alimentation, recueillir
   les diagnostics du système de fichiers avant toute réparation applicative.
5. Copier l’image brute et comparer les préfixes confirmés à l’oracle indépendant. Les octets
   non confirmés peuvent être absents, présents ou partiels selon le contrat ; aucune identité
   réservée confirmée ne peut être réattribuée. Un retrait confirmé doit persister ; un retrait
   non confirmé peut rester visible ou réapparaître. Un résidu de réservation bloque la reprise
   avec un diagnostic, sans remise à zéro automatique.
6. Pour chaque point FS-05 de mutation, effectuer au moins 100 cycles sur le tuple retenu.
   Ajouter des coupures à délais pseudo-aléatoires enregistrés pendant des transferts longs
   et aux limites de capacité. Le contrôleur, ses seeds et ses mesures doivent être archivés.
7. Répéter avec le journal canonique, les registres, le témoin indépendant et le lecteur retenu
   intégrés : perte du courant pendant reprise/rétention, redémarrages séparés, rollback combiné
   journal/ancrage et refus explicite en cas de sauvegarde du lecteur invalide. Ce complément
   dépend de #115/#116 et de l’application de #122 ; le worker de préfixes ne le remplace pas.

Critère d’acceptation physique : aucun préfixe/enregistrement confirmé perdu ou altéré, aucun
faux verdict de confirmation/protection, et traitement conforme de chaque état interrompu.
Chaque anomalie donne lieu à une issue avec stimulus, image, tuple, résultat attendu/observé
et cas réduit. Corriger puis rejouer le cas et les familles affectées. Zéro anomalie observée
dans 100 cycles est une preuve bornée, sans revendication de probabilité de panne universelle.

## Preuves, verdict et décision

Chaque invocation produit `run-*/report.json` : schéma 1, version du worker par SHA-256,
révision/diff, environnement/montage, paramètres, durée et statut de chaque cas, checkpoints,
accusés, lectures et errno observés. Les fichiers des interruptions et compteurs corrompus
restent dans ce répertoire. Les volumes tmpfs et répertoires de permissions sont temporaires ;
leurs observations sont conservées dans le rapport. Les descripteurs du superviseur sont comptés.

`PASS` : tous les cas de l’invocation exécutés et invariants satisfaits. `FAIL` : anomalie,
timeout, checkpoint absent, outil requis absent ou volume obligatoire indisponible ; code retour 1.
`PARTIAL` : prérequis du volume optionnel indisponible, cas identifié `SKIP` ; code retour 0
pour les tests portables du banc, sans prétendre avoir exécuté FS-07. Compilation défaillante
ou invocation invalide : échec, aucune campagne réussie revendiquée.

Archiver aussi commandes, CMakeCache/options, versions de la toolchain, stdout/stderr CTest,
rapport sanitizers, images et diagnostics physiques, SHA des artefacts, date et opérateur.
La conservation GitHub est limitée par sa politique : exporter les artefacts vers l’archive
de qualification avant expiration. La durée de conservation et l’acceptation sont décidées
par le responsable de profil et le mainteneur en #122, pas par le seul job vert.

Restent hors du banc automatisé livré : alimentation/caches matériels, EDQUOT réel et EIO
matériel sur une pile dédiée, authentification/indépendance du fournisseur et persistance de
la position du lecteur. EDQUOT/EIO sont injectés dans FS-03 ; la campagne réelle du fournisseur
est à définir en #115. Aucun `QualifiedFsync` de déploiement ni clôture de GAP-007 ne découle
du seul rapport logiciel. [Les résultats obtenus](file-storage-campaign-results.md) indiquent
exactement les essais réalisés et les prérequis manquants.
