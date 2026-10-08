# Suivi du tuple TSan/libc++ — #147

Ce dossier conserve la source et la portée de l'observation initialement publiée
dans [#120](https://github.com/ambroise-leclerc/mddlog/issues/120#issuecomment-6038244094),
désormais suivie explicitement dans [#147](https://github.com/ambroise-leclerc/mddlog/issues/147).
La clôture logicielle de #120 ne résout pas ce signalement.

Observation du 7 octobre 2026 : Linux x86_64, Clang/libc++ 21.1.8, libc++ partagée
non instrumentée. Deux des treize tests élargis du SimpleLogger historique sont
signalés autour de promise/future ; les onze autres et la sélection indépendante
de 29 anneaux/observateurs/registre/profil passent. Le reproducer autonome de
20 000 itérations signale aussi ce cas ; le contrôle GCC 16.1/libstdc++ sur le même
hôte termine avec code zéro. Voir [le relevé historique](../../../audit-service-validation.md).
Les logs bruts de cette observation ne sont pas présents dans l'artefact conservé :
il s'agit d'une observation rapportée, pas d'un résultat nouvellement reproduit.

Commande originale, depuis ce répertoire :

```bash
clang++-21 -std=c++23 -stdlib=libc++ -fsanitize=thread -g -O1 -pthread Repro.cpp -o /tmp/mddlog-tsan-libcxx-repro
TSAN_OPTIONS=halt_on_error=1 /tmp/mddlog-tsan-libcxx-repro
```

Reste à figer et construire libc++/libc++abi instrumentées, reproduire et comparer
les contrôles sur le même hôte, puis conserver sources, commandes, versions et logs
dans un nouveau jeu de preuves Git. La revue doit classer le signalement et décider
de son incidence sur la matrice #121 et la candidate #122 avant de revendiquer la
qualification de ce tuple. Aucune suppression globale ni qualification implicite.

#118 a remplacé ce mécanisme du logger par une condition de complétion. Le TSan
GCC de la révision livrée par #146 passe ; son log est conservé dans le
[jeu du 8 octobre](../2026-10-08/README.md). Cela ne démontre ni une course dans
mddlog actuel, ni un faux positif de TSan, ni une correction de libc++.
