# Note d'applicabilité de l'aptitude à l'utilisation

Maîtrise et statut : [index](../../README.md). Références : [matrice](../Applicability.md).

La bibliothèque expose une API de développeur et n'a pas d'interface clinique destinée à un
patient ou opérateur de dispositif. Un dossier d'évaluation d'interface de dispositif est donc
non applicable au composant seul, avec réexamen si une interface utilisateur de dispositif
est livrée. Le modèle correspondant aide le fabricant à constituer son propre dossier.

La revue d'ergonomie de #113/#122 porte sur appels courts, contexte explicite réutilisable,
configuration centralisée, orchestration hors métier et résultats d'audit explicites (REQ-009
à REQ-011). Elle utilise exemples avant/après et examen des erreurs d'usage. Une destruction
de portée ne prouve pas la réussite ; une admission n'est pas une exécution ou une durabilité.
Cette revue développeur ne remplace pas une évaluation IEC 62366-1 avec utilisateurs, tâches,
environnement et risques du dispositif. Aucun essai d'aptitude à l'utilisation de dispositif
n'est revendiqué dans ce dossier.


La [revue technique du premier pilotage](../../../docs/audit-service.md) examine
`FileAudit` : réglages et entretien sont au point de composition, l'émission métier
conserve `AuditBinding` et son résultat d'admission. Le rapport d'arrêt distingue fin du
cycle de vie, backlog et confirmation ; la relecture n'est pas annoncée comme durabilité
qualifiée. Cette revue de code reste limitée au lot #114/#116, avec acceptation du
profil matériel et évaluation applicative toujours ouvertes.


La composition #116 ajoute `AuditApplication` (VER-051), portable, et la composition
réelle à deux producteurs (VER-044). `inspect(AuditBinding&)` conserve l’émission et
son résultat ; réglages, budgets, rétention et arrêt sont au point de composition.
Les rapports distinguent backlog, confirmation, ancrage et dépassement du délai souple.
Cette revue d’ergonomie du code ne remplace pas une évaluation applicative indépendante.

## Ergonomie du diagnostic borné #118

L’exemple `BoundedDiagnostic.cpp` garde une ligne métier `Log::info` avant/après ; budgets,
flush avec résultat et consultation de santé sont regroupés dans la composition. Les
raccourcis void existants restent utilisables, mais ne permettent pas de déduire une
admission ni un flush réussi ; `tryLog`, `flushChecked` et santé portent cette distinction.
[La migration](../../../docs/migration/bounded-diagnostics.md) rend visibles le refus Fatal,
l’expiration sans annulation et l’obligation de retour des sinks. Revue locale livrée ;
ni essai d’utilisabilité représentatif des opérateurs ni acceptation indépendante présumés.
