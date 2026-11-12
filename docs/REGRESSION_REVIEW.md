# Recherche de régressions depuis la version poussée

Référence : `origin/main`, commit `5765fde5e702fedf3389e54d5f6ac3706294375f`, vérifié par `git fetch origin main`. La branche locale pointe sur ce même commit ; les écarts étudiés sont les modifications locales non commitées et leurs nouveaux modules.

## Régression confirmée et corrigée

### Commandes des lignes répétées après réanalyse

L’index ajouté dans `validation::preserve_controls` ne conservait qu’une ancienne entrée par triplet source, destination et type. Si plusieurs occurrences portaient des commandes distinctes, la dernière écrasait les autres dans l’index. À la réanalyse, toutes les occurrences recevaient donc les indicateurs de cette dernière ligne : une pause pouvait disparaître, ou une annulation/un masquage se propager à une autre occurrence.

Le test `rescan_keeps_individual_controls_for_repeated_paths` prépare deux occurrences identiques, dont la première est en pause et la seconde annulée/masquée. Résultats :

- Fonction locale avant correction : échec (`gad-regression-controls-local-before.log`).
- Fonction exacte extraite de `origin/main`, temporairement substituée pour exécuter le même test : réussite (`gad-regression-controls-git-reference.log`). Il s’agit d’une comparaison de cette fonction, pas d’un test de tout le binaire Git.
- Fonction corrigée : réussite (`gad-regression-controls-fixed.log`).

La correction chaîne les occurrences dans leur ordre initial et consomme une occurrence ancienne par nouvelle ligne. Elle conserve la recherche par chemin pour les listes réordonnées, sans allocation d’une collection séparée pour chaque chemin unique. Les anciens indices ne sont pas modifiés par ce rapprochement.

## Autres vérifications

Les diffs des transferts rapides, de la restauration, du journal, des instantanés/deltas, des commandes de configuration et profils, des notifications, des raccourcis et des installateurs ont été examinés. Aucun autre recul fonctionnel n’a été confirmé dans les cas examinés. Les validations plus strictes des valeurs invalides et les refus de supprimer des fichiers substitués sont des changements intentionnels de protection, pas des régressions établies.

Les 334 tests Rust nommés repérés dans les fichiers de la référence restent présents ; aucun n’a été retiré ou renommé. Cette comparaison de noms ne prouve pas à elle seule l’équivalence de leur contenu ni de tous les comportements.

Les rapports et preuves de cette passe sont conservés dans `target/regression-origin-main/`. Cette recherche ne garantit pas l’absence de régression sur tous les périphériques ou fournisseurs distants.

## Validation finale

Les neuf étapes globales passent : **366 tests Rust**, **130 tests Python**, **179 tests Qt principaux**, suites complémentaires, watchdog Dolphin, intégrations et validations CLI. Les 366 tests Rust passent aussi en compilation optimisée ; 17 benchmarks sont explicitement ignorés. Clippy avec avertissements interdits et formatage passent. Les scénarios local/reprise après SIGKILL, CLI et FTP réel passent avec le dernier binaire optimisé.

Le binaire corrigé a été installé par remplacement atomique dans `~/.local/bin/gad`, avec vérification du SHA-256. Aucun arrêt de Gad ou de ses transferts, commit, push, changement de version ou envoi au PPA. Les reçus de validation sont dans `target/regression-origin-main/delivery.json`.
