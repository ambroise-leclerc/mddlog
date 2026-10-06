# Revue de la campagne électrique Orin Nano — #114

Revue documentaire du 6 octobre 2026, sur le dossier `orin-nano-campagne` fourni par
le mainteneur. Celui-ci confirme explicitement qu’il s’agit de la campagne réelle et
demande de retirer les mentions héritées du template. Cette confirmation est la
provenance enregistrée ; aucun essai électrique n’a été réexécuté pendant cette revue.

**Bilan : les décomptes et les renvois internes sont cohérents. La qualification du
profil reste ouverte pour les réserves de traçabilité et de preuves détaillées ci-dessous.**
Les résultats du banc sont publiés avec leur portée ; la réussite déclarée ne devient
pas une vérification indépendante des images et traces absentes de la livraison.

## Livraison et mise en forme

Le [rapport de campagne](validation/orin-nano/ORIN-PWR-EX-001/rapport.md), le
[profil](validation/orin-nano/ORIN-PWR-EX-001/profil.json), les
[cycles](validation/orin-nano/ORIN-PWR-EX-001/cycles.csv), les événements JSONL et
l’[index des images](validation/orin-nano/ORIN-PWR-EX-001/images/index.csv) sont versionnés.
L’identifiant fourni `ORIN-PWR-EX-001` est conservé pour la traçabilité.

Les mentions de démonstration sont retirées sur instruction du mainteneur. Le rapport
est harmonisé sur `profil.json` pour l’opérateur A. Moreau et le relecteur A. Leclerc ;
les anciens noms du rapport ne sont pas retenus. Les chemins documentaires sont corrigés,
les références aux traces portent la mention de conservation hors dépôt et la médiane
du délai est donnée à 27,95 ms. L’attribution de 30 s au vidage périodique XFS est retirée comme justification de
couverture faute de relevé de configuration. La trace décimée sans identifiant de cycle est omise ;
elle ne sert pas de preuve électrique. Le nom NTP de démonstration est remplacé par le
constat qu’il n’est pas documenté, sans changer les valeurs de synchronisation.

Les seeds, horodatages, mesures, observations, verdicts et empreintes des images/données
sont conservés. Les incohérences techniques restent dans les observations originales
et sont décrites ci-dessous ; elles ne sont pas corrigées en inventant des mesures.
Les images brutes et traces électriques par cycle restent hors Git, conformément à la
décision du mainteneur. Leur absence du dépôt est prévue ; leur rattachement à une
archive vérifiable reste à établir.

La livraison originale contient 96 fichiers, soit 5 913 413 octets. Ses empreintes et
les contrôles sont consignés dans la [fiche de revue](validation/orin-nano/input-review.json).
Une copie originale a été conservée localement avant édition. Le manifeste publié
[SHA256SUMS](validation/orin-nano/ORIN-PWR-EX-001/SHA256SUMS) identifie la livraison mise
en forme ; il ne remplace pas une empreinte d’acquisition historique.

## Contrôles exécutés

| Contrôle documentaire | Résultat |
| --- | --- |
| Manifeste initial | 95 entrées ; 94 concordantes, divergence pour `profil.json` |
| Inventaire initial | Aucun fichier omis du manifeste, hormis le manifeste lui-même |
| Cycles | 2 913 identifiants uniques, numérotés de 1 à 2 913 |
| Verdicts du banc | 2 910 `PASS`, trois `INVALIDE` : 766, 1 742 et 2 214 |
| Renvois CSV → JSONL | 8 739 renvois contrôlés ; chaque événement porte le cycle attendu |
| Couverture des événements | Aucun événement JSONL orphelin ni double référencement |
| Points de mutation FS-05 | 25 points, chacun avec 100 cycles marqués `PASS` |
| Séries supplémentaires | 300 stream, 80 capacité et 30 lecture, tous marqués `PASS` |
| Inventaire des images | 2 913 entrées correspondant aux identifiants et noms du CSV |
| Reprise avec `Inventory` | 436 tentatives, dont 435 retenues ; résidus explicitement signalés |
| Rejeu XFS déclaré | 1 655 tentatives ; chiffre concordant avec le rapport |

Ces contrôles valident la cohérence du dossier reçu. Les verdicts `PASS` de ce tableau
sont ceux du banc : les octets des images et les traces électriques externes n’ont pas
été comparés à leurs empreintes pendant cette revue.

## Réserves techniques et suites attendues

| ID | Constat vérifiable dans la livraison | Suite nécessaire |
| --- | --- | --- |
| RN-01 | `profil.json` diverge du manifeste initial. Les identités du rapport et du profil divergeaient également. | La publication harmonise les identités sur le profil et recalcule son propre manifeste. Conserver l’historique de modification du profil ; le manifeste initial ne scelle pas cette version. |
| RN-02 | Les essais sont datés du 14 au 21 septembre 2026 ; le commit annoncé `f3df38328e141f5b85eebd07062f72d9e3086872` est daté du 5 octobre. Le dossier annonce pourtant un arbre sans modification. Le worker de ce commit produit des préfixes fixes ; les 2 533 événements d’oracle détaillés donnent des empreintes du segment 1 différentes des deux jeux de données possibles de ce worker. Les boucles stream/capacité décrites ne sont pas des opérations de ce worker. | Identifier la révision et les sources effectivement exécutées, le binaire associé, les commandes/options et tout diff ou contrôleur externe. Expliquer la relation entre cette exécution et le commit publié. Cette divergence ne permet pas actuellement de rattacher la preuve matérielle au logiciel revendiqué. |
| RN-03 | Les 2 913 images et 2 910 traces électriques référencées par les cycles retenus ne figurent pas dans la livraison. Le profil cite `acq-01` et une archive de qualification sans emplacement durable exact. | Renseigner le catalogue ou emplacement externe, le responsable et l’accès de revue. Vérifier les empreintes des images et les traces de coupure dans cette archive. Les fichiers volumineux restent hors Git. |
| RN-04 | L’oracle détaillé contient longueurs et empreintes, mais pas les octets attendus ni leur procédure de génération. La reprise fournit des libellés de comparaison sans empreintes observées. Les séries stream/capacité renvoient à des manifestes d’oracle non fournis. | Rattacher les données attendues ou leur générateur déterministe, les manifestes et les sorties de comparaison originales. La seed seule ne suffit pas sans algorithme et version. |
| RN-05 | 35 observations du segment 3 sont qualifiées de « préfixe strict » avec une longueur de 4 096 octets, égale aux 4 096 octets attendus. Dans 2 533 événements, le contenu textuel attendu du compteur comporte des caractères `\\n` et mesure 27 octets UTF-8, pour une longueur déclarée de 25. | Vérifier les octets originaux et préciser la représentation utilisée. Corriger les libellés ou l’encodage à partir des preuves, avec historique, sans modifier les mesures par supposition. |
| RN-06 | La série capacité comporte 80 cycles sur les 100 prévus. Les durées hors tension déclarées vont de 4 940,3 à 29 974,2 ms ; 735 cycles retenus sont inférieurs à 5 000 ms. Le rapport cite par ailleurs une durée minimale de 5 s. Le RMS de `clock-sync.txt` vaut 0,000000198 s, tandis que le profil d’acquisition annonce 0,2 ms. | Achever la série capacité ou accepter nominativement l’écart. Distinguer consignes et durées mesurées, définir leur tolérance et préciser la portée des deux mesures de synchronisation. |
| RN-07 | Les résumés de `xfs_repair -n` mentionnent un journal non rejoué pour 1 655 copies. Ils ne constituent pas à eux seuls une vérification structurelle complète. Deux démarrages lents comportent des timeouts NVMe. | Conserver les sorties complètes et justifier la méthode de vérification des copies après rejeu contrôlé, sans modifier la copie brute originale. Examiner les deux observations NVMe pour le profil de disponibilité ; leur `PASS` porte sur les données retrouvées. |

RN-01 est corrigée pour l’intégrité de la livraison éditée, avec conservation du constat
initial. Les autres réserves restent ouvertes ; aucune absence de réponse ne vaut
acceptation d’un écart. Les compteurs `confirmed.bytes` des séries longues doivent aussi
préciser s’ils représentent un cumul historique ou les segments encore présents.

## Portée de l’acceptation

Le [protocole](file-storage-test-plan.md) exige la conservation des préfixes confirmés,+le traitement conforme des états interrompus et la traçabilité du profil réellement
testé. La revue relève une couverture déclarée utile des points de mutation, des
invalidations exclues et des limites publiées. Elle n’établit pas encore la clôture des
réserves de qualification ci-dessus. GAP-007 reste ouvert.

Le raccordement au pilotage de #116 reste un critère distinct de #114. Le worker de
préfixes ne démontre pas la campagne intégrée du journal canonique, des registres,+du témoin indépendant et de la position retenue persistante de #115/#116/#122.
La clôture de #114 reste conditionnée aux preuves et décisions correspondantes.
