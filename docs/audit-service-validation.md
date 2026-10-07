# Validation du pilotage — #116

Relevé du 2026-10-07 sur le lot `116-audit-pilotage`, issu de
`8ac78610cfa398c0ccd224d36710281f1194662b`. La révision soumise et son diff identifient
le logiciel vérifié ; ce relevé ne modifie pas l’acceptation de la base #124.

## Couverture

- VER-036–040 : contrats conservés, drain/arrêt, flux inactif, support non qualifié,
  pertes et observateurs concurrents.
- VER-045 : budget global, rotation et refus de saturation/enrôlement.
- VER-046 : réconciliation d’une acceptation dont la réponse est perdue, sans rejeu.
- VER-047 : ancrage par âge sous le seuil en nombre ; fenêtre d’exposition publiée.
- VER-048 : appel fournisseur simulé lent, dépassement du délai souple d’arrêt et reprise.
- VER-049 : politique de rétention non déclarée et panne du registre au démarrage.
- VER-050 : callback défaillant isolé, panne persistante d’une instance et recomposition
  explicite sur support réparé, sans effacement du backlog ancien.
- VER-051 : exemple portable avec instrumentation métier séparée de l’exploitation.
- VER-052 : saturation, aucun retrait par défaut et reprise après trim durable
  sous permissions explicites.
- VER-053 : les rejets d’un flux défaillant consomment le budget sans priver l’autre.
- VER-044 : deux anneaux avec budget global, backend fichiers et témoin réel sous UID
  distincts, fermeture confirmée/ancrée, puis reprise et lecteur persistant.

## Résultats exécutés

Clang 21.1.8 / libc++ 21.1.8, CMake 4.2.3, Ninja, Linux : compilation Release avec
exemples, tests et backend fichiers, puis **208/208 tests CTest passés**. La preuve
multi-UID a également été exécutée en mode requis avec :

```sh
unshare --user --map-auto --setuid 0 --setgid 0 \
  python3 scripts/run-witness-deployment.py --require-isolation --outer-uid-start 5 \
  --worker build-clang/tests/mddlog_witness_deployment \
  --service build-clang/examples/mddlog_witness
```

Résultat : `PASS`, processus/UID séparés, deux producteurs, budget global,
protection des magasins, droits, redémarrages, retraite et rollback avec lecteur
persistant. Les messages de refus d’accès sont attendus dans ce scénario.

Clang Debug avec ASan/UBSan : **187/187 scénarios passés**, détection de fuites
activée et arrêt immédiat sur erreur. GCC 16.1.0 / libstdc++, Release :
**207/207 tests CTest passés** (le test de cache propre à Clang n’y est pas inscrit).
TSan, Clang Debug : **27/27 tests concurrents passés**, dont les observateurs du service.
L’analyse statique clang-tidy 21 est bloquante dans la
[CI Clang](../.github/workflows/clang-build.yml) ; les checks de la PR identifient
la révision contrôlée. Aucun résultat CI non exécuté n’est présumé.
Les autres systèmes de la matrice restent soumis à la CI de la PR ; aucun résultat
Windows ou macOS local n’est présumé.

## Réserves et suites

L’exemple portable utilise des doubles. Le backend réel du scénario multi-UID est
sur tmpfs : son option `QualifiedFsync` est une hypothèse de fixture, jamais une
preuve de coupure électrique. #117 doit caractériser les budgets mémoire/temps sur
le profil retenu, notamment `tick`, rétention, récupération et fermeture. Le délai
souple d’arrêt constate un dépassement ; il n’annule pas `fsync` ni un provider
arbitraire. #122 conserve la qualification et l’acceptation du dossier final.

Le contrat suppose quiescence avant arrêt/destruction et un seul consommateur.
La reprise du support exige une nouvelle session ; les refus et le backlog ancien
restent à traiter par la politique hôte. La revue d’ergonomie vérifie que le code
métier ne porte ni réglages ni entretien, sans accepter l’aptitude d’un dispositif.
