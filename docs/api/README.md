# Inventaire de la surface publique (#121)

[public-surface.json](public-surface.json) est l’inventaire **candidat** de ce que mddlog expose.
Il couvre les 44 modules et leur cible/composant, les noms exportés par `import mddlog;`, les
cibles installées, les composants du paquet et la version de chaque format persistant ou échangé.
Chaque entrée porte un niveau et la version qui l’a introduite. Les niveaux et leurs promesses
sont définis par [ADR-007](../adr/ADR-007-compatibility-and-distribution.md) ; aucun n’est figé
avant l’acceptation #122.

| Niveau | Contenu actuel |
| --- | --- |
| stable-candidate | `mddlog`, `mddlog.log`, les 11 modules `mddlog.core.*`, 152 noms du module parapluie, `mddlog::core`, `mddlog::mddlog` |
| extension | 12 modules adaptateurs importés directement (stockage, service, vérificateur, liaisons, texte, transport, fichiers Linux) et `mddlog::audit_tool` |
| format-codec | `auditprojection`, `auditevidence`, `witnesscodec` |
| implementation-module | 16 modules dont seuls les noms repris par le parapluie sont supportés |
| reduction-candidate | 18 assistants bas niveau du parapluie (encodeurs, scanners de layout, couleurs console), à revoir avant le gel |
| test-double | `InMemoryStorageMedium`, `InMemoryAnchorProvider` |
| link-only | `mddlog::mddlog_options` |

## Contrôle

```bash
python3 -B scripts/check-compatibility.py          # inventaire, formats, extraits de migration
python3 -B scripts/check-compatibility.py --git    # + historique des tags publiés
```

Le contrôle refuse un module non enregistré ou non inventorié, une cible ou un composant différent
de CMake, un nom du parapluie ajouté ou retiré sans mise à jour, `mddlog_warnings` installé, une
constante de version de format modifiée sans l’inventaire, et un extrait du guide de migration
différent de sa région compilée. Avec `--git`, il vérifie aussi le champ `since` de chaque nom et
module. Il refuse l’absence d’un nom publié sans enregistrement `removed[]`, comme celle de
l’archive d’audit d’une release depuis v0.2.0. La CI documentaire l’exécute avec tout l’historique.

Ce contrôle ne prouve pas la compatibilité source : ce sont les consommateurs compilés
(`SourceTreeMigrationExamples`, `InstallTreeConsumer`, `InstallTreeCoreConsumer`,
`SourceSubdirectoryConsumer`, `SourceArchiveConsumer`) qui l’établissent pour leurs usages.
Il ne vérifie pas non plus les signatures C++ des noms inventoriés.

## Mettre à jour l’inventaire

Un ajout public nouveau reçoit `"since": "unreleased"`, remplacé par la version au moment de
la release. Un retrait suit la dépréciation d’ADR-007 et ajoute une entrée `removed[]` avec
versions et guide de migration. Un changement de format incrémente sa version, met à jour
`formats` et ajoute une archive de référence lors de la release suivante.
