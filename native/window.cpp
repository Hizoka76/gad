#include "i18n.h"
#include "documentation.h"
#include "protocol.h"
// Implémentation de la fenêtre Gad, séparée par responsabilité.
#include "window.h"
#include "bridge.h"
#include <KWindowSystem>
#include <QDesktopServices>
#ifndef GAD_VERSION
#define GAD_VERSION "développement"
#endif
#ifndef GAD_BUILD_DATE
#define GAD_BUILD_DATE ""
#endif

/// Construit les contrôles, menus et connexions de signaux ; les choix persistants sont transmis au moteur.
Window::Window(){
    I18n::bindProperty(this,"windowTitle",[]{return GAD_TR("Gad - Advanced Duplication Manager");});
    setWindowIcon(applicationIcon());
    setMinimumSize(1100,600);
    resize(1150, 720);
    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    auto *panels = new QHBoxLayout;
    panels->setSpacing(12);
    // La source regroupe les modes d’ajout ; la destination reste une zone distincte et éditable.
    auto *sourceBox = I18n::localized(new QGroupBox(QString()),"title",[=]{return GAD_TR("Source");});
    sourceBox->setObjectName("sourceBox");
    auto *actions = new QHBoxLayout(sourceBox);
    actions->setSpacing(8);
    auto *destinationBox = I18n::localized(new QGroupBox(QString()),"title",[=]{return GAD_TR("Destination");});
    destinationBox->setObjectName("destinationBox");
    auto *destRow = new QHBoxLayout(destinationBox);
    destRow->setSpacing(8);
    destination = new QLineEdit(QDir::homePath());destination->setObjectName("destinationInput");
    I18n::bindProperty(destination,"placeholderText",[=]{return GAD_TR("Local path or sftp://, ftp://, smb://, kdeconnect:// address");});
    I18n::bindProperty(destination,"accessibleName",[=]{return GAD_TR("Destination folder");});
    destRow->addWidget(destination, 1);
    auto *browse = I18n::localized(new QPushButton(QIcon::fromTheme("folder-open"),QString()),"text",[=]{return GAD_TR("Browse…");});
    destRow->addWidget(browse);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, GAD_TR("Copy destination"), destination->text());
        if (!path.isEmpty()) destination->setText(path);
    });
    auto button = [&](std::function<QString()> text, const QString &icon, auto fn) {
        auto *b = I18n::localized(new QPushButton(QIcon::fromTheme(icon),QString()),"text",text); actions->addWidget(b); connect(b, &QPushButton::clicked, this, fn); return b;
    };
    button([]{return GAD_TR("Add files…");}, "list-add", [this] {
        const auto files = QFileDialog::getOpenFileNames(this, GAD_TR("Files to copy")); if (!files.isEmpty()) enqueue(files);
    });
    button([]{return GAD_TR("Add folder…");}, "folder-new", [this] {
        const auto dir = QFileDialog::getExistingDirectory(this, GAD_TR("Folder to copy")); if (!dir.isEmpty()) enqueue({dir});
    });
    auto *paste = button([]{return GAD_TR("Paste here");}, "edit-paste", [this] { pasteInto(destination->text()); });
    paste->setShortcut(QKeySequence::Paste);
    actions->addStretch();
    panels->addWidget(sourceBox, 1);
    panels->addWidget(destinationBox, 1);
    // Une seule boîte persistante conserve les contrôles, les connexions et les réglages immédiats.
    auto *settingsBox=I18n::localized(new QGroupBox,"title",[]{return GAD_TR("Settings");});settingsBox->setObjectName("settingsBox");
    auto *settingsRow=new QHBoxLayout(settingsBox);
    auto *settingsButton=I18n::localized(new QToolButton,"text",[]{return GAD_TR("Options…");});settingsButton->setObjectName("settingsButton");
    settingsButton->setIcon(QIcon::fromTheme("configure",style()->standardIcon(QStyle::SP_FileDialogDetailedView)));settingsButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto *settingsDialog=I18n::localized(new QDialog(this),"windowTitle",[]{return GAD_TR("Settings");});settingsDialog->setObjectName("settingsDialog");
    auto *dialogLayout=new QVBoxLayout(settingsDialog);dialogLayout->setContentsMargins(16,16,16,16);dialogLayout->setSpacing(12);
    auto *settingsPanel=new QWidget;settingsPanel->setObjectName("settingsPanel");auto *settingsLayout=new QVBoxLayout(settingsPanel);settingsLayout->setSpacing(12);
    auto *settingsScroll=new QScrollArea;settingsScroll->setWidgetResizable(true);settingsScroll->setFrameShape(QFrame::NoFrame);settingsScroll->setWidget(settingsPanel);
    auto *settingsTabs=new QTabWidget;settingsTabs->setObjectName("settingsTabs");settingsTabs->addTab(settingsScroll,QString());
    I18n::onChange(settingsTabs,"tabs",[settingsTabs]{settingsTabs->setTabText(0,GAD_TR("Settings"));if(settingsTabs->count()>1)settingsTabs->setTabText(1,GAD_TR("Shortcuts"));});
    auto *commandUsage=new QLabel("gad --configure [options]");commandUsage->setTextInteractionFlags(Qt::TextSelectableByMouse);dialogLayout->addWidget(commandUsage);
    dialogLayout->addWidget(settingsTabs);
    auto *settingsButtons=new QDialogButtonBox;auto *settingsClose=I18n::localized(new QPushButton(QIcon::fromTheme("window-close",style()->standardIcon(QStyle::SP_DialogCloseButton)),QString()),"text",[]{return GAD_TR("Close");});settingsClose->setObjectName("settingsCloseButton");
    settingsButtons->addButton(settingsClose,QDialogButtonBox::RejectRole);connect(settingsButtons,&QDialogButtonBox::rejected,settingsDialog,&QDialog::reject);dialogLayout->addWidget(settingsButtons);
    settingsDialog->setMinimumSize(800,740);settingsDialog->resize(800,740);
    settingsRow->addWidget(settingsButton);panels->addWidget(settingsBox);layout->addLayout(panels);
    // Les préférences Qt sont transmises au moteur et capturées ensuite dans chaque nouveau lot.
    verifyMd5 = I18n::localized(new QCheckBox(QString()),"text",[=]{return GAD_TR("Verify files using MD5");});
    verifyMd5->setObjectName("verifyMd5");
    I18n::bindProperty(verifyMd5,"toolTip",[=]{return GAD_TR("Reads and compares source and destination contents before marking each file complete.\nApplies to new transfers, including those started from Dolphin.");});
    verifyMd5->setChecked(QSettings().value("verifyMd5", false).toBool());
    engine({{"command", Protocol::Command::Configure}, {"verify_md5", verifyMd5->isChecked()}});
    connect(verifyMd5, &QCheckBox::toggled, this, [this](bool enabled) {
        QSettings().setValue("verifyMd5", enabled);
        engine({{"command", Protocol::Command::Configure}, {"verify_md5", enabled}});
    });
    verifyProperties=I18n::localized(new QCheckBox(QString()),"text",[=]{return GAD_TR("Verify properties");});
    verifyProperties->setObjectName("verifyProperties");
    I18n::bindProperty(verifyProperties,"toolTip",[=]{return GAD_TR("Compares visible owner, group, permissions, ACLs, and extended attributes.\nReports differences or incomplete checks.\nCopies are kept with a warning; a regular move keeps the source if verification fails.\nDoes not replicate these properties.");});
    verifyProperties->setChecked(QSettings().value("verifyProperties",false).toBool());
    engine({{"command",Protocol::Command::Configure},{"verify_properties",verifyProperties->isChecked()}});
    connect(verifyProperties,&QCheckBox::toggled,this,[this](bool enabled){
        QSettings().setValue("verifyProperties",enabled);
        engine({{"command",Protocol::Command::Configure},{"verify_properties",enabled}});
    });
    verifySpace=I18n::localized(new QCheckBox(QString()),"text",[=]{return GAD_TR("Check available space");});
    verifySpace->setObjectName("verifySpace");
    I18n::bindProperty(verifySpace,"toolTip",[=]{return GAD_TR("Before each batch, estimates the space needed for queued files on each volume.\nPrevious destinations remain accounted for so they can be restored.\nPrevents starting if space is insufficient.\nIf space cannot be checked, displays a warning and continues.\nExisting batches keep their settings.");});
    verifySpace->setChecked(QSettings().value("verifySpace",false).toBool());
    engine({{"command",Protocol::Command::Configure},{"verify_space",verifySpace->isChecked()}});
    connect(verifySpace,&QCheckBox::toggled,this,[this](bool enabled){
        QSettings().setValue("verifySpace",enabled);
        const auto result=engine({{"command",Protocol::Command::Configure},{"verify_space",enabled}});
        if(result.contains("error"))error(result["error"].toString());
    });
    keepHistory = I18n::localized(new QCheckBox(QString()),"text",[=]{return GAD_TR("Keep history");});
    keepHistory->setObjectName("keepHistory");
    I18n::bindProperty(keepHistory,"toolTip",[=]{return GAD_TR("Keeps operations between sessions.\nReplaced files are retained for restoration; clearing history removes this possibility.");});
    keepHistory->setChecked(QSettings().value("keepHistory", true).toBool());
    engine({{"command", Protocol::Command::Configure}, {"keep_history", keepHistory->isChecked()}});
    connect(keepHistory, &QCheckBox::toggled, this, [this](bool enabled) {
        QSettings().setValue("keepHistory", enabled);
        engine({{"command", Protocol::Command::Configure}, {"keep_history", enabled}});
    });
    // Les anciens journaux sans point compatible restent dans le parcours de remise en état.
    resumeInterrupted=I18n::localized(new QCheckBox(QString()),"text",[=]{return GAD_TR("Resume after interruption");});
    resumeInterrupted->setObjectName("resumeInterrupted");
    I18n::bindProperty(resumeInterrupted,"toolTip",[=]{return GAD_TR("Creates durable checkpoints and resumes new batches after a crash or restart.\nDeliberately canceled operations are never restarted.\nExisting batches keep their settings.");});
    resumeInterrupted->setChecked(QSettings().value("resumeInterrupted",true).toBool());
    engine({{"command",Protocol::Command::Configure},{"resume_interrupted",resumeInterrupted->isChecked()}});
    connect(resumeInterrupted,&QCheckBox::toggled,this,[this](bool enabled){
        QSettings().setValue("resumeInterrupted",enabled);
        engine({{"command",Protocol::Command::Configure},{"resume_interrupted",enabled}});
    });
    // Le démarrage de session reste facultatif et se fait dans la zone de notification.
    auto *startup = I18n::localized(new QCheckBox(QString()),"text",[=]{return GAD_TR("Launch at startup");});
    startup->setObjectName("launchAtStartup");
    I18n::bindProperty(startup,"toolTip",[=]{return GAD_TR("Starts Gad in the system tray when you sign in.");});
    startup->setChecked(autostartEnabled());
    connect(startup,&QCheckBox::toggled,this,[this,startup](bool enabled) {
        if(!setAutostart(enabled)) {
            QSignalBlocker blocker(startup);startup->setChecked(autostartEnabled());
            error(GAD_TR("Unable to change Gad's startup setting."));
        }
    });
    // Les noms CLI restent littéraux dans toutes les langues, sous la case correspondante.
    const auto commandHint=[](const QString &name,const QString &command) {
        auto *hint=new QLabel(command);hint->setObjectName(name+"CommandOption");
        auto palette=hint->palette();palette.setColor(QPalette::WindowText,palette.color(QPalette::Disabled,QPalette::WindowText));hint->setPalette(palette);
        hint->setWordWrap(true);hint->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));hint->setTextFormat(Qt::PlainText);hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return hint;
    };
    const auto commandOption=[commandHint](QCheckBox *check,const QString &command) {
        auto *container=new QWidget;auto *column=new QVBoxLayout(container);column->setContentsMargins(0,0,0,0);column->setSpacing(2);
        column->addWidget(check);auto *hint=commandHint(check->objectName(),command);
        hint->setContentsMargins(check->style()->pixelMetric(QStyle::PM_IndicatorWidth)+check->style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing),0,0,4);
        column->addWidget(hint);return container;
    };
    auto *checks=new QGridLayout;checks->addWidget(commandOption(verifyMd5,"--md5 / --no-md5"),0,0);checks->addWidget(commandOption(verifyProperties,"--properties / --no-properties"),0,1);checks->addWidget(commandOption(verifySpace,"--verify-space / --no-verify-space"),1,0);checks->addWidget(commandOption(keepHistory,"--keep-history / --no-keep-history"),1,1);checks->addWidget(commandOption(startup,"--autostart / --no-autostart"),2,0);checks->addWidget(commandOption(resumeInterrupted,"--resume-interrupted / --no-resume-interrupted"),4,0,1,2);settingsLayout->addLayout(checks);
    // Les préférences de sélection sont figées dans chaque requête, avant une éventuelle résolution KIO.
    exclusionRules={{"include_hidden",QSettings().value("includeHidden",true).toBool()},
        {"directories",QJsonArray::fromStringList(QSettings().value("excludeDirectories").toStringList())},
        {"files",QJsonArray::fromStringList(QSettings().value("excludeFiles").toStringList())}};
    auto *selectionOptions=new QVBoxLayout;
    includeHidden=I18n::localized(new QCheckBox, "text",[]{return GAD_TR("Include hidden items");});includeHidden->setObjectName("includeHidden");
    I18n::bindProperty(includeHidden,"toolTip",[]{return GAD_TR("Hidden items have a name starting with a dot.\nThis option applies to new copies and moves only.");});
    includeHidden->setChecked(exclusionRules["include_hidden"].toBool());checks->addWidget(commandOption(includeHidden,"--include-hidden / --no-include-hidden"),2,1);
    allowDuplication=I18n::localized(new QCheckBox,"text",[]{return GAD_TR("Allow duplication in the same folder");});allowDuplication->setObjectName("allowDuplication");
    I18n::bindProperty(allowDuplication,"toolTip",[]{return GAD_TR("Copying into the source folder creates name_copied1, name_copied2, etc.\nFile extensions are preserved.\nApplies to new copies only; moves are not duplicated.");});
    allowDuplication->setChecked(QSettings().value("allowDuplication",false).toBool());checks->addWidget(commandOption(allowDuplication,"--allow-duplication / --no-allow-duplication"),3,0,1,2);
    engine({{"command",Protocol::Command::Configure},{"allow_duplication",allowDuplication->isChecked()}});
    connect(allowDuplication,&QCheckBox::toggled,this,[this](bool enabled){
        const auto result=handle({{"command",Protocol::Command::Configure},{"allow_duplication",enabled}});
        if(result.contains("error")){QSignalBlocker blocker(allowDuplication);allowDuplication->setChecked(!enabled);error(result["error"].toString());}
    });
    auto *exclusionsButton=I18n::localized(new QPushButton(QIcon::fromTheme("view-filter",style()->standardIcon(QStyle::SP_FileDialogContentsView)),QString()),"text",[]{return GAD_TR("Exclusions…");});exclusionsButton->setObjectName("exclusionsButton");
    I18n::bindProperty(exclusionsButton,"toolTip",[]{return GAD_TR("Choose folder and file name patterns to exclude.\nExcluded folders are not explored.\nExcluded items stay at the source during moves.");});
    connect(exclusionsButton,&QPushButton::clicked,this,[this]{showExclusions();});selectionOptions->addWidget(exclusionsButton);
    selectionOptions->addWidget(commandHint("exclusions","--exclude-folder PATTERN / --exclude-file PATTERN / --clear-exclusions"));
    exclusionSummary=new QLabel;exclusionSummary->setObjectName("exclusionSummary");exclusionSummary->setTextFormat(Qt::PlainText);selectionOptions->addWidget(exclusionSummary);settingsLayout->addLayout(selectionOptions);
    I18n::onChange(exclusionSummary,"rules",[this]{updateExclusionSummary();});
    const auto exclusionsConfigured=engine({{"command",Protocol::Command::Configure},{"exclusions",exclusionRules}});
    if(exclusionsConfigured.contains("error")){const auto message=exclusionsConfigured["error"].toString();QTimer::singleShot(0,this,[this,message]{error(message);});}
    connect(includeHidden,&QCheckBox::toggled,this,[this](bool enabled){
        auto rules=exclusionRules;rules["include_hidden"]=enabled;
        const auto result=handle({{"command",Protocol::Command::Configure},{"exclusions",rules}});
        if(result.contains("error")){QSignalBlocker blocker(includeHidden);includeHidden->setChecked(exclusionRules["include_hidden"].toBool(true));error(result["error"].toString());}
    });
    auto *options = new QHBoxLayout;
    // Le plafond global s’applique immédiatement à la somme des transferts, même déjà démarrés.
    globalRate=new QDoubleSpinBox;globalRate->setObjectName("globalRateLimit");
    globalRate->setRange(0,1048576);globalRate->setDecimals(2);globalRate->setSingleStep(1);
    I18n::bindProperty(globalRate,"suffix",[=]{return GAD_TR(" MiB/s");});I18n::bindProperty(globalRate,"specialValueText",[=]{return GAD_TR("Unlimited");});globalRate->setKeyboardTracking(false);
    I18n::bindProperty(globalRate,"toolTip",[=]{return GAD_TR("Total speed limit for copies and moves that copy data.\n0 = unlimited.\nCan be changed during transfers; fast renames are not throttled.");});
    globalRate->setValue(QSettings().value("rateLimit",0).toLongLong()/1048576.0);
    engine({{"command",Protocol::Command::Configure},{"rate_limit",qRound64(globalRate->value()*1048576)}});
    connect(globalRate,&QDoubleSpinBox::valueChanged,this,[this](double value){
        const auto bytes=qRound64(value*1048576);
        const auto result=engine({{"command",Protocol::Command::Configure},{"rate_limit",bytes}});
        if(result.contains("error"))error(result["error"].toString());
        else QSettings().setValue("rateLimit",bytes);
        refresh();
    });
    auto *rateLabel=I18n::localized(new QLabel(QString()),"text",[=]{return GAD_TR("Global speed limit:");});rateLabel->setBuddy(globalRate);
    options->addWidget(rateLabel);options->addWidget(globalRate);
    settingsLayout->addLayout(options);
    settingsLayout->addWidget(commandHint("globalRate","--rate-limit MiB/s"));
    // Les demandes actives ont leur propre débit et estimation, sans ajouter de niveaux à l’arbre.
    requestMetrics=new QLabel;requestMetrics->setObjectName("requestMetrics");requestMetrics->setTextFormat(Qt::PlainText);
    requestMetrics->setWordWrap(true);requestMetrics->setTextInteractionFlags(Qt::TextSelectableByMouse);
    I18n::bindProperty(requestMetrics,"toolTip",[=]{return GAD_TR("Speeds measured over the last five seconds of activity.\nRemaining time covers the whole request; if MD5 speed is unknown, only copy time is estimated.\nTimes are approximate.");});
    requestMetrics->hide();metricsClock.start();
    summary = I18n::localized(new QLabel(QString()),"text",[=]{return GAD_TR("Ready to copy");});
    summary->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    summary->setWordWrap(true);
    summary->setTextFormat(Qt::PlainText);
    progress = new QProgressBar; progress->setRange(0, 1000); progress->setValue(0); progress->setTextVisible(false); progress->setMaximumHeight(8); layout->addWidget(progress);
    auto *controls = new QHBoxLayout;
    controls->addStretch();
    pause = I18n::localized(new QPushButton(QIcon::fromTheme("media-playback-pause"),QString()),"text",[=]{return GAD_TR("Pause all");});
    connect(pause, &QPushButton::clicked, this, [this] { engine({{"command", Protocol::Command::Pause}, {"paused", !paused}}); refresh(); });
    controls->addWidget(pause);
    // Le menu sépare les commandes globales des actions limitées aux lignes sélectionnées.
    auto *cancel = I18n::localized(new QPushButton(QIcon::fromTheme("process-stop"),QString()),"text",[=]{return GAD_TR("Cancellation");});
    cancel->setObjectName("cancellationMenu");
    auto *cancelMenu = new QMenu(cancel); cancelMenu->setToolTipsVisible(true);
    auto *cancelAll = I18n::localized(cancelMenu->addAction(QString()),"text",[=]{return GAD_TR("Cancel all");}); cancelAll->setIcon(QIcon::fromTheme("process-stop",style()->standardIcon(QStyle::SP_BrowserStop)));
    cancelAll->setObjectName("cancelAllAction");
    I18n::bindProperty(cancelAll,"toolTip",[=]{return GAD_TR("Stops active and queued copies and moves, keeping files already transferred.");});
    connect(cancelAll, &QAction::triggered, this, [this] {
        const auto result=handle({{"command",Protocol::Command::CancelAll}});
        if(result.contains("error"))error(result["error"].toString());
        refresh();
    });
    auto *undoAll = I18n::localized(cancelMenu->addAction(QString()),"text",[=]{return GAD_TR("Cancel all and restore");}); undoAll->setIcon(QIcon::fromTheme("edit-undo",style()->standardIcon(QStyle::SP_ArrowBack)));
    undoAll->setObjectName("undoAllAction");
    I18n::bindProperty(undoAll,"toolTip",[=]{return GAD_TR("Stops all operations and restores copies, moves, and replacements still in history, from newest to oldest.");});
    connect(undoAll, &QAction::triggered, this, [this] {
        const auto result=handle({{"command",Protocol::Command::UndoAll}});
        if(result.contains("error"))error(result["error"].toString());
        else if(result["skipped_legacy"].toInt()>0) QMessageBox::information(this,"Gad",QString(GAD_TR("%1 older operation(s) have no journal and cannot be restored. Other operations will be restored.")).arg(result["skipped_legacy"].toInt()));
        refresh();
    });
    cancelMenu->addSeparator();
    auto *cancelSelected = I18n::localized(cancelMenu->addAction(QString()),"text",[=]{return GAD_TR("Cancel selected operations");}); cancelSelected->setIcon(QIcon::fromTheme("process-stop",style()->standardIcon(QStyle::SP_BrowserStop)));
    auto *undoSelected = I18n::localized(cancelMenu->addAction(QString()),"text",[=]{return GAD_TR("Cancel, delete, and move back");}); undoSelected->setIcon(QIcon::fromTheme("edit-undo",style()->standardIcon(QStyle::SP_ArrowBack)));
    I18n::bindProperty(undoSelected,"toolTip",[=]{return GAD_TR("Stops selected operations, deletes their copies, and moves files back to their source.\nAlso restores replaced files.\nFiles changed since the transfer are kept with an explicit error.");});
    I18n::bindProperty(cancel,"toolTip",[=]{return GAD_TR("Menu commands apply to operations.\nDelete handles each selected row according to its state.\nBackspace also restores completed rows.");});
    cancel->setMenu(cancelMenu); controls->addWidget(cancel);
    connect(cancelSelected, &QAction::triggered, this, [this] { cancelSelection(false); });
    connect(undoSelected, &QAction::triggered, this, [this] { cancelSelection(true); });
    auto *clear = I18n::localized(new QPushButton(QIcon::fromTheme("edit-clear-history", QIcon::fromTheme("edit-clear", style()->standardIcon(QStyle::SP_DialogResetButton))),QString()),"text",[=]{return GAD_TR("Clear completed operations");});
    I18n::bindProperty(clear,"toolTip",[=]{return GAD_TR("Clears completed operations and the backups needed to restore them.");}); controls->addWidget(clear);
    connect(clear, &QPushButton::clicked, this, [this] { auto result=engine({{"command", Protocol::Command::Clear}}); if(result.contains("error")) error(result["error"].toString()); refresh(); });
    layout->addLayout(controls);
    // Les items sont conservés entre deux rafraîchissements afin de ne pas perdre survol et sélection.
    auto *transferTree=new TransferTree;tree=transferTree;
    transferTree->reorder=[this](QJsonObject request){
        request["command"]=Protocol::Command::ReorderRows;
        const auto result=engine(request);if(result.contains("error"))error(result["error"].toString());refresh();
    };
    transferTree->presentationChanged=[this]{refresh();};
    tree->setItemDelegate(new RequestDelegate(tree));
    tree->setHeaderLabels({GAD_TR("Source"), GAD_TR("Destination"), GAD_TR("Status"), GAD_TR("Progress"), GAD_TR("Details"), GAD_TR("Order")});
    // La nouvelle colonne est visuellement première ; Source conserve les branches de l’arbre.
    tree->setTreePosition(0);tree->header()->moveSection(tree->header()->visualIndex(TableView::OrderColumn),0);
    transferTree->updateHeaderIndicators();
    I18n::bindProperty(tree,"whatsThis",[=]{return GAD_TR("Delete: removes completed rows, cancels active rows, and removes queued rows. Backspace: restores selected completed copies and moves; otherwise acts like Delete. Two operations can run together on separate disks. Right-click: Process next and Reorder queue change the order of queued batches.");});
    tree->setAlternatingRowColors(false); tree->setUniformRowHeights(true);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Le clic droit réutilise exactement le protocole des boutons et des raccourcis.
    rowMenu = new QMenu(tree); rowMenu->setObjectName("rowContextMenu");
    rowOpenFolder=I18n::localized(rowMenu->addAction(QIcon::fromTheme("folder-open"),QString()),"text",[=]{return GAD_TR("Open containing folder");});rowOpenFolder->setObjectName("rowOpenFolder");
    rowLaunchFile=I18n::localized(rowMenu->addAction(QIcon::fromTheme("document-open"),QString()),"text",[=]{return GAD_TR("Open file");});rowLaunchFile->setObjectName("rowLaunchFile");
    rowOpenFolder->setEnabled(false);rowLaunchFile->setEnabled(false);
    connect(rowOpenFolder,&QAction::triggered,this,&Window::openSelectedFolders);
    connect(rowLaunchFile,&QAction::triggered,this,&Window::launchSelectedFile);
    rowMenu->addSeparator();
    rowSelectRequest=I18n::localized(rowMenu->addAction(QString()),"text",[=]{return GAD_TR("Select entire request");});rowSelectRequest->setObjectName("rowSelectRequest"); rowSelectRequest->setIcon(QIcon::fromTheme("edit-select-all",style()->standardIcon(QStyle::SP_DialogApplyButton)));
    connect(rowSelectRequest,&QAction::triggered,this,[this]{
        QSet<qint64> ids;for(auto *item:tree->selectedItems())ids.insert(item->data(0,Qt::UserRole).toLongLong());
        for(const auto id:ids) {
            const auto rows=entryRows.value(id);
            if(rows.isEmpty()){auto *placeholder=jobRows.value(id);if(placeholder && !placeholder->isHidden())placeholder->setSelected(true);}
            else for(auto *row:rows)if(!row->isHidden())row->setSelected(true);
        }
    });
    rowNext=I18n::localized(rowMenu->addAction(QIcon::fromTheme("go-top"),QString()),"text",[=]{return GAD_TR("Process next");});rowNext->setObjectName("rowNext");
    auto *priorityMenu=I18n::localized(rowMenu->addMenu(QString()),"title",[=]{return GAD_TR("Reorder queue");});
    rowUp=I18n::localized(priorityMenu->addAction(QIcon::fromTheme("go-up"),QString()),"text",[=]{return GAD_TR("Move up");});rowUp->setObjectName("rowUp");
    rowDown=I18n::localized(priorityMenu->addAction(QIcon::fromTheme("go-down"),QString()),"text",[=]{return GAD_TR("Move down");});rowDown->setObjectName("rowDown");
    rowLast=I18n::localized(priorityMenu->addAction(QIcon::fromTheme("go-bottom"),QString()),"text",[=]{return GAD_TR("Move to end of queue");});rowLast->setObjectName("rowLast");
    for(auto *action:{rowNext,rowUp,rowDown,rowLast})I18n::bindProperty(action,"toolTip",[=]{return GAD_TR("Changes the priority of queued rows.\nFolders keep their descendants; destinations and active transfers are unchanged.");});
    connect(rowNext,&QAction::triggered,this,[this]{reorderSelection("next");});
    priorityMenu->setIcon(QIcon::fromTheme("view-sort",style()->standardIcon(QStyle::SP_ArrowUp)));
    connect(rowUp,&QAction::triggered,this,[this]{reorderSelection("up");});
    connect(rowDown,&QAction::triggered,this,[this]{reorderSelection("down");});
    connect(rowLast,&QAction::triggered,this,[this]{reorderSelection("last");});
    rowMenu->addSeparator();
    rowCancel = I18n::localized(rowMenu->addAction(QIcon::fromTheme("process-stop"),QString()),"text",[=]{return GAD_TR("Cancel and keep completed files");}); rowCancel->setObjectName("rowCancel");
    rowUndo = I18n::localized(rowMenu->addAction(QIcon::fromTheme("edit-undo"),QString()),"text",[=]{return GAD_TR("Cancel and restore");}); rowUndo->setObjectName("rowUndo");
    rowMenu->addSeparator();
    rowRate=I18n::localized(rowMenu->addAction(QIcon::fromTheme("speedometer"),QString()),"text",[=]{return GAD_TR("Limit request speed…");});
    rowRate->setObjectName("rowRateLimit");
    I18n::bindProperty(rowRate,"toolTip",[=]{return GAD_TR("Applies a limit to all rows in the selected requests.\nThe global limit still applies.");});
    connect(rowRate,&QAction::triggered,this,&Window::showRateLimit);
    rowPause = I18n::localized(rowMenu->addAction(QIcon::fromTheme("media-playback-pause"),QString()),"text",[=]{return GAD_TR("Pause");}); rowPause->setObjectName("rowPause");
    rowResume = I18n::localized(rowMenu->addAction(QIcon::fromTheme("media-playback-start"),QString()),"text",[=]{return GAD_TR("Resume");}); rowResume->setObjectName("rowResume");
    rowRetry = I18n::localized(rowMenu->addAction(QIcon::fromTheme("view-refresh"),QString()),"text",[=]{return GAD_TR("Reactivate canceled rows");}); rowRetry->setObjectName("rowRetry");
    rowRetryErrors=I18n::localized(rowMenu->addAction(QIcon::fromTheme("view-refresh"),QString()),"text",[=]{return GAD_TR("Retry failed files only");});rowRetryErrors->setObjectName("rowRetryErrors"); rowRetryErrors->setIcon(QIcon::fromTheme("view-refresh",style()->standardIcon(QStyle::SP_BrowserReload)));
    connect(rowRetryErrors,&QAction::triggered,this,&Window::retryErrorSelection);
    rowRecover=I18n::localized(rowMenu->addAction(QIcon::fromTheme("media-playback-start"),QString()),"text",[=]{return GAD_TR("Resume interrupted batches");});
    rowRecover->setObjectName("rowRecover");
    I18n::bindProperty(rowRecover,"toolTip",[=]{return GAD_TR("Verifies and resumes interrupted batches containing the selected rows, without copying completed files again.");});
    connect(rowRecover,&QAction::triggered,this,[this]{
        QSet<qint64> selected;
        for(auto *item:tree->selectedItems()) {
            const auto *owner=RowTree::owner(item);
            if(owner->data(0,Qt::UserRole+6).toBool())selected.insert(owner->data(0,Qt::UserRole).toLongLong());
        }
        if(selected.isEmpty())return;
        QJsonArray ids;for(const auto id:selected)ids.append(id);
        const auto result=handle({{"command",Protocol::Command::ResumeInterrupted},{"ids",ids}});
        if(result.contains("error"))error(result["error"].toString());
        refresh();
    });
    rowMenu->addSeparator();
    rowRemove = I18n::localized(rowMenu->addAction(QIcon::fromTheme("edit-delete"),QString()),"text",[=]{return GAD_TR("Remove from list / stop");}); rowRemove->setObjectName("rowRemove");
    connect(rowCancel,&QAction::triggered,this,[this]{performSelection("cancel_selection");});
    connect(rowUndo,&QAction::triggered,this,[this]{performSelection("undo_selection");});
    connect(rowPause,&QAction::triggered,this,[this]{performSelection("pause_selection",{{"paused",true}});});
    connect(rowResume,&QAction::triggered,this,[this]{performSelection("pause_selection",{{"paused",false}});});
    connect(rowRetry,&QAction::triggered,this,[this]{performSelection("retry_selection");});
    connect(rowRemove,&QAction::triggered,this,[this]{deleteSelection();});
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree,&QTreeWidget::customContextMenuRequested,this,[this](const QPoint &point){
        auto *item=tree->itemAt(point); if(!item)return;
        if(!item->isSelected()){tree->clearSelection();tree->setCurrentItem(item);item->setSelected(true);}
        updateRowActions(); rowMenu->popup(tree->viewport()->mapToGlobal(point));
    });

    connect(tree, &QTreeWidget::itemSelectionChanged, cancel, [this,cancelSelected,undoSelected] {
        const bool selected=!tree->selectedItems().isEmpty();
        cancelSelected->setEnabled(selected);undoSelected->setEnabled(selected);
        updateRowActions();
    });
    cancelSelected->setEnabled(false);undoSelected->setEnabled(false);
    auto *remove = new QShortcut(QKeySequence(Qt::Key_Delete), tree);
    remove->setContext(Qt::WidgetWithChildrenShortcut);
    remove->setAutoRepeat(false);
    connect(remove, &QShortcut::activated, this, [this] { deleteSelection(); });
    auto *restore = new QShortcut(QKeySequence(Qt::Key_Backspace), tree);
    restore->setContext(Qt::WidgetWithChildrenShortcut);
    restore->setAutoRepeat(false);
    connect(restore, &QShortcut::activated, this, [this] { deleteSelection(true); });
    tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    // Qt garde seulement la largeur nécessaire au titre, à la flèche et aux numéros.
    tree->header()->setSectionResizeMode(TableView::OrderColumn,QHeaderView::ResizeToContents);
    tree->setColumnWidth(0, 285); tree->setColumnWidth(1, 295); tree->setColumnWidth(2, 100); tree->setColumnWidth(3, 150);
    tree->header()->setStretchLastSection(true);
    layout->addWidget(tree, 1);
    layout->addWidget(requestMetrics);
    // Les confirmations de conflit restent sous les fichiers, avant le récapitulatif et le pied de fenêtre.
    conflictBox = new QFrame;
    conflictBox->setObjectName("conflictPanel");
    conflictBox->setStyleSheet("QFrame#conflictPanel { border: 1px solid palette(mid); border-left: 4px solid palette(highlight); border-radius: 6px; }");
    auto *conflicts = new QVBoxLayout(conflictBox);
    auto *heading = new QHBoxLayout;
    auto *warning = new QLabel; warning->setPixmap(QIcon::fromTheme("dialog-warning", style()->standardIcon(QStyle::SP_MessageBoxWarning)).pixmap(24,24));
    heading->addWidget(warning);
    conflictTitle = new QLabel; conflictTitle->setTextFormat(Qt::PlainText); conflictTitle->setWordWrap(true);
    auto titleFont=conflictTitle->font(); titleFont.setBold(true); titleFont.setPointSize(titleFont.pointSize()+1); conflictTitle->setFont(titleFont);
    heading->addWidget(conflictTitle,1);
    auto *locate = I18n::localized(new QPushButton(QIcon::fromTheme("edit-find"),QString()),"text",[=]{return GAD_TR("Show file in list");});
    locate->setObjectName("locateConflict");
    connect(locate,&QPushButton::clicked,this,[this] {
        const auto id=conflictJob; const int index=conflictIndex;
        // Changer le filtre peut rafraîchir l’état : retrouver l’item ensuite, par son identifiant stable.
        static_cast<TransferTree *>(tree)->clearHeaderFilters();
        auto *row=entryRow(id,index);if(!row)return;
        for(auto *ancestor=row->parent();ancestor;ancestor=ancestor->parent())ancestor->setExpanded(true);
        tree->clearSelection();tree->setCurrentItem(row);tree->scrollToItem(row);
    });
    heading->addWidget(locate); conflicts->addLayout(heading);
    conflictSelector = new QComboBox; conflictSelector->setObjectName("conflictSelector");
    connect(conflictSelector,&QComboBox::currentIndexChanged,this,[this]{displayConflict();});
    conflicts->addWidget(conflictSelector);
    auto *comparison = new QHBoxLayout;
    conflictSource=makeConflictFilePanel([]{return GAD_TR("File to copy");},"conflictSource");
    conflictDestination=makeConflictFilePanel([]{return GAD_TR("Already at destination");},"conflictDestination");
    comparison->addWidget(conflictSource.box,1); comparison->addWidget(conflictDestination.box,1); conflicts->addLayout(comparison);
    all = new QCheckBox; all->setObjectName("applyToBatchConflicts"); conflicts->addWidget(all);
    auto *choices = new QHBoxLayout; choices->addStretch();
    for (const auto &choice : QList<QPair<std::function<QString()>, QString>>{{[]{return GAD_TR("Keep existing file");}, "skip"}, {[]{return GAD_TR("Keep both");}, "rename"}, {[]{return GAD_TR("Replace existing file");}, "replace"}}) {
        auto *button = I18n::localized(new QPushButton(QIcon::fromTheme(choice.second=="skip"?"media-skip-forward":choice.second=="rename"?"edit-rename":"document-save"),QString()),"text",choice.first);
        button->setObjectName("resolveConflict_"+choice.second); button->setAutoDefault(false);
        I18n::bindProperty(button,"toolTip",[=]{return choice.second=="skip"?GAD_TR("Skips this file and keeps its source, including during moves."):choice.second=="rename"?GAD_TR("Creates a copy using an available name, such as “file (copy 1).ext”."):GAD_TR("Replaces the destination with the source.\nThe previous destination is retained in the restoration journal.");});
        choices->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, choice] {
            // L’indice permet au moteur de refuser une réponse devenue périmée avant ce clic.
            const auto result=engine({{"command", Protocol::Command::Resolve}, {"id", conflictJob}, {"index",conflictIndex}, {"action", choice.second}, {"all", all->isChecked()}});
            if(result.contains("error"))error(result["error"].toString());
            refresh();
        });
    }
    conflicts->addLayout(choices); layout->insertWidget(layout->indexOf(tree)+1,conflictBox); conflictBox->hide();
    auto *footer = new QHBoxLayout;
    auto *about = I18n::localized(new QPushButton(QIcon::fromTheme("help-about", style()->standardIcon(QStyle::SP_MessageBoxInformation)),QString()),"text",[=]{return GAD_TR("About");});
    about->setObjectName("aboutButton");
    connect(about, &QPushButton::clicked, this, [this] { showAbout(); });
    auto *errorReport=I18n::localized(new QPushButton(QIcon::fromTheme("dialog-warning"),QString()),"text",[=]{return GAD_TR("Error report…");});errorReport->setObjectName("errorReportButton");
    connect(errorReport,&QPushButton::clicked,this,&Window::showErrorReport);
    // La préférence ne redémarre jamais le moteur : les transferts continuent normalement.
    auto *language = new QComboBox;language->setObjectName("languageSelector");
    I18n::bindProperty(language,"accessibleName",[=]{return GAD_TR("Language");});
    // Les drapeaux sont embarqués pour rester disponibles sans thème ni fichiers externes.
    language->setIconSize(QSize(24,24));
    language->addItem(QIcon::fromTheme("computer",style()->standardIcon(QStyle::SP_ComputerIcon)),GAD_TR("System"),"system");
    for(const auto &entry:I18n::languages()) {
        const auto icon=entry.flag.isEmpty()?QIcon::fromTheme("preferences-desktop-locale",style()->standardIcon(QStyle::SP_FileDialogDetailedView)):QIcon(entry.flag);
        language->addItem(icon,entry.name,entry.locale);
    }
    const auto savedLanguage=QSettings().value("language","system").toString();
    language->setCurrentIndex(qMax(0,language->findData(savedLanguage=="system"?savedLanguage:I18n::resolve(savedLanguage))));
    I18n::bindProperty(language,"toolTip",[=]{return GAD_TR("The language changes immediately without interrupting transfers.");});
    connect(language,&QComboBox::currentIndexChanged,this,[language](int){
        const auto choice=language->currentData().toString();QSettings().setValue("language",choice);
        I18n::initialize(choice);
    });
    auto *languageLabel=I18n::localized(new QLabel,"text",[]{return GAD_TR("Language");});languageLabel->setBuddy(language);
    auto *languageRow=new QHBoxLayout;languageRow->addWidget(languageLabel);languageRow->addWidget(language,1);settingsLayout->addLayout(languageRow);
    settingsLayout->addWidget(commandHint("language","--language LOCALE (system / fr_FR / en_US / …)"));
    auto *requestRate=I18n::localized(new QPushButton(QIcon::fromTheme("speedometer"),QString()),"text",[]{return GAD_TR("Limit request speed…");});requestRate->setObjectName("settingsRequestRate");
    controls->insertWidget(0,requestRate);connect(requestRate,&QPushButton::clicked,this,[this]{showRateLimit();});
    // La sélection peut évoluer pendant que les paramètres restent ouverts.
    connect(settingsButton,&QToolButton::clicked,this,[this,settingsDialog,requestRate]{requestRate->setEnabled(!tree->selectedItems().isEmpty());settingsDialog->show();settingsDialog->raise();settingsDialog->activateWindow();});
    connect(tree,&QTreeWidget::itemSelectionChanged,requestRate,[this,requestRate]{requestRate->setEnabled(!tree->selectedItems().isEmpty());});
    requestRate->setEnabled(!tree->selectedItems().isEmpty());
    auto *shortcuts=new QWidget;auto *shortcutOptions=new QVBoxLayout(shortcuts);shortcutOptions->setContentsMargins(16,16,16,16);shortcutOptions->setSpacing(12);settingsTabs->addTab(shortcuts,GAD_TR("Shortcuts"));
    auto *dolphinShortcut=I18n::localized(new QCheckBox,"text",[]{return GAD_TR("Ctrl+V with Gad in Dolphin");});dolphinShortcut->setObjectName("dolphinShortcut");shortcutOptions->addWidget(commandOption(dolphinShortcut,"--dolphin-ctrl-v / --no-dolphin-ctrl-v"));
    const auto helper=dolphinIntegration();dolphinShortcut->setEnabled(!helper.isEmpty());
    QFile receipt(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+"/gad/dolphin-paste.json");
    if(receipt.open(QIODevice::ReadOnly))dolphinShortcut->setChecked(!QJsonDocument::fromJson(receipt.readAll()).object()["files"].toObject().isEmpty());
    auto *shortcutNotice=I18n::localized(new QLabel,"text",[]{return GAD_TR("Restart Dolphin after changing its shortcut.");});shortcutNotice->setWordWrap(true);shortcutOptions->addWidget(shortcutNotice);
    connect(dolphinShortcut,&QCheckBox::toggled,this,[this,dolphinShortcut](bool enabled){
        {QSignalBlocker blocker(dolphinShortcut);dolphinShortcut->setChecked(!enabled);}
        configureDolphin(enabled,[this](const QJsonObject &result){if(result.contains("error"))error(result["error"].toString());});
    });
    auto *konquerorShortcut=I18n::localized(new QCheckBox,"text",[]{return GAD_TR("Ctrl+V with Gad in Konqueror");});konquerorShortcut->setObjectName("konquerorShortcut");shortcutOptions->addWidget(commandOption(konquerorShortcut,"--konqueror-ctrl-v / --no-konqueror-ctrl-v"));
    I18n::bindProperty(konquerorShortcut,"toolTip",[]{return GAD_TR("Applies to Konqueror launched with Gad. Text pastes remain native.");});
    konquerorShortcut->setChecked(QSettings().value("konquerorCtrlV",true).toBool());
    connect(konquerorShortcut,&QCheckBox::toggled,this,[](bool enabled){QSettings settings;settings.setValue("konquerorCtrlV",enabled);settings.sync();});
    auto *openKonqueror=I18n::localized(new QPushButton(QIcon::fromTheme("konqueror",style()->standardIcon(QStyle::SP_DirIcon)),QString()),"text",[]{return GAD_TR("Open Konqueror with Gad");});openKonqueror->setObjectName("openKonqueror");shortcutOptions->addWidget(openKonqueror);
    const auto konquerorLauncher=QStandardPaths::findExecutable("gad-konqueror");openKonqueror->setEnabled(!konquerorLauncher.isEmpty() && !QStandardPaths::findExecutable("konqueror").isEmpty());
    connect(openKonqueror,&QPushButton::clicked,this,[this,konquerorLauncher]{if(!QProcess::startDetached(konquerorLauncher,{}))error(GAD_TR("Unable to start Konqueror."));});
    shortcutOptions->addStretch();settingsLayout->addStretch();
    footer->addWidget(about);footer->addWidget(errorReport);footer->addStretch();footer->addWidget(summary,1);
    layout->addLayout(footer);
    setCentralWidget(body);
    setAcceptDrops(true);
    // Le systray garde un accès à Gad quand sa fenêtre est masquée et affiche le compteur de lots.
    tray = new QSystemTrayIcon(this);
    tray->setObjectName("transferTray");
    auto *trayMenu = new QMenu(this);
    auto *open = I18n::localized(trayMenu->addAction(QString()),"text",[=]{return GAD_TR("Show Gad");}); open->setIcon(QIcon::fromTheme("window",style()->standardIcon(QStyle::SP_TitleBarNormalButton)));
    connect(open, &QAction::triggered, this, [this] { showMainWindow(); });
    trayPause = I18n::localized(trayMenu->addAction(QString()),"text",[=]{return GAD_TR("Pause all");});
    connect(trayPause, &QAction::triggered, this, [this] {
        engine({{"command", Protocol::Command::Pause}, {"paused", !paused}}); refresh();
    });
    trayMenu->addSeparator();
    auto *quit = I18n::localized(trayMenu->addAction(QString()),"text",[=]{return GAD_TR("Quit Gad");}); quit->setIcon(QIcon::fromTheme("application-exit",style()->standardIcon(QStyle::SP_DialogCloseButton)));
    connect(quit, &QAction::triggered, this, [this] { quitRequested = true; close(); quitRequested = false; });
    tray->setContextMenu(trayMenu);
    connect(tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            if (isVisible() && !isMinimized()) hide();
            else { showMainWindow(); }
        }
    });
    // KDE pilote les mêmes actions ciblées que l’interface ; la notification ne manipule pas les fichiers.
    desktopTransfers = new TransferNotifications([this](const QString &action) {
        controlDesktopTransfers(action);
    },this,QGuiApplication::platformName()!="offscreen" && QGuiApplication::platformName()!="minimal");
    auto *timer = new QTimer(this); timer->setInterval(400); connect(timer, &QTimer::timeout, this, [this] { refresh(); }); timer->start();
    qApp->installEventFilter(this);
    I18n::onChange(language,"systemLabel",[language]{QSignalBlocker blocker(language);language->setItemText(0,GAD_TR("System"));},false);
    I18n::onChange(tree,"headers",[transferTree]{transferTree->updateHeaderIndicators();},false);
    I18n::onChange(this,"refreshLanguage",[this]{
        setLocale(QLocale());for(auto *widget:findChildren<QWidget *>())widget->setLocale(QLocale());
        refresh();
    },false);
    refresh();
    if (QGuiApplication::platformName() != "offscreen" && QGuiApplication::platformName() != "minimal") tray->show();
}

/// Libère les lignes avant leurs métadonnées, qui ne sont plus possédées par le widget arbre.
Window::~Window() {
    QSignalBlocker blocker(tree);
    for(auto *metadata:jobRows)if(metadata->treeWidget())tree->takeTopLevelItem(tree->indexOfTopLevelItem(metadata));
    tree->clear();qDeleteAll(jobRows);
}

/// La notification représente toute la file : sa pause partage l’état du bouton et du systray.
void Window::controlDesktopTransfers(const QString &action) {
    QJsonObject request;
    if(action==Protocol::Command::Cancel) {
        const auto ids=desktopTransfers->activeIds(engine({{"command",Protocol::Command::Status}}));
        if(ids.isEmpty())return;
        request={{"command","cancel_selection"},{"jobs",ids},{"entries",QJsonArray{}}};
    } else request={{"command",Protocol::Command::Pause},{"paused",action==Protocol::Command::Pause}};
    const auto result=engine(request);
    if(result.contains("error"))error(result["error"].toString());
    refresh();
}

/// Restaure la fenêtre et utilise, si fourni, le jeton Wayland autorisant son activation.
void Window::showMainWindow(const QString &token){
    if(!token.isEmpty())KWindowSystem::setCurrentXdgActivationToken(token);
    showNormal();raise();
    if(windowHandle())KWindowSystem::activateWindow(windowHandle());
}

/// Crée une boîte À propos non bloquante, détruite à sa fermeture.
void Window::showAbout(){
    auto *dialog = new QMessageBox(this);
    dialog->setObjectName("aboutDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    I18n::bindProperty(dialog,"windowTitle",[=]{return GAD_TR("About Gad");});
    dialog->setWindowIcon(windowIcon());
    dialog->setIconPixmap(windowIcon().pixmap(128, 128));
    dialog->setTextFormat(Qt::RichText);
    I18n::bindProperty(dialog,"text",[=]{return GAD_TR("<h2>Gad</h2><p><b>G</b>estionnaire <b>A</b>vancé de <b>D</b>uplication<br>Advanced Duplication Manager</p><p>Gad, il copie. (Gad copies.)</p>");});
    const auto date = QDate::fromString(QString::fromLatin1(GAD_BUILD_DATE), Qt::ISODate);
    I18n::bindProperty(dialog,"informativeText",[=]{return Qt::convertFromPlainText(QString(GAD_TR("Version: %1\nBuild date: %2")).arg(QString::fromUtf8(GAD_VERSION),date.isValid()?QLocale().toString(date,QLocale::ShortFormat):GAD_TR("not specified")))+Documentation::html();});
    dialog->setTextInteractionFlags(Qt::TextBrowserInteraction);
    for(auto *label:dialog->findChildren<QLabel *>()) {
        label->setOpenExternalLinks(false);
        connect(label,&QLabel::linkActivated,this,[](const QString &target){const QUrl url(target);if(url.isLocalFile())QDesktopServices::openUrl(url);});
    }
    // Un bouton personnalisé évite la réécriture du libellé par QDialogButtonBox lors de LanguageChange.
    auto *close=dialog->addButton(QString(),QMessageBox::RejectRole);close->setObjectName("aboutCloseButton");
    close->setIcon(QIcon::fromTheme("window-close",style()->standardIcon(QStyle::SP_DialogCloseButton)));
    I18n::bindProperty(close,"text",[]{return GAD_TR("Close");});
    dialog->setEscapeButton(close);
    dialog->open();
}

/// Intercepte les collages de fichiers même depuis le champ Destination, tout en laissant le collage de texte habituel.
bool Window::eventFilter(QObject *object, QEvent *event){
    auto *widget = qobject_cast<QWidget *>(object);
    if (widget && (widget == this || isAncestorOf(widget)) && widget->window() == this
        && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        const auto *key = static_cast<QKeyEvent *>(event);
        const auto *mime = QApplication::clipboard()->mimeData();
        const bool files = mime && (mime->hasUrls() || mime->hasFormat("application/x-kde4-urilist") || mime->hasFormat("x-special/gnome-copied-files"));
        if (key->matches(QKeySequence::Paste) && files) {
            event->accept();
            if (event->type() == QEvent::KeyPress) pasteInto(destination->text());
            return true;
        }
    }
    return QMainWindow::eventFilter(object, event);
}

/// Annonce l’acceptation des glissements contenant des URL ; le dépôt vérifie ensuite leurs chemins.
void Window::dragEnterEvent(QDragEnterEvent *event){
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

/// Valide les protocoles du dépôt et soumet les sources locales ou distantes dans leur ordre.
void Window::dropEvent(QDropEvent *event){
    QStringList paths;
    for (const auto &url : event->mimeData()->urls()) {
        if (!SourceUrls::supported(url)) { error(SourceUrls::unsupportedMessage()); return; }
        paths << (url.isLocalFile()?SourceUrls::localPath(url):url.toString(QUrl::FullyEncoded));
    }
    event->setDropAction(Qt::CopyAction); event->accept(); enqueue(paths);
}

/// Masque la fenêtre si le systray est disponible ; une vraie sortie attend l’absence de transfert actif.
void Window::closeEvent(QCloseEvent *event){
    if (!quitRequested && tray->isVisible() && QSystemTrayIcon::isSystemTrayAvailable()) {
        hide(); event->ignore(); return;
    }
    bool active = resolvingSources || !sourceQueue.isEmpty();
    for (const auto &j : engine({{"command", Protocol::Command::Status}})["jobs"].toArray()) if (!finished(j.toObject()["state"].toString())) active = true;
    if (active) {
        QMessageBox::information(this, "Gad", GAD_TR("Transfers are still queued or running. Let them finish or cancel the operations before quitting."));
        event->ignore(); return;
    }
    const auto shutdown=engine({{"command", Protocol::Command::Shutdown}});
    if (shutdown.contains("error")) { error(shutdown["error"].toString()); event->ignore(); return; }
    closing = true; event->accept();
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}
