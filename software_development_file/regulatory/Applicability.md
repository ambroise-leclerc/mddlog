# Périmètre et applicabilité

Maîtrise documentaire et statut : [index du dossier](../README.md).

mddlog est un composant C++23 pour diagnostics et événements d'audit structurés. L'hôte fournit
le temps, les identités de flux, les threads, la politique de refus et les supports externes.
Le projet fournit contrats, implémentations et contrôles ciblés. Le fabricant décide du rôle
sécuritaire de ces événements dans son dispositif et de la qualification de l'intégration.

Usages visés : instrumentation explicite, admission bornée en mémoire, transmission et chaîne
persistante lorsque l'hôte apporte un support et un ancrage qualifiés. Usages exclus des garanties
actuelles : preuve clinique, preuve d'exécution par admission, durabilité sur support non qualifié,
authentification d'auteur, horloge fiable implicite et garantie temporelle du système complet.
La classe de sécurité IEC 62304 dépend du dispositif et de ses mesures de maîtrise ; le titre
de l'ADR-001 ne classe pas automatiquement mddlog ni toutes ses intégrations en classe C.

## Matrice retenue pour la base initiale de #124

| Référence retenue et source officielle | Portée proposée et justification | Responsable de revue |
| --- | --- | --- |
| [IEC 62304:2006 + AMD1:2015](https://webstore.iec.ch/en/publication/22790), édition de base 1 + amendement 1 | Référence de cycle de vie du composant : architecture, conception, maintenance, configuration et problèmes. Pas de revendication de conformité intégrale. | Ambroise Leclerc ; classification système par le fabricant |
| [ISO 14971:2019](https://www.iso.org/standard/72704.html), édition 3 | Analyse des défaillances et hypothèses du composant utiles au dossier du dispositif. Acceptabilité clinique et risque global au fabricant. | Ambroise Leclerc ; responsable risques du fabricant |
| [ISO 13485:2016](https://www.iso.org/standard/59752.html), édition 3 | Contribution documentaire, revue et configuration ; certification et fonctionnement du système qualité hors périmètre du composant. | Ambroise Leclerc ; responsable qualité du fabricant |
| [IEC 81001-5-1:2021](https://webstore.iec.ch/en/publication/63293), édition 1, version corrigée 2025-12 incluant ISH1:2025 | Référence proposée pour menaces, dépendances et maintenance de sécurité ; portée et feuille d'interprétation à examiner sur une copie autorisée. | Ambroise Leclerc ; responsable sécurité du fabricant |
| [IEC 62366-1:2015](https://webstore.iec.ch/en/publication/21863) + [AMD1:2020](https://webstore.iec.ch/en/publication/59980), édition de base 1 + amendement 1 | Évaluation d'interface utilisateur de dispositif non applicable au seul composant sans interface clinique. Modèle fourni pour l'intégrateur ; la lisibilité d'une API développeur n'est pas cette évaluation. | Ambroise Leclerc ; responsable aptitude à l'utilisation du fabricant |

Les fiches éditeurs ont été consultées le 2026-10-04 pour les éditions et le périmètre général.
Le texte intégral des normes n'a pas été examiné : **références de clauses à confirmer** dans
GAP-002. Ce dossier n'attribue aucun numéro de clause et ne reprend pas ceux du modèle MduX.
Les éditions ci-dessus sont la base documentaire retenue, pas une déclaration des normes applicables dans
chaque juridiction ; le fabricant examine les éditions, amendements et adoptions locales requis.

Pour IEC 81001-5-1, la [fiche de la publication](https://webstore.iec.ch/en/publication/63293)
consultée le 2026-10-04 indique une version corrigée 2025-12 et l'inclusion de la feuille
d'interprétation ; la [fiche ISH1:2025](https://webstore.iec.ch/en/publication/108664) identifie
séparément cette feuille. Cela confirme la désignation éditoriale de la matrice, pas le contenu
normatif ni ses effets sur ce composant. Leur examen sur copie autorisée reste GAP-002.
