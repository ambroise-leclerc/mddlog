# Plans de développement, maintenance et vérification

Maîtrise documentaire et statut : [index](../README.md). Les plans sont regroupés dans ce
fichier car ils partagent responsables, révisions et portes de revue ; les procédures déjà
publiées sont référencées, sans second registre.

## Développement et maintenance

Le programme suit #112 : base de #124 acceptée, conception de #113, choix du profil, chaîne réelle,
robustesse, gel puis publication. Chaque lot commence par exigences et conception, préserve les
ADR-001 à ADR-004 sauf amendement explicite, implémente et établit des preuves utiles. Il met à
jour le [registre](../register.json) et les documents concernés avant revue. La configuration
centralisée et les appels courts sont des exigences de conception ; aucune annotation réglementaire
C++ supplémentaire n'est requise. Le mainteneur accepte les changements selon
[CONTRIBUTING](../../CONTRIBUTING.md), sur branche d'issue vers develop.

## Exigences et conception

Chaque REQ possède un état de réalisation, des risques éventuels, des contrôles et des
vérifications, ou une lacune ouverte attribuée à une épique. Un CTRL renvoie aux décisions et
sources qui le réalisent ; un VER nomme le fichier de test et la note qui explique ses limites.
Les exigences de la v0.2.0 sont reconstruites depuis les contrats existants ; elles ne prouvent
pas qu'un dossier de développement antérieur complet existait. Toute modification d'API,
format ou frontière de confiance entraîne revue de son impact, du guide de migration et des risques.

## Vérification

Les commandes de construction et CTest sont celles de [CMakePresets](../../CMakePresets.json).
Compiler avant CTest ; `SourceTreeCoreConsumer` et `InstallTreeCoreConsumer` sont distincts.
Exécuter format et clang-tidy 21 selon CONTRIBUTING quand du C++ change. Les contrôles documentaires
sont autonomes et tournent également en CI. Les rapports existants sont des inventaires de
couverture, pas de nouveaux résultats de campagne.

Pour chaque campagne, conserver SHA logiciel et dossier, OS/architecture, compilateur/bibliothèque
standard/CMake/Ninja, options, commandes, résultat, journaux et anomalies. En revue, citer la CI
exacte et ses artefacts ; un workflow configuré n'est pas une exécution réussie. GAP-003 prépare
cette collecte pour #120/#122. Les tests synthétiques de persistance ne qualifient pas un
support réel ; les essais de panne électrique, redémarrage et rollback du profil restent dus.
Mesurer mémoire et temps sur le profil choisi, documenter les charges et critères avant campagne.

## Configuration et composants tiers

Git identifie sources, documents et changements. CMake fait foi pour version et modules ; les
révisions des dépendances se lisent dans les fichiers référencés par DEP. Toute mise à jour
comporte provenance, licence, anomalies, impact sur tests et revue. Les sorties de build restent
hors versionnement. En release, conserver les références exactes des outils, OS et dépendances ;
les plages admises ne remplacent pas le profil construit. Le registre version 1 évolue par
changement relu avec migration du contrôleur et de ses tests.

## Résolution des problèmes et sécurité

Une anomalie est suivie dans une issue : reproduction et profil, versions touchées, effets sur
admission/durabilité/lecture, impact risque, contrôle concerné, correction ou justification de
report, vérification et décision du mainteneur. Les GAP du dossier relient les travaux manquants
à leur issue ; ils ne remplacent pas le suivi détaillé. Aucune anomalie résiduelle ne devient
acceptable par silence. Pour une vulnérabilité sensible, éviter de publier données exploitables
ou secrets ; le canal privé et les délais de traitement restent à établir dans GAP-004.

## Acceptation et publication

#124 exige une revue nominative de la base : révision, date, décision et réserves sont consignées
une fois dans `review`. Les réserves restent des GAP ouverts jusqu'à décision explicite.
Les épiques #113–#122 ne sont closes qu'avec exigences, conception, risques, dépendances et
preuves concernés à jour. #121 fixe API, formats et matrice ; #122 accepte les preuves de
persistance et le profil réel, les écarts résiduels et le dossier final. Publication selon
[docs/release-process.md](../../docs/release-process.md), sans tag direct sur master.
