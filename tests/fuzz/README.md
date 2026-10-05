# Corpus des lecteurs d’audit

Les fichiers `corpus/*.hex` représentent des octets, convertis dans un corpus privé du
build par `scripts/run-audit-robustness.py`. Leur SHA-256 figure dans chaque rapport.

`canonical-v1` à `canonical-v4` sont les **quatre vecteurs** de l’ADR-004 8.7, tous au
format canonique version 1 ; leur numéro n’est pas une version de format.
`layout-v1` contient ces vecteurs dans un segment dont les digests sont calculés par
Python hashlib et les CRC-32C selon le polynôme de l’ADR. `layout-cut` en tronque la fin.
`unknown-version` et `ledger-origin` atteignent les refus de version et le registre.
Ces fixtures ne revendiquent aucune provenance d’archive produite par une release.

Ne modifier le corpus source qu’après revue. Les mutations générées, crashs et résultats
restent sous le build et sont archivés par le workflow `Audit Robustness`.
