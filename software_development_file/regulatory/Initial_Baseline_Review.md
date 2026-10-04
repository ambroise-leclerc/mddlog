# Revue de préparation de la base initiale — #124

## Identification et portée

- Date de l'examen : 2026-10-04.
- Révision du dossier et du logiciel examinée : `7f0477a8e552e8eb44520ddc7372a6457e04995a`,
  fusion de [la PR #125](https://github.com/ambroise-leclerc/mddlog/pull/125) dans develop.
- Référence fonctionnelle : v0.2.0 ; aucune différence dans `include/`, `CMakeLists.txt` et
  `tests/spec/` entre cette référence et la révision examinée.
- Auteur du rapport : historique Git ; responsable de la décision : Ambroise Leclerc.
- Statut : **examen préparatoire terminé ; décision d'acceptation non enregistrée**.

Ce rapport examine l'aptitude du dossier à servir de base au programme 1.0 selon
[#124](https://github.com/ambroise-leclerc/mddlog/issues/124). Il ne prononce aucune conformité
normative, qualification de support, acceptabilité de risque de dispositif ou acceptation des
preuves finales d'ADR-004. La décision faisant autorité reste le champ `review` du
[registre](../register.json) ; aucun état de GAP n'est modifié par ce rapport.

## Résultats de l'examen documentaire

| Critère de #124 | Constat sur la révision examinée | Limite de la conclusion |
| --- | --- | --- |
| A — Périmètre et applicabilité | [Applicability](Applicability.md) sépare composant/fabricant, cinq références et éditions, usages visés/exclus et responsables ; absence de classification automatique. | Les fiches éditeurs ne vérifient pas les clauses. GAP-002 demeure ouvert. |
| A — Maîtrise et traçabilité | [Index](../README.md), [plans](Plans.md) et registre relient exigence, risque éventuel, contrôle, conception/source, test, note de preuves et lacune. | La décision nominative manque encore ; GAP-001 et GAP-019 restent ouverts. |
| B — Documents renseignés et modèles | Architecture, conception, SOUP, risques, cybersécurité, qualité, aptitude à l'utilisation et plans sont renseignés ; les modèles correspondants sont séparés. | L'application et ses données cliniques, risques, supports et budgets restent propres au fabricant. |
| B — Architecture effective | [SAD](IEC_62304/SAD.md) et [SDD](IEC_62304/SDD.md) correspondent au FILE_SET et aux contrats examinés dans les sources. | Les primitives d'ADR-004 sont présentes ; aucun support ou fournisseur indépendant réel n'est livré. |
| B — Risques et dépendances | Le registre distingue contrôles existants, absence de contrôle, limites hôte et lacunes assignées ; [SOUP](IEC_62304/SOUP.md) sépare déploiement, tests et outils. | Les licences/runtime exacts et le classement du profil restent à confirmer ; la surveillance est un plan. |
| C — Simplicité et source unique | Les exigences d'appels courts, contexte local, composition, orchestration et résultat explicite sont présentes ; aucun identifiant documentaire supplémentaire n'est imposé aux appels C++. | Ces exigences de conception restent prévues ; elles ne démontrent pas encore l'ergonomie de la future API. |
| D — Vérification et états honnêtes | Les contrôles documentaires passent ; les rapports distinguent couverture synthétique et qualification réelle, prévu et implémenté. | Les tests ne prennent aucune décision de fond. L'acceptation initiale et les preuves de publication sont distinctes. |

Les responsabilités et règles de mise à jour à la clôture de #113–#122 figurent dans les plans
et CONTRIBUTING. Les modèles renvoient à une section existante du README du dossier. Les sources
et la licence de la structure MduX sont indiquées dans l'index ; aucune API MduX ou référence
de clause non confirmée n'est reprise comme mécanisme livré par mddlog.

## Contrats et couverture examinés

L'examen des sources et des tests cités par les CTRL/VER a confirmé les distinctions suivantes :

- CTRL-001/VER-001 portent sur la frontière cœur/adaptateurs ; ils ne sont plus présentés comme
  maîtrise de RISK-011, qui conserve ses lacunes de chaîne de construction.
- CTRL-002 et les sources de champs possédés définissent refus et troncature ; REQ-002 garde
  une qualification du profil ouverte dans GAP-018. VER-002 est une couverture existante,
  pas une validation de toutes les données d'intégration.
- `AuditEvent::assign` permet les actions réservées pour le ledger ; `AuditRing::tryRecord`
  les refuse à l'admission producteur. Les phases exposées sont Requested, Confirmed,
  Executed et Failed. Admission, transmission et confirmation durable sont distinctes.
- CTRL-005 à CTRL-008 séparent octets canoniques, chaînage, ancrage, sync, ledger et lecture.
  La mémoire du lecteur reste allouante. La position retenue et le fournisseur de test
  ne démontrent pas un stockage indépendant persistant.
- CTRL-009/VER-009 couvrent le SPSC diagnostique exercé, sans preuve exhaustive des interleavings
  ni qualification de concurrence du sink audit.
- CTRL-010/VER-010 couvrent le transport borné de REQ-015. REQ-012 n'en déduit aucun budget
  pour la file SimpleLogger ou la lecture d'archives.
- RISK-009 et REQ-016 renvoient à GAP-017 : minimisation, accès, confidentialité et rétention
  du profil doivent être qualifiés par le fabricant. Aucun contrôle hôte n'est tenu pour acquis.

Les [preuves de frontière](../../docs/governed-evidence.md),
[de scénarios](../../docs/audit-scenario-validation.md) et
[de persistance](../../docs/audit-persistence-validation.md) restent les sources de couverture.
Ce rapport ne crée pas un deuxième inventaire d'exigences, de risques ou de tests.

## Vérifications exécutées pour cet examen

Sur la révision examinée, le 2026-10-04 :

| Vérification | Profil et commande | Résultat |
| --- | --- | --- |
| Cohérence documentaire | Python 3.14.6 ; `python3 scripts/check-development-file.py` | Réussite |
| Tests du contrôleur | `python3 -m unittest discover -s tests/documentation -p 'Test*.py'` | 30 tests réussis |
| Configuration | Linux x86_64, Clang 21.1.8/libc++, CMake 4.2.3, Ninja 1.13.2 ; `cmake --preset ninja-clang` | Réussite |
| Compilation | Release, `-stdlib=libc++`, exemples/tests ON, sanitizers et analyseurs OFF, cache ON ; `cmake --build --preset ninja-clang --parallel 4` | Réussite |
| CTest | `ctest --preset ninja-clang --output-on-failure` | 155/155 réussis, dont consommateurs source/installés et cinq contrôles governed |

Les journaux de cette campagne locale sont des sorties de construction non versionnées ;
leur conservation pérenne et le profil complet des futures campagnes restent GAP-003.
Un résultat local ne représente pas toutes les plateformes. La
[CI documentaire de la révision fusionnée](https://github.com/ambroise-leclerc/mddlog/actions/runs/37215863582)
a également terminé avec succès. Ce résultat CI ne remplace pas une revue du fond.

## Revue, identité et indépendance

Les commits de la PR #125 sont attribués à Ambroise Leclerc. GitHub conserve une
[approbation de cette PR](https://github.com/ambroise-leclerc/mddlog/pull/125#pullrequestreview-5407025878)
par le compte `AM-L` le 2026-10-04 à 15:58:26 UTC, sur `72daebb4d6b6469f7e1ea793dfa624c9d1b426d0`.
Cette approbation ne contient pas de décision textuelle d'acceptation du dossier de #124.
L'identité du relecteur, sa désignation pour cette acceptation et son indépendance vis-à-vis
de l'auteur ne sont pas établies par un pseudonyme GitHub.

Une décision prise par Ambroise Leclerc en tant qu'auteur et approbateur constitue l'auto-revue
non indépendante déjà décrite dans l'index. GAP-019 doit être levé en consignant cette limite
et les cumuls de rôles dans la décision, sans attribuer une certification ou une satisfaction
d'exigence normative au processus. Les revues que requiert le fabricant restent sa responsabilité.

## Proposition de traitement avant acceptation

Aucune nouvelle discordance de contrat n'a été relevée dans les éléments examinés qui
empêcherait l'utilisation du dossier comme base initiale. La recommandation est de soumettre
cette base à une acceptation de projet limitée, avec les dispositions suivantes :

1. Consigner une décision nominative portant sur le SHA examiné, le présent rapport et la limite
   d'indépendance ; elle permettrait de clore GAP-001 et GAP-019. La fusion de #125 ne suffit pas.
2. Traiter GAP-002 comme une limite normative explicite de cette base, conformément à #124 qui
   autorise de signaler les références à confirmer lorsque le texte n'est pas disponible.
   Aucun numéro de clause n'est revendiqué. Proposer de transférer son examen restant à #122
   et de le maintenir ouvert ; ne pas prétendre que des copies autorisées ont été examinées.
3. Retirer GAP-002 à GAP-005 des réserves bloquant la **base initiale**, avec décision motivée,
   tout en conservant leur état ouvert. Les porter dans `review.acceptedGaps` : absence
   d'examen normatif complet pour GAP-002, conservation et campagnes futures pour GAP-003,
   processus de réponse à établir avant publication pour GAP-004, inventaire et qualification
   du profil à finaliser pour GAP-005. Les obligations de la 1.0 restent inchangées.
4. Pour les autres GAP ouverts liés aux exigences implémentées, enregistrer des justifications
   dans `review.acceptedGaps` : qualification du support (GAP-007), indépendance/position retenue
   (GAP-008), orchestration (GAP-009), outil de lecture/export (GAP-012), robustesse/concurrence
   (GAP-013), stabilité/distribution (GAP-014), preuve applicative et acceptation finale (GAP-015),
   absence de preuve d'auteur avec signature différée (GAP-016), qualification des champs (GAP-018).
   Leur ouverture indique que seuls les mécanismes et la couverture cités sont reconnus, sans
   accepter les capacités futures ni une intégration clinique.
5. Garder ouverts les travaux de conception des exigences prévues et leurs GAP ; après
   acceptation initiale enregistrée et livrée, commencer la conception de #113. #122 conserve
   sa porte d'acceptation finale avant publication.

Cette proposition ne modifie ni `review`, ni les réserves, ni les issues. Sa mise en œuvre
requiert la décision du mainteneur sur les reports et les limites décrits ci-dessus.
