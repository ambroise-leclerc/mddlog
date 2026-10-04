# Étude d'usage contextualisé — #113/#127

Date : 2026-10-04. Statut : **conception acceptée**, décision du 2026-10-04 sur `f3063bc`.
Base : `develop` à `67a15e8`, après acceptation de #124 ; évolution sur
`113-contextual-api-design`. La révision exacte des fichiers étudiés est celle du commit
contenant ce rapport. Aucune campagne de qualification du profil 1.0 n'est revendiquée.

## Découpage retenu

#113 est réalisable mais ne constitue pas un lot unique de livraison : elle comporte une
acceptation de conception préalable, deux chemins d'implémentation aux contraintes différentes
et une revue d'intégration finale. Quatre sous-issues GitHub sont créées et rattachées à l'épique :
[#127](https://github.com/ambroise-leclerc/mddlog/issues/127) conception,
[#128](https://github.com/ambroise-leclerc/mddlog/issues/128) diagnostic,
[#129](https://github.com/ambroise-leclerc/mddlog/issues/129) audit,
[#130](https://github.com/ambroise-leclerc/mddlog/issues/130) intégration et preuves.
Les lots diagnostic et audit peuvent démarrer après acceptation de #127 ; #130 les réunit.
Pas de sous-issue par méthode ou par champ : chacun conserve tests, migration et dossier.

Ce lot prépare le jalon A : inventaire, capture, filtrage, propriété, erreurs et alternatives
dans [ADR-005](adr/ADR-005-contextual-logging-api.md), plus usages compilables.
La conception est acceptée à la révision `f3063bc4122cad41f23352721824db9094066b3d` ;
la livraison sur develop et les lots d'implémentation restent distincts. #113 et GAP-006 restent ouverts.

## Démonstrateur et comparaison

[ContextualUsage.cpp](../examples/ContextualUsage.cpp) est un démonstrateur de composant
`pump` : il ne pilote pas un dispositif réel et ne remplace pas la revue applicative #130/#122.
Ses petites classes locales `study` représentent les formes proposées ; elles ne sont pas
exportées, installées ni incluses dans `FILE_SET cxx_modules`. Les modules publics restent
ceux de la v0.2. Tous les paramètres de destination/contexte sont préparés dans `main` ;
les fonctions avant/après contiennent seulement l'instrumentation et la politique d'action.

Décompte manuel sur le code formaté : lignes non vides à l'intérieur des fonctions métier,
hors signature/accolades ; pour l'audit, distinguer assemblage d'une émission et politique de
l'action complète. Les helpers de prototype et la composition ne sont pas des lignes métier.

| Usage et fonctions | Lignes métier avant → après | Répétitions d'invariants avant → après |
| --- | --- | --- |
| Composant, `componentBefore`/`componentAfter` | 2 → 2 | 2 préfixes de composant et 2 captures explicites de source → 0 |
| Opération, `operationBefore`/`operationAfter` | 2 → 2 | 2 préfixes composant/opération/corrélation et 2 captures explicites de source → 0 |
| Producteur gouverné, `governedBefore`/`governedAfter` | 1 → 1 | 1 champ composant et 1 capture explicite de source → 0 ; temps et résultat conservés |
| Émission audit, `auditWriteBefore`/`AuditBinding::record` à l'appelant | 7 lignes d'agrégat → 1 appel | 5 champs invariants (catégorie/action/acteur/cible/corrélation) → 0 ; phase et temps conservés |
| Action auditée, `auditBefore`/`auditAfter` | 6 → 6, plus l'assemblage ci-dessus avant | 2 assemblages des mêmes invariants par action → 0 ; décision de refus et issue restent visibles |
| Plusieurs producteurs, appels dans `main` | 1 émission par producteur → 1 | Contexte par appel → liaison injectée propre à chaque producteur ; aucun anneau partagé |

Le bénéfice est la disparition des répétitions, pas la suppression de la politique d'admission.
Une origine capturée par `auditWriteBefore` n'est pas prétendue : l'audit n'a pas ce champ.
Les fonctions diagnostic conservent leur véritable localisation via le paramètre par défaut
sur la méthode de liaison ; l'exécutable vérifie précisément le numéro de ligne d'un appel.

Objets sémantiques minimaux à gérer à la composition, hors résultats transitoires de factory,
politique, horloge et consommateurs inchangés :

| Chemin | Avant | Après proposé |
| --- | --- | --- |
| Diagnostic composant | 1 logger | 1 logger + 1 contexte + 1 liaison = 3 |
| Opération dérivée | 1 logger | 1 logger + contexte parent + contexte dérivé + liaison = 4 |
| Diagnostic gouverné | 1 anneau | 1 anneau + contexte + liaison = 3 |
| Audit | 1 anneau et champs par émission | 1 anneau + description + options de contexte + liaison = 4 |
| Deux producteurs gouvernés | 2 anneaux | 2 anneaux + 2 contextes + 2 liaisons = 6 |

Le démonstrateur garde volontairement avant/après et résultats de factories en parallèle :
son `main` contient donc davantage d'objets que ces compositions minimales. La revue doit
juger cette hausse au point de composition contre la baisse des répétitions métier.
Une factory finale pourra fusionner valeur intermédiaire et liaison, après acceptation.

## Contrats exercés

L'exécutable échoue par exception si une vérification échoue, y compris en `Release` :
aucun `assert` supprimé par `NDEBUG`. Le test CTest `examples.contextualUsage` est enregistré
si tests et exemples sont activés. Il vérifie :

- les cinq usages ci-dessus, les préfixes de contexte et leur indépendance ;
- l'identifiant temporaire de corrélation capturé, le message temporaire possédé et la vraie ligne d'appel ;
- le refus d'un composant trop long et la troncature de message restant visible ;
- la fabrique non évaluée au niveau désactivé, évaluée une fois au niveau activé ;
- aucune publication à la construction/destruction de la liaison audit ;
- les phases explicites, une issue de succès et une issue d'échec obtenues par callbacks de l'hôte ;
- le refus de demande bloquant le callback et le refus d'issue préservant l'information d'une action exécutée ;
- le motif `RingFull` et le refus du vocabulaire réservé `mddlog.`.

La politique d'action est un helper **de l'hôte** dans le démonstrateur. Elle expose séparément
le résultat d'admission et le fait/issue de l'action. Elle ne constitue pas une nouvelle façade
mddlog ni une garantie de durabilité. Une exception du callback n'émet aucune phase implicite.

## Vérification locale

Profil : Linux x86_64, Clang/libc++ 21.1.8, CMake 4.2.3, Ninja, preset `ninja-clang`
`Release`, exemples/tests activés, SpecLab épinglé par le CMake existant.

```sh
cmake --preset ninja-clang
cmake --build --preset ninja-clang --parallel 4
ctest --preset ninja-clang --parallel 4
ctest --preset ninja-clang -R examples.contextualUsage --output-on-failure
scripts/check-format.sh
JOBS=4 scripts/run-clang-tidy.sh build-clang
python3 scripts/check-development-file.py
python3 -m unittest discover -s tests/documentation -p 'Test*.py'
```

Configuration **et construction effectives** réussies. Première campagne complète :
156/156 tests passent, incluant `SourceTreeCoreConsumer`, `InstallTreeCoreConsumer` et
`InstallTreeConsumer`. Après renforcement du seul exemple d'action auditée, reconstruction
réussie et test ciblé `examples.contextualUsage` réussi. Une seconde campagne complète
passe également 156/156 ; les dernières corrections de style du seul prototype sont ensuite
reconstruites et vérifiées par son test ciblé.

Contrôles finaux : formatage réussi sur 68 fichiers avec clang-format 21 ; analyse complète
clang-tidy 21 réussie sur 62/62 unités, dont les 30 interfaces `.cppm`, zéro unité en échec
et zéro occurrence `clang-diagnostic-error` dans le rapport. L'analyse ciblée du prototype
corrigé réussit également. Contrôle du dossier réussi et 30/30 tests documentaires réussis.
La fixture documentaire inclut les deux nouvelles références pour vérifier les liens dans
son dépôt temporaire. `git diff --check` réussit. Aucune acceptation mainteneur n'est déduite
de ces résultats.

## Réserves pour la revue

- Le diagnostic final doit rendre l'erreur de création typée ; le prototype utilise `optional`.
- La description et le contexte d'audit doivent devenir des options distinctes ; le prototype
  utilise `AuditInput` et ne retient que ses identifiants/catégorie invariants.
- La possession par enregistrements complets facilite l'étude, mais ne fixe pas le budget des
  futures liaisons. Aucun coût temporel ni nouvelle taille limite n'est accepté ici.
- Les anneaux empruntés exigent une durée de vie supérieure à la liaison et un unique producteur.
  Les constructeurs d'anneau rvalue sont refusés ; une référence ne prouve pas toute la durée de vie.
- Le prototype n'établit pas les tests de concurrence, de contextes profondément imbriqués,
  de toutes les erreurs d'identifiant ou de troncature UTF-8 de la future API. #128/#129/#130
  les livrent ; les tests existants continuent de couvrir les primitives de la v0.2.
- `SimpleLogger` et `Log` sont inventoriés, mais l'essai allouant repose sur `TextLogger`.
  Le filtrage paresseux de `SimpleLogger` et la projection vers ses champs historiques doivent
  être précisés en #128 ; aucune perte silencieuse de corrélation n'est acceptable.
- La qualification d'un composant réel, les budgets et le gel final restent dus à leurs lots.

## Décision attendue

Revoir l'injection, les copies possédées, le temps par événement, les deux canaux, la politique
de filtre concurrent et les cinq exemples. Consigner acceptation/réserves, mainteneur, date
et révision dans `Approval` d'ADR-005. La contrainte vient de #113, jalon A :
« Faire accepter une décision de conception et des exemples compilables avant de figer les signatures. »
Le présent lot rend cette décision concrète ; les nouvelles signatures publiques attendent cette revue.
