# Corpus des lecteurs d’audit

Les fichiers `corpus/*.hex` représentent des octets, convertis dans un corpus privé du
build par `scripts/run-audit-robustness.py`. Leur SHA-256 figure dans chaque rapport.

`canonical-v1` à `canonical-v4` sont les **quatre vecteurs** de l’ADR-004 8.7, tous au
format canonique version 1 ; leur numéro n’est pas une version de format.
`layout-v1` contient ces vecteurs dans un segment dont les digests sont calculés par
Python hashlib et les CRC-32C selon le polynôme de l’ADR. `layout-cut` en tronque la fin.
`unknown-version` et `ledger-origin` atteignent les refus de version et le registre.
Ces huit fixtures initiales ne revendiquent aucune provenance d’archive produite par une release.

`real-v02-ledger` et `real-v02-producer` sont les segments 1 et 2, sans transformation, de
[la vraie archive v0.2.0](../archives/audit-export/README.md), produite à la révision
`e7012f299b3c976b37b43b53add43e5e6e4636b8`. Leur origine est le champ `segments[].hex`
de `tests/archives/audit-export/v0.2.0.json` ; comparer les octets avant toute modification.
`real-v03-ledger` et `real-v03-producer` reproduisent les deux segments de la release
v0.3.0 reconstruite à `073761b7a6d5ed29ed87bc37c85967db72386d3c`, selon le même protocole.
Les deux releases productrices d’audit publiées sont ainsi présentes ; les futures releases
et la durée de support restent #121.

Ne modifier le corpus source qu’après revue. Les mutations générées, crashs et résultats
restent sous le build et sont archivés par le workflow `Audit Robustness`.

## Limites du build instrumenté

Sous Linux, le runtime libFuzzer de la distribution est construit avec libstdc++, alors
que mddlog utilise libc++. Le lien `-Wl,-lstdc++` est limité au fuzzer et seule l’interface
C de libFuzzer traverse cette frontière. Ce montage est vérifié avec Clang 21 sur la CI
Linux actuelle ; il n’est pas une garantie de compatibilité avec chaque runtime Clang
ou distribution. Toute évolution du tuple doit rejouer compilation, lien et campagne ;
préférer un runtime libFuzzer construit avec la même bibliothèque standard si disponible.

`MDDLOG_BUILD_FUZZERS` instrumente les adaptateurs et le harness : les consommateurs de
ce build peuvent donc subir le coût de ces compteurs. La CI construit les scénarios
ASan/UBSan dans `build-robustness` et le fuzzer seul dans `build-robustness-fuzz`.
Les autres builds n’activent pas cette option. Sans `LLVM_PROFILE_FILE`, les profils
sont jetés vers le périphérique nul ; le superviseur le définit explicitement dans son
répertoire de campagne pour conserver la couverture. Les répétitions CTest et la
découverte des scénarios ne créent ainsi pas un fichier de profil par processus.

CTest demande `--ephemeral` pour l’oracle SHA-256 : un passage réussi est nettoyé,
un échec garde son rapport. Les campagnes explicites conservent toujours leurs preuves
par défaut et le mode éphémère est refusé avec un fuzzer.
