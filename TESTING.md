# Tests de Gad

[English (US)](TESTING.en-US.md)

## Lancer la validation

Depuis la racine du projet :

```bash
make doctor           # Vérifie les outils, bibliothèques et composants Rust
make test             # Formatage, Clippy, Rust, Python, Qt et intégration
```

Sans `make`, la commande équivalente est :

```bash
python3 scripts/run_tests.py all
```

Le lanceur renvoie **0 si toutes les étapes réussissent**, et un code non nul en cas d’échec. Par défaut, il s’arrête au premier échec. `--keep-going` exécute aussi les étapes indépendantes suivantes ; l’intégration reste suspendue si sa compilation échoue.

```bash
python3 scripts/run_tests.py all --keep-going
python3 scripts/run_tests.py all --list
```

## Groupes disponibles

| Commande | Périmètre |
| --- | --- |
| `make test-unit` | Tests Rust et Python |
| `make test-python` | Emplacements XDG, raccourci Dolphin et lanceur de tests |
| `make test-qt` | Presse-papiers, fenêtre, conflits, menus et notifications |
| `make test-integration` | Compile puis teste le vrai binaire et son protocole local |
| `make check` | Formatage et Clippy avec alertes traitées en erreurs |
| `make test-bench` | Mesure volontaire du rafraîchissement des gros historiques |

Le nom du groupe peut être passé directement au lanceur : `unit`, `python`, `qt`, `integration`, `lint`, `bench` ou `all`.

```bash
python3 scripts/run_tests.py unit --filter rollback_
python3 scripts/test_qt.py --suite row_actions
python3 scripts/test_qt.py --suite clipboard --suite notifications
cargo test --locked engine::tests::copies_tree_and_symlink_without_following_it
```

Le filtre du lanceur s’applique aux tests Rust ; les tests Python restent exécutés. Le benchmark est ignoré dans la validation normale. Les anciens scripts `test_clipboard.py`, `test_notifications.py` et `test_integration.py` restent utilisables.

## Rapports

Chaque exécution du lanceur écrit dans `target/test-results/` :

- Un fichier `.log` par étape exécutée.
- `results.json` : commandes, codes de sortie, états et durées.
- `junit.xml` : une entrée **par étape de validation**, avec sa sortie complète. Ce rapport ne prétend pas représenter chaque cas Rust ou Qt individuellement ; leurs noms et résultats détaillés figurent dans les journaux.

```bash
python3 scripts/run_tests.py all --report-dir /tmp/gad-results
```

Les rapports JSON et JUnit correspondent à la dernière exécution ; ils indiquent les journaux de ses étapes. Des journaux d’anciennes étapes peuvent rester disponibles. Les fichiers sous `target/` sont exclus des sources par `.gitignore`.

## Prérequis

- Linux, Rust/Cargo récents (1.85 ou supérieur), `rustfmt` et Clippy.
- Un compilateur C++17, `pkg-config`, Qt 6 avec Widgets, Network, DBus et Test.
- `qmake6`, `moc`, `rcc`, KF6WindowSystem et `dbus-run-session`.
- Python 3, uniquement sa bibliothèque standard : aucun `pip install` n’est nécessaire.

Sur Debian Trixie, les paquets nécessaires sont également décrits dans le workflow CI :

```bash
sudo apt install build-essential pkg-config python3 cargo rustc rustfmt rust-clippy \
  qt6-base-dev qt6-base-dev-tools qt6-qpa-plugins qmake6 libkf6windowsystem-dev dbus fonts-dejavu-core
```

Les composants [KF6WindowSystem](https://packages.debian.org/trixie/libkf6windowsystem-dev), [Clippy](https://packages.debian.org/trixie/rust-clippy) et [rustfmt](https://packages.debian.org/trixie/rustfmt) sont disponibles dans cette distribution. Sur une autre distribution, utiliser les paquets équivalents ; `make doctor` indique les éléments manquants.

## Isolation

Les tests de fichiers utilisent des répertoires temporaires. Les tests Qt se lancent hors écran et utilisent des répertoires XDG ainsi qu’un bus D-Bus privés. Ils n’installent pas Gad, ne modifient pas le presse-papiers réel et ne redémarrent pas l’instance du bureau.

Les ouvertures de fichiers et de dossiers sont interceptées par un gestionnaire `QDesktopServices` dans les tests : aucun éditeur, navigateur de fichiers ou programme utilisateur n’est lancé. Les demandes KIO et notifications KDE utilisent des services simulés. Le test d’intégration contrôle uniquement son propre binaire dans un environnement isolé. Les serveurs FTP/SFTP/SMB réels ne sont pas nécessaires.

## Ajouter un test

- **Moteur Rust** : ajouter une fonction `#[test]` dans `src/engine/tests.rs`, avec `Fixture` pour les fichiers temporaires. Utiliser `wait` pour attendre un état métier, puis vérifier aussi le contenu des fichiers. Terminer les opérations et appeler `shutdown` pour les moteurs créés par les nouveaux tests.
- **Menus et chemins d’ouverture Qt** : ajouter un slot de test ou des lignes de données dans `tests/row_actions.cpp`. Le double du moteur contrôle l’état affiché, `DesktopCapture` intercepte l’ouverture.
- **Autres comportements de fenêtre ou presse-papiers** : scénarios dans `tests/clipboard.cpp` ; progression D-Bus dans `tests/notifications.cpp`. `scripts/qt_tests.py` mutualise la compilation des unités natives.
- **Scripts Python** : créer `tests/python/test_*.py`, utiliser `unittest.TestCase`, `TemporaryDirectory` et `patch.dict` pour isoler fichiers et environnement. La découverte est automatique.
- **Nouvelle suite Qt** : l’ajouter à `SUITES` dans `scripts/qt_tests.py`, prévoir son fichier `tests/NOM.cpp` et son inclusion `NOM_test.moc`.

Un bon test reproduit un comportement observable : état obtenu, fichiers préservés ou restaurés, commande exacte envoyée au bureau, absence d’une modification indésirable. Éviter les attentes arbitraires lorsqu’un signal ou un état peut être attendu.

## Intégration continue

`.github/workflows/tests.yml` fournit une validation complète dans un conteneur Debian Trixie et conserve les rapports même en cas d’échec. Il sera actif lorsque ce projet sera placé dans un dépôt GitHub. Cette configuration est fournie ; son exécution distante n’a pas été effectuée ici.

## Déplacements rapides locaux

Les cas `fast_move` vérifient les inodes après transfert et restauration, l’exécution à côté d’une copie (y compris vers un dossier de destination commun), la réservation des nouveaux noms après conflit, les chemins qui se recouvrent, MD5, les conflits tardifs, les cibles concurrentes, la replanification EXDEV, les annulations et le journal après redémarrage. Une altération de même taille avec mtime rétabli doit être refusée à la restauration. Les chaînes de remplacement après un déplacement rapide sont également contrôlées.

```bash
python3 scripts/run_tests.py unit --rust-filter fast_
```

Les fixtures rapides utilisent `/dev/shm`, un tmpfs local accessible en écriture sous Linux, afin de tester le parcours même lorsque `/tmp` est sur un overlay dans le conteneur CI. Les cas d’alias de montage, réseau et FUSE utilisent des descriptions de montages simulées ; les tests ne créent aucun montage réel. La primitive de renommage injectable reproduit EXDEV sans déplacer les données sur un autre volume.

Les cas `fast_bind_mounts` utilisent une table de montages simulée correspondant à deux bind mounts et à leur vue commune. Ils vérifient la traduction dans les deux sens, l’absence de montage commun, les partitions différentes et les restrictions de lecture seule. Le contrôle d’identité couvre les fichiers existants et les suffixes de destination absents. Aucun privilège de montage n’est nécessaire aux tests unitaires.

Les cas `fast_tree` vérifient aussi le renommage du dossier racine (inode et permissions conservés), les dossiers vides, plusieurs racines, les pauses ciblées, les restaurations partielles, les fichiers extérieurs ajoutés, les contenus modifiés et la récupération des journaux avant/après le renommage atomique. Les sous-montages, y compris ceux masqués dans la vue commune, empêchent le raccourci en bloc.

## Reprise après interruption

```bash
cargo test --locked resume_
python3 scripts/test_resume_integration.py
```

Le test d’intégration de reprise est aussi exécuté par `make test-integration` et `make test`. Il utilise le binaire de développement compilé, une configuration et une socket privées. Il crée un fichier de 256 Mio sous `target`, tue uniquement son propre processus enfant après un point durable, puis relance Gad sur le même journal. Il vérifie le point repris, l’inode du temporaire publié et les contenus MD5. Aucun transfert de la session utilisateur n’est interrompu.

Les tests Rust couvrent aussi les temporaires corrompus, remplacés, trop courts ou symboliques, les changements de source malgré une date rétablie, les destinations modifiées, les pauses conservées, la reprise manuelle après retour d’un volume, les annulations volontaires, les phases de publication ambiguës, les permissions et le déplacement avec MD5 puis remise en état.

La suite couvre également les points à zéro et à la fin du fichier, les empreintes invalides ou discordantes, les liens physiques du temporaire, les chemins de temporaire étrangers, les destinations créées pendant l’arrêt, le redémarrage avant le premier point, le refus d’une partie écrite sans point durable, la validation atomique des identifiants, l’historique désactivé et les journaux anciens sans option de reprise.

## Destinations distantes

Les tests Qt simulent KIO Fuse sur un bus privé : résolution FTP/SFTP/SMB/KDE Connect, source et destination distantes simultanées, adresses encodées, mots de passe retirés, version minimale, contexte Dolphin dossier/fichier, panne et annulation pendant la préparation. Un test Rust contrôle la persistance de l’origine de destination et la remise en état.

Pour tester un vrai serveur FTP local et un vrai montage KIO Fuse :

```bash
make test-remote
```

Ce test optionnel exige `pyftpdlib` (`python3-pyftpdlib`), `kio-fuse`, `fusermount3` et un accès à `/dev/fuse`. Il démarre son propre bus D-Bus et son serveur sur `127.0.0.1` avec un port libre, une arborescence et des identifiants temporaires. Il vérifie la CLI, MD5, les dossiers, le déplacement, le remplacement et leurs remises en état, ainsi que l’absence de mot de passe dans le journal. Il ne contacte ni serveur réel externe ni montage de la session utilisateur. SFTP, SMB et KDE Connect sont couverts par l’adaptateur simulé ; ces protocoles ne sont pas attestés sur un serveur réel par ce test FTP.

Un test de régression répète aussi 200 fermetures avec des travailleurs devenant inactifs, avec une échéance explicite pour détecter un réveil perdu.

## Vérification des propriétés

```bash
cargo test --locked properties_
```

Les tests couvrent les UID/GID/modes, ACL d’accès et par défaut, attributs binaires manquants/modifiés/supplémentaires, refus de lecture, limites de mémoire et reprises ERANGE. Des ACL POSIX et xattr réels sont créés uniquement dans les répertoires privés de test. Les scénarios moteur vérifient la copie avec avertissement, la protection de l’original et de la destination d’un déplacement, les renommages rapides de fichier et d’arbre, les liens sans suivi de leur cible, la persistance et la reprise après interruption. Les tests Qt contrôlent la case, le résumé du lot, l’icône d’avertissement et la stabilité de l’infobulle. L’intégration exerce `--properties` ; `make test-remote` vérifie aussi le contrôle incomplet sur FTP/KIO Fuse et la conservation de l’original en déplacement.

## Arborescence des transferts

La suite Qt `row_actions` couvre aussi l’imbrication des dossiers, les noms courts et chemins complets, la stabilité des items/sélections/infobulles, les indices des feuilles profondes, la déduplication parent/enfant, les ouvertures et pauses, les filtres avec contexte, le repli à la fin d’un dossier, les réanalyses raccourcies, les racines indépendantes, la récupération, la navigation vers les conflits et la réactivation des descendants annulés.

Les tests moteur vérifient aussi que la pause, l’annulation, le retrait et l’état final d’un dossier ne concernent que sa branche de destination, même lorsqu’un de ses fichiers a été ajouté séparément. Les tests Qt couvrent les descendants ajoutés tardivement et les parents présents plus loin dans l’historique.

## Association Ctrl+V dans Dolphin

Les tests Python vérifient activation, restauration ciblée, idempotence, réinstallation, désinstallation, raccourcis personnalisés, conflits (y compris composés), validation des deux emplacements avant modification, refus des reçus invalides et retours arrière après erreur d’écriture.

```bash
make test-dolphin-paste
```

Ce test optionnel nécessite CMake, Qt 6 Widgets/Test et les fichiers de développement de KF6 XmlGui (`libkf6xmlgui-dev`). Il utilise les vrais KXMLGUI/KActionCollection, avec HOME/XDG, session D-Bus et presse-papiers Qt hors écran privés. Il contrôle Ctrl+V vers Gad et la libération du raccourci secondaire, Maj+Inser vers KIO, le collage natif d’un champ de texte et le retour à Ctrl+V natif après désactivation. Les actions sont des compteurs : aucune copie réelle n’est déclenchée. Il ne redémarre pas le Dolphin de l’utilisateur.

La régression de destination Dolphin est couverte dans `clipboard` par un appel D-Bus injectable : fenêtre active unique, alias de service, chemin avec espaces et dièse, titre court, dossier fermé, changement de navigation/focus, panne et URL distante. Les tests Python vérifient la migration de l’ancienne action fondée sur `%U` vers l’action de dossier actif, ainsi que sa restauration. Une validation isolée et mise en pause du cas réel a confirmé les cinq fichiers analysés et la destination correcte sans lancer de copie.

La régression de pause depuis la notification est couverte par un double moteur qui applique les commandes : pause et reprise natives, libellé du bouton, infobulle du systray, reprise depuis le bouton ou le menu et commande d’annulation distincte. Le bus KDE privé vérifie aussi le retour de la propriété `suspended` à faux sans créer une seconde notification.

Le contrôle d’espace couvre les budgets partagés entre lots, les volumes indépendants, l’arrondi aux blocs, les dépassements numériques, les temporaires déjà alloués sur le bon volume, la persistance de l’option et les déplacements rapides. Un fichier creux dans une fixture tmpfs provoque une insuffisance sans remplir le disque : aucun dossier cible n’est créé. Une origine distante simulée vérifie que la capacité inconnue laisse terminer le transfert avec un avertissement. Les tests Qt couvrent la case, sa transmission, le protocole 14 et l’affichage persistant de l’avertissement sur un lot OK.

Les priorités sont testées par le moteur réel : choix du prochain travailleur, ordre multiple stable, conservation des pauses, validation atomique des sélections périmées et restitution des fichiers remplacés après inversion de deux lots. Les tests Qt couvrent le menu, la déduplication par lot, les bornes et états non déplaçables, puis une réponse différentielle ne contenant que l’ordre : fichiers, sélection, branches ouvertes et infobulles restent conservés. L’intégration IPC déplace deux lots sous pause puis les replace avant de poursuivre les copies.

La vue sans en-têtes est vérifiée avec les vrais widgets : couleurs communes par demande et adaptation au thème, sélection d’une demande entière, glisser-déposer interne par références stables, refus d’un autre widget, conservation de la parenté et des chemins. Le moteur teste l’alternance réelle entre racines de demandes indépendantes, l’ordre des frères observable par les conflits, le refus de reparentage et d’indices périmés, la conservation de la priorité pendant l’analyse, l’émission différentielle de l’ordre et les dépendances de remplacement, y compris pendant une pause ou une finalisation sans écriture.

Les objets natifs Qt sont mis en cache dans `target/qt-test-cache`, selon le contenu des sources/en-têtes, les versions Qt/KDE et l’identité du compilateur. Un verrou de construction empêche deux compilations concurrentes du même cache. Les exécutables, tests, données et sessions D-Bus restent isolés dans des répertoires temporaires à chaque exécution.

Les dépendances de destination couvrent aussi les parents normalisés, les alias de montages simulés, les noms homonymes au sein d’une demande et leur pause sélective. Le test de suffixe vérifie que « Garder les deux » ne prend pas un nom déjà prévu par une autre entrée déplacée plus loin. Les résumés d’avertissements de propriétés des dossiers sont limités à leurs descendants réels.

## Doublons de transfert

Les tests Rust `duplicates_` vérifient les collages répétés sous pause, les analyses simultanées, les doublons dans une sélection, les dossiers partiellement recoupés, les alias de parents et les indices d’une file restaurée. Ils préservent les destinations distinctes, la différence copie/déplacement, les liens physiques à noms distincts et les nouvelles demandes après annulation ou achèvement. Le test Qt vérifie le message temporaire lors des mises à jour de métadonnées et son absence de répétition. Les scénarios de conflits utilisent des sources distinctes portant le même nom pour rester indépendants du filtrage.

## Limitation de débit

Les tests de `engine::rate` utilisent une horloge injectée pour vérifier les plafonds global et individuel, leur consommation agrégée, la borne des crédits après une pause et les changements à chaud. Les tests moteur vérifient une vraie copie limitée puis débloquée, la pause et l’annulation pendant l’attente, la validation atomique et la persistance. Le test Qt vérifie la conversion Mio/s vers octets/s et la déduplication des demandes ciblées par plusieurs lignes, même si la sélection change pendant le dialogue.

## Débit et temps restant par demande

Quatre tests Qt déterministes couvrent les débits et durées indépendants de deux demandes, les phases copie/MD5, les pauses et conflits, les renommages, le préfixe de reprise, les compteurs réinitialisés, une attente sans progression et les formats de durée. Le vrai récapitulatif est également vérifié avec une réponse `poll.unchanged`, sans remplacer les lignes ni leur sélection et leurs infobulles.

## Reconnexion des périphériques

Les tests `reconnect_` simulent le retrait d’un ancrage par renommage de répertoires temporaires : reprise d’un préfixe durable vérifié, respect des pauses, absence d’écriture dans un dossier de remplacement, refus d’une source remplacée, annulation et libération du travailleur pour une autre demande. Ils couvrent le redémarrage d’un temporaire sans point durable, le refus de rejouer une publication et le remappage limité des numéros de périphériques avec conservation des versions et empreintes. Aucun disque réel n’est démonté. Les tests Qt couvrent les libellés, l’annulation, le suivi natif suspendu et une nouvelle résolution KIO sans ajout de copie ni ouverture de fenêtre sur bus privé.

## Bilan des erreurs et réessais

Les tests `errors_` vérifient le rapport en lecture seule, les erreurs d’analyse et de finalisation, la validation atomique, les indices périmés, les références exactes du bilan, la réparation d’une préparation échouée et la protection d’un résultat publié modifié. Une copie réelle partiellement échouée vérifie que les octets, l’inode et le journal du fichier OK restent identiques après réessai ; la pause globale et les lignes annulées/ignorées/en attente sont préservées. Les tests Qt vérifient le filtrage, le ciblage après tri, les actions du menu, le bilan vide, l’encodage UTF-8/JSON et l’écriture atomique. Aucun export n’est écrit dans le dossier personnel pendant les tests.

## Documentation multilingue

Les cas de `tests/python/test_documentation.py` utilisent seulement la bibliothèque standard : catalogue complet des six langues et des trois types de guide, navigation et bascule de langue par public, papier A4/US Letter, liens relatifs et fragments, couverture/légendes localisées, images embarquées, titre absent et génération des 18 guides. Markdown et WeasyPrint sont simulés pour ces tests.

`make docs` effectue le rendu réel, renouvelle les schémas modifiés et vérifie les liens des README/guides/tests, les 84 captures, les langues HTML et les 36 exports. Les dépendances facultatives sont indiquées dans [l’index documentaire](docs/README.md). Le test Qt du bilan vérifie aussi les icônes des six boutons avec un thème volontairement absent pour contrôler leur repli standard.

## Traductions de l’application

Cinq tests Python vérifient la couverture des textes Qt marqués, les empreintes TS/QM, l’exclusion des commentaires et clés non marquées, les traductions manquantes et la conservation des paramètres et liens. Les tests Qt chargent le vrai catalogue FR embarqué, contrôlent les widgets et états, la préférence de langue, le bilan texte et la conservation des chemins et JSON. L’export vide et ses transitions après actualisation et filtrage sont aussi testés. `make translations-check` permet le contrôle indépendant.

Les tests Qt de langue à chaud alternent français et US avec un transfert en cours et une réponse poll inchangée. Ils contrôlent l’identité des lignes, la sélection, le développement des dossiers, les champs destination/débit, les dialogues À propos/bilan/erreur/limite de débit, les choix de conflit, la stabilité du nombre de liaisons et l’absence de commandes moteur d’écriture.

## Exclusions et éléments cachés

Les tests Rust couvrent les motifs Unicode, la validation atomique, les valeurs historiques par défaut, l’exclusion de dossiers entiers, les fichiers temporaires, les liens symboliques, la persistance des règles par lot, les réessais, une sélection entièrement exclue et les déplacements suivis d’une restauration. Les tests Qt vérifient les préférences, l’annulation et le rejet des modifications, la traduction à chaud et la capture des règles des nouvelles demandes.

```sh
cargo test --locked exclusions
python3 scripts/test_qt.py --suite row_actions
```

## Intégration Konqueror

`python3 scripts/test_qt.py --suite konqueror` vérifie le dossier courant, les URL distantes et encodées, la conservation du mode couper, le collage de texte et Maj+Inser dans un environnement privé. Le module a aussi été chargé dans Konqueror 26.08.2 extrait dans un dossier temporaire, avec HOME/XDG et bus D-Bus privés ; l’URL exacte du dossier actif a été vérifiée. Les journaux de cet essai local sont dans `target/konqueror-real-test.log`.

## Boîte des paramètres {#menu-parametres}

Les tests Qt contrôlent les minima des fenêtres, les deux onglets, Fermer et Échap, la conservation des widgets, la traduction à chaud et le bouton de débit dans la barre d’actions. La CLI est testée pour les valeurs invalides, les options opposées, les exclusions partielles et la synchronisation des préférences. Les tests Python vérifient les six manpages, la couverture des six langues, leur installation et le refus des traductions manquantes.

```sh
make manpages-check
make test-cli-settings
```

## Surveillance automatique de Ctrl+V {#dolphin-watchdog}

Les tests Qt du module couvrent la recréation des actions, le minuteur réel, la désactivation, la restauration du collage natif, les changements utilisateur, les dialogues modaux, les reçus invalides et les actions manquantes. Le collage de texte reste natif. Les tests Python vérifient la protection réversible du lanceur, ses arguments et les personnalisations ultérieures. L’essai avec Dolphin réel provoque un rafraîchissement des menus puis appuie sur Ctrl+V : une commande factice enregistre le lancement, sans copie réelle ni accès au bureau.

```sh
make test-dolphin-watchdog
make test-dolphin-watchdog-real
```
