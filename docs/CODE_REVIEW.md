# Revue transversale du code — 12 novembre 2026

Cette passe élargit les vérifications précédentes du moteur aux scripts de maintenance, à la frontière Rust/Qt et aux intégrations du bureau. La matrice ci-dessous distingue les chemins examinés et les défauts reproduits. Les tests réussis ne constituent pas une preuve d’absence de défaut sur tous les systèmes de fichiers.

## Défauts corrigés

| Composant | Déclencheur et effet avant correction | Correction et régression |
| --- | --- | --- |
| Nettoyage | `debian/` lié vers un dossier extérieur : les sorties portant des noms connus pouvaient être supprimées dans cette destination. | Refuser les candidats dont un parent sous le dépôt est un lien. Test sur une arborescence Git temporaire, en conservant les données extérieures. |
| Désinstallation | Krusader conservait son action et son raccourci vers un binaire supprimé ; le lanceur et le module dédiés à Konqueror restaient installés. | Restaurer Krusader avant le retrait du programme ; retirer les fichiers dédiés à Konqueror. Tests avec intégration Krusader réelle dans des dossiers privés, conservation de l’historique et arrêt avant suppression si la restauration échoue. |
| Client IPC | Une réponse tronquée (`{`), un document JSON sans fin de ligne, un tableau ou du texte invalide pouvaient produire un code de succès. | Exiger une ligne complète contenant un objet JSON valide avant de publier la réponse et de calculer le code de sortie. Quatre cas testés avec un faux serveur Unix et le véritable binaire ; les échanges normaux sont ensuite vérifiés sur une instance isolée. |

La désinstallation a aussi été extraite dans une fonction, avec un point d’entrée explicite : ses tests peuvent importer le module sans retirer les fichiers du compte réel.

## Périmètre examiné

| Ensemble | Points examinés |
| --- | --- |
| Moteur Rust | Commandes et sélections, annulations, ordonnanceur, doublons, ordre des lignes, reprise, restauration, publication, conflits et limites de débit. |
| Persistance | Écritures du journal, séquences, rejeu des deltas, contrôle des bases, états chargés et frontières de sauvegarde. |
| Fichiers et montages | Alias de montage, déplacements rapides, espace disponible, temporaires, propriétés, exclusions et reconnexion. |
| Interface Qt | IPC, collage, résolution KIO, choix des chemins, profils, recherche et tri, conflits, erreurs, diagnostics et fin de lot. |
| Intégrations KDE | Routage Dolphin, surveillance des raccourcis, filtre Konqueror, configuration Krusader et désinstallation. |
| Scripts et distribution | Nettoyage, installation utilisateur/système, ressources, documentation, manpages, tests, build Rust/C++/Qt, règles Debian et workflow CI. |

La revue combine lecture des chemins d’exécution, recherche transversale des opérations sensibles et tests exécutables. Les dépendances tierces, les fichiers générés et les scripts privés d’envoi au PPA ne sont pas présentés comme audités. Aucun envoi ni modification des préférences du bureau réel n’a été effectué.

## Vérification

La validation complète est lancée par :

```sh
python3 scripts/run_tests.py all --keep-going --report-dir target/review-full
```

Les détails par étape, les sorties brutes et le JUnit se trouvent dans `target/review-full/`. Le binaire optimisé est également testé séparément avec `GAD_TEST_BINARY=target/release/gad`, dans des dossiers XDG privés et sur un bus D-Bus isolé pour la configuration.

Les liens de documentation, les 138 captures existantes et les 18 exports HTML sont contrôlés, ainsi que les catalogues et les manpages. Les scripts Python et les deux lanceurs shell sont vérifiés syntaxiquement.

Le montage SSHFS utilisé précédemment n’est pas disponible dans cette session : cette passe ne valide pas de nouveau transfert réel sur ce serveur, ni une reconnexion matérielle réelle. Les tests locaux d’injection et les régressions existantes restent exécutés.

### Résultat final

Les neuf étapes de la validation complète sont réussies : formatage, Clippy, tests Rust, tests Python, tests Qt, surveillance Dolphin, compilation et deux intégrations. Le moteur compte 347 tests réussis et 17 benchmarks ignorés, en mode normal puis en mode optimisé avec un seul thread. Les 117 tests Python réussissent ; les suites Qt comptent 162 tests d’actions, cinq Konqueror, 11 interactions et 11 de surveillance Dolphin, complétés par les scénarios presse-papiers et notifications.

Les intégrations passent aussi sur le binaire optimisé, avec les quatre réponses IPC invalides, la configuration normale, les transferts, les restaurations et la reprise après SIGKILL avec MD5. Après les corrections, la nouvelle passe et les vérifications ci-dessus n’ont pas révélé d’autre défaut confirmé. Aucun commit, publication, envoi au PPA ou arrêt des applications de l’utilisateur n’a été effectué.

## Approfondissement : configuration combinée et lanceurs

### Configuration avant effets externes

Une demande peut combiner Krusader, Dolphin, une langue, des règles ou un débit. Les chemins asynchrones contrôlaient certaines options après la première modification de raccourci. Les tests de régression ont reproduit dix refus tardifs ou acceptations de valeurs invalides : langue inconnue, débits invalides, booléens textuels, politique de conflit, exclusions mal formées et champs inattendus.

Une validation commune contrôle maintenant l’ensemble de la demande de configuration avant le chargement d’un profil et avant les actions externes. Les deux points d’entrée, synchrone et asynchrone, l’utilisent. Les règles complètes et les patches conservent leurs valeurs par défaut respectives. Le test supplémentaire avec un profil enregistré vérifie que ses options ne sont pas appliquées avant le rejet d’une demande invalide. Cette correction porte sur la validation préalable ; elle ne prétend pas rendre atomiques les écritures de plusieurs applications en cas d’erreur matérielle ultérieure.

### Encodage des chemins de programme

La fonction d’installation de Gad laissait les `%` du chemin intacts dans `Exec`. Un chemin contenant `%u` pouvait donc être interprété comme un code de champ Desktop. Les lanceurs Gad, Dolphin et Konqueror utilisent maintenant la même fonction Python d’encodage. Elle protège les codes de champ, les caractères spéciaux d’argument et les sauts de ligne dans la valeur Desktop. Cela évite également de maintenir trois variantes divergentes.

Les tests couvrent `%u`, un pourcentage littéral, guillemets, dollar, accent grave, antislash et caractères de contrôle. Un test de protection du lanceur Dolphin vérifie le fichier réellement écrit dans un dossier isolé.

### Traçabilité de cette passe

`target/deep-review/source-inventory.json` répertorie les 151 fichiers applicatifs, tests et fichiers de construction examinés, avec taille en lignes et empreinte SHA-256. Cet inventaire fixe la version des sources correspondant à la validation ; les tests et les traces d’exécution complètent l’examen des chemins décrit plus haut. Les rapports de cette poursuite sont dans `target/deep-review/`.

### Course de fermeture découverte par l’intégration optimisée

L’échec de l’intégration à la fermeture a révélé une régression de la validation préalable : l’accès Qt à un champ absent ajoutait `gui_settings: null`, puis la nouvelle validation refusait cette demande. L’accès utilise maintenant `value()`, sans insertion. Un test valide explicitement les configurations moteur dépourvues de réglages GUI, et l’intégration vérifie les réponses de configuration et de fermeture avant de contrôler l’historique.

L’examen de ce chemin a aussi isolé une course indépendante. Le moteur publie l’état terminal avant ses derniers acquittements et avant la libération de sa réservation ; le nettoyage exclut légitimement les travailleurs encore réservés. La fermeture pouvait donc effectuer ce nettoyage trop tôt, puis arrêter les travailleurs et conserver le lot dans son instantané final.

Le test `shutdown_waits_for_terminal_workers_before_discarding_history` reproduit cette frontière sans dépendre de la vitesse du disque. Il a échoué avant correction avec « Shutdown returned before finalization ». La fermeture attend maintenant la libération des travailleurs, préparations et finalisations avant le nettoyage, puis conserve son ordre normal de sauvegarde et d’arrêt. Le fichier copié doit rester intact, tandis que l’historique non conservé disparaît.

La première validation complète, avec son échec d’intégration, est conservée dans `target/deep-review-before/`. La validation finale réutilise `target/deep-review/`. Le script d’intégration distante accepte également `GAD_TEST_BINARY`, comme les autres intégrations, pour tester le même binaire optimisé.

### Sauvegardes et dates de changement KIO

Le serveur FTP local isolé et le montage KIO Fuse ont permis de reproduire un refus de restauration d’une sauvegarde intacte : le fournisseur expose un ctime instable. Les nouvelles sauvegardes KIO de fichiers portent donc une empreinte de contenu plutôt qu’une comparaison de cette date. Les sauvegardes POSIX conservent leur contrôle ctime, sans relecture complète supplémentaire. L’identité et les métadonnées reconnues restent vérifiées dans les deux cas ; les liens sont contrôlés par leur identité et leur cible.

Le nettoyage de l’historique réutilise désormais le même contrôle que la restauration. Le test `backup_version_checks_protect_restoration_and_history_cleanup` vérifie qu’une modification à taille et mtime constantes refuse les deux actions et conserve sauvegarde et résultat. La capture d’empreinte KIO respecte les points d’annulation du transfert. Cette lecture supplémentaire ne concerne que les fournisseurs dont ctime n’est pas fiable.

### Validation finale de l’approfondissement

Les neuf étapes complètes sont réussies. Le dernier code du moteur repasse ses 347 tests, Clippy, puis les 347 tests en compilation optimisée et avec un seul thread ; 17 benchmarks restent volontairement ignorés. Les 117 tests Python, 162 cas Qt d’actions/réglages, cinq Konqueror, 11 interactions et 11 de surveillance Dolphin passent, ainsi que presse-papiers et notifications. Les catalogues, manpages et liens des guides sont valides.

Trois répétitions de l’intégration locale ont réussi après correction, puis le dernier binaire repasse l’intégration locale, la CLI et le serveur FTP/KIO Fuse réels et isolés. Le montage SSHFS de l’utilisateur reste indisponible. L’identification du montage déjà capturée pour le temporaire est réutilisée pour sa sauvegarde dans le même parent, afin d’éviter une ouverture et une nouvelle lecture des montages à chaque remplacement.

Les tests ajoutent 22 cas par rapport à la passe précédente : deux Rust, cinq Python et quinze cas Qt. Les sauvegardes altérées ne sont plus supprimées lors d’une fermeture sans historique : le refus laisse le moteur utilisable, et la fermeture avec conservation de l’historique reste possible. Aucun commit ni envoi n’est effectué.


## Nouvelle passe : réglages, précision des débits et fiabilité des essais

Cette passe complète l’examen précédent par les chemins de configuration, profils, CLI, traduction des nombres et infrastructure de tests. L’inventaire `target/review-settings/source-inventory.json` fixe les empreintes de 155 fichiers de sources, tests et construction ; il ne constitue pas une garantie d’absence de bugs.

### Corrections

- Les configurations combinées réutilisent le contrôleur transactionnel des profils. Un échec Dolphin après modification Krusader, ou un échec moteur après modification externe, déclenche la restauration des réglages précédents. Les configurations concurrentes sont refusées pendant cette opération. La restauration reste au mieux : si elle échoue aussi, un message demande de vérifier les options.
- Une configuration partielle ne réécrit que les options demandées. Les actions de réparation des raccourcis restent possibles même quand la case indique déjà l’état voulu. Une modification externe du démarrage automatique n’est plus écrasée par une demande concernant uniquement les raccourcis.
- Les trois contrôles de débit conservent les valeurs entières en octets/s, jusqu’à 1 Tio/s, avec sept décimales en Mio/s. Les limites de 1 ou 1024 octets/s ne deviennent plus illimitées au chargement ou à l’application d’un profil. Les descriptions emploient le même format ; les nombres usuels gardent deux décimales.
- La saisie interactive positive choisit au minimum un octet/s. La CLI refuse une valeur positive qui serait arrondie à zéro ; seul zéro explicite demande l’absence de limite. Le changement de langue actualise le séparateur des nombres sans modifier le débit.
- Le délai de réponse CLI passe à 60 secondes pour les profils et configurations, dont les opérations externes et restaurations peuvent dépasser dix secondes. Un véritable pair IPC retardé de 10,2 secondes reproduit l’ancien échec et vérifie la correction.
- Les intégrations locales, reprise, CLI et FTP démarrent leur propre session D-Bus avec HOME et répertoires XDG temporaires avant de lancer les services. Elles évitent le bus système réel. L’import des scripts ne lance plus de tests, compilations ou processus ; trois tests Python protègent ces propriétés, y compris sans dépendance FTP facultative.
- Les messages communs aux profils et configurations parlent maintenant de réglages, avec mise à jour des cinq traductions.

### Reproductions et validation

Les traces avant correction sont conservées dans `target/review-settings/` : échecs de restauration, perte de précision et expiration CLI. Les nouveaux scénarios représentent 13 cas Qt et trois tests Python supplémentaires par rapport à la passe précédente, ainsi que l’essai IPC retardé dans l’intégration.

Les neuf étapes de validation sont réussies : 347 tests Rust, Clippy sans avertissement, 120 tests Python, 175 cas Qt d’actions/réglages, cinq Konqueror, 11 interactions et 11 de surveillance Dolphin, ainsi que presse-papiers et notifications. Les 17 benchmarks Rust restent volontairement ignorés. Le moteur repasse aussi les 347 tests en compilation optimisée et avec un seul thread.

Le binaire optimisé repasse l’intégration locale, la reprise après SIGKILL avec comparaison MD5, les configurations CLI et les transferts FTP/KIO Fuse réels (copie, déplacement, remplacement et restauration). Le montage SSHFS personnel n’étant pas disponible, aucun essai n’est revendiqué sur ce serveur. Les traductions, manpages, liens, 138 captures et 18 exports HTML sont vérifiés. Aucun commit ni envoi Git/PPA n’est effectué.

## Poursuite de la revue : commandes, saisie et préparation des paquets

Cette passe examine les interactions CLI/IPC et KIO, les contrôles de débit et diagnostics, les exclusions et profils, les scripts d’installation et de paquetage. Les chemins sensibles du moteur (publication, restauration, reprise, journal, reconnexion, ordre de traitement et propriétés) sont également recontrôlés ; aucune modification Rust supplémentaire n’est nécessaire dans cette passe. L’inventaire des sources et les traces de validation sont conservés dans `target/review-followup/`.

### Défauts reproduits et corrigés

- **Réponse distante annoncée trop tôt en échec.** Le client abandonnait après dix secondes alors que le résolveur KIO pouvait travailler trente secondes et soumettre le transfert ensuite. Les demandes nécessitant une résolution disposent maintenant de 35 secondes pour recevoir leur réponse. Profils et configurations gardent 60 secondes, les commandes ordinaires dix secondes. Un pair IPC réel simulant une réponse après 10,2 secondes reproduit l’ancien échec (`Broken pipe` côté serveur), puis vérifie la réponse acceptée après correction. Cet essai contrôle le transport et son délai ; les transferts distants sont vérifiés séparément par le scénario FTP réel.
- **Motif `-h` interprété comme une commande d’aide.** `gad --configure --exclude-file -h` affichait l’aide avec un succès, sans enregistrer l’exclusion. L’aide est désormais reconnue comme commande autonome. L’intégration CLI vérifie la réponse et la présence effective du motif dans les options du moteur.
- **Saisie de débit effacée par la progression.** Le rafraîchissement appelait `setValue()` même pour une limite identique. Qt réécrivait l’éditeur malgré le blocage des signaux et effaçait un texte non validé. Le test reproduisait « 12 Mio/s » remplacé par « 1,00 Mio/s ». La synchronisation n’appelle plus cette méthode quand la valeur en octets/s est inchangée. Le même test vérifie qu’une nouvelle consigne externe reste affichée.
- **Copie récursive des sources de paquet.** Une sortie de `package.py` placée dans un répertoire embarqué pouvait inclure sa propre arborescence lors de la copie. Le script refuse ces emplacements avant création et avant appel à dpkg/Cargo. Le test vérifie les dossiers `docs`, `scripts`, `debian` et un lien symbolique vers `docs`. Les sorties habituelles dans `target/` restent permises et les tests d’archives xz/gz passent.

### Amélioration de coût dans l’interface

Le rafraîchissement du diagnostic construisait sa correspondance entre lots et lignes par un parcours complet pour chaque lot : cette recherche était quadratique. Un index temporaire par identifiant ramène la recherche des lignes à un coût linéaire total. Les items restent conservés ; les tests existants vérifient sélection, changement de langue, mise à jour et retrait après terminaison. Le calcul des métriques de police est aussi mutualisé par rafraîchissement. Aucun gain chronométré n’est revendiqué pour cette modification.

### Vérifications de cette passe

Un test Qt et un test Python sont ajoutés, ainsi que deux scénarios dans l’intégration CLI. Les reproductions avant correction sont conservées avec les rapports. Les neuf étapes sont réussies. Les suites validées comprennent : 347 tests Rust (17 mesures explicites ignorées), 121 tests Python, 176 cas Qt principaux, cinq Konqueror, 11 interactions et 11 de surveillance Dolphin, plus presse-papiers et notifications. Le fichier `results.json` fournit le résultat de chacune des neuf étapes globales.

Les tests Rust optimisés avec un seul thread repassent également, ainsi que les scénarios du dernier binaire : transferts locaux, reprise après SIGKILL avec comparaison MD5, configuration CLI et transferts FTP/KIO Fuse réels avec remplacement et restauration. Les traductions, manpages, syntaxe Python/shell et liens des guides sont contrôlés. Aucun essai n’est revendiqué sur le serveur SSHFS personnel indisponible. Aucun commit, push, changement de version ou envoi au PPA n’est effectué.

## Poursuite : démarrage du moteur, compatibilité IPC et parcours profonds

Cette passe approfondit le démarrage persistant, les travailleurs, la compatibilité entre instances, la préparation des arbres, la simulation, le bilan d’erreurs et son export. Les chemins de restauration, de reprise et les contrôles de publication sont recontrôlés sans nouvelle modification de leur politique de sécurité.

### Initialisation persistante sans travailleurs orphelins

Le moteur créait ses deux travailleurs avant la sauvegarde initiale du journal. Si cette sauvegarde échouait, le constructeur retournait une erreur mais les travailleurs conservaient chacun une poignée de l’état partagé et attendaient indéfiniment. Le test `failed_initial_journal_does_not_start_workers` reproduit ce cas en provoquant une erreur à cette frontière. Il vérifie l’absence de travailleurs démarrés et la libération de l’état partagé par une référence faible. La reproduction nettoie aussi les travailleurs de la version défectueuse avant de signaler l’échec.

La construction de l’état et le lancement des travailleurs sont maintenant distincts. Le démarrage persistant affecte la séquence, sauvegarde le journal et ne lance les travailleurs qu’après succès. Les moteurs de test sans journal démarrent leurs travailleurs comme auparavant. La fermeture normale conserve ses barrières de finalisation et de durabilité.

### Sonde IPC malformée

La réponse à la sonde de compatibilité n’était pas validée comme la réponse principale. Un JSON tronqué, invalide ou sans version devenait implicitement une version zéro, puis ouvrait le dialogue signalant une ancienne instance. Le client pouvait rester bloqué dans ce dialogue alors que le serveur était simplement déconnecté. L’attente de la sonde pouvait aussi tourner jusqu’au délai après une déconnexion.

Le client exige désormais une ligne complète contenant un objet JSON et une version entière positive. Une réponse invalide termine la commande en échec sans ouvrir le dialogue de compatibilité ; une version ancienne valide conserve le dialogue normal. La déconnexion interrompt immédiatement l’attente. Six réponses incorrectes sont ajoutées à l’intégration CLI ; la première provoquait un dépassement des cinq secondes du test avant correction. Ces essais utilisent un faux serveur et un bus privés.

### Analyse des arbres sans récursion

L’analyse de préparation et celle de la prévisualisation appelaient récursivement leur propre fonction pour chaque sous-dossier. Le test `scan_deep_tree_uses_bounded_stack` exécute le scénario dans un sous-processus pour qu’une régression ne tue pas toute la suite. Avec 180 niveaux et une pile de travail limitée à 128 Kio, il a reproduit séparément un débordement dans chacun des deux analyseurs.

Les parcours sont maintenant itératifs et conservent le préordre trié, parents avant enfants. La simulation utilise une étape de sortie de dossier pour conserver l’agrégation des branches déjà présentes dans la file. Les itérateurs de frères sont conservés plutôt que deux chemins complets par frère en attente : une grande largeur n’impose pas de préparer tous ces chemins supplémentaires. Chaque entrée conserve ses contrôles d’annulation et d’exclusion. La recherche du lot pendant le scan réutilise également l’index validé du moteur plutôt que de reparcourir l’historique à chaque fichier.

Le test passe après correction pour les deux parcours et compare aussi leur ordre complet. Les tests existants de simulation, conflits, exclusions et préparation repassent. Cette validation ne revendique pas de test sur un montage bind cyclique ni de suppression des limites de chemins du système.

### Indexation du bilan d’erreurs

L’affichage et l’export texte recherchaient le lot d’origine par un parcours complet pour chaque source et destination. Ils construisent maintenant un index par instantané, puis réutilisent les métadonnées du bon lot pour ses lignes. L’export JSON reste inchangé. Un test Qt vérifie deux lots partageant les mêmes chemins de montage mais des origines distantes différentes, dans le texte exporté et dans la véritable fenêtre du bilan. Les recherches isolées gardent le même comportement, y compris sans métadonnées de lot.

### Validation finale

Deux tests Rust, un test Qt et six scénarios IPC sont ajoutés dans cette passe. Les neuf étapes globales réussissent dans `target/review-lifecycle-final/results.json` : 349 tests Rust, Clippy sans avertissement, 121 tests Python, 177 cas Qt principaux, cinq Konqueror, 11 interactions et 11 de surveillance Dolphin, plus presse-papiers et notifications. Les 17 mesures explicites restent volontairement ignorées. Les 349 tests Rust repassent aussi en compilation optimisée, avec un seul thread.

Le dernier binaire optimisé repasse les scénarios locaux, la reprise après SIGKILL avec comparaison MD5, les configurations CLI et les transferts FTP/KIO Fuse réels avec remplacement et restauration. Les catalogues, manpages, liens des guides, syntaxe Python et shell sont vérifiés. Le serveur SSHFS personnel indisponible n’est pas utilisé. Les traces avant/après et l’inventaire des sources sont conservés avec les rapports ; aucun commit, push, changement de version ou envoi au PPA n’est effectué.

## Poursuite : réactivation, décisions de conflit et sélection multiple

Cette passe approfondit les commandes de sélection, d’annulation et de réactivation, la gestion des choix de conflit et le calcul des actions de priorité de l’interface. Les scénarios de reprise, restauration et transferts distants sont revérifiés par la validation globale. Les rapports et reproductions sont conservés dans `target/review-reactivation/`.

### Une décision ponctuelle ne survit plus à l’annulation

Un conflit pouvait recevoir la réponse « remplacer » pendant une pause. Si le lot était annulé avant que son travailleur consomme cette réponse, elle restait dans `job.resolution`. La réactivation remettait les fichiers en attente mais conservait la réponse : la nouvelle tentative remplaçait alors la destination sans poser de nouvelle question.

Le test `review_retry_requires_a_new_conflict_decision` reproduit cette séquence avec les commandes réelles : attendre un conflit, mettre en pause, répondre « remplacer », annuler, attendre l’arrêt puis réactiver. Avant correction, le fichier de destination était remplacé et le lot finissait en OK. Après correction, la nouvelle tentative attend un nouveau choix et le contenu original de la destination reste intact.

La préparation de réactivation efface maintenant la réponse ponctuelle. La politique du lot, y compris un choix explicitement demandé pour tous les conflits suivants, reste conservée.

### Données de tentative remises à zéro

La réactivation conservait aussi `interrupted`, `resume_checking`, `resume_checked`, `resumed_from` et l’ancien rapport de propriétés. Une nouvelle copie pouvait donc encore sembler vérifier un ancien préfixe, présenter un rapport périmé et sous-compter ses nouveaux octets dans les mesures de débit.

Les entrées réactivées repartent désormais sans ces données, en plus des compteurs et empreintes déjà réinitialisés. Le test `review_retry_resets_only_selected_attempt_metadata` reproduit les valeurs restées incorrectes avant correction et vérifie leur remise à zéro. Il vérifie aussi que l’entrée voisine terminée conserve ses données et son rapport de propriétés. Les parents nécessaires à la nouvelle tentative suivent la préparation normale.

### Réponse refusée en l’absence de conflit

La commande `resolve` vérifiait l’état du fichier seulement lorsque le client donnait un indice. Sans indice, elle pouvait enregistrer une réponse et même modifier la politique d’un lot préparé qui n’avait encore aucun conflit.

Une demande sans indice doit maintenant trouver un conflit non annulé dans le lot. Une demande avec indice conserve son contrôle précis de la ligne ; les demandes sur un lot annulé sont refusées. Le diagnostic existant de conflit périmé est réutilisé. Le test `resolve_without_index_requires_a_pending_conflict` vérifie le refus et l’absence de modification de la politique ou de la réponse. Les tests de réponses sans indice sur un véritable conflit passent toujours.

### Actions de priorité moins coûteuses

Pour chaque ligne sélectionnée, l’interface parcourait ses frères avant et après afin de déterminer si « monter » ou « descendre » était disponible. Sélectionner toutes les lignes d’une longue liste entraînait ainsi un travail quadratique pour ce calcul.

L’interface parcourt maintenant une seule fois chaque groupe de frères concerné. Les lignes sélectionnées mais cachées par un filtre restent distinguées des cibles visibles : elles ne doivent pas activer une priorité pour une ligne différente. Un test Qt couvre les sélections en tête, en fin, contiguës, dispersées, entièrement sélectionnées et une sélection partiellement cachée. Cette amélioration concerne le calcul des disponibilités de priorité ; aucun gain chronométré ni coût linéaire de l’ensemble des actions contextuelles n’est revendiqué.

### Vérification

Trois tests Rust et un test Qt sont ajoutés. Les résultats détaillés des neuf étapes se trouvent dans `results.json` : 352 tests Rust, Clippy sans avertissement, 121 tests Python, 178 cas Qt principaux, cinq Konqueror, 11 interactions et 11 de surveillance Dolphin, plus presse-papiers et notifications. Les 17 mesures explicites restent ignorées. Le dernier moteur repasse également les 352 tests en compilation optimisée avec un seul thread.

Le dernier binaire repasse l’intégration locale, la reprise après SIGKILL avec comparaison MD5, les configurations CLI et les transferts FTP/KIO Fuse réels avec remplacement et restauration. Les traductions, manpages, liens des guides, syntaxe Python et shell sont vérifiés. Le serveur SSHFS personnel indisponible n’est pas utilisé. Aucun commit, push, changement de version ou envoi au PPA n’est effectué.

## Poursuite : propriétés restaurées, presse-papiers GNOME et écritures atomiques

Cette passe approfondit les opérations de propriétés pendant la restauration, les formats de presse-papiers et les écritures de configuration utilisées par les intégrations Dolphin/Krusader. Les publications, le journal et ses deltas ont aussi été relus ; leurs garanties de publication et de vérification restent conservées. Les reproductions et rapports sont dans `target/review-metadata-clipboard/`.

### Propriétés appliquées uniquement au fichier reconnu

La restauration ouvrait un descripteur O_PATH pour reconnaître le montage, mais modifiait ensuite la date et les permissions par de nouvelles opérations sur le chemin. Une substitution entre les étapes pouvait donc conduire à modifier les propriétés d’un fichier étranger.

L’identité attendue du résultat restauré est maintenant transmise explicitement, indépendamment de l’empreinte fournissant les propriétés d’origine. Les dossiers utilisent leur identité source reconnue ou celle du dossier recréé, et les temporaires de restauration celle de leur garde. L’ouverture initiale et celle du descripteur de modification doivent correspondre à cette identité. L’ouverture de modification ne suit pas le lien final et n’attend pas sur un FIFO. Date et permissions sont ensuite appliquées au même descripteur ; une dernière vérification refuse une disparition ou substitution du nom.

Le test `metadata_restoration_rejects_replacements_at_pin_boundary` provoque une substitution avant ou immédiatement après l’ouverture initiale, avec remplacement ordinaire et lien symbolique. Il a échoué avant correction. Après correction, les quatre variantes refusent la restauration et vérifient que la date et les permissions du fichier étranger sont intactes. Le rappel injectable est privé à la primitive, avec rappel vide dans le fonctionnement normal.

### Répétition de propriétés déjà appliquées

Après une interruption entre application des propriétés et acquittement du journal, une répétition pouvait retrouver une entrée déjà privée de lecture, puis échouer en tentant de l’ouvrir à nouveau. La primitive vérifie maintenant les propriétés par le descripteur O_PATH : si date et permissions sont déjà celles demandées, aucune ouverture de lecture ni écriture supplémentaire n’est nécessaire. L’identité reste vérifiée, y compris sur ce chemin court.

Le test `metadata_restoration_is_idempotent_without_read_permission` reproduisait le refus sur un fichier en mode 000. Il vérifie maintenant la réussite sur fichier et dossier, sans leur redonner des droits. Le test antérieur de restauration des modes restrictifs passe aussi. Les montages KIO conservent leur traitement particulier des permissions imposées par le fournisseur.

### Intention Couper avec les formats GNOME et URI simultanés

Le décodeur choisissait `text/uri-list` avant `x-special/gnome-copied-files`. Quand les deux étaient proposés sans marqueur KDE, il perdait donc l’en-tête GNOME `cut` et construisait une copie.

Il lit maintenant cette intention GNOME lorsque la liste URI ne fournit pas de marqueur KDE. Les deux listes sont décodées et doivent désigner le même ensemble de fichiers ; l’ordre de la liste principale est conservé. Une seconde lecture refuse un changement de l’intention GNOME. Le décodage secondaire ne reçoit que le format GNOME et ne réentre jamais dans la branche de comparaison. Un en-tête inconnu est refusé plutôt que traité implicitement comme une copie. Le chemin KDE avec son marqueur reste prioritaire.

Cinq scénarios sont ajoutés au véritable test de presse-papiers Qt : couper avec les deux formats, copier avec les deux, listes divergentes, intention modifiée pendant la lecture et en-tête invalide. Le premier échouait avant correction. Ce sont des injections de formats de presse-papiers ; aucun pilotage d’un gestionnaire de fichiers GNOME réel n’est revendiqué.

### Temporaires distincts pour les écritures de configuration

`atomic_write` employait un nom temporaire fixe avec troncature. Deux écritures simultanées pouvaient partager le même fichier de préparation ; après publication par l’une, l’autre ne trouvait plus son temporaire. Le test concurrent avec barrière sur les synchronisations reproduisait `FileNotFoundError`.

Chaque écriture crée désormais un temporaire exclusif avec `mkstemp` dans le dossier cible, applique les permissions attendues par descripteur, écrit et synchronise avant de publier. Le nettoyage ne concerne que son temporaire et n’est plus exécuté après une publication réussie. Le test vérifie la réussite des deux écritures, un résultat complet, les permissions conservées et l’absence de temporaires restants. Cette correction évite les collisions de préparation ; elle ne prétend pas sérialiser l’ensemble des transactions de configuration entre plusieurs processus.

### Validation finale

Deux tests Rust, un test Python et cinq scénarios presse-papiers sont ajoutés. Les neuf étapes globales réussissent : 354 tests Rust, Clippy sans avertissement, 122 tests Python, 178 cas Qt principaux, cinq Konqueror, 11 interactions et 11 de surveillance Dolphin, plus presse-papiers et notifications. Les 17 mesures explicites restent volontairement ignorées. Les 354 tests Rust passent aussi en compilation optimisée avec un seul thread.

Le dernier binaire repasse les transferts locaux, la reprise après SIGKILL avec comparaison MD5, les configurations CLI et les transferts FTP/KIO Fuse réels avec remplacement et restauration. Les catalogues, manpages, liens des guides et syntaxe shell sont vérifiés. L’assistant Dolphin installé correspondait à la version Git d’origine ; il est remplacé atomiquement par la version corrigée, également utilisée par Krusader pour ses écritures. Aucun gestionnaire de fichiers ni transfert utilisateur n’est arrêté. Aucun commit, push, changement de version ou envoi au PPA n’est effectué.

La première validation globale de cette passe est conservée dans `target/review-metadata-clipboard-before-final/` : les tests fonctionnels passaient, mais Clippy refusait un littéral décimal de permission dans un nouveau test. Le littéral est écrit en octal et la validation globale relancée est réussie avant installation.

## Poursuite : écritures et compaction du journal

Cette passe approfondit les écritures de l’instantané et du journal incrémental (WAL), ainsi que leurs chemins d’erreur. Les cas suivants sont reproduits dans des dossiers temporaires isolés. Les traces sont conservées dans `target/review-journal-final/`.

### Temporaire de compaction exclusif

La compaction réutilisait `state.json.tmp` avec troncature. Le refus des liens symboliques ne protégeait pas d’un lien physique préexistant : l’ouverture pouvait tronquer son autre nom avant même la publication de l’instantané.

La compaction crée désormais un fichier neuf avec création exclusive, permissions privées, identifiant de processus et compteur. Un nom déjà occupé est ignoré. Un garde conserve l’identité du temporaire, contrôle sa version avant publication et nettoie uniquement le fichier reconnu en cas d’échec. Aucun ancien temporaire inconnu n’est repris ou supprimé.

Le test `review_journal_checkpoint_uses_an_exclusive_temporary` installe un lien physique sous l’ancien nom fixe. Avant correction, le contenu étranger était modifié ; après correction, il reste intact et le nouvel instantané est lisible.

### WAL validé avant publication et remise à zéro

L’ajout incrémental contrôlait le fichier ouvert, mais la compaction ouvrait le WAL avec troncature après publication de l’instantané. Un WAL remplacé pouvait donc être vidé et l’instantané déjà changé avant le refus.

L’ouverture est maintenant commune aux deux chemins, sans troncature initiale. Le descripteur doit désigner un fichier ordinaire à lien unique, correspondre au nom courant et, lorsque l’identité est déjà connue, conserver cette identité et la longueur attendue. La compaction le valide avant publication, puis le revalide après synchronisation de l’instantané. Sa version est également comparée à celle qui a été épinglée. La remise à zéro utilise alors `set_len` sur ce même descripteur reconnu.

Le test `review_journal_compaction_refuses_a_replaced_log` vérifie les remplacements par un fichier ordinaire et par un lien physique. Avant correction, la compaction était acceptée. Après correction, elle est refusée ; le fichier substitué et les octets de l’ancien instantané restent intacts. Un échec survenant après publication conserve le verrou de journal « empoisonné » existant, qui exige un rechargement avant toute autre écriture.

### Refus rapide des tubes nommés

Les ouvertures en écriture pouvaient attendre indéfiniment lorsqu’un WAL avait été remplacé par un FIFO sans lecteur. Elles utilisent maintenant le mode non bloquant et conservent le refus des liens symboliques ; les contrôles de type précèdent toute écriture ou remise à zéro.

Le test `review_journal_fifo_never_blocks_a_writer` s’exécute dans un sous-processus limité à cinq secondes. Il reproduisait le blocage de l’ajout incrémental avant correction. Il vérifie maintenant que l’ajout et la compaction échouent rapidement sur le FIFO. Le dossier du scénario appartient au test seulement après sa création réussie, pour ne jamais nettoyer un emplacement préexistant.

### Réécriture externe à taille et mtime identiques

L’empreinte volatile de l’instantané comparait périphérique, inode, taille et date de modification. Une réécriture du même fichier avec conservation de sa taille et rétablissement de mtime passait ce contrôle.

L’empreinte inclut maintenant ctime et ses nanosecondes. Le test `review_journal_refuses_snapshot_rewrite_with_preserved_mtime` modifie un booléen dans un JSON de même longueur, rétablit mtime puis demande une sauvegarde. Avant correction, la sauvegarde était acceptée. Elle est maintenant refusée sans écraser le JSON modifié ni ajouter d’enregistrement au WAL. Ce contrôle réutilise les métadonnées déjà lues et ne recalcule aucune empreinte de contenu. Le format persistant reste inchangé.

### Validation finale

Quatre tests Rust sont ajoutés. Les neuf étapes globales passent : 358 tests Rust, Clippy sans avertissement, 122 tests Python, 178 cas Qt principaux, cinq Konqueror, 11 interactions et 11 de surveillance Dolphin, plus presse-papiers et notifications. Les 17 mesures explicites restent ignorées. Les 358 tests passent aussi en compilation optimisée avec un seul thread. Le dernier ajustement de propriété du dossier de test FIFO repasse son scénario ciblé et Clippy.

Le binaire optimisé repasse les transferts locaux, la reprise après SIGKILL avec comparaison MD5, les configurations CLI et les transferts FTP/KIO Fuse réels avec remplacement et restauration. Traductions, manpages, liens des guides et syntaxe shell sont vérifiés. L’inventaire, les empreintes et les rapports accompagnent cette passe. Aucun commit, push, changement de version ou envoi au PPA n’est effectué ; les transferts utilisateur restent en cours sans redémarrage forcé.

## Poursuite : autorisation de démarrage, pause et exclusions pendant configuration

Cette passe approfondit la préparation manuelle, la reprise globale, la validation des commandes de pause et les interactions entre application de profil et édition des exclusions. Les reproductions et rapports sont conservés dans `target/review-start-controls-final/` ; la première validation globale est conservée dans `target/review-start-controls/`.

### Réservation après sauvegarde de l’autorisation

`start_prepared` et la reprise globale retiraient le drapeau de préparation avant le retour de leur sauvegarde. Un travailleur réveillé entre ces étapes pouvait donc réserver le lot alors que l’autorisation attendait encore le verrou du journal. Si la sauvegarde échouait, le traitement pouvait aussi avoir quitté l’état permettant de rendre le lot à sa préparation. Les sauvegardes de chaque entrée protégeaient toujours leurs propres opérations de fichiers ; ce défaut concerne la synchronisation du démarrage du lot.

Un ensemble volatil `pending_starts`, absent du JSON persistant, bloque désormais ces réservations. Le chemin de sauvegarde commun libère les identifiants après succès ou remet leurs drapeaux de préparation après refus, puis réveille les travailleurs. Les lots déjà autorisés conservent leur fonctionnement normal.

Le test `manual_start_waits_for_its_journal_before_scheduling` bloque le journal et observe directement le prédicat de l’ordonnanceur, sans dépendre d’un réveil chanceux du travailleur. Avant correction, le lot était éligible avant la fin de la sauvegarde. Après correction, le test vérifie Démarrer et reprise globale, avec sauvegarde réussie et avec instantané volontairement altéré. Le succès finit le transfert ; le refus conserve la préparation et ne crée pas le résultat. Le moteur au journal délibérément inutilisable est arrêté uniquement dans sa fixture de test.

### Valeurs de pause strictement booléennes

Les commandes de pause globale et de sélection interprétaient une valeur fournie mais non booléenne comme `true`. Une demande avec `"paused":"false"`, par exemple, pouvait donc mettre en pause au lieu de signaler son erreur.

Le parseur partagé conserve la pause par défaut lorsque le champ est absent, mais refuse maintenant `null`, nombres, chaînes, tableaux et objets. La réactivation, dont le paramètre ne pilote pas la pause, conserve sa sémantique. Le test `invalid_pause_values_do_not_change_runtime_controls` vérifie les deux commandes, depuis une pause globale active ou inactive, et compare les contrôles des lots et fichiers. Le nouveau diagnostic anglais est traduit dans les cinq catalogues.

### Exclusions protégées pendant une configuration asynchrone

L’application d’un profil ou d’une configuration combinée désactivait les panneaux d’options, de raccourcis, de débits et de fin de lot, mais omettait celui des exclusions. Ce panneau est désormais désactivé et réactivé avec les autres.

La désactivation d’un éditeur peut aussi lui retirer le focus. Le gestionnaire d’exclusion ignore maintenant la validation lorsque l’éditeur est désactivé : un brouillon ne provoque plus une commande concurrente et une boîte d’erreur pendant la transaction. Une modification de raccourci seule conserve ce brouillon ; sa prochaine perte de focus, lorsque le champ est actif, le valide normalement.

Le test Qt existant `combinedConfigurationBlocksConcurrentChanges` est renforcé pour vérifier les états désactivé/réactivé, l’absence de dialogue d’erreur sur la perte de focus désactivée et la validation ultérieure du brouillon. Il a reproduit séparément le panneau laissé actif, puis la validation concurrente liée au retrait de focus.

### Test du journal indépendant de la granularité temporelle

La suite complète a révélé une ancienne attente devenue fragile depuis le contrôle de ctime : un test déplaçait l’instantané hors de son emplacement puis le remettait et supposait l’empreinte inchangée. Selon la granularité de l’horloge, les deux renommages pouvaient conserver ou changer la valeur observée.

Le test vérifie maintenant le refus de l’ancien écrivain après modification de ctime, la lecture valide du contenu remis en place, puis le rechargement de l’écrivain avant de poursuivre ses scénarios de liens symboliques. Aucune garantie de production n’est relâchée. Le scénario ciblé repasse dix répétitions.

### Validation finale

Deux tests Rust sont ajoutés et un test Qt existant est enrichi. Les neuf étapes finales réussissent : 360 tests Rust, Clippy sans avertissement, 122 tests Python, 178 cas Qt principaux, cinq Konqueror, 11 interactions et 11 de surveillance Dolphin, plus presse-papiers et notifications. Les 17 mesures explicites restent ignorées. Les 360 tests passent aussi en compilation optimisée avec un seul thread.

Le dernier binaire repasse les transferts locaux, la reprise après SIGKILL avec comparaison MD5, les configurations CLI et les transferts FTP/KIO Fuse réels avec remplacement et restauration. Traductions, manpages, liens des guides et syntaxe shell sont vérifiés. La garde de démarrage n’ajoute aucun champ persistant et le format du journal reste inchangé. Aucun commit, push, changement de version ou envoi au PPA n’est effectué ; aucun transfert utilisateur n’est interrompu.

## Poursuite : sélections de dossiers et commandes pendant une nouvelle analyse

Deux nouvelles passes ont conduit aux changements suivants, sans modification de version ni publication :

- **Sélection multiple de dossiers** (`controls.rs`) : chaque dossier sélectionné reparcourait auparavant toutes les entrées du lot sous le verrou moteur. L’expansion regroupe maintenant les racines et traverse les entrées une seule fois par lot, en recherchant leurs ancêtres dans un index. Les contraintes sur les deux chemins, les indices stables et la validation des lignes restent conservées. La sélection d’un lot entier évite cette expansion ; les identifiants de lots utilisent le cache existant et les identifiants répétés ne reconstruisent plus la sélection complète.
- **Conservation des commandes lors d’une nouvelle analyse** (`validation.rs`) : le rapprochement par position perdait des pauses et annulations si les entrées changeaient d’ordre ou si un fichier apparaissait. Le rapprochement utilise désormais le triplet chemin source, destination et type. Un nouvel élément n’hérite pas des commandes d’un voisin. Le test de régression a échoué avant correction (`/tmp/gad-review-rescan-before.log`), puis réussi après correction.

Tests ajoutés : `directory_selection_matches_reference_for_overlapping_roots` compare toutes les combinaisons de dossiers d’une arborescence à la règle d’expansion antérieure, y compris doublons, destinations divergentes, lot entier et indice invalide. `preparation_preserves_row_controls_after_scan_reordering` vérifie pause, annulation et masquage après inversion des entrées et insertion d’un nouveau fichier.

La passe suivante sur les chemins de reprise, publication, diagnostic, estimation d’espace et les scripts inspectés n’a pas identifié d’autre correction suffisamment établie. Cela ne constitue pas une garantie d’absence de bugs dans tout le projet.

Validation de cette poursuite : 362 tests Rust réussis (17 mesures de performance explicitement ignorées), 122 tests Python et 178 tests Qt de la suite principale. Les neuf étapes du lanceur global passent (`target/review-selection/results.json`) ; Clippy avec avertissements interdits et les tests Rust sont repassés après la dernière adaptation du cas à dossier unique. Les autres suites Qt et les scénarios d’intégration sont également réussis. Le cas courant d’un seul dossier conserve une simple comparaison des préfixes, sans index auxiliaire.

Les 362 tests Rust passent aussi en compilation optimisée. Les scénarios local/reprise après SIGKILL, CLI et FTP réel ont été exécutés avec le dernier binaire optimisé. Celui-ci a été installé par remplacement atomique dans `~/.local/bin/gad`, avec vérification de son SHA-256 (`target/review-selection/delivery.json`), sans arrêter l’instance utilisateur. Aucun commit, push ni envoi au PPA.

## Poursuite : annulation des interruptions et expansion commune des sélections

- **Annulation d’un lot interrompu** : le menu contextuel ignorait un lot récupérable dont l’état était `Error`, parce que cet état est terminal. Sa reprise restait proposée après « Annuler », contrairement à « Tout annuler ». La sélection du lot désactive maintenant la reprise et marque les entrées interrompues comme annulées, sans supprimer leur temporaire ni leur journal de restauration.
- **Annulation d’un fichier interrompu** : le test étendu a reproduit le même défaut sur une entrée seule. Elle peut maintenant être annulée ; lorsque plus aucune entrée interrompue visible en erreur n’attend de reprise, le lot devient annulé. Les autres entrées récupérables restent éligibles. Les journaux des résultats restent disponibles pour une restauration explicite.
- **Expansion commune des dossiers** : Suppr, Retour arrière et l’annulation contextuelle partageant la même sémantique de descendants utilisent maintenant l’algorithme de `controls::expand_directories`. Les comparaisons répétées de chaque entrée avec chaque dossier sélectionné et les copies des chemins des racines sont évitées. Le cas d’un seul dossier conserve le parcours simple.
- **Lecteur Python du journal** : l’appartenance des lots à l’ordre de chaque enregistrement utilise un ensemble au lieu de recherches répétées dans une liste. La liste originale reste utilisée pour reconstruire l’ordre exact.

Preuves de régression : `/tmp/gad-review-cancel-resume-before.log` et `/tmp/gad-review-cancel-entry-before.log` échouent avant les corrections correspondantes. Les variantes corrigées passent. Le test `cancel_selected_interrupted_job_disables_resume_without_removing_partial` vérifie lot entier et fichier seul, refus de reprise, conservation de l’inode et du contenu du partiel, puis rechargement du journal. Le test `keyboard_cancellation_expands_overlapping_directories_only` couvre les trois commandes sur des dossiers imbriqués et des lignes répétées, avec une branche voisine conservée. Le test Python supplémentaire conserve un ordre inversé de 1 024 lots parmi 2 048 entrées, avec retrait des lots absents et mise à jour d’une entrée.

La passe suivante sur les temporaires, conflits, reprise, agrégation des dossiers, diagnostic et fin de lot n’a pas identifié de nouvelle correction certaine. Cette revue et les tests ne prouvent pas l’absence de tout bug.

Validation finale : les neuf étapes de `scripts/run_tests.py all` passent (`target/review-cancellation/results.json`), avec **364 tests Rust**, **123 tests Python**, **178 tests Qt principaux**, plus les suites Qt complémentaires et le watchdog Dolphin. Les 17 benchmarks Rust restent explicitement ignorés. Clippy avec avertissements interdits, formatage, syntaxe des scripts modifiés et vérification du diff passent. Les 364 tests Rust passent aussi en compilation optimisée. Les intégrations locale/reprise après SIGKILL, CLI et FTP réel passent avec le dernier binaire optimisé. Le binaire installé dans `~/.local/bin/gad` a été remplacé atomiquement et son SHA-256 vérifié (`target/review-cancellation/delivery.json`). Aucun arrêt de Gad, commit, changement de version, push ou envoi au PPA.

## Poursuite : configuration Krusader et publication de la documentation

- **Reprise d’une configuration Krusader interrompue** : le reçu précède volontairement les XML. Une interruption pouvait laisser un XML dans son état d’origine, puis l’activation suivante le refusait comme une modification inconnue. La reprise accepte maintenant exclusivement les deux versions exactes enregistrées (origine et version installée). Une modification utilisateur différente reste refusée avant toute écriture. Le nouveau test simule l’interruption avant le premier XML puis entre les deux XML, réactive Gad et vérifie la restauration du raccourci initial. Un second test vérifie qu’une modification inconnue laisse XML et reçu inchangés. Échec reproduit avant correction dans `/tmp/gad-review-krusader-before.log`.
- **Publication des guides et manpages** : les temporaires fixes `.nom.install` étaient ouverts sans création exclusive. Un lien préexistant pouvait donc faire écraser un fichier extérieur. `scripts/atomic_files.py` crée maintenant un temporaire unique avec `mkstemp`, écrit via son descripteur, fixe les permissions puis remplace la cible. Son nettoyage ne porte que sur ce temporaire. Les deux installateurs partagent ce mécanisme. Les tests couvrent les liens temporaires préexistants, un lien sur la cible finale, l’échec de publication et un nom final de 255 caractères. Échec reproduit avant correction dans `/tmp/gad-review-doc-publication-before.log`.
- **Index des parents dans l’arborescence et les doublons** : les clés utilisent des références aux chemins des entrées plutôt que de recopier deux chemins lors de chaque recherche d’ancêtre. Les règles de choix des parents et de conservation des doublons restent identiques. Les tests de doublons et de réordonnancement passent ; le benchmark existant de construction de l’arbre est exécuté séparément pour évaluer la portée du changement.

La passe suivante sur les chemins de reprise, les métadonnées, les URL distantes, les destinations de raccourcis et les scripts de distribution n’a pas identifié d’autre correction suffisamment établie. Cela ne garantit pas l’absence de tout bug dans le projet.

Mesure de l’index : le benchmark existant de construction à froid est passé de 39,6 ms environ avant modification à 39,9–41,3 ms dans la répétition au calme ; le chemin avec cache reste proche de 2 ms. Ces mesures ne démontrent pas un gain significatif de temps et ne permettent pas d’attribuer leur faible écart à la modification. Le changement retire les allocations des clés et des recherches d’ancêtres, mais aucun pourcentage d’accélération n’est annoncé. Les premières mesures sous compilation concurrente ne servent pas à conclure.

Validation finale : les neuf étapes du lanceur global passent (`target/review-publication/results.json`), dont **364 tests Rust**, **130 tests Python**, **178 tests Qt principaux**, les suites Qt complémentaires, le watchdog Dolphin et les intégrations. Les 17 benchmarks restent explicitement ignorés par la suite standard ; celui de l’arbre a été exécuté séparément. Clippy avec avertissements interdits et formatage passent. Les 364 tests Rust passent aussi en compilation optimisée. Les intégrations locale/reprise après SIGKILL, CLI et FTP réel passent avec le dernier binaire optimisé. Compilation des scripts Python modifiés, syntaxe shell et vérification du diff passent également.

Le binaire et le script Krusader installés ont été remplacés atomiquement et vérifiés (`target/review-publication/delivery.json`). Le script installé correspondait exactement à sa base Git connue avant mise à jour. Aucun réglage Krusader utilisateur n’a été activé ou désactivé pendant ces tests ; les configurations testées sont temporaires. Aucun arrêt de Gad, commit, push, changement de version ni envoi au PPA.

## Poursuite : fin de lot après démarrage et réactivation complète

- **Action de fin de lot désarmée par Start ou pause ciblée** : le moteur signale ces commandes dans sa garde, mais l’automate de fin de lot ne les reconnaissait pas parmi les changements autorisés. Démarrer un lot préparé ou mettre une sélection en pause annulait donc l’action enregistrée pour ce lot. `start_prepared` et `pause_selection` suivent maintenant la même règle que la pause globale : l’action reste armée, attend la fin effective du travail et conserve le compte à rebours complet. Les gardes sur erreur, annulation, suppression et restauration ne sont pas assouplies. Le nouveau test Qt reproduit l’échec avant correction (`/tmp/gad-review-completion-before.log`) et couvre les deux commandes jusqu’au déclenchement simulé, sans effet système réel.
- **Index des sources d’une réactivation complète** : chaque entrée retrouvée rescannait les entrées déjà connues, avec un coût quadratique sur les grands lots. Un ensemble des chemins connus conserve maintenant l’ordre et les indices existants, puis ajoute les nouvelles entrées une seule fois. Le contrôle initial de chaque source n’appelle plus deux fois `symlink_metadata` ; une disparition après ce contrôle est traitée par l’analyse elle-même au lieu de pouvoir être ignorée entre deux sondages.

Le test Rust ajouté prépare 256 fichiers, annule le lot, ajoute un fichier, puis réactive le lot entier. Il vérifie les anciens indices, l’unicité des entrées, la découverte du nouveau fichier et le contenu de toutes les copies.

La passe suivante sur les simulations, deltas JSON/journal, instantanés Qt, profils, diagnostic et scripts de tests n’a pas identifié de nouvelle correction suffisamment établie. Cette revue ne garantit pas l’absence de tout bug.

La boucle de revue a ensuite repéré un second coût quadratique dans `prepare_retry` : la recherche des parents nécessaires comparait chaque dossier à tous les chemins sélectionnés et copiait ces chemins. Un index temporaire des dossiers par chemin source permet maintenant de parcourir les ancêtres des seules lignes sélectionnées, en conservant le contrôle du préfixe destination. Les tests de réactivation complète et partielle, création des parents manquants et préservation des branches voisines passent après cette adaptation. Le premier index des sources utilise lui aussi des chemins empruntés ; seul un indicateur par entrée rescannée est conservé avant l’ajout des nouvelles lignes.

Validation finale : les neuf étapes globales passent (`target/review-completion-retry/results.json`), avec **365 tests Rust**, **130 tests Python**, **179 tests Qt principaux** et les suites complémentaires. Après la dernière adaptation de la recherche des parents, formatage, Clippy avec avertissements interdits et les 365 tests Rust ont été relancés avec succès ; les 365 tests passent également en compilation optimisée. Les 17 benchmarks restent explicitement ignorés. Les intégrations locale/reprise après SIGKILL, CLI et FTP réel passent avec le dernier binaire optimisé. Les traductions sont validées sans modification des catalogues et le diff ne comporte pas d’erreur d’espacement. Les empreintes des 148 fichiers sources de l’inventaire final restent identiques après les vérifications.

Le binaire a été installé atomiquement dans `~/.local/bin/gad`, avec vérification de son SHA-256 (`target/review-completion-retry/delivery.json`). Aucun arrêt de Gad ou de ses transferts, commit, push, changement de version ni envoi au PPA.

## Comparaison avec la version poussée

La recherche ciblée de régressions par rapport à `origin/main` (`5765fde`) a confirmé puis corrigé la perte des commandes individuelles sur des occurrences ayant les mêmes chemins lors d’une réanalyse. Le même test échoue avant correction et réussit avec la fonction exacte de la référence Git, puis avec la fonction corrigée. Le détail, les limites de la comparaison et les vérifications finales sont consignés dans [REGRESSION_REVIEW.md](REGRESSION_REVIEW.md).
