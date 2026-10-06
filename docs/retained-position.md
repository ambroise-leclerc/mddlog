# Position retenue persistante — premier lot de #115

Ce document décrit le premier lot de sauvegarde du lecteur Linux. Il est complété dans
la même branche par le [témoin indépendant](independent-witness.md), son service authentifié
et sa persistance. La qualification physique du déploiement reste à établir dans #115/#122.

## Autorités et intégration

Le journal appartient au rédacteur. La position appartient au lecteur, sous une autre
identité système, sur son hôte ou dans un répertoire dont le rédacteur ne peut modifier
ni les fichiers ni les parents. Le lecteur provisionne et rend durable ce répertoire
privé avant utilisation. Un répertoire à côté du journal avec le même utilisateur ne
satisfait pas cette frontière. Le contrôle local exige propriétaire égal à l'UID effectif
et aucune permission groupe/autres ; il ne prouve ni séparation des comptes ni protection
des parents, ACL, montages ou privilèges administratifs. Ces propriétés sont à éprouver
sur le profil complet.

`import mddlog.adapter.fileretainedposition` est disponible avec
`MDDLOG_BUILD_FILE_STORAGE=ON`, sur Linux seulement. Le module reste dans l'adaptateur,
sans dépendance additionnelle du cœur. Les appels métier et l'admission d'audit ne changent pas.

La configuration est centralisée : `FileRetainedPosition{directory, enrolledProviderId}`.
Chaque magasin lie les positions de tous ses flux à **un seul fournisseur** ; ses heads
contiennent exactement cette identité. Changer d'identité exige une procédure d'enrôlement
et migration séparée, jamais un remplacement automatique. Cette contrainte évite d'associer
les anchors, indexés par flux dans `RetainedPosition`, à plusieurs autorités ambiguës.

Au premier enrôlement, l'hôte appelle explicitement `initialize(position)` ; un head à zéro
sans anchor permet un état initial vide. L'absence à la restauration est `Missing`, jamais
un succès avec une position vide. Une perte après enrôlement impose la politique de récupération
de l'hôte ; appeler `initialize` dans la branche d'erreur de `load` détruirait la protection.
Une sauvegarde existante, corrompue, inconnue ou d'un autre fournisseur n'est pas écrasée.

Le cycle normal du lecteur est :

```cpp
FileRetainedPosition checkpoint{readerDirectory, "enrolled-witness"};
auto restored = checkpoint.load();
if (!restored) {
    return reportRestorationFailure(restored.error());
}
AnchorVerifier verifier{provider, restored->position};
auto report = verifier.verify(streamId, records);
auto saved = checkpoint.save(restored->position, restored->generation);
if (!saved) {
    return reportCheckpointFailure(report, saved.error());
}
return reportPersistedVerification(report);
```

Les fonctions `report…` illustrent la politique de l'hôte, sans constituer une API du module.
Le verdict d'intégrité reste celui du vérificateur. Un succès de vérification ne prouve pas
la sauvegarde. Le lecteur expose séparément son échec de checkpoint et ne revendique la
protection après redémarrage qu'après un `save` réussi sur un support qualifié. Sans position
retenue, le verdict existant conserve « rollback non exclu ».

## Format v1 et invariants

Tous les entiers sont des uint64 little endian, sans padding natif. Chaînes : longueur uint64
puis octets, sans terminateur. Identité fournisseur : 1 à 1024 octets, aucun NUL ; flux :
identifiant valide d'`AuditEvent`, au plus 96 octets. Au plus 4096 anchors et 1 Mio par fichier.
Ces limites bornent la restauration ; elles ne sont pas des budgets temporels de #117.

| Champ | Encodage |
| --- | --- |
| Magic | 8 octets `MDDRETP` puis NUL |
| Version | uint64, 1 |
| Génération | uint64 strictement positif |
| Fournisseur | chaîne |
| Head du fournisseur | uint64 |
| Nombre d'anchors | uint64 |
| Chaque anchor | flux, position, counter, retired (uint64 0/1), digest de 32 octets |
| Contrôle final | SHA-256 de tous les octets précédents |

L'écrivain trie les anchors par leur clé de map. Le décodeur refuse doublons, champs tronqués,
octets excédentaires, version inconnue, checksum incorrect, position/counter nuls ou counter
supérieur au head. La somme contrôle la corruption accidentelle ; elle n'authentifie pas le
lecteur et ne protège pas d'une réécriture par son autorité.

`save(position, generation)` exige la génération courante et l'incrémente sans débordement.
Il conserve tous les flux et refuse la baisse des heads, positions et counters, le changement
de digest à position égale et toute modification d'un anchor déjà retraité. Un retrait est
irréversible. Le verrou `flock` non bloquant retourne `Busy` si une autre opération détient
le magasin ; après son relâchement, un lecteur ancien reçoit `Conflict`. Aucune fusion
silencieuse n'élimine un checkpoint concurrent. L'hôte recharge puis revérifie avec la position
courante avant une nouvelle tentative.

## Atomicité et échecs

`retained.lock` est un fichier privé stable. Sous son verrou exclusif, chaque opération lit
`retained.bin`. Les sauvegardes utilisent `retained.tmp` créé exclusivement, écrivent tous les
octets (reprise EINTR et écritures courtes), font fsync du fichier, rename atomique vers
`retained.bin`, puis fsync du répertoire. Le temporaire orphelin est ignoré par `load` et retiré
à la prochaine sauvegarde sous verrou. Liens symboliques finaux, fichiers non réguliers,
fichiers partagés ou liens physiques sont refusés. Les accès sont relatifs au descripteur
du répertoire, avec CLOEXEC et sans blocage sur FIFO.

Les erreurs système exposent l'errno ; les refus de format, identité, génération et
monotonie ont errno zéro. Les refus de type, propriétaire ou permissions d'un fichier
peuvent exposer EACCES. Aucun secret
n'est inclus. `Read`, `Write`, `Sync`, `Access`, `Busy`, `Conflict`, `Regression`, `Invalid`,
`UnsupportedVersion`, `ProviderMismatch` et `Missing` restent distincts.

Un échec avant rename laisse l'ancien checkpoint. Un échec de fsync du répertoire après
rename peut laisser le nouveau visible sans durabilité confirmée : recharger, réconcilier
et sauvegarder à nouveau avant d'affirmer la protection. La destruction ne synchronise rien.
La qualification filesystem, contrôleur, caches et alimentation est à établir ; les tests
de réouverture et d'injection ne la remplacent pas. La restauration malveillante de l'ensemble
du magasin du lecteur est hors modèle ; une garde externe est alors nécessaire.

## Service témoin complémentaire

Le [profil du témoin](independent-witness.md) implémente le service séparé, l'authentification
UID, les permissions d'avancement/retrait, la sauvegarde et la réconciliation des réponses
perdues. La [campagne d'intégration](independent-witness-validation.md) utilise cette
position retenue après redémarrage du lecteur avec le service réel. Le premier essai ci-dessous
conserve sa portée historique sur le fournisseur en mémoire.

## Vérification et limites

`tests/spec/FileRetainedPositionSpec.cpp`, scénario `audit-retained-file`, couvre sauvegarde,
réouverture, génération concurrente, régression/conflit, retrait, identité, perte, corruption,
version inconnue, droits, écritures courtes/EINTR et erreurs avant/après rename. Le rollback
combiné du journal et du fournisseur est détecté avec la position restaurée ; le fournisseur
est un double. Le consommateur fichiers importe aussi ce module depuis les sources et le
package installé. Résultats locaux : [rapport](retained-position-validation.md).

GAP-008 reste ouvert pour acceptation et qualification du profil. Les campagnes physiques
du lecteur, la migration opérée du fournisseur et les preuves finales #119/#120/#122 restent dues.
