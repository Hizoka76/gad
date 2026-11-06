# Revue du 4 novembre 2026

Revue du moteur Rust, du journal de restauration, de l’interface Qt et des intégrations KIO/Plasma. Les tests utilisent des fichiers temporaires et un bus D-Bus privé. Aucun serveur FTP, SFTP ou SMB réel n’a été sollicité pendant cette revue.

## Problèmes corrigés

| Priorité | Déclencheur et conséquence | Correction et vérification |
| --- | --- | --- |
| Haute | Deux sources homonymes se remplacent dans un même lot : sa remise en état échoue, car le premier résultat se trouve dans la sauvegarde du remplacement suivant. | Indexer les sauvegardes des entrées sélectionnées par destination et contrôler les versions intermédiaires avant la restauration inverse. Vérifié en copie et déplacement, avec sauvegarde altérée et sélection partielle. |
| Haute | « Garder les deux » sur un nom proche de NAME_MAX : la recherche du nouveau nom boucle indéfiniment sur ENAMETOOLONG. | Propager les erreurs de recherche autres que l’absence du candidat. Vérifié avec un nom de 252 octets ; source et destination existante conservées. |
| Haute | Annulation globale pendant la préparation KIO : le lot peut encore démarrer après la réponse du service, ainsi que les suivants locaux. | Invalider la génération de préparation, retirer la file et abandonner le résolveur. Les deux actions globales et un nouvel ajout après annulation sont testés. |
| Moyenne | Le premier enregistrement d’un lot échoue : son analyseur ne démarre pas et le lot reste en analyse. | Passer ce lot en erreur avant de renvoyer le refus. Test avec un journal dont la publication est rendue impossible. |
| Moyenne | Après réactivation d’une ligne annulée, le lot devient « OK » malgré une erreur persistante sur une autre ligne. | Prendre en compte les erreurs restantes lors du calcul de l’état final. Test réel de conflit, annulation puis reprise partielle. |
| Moyenne | Un dossier vide échoue, puis son état est recalculé « OK » à partir de ses enfants absents. | Préserver l’erreur propre du dossier pendant l’agrégation. Test avec un fichier occupant le chemin du dossier cible. |
| Moyenne | Une demande de suivi Plasma échoue ou le service redémarre : le suivi courant n’est pas recréé. | Réessayer avec un délai de deux secondes, invalider les anciens objets et réponses D-Bus au changement de propriétaire. Tests de panne temporaire, reprise et déconnexion des anciennes commandes. |

Les six premiers cas ont été reproduits par des tests en échec avant correction. Le dernier est couvert par une simulation explicite de panne et de redémarrage. Les opérations de restauration continuent de refuser les résultats et sauvegardes modifiés.

## Améliorations réalisées après la revue

- **Protocole commun** : `native/protocol.def` définit les états, leur terminaison, les commandes et la version de protocole. Le préprocesseur Qt et le générateur Rust consomment ce même manifeste. Les états du moteur sont des enums ; les chaînes sérialisées du journal restent identiques. Le protocole passe à 10, les anciennes commandes restent disponibles.
- **Découpage** : les tests Rust sont dans `src/engine/tests.rs`. L’interface sépare `window.cpp`, `window_rows.cpp`, `window_conflicts.cpp`, `window_commands.cpp` et `application.cpp`, avec des déclarations dans les headers. Les tests Qt lient ces unités au lieu d’inclure leur implémentation. Les commentaires métier sont conservés.
- **Diffusion différentielle** : les révisions volatiles suivent les accès mutables aux lots et les changements de file. La nouvelle commande `poll` ne sérialise que les lots modifiés ; `status` conserve son instantané complet compatible. Qt fusionne les lots dans leur ordre et mémorise leurs compteurs. Les ticks inchangés ne réécrivent pas les lignes. Filtres, pause, suppressions, survol et notifications restent actualisés.
- **Validation asynchrone** : résolution de la destination, métadonnées, contrôle des racines et énumération s’exécutent hors du thread Qt. Le lot conserve sa place dès sa soumission ; les lots suivants attendent sa préparation. L’annulation termine logiquement une préparation bloquée et ses réponses tardives sont ignorées. Une pause demandée pendant la validation reste prioritaire sur le démarrage automatique du collage.
- **Construction** : modifier le Rust seul ne reconstruit plus les unités Qt. Le format du journal et les limites de métadonnées annoncées dans le README restent inchangés.

### Mesure du rafraîchissement

Mesure reproductible avec `cargo test benchmark_large_history_polling -- --ignored --nocapture`, compilation de développement, historique synthétique de **200 lots × 50 lignes**, un lot modifié à chaque actualisation :

| Dix actualisations | Instantanés complets | Diffusion différentielle |
| --- | ---: | ---: |
| JSON produit | 95 092 090 octets | 483 820 octets |
| Construction et sérialisation Rust | 4,825 s | 0,071 s |

Cela représente environ **196 fois moins de JSON** dans ce scénario. Ces durées dépendent de la machine et de la compilation ; elles ne mesurent pas le débit des fichiers ni le rendu Qt. La réactivité pendant une attente de métadonnées est vérifiée par une préparation bloquée contrôlée dans les tests ; aucune latence FTP/SFTP/SMB réelle n’a été mesurée.

## Validation

- `cargo test` : 58 tests réussis et un test de mesure exécuté séparément.
- `cargo clippy --all-targets -- -D warnings` : aucune alerte.
- `cargo fmt --check` : conforme.
- Tests Qt du presse-papiers et de l’interface, avec résolution KIO simulée, cache différentiel, filtres, pause, survol et annulation de préparation : réussis.
- Tests D-Bus du suivi Plasma, y compris panne et redémarrage : réussis.
- Tests d’intégration du vrai binaire, de la socket, de la migration et des opérations de fichiers : réussis.

## Deuxième passe de vérification

Cinq nouveaux problèmes ont été reproduits avant correction :

| Cas | Correction et contrôle |
| --- | --- |
| Un fichier grandit ou rétrécit dans la file : la copie utilise son contenu actuel, mais l’affichage garde l’ancienne taille. | Mettre à jour la taille après ouverture et contrôle d’identité de la source. Test des deux variations, avec vérification du contenu copié et des compteurs. |
| Un dossier annulé est remplacé par un fichier avant réactivation : le lot peut créer un dossier cible vide et annoncer OK. | Partager la classification des types avec l’analyse et refuser la reprise lorsque le type a changé. Le message demande de réajouter la source ; copie et déplacement sont testés, sans création du faux dossier cible. |
| Un lien symbolique est copié sur lui-même via un alias de son dossier parent. | Normaliser uniquement le parent du lien pour comparer les chemins réels ; ne plus résoudre la cible du lien pendant cette validation. Test avec un lien pendant et contrôle de conservation de son inode et de sa cible. |
| Les actions d’un menu contextuel déjà ouvert gardent un ancien état, et la réactivation peut être proposée avant la fin du lot. | Actualiser les actions après les lignes et attendre l’arrêt du lot pour réactiver ses enfants annulés. Tests avec une sélection inchangée et des réponses différentielles. |
| Un message d’erreur attend l’acquittement de l’utilisateur avant de rendre la réponse IPC, pouvant provoquer une expiration côté client. | Ouvrir le message sans boucle bloquante et renvoyer aussitôt l’erreur JSON. Test avec fermeture temporisée et vérification du texte brut des noms de fichiers. |

Validation de cette passe : 61 tests Rust réussis (la mesure de performance reste volontairement séparée), tests Qt, notifications KDE et intégration du binaire ; contrôle Clippy sans alerte. Le journal conserve son format.

## Revue itérative jusqu’à une passe sans nouveau défaut confirmé

Quatre passes ont apporté des corrections, chacune suivie de ses tests ; la passe finale de relecture et de non-régression n’a pas fait ressortir de nouvelle correction concrète dans les zones examinées.

### Passe 1 — commandes et effets de fichiers

- Refuser les reprises/suspensions sans sélection applicable : une commande vide ou visant seulement des lignes terminées ne reprend plus les autres transferts.
- Recontrôler l’identité et le type des dossiers au démarrage effectif, comme pour les fichiers et les liens. Un dossier disparu ou remplacé pendant l’attente n’est plus copié comme un dossier vide annoncé OK.
- Nettoyer par garde RAII les dossiers temporaires même lorsque les métadonnées ou l’enregistrement du journal échouent avant publication.
- Ajouter une matrice de douze scénarios : trois politiques de conflit × copie/déplacement × MD5 activé/désactivé, sur deux arbres fusionnés avec liens pendants et dossiers vides. Après arrêt, rechargement du journal et restauration, chemins et contenus doivent être identiques à l’état initial.

### Passe 2 — sources, URL et noms de fichiers

- Valider les URL avant conversion en chemins : un `file://` avec hôte distant, identifiants, port, requête ou fragment n’est plus accepté silencieusement comme un fichier local.
- Garder une URL mal formée pour la refuser explicitement, au lieu de la transformer en argument vide ; refuser aussi les sources et destinations vides avant création d’un lot.
- Reconnaître un schéma uniquement en tête de chaîne : un chemin relatif comme `folder/proto://file` reste un chemin.
- Normaliser `file://localhost/…` vers le chemin local réel, de la même façon dans la CLI, le presse-papiers et les dépôts.
- Refuser les octets non UTF-8 et NUL dans les URL de fichiers locaux avant leur conversion. Les caractères Unicode valides, y compris U+FFFD, et les pourcentages littéraux restent acceptés.

### Passe 3 — presse-papiers et destinations

- Relire le marqueur KDE Copier/Couper après réception des URI : un changement détecté invalide toute la demande, au lieu de déplacer une sélection copiée entre-temps. Ce contrôle détecte la transition testée ; il ne rend pas atomiques les lectures de formats distincts du protocole de presse-papiers.
- Partager la normalisation des destinations entre les commandes CLI et la fenêtre. Les URL distantes non montées sont refusées avant transformation en chemins locaux fictifs ; les URL `file` locales valides sont décodées.
- Vérifier les arguments Unix bruts avant Qt : un nom non UTF-8 ne peut plus être converti en nom de remplacement et sélectionner un autre fichier Unicode existant. Le test d’intégration crée explicitement ces deux fichiers différents.

### Passe 4 — lancement et installation

- Ignorer les chemins XDG vides ou relatifs, avec repli vers les emplacements utilisateur absolus. Le moteur et les scripts partagent maintenant cette règle ; un environnement incomplet ne dépose plus journal, raccourci ou lanceur dans le dossier courant.
- Centraliser les emplacements des scripts dans `scripts/xdg_paths.py` et rafraîchir aussi le cache MIME à la désinstallation.
- Vérifier avec de vrais processus isolés les deux cas `XDG_DATA_HOME=""` et `XDG_DATA_HOME="relative-data"`, pour le journal et la configuration du raccourci Dolphin.

### Passe finale

- Relecture des commandes, annulations, préparations tardives, validation des entrées, caches, notifications et emplacements utilisateur : aucun nouveau défaut confirmé après les corrections ci-dessus.
- 66 tests Rust réussis ; un test de mesure de performance reste volontairement séparé. La matrice de douze restaurations est incluse dans les tests réussis.
- Tests Qt/KIO et notifications sur bus privé réussis, intégration du vrai binaire réussie, Clippy sans alerte, formatage Rust et syntaxe Python conformes.
- Le format du journal reste inchangé. Les tests distants utilisent le double KIO ; aucun serveur FTP/SFTP/SMB réel n’a été utilisé pour cette revue. Ces vérifications ne prouvent pas l’absence de tout bug.
