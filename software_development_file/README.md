# Dossier de développement logiciel de mddlog

Base initiale du jalon 0 de [#112](https://github.com/ambroise-leclerc/mddlog/issues/112),
réalisée pour [#124](https://github.com/ambroise-leclerc/mddlog/issues/124).
Ce dossier décrit un composant de logging, ses hypothèses et ses limites. Il ne constitue
ni une certification, ni la validation d'un dispositif, ni un système qualité complet.

## Maîtrise documentaire

- Version du dossier : 1, préparation du 2026-10-04 ; statut : **base initiale acceptée**, selon la décision du registre.
- Base logicielle : v0.2.0, commit `e7012f299b3c976b37b43b53add43e5e6e4636b8`.
- Révision de develop examinée : `8344aba4e5516e446ea1b73e52b87547bb79bffb` ; même arbre que v0.2.0.
- Auteur : contributeur identifié par l'historique Git de chaque document.
- Responsable de la revue de projet : Ambroise Leclerc, mainteneur indiqué par [CODEOWNERS](../.github/CODEOWNERS).
- Date de décision : 2026-10-04 ; approbateur : Ambroise Leclerc.
- Révision acceptée : `7f0477a8e552e8eb44520ddc7372a6457e04995a` ; rapport de revue lié ci-dessous.
- Version et historique de chaque document : Git ; la révision du dossier soumis est le SHA
  du commit contenant les documents. Ne pas utiliser le SHA de base comme preuve de leur acceptation.
- La décision, la révision relue, la date et les dispositions sont consignées dans le champ
  `review` du [registre](register.json). Ne pas déduire une acceptation d'un test réussi.

La base initiale de #124 est soumise par Ambroise Leclerc, également responsable de la revue de projet et de
plusieurs lacunes. La décision initiale cumule auteur, relecteur et approbateur : elle est une **auto-revue non indépendante**.
Le projet assume cette limite organisationnelle ; aucune indépendance de revue ni satisfaction
d'exigences IEC 62304/ISO 13485 par ce cumul de rôles n'est revendiquée. La décision de `review`
consigne les cumuls et la limite d'indépendance, levant GAP-019. Le fabricant détermine les revues
et qualifications nécessaires dans son propre système qualité ; cette acceptation de projet
ne les remplace pas. Aucun relecteur indépendant n'est actuellement désigné.

Les métadonnées ci-dessus s'appliquent à tous les documents de `regulatory/` et au registre.
Les modèles sont des aides à remplir ; ils ne portent aucune décision pour un dispositif.

## Index

| Document rempli pour mddlog | Modèle pour l'intégrateur |
| --- | --- |
| [Applicabilité et périmètre](regulatory/Applicability.md) | [Applicabilité](templates/Applicability.md) |
| [Plans, configuration, anomalies et publication](regulatory/Plans.md) | [Plans](templates/Plans.md) |
| [Architecture](regulatory/IEC_62304/SAD.md) | [SAD](templates/IEC_62304/SAD.md) |
| [Conception détaillée](regulatory/IEC_62304/SDD.md) | [SDD](templates/IEC_62304/SDD.md) |
| [Composants tiers](regulatory/IEC_62304/SOUP.md) | [SOUP](templates/IEC_62304/SOUP.md) |
| [Gestion des risques](regulatory/ISO_14971/Risk_Management_File.md) | [Risques](templates/ISO_14971/Risk_Management_File.md) |
| [Cybersécurité](regulatory/IEC_81001/Cybersecurity_SAD.md) | [Cybersécurité](templates/IEC_81001/Cybersecurity_SAD.md) |
| [Périmètre qualité](regulatory/ISO_13485/README.md) | [Qualité](templates/ISO_13485/README.md) |
| [Applicabilité de l'aptitude à l'utilisation](regulatory/IEC_62366/Usability_Engineering_File.md) | [Aptitude à l'utilisation](templates/IEC_62366/Usability_Engineering_File.md) |

Le [registre JSON](register.json), format 2, est la **source unique** des exigences, risques,
contrôles, vérifications, dépendances et lacunes. Les documents expliquent ces données par
référence ; ils ne recopient pas leurs lignes. Une vérification lie conception et implémentation
à un test existant et à une note de preuves. Sa présence ne constitue pas un résultat d'exécution.
Les résultats effectifs restent dans les rapports de CI ou de campagne cités par les notes ;
Les résultats de campagne sont cités séparément dans les rapports ; leur portée reste celle
du profil exécuté.

Le [rapport de préparation de la revue initiale](regulatory/Initial_Baseline_Review.md) examine
la révision fusionnée de #125, consigne les vérifications et propose le traitement des réserves.
La décision nominative est enregistrée dans le registre ; GAP-001 et GAP-019 sont clos.
Les autres lacunes restent ouvertes, avec dispositions explicites pour la base initiale.

## Sources et provenance

[CMakeLists.txt](../CMakeLists.txt) fait foi pour les modules et les compilateurs admis ;
[les ADR](../docs/adr/README.md) pour les décisions ; [les tests](../tests/) et les rapports de
[frontière](../docs/governed-evidence.md), [scénarios](../docs/audit-scenario-validation.md) et
[persistance](../docs/audit-persistence-validation.md) pour la couverture et ses limites.
La présence d'une conception acceptée ne vaut pas acceptation de ses preuves.

Structure adaptée de [MduX au commit d972d77](https://github.com/ambroise-leclerc/MduX/tree/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/software_development_file),
en particulier son README, son modèle SAD et son dossier de risques. MduX et mddlog sont sous
EUPL-1.2 ; voir [LICENSE](../LICENSE) et la
[licence MduX à cette révision](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/LICENSE).
Adaptation du 2026-10-04 : textes rédigés pour mddlog, registres JSON documentaires,
suppression des API et classifications MduX et des références de clauses non confirmées.
Aucun texte normatif protégé n'est reproduit. Les notices d'origine restent dans les fichiers
vendus et leur licence ; les liens de provenance sont conservés ici.

## Utilisation et traçabilité

Un fabricant copie les modèles dans son propre dossier, fixe l'usage prévu, la classification
justifiée, le profil exact de construction et de déploiement, les responsabilités et les
critères de risque. Il complète les hypothèses système, qualifie support et fournisseur,
mesure les budgets et conserve ses propres résultats. Le dossier mddlog reste une entrée de
son évaluation, pas une acceptation transférable.

Chaîne documentaire : `REQ-*` → `RISK-*` éventuel → `CTRL-*` → ADR/conception → source →
`VER-*` → note de preuves → `GAP-*` et décision de revue. Les identifiants ne sont pas ajoutés
aux appels C++ ; `requirementRef` et `riskRef` gardent leur contrat d'audit facultatif.
Une exigence prévue a une lacune rattachée à son épique ; une exigence implémentée peut avoir
une couverture existante et une lacune de qualification. Le vocabulaire distingue :
**implémenté**, **vérifié sur un profil cité**, **accepté par décision explicite**, **prévu**,
**non applicable avec justification** et **lacune ouverte**. Aucun raccourci entre ces états.

## Vérification et revue

Depuis la racine, sans compiler le C++ :

```sh
python3 scripts/check-development-file.py
python3 -m unittest discover -s tests/documentation -p 'Test*.py'
```

Le contrôle vérifie la structure du registre, les identifiants uniques, les relations typées,
la compatibilité des statuts d'une exigence implémentée avec ses contrôles et vérifications,
les fichiers cités et les liens locaux du dossier (destinations avec parenthèses, titres et
ancres de titres Markdown). Les liens doivent être inline ; les liens de référence sont refusés.
Le contrôle lexical C++ exige un identifiant littéral passé à `speclab::Test`, avec ou sans
argument de template ; commentaires et autres chaînes ne suffisent pas. Il n'évalue pas les
macros, les branches de préprocesseur ni la sémantique C++.

Pour `review.status = accepted`, toutes les réserves doivent être closes. Une lacune ouverte
liée à une exigence implémentée doit être close ou figurer dans `review.acceptedGaps`, avec
une justification nominativement couverte par la décision de revue. Ce champ associe chaque
GAP à une justification non vide ; il reste vide avant acceptation. Il ne clôt pas la lacune
et ne transforme pas une qualification future en preuve acquise. Les exigences prévues gardent
leurs lacunes ouvertes. Ainsi la base initiale peut accepter explicitement ses limites sans
revendiquer les capacités différées. Une acceptation sans cette décision par lacune est refusée.

Le contrôle ne vérifie ni l'accessibilité des liens
externes, ni les clauses, ni la vérité d'une preuve, ni l'acceptabilité d'un risque.
La base initiale de #124 est acceptée avec les limites explicites de `review.acceptedGaps`.
GAP-002 est reporté vers #122 ; les autres qualifications conservent leurs épiques responsables.
La conception de #113 peut démarrer après livraison de cette décision sur develop ; la clôture
de #124 suit cette livraison. Cette décision ne vaut pas acceptation finale de la 1.0.
Chaque épique #113–#122 actualise les données concernées, la conception et ses preuves ;
#122 accepte le dossier final avant la publication selon le [processus de release](../docs/release-process.md).
