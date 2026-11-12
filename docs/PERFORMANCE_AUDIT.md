# Audit d’efficacité de Gad — 11 novembre 2026

> Mise à jour après mise en œuvre : les optimisations retenues, leurs limites et les comparaisons du binaire final sont détaillées dans [PERFORMANCE.md](PERFORMANCE.md#suivi-fin-copies-et-interface--11-novembre-2026). Cet inventaire de 42 pistes comprend encore des chantiers structurels et des expériences non appliqués.

Cet audit décrit le code de travail avant cette mise en œuvre, après les optimisations précédentes décrites dans [PERFORMANCE.md](PERFORMANCE.md). Il inventorie **42 pistes**, sans prétendre qu’elles produiront toutes un gain. Les coûts directement constatés sont distingués des modifications architecturales et des expériences à comparer. Aucune optimisation de cette nouvelle passe n’avait encore été appliquée au moment de ce relevé.

## Constats mesurés

Binaire optimisé `target/release/gad`, instances déjà démarrées avec HOME, runtime, configuration, journal et fichiers privés. Aucun presse-papiers ni transfert de la session utilisateur n’est utilisé.

### Suivi d’un lot préparé

Une sélection est préparée sans démarrer sa copie. Cinq modifications successives de sa seule limite de débit invalident sa révision. Après chacune, une requête `poll` demande les changements, suivie d’une requête sans changement. Les destinations sont vérifiées vides à la fin.

| Fichiers dans le lot | Réponse JSON différentielle¹ | Lecture différentielle² | Lecture inchangée² | Commande de débit² |
| --- | ---: | ---: | ---: | ---: |
| 100 | 59 473 octets | 12,05 ms | 0,18 ms | 4,59 ms |
| 1 000 | 569 773 octets | 30,59 ms | 0,48 ms | 36,57 ms |
| 5 000 | 2 837 773 octets | 95,31 ms | 1,26 ms | 191,74 ms |

¹ Taille du JSON compact réencodé par le client Python, pas un compteur exact des octets sur la socket.

² Médiane de cinq mesures dans une instance par taille, incluant IPC et décodage Python. Les commandes modificatrices rafraîchissent aussi la fenêtre ; ces nombres ne mesurent pas le seul journal. La fenêtre est masquée après préparation, avec Qt hors écran. Une série aussi courte sert à identifier le coût, pas à prédire un gain en production.

**Le différentiel est actuellement au niveau du lot. Une propriété changée renvoie encore tous ses fichiers.**

### Copie et appels système

Trois copies de 100 fichiers de neuf octets donnent une médiane de **1 580,56 ms**, fin observée par IPC, historique conservé et vérifications facultatives désactivées. Les contenus destination sont comparés aux sources et les sources sont conservées.

Avec 500 fichiers de même contenu, les trois essais donnent une médiane de **12 404,88 ms**. Multiplier le nombre de fichiers par cinq multiplie ici la durée par environ 7,85 ; cela signale une mauvaise évolution avec la taille du lot, sans suffire à isoler sa cause ou à établir une complexité mathématique.

Une exécution distincte sous `strace`, avec les mêmes 100 fichiers, compte :

- 1 036 `fsync` et un `fdatasync` ;
- 8 063 `statx` ;
- 2 300 `openat` ;
- 308 ouvertures de `/proc/self/mountinfo` ;
- 203 ouvertures de `/proc/self/fdinfo/*`.

Ces compteurs incluent le démarrage, la fermeture et les activités Qt. Ils ne signifient pas que chaque appel est évitable, et ne donnent pas le temps passé dans chaque catégorie. La durée sous trace ne sert pas de référence.

### Ce que le code confirme

- `snapshot::poll` sérialise chaque `TrackedJob` modifié en entier sous le verrou d’état.
- `Writer::save` sérialise également le lot entier avant de rechercher un delta textuel. Le WAL est plus petit, mais la préparation du delta reste proportionnelle au lot.
- `Engine::persist` conserve le verrou d’état pendant la sérialisation, l’écriture, `fsync` et éventuellement la compaction.
- La copie travaille par blocs de 64 Kio. Pour un bloc écrit sans limitation, elle passe normalement par cinq acquisitions du verrou d’état, une acquisition du limiteur, deux mises à jour de `copied` et deux `notify_all` ; les contrôles de reconnexion et de reprise peuvent en ajouter.
- L’interface repeint seulement les lots modifiés, mais reparcourt toutes les entrées de chacun de ces lots. Plusieurs consommateurs reconstruisent leurs propres agrégats.

## Priorité 1 — Les six changements structurants

### 01. Journaliser les changements métier par entrée

**Coût confirmé.** Remplacer la sérialisation complète suivie d’une comparaison textuelle par des enregistrements typés : progression durable d’une entrée, publication, sauvegarde, restauration, réglage du lot, changement d’ordre. Garder les instantanés pour la compaction.

Sources : `src/engine/journal.rs`, `src/engine/journal/delta.rs`, `src/engine/history.rs`.

Cela vise surtout les milliers de petits fichiers. Les lots dépassant le cache de 8 Mio par lot utilisent aujourd’hui le chemin complet : le nouveau format doit également améliorer ce cas. Tester version de format, séquences, checksums, écritures refusées, queue tronquée, rejeu et toutes les interruptions avant/après effet de fichier.

### 02. Diffuser les changements par entrée vers Qt

**Coût confirmé et mesuré ci-dessus.** Ajouter révisions d’entrée, changements des propriétés du lot et révision de structure distincte. Un compteur de fichier ou une limite de débit ne doit pas renvoyer le manifeste entier.

Sources : `src/engine/snapshot.rs`, `native/snapshot.h`, `native/window_rows.cpp`.

Garder une remise à zéro complète pour resynchroniser le client. Tester suppressions, réanalyses, indices stables, changement de langue, révisions manquées et rendu identique au statut complet.

### 03. Séparer manifeste stable et état variable

**Architecture.** Les chemins, types et identités sont stables pendant la majorité du transfert. Les distinguer de progression, état, erreurs et reçus évite de les recopier, sérialiser et décoder à chaque mise à jour.

Sources : `src/engine.rs` (`Entry`, `Job`), `src/engine/snapshot.rs`, journal et cache Qt.

Cette séparation facilite 01 et 02 et réduit aussi les clones des travailleurs. Une destination renommée après conflit ou une reconnexion doit publier une vraie modification du manifeste.

### 04. Retirer les E/S du verrou global, avec acquittement durable

**Coût confirmé ; solution à concevoir et mesurer.** Capturer une transaction immuable courte sous verrou, la faire écrire par un rédacteur ordonné, puis attendre son acquittement avant l’effet de fichier dépendant. Les lectures de progression et les commandes d’arrêt pourront accéder à l’état pendant une synchronisation lente.

Source : `src/engine/history.rs::persist`.

Une séparation plus simple a déjà été essayée puis retirée faute de gain stable : voir la deuxième passe de `PERFORMANCE.md`. Il faut une file bornée, un ordre strict des transactions et une propagation des échecs ; déplacer simplement les mêmes verrous ne suffit pas. Mesurer surtout la latence de pause et d’annulation pendant un `fsync` retardé, en plus du débit.

### 05. Distinguer progression volatile et mutations durables

**Coût confirmé.** Toute prise mutable de `TrackedJob` change sa révision. Les compteurs de progression empruntent ce mécanisme, et des mutations identiques peuvent aussi invalider inutilement le lot.

Sources : `src/engine/snapshot.rs::TrackedJob`, `src/engine.rs::update`, `src/engine/journal.rs::save`.

Créer des révisions séparées pour présentation, structure et état durable, et ne marquer que les changements effectifs. Attention : les barrières de publication doivent toujours persister leurs étapes, même si une autre révision semble inchangée.

### 06. Examiner et fusionner les synchronisations redondantes

**Occurrences confirmées ; suppressions à prouver.** Le point de reprise final synchronise le fichier et son parent, puis les permissions et dates sont appliquées et le fichier est à nouveau synchronisé. Chaque point intermédiaire synchronise également le parent, même si le nom du temporaire n’a pas changé.

Sources : `src/engine/resume.rs::save_copy_point`, `src/engine.rs::copy_entry`, `src/engine/fast_move.rs::sync_names`.

Évaluer un ordre unique données/métadonnées → synchronisation → journal, la persistance du nom à sa création plutôt qu’à chaque point, et la déduplication de parents identiques. Chaque réduction nécessite un test de récupération correspondant ; retirer les `fsync` globalement changerait les garanties.

## Priorité 2 — Préparation, renommage et restauration

### 07. Réutiliser une capture de métadonnées dans une même vérification

`resume::Version::read` lit les métadonnées puis `Stamp::read` les relit. `Version::matches`, la vérification facultative de renommage et certaines vérifications de restauration consultent également plusieurs fois le même chemin pour identité et ctime.

Réunir ces contrôles à partir d’une seule capture fraîche dans la même étape ; conserver les captures distinctes qui encadrent une E/S. Sources : `resume.rs`, `undo.rs`, `fast_verify.rs`.

### 08. Partager le contexte de préparation des montages et périphériques

Plusieurs étapes indépendantes relisent `mountinfo`, normalisent les parents ou recherchent le périphérique. Un contexte borné à une préparation peut partager table de montages, métadonnées déjà connues et parents normalisés.

Sources : `validation.rs`, `scheduling.rs`, `fast_move.rs`, `duplicates.rs`, `reconnect.rs`. Ne pas transformer ce contexte en cache permanent : montage, périphérique et alias restent revalidés avant effet.

### 09. Alléger les instantanés utilisés pour dédoublonner

`filter_duplicates` clone des `Job` antérieurs entiers, alors qu’il a besoin de clés et de quelques états. Extraire ces informations ou emprunter un manifeste immuable évite de recopier reçus, erreurs et rapports.

Source : `src/engine/duplicates.rs`. Tester en particulier une annulation, un nettoyage ou une fin de lot pendant cette préparation hors verrou.

### 10. Indexer les doublons actifs

Les clés normalisées des lots antérieurs sont recalculées lors des nouveaux ajouts. Maintenir un index borné des opérations pertinentes peut amortir ces ajouts successifs.

Source : `duplicates.rs`. Les retraits, reprises, conflits, changements de destination et alias doivent mettre à jour l’index. Conserver la décision finale sur état frais.

### 11. Indexer les destinations et temporaires réservés

`staging::reserved_name` parcourt les destinations de la file pour chaque temporaire créé. Les suffixes en conflit font aussi rechercher des chemins réservés.

Sources : `staging.rs`, `Engine::available_queued_name`. Un index des réservations réduit ce travail répété, potentiellement quadratique sur un grand lot. Tester tous les états et les collisions par alias, pas seulement les noms usuels.

### 12. Indexer les identifiants de lots

Les contrôles par bloc, changements d’entrée et décisions d’ordonnancement recherchent fréquemment un identifiant dans `Vec<TrackedJob>`. Ajouter un index identifiant → position, ou un stockage stable indexé, vise les grands historiques.

Sources : `engine.rs`, `scheduling.rs`, `diagnostics.rs`. L’ordre de traitement reste séparé ; les suppressions et migrations doivent garder l’index cohérent.

### 13. Invalider les index structurels par révision explicite

Le cache d’arborescence valide la forme de tous les lots ; celui des dossiers compare aussi chemins et types des entrées. Ajouter une révision de structure permet un contrôle constant lorsqu’aucun chemin n’a changé.

Sources : `row_queue.rs::layout`, `directories.rs::Index::matches`. Tester modifications de chemin à taille constante et mutations aujourd’hui faites directement sur les vecteurs.

### 14. Réduire les clones de plans et de reçus de renommage

Un travailleur clone le lot ; `rename_tree_using` recopie son sous-arbre et construit métadonnées, plan et reçus. Un manifeste partagé et des reçus consommés par déplacement peuvent réduire mémoire et allocations.

Sources : `engine.rs::work`, `tree_move.rs`. Conserver un instantané cohérent face aux commandes concurrentes.

### 15. Rendre les parcours longs régulièrement interruptibles

L’énumération et le tri d’un dossier très large, ainsi que `inspect_tree`, peuvent travailler longtemps entre deux contrôles utilisateur. Ajouter des points d’arrêt bornés en temps dans les parcours en mémoire et entre E/S.

Sources : `engine.rs::scan_selected`, `tree_move.rs::inspect_tree`, `undo.rs::check_output`. Certaines relectures MD5 de restauration utilisent un rappel sans action : elles méritent progression et règles explicites d’interruption sûre. Cela améliore la réactivité ; une E/S noyau déjà bloquée peut rester non interruptible par ce mécanisme.

### 16. Éviter une pile proportionnelle à la profondeur

Le scan récursif peut devenir coûteux ou épuiser la pile sur une arborescence très profonde. Un parcours itératif avec pile explicite garde le préordre et permet une limite mémoire observable.

Source : `engine.rs::scan_selected`. Gain à mesurer sur des arbres profonds ; le cas courant peut rester équivalent.

### 17. Compacter les manifestes de déplacements d’arbre

Un renommage complet n’écrit physiquement qu’un changement de nom, mais conserve de nombreux chemins source/destination et reçus similaires. Stocker une racine et des chemins relatifs, avec données communes factorisées, réduirait journal et mémoire.

Sources : `tree_move.rs`, `undo.rs::Record`, sérialisation de `Entry`. Cela reste un travail proportionnel au nombre de fichiers si l’on conserve les garanties par entrée. Un renommage avant tout inventaire ne fournit pas automatiquement ces mêmes garanties.

## Priorité 2 — Boucle de copie

### 18. Supprimer la seconde publication identique de `copied`

Après une écriture sans fractionnement, le compteur est publié dans la boucle de budget puis de nouveau après sa sortie. Garder les mises à jour utiles aux très faibles débits, et éviter la seconde quand sa valeur est identique.

Source : `engine.rs::copy_entry`. Tester écritures fractionnées, pause, compteur final et sources qui grandissent.

### 19. Réserver les réveils aux changements utiles aux travailleurs

`Engine::update` appelle `wake.notify_all`, y compris pour chaque progression de bloc. Une progression ne rend généralement pas un lot exécutable. Distinguer publication de compteurs et réveil d’ordonnanceur.

Sources : `engine.rs::update`, `rate.rs`. Tester qu’un changement de pause, de débit, de conflit ou une fin de lot réveille toujours les attentes concernées.

### 20. Alléger le chemin sans limitation de débit

Le chemin illimité prend encore le verrou du limiteur et met à jour ses buckets. Un chemin rapide, validé sous le contrôle courant des options, peut éviter ce travail.

Source : `rate.rs::transfer_budget`. Le passage à une limite pendant une copie doit prendre effet rapidement et ne permettre aucune réservation ancienne d’octets illimités.

### 21. Séparer compteurs et contrôles rapides du gros état métier

Des compteurs atomiques et un contrôle de pause/annulation par travailleur peuvent réduire les acquisitions du mutex global à chaque bloc. Les transitions et points durables continuent de passer par l’état central.

Sources : `engine.rs::checkpoint_for`, `engine.rs::update`, `rate.rs`. Refactorisation concurrente à tester avec changements de génération, retrait d’entrée et restauration.

### 22. Comparer des tampons plus grands et réutilisables

Le tampon de copie maximal est 64 Kio, contre 1 Mio pour MD5. Comparer 64 Kio, 256 Kio et 1 Mio, réutilisés par travailleur et dimensionnés pour les petits fichiers.

Source : `engine.rs::copy_buffer`. Mesurer débit local et distant, RSS, latence d’arrêt et limitation très faible. Un plus grand tampon peut augmenter la latence d’une E/S distante ; aucun gain universel n’est acquis.

### 23. Optimiser le calcul d’empreinte sans le supprimer

Une empreinte est calculée pendant les copies ordinaires pour reconnaître les résultats lors de la restauration, même quand l’option de comparaison MD5 est décochée. Mesurer son coût CPU et comparer une implémentation compatible plus efficace si ce coût domine.

Sources : `engine.rs::copy_entry`, `undo.rs::check_output`. Changer d’algorithme demanderait un format versionné ; conserver MD5 permettrait de garder les reçus existants tels quels.

### 24. Éviter les allocations répétées pour les petits objets

Réutiliser tampons, vecteurs de travail et contextes là où leur durée de vie est celle du travailleur. Réserver les capacités connues, et ne pas conserver des caches non bornés.

Sources : boucle de copie, vérification, `tree_move.rs`, `staging.rs`. Piste secondaire après profilage des allocations ; la précédente tentative de réemploi du tampon WAL a déjà été retirée faute de gain stable.

## Priorité 2 — Interface et notifications

### 25. Mettre à jour seulement les cellules dynamiques

Un lot modifié reconstruit actuellement dates, chemins affichés, infobulles et données de recherche de toutes ses lignes. Distinguer changements de structure, présentation et progression.

Source : `native/window_rows.cpp`. Garder les mêmes items pour conserver survol, sélection, infobulles et expansion ; les optimisations précédentes sur ces points restent utiles.

### 26. Partager les agrégats et les actualiser par différence

Résumé, mesures et notification parcourent séparément les entrées. Diffuser ou calculer une fois les totaux du lot, puis les corriger à partir des entrées modifiées.

Sources : `window_rows.cpp::refreshSummary`, `transfer_metrics.h`, `notifications.h`. Tester reprises, remises à zéro, renommages sans trafic, MD5 et reconnections pour éviter les faux débits.

### 27. Réduire le travail des notifications désactivées ou inchangées

`progressMessage=false` empêche la publication D-Bus, mais ne supprime pas les calculs de suivi d’une session active. Les mises à jour natives peuvent aussi être envoyées avec des propriétés identiques ; le compte à rebours est actualisé plus souvent que sa seconde affichée.

Source : `native/notifications.h`. Garder le minimum nécessaire à une réactivation correcte, et envoyer seulement les changements utiles. Maintenir les tentatives de reconnexion et les commandes fraîches.

### 28. Préparer la recherche une fois par requête

La recherche normalise et découpe son texte et construit une expression régulière lors de chaque test de ligne. Compiler les mots une fois ; mettre en cache les chemins normalisés jusqu’à leur changement.

Source : `native/history_search.h::Query::matches`. Tester Unicode, recherches de chemins, dates et changement de langue.

### 29. Trier uniquement lorsque les clés changent

Tout rendu modifié appelle `applyPresentation`, qui peut retrier tous les frères. Une progression n’invalide pas un tri par nom ; seules les branches dont une clé varie ont besoin du tri.

Sources : `native/table_view.h`, `native/transfer_tree.h`, `window_rows.cpp`. Conserver le tri multiple, le rang métier pour les égalités, la sélection et le défilement.

### 30. Réduire le travail périodique sans activité

Le timer interroge et actualise à 400 ms même au repos ; les analyses passent à 50 ms. Conserver une cadence active raisonnable, ralentir au repos et déclencher immédiatement les transitions de travail utiles.

Sources : `native/window.cpp`, `window_rows.cpp`, `engine/snapshot.rs`. Les comptes à rebours, reconnexions, mesures de débit et changements externes imposent leurs propres échéances. Une notification moteur de changement serait préférable à un polling plus rapide généralisé.

### 31. Mutualiser les diagnostics structurels et supprimer les rafraîchissements en double

Même un `poll` inchangé reconstruit les diagnostics. Les réservations se prêtent à un cache de génération ; les attentes de débit restent temporelles. Certains chemins d’ajout appellent aussi `refresh` après un `handle` qui rafraîchit déjà.

Sources : `src/engine/diagnostics.rs`, `native/window_commands.cpp`, `native/window_rows.cpp`. Ne pas figer les raisons d’attente lors d’un changement de débit ou de travailleur.

### 32. Charger l’historique visuel progressivement

Pour les grands historiques, un modèle `QAbstractItemModel` ou une matérialisation paresseuse des branches fermées peut éviter des milliers d’objets Qt et de chaînes dupliquées.

Sources : `native/window_rows.cpp`, `row_tree.h`, `snapshot.h`. Chantier important : recherche globale, sélection d’un lot entier, tri et actions sur descendants doivent fonctionner même sans item visuel créé.

## Priorité 3 — Plateforme, démarrage et mémoire

### 33. Alléger le client CLI d’une instance déjà ouverte

`QApplication`, icônes et traductions sont initialisées avant l’envoi à l’instance existante. Les commandes sans interface pourraient utiliser un client plus léger. Le collage pourrait être transmis au propriétaire existant lorsque les garanties de contexte sont conservées.

Source : `native/application.cpp`. Le presse-papiers Wayland et le dossier actif exigent leurs propres tests ; ne pas déplacer aveuglément leur lecture.

### 34. Mettre en cache la configuration du watchdog Dolphin

Il relit et analyse les fichiers de configuration chaque seconde. Surveiller leur remplacement et réutiliser la configuration, tout en conservant une vérification légère et régulière des QAction.

Source : `native/dolphin/watchdog.h`. Tester édition atomique, suppression, mise à jour Dolphin et réactivation ; la réparation régulière du raccourci est une fonctionnalité à conserver.

### 35. Réduire les lectures de montage sur les descripteurs

La sonde optimisée par chemin existe déjà ; celle sur descripteur utilise encore `fdinfo`, puis `mountinfo` pour KIO Fuse. Étudier une sonde noyau sur le descripteur et une réutilisation bornée du classement du montage pendant l’opération.

Source : `src/engine/fast_move.rs::descriptor_mount_id`. Garder le repli actuel et l’identification du descripteur réellement ouvert, y compris après remplacement d’un chemin.

### 36. Alléger les contrôles d’espace et de reconnexion

L’espace libre capture des lots complets avant d’en extraire les demandes ; la capture de connexion parcourt de nombreux chemins pour déduire des ancrages communs. Extraire des données minimales, partager les volumes et métadonnées de préparation.

Sources : `space.rs`, `reconnect.rs`. Ne pas mettre durablement en cache l’espace disponible ni considérer qu’un numéro de périphérique réutilisé prouve une reconnexion correcte.

### 37. Limiter la mémoire et le chargement de l’historique froid

Le manifeste, les reçus, les JSON, les caches Qt et les items peuvent représenter plusieurs formes du même historique. Mesurer le pic RSS et envisager des segments archivés ou un chargement paresseux des lots terminés.

Sources : journal, `Engine::persistent`, snapshots Qt. La restauration, les sauvegardes conservées et la recherche sur l’ensemble de l’historique doivent rester disponibles. Les rapports d’erreurs peuvent également devenir incrémentaux.

## Expériences conditionnelles — Gain non établi

### 38. Copie assistée par le noyau et clones de fichiers

Comparer une voie de copie native ou un clone lorsque le système de fichiers le permet, avec repli vers la copie actuelle. Le calcul d’empreinte pour restauration, les fichiers clairsemés, la reprise et les garanties de publication compliquent cette voie.

Le code calcule actuellement une empreinte pendant le flux ; une copie native suivie d’une relecture peut annuler le gain. Tester avant d’ajouter un backend.

### 39. Préserver les fichiers clairsemés

La boucle actuelle écrit tout le flux. Un parcours des zones réellement allouées peut réduire octets écrits et espace occupé pour images disque et fichiers à trous.

Il faut conserver taille logique, zéros, empreintes, reprise et estimation d’espace ; le système de fichiers doit être capable de fournir des informations exploitables. Mesurer uniquement sur des fixtures prévues pour ce cas.

### 40. Adapter le parallélisme et les résolutions distantes

Le moteur limite à deux travailleurs et autorise déjà les disques disjoints et les renommages locaux indépendants. Comparer un parallélisme borné différent pour SSD, réseau et petits fichiers, ainsi qu’une résolution KIO limitée en concurrence ou mutualisée.

Sources : `scheduling.rs`, `engine.rs::start`, `native/kio_sources.h::Resolver`. Davantage de concurrence sur un disque mécanique peut ralentir ; préserver ordre, authentification, délai commun et annulation. Un renommage distant côté serveur serait un backend distinct, avec preuves et reçus adaptés.

### 41. Grouper les transactions durables et compacter sans longue pause

Avec un rédacteur ordonné, plusieurs travailleurs pourraient attendre la même synchronisation de journal, sans effectuer leurs effets avant l’acquittement commun. Une compaction incrémentale peut réduire les pauses.

Sources : `journal.rs`, `history.rs`. C’est plus complexe qu’une suppression de `fsync`. Le gain est incertain avec seulement deux travailleurs ; tester pertes de courant simulées, erreurs et reprise pendant la compaction.

### 42. Réduire la frontière JSON interne et comparer les réglages de compilation

Rust construit un `Value`, l’encode en texte, puis Qt le décode ; une interface interne typée pourrait éviter ces conversions après introduction des deltas par entrée. Garder JSON pour les clients externes et le diagnostic.

Sources : `src/main.rs::sc_request`, `native/bridge.h`. Les optimisations de compilation guidées par des profils représentatifs sont aussi une expérience possible ; `release` et LTO sont déjà activés. Ne pas utiliser un binaire propre au CPU de la machine pour les paquets distribués.

## Ordre conseillé

1. Installer les mesures de référence et les scénarios de récupération ci-dessous.
2. Commencer par les corrections locales : 07, 18, 19, 20, 27, 28, 29 et 31. Elles permettent des comparaisons isolées et limitent les changements de format.
3. Introduire 05 puis 02 et 25 : réduire immédiatement la charge d’un grand lot actif sur l’interface.
4. Concevoir ensemble 03 et 01 : réduire le coût du journal et les clones sans multiplier les migrations.
5. Traiter 06 et 04 avec tests de barrières et de latence, puis seulement envisager 41.
6. Mesurer à nouveau ; sélectionner les autres pistes selon le coût restant. Les expériences 38 à 42 n’ont pas un gain acquis.

Une réécriture totale, une hausse générale du parallélisme, la suppression du journal ou l’abandon des empreintes ne constituent pas des gains équivalents à garanties constantes.

## Validation avant de conserver une optimisation

Pour chaque étape : changement isolé, tests ciblés, comparaison alternée avant/après avec mêmes fichiers et options, puis tests de non-régression adaptés. Ne conserver qu’un gain reproductible sur le cas visé sans dégradation inexpliquée ailleurs.

Scénarios à couvrir :

- Renommage d’arbre et retour arrière : 1, 100, 1 000, 5 000 puis 50 000 fichiers ; mesurer séparément acceptation, changement réel du chemin, état terminé et temps de restauration.
- Copies de petits fichiers : nombre, profondeur, conflits, liens symboliques et physiques, exclusions, destinations déjà remplies, historique important et journal dépassant les limites des caches.
- Gros fichiers : tampons, calcul d’empreinte, MD5 comparatif, faible débit, flux non limité et fichiers clairsemés.
- Interface visible et masquée : temps du thread Qt, nombre et taille des réponses, coût du rendu et du tri, mémoire, CPU au repos.
- Réactivité : pause, reprise, annulation et retour arrière pendant synchronisation lente, calcul d’empreinte, scan, vérification, compaction et déconnexion.
- Reprise : interruption avant/après chaque barrière durable, même temporaire retrouvé, contenus comparés, destinations externes modifiées conservées.
- Montages : alias d’une même partition, sous-montages, volumes remplacés, KIO Fuse et erreurs de métadonnées ; bancs simulés lorsque le matériel manque.

Les nouveaux formats exigent les tests de lecture des journaux existants et de refus explicite des formats futurs non compris. La comparaison au gestionnaire natif doit distinguer un simple renommage de l’inventaire complet et de la préparation d’une restauration par fichier.

## Artefacts de cet audit

Les données locales non versionnées sont dans `target/efficiency-audit/` :

- `poll_audit.py` et `poll-results.json` : protocole utilisé et cinq mesures par taille ;
- `trace_copy.py`, `copy-100-syscalls.log` et `copy-syscalls.json` : compteurs d’appels système ;
- `baseline.json` : empreinte du binaire et résultats résumés.

Les trois essais de copie sans trace sont dans `target/copy-performance/audit-100-results.json` et `audit-500-results.json`. Les scripts de benchmark existants vérifient les contenus et détruisent uniquement leurs fixtures privées.
