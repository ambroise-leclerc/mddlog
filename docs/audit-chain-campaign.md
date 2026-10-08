# Chaîne réelle : modèle et campagnes #120

Ce lot complète les [preuves précédentes](audit-robustness-results.md) sur la base
`197574f2ceefb72a7e0aaebe6ad322cce2e8fe7d` de `develop`. Il ajoute des histoires
reproductibles aux composants livrés par #115–#119. Les contrats des ADR ne changent pas.
Livraison des bancs et résultats locaux soumis à revue ; aucune acceptation du mainteneur
ni clôture de #120/#121/#122 n’est inférée de leur exécution.

## Traçabilité et plan de preuves

Les identifiants relient le registre unique aux exigences ; les scénarios existants restent
les preuves des variantes que le profil génératif ne visite pas. Statut de revue de ce
complément : **proposé**, y compris sérialisation indépendante et limites ci-dessous.

| Contrat / exigence | Scénario et preuve attendue | Limite |
| --- | --- | --- |
| ADR-001/002, REQ-003/008, CTRL-003/009 | VER-030 et VER-070 : file abstraite de capacité quatre, ordre exact, refus uniquement à capacité atteinte, réemploi sans consommation de séquence | SPSC ; les admissions ne prouvent pas une mutation métier |
| ADR-002/005, REQ-009/011, CTRL-013/014 | Tests de bindings/contextes existants ; VER-070 reconstruit Lifecycle/Requested, sourceSequence et troncature à 160 octets depuis la requête originale | Les autres phases et temps sont couverts par les vecteurs, pas par le générateur réel |
| ADR-003/004 9.3, REQ-004/007, CTRL-004/007 | VER-070 : comptabilité admissions = transmis + en attente ; digest témoin jamais au-delà du préfixe confirmé, writes courts réels, échec EIO après un octet ou lors de sync | Sync logiciel sur fichiers ; pas de coupure électrique |
| ADR-004 8, REQ-005, CTRL-005 | VER-070/072 : encodeur Python selon le contrat, hashlib sur chaque événement admis et ses digests ; corpus ADR conservé et segments originaux v0.2/v0.3 ajoutés | Deux implémentations ne démontrent pas à elles seules l’exhaustivité du contrat |
| ADR-004 7.3, REQ-006, CTRL-006 | VER-070 : indisponibilité prolongée, réponse perdue après commit réel, réconciliation latest ; checkpoint réel rechargé après rollback de journal et témoin | Témoin fonctionnel sous le même UID ; indépendance par VER-044 répété séparément |
| ADR-004 10.2/10.3, REQ-007/010, CTRL-008 | VER-070 : nouveau ledger à chaque boot, récupération du préfixe antérieur et refus de l’ancienne identité producteur | Destruction sans close, sans tuer le processus ; les arrêts SIGSTOP/SIGKILL restent VER-027 |
| ADR-004 10.4/10.5, REQ-010, CTRL-008 | VER-070 : trim borné par le préfixe ancré ; unlink réel puis barrière de répertoire rompue ; suffixe relu sans trou après reprise. Tests AuditRetentionSpec pour retrait complet/retirement | Un seul reclaim interrompu par boot ; pas toutes les permutations des retraits complets |
| Pilotage #116, REQ-010, CTRL-004/007 | VER-070/071 : budget global d’un événement par poll, horloge injectée et saturation persistante ; VER-036–060 conservent équité multi-producteurs, callbacks et observateurs | Générateur séquentiel ; aucune extension du contrat concurrent |
| Ressources #117 et diagnostic #118, REQ-012/017, CTRL-016/017 | VER-071 avec WitnessSpec (compteur UINT64_MAX), FileStorageSpec (références épuisées), AuditResourceSpec (addition/cardinalités/tailles) et VER-064/065 | Profils finis ; jamais une exécution de 2^64 admissions ni un WCET |
| ADR-006 proposé, outils #119, REQ-018, CTRL-018 | VER-071/069 : CLI, export complet, relecture, corruption, confiance et budgets sur fichiers et archives v0.2/v0.3 ; VER-072 injecte ces vrais segments dans les lecteurs | Politique multi-releases, durée de lecture et gel du format dus à #121 |

## Modèle indépendant

`AuditChainCampaign.cpp` compose AuditRing, AuditService, FileStorageMedium,
FileAnchorAuthority et FileRetainedPosition. Les seams limitent les transferts de vrais
descripteurs et renvoient EIO selon les conventions POSIX. Les commits du témoin restent
réels : le décorateur peut masquer leur réponse ou simuler l’indisponibilité, sans fabriquer
d’anchor. L’interruption du reclaim est armée dans le décorateur StorageMedium au moment
du vrai reclaim, pour ne pas confondre une rotation du ledger avec une suppression.

Le JSONL contient les requêtes admises originales, refus, positions observées, octets et
digests stockés, bornes de trim et octets relus après reprise. Le superviseur possède sa
propre file abstraite : taille = admissions − transmissions, refus seulement à quatre
éléments, séquence suivante = nombre d’admissions + 1. Il construit C_k depuis la requête
selon ADR-004 8.2 (y compris troncature/flag), puis H_k = hashlib.sha256(C_k || H_k−1),
H_0 nul. Il exige l’égalité des octets, digests et anchors. L’oracle ne rappelle ni
CanonicalRecord, ni decodeCanonical, ni AuditChain. Les digests de reprise restent ceux
de l’histoire entière, même quand un préfixe physique a été supprimé.

Après un EIO d’append, le suffixe incomplet n’est jamais exigé ; après un EIO de sync,
l’événement entièrement écrit peut être relu mais n’agrandit pas l’anchor confirmé.
Après un reclaim interrompu, certains segments du trim peuvent rester : leur chevauchement
est permis, tout trou dans le suffixe confirmé est refusé. Un boot supplémentaire vérifie
aussi le dernier préfixe. Le lecteur recharge son checkpoint avant le rollback combiné ;
Le compteur takenUnacknowledged désigne une partie des événements encore présents dans
l’anneau : il ne s’additionne pas à pendingInRings. Après une faute terminale, transmis +
pending doit égaler les admissions, et chaque transmission dépassant le préfixe durable
doit être comptée dans reportedLosses. Au moins un verdict RolledBack est obligatoire, aucun PASS global provenant du seul code
de sortie du worker n’est accepté. Les contrôles négatifs altèrent inputs, digests,
couverture de reprise, anchors et bornes, et refusent un worker à code zéro sans trace.

Chaque boot combine saturation, calendrier de polls généré, writes courts, indisponibilité,
flush, réponse perdue, réconciliation, rotation et une faute terminale. `(seed + boot) % 3`
sélectionne append partiel, sync rompue ou unlink/barrière interrompu. Les trois familles
sont obligatoires par histoire ; la seed règle aussi la taille des writes et le calendrier.
Les états après coupure électrique ne sont pas simulés par la simple présence de fichiers.

## Exécution, budgets et reproduction

Build Linux normal avec tests et backend fichiers ; le preset Clang les active. Profil court :

```bash
cmake --preset ninja-clang
cmake --build build-clang --parallel 4
ctest --test-dir build-clang -R '^audit.chain-campaign$' --output-on-failure
```

Profil prolongé et intégrations réellement requises :

```bash
python3 scripts/run-audit-chain-campaign.py \
  --worker build-clang/tests/mddlog_audit_chain_campaign \
  --output build-clang/chain-long --seed 120 --seeds 32 \
  --boots 24 --records 64 --outage 1000 --timeout 120 \
  --deployment-worker build-clang/tests/mddlog_witness_deployment \
  --witness-service build-clang/examples/mddlog_witness --deployment-repetitions 32 \
  --tools-cli build-clang/mddlog-audit \
  --tool-reference build-clang/tests/mddlog_audit_tool_reference
```

Le témoin d’exemple exige `MDDLOG_BUILD_EXAMPLES=ON`, les outils
`MDDLOG_BUILD_AUDIT_TOOLS=ON`. Le profil court choisit deux seeds, trois boots,
32 événements par flux, 16 refus puis 16 polls sans fournisseur. Bornes maximales :
32 seeds, 24 boots + un boot de récupération, 64 événements par flux, 1 000 refus
puis 1 000 polls sans fournisseur ; 512 segments de 4 Kio et reads de 4 Kio, JSONL
de 16 Mio par histoire. Délai de 60 s par worker court, 120 s demandé en long,
maximum configurable 600 s. Le superviseur partagé borne les logs à 2 Mio et tue/récolte
le groupe en cas d’échec ou timeout. Les intégrations sont bornées à 75 s par invocation.
Le job CI garde son délai global de 90 minutes ; un timeout ne vaut pas succès.

Chaque histoire conserve trace, fichiers et logs ; report.json contient commandes,
seeds, profils, révision/worktree, empreintes des sources/worker/traces/corpus et versions.
L’absence des options d’intégration produit NOT_EXECUTED pour celles-ci ; leur présence
exige toutes les répétitions avec isolation multi-UID, sans SKIP converti en PASS.
Rejouer une anomalie avec `--seed N --seeds 1` et les autres paramètres du rapport.
Conserver la trace réduite en régression après revue ; ne remplacer aucun vieux corpus
par une sortie générée avec la bibliothèque actuelle. Le contrôle CI court et la campagne
longue sont distincts ; leur configuration ne constitue pas un résultat exécuté.

## Résultats et réserves

Les résultats locaux du 8 octobre 2026 sont consignés dans le
[rapport de robustesse](audit-robustness-results.md). VER-070–072 enrichissent GAP-003/013,
qui restent ouverts pour revue et conservation des preuves. Les campagnes ne prouvent
ni chaque interleaving, ni l’endurance physique du stockage, ni l’indépendance du témoin
du profil sous un seul UID. La campagne multi-UID répète VER-044 sans attribuer ses
permissions aux histoires injectées sous un seul UID. Le suivi libc++/TSan antérieur,
la politique des futures archives et la durée de support #121, ainsi que l’acceptation
explicite #122 restent ouverts. Ce lot ne ferme aucune issue automatiquement.
