# Gad — Optimisations des transferts

Mesures locales du 9 novembre 2026, binaire optimisé et système de fichiers ext4 du projet.

## Résultats

| Situation | Avant | Après |
| --- | ---: | ---: |
| 5 000 fichiers, fenêtre masquée | 5.08 s | 1.06 s |
| 5 000 fichiers, fenêtre affichée hors écran | 5.12 s | 1.63 s |
| 1 000 fichiers dans 1 000 sous-dossiers, fenêtre masquée | 1.70 s | 0.61 s |
| Petit transfert après un historique de 5 000 fichiers, fenêtre affichée | 1.01 s | 0.31 s |

Ces durées mesurent la fin observée par la socket Qt, après finalisation du moteur. Le changement physique de chemin des 5 000 fichiers passe de 613 à 515 ms en arrière-plan. Le renommage POSIX seul reste voisin de 0,01 ms : Gad parcourt les entrées pour suivre leur état et préparer leur restauration.

## Changements appliqués

1. Construction des nouvelles branches Qt hors du modèle, puis insertion groupée. Les objets existants, sélections et états de repli sont conservés.
2. Rendu détaillé différé tant que la fenêtre est masquée ou minimisée. Compteurs, commandes, notifications et moteur restent actifs ; la vue rattrape les changements à l’ouverture.
3. Renumérotation indépendante du rendu détaillé des anciens lots. Les ajouts et reprises ne marquent plus un ancien lot comme modifié si son option de démarrage est déjà correcte.
4. Caches limités à une préparation pour les parents, périphériques et montages ; réutilisation immédiate des métadonnées pour les empreintes. Les contrôles avant publication restent frais.
5. Index des dossiers et agrégation de leurs états, sans rescanner toutes les entrées pour chaque dossier. Un test compare 500 configurations à l’ancien algorithme.
6. Journal durable des seuls lots modifiés dans `state.json.wal`, avec instantané, séquences, contrôle de corruption et compaction récupérable. Sérialisation directe, sans arbre JSON intermédiaire.
7. Réglage du rendu visible : suspension temporaire de la largeur automatique et de la peinture, expansion uniquement des nouveaux dossiers après remplissage. Ce point supplémentaire corrige une régression détectée par les mesures visibles.

## Vérifications

- Rust : 284 tests réussis et un benchmark volontairement ignoré, en exécution séquentielle optimisée et en exécution parallèle de développement.
- Qt : presse-papiers, notifications, 138 tests de lignes/actions, 5 tests Konqueror et 9 scénarios d’interface réussis.
- Python : 102 tests réussis. Formatage, Clippy sans avertissement, catalogues de traduction, manpages et documentation vérifiés.
- Intégration réelle : copie, déplacement, MD5, restauration, paramètres CLI et reprise du même temporaire après SIGKILL réussis.
- Journal : modifications/suppressions/ordre des lots, ligne finale tronquée, corruption complète, séquence manquante ou répétée, compaction interrompue, permissions, refus des liens en écriture et lecture des anciens instantanés testés.

Sur le scénario tracé de 1 000 fichiers, les deux versions effectuent un seul `renameat2`. Les accès à `/proc/self/fdinfo` passent de 3 023 à 1 020, et les appels `openat` de 9 124 à 2 642. Ces compteurs couvrent aussi le démarrage et la fermeture de l’instance privée ; les durées de référence restent celles sans trace.

## Méthode et reproduction

Trois essais par taille, médianes ci-dessus. Instances, bus D-Bus, préférences, journal et fichiers entièrement privés ; aucun transfert personnel piloté. Chaque fichier de démonstration contient neuf octets. L’historique est conservé ; MD5 et vérifications facultatives sont désactivés uniquement dans ces instances. Le programme est déjà lancé quand la mesure commence. Le mode visible utilise le backend Qt hors écran et ne mesure pas les performances du compositeur KDE. Les chiffres varient selon la machine, les disques et la charge.

```bash
cargo build --release --locked
dbus-run-session -- python3 scripts/benchmark_rename.py --binary target/release/gad --label essai-
dbus-run-session -- python3 scripts/benchmark_rename.py --binary target/release/gad --label visible- --visible
```

Le script mesure aussi un nouveau processus `kioclient`, dont le démarrage ne représente pas exactement un geste dans une fenêtre Dolphin déjà ouverte. Une option `--trace` nécessite strace ; ses durées ne servent pas de référence. Les JSON et traces sont conservés sous `target/rename-performance/`.

Le journal durable associe désormais `state.json` et `state.json.wal` pendant l’exécution. Le [guide développeur](GUIDE_DEVELOPPEUR.md#journal-et-recuperation) décrit les garanties de reprise et la copie cohérente du journal.


## Deuxième passe : journal et démarrage

La relecture parcourt le WAL par morceaux de 64 Kio et décode les lots directement dans les types Rust. Elle évite les objets JSON intermédiaires de chaque entrée et la copie des lots de l’instantané. Les sauvegardes inchangées ne déclenchent plus une compaction au seul motif que le seuil est atteint. Les instantanés explicites de fermeture restent synchronisés. La lecture refuse aussi les liens finaux et fichiers non ordinaires, sans les modifier.

Mesures sur trois redémarrages privés d’un historique synthétique terminé de 5 000 fichiers :

| Situation | Avant cette passe | Après |
| --- | ---: | ---: |
| Journal incrémental de 60 Mo : démarrage | 620 ms | 540 ms |
| Même cas : pic RSS du processus | 123.5 Mio | 101.6 Mio |
| Journal incrémental de 24 Mo : démarrage | 413 ms | 417 ms |

Le pic RSS inclut le démarrage Qt et les premières requêtes de statut, et pas uniquement le décodeur du journal. Pour 24 Mo, le pic global reste proche de 102 Mio : aucun gain mémoire global n’est revendiqué sur ce cas. Les lots synthétiques sont terminés et ne déclenchent ni transfert ni restauration.

```bash
dbus-run-session -- python3 scripts/benchmark_startup.py --binary target/release/gad --records 35
```

La passe ne modifie pas la méthode de copie par fichier. Aucun gain sur le débit des copies classiques n’est revendiqué. Le coût des étapes durables par fichier reste présent et les garanties de synchronisation n’ont pas été allégées. Pour le journal de 24 Mo, le temps de démarrage reste lui aussi sensiblement inchangé.

Validation : 287 tests Rust réussis, un benchmark ignoré, 103 tests Python réussis, suites Qt et tests d’intégration réels réussis, y compris la reprise du même temporaire après SIGKILL. Clippy, formatage, traductions, manpages et documentation vérifiés. Trois tests Rust supplémentaires couvrent la compaction sans mutation, les instantanés mal formés et le refus des liens en lecture. Un test instable de restauration attend désormais la fin du lot, au lieu de relancer une restauration dès qu’une feuille est remise en place.

Une séparation de la capture du journal et de ses écritures a été évaluée puis retirée. Elle rendait le verrou du moteur disponible pendant les E/S, mais les mesures alternées de renommage n’ont pas montré un gain stable et les requêtes de suivi pouvaient concurrencer davantage le transfert. Cette modification et ses tests spécifiques ne font pas partie du résultat conservé.


## Troisième passe : deltas dans les lots actifs — 10 novembre 2026

Le WAL ne réécrit plus systématiquement un lot actif entier lorsqu’un seul fichier change. Le rédacteur garde une base JSON exacte et écrit un remplacement limité (`offset`, `remove`, `insert`) lorsque celui-ci occupe moins de la moitié d’un objet complet. Il vérifie les empreintes de base et de résultat et conserve les mêmes fsync avant chaque effet de fichier. Les modifications larges restent écrites en entier.

Les caches de texte JSON sont limités à 8 Mio par lot et 16 Mio au total. Les lots trop grands utilisent le chemin complet, sans retenter une double sérialisation à chaque sauvegarde. Les bases en mémoire ne sont remplacées qu’après écriture durable réussie et sont réinitialisées à chaque compaction. Les empreintes des bases complètes ne sont calculées que si un delta en a besoin.

Trois essais alternés de chaque binaire, avec 100 fichiers de neuf octets, copie réelle, historique conservé et vérifications facultatives désactivées. Les sources sont conservées et tous les contenus destination sont comparés après chaque essai.

| Mesure | Avant cette passe | Après |
| --- | ---: | ---: |
| Copie de 100 petits fichiers, fin observée par IPC | 2.93 s | 1.78 s |
| Segment WAL restant en fin de copie | 7.45 Mo | 0.20 Mo |
| Renommage de 5 000 fichiers, fin observée par IPC | 1.08 s | 1.02 s |
| Démarrage avec un ancien WAL de 60 Mo | 462 ms | 411 ms |

Le gain de copie est d’environ 39 % dans ce scénario local. Le segment WAL restant ne représente pas les écritures cumulées : des instantanés sont aussi produits lors des compactions. Aucun gain uniforme n’est promis pour les gros fichiers, les lots hors cache, les réseaux ou tous les supports.

```bash
dbus-run-session -- python3 scripts/benchmark_copy.py --binary target/release/gad --files 100
# Avec un ancien binaire conservé : ajouter --compare /chemin/ancien/gad
```

Le format WAL 2 contient `jobs: null`, `full_jobs` et `patches`. Les anciens WAL restent lus, mais un ancien lecteur refuse volontairement les nouveaux enregistrements. Quitter normalement la nouvelle version produit un instantané complet et vide le WAL avant un retour à une ancienne version. Le format des instantanés et les commandes IPC ne changent pas.

Validation : 295 tests Rust réussis en mode normal et optimisé, 104 tests Python, toutes les suites Qt, intégration réelle, paramètres CLI et Clippy. La reprise après SIGKILL a aussi été vérifiée avec accents, emoji, guillemet et saut de ligne dans le nom du fichier, en réutilisant le même temporaire et en comparant les contenus par MD5. Les nouveaux tests couvrent les bornes UTF-8, les empreintes et identités incorrectes, les bases manquantes, les compactions, une écriture refusée, les limites du cache et la conservation du JSON exact dans le lecteur Python.

## Quatrième passe : index temporaire des dossiers — 10 novembre 2026

Les relations entre dossiers et descendants ne sont plus reconstruites après chaque fichier. Un index par lot actif est réutilisé tant que les chemins source/destination, les types et l’ordre des entrées restent identiques. Les états restent recalculés à chaque appel : les erreurs, annulations et reprises ne sont pas mis en cache. Tout changement structurel invalide l’index.

Le cache est limité à 16 384 entrées et environ 8 Mio par lot actif. Au-delà, le calcul utilise un index temporaire. Les clones et la sérialisation l’excluent ; la libération du travailleur le supprime sans rendre la révision du lot obsolète. Les lots de fichiers seuls ne conservent pas d’index.

| Mesure médiane sur trois essais | Avant | Après |
| --- | ---: | ---: |
| 200 recalculs, arbre de 5 101 entrées | 735,633 ms | 17,360 ms |
| Copie complète de 100 petits fichiers dans 20 dossiers | 1,886 s | 1,930 s |

Le calcul isolé est environ 42 fois plus rapide. Le petit transfert complet ne montre pas de gain global : les coûts durables par fichier restent dominants et les écarts sont du même ordre que la dispersion des essais. Il ne faut pas appliquer ce facteur au temps de copie total.

```bash
cargo test --release --locked benchmark_directory_completion -- --ignored --nocapture
dbus-run-session -- python3 scripts/benchmark_copy.py --binary target/release/gad --compare /chemin/ancien/gad --files 100 --folders 20
```

Les tests comparent les résultats à un algorithme de référence avec changements d’état, de chemins, de type, d’ordre et de nombre d’entrées. Ils couvrent aussi le dépassement du budget, l’absence du cache dans les clones/JSON et sa libération sans changement de révision. Le générateur des benchmarks vérifie désormais les répartitions non divisibles et les dossiers vides.

## Cinquième passe : comparaison des deltas par blocs — 10 novembre 2026

Le calcul du préfixe et du suffixe communs de deux lots JSON utilise désormais des comparaisons de tranches de 64 octets. Seul le premier bloc différent est parcouru octet par octet. Le code Rust reste sûr, sans allocation supplémentaire ni dépendance à une architecture CPU. Les ajustements aux frontières UTF-8, les empreintes, le format WAL 2 et les fsync restent identiques.

Pour 100 calculs de delta sur deux JSON d’environ 4 Mio ne différant que d’un caractère, la médiane de trois essais passe de **183,969 ms à 15,858 ms**, soit un facteur d’environ **11,6**. Les empreintes sont préchargées dans ce benchmark pour isoler la comparaison. Un vrai changement doit encore être sérialisé et son empreinte calculée ; ce facteur ne représente donc ni le temps total de journalisation ni celui du transfert.

```bash
cargo test --release --locked benchmark_delta_comparison -- --ignored --nocapture
```

Deux tests supplémentaires comparent les préfixes/suffixes à une référence scalaire aux frontières des blocs, avec longueurs différentes et tranches vides, puis vérifient les deltas de bout en bout avec suppressions, Unicode et caractères échappés autour de ces frontières.

Copie réelle de 300 fichiers de neuf octets dans cinq dossiers, trois essais alternés par binaire : médiane **7.014 s → 6.438 s**, soit environ **8.2 %** de réduction dans ce scénario local. Les contenus destination et la conservation des sources sont contrôlés après chaque essai. Cette courte série ne garantit pas un gain identique sur tous les supports.

Validation : 300 tests Rust réussis en mode normal et optimisé, 105 tests Python, suites Qt et tests d’intégration, dont reprise du même temporaire après SIGKILL et contrôle MD5. Clippy, formatage, documentation, traductions et manpages vérifiés.

## Sixième passe : cache de l’en-tête du journal — 10 novembre 2026

Les réglages persistants et l’ordre personnalisé des lignes disposent désormais d’une représentation typée et d’un JSON déjà sérialisé dans `engine/journal/settings.rs`. Une sauvegarde compare les valeurs actuelles sans reconstruire leur arbre JSON. Le changement d’un réglage ou de l’ordre régénère le cache. Il n’est remplacé qu’après écriture durable réussie ; une sauvegarde refusée conserve le dernier cache validé. Chaque enregistrement WAL 2 contient toujours l’en-tête complet. Les formats, les limites de compaction et les fsync restent inchangés.

Avec un ordre de 10 000 lignes, la médiane de trois essais de 100 sauvegardes sans mutation passe de **165,190 ms à 0,888 ms**. Ce benchmark inclut les contrôles du fichier d’instantané, mais ne déclenche aucune écriture : il mesure la détection d’absence de changement, pas le débit de copie. Les lots actifs profitent également du JSON réutilisé quand seuls leurs fichiers changent.

```bash
cargo test --release --locked benchmark_unchanged_header -- --ignored --nocapture
dbus-run-session -- python3 scripts/benchmark_copy.py --binary target/release/gad --compare /chemin/ancien/gad --files 300 --folders 5 --reorder
```

Les nouveaux tests confrontent les réglages mis en cache au sérialiseur complet de State, changent chaque réglage individuellement, vérifient la réutilisation lors d’une progression de lot et le rafraîchissement après réordonnancement, puis simulent une écriture refusée et une nouvelle tentative. Validation : 303 tests Rust réussis en mode normal et optimisé, 105 tests Python, suites Qt, intégration et paramètres CLI. La reprise du même temporaire après SIGKILL reste vérifiée par MD5. Clippy, formatage, traductions, manpages et documentation dans les six langues vérifiés.

Mesures de copie réelle, 300 fichiers de neuf octets dans cinq dossiers, trois essais alternés par version :
| Scénario | Médiane avant | Médiane après |
| --- | ---: | ---: |
| Ordre initial | 6.576 s | 6.593 s |
| Ordre personnalisé | 6.150 s | 5.838 s |

Le scénario avec ordre personnalisé montre environ 5 % de réduction de la médiane locale. Le scénario classique est sensiblement inchangé. La dispersion et le nombre limité d’essais ne permettent pas de garantir ces gains sur d’autres supports. Chaque copie vérifie les contenus et la conservation des sources. Le scénario personnalisé inclut sa préparation et le changement d’ordre avant démarrage.

## Septième passe : dépendances indexées par branche — 10 novembre 2026

Le graphe des destinations dans `engine/row_queue.rs` stocke directement les branches racines concernées, au lieu de conserver leurs feuilles puis de les reconvertir. Les relations sont dédoublonnées par les ensembles de sortie : le calcul ne crée plus un ensemble temporaire de feuilles pour chaque fichier. Une destination normalisée identique à la destination d’origine n’est plus traitée deux fois.

Un chemin associé à une seule branche conserve sa clé directement, sans allocation d’un ensemble haché. Dès qu’il concerne plusieurs branches, un ensemble haché est créé pour conserver une recherche efficace en présence de collisions nombreuses. Les contraintes de priorité entre demandes et les prédécesseurs des destinations répétées restent inchangés.

Médianes de trois essais, chacun reconstruisant cinq fois le graphe de deux lots de 5 001 entrées. Les données et la correspondance des racines sont préparées hors chronométrage.

| Destinations | Avant | Index par branche | Avec clé unique sans allocation |
| --- | ---: | ---: | ---: |
| Distinctes | 23,668 ms | 20,827 ms | 16,265 ms |
| Partagées | 28,853 ms | 20,701 ms | 20,894 ms |

Le résultat final réduit le temps de calcul isolé d’environ 28 à 31 %. La seconde étape bénéficie surtout au cas distinct ; son écart dans le cas partagé est faible. Ce calcul intervient lors de la reconstruction du plan, pas à chaque octet copié.

```bash
cargo test --release --locked benchmark_dependency_graph -- --ignored --nocapture
# Mesures complètes de renommage avec un binaire donné :
dbus-run-session -- python3 scripts/benchmark_rename.py --binary target/release/gad --label pass7-
```

Une comparaison locale alternée de deux binaires, trois essais par scénario, conserve les inodes des dossiers déplacés et vérifie tous les contenus après transfert :

| Scénario | Fin observée par IPC avant | Après |
| --- | ---: | ---: |
| 5 000 fichiers, un sous-dossier | 1 014,712 ms | 969,479 ms |
| 1 000 fichiers, 1 000 sous-dossiers | 591,221 ms | 581,507 ms |

Ces durées comprennent l’analyse, le renommage et la finalisation de Gad. Elles ne sont pas le coût du seul appel système `rename`. Le premier scénario montre environ 4 % de réduction dans cette courte série locale ; le second varie davantage et aucun gain uniforme n’est garanti.

Validation : 305 tests Rust réussis en mode normal et optimisé, 105 tests Python, suites Qt, intégration et paramètres CLI. Deux nouveaux tests confrontent le graphe à l’ancien algorithme sur 500 configurations reproductibles avec alias, plusieurs branches, types différents, clés normalisées absentes/identiques/partielles et destinations répétées ; ils vérifient aussi tous les prédécesseurs de quatre branches visant la même destination. Les tests de réordonnancement et de restauration ainsi que la reprise après SIGKILL restent réussis. Clippy, formatage, documentation, traductions et manpages vérifiés.


## Passes 8 à 14 : boucle sur les chemins chauds — 10 novembre 2026

Sept passes ont été réalisées avec tests Rust après chaque changement, puis une validation complète. Elles conservent les formats du journal, les écritures durables, les plafonds des blocs pour les gros fichiers et les contrôles d’identité des fichiers.

- **Temporaires** : le candidat usuel est vérifié directement dans la file, en comparant d’abord son nom. L’index complet des noms réservés n’est construit qu’en cas de collision. Les fichiers existants et liens restent protégés par `create_new`.
- **Tri** : les noms sont mémorisés pendant le tri des listes mélangées. Les listes déjà triées et strictement inversées utilisent des parcours simples ; les listes de moins de deux éléments sont ignorées. Les noms égaux conservent leur ordre.
- **Exclusions** : les noms littéraux, motifs sans étoile et motifs à étoile unique ont des chemins directs. Le cas général Unicode garde une programmation dynamique bornée, avec une seule ligne réutilisée en place.
- **Arborescence** : les accès au cache comparent la forme sans construire une nouvelle liste. Un essai de chemins empruntés pendant la reconstruction a été retiré, faute de gain stable. La reconstruction complète reste sensiblement inchangée.
- **Pause et annulation** : une seule recherche du lot suffit au contrôle initial. L’annulation reste prioritaire sur les pauses ; les identifiants et indices absents sont refusés.
- **Tampons** : leur taille dépend de la quantité à lire, avec un minimum d’un octet pour ne pas confondre une lecture vide avec une vraie fin de fichier. Les maximums restent de 64 Kio pour copier et 1 Mio pour MD5 et la vérification d’une reprise. MD5 interrompt aussi immédiatement la lecture si la source grandit au-delà de sa taille initiale.
- **Montages** : l’identification KIO Fuse n’alloue plus de descriptions pour tous les montages. Elle filtre le type et l’identifiant recherchés, puis garde la validation complète d’un enregistrement positif. Aucun cache persistant des montages n’est ajouté.

Médianes de trois essais isolés. Les temps de tri et d’analyse de montages excluent respectivement la collecte des fichiers et la lecture de `/proc`. L’allocation initiale des données est hors chronométrage.

| Mesure ciblée | Avant | Après |
| --- | ---: | ---: |
| Temporaires : 100 créations avec 5 000 fichiers planifiés | 47.008 ms | 10.765 ms |
| Tri : dix listes mélangées de 5 000 noms | 15.764 ms | 4.397 ms |
| Exclusions : 100 000 motifs *.tmp | 26.948 ms | 0.974 ms |
| Cache de l’arbre : 1 000 accès, 2 000 lots | 3.263 ms | 2.007 ms |
| Contrôles de pause : 1 000 appels, 5 000 lots | 4.686 ms | 2.269 ms |
| MD5 : 1 000 calculs réels sur un fichier de neuf octets | 58.138 ms | 6.174 ms |
| Montages : 1 000 analyses de 128 lignes sans KIO | 63.120 ms | 0.232 ms |

Ces facteurs ne sont pas ceux d’un transfert complet. La comparaison initiale de 300 petits fichiers dans cinq dossiers, trois essais par version, reste proche : 7,245 s avant et 7,341 s après sans MD5 ; 7,409 s avant et 7,351 s après avec MD5. Une seconde série de quatre essais par version, avec ordre de passage contrebalancé, donne **7,337 s avant et 7,324 s après** sans MD5. Aucun gain global significatif n’est donc démontré sur ce petit lot. Les sources, contenus copiés et empreintes MD5 demandées sont vérifiés. Les essais de renommage restent eux aussi variables et ne justifient pas de revendication de gain global.

Le benchmark public de copie accepte `--verify-md5` ; avec `--compare`, il inverse l’ordre des deux binaires un essai sur deux pour limiter les effets d’ordre.

```bash
dbus-run-session -- python3 scripts/benchmark_copy.py --binary target/release/gad --compare /chemin/ancien/gad --files 300 --folders 5 --repeat 4 --verify-md5
```

Validation finale : 312 tests Rust réussis en modes normal et optimisé, 13 benchmarks ignorés par défaut, 105 tests Python, suites Qt, intégration réelle et paramètres CLI. Les nouveaux tests couvrent les réservations selon l’état des lots, le tri stable des noms égaux, plus de 40 000 comparaisons de globs avec l’ancien algorithme, toutes les combinaisons de pause/annulation, les fichiers vides et limites de blocs avec MD5, les sources qui grandissent et la classification des montages mal formés ou ambigus. Les reprises après SIGKILL conservent le même temporaire et sont vérifiées par MD5. Clippy, formatage, documentation, traductions et manpages vérifiés.


## Passes 15 à 19 : lecture et encodage du journal — 10 novembre 2026

Quatre optimisations sont conservées après comparaison ; une piste de réutilisation du tampon de rejeu a été retirée faute de gain stable. Les formats de fichiers, checksums et points de synchronisation ne changent pas.

- `journal/snapshot_reader.rs` extrait la séquence et décode `State` en un seul parcours. Il préserve l’obligation du champ `jobs`, la validation de la séquence, les clés échappées et le refus des champs dupliqués reconnus.
- L’encodage WAL réserve son préfixe de checksum, sérialise et ajoute la somme dans un même tampon. Les tests comparent exactement ses octets à l’ancien résultat, y compris Unicode et erreurs de sérialisation.
- Le lecteur WAL emprunte les blocs JSON à la ligne lue. Seuls les lots retenus sont copiés, et les réglages sont décodés directement dans `State`. Les réglages ambigus avec clés dupliquées sont désormais refusés plutôt que de choisir implicitement la dernière occurrence.
- Lors du rejeu, un en-tête déjà validé identique octet pour octet réutilise les réglages courants. Le cache local est limité à 1 Mio. Tout en-tête plus grand invalide la base précédente et le cache disparaît à la fin du chargement. Tous les enregistrements restent vérifiés par checksum et séquence ; les préfixes déjà couverts par l’instantané gardent leur règle d’ignorance.

Médianes de trois essais isolés, données préparées hors chronométrage :

| Mesure | Avant | Après |
| --- | ---: | ---: |
| Dix chargements d’un instantané de 10 000 entrées | 131.057 ms | 105.933 ms |
| Encodage de 10 000 enregistrements, charge de 256 octets | 10.569 ms | 9.237 ms |
| Encodage de dix enregistrements, charge de 4 Mio | 75.701 ms | 66.675 ms |
| Trois lectures de 30 en-têtes changeants avec 10 000 rangs | 228.461 ms | 84.210 ms |
| Trois lectures de 30 en-têtes identiques avec 10 000 rangs | 85.017 ms | 52.088 ms |

Les deux lignes sur les en-têtes correspondent à deux étapes distinctes : décodage direct de réglages qui changent, puis cache pour des réglages répétés. L’étape de cache ne ralentit pas significativement les en-têtes changeants (84,239 contre 84,229 ms). Un test petit en-tête → grand en-tête non conservé → petit en-tête vérifie l’invalidation correcte de la base.

L’essai retiré consommait la base du delta pour réutiliser son allocation. Le benchmark de trente deltas de 2 Mio passait seulement de 99,009 à 97,825 ms ; ce faible écart ne justifiait pas le changement d’ownership. L’API de rejeu et ses validations restent inchangées. Un test supplémentaire refuse du JSON invalide même si son empreinte de résultat concorde.

Le benchmark public de démarrage accepte `--records 0` (instantané seul), `--wal-format delta`, `--custom-order` et `--compare`. Les versions sont contrebalancées. Les fixtures ne contiennent que des entrées terminées sans reçus de restauration ; les offsets UTF-8 des deltas et l’état final sont vérifiés dans les tests Python.

Comparaisons de quatre essais par version : les deux premiers scénarios proviennent de la série avant ajout du cache de réglages ; le dernier utilise le binaire final.

| Démarrage observé jusqu’au premier état IPC | Avant | Après |
| --- | ---: | ---: |
| Instantané seul, 20 000 entrées | 940.662 ms | 938.269 ms |
| Ancien WAL complet, 5 000 entrées et 14 enregistrements | 358.707 ms | 366.494 ms |
| WAL à deltas final avec ordre personnalisé, 10 000 entrées et 32 enregistrements | 858.933 ms | 797.858 ms |

Le dernier scénario montre environ 7 % de réduction de la médiane. Un essai du nouveau binaire atteint toutefois 1 260 ms (les trois autres : 781 à 803 ms), contre 824 à 901 ms pour l’ancien : cette série reste courte et variable. Les autres scénarios n’établissent pas de gain global significatif. Les pics mémoire de l’application complète sont sensiblement inchangés. Aucun gain sur toutes les copies ou tous les démarrages n’est revendiqué.

```bash
dbus-run-session -- python3 scripts/benchmark_startup.py --binary target/release/gad --compare /chemin/ancien/gad --files 10000 --records 32 --wal-format delta --custom-order --repeat 4
```

Validation : 317 tests Rust réussis en modes normal et optimisé, 17 benchmarks ignorés par défaut, 108 tests Python, suites Qt, paramètres CLI et intégration réelle. La reprise du même temporaire après SIGKILL reste vérifiée par MD5. Clippy, formatage, traductions, manpages et exports de documentation vérifiés. Le libellé de sélection du menu contextuel devient « Sélectionner le lot entier » ; son changement à la volée français/anglais est testé avec la sélection effective des lignes du lot.
