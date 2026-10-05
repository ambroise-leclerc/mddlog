# Résultats complémentaires de stockage et robustesse — #114 / #120

Campagne locale du 5 octobre 2026, workspace fondé sur `f3df383` avec les modifications
de ce lot. La PR #136 a été fusionnée à `4757e6e` ; son contenu documentaire est présent
dans cette base locale. Références : [plan de robustesse](audit-robustness-test-plan.md),
[protocole fichiers](file-storage-test-plan.md) et [résultat électrique Orin Nano](orin-nano-electrical-results.md).
La revue des compléments et la traçabilité du profil physique restent ouvertes.

## Résultats exécutés

Profil logiciel : Linux x86_64 7.0.0-34-generic, Clang/libc++ 21.1.8, GCC/libstdc++ 16.1,
CMake 4.2.3 et Python 3.14.6. Builds Release des deux compilateurs, Debug Clang ASan/UBSan
et Debug GCC TSan ; SpecLab reste figé à `d85c1f74d95a25b2bf2733316149ba7936ed7df6`.

| Vérification | Résultat |
| --- | --- |
| Modèle d’admission | 32 seeds × 2 000 décisions, file abstraite, refus sans consommation d’identité |
| Reprise générative | 64 seeds, trois opérations et trois positions de coupure, quatre politiques de suffixe, réponse du témoin perdue ; préfixes confirmés comparés aux copies indépendantes |
| Audit concurrent | Trois producteurs SPSC, un consommateur et un observateur ; 1 200 événements vérifiés, rejets et exceptions réessayés |
| Répétitions des scénarios ciblés | Quatre scénarios × 100 passages réussis ; le test d’arrêt exerce 2 000 cycles par passage |
| Stockage à 100 répétitions | 2 609 PASS, zéro FAIL, un SKIP pour le volume ; statut global PARTIAL |
| SHA-256 indépendant | 268 entrées concordent avec Python hashlib ; limites du padding et des blocs, jusqu’à 64 Kio |
| Fuzzing Clang ASan/UBSan | 100 000 entrées, seed 120, aucun crash/UB/timeout ; RSS maximal 262 Mio pour un budget de 512 Mio |
| TSan GCC | 26/26 scénarios RingLog, AuditRing, observateurs et SinkRegistry réussis, sans fichier de suppression |
| Contrôles documentaires Python | 42 tests, dont quatre contrôles négatifs du superviseur de robustesse |
| CTest Release final | 188/188 Clang, 187/187 GCC, consommateurs source/installé inclus |
| CTest Debug ASan/UBSan final | 182/182 réussis au rejeu complet ; consommateurs installés exclus |
| Style et analyse statique | 90 fichiers conformes à clang-format ; six unités C++ modifiées sans diagnostic clang-tidy restant |

Les journaux distinguent les passages initiaux, les corrections et les rejeux.

Le SKIP du volume est dû au refus des namespaces dans l’environnement local. Les essais
ENOSPC réels de la campagne précédente restent identifiés dans son rapport et sa CI ;
ce rejeu à 100 passages ne revendique pas leur nouvelle exécution. La CI longue ajoutée
impose une exécution distincte du volume plein en montage root privé.

LSan ne peut pas fonctionner sous le ptrace de l’environnement local : le premier essai
du probe instrumenté a échoué avec ce diagnostic, malgré 268 digests produits. Le fuzzing
réussi utilise explicitement `--disable-leak-detection` ; ASan et UBSan restent actifs.
La CI conserve LSan activé ; aucun résultat nouveau de cette CI n’est revendiqué ici.

## Couverture mesurée du fuzzing

Rapport LLVM du rejeu à 100 000 entrées, incluant l’instrumentation des corps inline
importés dans le harness. Les six lecteurs principaux possèdent une couverture exécutée.

| Unité | Branches couvertes |
| --- | --- |
| AuditCanonical | 76,88 % |
| AuditLayout | 88,46 % |
| AuditLedger | 38,95 % |
| AuditLog | 28,31 % |
| AuditLogVerifier | 25,84 % |
| AuditStore | 6,88 % |
| SHA-256 | 100 % |

Ces chiffres mesurent le harness des lecteurs, pas l’union avec les scénarios fonctionnels.
Les branches d’écriture, de rétention, de fournisseur et les ordres complexes de registres
ne sont pas entièrement couvertes par ce corpus. Les scénarios SpecLab les exercent séparément.
Aucune couverture exhaustive ni probabilité universelle de défaut n’est déduite de ce passage.

## Anomalies et corrections

L’examen du blocage initial du sink asynchrone a identifié une course possible dans
`SimpleLogger::shutdown()` : modification du prédicat sans le mutex de la condition,
avec notification perdue entre le test du prédicat et l’entrée en attente. La modification
utilise ce mutex et les 100 rejeux des deux scénarios concernés passent. Cela établit la
correction de la course identifiée ; sans trace de pile initiale, son lien avec l’ancien
blocage n’est pas affirmé comme une reproduction déterministe de sa cause.

Le premier rejeu GCC avec un délai global de 30 secondes a dépassé ce délai dans
`InstallTreeConsumer`. Son rejeu isolé à 120 secondes passe en environ 48 secondes.
Les journaux conservent le passage initial et le rejeu, sans transformer le timeout en succès.

Le premier passage ASan/UBSan comptait 180/182 réussites : le build de fuzzing propageait
ses options de lien au cœur seul et les contrôles de graphe et leurs contrôles négatifs
l’ont refusé. Les options appartiennent désormais à l’interface de l’adaptateur ; les
182 tests passent au rejeu complet après correction. Le SHA-256
du fuzzer reste identique à celui de la campagne de 100 000 entrées après ce déplacement
des options, donc les preuves de cette campagne correspondent au binaire final.

## Preuves et statut des issues

### Revue technique d’usage du backend

L’exemple `FileAudit.cpp` centralise répertoire, limites, durée de vie du support et
configuration du sink. `emit()` reçoit seulement un `AuditBinding` et conserve l’appel
d’admission `record()` ; il n’ouvre pas de fichier. Description et contexte sont créés
avant l’émission, la phase est `Requested` et l’heure indisponible reste explicitement
indisponible. Le consommateur réalise drainage et flush hors de ce point d’émission.
La fermeture du rédacteur précède la réouverture en lecture seule.

Les binaires Clang et GCC de l’exemple ont été exécutés : argument manquant refusé,
un événement écrit puis relu avec chaînage vérifié, répertoire non vide refusé lors
d’un second lancement. La position durable annoncée reste zéro en `Unqualified` ;
la lecture n’est pas présentée comme un ancrage indépendant. Le refus d’admission
interrompt cette démonstration via son résultat booléen ; une application doit exploiter
la raison structurée et appliquer sa propre politique de refus.

Cette revue technique couvre la composition autonome disponible. L’acceptation du
mainteneur et la revue du futur pilotage de #116 restent distinctes ; son cycle de vie,
le redémarrage applicatif et les décisions sur les refus ne sont pas qualifiés ici.

### Conservation des preuves

Les artefacts locaux restent ignorés par Git :

- `build-clang/storage-campaign-100/run-nkd_43r9/report.json` : 100 répétitions et SKIP explicite.
- `build-robustness-fuzz/campaign/run-n3hacx65/report.json` : fuzzing, hashes des workers/seeds,
  oracle SHA-256, corpus final, logs, profils LLVM et couverture.
- `build-clang/verification-audit-robustness/` : journaux `mddlog-robustness-*`, patch
  du lot et manifeste de révision, des sources modifiées et des binaires vérifiés.

Les métadonnées du build ont été ajoutées au rapport du fuzzing après l’exécution,
depuis le cache du build correspondant ; le hash du binaire a été contrôlé avant cet ajout.

Le workflow `Audit Robustness` ajoute campagnes courtes/longues, collecte de couverture
et archivage ; le workflow TSan élargit sa sélection à AuditRing et aux observateurs.
Ces modifications de CI sont configurées et doivent être exécutées à la révision soumise.

| Issue | Travaux démontrés par ce lot | Éléments restant nécessaires à la clôture actuelle |
| --- | --- | --- |
| #114 | Conformité, erreurs réelles précédentes, reprises SIGKILL à 100 passages ; succès électrique déclaré sur Orin Nano ; revue technique de l’exemple | Paramètres et preuves du profil électrique, acceptation des revues du profil et de l’ergonomie, raccordement au pilotage de #116 |
| #120 | Modèles génératifs, fuzzing/corpus, oracle indépendant, concurrence/TSan, campagnes longues et rapports | Revue des preuves, CI effective du lot, témoin réel et position retenue de #115, campagnes du pilotage de #116 et archives interversions |

Le maintien ou le transfert explicite des critères d’intégration vers #115/#116/#121
reste à décider. Aucun critère n’est supprimé par la réussite des tests logiciels.
GAP-003, GAP-007 et GAP-013 restent ouverts jusqu’aux décisions et preuves correspondantes.
