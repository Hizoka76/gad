#pragma once
// Déclarations de la fenêtre ; les responsabilités sont réparties dans les fichiers window_*.cpp.
#include <QtWidgets>
#include <QtNetwork>
#include "notifications.h"
#include "snapshot.h"
#include "row_paths.h"
#include "row_tree.h"
#include "transfer_tree.h"
#include "presentation.h"
#include "transfer_metrics.h"

class Window : public QMainWindow {
    // Widgets possédés par la hiérarchie Qt ; les lignes sont aussi indexées dans jobRows.
    QTreeWidget *tree;
    QLabel *summary, *conflictTitle;
    QLabel *requestMetrics;
    TransferMetrics::Tracker metricsTracker;
    QElapsedTimer metricsClock;
    QHash<qint64,QPointer<SourceUrls::Resolver>> reconnectResolvers;
    QHash<qint64,qint64> reconnectAttempts;
    QHash<qint64,QString> reconnectWarnings;
    QComboBox *conflictSelector;
    ConflictFilePanel conflictSource, conflictDestination;
    // La clé lot/indice garde le conflit affiché stable entre deux rafraîchissements.
    QJsonArray pendingConflicts;
    QString displayedConflictKey, displayedConflictLanguage;
    int conflictIndex = -1;
    QFrame *conflictBox;
    QCheckBox *all;
    QCheckBox *includeHidden;
    QCheckBox *allowDuplication;
    QLabel *exclusionSummary;
    QJsonObject exclusionRules;
    QCheckBox *verifyMd5;
    QCheckBox *verifyProperties;
    QCheckBox *verifySpace;
    QCheckBox *keepHistory;
    QDoubleSpinBox *globalRate;
    QCheckBox *resumeInterrupted;
    QPushButton *pause;
    QLineEdit *destination;
    QProgressBar *progress;
    // Instantané de présentation : seul le moteur reste propriétaire de l’état métier.
    QJsonObject state;
    QueueSnapshot queueSnapshot;
    QJsonArray renderedQueueOrder;
    QHash<qint64,JobTotals> jobTotals;
    QString renderedLanguage;
    quint64 renderedViewRevision=0,renderedHeaderFilterRevision=0;
    bool renderedPause=false;
    qint64 reportedDuplicates=-1;
    QColor renderedBase;
    QMenu *rowMenu;
    QAction *rowSelectRequest;
    QAction *rowRetryErrors;
    QAction *rowRate;
    QAction *rowNext, *rowUp, *rowDown, *rowLast;
    QAction *rowRecover, *rowCancel, *rowUndo, *rowPause, *rowResume, *rowRetry, *rowRemove, *rowOpenFolder, *rowLaunchFile;
    // Accès permanent au programme caché et actions de notification.
    QSystemTrayIcon *tray;
    QAction *trayPause;
    QString trayVisualKey;
    // Distingue la sortie explicite du simple masquage de fenêtre.
    bool quitRequested = false;
    // Identifiants stables des lots vers les items réutilisés à chaque rafraîchissement.
    QHash<qint64, QTreeWidgetItem *> jobRows;
    // Indices stables du moteur vers les lignes, indépendamment de leur parent visuel.
    QHash<qint64,QVector<QTreeWidgetItem *>> entryRows;
    qint64 conflictJob = 0;
    bool paused = false;
    // Coupe le rafraîchissement une fois la fermeture du moteur acceptée.
    bool closing = false;
    TransferNotifications *desktopTransfers;
    // Les ajouts suivants attendent la résolution du premier lot pour préserver leur ordre.
    QQueue<QJsonObject> sourceQueue;
    bool resolvingSources = false;
    // Le résolveur et sa génération permettent d’ignorer toute réponse après annulation globale.
    QPointer<SourceUrls::Resolver> sourceResolver;
    quint64 sourceGeneration = 0;
public:
    /// Construit les contrôles, menus et connexions de signaux ; les choix persistants sont transmis au moteur.
    Window();
    /// Les métadonnées des demandes sont désormais détachées de l’arbre visuel.
    ~Window() override;
    /// Construit une carte de comparaison dont les textes restent copiables et sans interprétation HTML.
    ConflictFilePanel makeConflictFilePanel(std::function<QString()> title, const QString &prefix);
    /// Lit les métadonnées lors du changement de conflit, pas à chaque tick de progression.
    void populateConflictFile(ConflictFilePanel &panel, const QString &path, const QString &displayPath = QString());
    /// Affiche le conflit choisi et remet le choix « tous les suivants » à zéro pour une nouvelle ligne.
    void displayConflict();
    /// Conserve la même cible d’action quand un deuxième transfert rencontre aussi un conflit.
    void updateConflictPanel(const QJsonArray &entries);
    /// Applique pause/reprise globale ou annulation des lots actifs depuis la notification.
    void controlDesktopTransfers(const QString &action);
    /// Restaure la fenêtre et utilise, si fourni, le jeton Wayland autorisant son activation.
    void showMainWindow(const QString &token = QString());
    /// Crée une boîte À propos non bloquante, détruite à sa fermeture.
    void showAbout();
    /// Encode les indices des enfants sélectionnés et déduplique ceux déjà couverts par un lot entier.
    QJsonObject selectionRequest(const QString &command) const;

    /// Complète et envoie une commande ciblée, puis affiche son éventuelle erreur et rafraîchit la vue.
    void performSelection(const QString &command, const QJsonObject &options = {});
    /// Déplace les lots propriétaires des lignes sélectionnées, sans modifier leurs fichiers.
    void reorderSelection(const QString &action);
    /// Fixe un plafond pour les demandes propriétaires des lignes sélectionnées.
    void showRateLimit();
    /// Regroupe les erreurs et permet leur export et leur réessai ciblé.
    void showErrorReport();
    /// Modifie seulement les règles par défaut des prochaines demandes.
    void showExclusions();
    void updateExclusionSummary();
    void retryErrorSelection();
    /// Réactive les URL KIO en arrière-plan, sans modifier les chemins des journaux.
    void refreshReconnect();
    /// Applique l’ordre de file en conservant items, sélection et développement des branches.
    void syncQueueOrder(const QJsonArray &order);
    /// Choisit l’annulation simple ou la remise en état, en conservant la granularité des lignes.
    void cancelSelection(bool restore);
    /// Associe Suppr au retrait/arrêt et Retour arrière à la restauration des lignes terminées.
    void deleteSelection(bool restore = false);
    /// Adapte les actions du clic droit aux états de la sélection et évite une seconde restauration concurrente.
    void updateRowActions();
    /// Retrouve les chemins du menu par identifiant et indice stables, même après un rafraîchissement.
    QList<RowPaths::Location> locationsForSelection(const QJsonObject &selection) const;
    /// Ouvre les dossiers sélectionnés, une seule fois par chemin, dans le gestionnaire de fichiers associé.
    void openSelectedFolders();
    /// Ouvre un unique fichier avec son application associée ; aucun chemin n’est interprété par un shell.
    void launchSelectedFile();
    /// Soumet des chemins ajoutés depuis la fenêtre en utilisant le dossier de destination courant.
    QJsonObject enqueue(const QStringList &sources);
    /// Lit le presse-papiers immédiatement et conserve le mode Copier/Couper ainsi que le lancement en arrière-plan.
    QJsonObject pasteInto(const QString &path, bool background = false);

    /// Affiche une erreur sans bloquer la réponse JSON au client ni la préparation des lots suivants.
    QJsonObject error(const QString &text);
    /// Retire les lots encore hors du moteur et invalide leurs réponses asynchrones.
    int cancelSourcePreparation();
    /// Résout les sources dans l’ordre d’arrivée, puis utilise le même moteur que les copies locales.
    void resolveNextSources();
    /// Traite les commandes d’interface avant délégation au moteur ; un collage en arrière-plan ne relève pas la fenêtre.
    QJsonObject handle(QJsonObject request);
    /// Met à jour les lignes existantes à partir d’un instantané JSON, sans reconstruire le modèle.
    /// La conservation des items préserve le survol, les infobulles, les indices et la sélection.
    void refresh();
    /// Accès direct par indice moteur, utilisé aussi par la navigation vers un conflit.
    QTreeWidgetItem *entryRow(qint64 job,int index) const;
protected:
    /// Intercepte les collages de fichiers même depuis le champ Destination, tout en laissant le collage de texte habituel.
    bool eventFilter(QObject *object, QEvent *event) override;
    /// Annonce l’acceptation des glissements contenant des URL ; le dépôt vérifie ensuite leurs chemins.
    void dragEnterEvent(QDragEnterEvent *event) override;
    /// Valide les protocoles du dépôt et soumet les sources locales ou distantes dans leur ordre.
    void dropEvent(QDropEvent *event) override;
    /// Masque la fenêtre si le systray est disponible ; une vraie sortie attend l’absence de transfert actif.
    void closeEvent(QCloseEvent *event) override;
};
