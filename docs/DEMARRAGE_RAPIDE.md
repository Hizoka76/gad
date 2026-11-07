# Gad — Démarrage rapide

**Gad, il copie. · Gestionnaire Avancé de Duplication · Version 0.1.0**

[Guide utilisateur](GUIDE_UTILISATEUR.md) · [Guide développeur](GUIDE_DEVELOPPEUR.md) · [Toutes les langues](README.md)

## Ouvrir Gad

Lancez **Gad** depuis le menu des applications, ou exécutez :

```sh
gad
```

Si nécessaire, suivez les [instructions d’installation](GUIDE_UTILISATEUR.md#installer-et-demarrer). Fermer la fenêtre laisse normalement Gad dans la zone de notification lorsqu’elle est disponible. Cliquez sur son icône pour le retrouver.

## Faire une première copie

![La fenêtre Gad en thème clair](images/fr/01-vue-ensemble.png)

*Source, Destination et Paramètres sont regroupés au-dessus de la liste.*

1. Sélectionnez les fichiers ou dossiers dans votre gestionnaire et faites **Ctrl+C**.
2. Dans Gad, renseignez **Destination** avec le dossier existant qui doit les recevoir.
3. Cliquez sur **Coller ici**, ou utilisez **Ctrl+V dans Gad**.
4. Vérifiez la destination dans la liste. Le transfert démarre automatiquement, sauf si la file est en pause.

Vous pouvez aussi utiliser **Ajouter des fichiers…**, **Ajouter un dossier…** ou glisser les fichiers dans Gad après avoir choisi la destination. **Le glisser-déposer effectue toujours une copie.** Pour déplacer des fichiers, utilisez **Ctrl+X**, puis collez-les si votre gestionnaire transmet un marqueur Couper reconnu. Vérifiez le type d’opération dans la liste ; les [instructions pour les autres gestionnaires](GUIDE_UTILISATEUR.md#utiliser-gad-avec-un-autre-gestionnaire-de-fichiers) précisent les alternatives.

Un nouveau collage ajoute une demande. Gad organise la file et peut traiter deux transferts compatibles en parallèle. Les opérations identiques déjà en attente sont ignorées.

## Suivre et piloter les opérations

- **En attente** : attend son tour. **En cours** : transfert actif. **OK** : terminé.
- Développez un dossier pour voir ses fichiers. Les couleurs distinguent les demandes ; **Ordre** indique l’ordre de traitement.
- **Tout mettre en pause** suspend la file. Le même bouton permet de reprendre. Le clic droit propose les commandes individuelles : pause, reprise, réessai et annulation.
- Sélectionnez des lignes en attente pour les **Traiter ensuite**, ou déplacez-les par glisser-déposer lorsque la table affiche l’ordre de la file.
- Annuler arrête l’opération ; **Annuler et remettre comme avant** tente aussi de restaurer ce qui a déjà été traité grâce à l’historique conservé. Lisez la confirmation et le résultat avant de nettoyer l’historique.

## Régler les préférences

![La boîte des options, ouverte depuis le bloc Paramètres.](images/fr/02-sources-options.png)

*La boîte des options, ouverte depuis le bloc Paramètres.*

Dans **Paramètres → Options…**, les onglets **Paramètres** et **Raccourcis** regroupent les préférences et les intégrations. Les changements s’appliquent immédiatement ; **Fermer** ou Échap ferme la boîte. Les paramètres de commande sont indiqués en gris. **Exclusions…** accepte des motifs de dossiers comme `__*` et de fichiers comme `*.tmp` ou `*~`. Le bouton de débit des lots sélectionnés se trouve à gauche de la barre d’actions principale.

**Autoriser la duplication dans le même dossier** crée par exemple `photo_copied1.jpg` lorsqu’on recopie un fichier dans son dossier d’origine. Désactivée par défaut, cette option concerne uniquement les copies. Les choix de sélection, vérification et duplication sont figés pour les nouvelles demandes ; les anciennes gardent leurs réglages. Le débit global peut changer pendant les transferts.

## Coller depuis le gestionnaire de fichiers

- **Dolphin** : activez **Ctrl+V avec Gad dans Dolphin** dans les paramètres, puis relancez Dolphin. Le menu contextuel propose aussi **Coller avec Gad**.
- **Konqueror** : utilisez le lanceur **Konqueror avec Gad** et activez sa case dans les paramètres. Les collages de fichiers passent par Gad ; le texte reste natif.
- **Autres gestionnaires** : collez dans Gad, glissez les fichiers ou créez une action avec une destination explicite, par exemple `gad --paste "/home/demo/Documents/Copies"`. Leur Ctrl+V n’est pas automatiquement intercepté.

Un collage lancé depuis le gestionnaire peut travailler en arrière-plan sans ouvrir Gad. La notification de progression Plasma contient un lien **Afficher Gad**.

## Résoudre un conflit ou une erreur

![Comparaison des fichiers en conflit](images/fr/08-conflit.png)

*Comparez la source et le fichier existant avant de décider.*

Lorsqu’un fichier existe déjà, choisissez **Conserver le fichier existant**, **Conserver les deux** ou **Remplacer le fichier existant**. N’appliquez la décision aux conflits suivants que si c’est souhaité.

Le **Bilan des erreurs…** permet de consulter, copier ou exporter les détails et de réessayer uniquement les fichiers en erreur. Si un périphérique disparaît, reconnectez le même au même emplacement et laissez Gad vérifier avant de reprendre.

**Conserver l’historique** garde les informations de remise en état. **Effacer les opérations terminées** retire les informations de restauration correspondantes. Pour la récupération, les adresses distantes, MD5 et les différentes annulations, consultez le [guide utilisateur](GUIDE_UTILISATEUR.md).

## Configuration et pages de manuel {#command-settings}

`gad --configure` enregistre les préférences et met à jour les contrôles de l’instance ouverte. Sans instance, Gad démarre en arrière-plan et la commande rend la main. Les formes `--no-…` désactivent les cases. Les exclusions non fournies sont conservées ; répéter `--exclude-folder` ou `--exclude-file` remplace la liste correspondante. `--clear-exclusions` vide les deux listes. Le débit global est exprimé en Mio/s ; 0 signifie illimité. `--md5` et `--properties` utilisés avec une copie ou un déplacement ne modifient que ce transfert. Les raccourcis Dolphin utilisent `gad --configure --dolphin-ctrl-v` ou `--no-dolphin-ctrl-v`.

```bash
gad --configure --md5 --verify-space --no-include-hidden
gad --configure --exclude-folder "__*" --exclude-file "*.tmp" --language fr_FR --rate-limit 25
```

`man gad` · `make manpages`
