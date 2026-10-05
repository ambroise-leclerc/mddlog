# Plan de robustesse de l’audit — #120

Ce plan complète le [protocole du support fichiers](file-storage-test-plan.md).
Il couvre les composants disponibles ; le témoin réel, sa position retenue persistante
et le pilotage dépendent encore de #115/#116. Les [résultats](audit-robustness-results.md)
identifient les essais exécutés et les critères de clôture restant ouverts.

| Famille | Banc et oracle | Critère |
| --- | --- | --- |
| Admission générative | `audit-generated-admission-model`, 32 seeds × 2 000 décisions ; file abstraite de valeurs attendues | Saturation et identifiant invalide ne consomment aucune séquence ; lecture et acknowledgement partiel préservent ordre et contenu |
| Reprise générative | `audit-generated-storage-recovery`, 64 seeds ; copies des préfixes confirmés hors du sink | Coupures open/append/sync avant/pendant/après, quatre politiques de suffixe, réponse du témoin perdue : préfixes intacts, claims bornés, ancienne identité refusée |
| Concurrence autorisée | `audit-concurrent-retries-observers`, trois SPSC de huit places, un consommateur, un observateur | 1 200 événements relus sans perte ; RingFull compté, rejet/exception réessayés, claims publiés jamais supérieurs à la position durable |
| Arrêt du diagnostic | `sink-idle-worker-stop-stress`, 2 000 constructions/arrêts ; scénario préexistant du sink lançant une exception | Aucun blocage dans le délai CTest ; modifier le prédicat sous le mutex de la condition, puis notifier |
| Lecteurs arbitraires | `mddlog_audit_readers_fuzz` et corpus versionné | Aucun crash, UB, timeout, dépassement RSS ou faux verdict ancré ; bornes de scan, refus sémantiques et reprise explicites |
| SHA-256 | `audit.sha256-oracle` / Python `hashlib.sha256`, 268 entrées reproductibles | Même digest pour entrées vides, limites de blocs/padding et entrées jusqu’à 64 Kio ; vecteurs ADR/FIPS existants conservés |
| Scénarios de chaîne | `AuditStorageSpec`, `AuditRestartSpec`, `AuditRetentionSpec`, `AuditEndToEndSpec` | Réponses perdues, ruptures de barrière, rollback, suppression/rétention interrompue et verdicts bornés conformément aux scénarios existants |

Les snapshots de santé ne sont pas des transactions globales. Le test concurrent compare
les compteurs individuellement et échantillonne le claim avant la position durable monotone.
L’enregistrement des anneaux est terminé avant les threads. Une seule personne logique
consomme et modifie le sink ; le fournisseur et les positions retenues restent sur leur thread
autorisé. Aucun test n’attribue à ces objets une sûreté concurrente absente du contrat.

## Campagnes reproductibles

Construire avec Clang 21/libc++, ASan et UBSan dans un build distinct :

```bash
cmake -S . -B build-robustness -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-clang21-libcxx.cmake \
  -DCMAKE_BUILD_TYPE=Debug -DMDDLOG_BUILD_TESTS=ON -DMDDLOG_BUILD_EXAMPLES=OFF \
  -DMDDLOG_BUILD_FUZZERS=ON -DENABLE_SANITIZER_ADDRESS=ON \
  -DENABLE_SANITIZER_UNDEFINED_BEHAVIOR=ON
cmake --build build-robustness --parallel 4
python3 scripts/run-audit-robustness.py \
  --sha-worker build-robustness/tests/mddlog_sha256_oracle \
  --fuzzer build-robustness/tests/mddlog_audit_readers_fuzz \
  --output build-robustness/readers --runs 100000 --seed 120
```

libFuzzer utilise des mutations guidées par couverture. Le banc exerce décodage canonique,
framing/CRC, registre, analyse de journal, vérification et récupération. Il conserve aussi
un encadrement valide des octets mutés pour atteindre les lecteurs sémantiques après le CRC.
Le corpus initial contient les vecteurs ADR-004, leur segment composé, une troncature,
une version inconnue et un registre d’origine. Ces seeds sont des vecteurs de test,
pas des archives produites par une version publiée ; cette compatibilité reste due en #121.

Budgets : entrée de 64 Kio, quatre segments pour la récupération, RSS de 512 Mio,
deux secondes par entrée, sortie de 2 Mio, 600 secondes par invocation par défaut.
Le superviseur tue et récolte le groupe de processus en cas de faute ou timeout.
Une sortie prématurée à code zéro ne suffit pas : le compteur d’entrées doit atteindre
le budget demandé. Un profil de couverture exécuté est exigé pour chacun des six lecteurs
principaux ; cela ne fixe pas une couverture exhaustive ni un pourcentage global arbitraire.

Le workflow `Audit Robustness` exécute 10 000 entrées par PR/push ; son exécution hebdomadaire
ou manuelle demande un million d’entrées avec un délai de 3 600 secondes, 100 répétitions
des interruptions fichiers et un volume plein réel obligatoire. Le job est borné à 90 minutes.
Le workflow TSan sélectionne aussi les scénarios `AuditRing`
et les observateurs de l’audit. Un workflow configuré ne constitue pas un résultat exécuté.

En cas de crash, conserver l’artefact libFuzzer, puis réduire avec `-minimize_crash=1`
sur le même build, transformer le résultat en régression et rejouer sa famille. Les nouveaux
seeds du corpus généré sont à examiner avant leur ajout au corpus versionné. Archiver
rapport JSON, hashes des workers/seeds, logs, corpus final, profils LLVM et couverture,
versions/options, révision et diff. Le responsable de campagne décide leur conservation
avec #122. `--disable-leak-detection` consigne explicitement l’absence de LSan sous ptrace ;
la CI conserve LSan activé et aucune fuite n’est réputée vérifiée par une exécution sans LSan.

## Portes de clôture

Le bilan électrique de l’Orin Nano doit identifier le tuple matériel/stockage/OS/cache,
la révision, l’opérateur, les points et cycles, les accusés et l’acquisition indépendante.
Une réussite déclarée est un résultat à consigner ; les paramètres absents ne sont pas inventés.
La partie logicielle ne remplace ni ces preuves, ni les campagnes du témoin réel de #115,
ni le raccordement au pilotage de #116. Leur maintien ou leur transfert explicite dans
ces issues doit être décidé avant de présenter #114/#120 comme terminées.
