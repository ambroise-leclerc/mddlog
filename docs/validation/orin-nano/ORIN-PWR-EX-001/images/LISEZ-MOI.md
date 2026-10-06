# Images brutes conservées hors Git

Les images de la partition d’essai de 1 Gio sont conservées hors de ce dépôt, conformément
à la décision du mainteneur du 6 octobre 2026. Selon le dossier fourni, elles sont acquises
avant montage depuis l’environnement de secours puis compressées sur `acq-01`.

[index.csv](index.csv) contient les 2 913 identifiants de cycle, noms de fichiers, tailles
et empreintes déclarées de l’image brute et de sa compression. Ces empreintes sont
versionnées pour identifier les fichiers externes ; elles n’ont pas été vérifiées contre
les images lors de cette revue. Les chemins `images/cycle-NNNN.img.zst` sont des références
à l’archive externe, pas des liens vers des fichiers présents dans Git.

Le profil indique une conservation sur `acq-01`, puis dans une archive de qualification.
L’emplacement durable exact et les modalités d’accès n’ont pas été fournis. La revue de
qualification reste ouverte jusqu’à rattachement et examen des preuves.
