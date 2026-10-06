# Essais électriques sur Orin Nano — #114

Le mainteneur a déclaré la réussite des essais le 5 octobre 2026. Le 6 octobre, il
fournit le dossier de campagne et confirme explicitement que les données sont réelles ;
les mentions héritées du template sont retirées sur son instruction. Les images restent
hors Git. Cette publication consigne les résultats du banc et leur revue documentaire,
sans nouveau rejeu matériel.

Le [rapport](validation/orin-nano/ORIN-PWR-EX-001/rapport.md), le
[profil](validation/orin-nano/ORIN-PWR-EX-001/profil.json) et les
[cycles](validation/orin-nano/ORIN-PWR-EX-001/cycles.csv) sont versionnés. La
[revue de qualité](orin-nano-campaign-review.md) identifie les contrôles exécutés,
les transformations éditoriales et les réserves techniques.

| Élément | Paramètres fournis |
| --- | --- |
| Campagne | `ORIN-PWR-EX-001`, du 14 au 21 septembre 2026 ; A. Moreau, relecture A. Leclerc selon `profil.json` |
| Cible | Jetson Orin Nano Developer Kit 8 Go, P3767-0005 / P3768-0000, révision A02 |
| Support | Samsung 970 EVO Plus 500 Go, firmware 2B2QEXM7, cache volatil activé, sans protection électrique déclarée |
| Système | Ubuntu 22.04.5 / JetPack 6.2, noyau 5.15.148-tegra, glibc 2.35, XFS et options archivées dans le profil |
| Logiciel annoncé | `f3df38328e141f5b85eebd07062f72d9e3086872`, Clang/libc++ 21.1.8, Release ; rattachement à l’exécution réservé en RN-02 |
| Coupure | Relais bipolaire sur l’entrée 19 V, acquisition du rail M.2 ; consignes 5, 10 et 30 s, mesures dans les événements JSONL |
| Acquisition | `acq-01`, alimentation indépendante selon le profil ; images et traces conservées hors dépôt |
| Résultats du banc | 2 913 tentatives, 2 910 `PASS`, trois `INVALIDE` ; 25 points de mutation à 100 cycles retenus chacun |
| Écarts consignés | Capacité 80/100, trois invalidations d’acquisition, deux démarrages lents NVMe ; réserves RN-01–07 dans la revue |

Les 8 739 renvois CSV vers les événements JSONL sont cohérents, sans orphelin ni double
référencement. Le manifeste initial diverge pour `profil.json` ; le manifeste de la
publication mise en forme est recalculé et le constat initial est conservé.

**Statut : succès de campagne déclaré, qualification du profil encore ouverte.**
La révision réellement exécutée, les preuves externes et l’oracle doivent être rattachés ;
les incohérences des observations doivent être résolues et les écarts examinés. La
confirmation de provenance n’efface pas ces réserves techniques. GAP-007 reste ouvert.

La campagne éprouve des préfixes d’octets et conserve sa portée propre. Le raccordement
au pilotage de #116 et les essais intégrés du témoin/lecteur de #115/#116/#122 restent
à réaliser. Les résultats électriques ne sont pas confondus avec les essais SIGKILL,
les erreurs injectées ou la campagne ENOSPC.
