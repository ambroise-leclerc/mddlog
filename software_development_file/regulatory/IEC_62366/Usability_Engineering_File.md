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
