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
