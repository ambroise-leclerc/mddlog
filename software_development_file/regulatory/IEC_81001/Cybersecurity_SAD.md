# Architecture de cybersécurité et menaces

Maîtrise et statut : [index](../../README.md). Applicabilité proposée dans
[la matrice](../Applicability.md), menaces et contrôles dans [le registre](../../register.json).

Actifs : contenu des événements, ordre et couverture de l'audit, identité de flux, disponibilité
d'admission/lecture, position retenue, ancrages et artefacts de construction. Frontières : hôte
vers cœur, anneau vers adaptateur, adaptateur vers support modifiable, support vers lecteur,
fournisseur indépendant et stockage du lecteur. Aucune clé ou signature n'existe dans le cœur.

Adversaires considérés : rédacteur capable de réécrire le support, opérateur pouvant restaurer
une ancienne image, fournisseur indisponible ou compromis, appels surchargés ou identifiants
malformés, accès indu aux journaux, dépendance/outillage compromis. Les RISK relient effets,
contrôles existants et lacunes : l'intégrité du journal est relative à un ancrage authentique ;
le chaînage seul n'authentifie pas l'auteur. Au-delà du dernier ancrage, altération et suppression
peuvent rester indétectables. Si journal et ancrage reculent ensemble, seule une position retenue
indépendante permet de détecter le rollback ; si cette autorité est compromise, la garantie tombe.

Le backend fichiers Linux vérifie propriétaire/permissions et refuse liens et inventaire ambigu,
avec verrou consultatif. VER-024 couvre les substitutions après ouverture, le FIFO sans
écrivain et la fermeture à exec ; la validation du nom et de l'inode reste soumise à la
coopération des accès concurrents. VER-025 couvre le compteur persistant d'identités, qui
ne constitue pas un témoin indépendant contre restauration d'une ancienne image.
Ces contrôles locaux ne résistent pas à un propriétaire malveillant
ou privilégié. Ni chiffrement ni effacement sécurisé ne sont fournis par mddlog. Le fabricant minimise les données sensibles, protège stockage, sauvegardes et accès,
qualifie la disponibilité et l'indépendance du fournisseur, et protège sa chaîne de livraison.
#114/#115 produisent les backends ; #119 le lecteur/export ; #120 les essais adverses ;
#123 porte la signature et les clés, explicitement hors critères de publication 1.0.

GAP-017 suit explicitement la qualification des hypothèses de minimisation et confidentialité
par le fabricant du profil : catégories autorisées/exclues, messages et identifiants, accès,
stockage, export et conservation. REQ-016 et RISK-009 y renvoient ; ces mesures hôte ne sont
ni implémentées ni vérifiées par la bibliothèque.

Les DEP identifient la chaîne de construction et sa surveillance. Le suivi des vulnérabilités
utilise la procédure d'anomalies des [plans](../Plans.md) ; canal privé, responsable opérationnel
et délais sont une lacune explicite GAP-004 à résoudre avant publication. Aucun processus de
réponse déjà opérationnel n'est revendiqué ici.


La [revue des archives externes](../../../docs/file-storage-closure.md) compare les
empreintes sans importer les images dans Git et refuse les références sortant du
répertoire déclaré. Elle requiert une archive stable ; un SHA-256 comparé à l'index
identifie des octets, sans prouver leur provenance ni authentifier l'oracle. Le pilotage
ne remplace ni les accès exclusifs du backend ni la responsabilité de l'hôte sur les
identités, la quiescence et les durées de vie. Aucun chiffrement ni témoin indépendant
n'est ajouté par ce lot.

La [sauvegarde du lecteur](../../../docs/retained-position.md) complète CTRL-006/VER-041
pour RISK-005/RISK-006 : identité du fournisseur explicite, absence/corruption refusées,
génération concurrente et monotonie des checkpoints. Les permissions privées et le refus
des liens finaux ne prouvent pas la séparation des autorités, ni la protection des parents,
ACL ou privilèges root. Le checksum n'est pas une authentification. Un rollback du magasin
du lecteur par sa propre autorité reste hors modèle ; le service authentifié et les essais
d'accès croisés sont complétés par le lot ci-dessous, sans clore GAP-008.

Le [témoin séparé](../../../docs/independent-witness.md) complète CTRL-006 et VER-042–044 :
UID serveur épinglé avec SO_PEERCRED, identité fournisseur enrôlée, droits UID/flux
exacts et state inaccessible au rédacteur dans le profil exécuté. Les réponses perdues
restent incertaines jusqu'à une lecture authentifiée ; les conflits ne deviennent pas
idempotents. Les credentials dépendent du namespace utilisateur et du noyau ; comptes,
ACL/parents/montages et non-réaffectation des UID restent à qualifier. Root ou le témoin
compromis restent hors résistance. Les frames sont bornées, mais saturation par clients
autorisés et fsync lent requièrent la politique hôte. Aucune signature ou authentification
de service réseau distant n'est fournie. GAP-008 reste ouvert pour revue/acceptation.

## Surcharge diagnostique #118

Des entrées très longues ou un débit excessif ne créent plus une file illimitée : CTRL-017
refuse avant allocation au-delà des budgets, y compris les commandes de flush expirées.
La boucle de diagnostic synchrone est refusée par thread et les erreurs sont dans la santé,
sans sink secondaire. Les callbacks différés, données propres des sinks et leur durée restent
des obligations hôte ; une panne peut encore supprimer l’observabilité. VER-064/065 vérifient
les bornes locales ; aucune résistance exhaustive au déni de service n’est revendiquée.

## Lot outils et export #119

RISK-013/CTRL-018 : le décodeur refuse versions/troncatures/doublons/budgets et ne restaure aucun chemin de l’archive. La confiance embarquée est une hypothèse explicitement acceptée ; le fournisseur Unix authentifié nécessite une indépendance de déploiement qualifiée. Checkpoint embarqué non authentifié, source externe privilégiée ; aucune signature ni mécanisme #123 ajouté.

Voir [contrat et usage](../../../docs/audit-tools.md) et [preuves](../../../docs/audit-tools-validation.md).
