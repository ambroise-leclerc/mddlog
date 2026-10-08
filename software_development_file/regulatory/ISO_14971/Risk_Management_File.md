# Gestion des risques du composant

Maîtrise et statut : [index](../../README.md). Source unique des analyses : section `risks`
du [registre](../../register.json), reliée aux exigences, contrôles et lacunes.

L'analyse décrit des modes de défaillance du composant et leurs effets possibles sur la preuve
ou l'observabilité. Elle ne chiffre pas gravité/probabilité cliniques : elles dépendent de
l'utilisation dans le dispositif. Le fabricant relie ces effets à ses dangers et situations
dangereuses, établit ses critères, vérifie l'efficacité des mesures système et accepte le risque
résiduel. Aucun risque global de dispositif n'est accepté dans cette base.

La première analyse couvre perte/refus, confusion admission/durabilité, alimentation, altération,
troncature, rollback, fournisseur compromis ou indisponible, surcharge, concurrence et durées de
vie, données sensibles et mauvaise utilisation de l'API. Les mesures existantes sont identifiées
par CTRL et leur couverture par VER. Les mesures futures et limites non couvertes restent des
GAP avec épique responsable ; une mesure annoncée n'abaisse pas un risque actuel.

Revoir l'analyse à chaque changement de contrat ou profil, incident, défaut tiers ou retour
terrain. La revue du composant de #124 vérifie complétude et hypothèses ; #122 examine les
preuves réelles et réserves de la 1.0. Le fabricant conserve la décision d'acceptabilité système
et ses obligations de suivi après mise en service. Les procédures sont dans [les plans](../Plans.md).


Le premier [pilotage de référence](../../../docs/audit-service.md) complète CTRL-004
et CTRL-007 pour RISK-001/RISK-002 : un arrêt qui laisse du backlog ne ferme pas le sink,
les pertes sont raccordées à un signal indépendant et un support non qualifié conserve
ses positions durables nulles (VER-036–040). La cadence de l'hôte et la durée des opérations
externes restent des obligations de l'intégrateur ; aucun WCET ni reprise automatique
d'une instance échouée n'est établi. RISK-003/GAP-007 conservent les réserves électriques
RN-02–07 ; les empreintes d'images externes ne remplacent pas la preuve de conservation.

Le premier [lot de position retenue](../../../docs/retained-position.md) apporte
CTRL-006/VER-041 à RISK-005/RISK-006 : un lecteur redémarré conserve les checkpoints,
un conflit de génération n'écrase pas une observation concurrente et une restauration
impossible est signalée. L'hôte doit exposer séparément les erreurs de sauvegarde ; la
vérification en mémoire ne suffit pas à revendiquer une protection durable. Les preuves
utilisent un témoin en mémoire ; droits croisés, fournisseur indépendant et qualification
physique du lecteur restent ouverts dans GAP-008. Aucun risque système n'est accepté par
ce lot.

Le [témoin sous UID distinct](../../../docs/independent-witness.md) réduit les mécanismes
logiciels de RISK-005/RISK-006 via CTRL-006/VER-042–044 : custody séparée éprouvée,
retraits persistants, réponses perdues réconciliées sans stamp inventé, et rollback détecté
après redémarrage réel du lecteur. Le fixture déclare QualifiedFsync sur tmpfs uniquement
pour exercer le chemin de confirmation ; il n'accepte aucun risque de support physique.
Les risques résiduels des comptes, autorités privilégiées, disponibilité et durabilité
cible restent soumis à qualification et revue en GAP-008/#122. Aucun critère de clôture
n'est supprimé par la réussite de cette campagne.

RISK-006/RISK-007 incluent une menace de disponibilité par un UID enrôlé : un client
qui se connecte sans transmettre peut monopoliser le consommateur unique pendant
chaque échéance (1 s par défaut), puis se reconnecter en boucle. Le délai borne une
connexion, pas l'occupation cumulée ; aucun quota par UID, limitation de débit ni
équité d'ordonnancement n'est fourni. L'hôte doit définir supervision, cadence et
politique d'accès, et accepter ce risque résiduel dans son profil. Les refus de capacité
avant écriture laissent l'état lisible et n'arrêtent pas le service ; les erreurs de
stockage/barrière continuent d'arrêter l'autorité. Le coût de réécriture de l'inventaire
et des fsync synchrones participe aussi au risque de saturation, sans WCET revendiqué.


Le lot #116 ajoute VER-045–053 pour RISK-001/RISK-002/RISK-006/RISK-007 : budgets
avec rotation, exposition non confirmée/non ancrée, réconciliation des réponses perdues
et exceptions de callback isolées. La rétention exige une déclaration. Un délai souple
peut être dépassé par une I/O ; le rapport le constate au retour. Aucun arrêt forcé ne
transforme les données en preuve durable. La quiescence, la cadence inactive et le
traitement d’un backlog après panne restent des obligations hôte. #117 caractérise
les budgets du profil ; #122 garde l’acceptation et la qualification physiques.
Les inversions de positions restent des anomalies explicites malgré le bornage des
écarts à zéro. Après divergence du témoin, l’ancrage de l’instance est bloqué pour
éviter des tentatives et fautes répétées. Une fermeture terminée conserve son statut
dégradé même si le délai est dépassé ; le dépassement reste mesuré dans `elapsed`.

Pour RISK-007, CTRL-016 borne les données conservées et le travail de lecture du
profil de #117 ; VER-054–059 refusent les métadonnées hors profil, les inventaires
excessifs et les défaillances de mémoire sans verdict complet ni promotion du
checkpoint. Une limite d’historique peut empêcher une nouvelle session après longue
exploitation : l’hôte prévoit archivage, supervision et reconfiguration mesurée.
Les chunks ne réduisent pas l’image possédée à une mémoire constante. Les allocations
internes d’un backend personnalisé, fsync, callbacks, ordonnanceur et entrées
diagnostiques arbitraires restent sous contrat de déploiement. Les mesures de RSS et
les seuils CI détectent des régressions ; ils ne démontrent pas un WCET et ne décident
pas l’acceptabilité du risque système. GAP-011/GAP-015 restent ouverts.

## Diagnostic borné #118

RISK-007/008 sont reliés à CTRL-017 et VER-064/065 : plafonds avant allocation, compteurs
de pertes/commandes, barrières FIFO, santé sans réémission, snapshots possédés et garde
réentrante. La livraison diminue la croissance de file ; elle ne garantit pas un diagnostic
critique conservé ni le retour d’un sink bloqué. Un refus Fatal reste possible. Le fabricant
dimensionne les budgets et définit sa réponse aux pertes, l’exclusivité/statistique et les
durées de sinks ; GAP-011 conserve revue/acceptation et REQ-012 sa qualification du profil.

## Lot outils et export #119

RISK-013/CTRL-018 rendent explicite la confiance d’un paquet modifiable : aucun ancrage/checkpoint embarqué ne devient authentique par présence ou checksum. RISK-004/RISK-006 conservent leurs limites hors couverture et hors checkpoint indépendant ; RISK-009 exige une destination privée et des mesures hôte de confidentialité. Le filtre marque une sélection partielle sans masquer le rapport du journal complet.

Voir [contrat et usage](../../../docs/audit-tools.md) et [preuves](../../../docs/audit-tools-validation.md).

## Preuves combinées #120

VER-070–072 enrichissent les contrôles de pertes, couverture exagérée et reprise/retention : saturation, indisponibilité, writes courts, EIO, réponse perdue et suppression interrompue sont combinés. Un oracle de requêtes/octet/digest indépendant réduit le risque d’un accord circulaire encodeur/décodeur ; les contrôles négatifs refusent une fausse réussite du banc. Le [profil et ses limites](../../../docs/audit-chain-campaign.md) restent bornés ; aucune acceptation de risque ni qualification physique supplémentaire.
