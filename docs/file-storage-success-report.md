# Bilan de réussite de la campagne logicielle de stockage

Date : 5 octobre 2026. Références : #114 / #120.

Le lot est réalisé et intégré : le protocole d’essais, le banc automatisé et la
collecte des preuves ont été fusionnés dans `develop` par la
[PR #135](https://github.com/ambroise-leclerc/mddlog/pull/135), révision
`bca737f0de6a7305c9a00a181a30a20e9c792cba`. Les objectifs logiciels de ce lot sont atteints.

Le banc vérifie la reprise après SIGKILL à 26 points observés, les préfixes confirmés,
les références et compteurs, les refus de permissions, le volume plein réel et la
stabilité des descripteurs. Ses contrôles négatifs détectent notamment les fausses
confirmations et la perte d’un préfixe. Il est exécuté par CTest et la CI archive ses preuves.

| Vérification | Résultat |
| --- | --- |
| Campagne locale répétée, dix passages par point | 270/270 cas réussis, aucun échec ni saut |
| Suites CTest locales Clang / GCC | 183/183 au rejeu Clang ; 182/182 GCC |
| Contrôles Python et dossier | 38 tests réussis ; structure et références conformes |
| CI de la PR #135 | Tous les contrôles réussis : Linux Clang/GCC, Windows, macOS, sanitizers, format et analyse statique |

Les [artefacts du run Clang 37339421114](https://github.com/ambroise-leclerc/mddlog/actions/runs/37339421114)
ont été téléchargés et vérifiés. La campagne répétée compte 269 réussites et un saut
explicite pour le volume sans namespace utilisateur ; les trois scénarios ENOSPC réels
réussissent dans l’exécution root isolée obligatoire. Aucun échec n’est masqué.
La tête de PR contrôlée est `e7b3260255106712a130ef68be58e002f5f89c70` ; ces preuves
précèdent la fusion et ne constituent pas un nouveau rejeu sur le commit de fusion.

Le [rapport détaillé](file-storage-campaign-results.md) conserve l’anomalie du premier
passage Clang : un test préexistant du sink asynchrone s’est bloqué, puis le rejeu complet
a réussi. Son examen reste à mener avec #118/#120.

Ce bilan conclut à la réussite du lot logiciel livré. Les campagnes à 100 répétitions,
les coupures électriques sur matériel réel et l’intégration du témoin et du lecteur
indépendants restent à effectuer selon le [protocole](file-storage-test-plan.md).
La qualification du déploiement et GAP-007 restent ouverts.
