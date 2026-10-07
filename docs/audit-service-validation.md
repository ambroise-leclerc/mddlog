# Validation du pilotage — #116

Relevé du 2026-10-07 sur le lot `116-audit-pilotage`, issu de
`8ac78610cfa398c0ccd224d36710281f1194662b`. La révision soumise et son diff identifient
le logiciel vérifié ; ce relevé ne modifie pas l’acceptation de la base #124.

## Couverture

- VER-036–040 : contrats conservés, drain/arrêt (dont budget invalide), accès au stockage,
  flux inactif, support non qualifié,
  pertes et observateurs concurrents.
  L’arrêt sans témoin conserve l’exposition non ancrée et `Degraded` ; `stop(0)`
  laisse un backlog intact, sans fermer.
- VER-045 : budget global, rotation et refus de saturation/enrôlement ; collisions
  avec le registre et capacité comptées en santé, sans drain des anneaux refusés.
- La rotation est aussi vérifiée sur trois anneaux partiellement drainés avec un
  budget global d’une tentative.
- VER-046 : réconciliation d’une acceptation dont la réponse est perdue, sans rejeu ;
  divergence ou retraite bloque les appels ultérieurs, même après croissance du flux.
  Absence et ancre plus ancienne autorisent un rejeu sans réduire la couverture
  publiée non vérifiée ; un rapprochement partiel conserve l’âge d’exposition prudent.
- VER-047 : ancrage par âge sous le seuil en nombre ; fenêtre d’exposition publiée.
- VER-048 : appel fournisseur simulé lent, dépassement du délai souple d’arrêt et reprise ;
  une fermeture terminée conserve sa qualité même après dépassement du délai.
  Les écarts de positions inversées sont bornés et leur anomalie reste explicite.
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

Après revue, les vérifications VER-036 et VER-045 ont été complétées : budget d’arrêt
invalide sans gel des inscriptions, accès au sink possédé, refus d’identité du registre
et de capacité visibles en santé sans consommation des anneaux refusés. La suite
Clang reste à **208/208** ; les **14/14 tests du service** passent aussi sous GCC et
ASan/UBSan. Ces ajouts complètent des scénarios existants sans changer leur nombre.
La seconde revue complète VER-045/046/048 : rotation à trois anneaux, divergence et
retraite du témoin sans nouvelles tentatives, écarts de positions inversées et
fermeture lente conservant `Completed`/`Degraded`. Les **208/208 CTest Clang**,
**14/14 tests du service GCC** et **187/187 scénarios ASan/UBSan** passent après
ces corrections. Les erreurs de budgets et seuils sont distinguées avant I/O.
La revue des cas conservateurs complète VER-036/046 : fermeture sans témoin,
`stop(0)`, ancre plus ancienne ou absente et âge conservé après rapprochement
partiel. Après suppression de l’état d’inscription dupliqué dans le service,
les **208/208 CTest Clang**, **14/14 tests du service GCC** et
**187/187 scénarios ASan/UBSan** passent encore.

## Réserves et suites

L’exemple portable utilise des doubles. Le backend réel du scénario multi-UID est
sur tmpfs : son option `QualifiedFsync` est une hypothèse de fixture, jamais une
preuve de coupure électrique. Le [lot #117](audit-resource-validation.md) caractérise
les budgets mémoire/temps du profil logiciel Linux x86_64, notamment reprise,
rétention, poll, observateurs et fermeture. Le délai
souple d’arrêt constate un dépassement ; il n’annule pas `fsync` ni un provider
arbitraire. #122 conserve la qualification et l’acceptation du dossier final.

Le contrat suppose quiescence avant arrêt/destruction et un seul consommateur.
La reprise du support exige une nouvelle session ; les refus et le backlog ancien
restent à traiter par la politique hôte. La revue d’ergonomie vérifie que le code
métier ne porte ni réglages ni entretien, sans accepter l’aptitude d’un dispositif.


## Clôture du critère observateurs/budgets — lot dépendant de #142

Le dernier critère de #116 est désormais couvert pour le profil logiciel déclaré :
VER-059 applique les limites de #117 au service et mesure 32 producteurs, saturation
imposée, témoin indisponible puis rétabli, passes/observations et arrêt quiescent.
Les trois répétitions respectent les enveloppes ; les mesures et leurs réserves
sont dans [audit-resource-validation.md](audit-resource-validation.md).

Le scénario `audit-service-concurrent-health` déclare aussi explicitement ses
cardinalités et couvre tous les observateurs permis : santé du service/sink,
claim et position durable, admissions/acquittements/refus atomiques. Il conserve
l’ordre des positions sans exiger la simultanéité de snapshots distincts.
VER-060 ajoute le cas de consommation avancée du sink : un historique épuisé reste
visible dans `resourceIssue` et donne `Degraded` à l’arrêt, même si tous les records
stockés sont confirmés et ancrés et les anneaux vides. Il ne masque pas la cause
sous `Completed` et ne crée ni perte ni refus d’ancrage fictifs.

La PR du présent lot cible `develop` et dépend de #142 : fusionner #142 puis
rebaser ce lot pour garder uniquement son diff de clôture. La revue du profil et
la CI demeurent requises ; la livraison du code et la preuve de budget ne valent
pas nouvelle acceptation de la base ou du risque de dispositif. GAP-009 peut être
clos après revue/fusion de ces deux lots ; GAP-015 et #122 restent ouverts.


Validation de ce lot de clôture : Clang Release **218/218 CTest**, GCC Release
**217/217 CTest**, ASan/UBSan **197/197** scénarios et intégrations sélectionnés.
La sélection TSan des anneaux, observateurs du service, registre, refus de ressources
et benchmark de charge passe **29/29** sous Clang/libc++ ; la CI utilise GCC/libstdc++.

Un élargissement local TSan à 13 cas sous Clang/libc++ 21 a échoué dans deux tests
du `SimpleLogger` historique, tandis que les onze autres cas passent. Le signalement
porte sur la destruction de l’état partagé d’un `promise<void>` et l’attente du
`future`. Il se reproduit aussi sans mddlog, avec 20 000 itérations de la seule
séquence standard suivante : créer promise/future, déplacer la promise dans un
thread qui appelle `set_value`, attendre puis détruire le future, joindre le thread.
Ce résultat ne permet pas d’affirmer une course dans le logger : la libc++ partagée
de ce tuple n’est pas instrumentée. Aucun filtre de suppression n’est ajouté et
ce contrôle élargi n’est pas déclaré réussi. #120 conserve le suivi du tuple et
#118 la validation du logger/flush. Les maxima de budget restent des mesures,
sans extrapolation WCET.
