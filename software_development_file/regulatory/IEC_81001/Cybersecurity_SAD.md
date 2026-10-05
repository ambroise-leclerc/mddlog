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
