# Composants tiers et outils

Maîtrise et statut : [index](../../README.md). Inventaire unique : section `dependencies` du
[registre](../../register.json). Chaque DEP indique portée, provenance, contrainte/version,
licence, anomalies et surveillance ; les versions exactes de campagne restent dans ses preuves.

La bibliothèque n'importe pas MduX ni SpecLab à l'exécution. Le standard C++, la bibliothèque
standard et le runtime de threads sont néanmoins des éléments déployés à évaluer dans le
profil de l'hôte ; « sans dépendance externe » ne les retire pas de l'analyse. Leur classement
SOUP est une proposition à confirmer selon le dossier de développement disponible et la
classification système du fabricant (GAP-005), pas une conséquence automatique de leur licence.

SpecLab est réservé aux tests ; CPM est un outil de construction dont une copie est intégrée au dépôt. CMake, Ninja,
compilateurs, analyseurs, Python, Git et runners CI influencent la construction ou les preuves
sans être liés au composant. Le premier backend Linux de #114 ajoute la pile système DEP-009 (appels Linux et libc) ; matériel, système de fichiers et caches restent à qualifier. Le fournisseur réel #115 reste prévu.
Les versions supportées et exclusions restent gouvernées par CMake ; en particulier GCC 16.2
est exclu pour corruption des BMI, sans généraliser une garantie aux versions ultérieures.

Les sources de surveillance propres à chaque DEP et leur cadence figurent dans le registre.
Elles définissent un plan ; aucun examen complet des avis ni suivi opérationnel déjà réalisé
n’est revendiqué. Les résultats et anomalies de chaque examen restent à conserver.

Avant une mise à jour : vérifier licence et provenance, examiner défauts et avis de sécurité,
reconstruire modules et consommateurs, exécuter les vérifications impactées et faire accepter
le changement. Les rôles et la conservation sont définis dans [les plans](../Plans.md).
