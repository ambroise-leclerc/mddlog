# Préparation de clôture du stockage — #114

Base examinée : `0c13e68978048b193bb09ede0464ce1e32f54636`, fusion de #138.
Ce lot raccorde l'exemple au [pilotage de référence](audit-service.md) et fournit un
contrôle des empreintes des images externes. Il ne décide pas la qualification.
Les images brutes et traces restent hors Git, conformément à l'instruction du mainteneur.
La conservation externe et les empreintes versionnées sont compatibles avec la revue.

## Preuves externes sans import des images

Le script [verify-orin-archive.py](../scripts/verify-orin-archive.py) lit l'index publié,
compare taille/empreinte des fichiers compressés puis décompresse en flux pour comparer
les tailles et SHA-256 bruts. Il n'écrit aucune image et refuse les chemins sortant de
l'archive. Utiliser une copie stable en lecture seule. Le rapport JSON doit être écrit
hors de l'archive, dans un nouveau fichier ; il identifie l'index par son SHA-256 et l'emplacement effectivement lu.

```sh
python3 scripts/verify-orin-archive.py \
  --index docs/validation/orin-nano/ORIN-PWR-EX-001/images/index.csv \
  --archive /chemin/archive-externe \
  --output /chemin/rapports/orin-images-review.json
```

Le chemin externe doit contenir les références `images/cycle-NNNN.img.zst` de l'index.
Le contrôle complet décompresse les 2 913 partitions en flux : prévoir le temps de lecture.
`--cycle N` permet une revue partielle ; son succès reste `PARTIAL` et ne retourne pas
le code de réussite d'une campagne complète. Une image absente, corrompue, tronquée,
une décompression échouée ou un index ambigu donnent un échec. Un `PASS` porte sur
l'intégrité des images seulement, pas sur l'oracle ni la coupure électrique.

## Dispositions des réserves

| Réserve | Action préparée | Preuve ou décision encore nécessaire |
| --- | --- | --- |
| RN-02 | Contrat de pilotage et exemple exécutables, avec identités explicites. | Sources et binaire réellement employés en septembre, commandes et contrôleur ; explication du commit annoncé d'octobre. Une nouvelle exécution ne réattribue pas l'ancienne. |
| RN-03 | Vérification en flux des empreintes, sans images dans Git. | Emplacement durable, responsable et accès de revue ; exécution contre les images réelles et examen des traces électriques originales. |
| RN-04 | Conservation des références et séparation intégrité/qualification dans le rapport. | Générateur déterministe réellement employé, données ou manifestes attendus et sorties originales de comparaison aux images. |
| RN-05 | Données originales conservées. | Octets originaux du compteur et préfixes pour corriger les libellés et la représentation avec historique. |
| RN-06 | Écarts publiés conservés. | Achèvement ou décision nominative sur les 80/100 cycles capacité, tolérance des durées mesurées et portée des deux mesures de synchronisation. |
| RN-07 | Observations NVMe et méthode XFS conservées. | Sorties complètes, examen après rejeu contrôlé sur copie et analyse des deux timeouts. |
| Exemple / #116 | `FileAudit` emploie `AuditService` pour drain, sync, ancrages et arrêt. | Revue du lot et campagne intégrée selon le périmètre de #116 ; pas de clôture automatique de cette épique. |

La publication des empreintes et des synthèses ne permet pas de prétendre que les images
ou traces ont été examinées. Aucun écart n'est accepté par
absence de réponse. La décision de qualification identifie révision, profil, date,
responsable et réserves levées ou explicitement acceptées ; mettre à jour le registre
et les critères de #114 seulement après cette décision. GAP-007 reste ouvert.

## Résultats de ce lot

Campagne locale du 6 octobre 2026, branche `114-storage-completion`, fondée sur la
révision ci-dessus. Compilation et liens ont effectivement terminé avant chaque CTest.
Les résultats portent sur le diff de ce lot, sans rejeu matériel Orin Nano.

| Contrôle | Résultat exécuté |
| --- | --- |
| Clang 21.1.8 / libc++, Release | Configuration, compilation et liens réussis ; CTest 195/195 |
| GCC 16.1.0, Release | Configuration, compilation et liens réussis ; CTest 194/194 |
| Consommateurs cœur source/installé | Réussis dans les deux campagnes CTest, séparément des tests de stockage |
| Exemple fichiers | Deux démarrages avec relecture, segments historiques inchangés et identité réutilisée refusée, via `examples.fileAudit.lifecycle` |
| GCC 16.1.0, Debug, ThreadSanitizer | Configuration, compilation et liens réussis ; six scénarios du service 6/6, `halt_on_error=1` |
| Dossier documentaire | Structure/références réussies ; 57 tests Python réussis, dont neuf contrôles du vérificateur d'archives |
| Format | clang-format 21 : 92 fichiers conformes |
| clang-tidy 21 | Quatre unités modifiées conformes : AuditDrain, AuditService, FileAudit et AuditServiceSpec ; pas de résultat global local revendiqué |

Le job TSan inclut désormais explicitement le scénario d'observation de santé du
service dans sa sélection CI. Aucun nouveau résultat matériel n'est revendiqué. Les contrôles négatifs
[TestOrinArchive.py](../tests/documentation/TestOrinArchive.py) utilisent des images
miniatures créées pour tester le vérificateur, sans valeur de preuve pour l'Orin Nano.
