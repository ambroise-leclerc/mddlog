# Fiche de préparation de l’application de référence — #122, jalon A

Proposition de travail du 2026-10-08, **non acceptée** : elle prépare le jalon A de
[#122](https://github.com/ambroise-leclerc/mddlog/issues/122) et ne coche aucune de ses cases.
Le consommateur, son propriétaire et le profil matériel restent à désigner par le mainteneur.
L’application vit dans **son propre dépôt** ; mddlog n’en contient ni code, ni dépendance de test
([#103](https://github.com/ambroise-leclerc/mddlog/issues/103)). Ce document ne décrit pas une
validation de production et ne revendique la conformité à aucune norme.

## 1. Consommateur proposé

| Élément | Proposition | Statut |
| --- | --- | --- |
| Application | Contrôleur de pompe à perfusion volumétrique, sans actionneur réel : la mécanique est simulée, la chaîne d’audit est réelle | À confirmer |
| Dépôt | Nouveau dépôt du mainteneur (nom suggéré : `mddlog-infusion-pump`) | À créer |
| Propriétaire | À désigner | Ouvert |
| Profil matériel/OS/support | À fixer. La campagne [Orin Nano](orin-nano-campaign-review.md) est un profil déjà exercé par #114 ; l’adopter ou non est une décision du mainteneur | Ouvert |
| Témoin et lecteur | Témoin indépendant Linux ([contrat](independent-witness.md)) sous UID séparé, lecteur `mddlog-audit` ([outils](audit-tools.md)) sur copie propriétaire du lecteur | À configurer |
| Versions | Révision mddlog figée par SHA ; révision du consommateur figée par tag | À fixer à l’exécution |

**Pourquoi ce métier.** La pompe à perfusion concentre les besoins que l’audit persistant de mddlog
adresse : demandes opérateur à tracer avec leur issue, alarmes à conserver pour analyser un incident,
coupures d’alimentation en cours de traitement, journal dont l’intégrité doit pouvoir être contestée
hors de l’appareil. Des rappels publics de pompes documentent des défauts logiciels réels, dont un
cas de corruption du journal d’historique
([fiche FDA, Smiths Medical Medfusion 4000](https://www.accessdata.fda.gov/scripts/cdrh/cfdocs/cfres/res.cfm?id=107652))
et un cas d’absence d’avertissement sur un bolus dépassant la limite
([rappel Ivenix, Fresenius Kabi](https://cdph.ca.gov/Programs/CEH/DFDCS/CDPH%20Document%20Library/FDB/DeviceandDrugSafetyProgram/FreseniusKabiLVPSoftwareRecall.pdf)).
L’enquête sur le Therac-25 rapporte des journaux de traitement incomplets et le refus d’une piste
d’audit pour raison de mémoire ([Leveson et Turner](https://cs.huji.ac.il/w~feit/sem/se11/therac.pdf),
[MIT 6.033](https://web.mit.edu/6.033/2004/wwwdocs/papers/Therac_4.html)).

Ces sources motivent le choix du parcours. Elles ne fondent aucune exigence de mddlog et ne
remplacent pas la lecture des textes normatifs (journalisation des alarmes selon la norme
IEC 60601-1-8, pistes d’audit d’enregistrements électroniques selon 21 CFR Part 11) : leurs
formulations exactes n’ont pas été vérifiées ici, et aucune clause n’est revendiquée, comme dans
le [dossier de développement](../software_development_file/README.md) (GAP-002).

## 2. Vocabulaire hôte à figer

Tout identifiant invalide est refusé, jamais tronqué : le vocabulaire se fixe donc avant le code.
Les valeurs ci-dessous sont des propositions, à reprendre dans le dépôt de l’application.

| Notion | Proposition | Remarque |
| --- | --- | --- |
| Flux (`streamId`) | `pump-<serie>:boot-<n>:<producteur>` | Un flux par producteur et par démarrage ; un anneau par flux |
| Actions | `therapy.program`, `therapy.start`, `therapy.stop`, `alarm.occlusion`, `alarm.bolus-limit`, `power.loss-recovered` | Les actions `mddlog.` sont réservées au registre |
| Catégories | `Operator` (programmation, démarrage, arrêt), `RiskControl` (alarmes, limites), `Lifecycle` (démarrage, reprise), `Configuration` (bibliothèque de médicaments) | `Operator` et `Lifecycle` sont visés par l’épique |
| Phases | `Requested`, `Confirmed` (double validation d’un bolus), `Executed`, `Failed` | Une demande n’est pas une exécution |
| Acteur / cible | `nurse-<id>` ou `service-<id>` ; `pump-<serie>/channel-<n>` | Identités fournies par l’hôte |
| Corrélation | `pump-<serie>:boot-<n>:input-<m>` | Même schéma que les exemples du README |
| Références d’exigence et de risque | Seulement si l’application les possède réellement | L’épique interdit d’en inventer |
| Temps | Horloge hôte explicite ; `RawTime::unavailable()` si elle manque | Aucune garantie temporelle propre à mddlog |
| Politique de refus | Voir la section 3 | Responsabilité de l’hôte, rendue visible |

## 3. Parcours métier et critères d’acceptation de l’hôte

Chaque scénario correspond à une ligne du jalon B et produit un résultat exploitable par les
exports de [#119](audit-tools.md). Le verdict attendu est celui que `LogVerifier` doit rendre ;
l’application ne le déduit pas.

| # | Scénario | Comportement attendu de l’hôte | Verdict ou preuve attendus |
| --- | --- | --- | --- |
| 1 | Programmation puis perfusion nominale | `Requested`, `Executed` corrélés par commande | Flux *Anchored*, plage identifiée |
| 2 | Bolus au-delà de la limite, avec double validation | `Requested`, `Confirmed` ou refus, puis `Executed` ou `Failed` | Phases ordonnées, aucune exécution sans confirmation |
| 3 | Rafale d’alarmes jusqu’à anneau plein | Refus `RingFull` constaté, compteur de pertes exposé, alarme de repli de l’hôte | Aucun écrasement ; séquences sans trou côté admis |
| 4 | Fournisseur d’ancrage indisponible | Fonctionnement dégradé déclaré, santé visible | *Anchor unavailable* ou *Unanchored*, jamais « valide » |
| 5 | Coupure d’alimentation pendant un `sync` | Reprise recalcule la chaîne, frontière consignée | Frontière rapportée par le lecteur, préfixe confirmé intact |
| 6 | Capacité pleine du support et rotation | Rétention déclarée, retrait borné par l’ancrage | Retraits rapportés comme frontières |
| 7 | Redémarrage du dispositif puis du lecteur | Nouveau flux de démarrage, position retenue conservée | Continuité non déduite d’un redémarrage |
| 8 | Altération hors ligne, avec recalcul des condensats | Hors du dispositif, sur la copie du lecteur | *Altered* ou *Conflict* |
| 9 | Restauration conjointe d’un ancien journal et de son ancrage | Hors du dispositif | *Rolled back* via la position retenue |

La politique de sécurité applicative devant un refus (arrêt sûr, alarme, poursuite dégradée) est
décidée et testée par l’hôte, puis consignée avec ses réserves. Les scénarios 8 et 9 se jouent sur la
copie du lecteur, jamais sur le journal source, et leurs verdicts s’obtiennent par `mddlog-audit`.

## 4. Base avant instrumentation et lisibilité

Capturer, **avant** d’intégrer mddlog, une version du contrôleur qui journalise par un mécanisme
local (fichier texte ou appels directs). Mesurer sur cette base, puis sur la version instrumentée :

| Critère | Mesure |
| --- | --- |
| Configuration centralisée | Nombre de sites où destinations, anneaux et services sont configurés (cible : un, au point de composition) |
| Contexte réutilisé | Nombre d’appels qui répètent acteur, cible ou corrélation |
| Objets à gérer | Nombre d’objets mddlog que le code métier doit posséder ou connaître |
| Répétitions | Lignes dupliquées d’une action à l’autre |
| Lignes ajoutées | Lignes ajoutées par chemin métier, hors composition |

La méthode du [script de mesure existant](../scripts/measure-contextual-usage.py) peut servir de
modèle ; les seuils d’acceptation sont fixés par le mainteneur avant la mesure, pas après.

## 5. Tests et gel

- Les tests du consommateur tournent dans son dépôt, sans mddlog en sous-arbre.
- L’application épingle une révision mddlog ; une montée de version est un changement tracé.
- Les résultats de compilation, d’édition de liens et d’exécution sont publiés séparément.
- Les preuves décrivent un déploiement identifié (version, configuration, matériel, essais,
  limites) sans généraliser à d’autres dispositifs.

## 6. Ce que cette fiche ne fait pas

- Elle ne désigne ni propriétaire ni profil, et n’enregistre aucune décision d’acceptation.
- Elle ne modifie ni le registre du dossier de développement, ni les exigences, ni les lacunes ouvertes.
- Elle ne revendique ni certification, ni validation de production, ni conformité normative.
- Elle ne remplace pas l’examen des preuves d’ADR-004 ni la validation système du fabricant.

## 7. Décisions attendues du mainteneur

1. Confirmer la pompe à perfusion comme parcours métier (variantes écartées : ventilateur,
   traçabilité de stérilisation, radiothérapie).
2. Désigner le propriétaire et créer le dépôt de l’application.
3. Choisir le profil matériel/OS/support/témoin/lecteur.
4. Valider le vocabulaire de la section 2 et les seuils de la section 4.
5. Planifier la revue des preuves d’ADR-004, distincte de la clôture des issues.
