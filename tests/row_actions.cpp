// Tests unitaires Qt des chemins et actions d’ouverture ; aucune application externe n’est lancée.
#include "../native/window.h"
#include "../native/cli_options.h"
#include "../native/application.h"
#include "../native/documentation.h"
#include "../native/kio_sources.h"
#include "../native/error_report.h"
#include <atomic>
#include <QtTest>
#include <QDesktopServices>
#include <QStatusBar>
#include <QScopeGuard>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <memory>

static QJsonObject status;
static QJsonObject errorReport;
static QJsonObject simulationPlan;
static std::atomic_int simulationDelay{0},simulationCancels{0},simulationCompleted{0};
static QList<QJsonObject> mutations;
static bool rejectExclusions=false,clearHistoryInMock=false;
/// Remplace uniquement la frontière Rust ; les vrais widgets et connexions du menu sont conservés.
extern "C" char *sc_request(const char *input) {
    const auto request=QJsonDocument::fromJson(input).object();QJsonObject result;
    if(request["command"]=="simulation_begin")result={{"id",1}};
    else if(request["command"]=="simulation_cancel"){++simulationCancels;result={{"ok",true}};}
    else if(request["command"]=="simulate"){QThread::msleep(simulationDelay.load());result=simulationPlan;++simulationCompleted;}
    else if(request["command"]=="error_report")result=errorReport;
    else if(request["command"]=="status" || request["command"]=="poll")result=status;
    else {mutations.append(request);if(clearHistoryInMock && (request["command"]=="clear" || request["command"]=="completion_clear")){QJsonArray kept;for(const auto &value:status["jobs"].toArray()){const auto row=value.toObject();if(!Protocol::terminal(row["state"].toString()) || row["undo_requested"].toBool())kept.append(row);}status["jobs"]=kept;}result=rejectExclusions && request["command"]=="configure" && request.contains("exclusions")?QJsonObject{{"error","Invalid exclusion rules"}}:QJsonObject{{"ok",true},{"id",1}};}
    return strdup(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}
extern "C" void sc_free(char *value){free(value);}

/// Pilote les véritables actions de langue, sans dépendre d'un contrôle dans les options.
class LanguageMenuDriver {
    QMenu *menu;
public:
    explicit LanguageMenuDriver(Window &window):menu(window.findChild<QMenu *>("languageMenu")){}
    LanguageMenuDriver *operator->(){return this;}
    explicit operator bool() const{return menu;}
    int count()const{return menu->actions().size();}
    int findData(const QString &locale)const{for(int i=0;i<count();++i)if(menu->actions()[i]->data().toString()==locale)return i;return -1;}
    int currentIndex()const{for(int i=0;i<count();++i)if(menu->actions()[i]->isChecked())return i;return -1;}
    QVariant currentData()const{return currentIndex()<0?QVariant{}:menu->actions()[currentIndex()]->data();}
    QIcon itemIcon(int index)const{return index<0?QIcon{}:menu->actions()[index]->icon();}
    QString itemText(int index)const{return index<0?QString{}:menu->actions()[index]->text();}
    void setCurrentIndex(int index){if(index>=0 && index<count())menu->actions()[index]->trigger();}
};

/// Intercepte le protocole file de QDesktopServices, y compris les ouvertures de dossiers.
class DesktopCapture : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public Q_SLOTS:
    void capture(const QUrl &url){urls.append(url);}
};

/// Chaque test de profils restaure les préférences du compte Qt isolé.
struct ProfileSettingsGuard {
    QMap<QString,QVariant> previous;
    QByteArray startupBytes;bool startupExists=QFileInfo::exists(autostartPath());
    ProfileSettingsGuard(){if(startupExists){QFile file(autostartPath());if(!file.open(QIODevice::ReadOnly))qFatal("Cannot snapshot test startup file");startupBytes=file.readAll();}for(const auto &key:QStringList{"progressMessage","completionAction","completionDelay","verifyFastMove","conflictPolicy","transferProfiles","requestRateLimit","rateLimit","verifyMd5","verifyProperties","verifySpace","allowDuplication","includeHidden","excludeDirectories","excludeFiles","keepHistory","resumeInterrupted","language","konquerorCtrlV"})previous[key]=QSettings().value(key);QSettings().remove("transferProfiles");QSettings().remove("requestRateLimit");}
    ~ProfileSettingsGuard(){QSettings settings;for(auto it=previous.begin();it!=previous.end();++it){if(it.value().isValid())settings.setValue(it.key(),it.value());else settings.remove(it.key());}if(startupExists){QFile file(autostartPath());if(!file.open(QIODevice::WriteOnly))qFatal("Cannot restore test startup file");file.write(startupBytes);}else QFile::remove(autostartPath());I18n::initialize("fr_FR");}
};
static TransferProfiles::Profile sampleProfile(const QString &name="Backup") {
    TransferProfiles::Profile p;p.id=QUuid::createUuid().toString(QUuid::WithoutBraces);p.name=name;p.rate=2621440;
    p.options={{"verify_fast_move",false},{"conflict_policy","ask"},{"verify_md5",true},{"verify_properties",true},{"verify_space",true},{"allow_duplication",false},{"keep_history",false},{"resume_interrupted",false},{"rate_limit",1048576},{"exclusions",QJsonObject{{"include_hidden",false},{"directories",QJsonArray{"__*"}},{"files",QJsonArray{"*.tmp"}}}}};p.gui={{"progressMessage",true},{"completion_action","none"},{"completion_delay",30},{"language","fr_FR"},{"autostart",false},{"dolphin_ctrl_v",false},{"konqueror_ctrl_v",false}};return p;
}

/// Simuler uniquement l’action externe Dolphin ; conserver les vrais widgets et le moteur de configuration Qt.
class ProfileWindow : public Window {
public:
    bool rejectShortcut=false;QList<bool> shortcutCalls;
    void configureDolphin(bool enabled,std::function<void(QJsonObject)> reply) override {
        shortcutCalls.append(enabled);QTimer::singleShot(0,this,[this,enabled,reply]{if(rejectShortcut){reply({{"error","Shortcut configuration rejected"}});return;}auto *check=findChild<QCheckBox *>("dolphinShortcut");QSignalBlocker blocker(check);check->setChecked(enabled);reply({{"ok",true}});});
    }
};

/// Isoler les effets externes tout en vérifiant la validation des requêtes combinées.
class ConfigurationWindow : public ProfileWindow {
public:
    QList<bool> krusaderCalls;
    void configureKrusader(bool enabled,std::function<void(QJsonObject)> reply) override {
        krusaderCalls.append(enabled);
        auto *check=findChild<QCheckBox *>("krusaderShortcut");QSignalBlocker blocker(check);check->setChecked(enabled);
        reply({{"ok",true}});
    }
};

/// Aucun test ne ferme Gad ni ne contacte logind : les effets de fin de file passent par ce double.
class CompletionWindow : public Window {
public:
    QStringList actions,notices;
    void executeCompletion(const QString &action,std::function<void(QJsonObject)> reply) override {actions.append(action);reply({{"ok",true}});}
    void completionNotice(const QString &title,const QString &message) override {notices.append(title+"\n"+message);}
};

class DelayedCompletionWindow : public CompletionWindow {
public:
    std::function<void(QJsonObject)> pending;
    int shortcutCalls=0;
    void executeCompletion(const QString &action,std::function<void(QJsonObject)> reply) override {actions.append(action);pending=reply;}
    void configureDolphin(bool,std::function<void(QJsonObject)> reply) override {++shortcutCalls;reply({{"ok",true}});}
};

class RowActionsTest : public QObject {
    Q_OBJECT
    DesktopCapture desktop;
    /// Construit un lot minimal, sans dépendre des fichiers de l’utilisateur.
    static QJsonObject job(const QJsonArray &entries,const QString &state="done") {
        return {{"id",1},{"sources",QJsonArray{"/tmp/source"}},{"destination","/tmp/destination"},{"state",state},{"entries",entries},{"mode","copy"}};
    }
    /// Les caractères spéciaux doivent rester un chemin de fichier, jamais une commande ou un fragment d’URL.
    static QJsonObject file(const QString &state="done") {
        return {{"kind","file"},{"source","/tmp/source/a # ; $(ignored).txt"},{"destination","/tmp/destination/a # ; $(ignored).txt"},{"state",state},{"size",4},{"copied",4}};
    }
    static void publish(const QJsonObject &value){status["jobs"]=QJsonArray{value};}
    static QTreeWidgetItem *selectFile(Window &window,int index=0) {
        auto *tree=window.findChild<QTreeWidget *>();auto *item=window.entryRow(1,index);
        for(auto *parent=item->parent();parent;parent=parent->parent())parent->setExpanded(true);
        tree->setCurrentItem(item);item->setSelected(true);return item;
    }
    /// Arbre préordonné comme celui produit par l’analyse Rust, avec une feuille extérieure au dossier.
    static QJsonArray nestedEntries() {
        QJsonArray entries;
        for(const auto &name:QStringList{"root","root/sub","root/sub/a.txt","root/b.txt","outside.txt"}) {
            auto entry=file("waiting");entry["source"]="/tmp/source/"+name;entry["destination"]="/tmp/destination/"+name;
            if(!name.endsWith(".txt")){entry["kind"]="directory";entry["state"]="running";entry["size"]=0;entry["copied"]=0;}
            entries.append(entry);
        }
        return entries;
    }
private Q_SLOTS:
    /// En arrière-plan les compteurs continuent, puis la vue rattrape toutes les révisions à l’ouverture.
    void backgroundTreeDefersRowsButKeepsTransfersAndSummary() {
        auto value=job(nestedEntries(),"running");publish(value);Window window(true);auto *tree=window.findChild<QTreeWidget *>();QCOMPARE(tree->topLevelItemCount(),0);
        auto *tracking=window.findChild<TransferNotifications *>("desktopTransfers");QVERIFY(tracking);QVERIFY(tracking->isTracking());QVERIFY(window.findChild<QPushButton *>("queueControlButton")->isEnabled());
        auto entries=nestedEntries();for(int i=0;i<entries.size();++i){auto e=entries[i].toObject();e["state"]="done";e["copied"]=e["size"];entries[i]=e;}publish(job(entries,"done"));window.refresh();QCOMPARE(tree->topLevelItemCount(),0);QVERIFY(!window.findChild<QPushButton *>("queueControlButton")->isEnabled());
        window.showMainWindow();QVERIFY(window.entryRow(1,0));QCOMPARE(window.entryRow(1,2)->text(2),QString("OK"));QVERIFY(!window.entryRow(1,0)->isExpanded());
        window.showMinimized();QCoreApplication::processEvents();publish(job(nestedEntries(),"running"));window.refresh();QCOMPARE(window.entryRow(1,2)->text(2),QString("OK"));
        window.showNormal();QCoreApplication::processEvents();QVERIFY(window.entryRow(1,2)->text(2)!=QString("OK"));
        window.hide();status["jobs"]=QJsonArray{};window.refresh();QVERIFY(tree->topLevelItemCount()>0);window.showMainWindow();QCOMPARE(tree->topLevelItemCount(),0);
    }
    /// Une branche ajoutée ne transite pas par la racine, et les ajouts gardent les anciens objets.
    void newBranchesInsertWithoutRemovingRows() {
        status={{"jobs",QJsonArray{}},{"paused",false}};Window window;auto *tree=window.findChild<QTreeWidget *>();
        QSignalSpy removed(tree->model(),&QAbstractItemModel::rowsRemoved),inserted(tree->model(),&QAbstractItemModel::rowsInserted);
        auto entries=nestedEntries();publish(job(entries,"running"));window.refresh();QCOMPARE(removed.count(),0);QCOMPARE(inserted.count(),1);
        auto *root=window.entryRow(1,0);auto *leaf=window.entryRow(1,2);QVERIFY(leaf->parent()==window.entryRow(1,1));leaf->setSelected(true);root->setExpanded(false);
        inserted.clear();removed.clear();
        for(int i=0;i<100;++i){auto entry=file("waiting");entry["source"]=QString("/tmp/source/root/extra%1.txt").arg(i);entry["destination"]=QString("/tmp/destination/root/extra%1.txt").arg(i);entries.append(entry);}
        publish(job(entries,"running"));window.refresh();QCOMPARE(removed.count(),0);QCOMPARE(inserted.count(),1);QCOMPARE(window.entryRow(1,2),leaf);QVERIFY(leaf->isSelected());QVERIFY(!root->isExpanded());QCOMPARE(window.entryRow(1,104)->parent(),root);
        QVERIFY(tree->updatesEnabled());QCOMPARE(tree->header()->sectionResizeMode(TableView::OrderColumn),QHeaderView::ResizeToContents);
    }
    /// Les réglages survivent à une nouvelle lecture, et les noms Unicode équivalents ne se remplacent pas.
    void profilesPersistAndRejectEquivalentNames() {
        ProfileSettingsGuard guard;QString failure;auto profile=sampleProfile(QString::fromUtf8("Été"));QVERIFY(TransferProfiles::store({profile},failure));
        const auto stored=QSettings().value("transferProfiles");const auto loaded=TransferProfiles::load(failure);QVERIFY(failure.isEmpty());QCOMPARE(loaded.size(),1);QCOMPARE(loaded[0].json(),profile.json());
        auto duplicate=sampleProfile(QString::fromUtf8("été"));QVERIFY(!TransferProfiles::store({profile,duplicate},failure));QCOMPARE(QSettings().value("transferProfiles"),stored);
        QVERIFY(!loaded[0].json().contains("destination"));QCOMPARE(loaded[0].options["keep_history"].toBool(),false);QCOMPARE(loaded[0].gui["language"].toString(),QString("fr_FR"));
    }
    /// Une donnée future ou abîmée n’est jamais écrasée depuis les actions de gestion.
    void corruptProfilesStayUntouched() {
        ProfileSettingsGuard guard;const QByteArray raw="{\"version\":99,\"profiles\":[]}";QSettings().setValue("transferProfiles",raw);QString failure;
        QVERIFY(TransferProfiles::load(failure).isEmpty());QVERIFY(!failure.isEmpty());Window window;window.showProfiles();auto *dialog=window.findChild<QDialog *>("profilesDialog");QVERIFY(dialog);
        QVERIFY(!dialog->findChild<QPushButton *>("saveNewProfile")->isEnabled());QCOMPARE(QSettings().value("transferProfiles").toByteArray(),raw);
    }
    /// Un clic applique tous les réglages, y compris le débit global, sans changer la destination ni les fichiers.
    void profileMenuAppliesAllOptionsWithoutChangingDestination() {
        ProfileSettingsGuard guard;QString failure;const auto profile=sampleProfile("Photos & videos");QVERIFY(TransferProfiles::store({profile},failure));
        publish(job({file("running")},"running"));Window window;auto *global=window.findChild<QDoubleSpinBox *>("globalRateLimit");global->setValue(7.25);mutations.clear();
        window.refreshProfilesMenu();auto *action=window.findChild<QAction *>("applyProfile_"+profile.id);QVERIFY(action);QCOMPARE(action->text(),QString("Photos && videos"));const auto destinationBefore=window.findChild<QLineEdit *>("destinationInput")->text();action->trigger();QCoreApplication::processEvents();
        QCOMPARE(window.findChild<QLineEdit *>("destinationInput")->text(),destinationBefore);QCOMPARE(window.findChild<QDoubleSpinBox *>("defaultRequestRateLimit")->value(),2.5);QCOMPARE(global->value(),1.0);
        QVERIFY(window.findChild<QCheckBox *>("verifyMd5")->isChecked());QVERIFY(!window.findChild<QCheckBox *>("includeHidden")->isChecked());
        QCOMPARE(mutations.size(),1);QCOMPARE(mutations[0]["command"].toString(),QString("configure"));QCOMPARE(mutations[0]["rate_limit"].toInteger(),1048576);QVERIFY(!mutations[0]["keep_history"].toBool());QVERIFY(!window.findChild<QCheckBox *>("resumeInterrupted")->isChecked());QVERIFY(!window.findChild<QCheckBox *>("konquerorShortcut")->isChecked());
        QCOMPARE(status["jobs"].toArray()[0].toObject()["state"].toString(),QString("running"));
        mutations.clear();window.enqueue({"/tmp/a"});QCOMPARE(mutations.last()["rate_limit"].toInteger(),2621440);QCOMPARE(mutations.last()["exclusions"].toObject(),profile.options["exclusions"].toObject());
        const auto queued=mutations.last();window.findChild<QDoubleSpinBox *>("defaultRequestRateLimit")->setValue(1);QCOMPARE(queued["rate_limit"].toInteger(),2621440);
        window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/b"}},{"destination","/tmp/other"},{"rate_limit",0}});QCOMPARE(mutations.last()["rate_limit"].toInteger(),0);
        window.findChild<QLineEdit *>("destinationInput")->clear();QVERIFY(window.applyTransferProfile(profile.id));QVERIFY(window.findChild<QLineEdit *>("destinationInput")->text().isEmpty());window.refreshProfilesMenu();QVERIFY(window.findChild<QAction *>("applyProfile_"+profile.id)->isChecked());
    }
    /// Un rejet moteur ne laisse ni destination ni limites ni cases appliquées partiellement.
    void rejectedProfileLeavesCurrentSettingsIntact() {
        ProfileSettingsGuard guard;QString failure;const auto profile=sampleProfile();QVERIFY(TransferProfiles::store({profile},failure));Window window;
        auto *destination=window.findChild<QLineEdit *>("destinationInput");destination->setText("/tmp/before");auto *rate=window.findChild<QDoubleSpinBox *>("defaultRequestRateLimit");rate->setValue(1);
        const auto checked=window.findChild<QCheckBox *>("verifyMd5")->isChecked();rejectExclusions=true;QVERIFY(!window.applyTransferProfile(profile.id));rejectExclusions=false;
        QCOMPARE(destination->text(),QString("/tmp/before"));QCOMPARE(rate->value(),1.0);QCOMPARE(window.findChild<QCheckBox *>("verifyMd5")->isChecked(),checked);
    }
    /// Enregistrer conserve les options exactes et ne conserve jamais le chemin de destination.
    void profileDialogSavesSnapshotAndManagerEditsDefinitionsOnly() {
        ProfileSettingsGuard guard;Window window;auto *destination=window.findChild<QLineEdit *>("destinationInput");destination->setText("/tmp/saved target");auto *defaultRate=window.findChild<QDoubleSpinBox *>("defaultRequestRateLimit");defaultRate->setValue(4.5);
        window.saveCurrentProfile();auto *save=window.findChild<QDialog *>("saveProfileDialog");QVERIFY(save);I18n::initialize("en_US");QVERIFY(save->findChild<QPlainTextEdit *>("profilePreview")->toPlainText().contains("Global speed limit:"));save->findChild<QLineEdit *>("profileName")->setText("Personal <backup>");save->findChild<QPushButton *>("confirmSaveProfile")->click();
        QString failure;auto profiles=TransferProfiles::load(failure);QCOMPARE(profiles.size(),1);QCOMPARE(profiles[0].name,QString("Personal <backup>"));QCOMPARE(profiles[0].rate,4718592);QCOMPARE(defaultRate->value(),4.5);
        QVERIFY(!profiles[0].json().contains("destination"));Window second;QVERIFY(second.applyTransferProfile(profiles[0].id));QCOMPARE(second.findChild<QDoubleSpinBox *>("defaultRequestRateLimit")->value(),4.5);
        window.showProfiles();auto *dialog=window.findChild<QDialog *>("profilesDialog");auto *list=dialog->findChild<QListWidget *>("profilesList");QCOMPARE(list->count(),1);QCOMPARE(list->currentItem()->text(),QString("Personal <backup>"));
        dialog->findChild<QPushButton *>("renameProfile")->click();auto *rename=dialog->findChild<QInputDialog *>("renameProfileDialog");QVERIFY(rename);rename->setTextValue("Renamed");rename->accept();QCOMPARE(TransferProfiles::load(failure)[0].name,QString("Renamed"));
        destination->setText("/tmp/updated target");dialog->findChild<QPushButton *>("updateProfile")->click();QVERIFY(!TransferProfiles::load(failure)[0].json().contains("destination"));
        mutations.clear();dialog->findChild<QPushButton *>("deleteProfile")->click();QVERIFY(TransferProfiles::load(failure).isEmpty());QVERIFY(mutations.isEmpty());QCOMPARE(destination->text(),QString("/tmp/updated target"));QCOMPARE(list->count(),0);
    }
    /// Une sélection reste un item stable ; le changement de langue conserve l’identifiant sélectionné.
    void profileSelectionSurvivesLanguageChanges() {
        ProfileSettingsGuard guard;QString failure;auto first=sampleProfile("First"),second=sampleProfile("Second");QVERIFY(TransferProfiles::store({first,second},failure));Window window;window.showProfiles();
        auto *list=window.findChild<QListWidget *>("profilesList");auto *item=list->item(1);list->setCurrentRow(1);QCOMPARE(list->currentItem(),item);
        const auto restore=qScopeGuard([]{I18n::initialize("fr_FR");});I18n::initialize("en_US");QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(),second.id);QCOMPARE(window.findChild<QDialog *>("profilesDialog")->windowTitle(),QString("Transfer profiles"));
    }
    /// Les limites futures ont leur propre commande et les valeurs invalides ne touchent pas les réglages.
    /// Un échec externe après Krusader remet son raccourci dans son état initial.
    void validConfigurationRestoresShortcutsWhenDolphinFails() {
        ProfileSettingsGuard guard;ConfigurationWindow window;const auto previous=window.currentTransferProfile();
        window.rejectShortcut=true;bool completed=false;QJsonObject response;mutations.clear();
        window.handleAsync({{"command","configure"},{"gui_settings",QJsonObject{{"krusader_ctrl_v",true},{"dolphin_ctrl_v",true}}}},[&](QJsonObject result){response=result;completed=true;});
        QTRY_VERIFY(completed);QVERIFY(response.contains("error"));
        QCOMPARE(window.currentTransferProfile().gui,previous.gui);QCOMPARE(window.krusaderCalls,QList<bool>({true,false}));QVERIFY(mutations.isEmpty());
    }
    /// Le refus du moteur restitue les raccourcis et le fichier de démarrage.
    void validConfigurationRestoresExternalSettingsWhenEngineFails() {
        ProfileSettingsGuard guard;ConfigurationWindow window;const auto previous=window.currentTransferProfile();
        const bool startup=autostartEnabled();rejectExclusions=true;const auto restore=qScopeGuard([]{rejectExclusions=false;});
        bool completed=false;QJsonObject response;
        window.handleAsync({{"command","configure"},{"exclusions",previous.options["exclusions"]},{"gui_settings",QJsonObject{{"krusader_ctrl_v",true},{"autostart",!startup}}}},[&](QJsonObject result){response=result;completed=true;});
        QTRY_VERIFY(completed);QVERIFY(response.contains("error"));QCOMPARE(autostartEnabled(),startup);
        QCOMPARE(window.currentTransferProfile().gui,previous.gui);QCOMPARE(window.krusaderCalls,QList<bool>({true,false}));
    }
    /// Une configuration asynchrone ne doit pas écraser une seconde modification arrivée entre ses étapes.
    void combinedConfigurationBlocksConcurrentChanges() {
        ProfileSettingsGuard guard;ConfigurationWindow window;bool completed=false;QJsonObject response;
        auto *draft=window.findChild<QPlainTextEdit *>("excludedFiles");QVERIFY(draft);draft->setPlainText("*.draft");
        window.handleAsync({{"command","configure"},{"gui_settings",QJsonObject{{"krusader_ctrl_v",true},{"dolphin_ctrl_v",true}}}},[&](QJsonObject result){response=result;completed=true;});
        QVERIFY(!completed);auto *exclusions=window.findChild<QWidget *>("exclusionsPanel");QVERIFY(exclusions);QVERIFY(!exclusions->isEnabled());
        QFocusEvent disabledBlur(QEvent::FocusOut,Qt::OtherFocusReason);QApplication::sendEvent(draft,&disabledBlur);
        QVERIFY(!window.findChild<QMessageBox *>("errorDialog"));QJsonObject concurrent;
        window.handleAsync({{"command","configure"},{"keep_history",true}},[&](QJsonObject result){concurrent=result;});
        const bool rejected=concurrent.contains("error");
        QTRY_VERIFY(completed);QVERIFY(!response.contains("error"));QVERIFY(rejected);QVERIFY(exclusions->isEnabled());
        QCOMPARE(draft->toPlainText(),QString("*.draft"));
        QFocusEvent enabledBlur(QEvent::FocusOut,Qt::TabFocusReason);QApplication::sendEvent(draft,&enabledBlur);
        QCOMPARE(window.currentTransferProfile().options["exclusions"].toObject()["files"].toArray(),QJsonArray{"*.draft"});
    }
    /// Un réglage explicite reste réapplicable sans réécrire les options internes.
    void explicitShortcutOnlyPreservesOtherSettings() {
        ProfileSettingsGuard guard;ConfigurationWindow window;const auto previous=window.currentTransferProfile();
        const bool enabled=previous.gui["krusader_ctrl_v"].toBool();mutations.clear();bool completed=false;QJsonObject response;
        window.handleAsync({{"command","configure"},{"gui_settings",QJsonObject{{"krusader_ctrl_v",enabled}}}},[&](QJsonObject result){response=result;completed=true;});
        QTRY_VERIFY(completed);QVERIFY(!response.contains("error"));QCOMPARE(window.krusaderCalls,QList<bool>{enabled});QVERIFY(mutations.isEmpty());
        QCOMPARE(window.currentTransferProfile().gui,previous.gui);QCOMPARE(window.currentTransferProfile().options,previous.options);
    }

    /// Une modification externe du démarrage reste intacte lors d’une demande portant sur un seul raccourci.
    void shortcutConfigurationPreservesExternallyChangedStartupFile() {
        ProfileSettingsGuard guard;QVERIFY(setAutostart(false));ConfigurationWindow window;
        QVERIFY(setAutostart(true));QFile saved(autostartPath());QVERIFY(saved.open(QIODevice::ReadOnly));const auto contents=saved.readAll();saved.close();
        bool completed=false;QJsonObject response;
        window.handleAsync({{"command","configure"},{"gui_settings",QJsonObject{{"krusader_ctrl_v",true}}}},[&](QJsonObject result){response=result;completed=true;});
        QTRY_VERIFY(completed);QVERIFY(!response.contains("error"));QVERIFY(autostartEnabled());
        QFile current(autostartPath());QVERIFY(current.open(QIODevice::ReadOnly));QCOMPARE(current.readAll(),contents);
    }
    /// Seul zéro signifie illimité ; une valeur positive inférieure à un demi-octet ne peut pas devenir zéro.
    void cliRejectsPositiveRatesRoundedToUnlimited() {
        for(const auto &option:QStringList{"--rate-limit","--request-rate-limit"}) {
            QString failure;QVERIFY(CliOptions::configure({"gad","--configure",option,"0.0000001"},failure).isEmpty());QVERIFY(!failure.isEmpty());
            const auto valid=CliOptions::configure({"gad","--configure",option,"0.000001"},failure);QVERIFY(failure.isEmpty());
            const auto bytes=option=="--rate-limit"?valid["rate_limit"]:valid["gui_settings"].toObject()["request_rate_limit"];
            QCOMPARE(bytes.toInteger(),qint64(1));
        }
    }

    /// Les séparateurs suivent la langue de l’interface sans modifier le nombre d’octets ni envoyer de commande.
    /// Une saisie positive sous la résolution d’un octet reste une limite active, jamais « illimité ».
    void positiveSpeedInputCannotRoundToUnlimited() {
        ProfileSettingsGuard guard;Window window;
        for(const auto &name:QStringList{"globalRateLimit","defaultRequestRateLimit"}) {
            auto *spin=window.findChild<QDoubleSpinBox *>(name);QVERIFY(spin);spin->setValue(2.5);
            auto *editor=spin->findChild<QLineEdit *>();QVERIFY(editor);
            editor->setText(spin->locale().toString(0.0000001,'f',7));spin->interpretText();
            QCOMPARE(qRound64(spin->value()*1048576),qint64(1));
        }
    }

    void speedLimitLocaleChangesWithoutChangingBytes() {
        ProfileSettingsGuard guard;I18n::initialize("fr_FR");Window window;
        QVERIFY(!window.handle({{"command","configure"},{"rate_limit",1},{"gui_settings",QJsonObject{{"request_rate_limit",1}}}}).contains("error"));
        mutations.clear();
        for(const auto &language:QStringList{"en_US","de_DE","fr_FR"}) {
            I18n::initialize(language);
            for(const auto &name:QStringList{"globalRateLimit","defaultRequestRateLimit"}) {
                const auto *spin=window.findChild<QDoubleSpinBox *>(name);QVERIFY(spin);QCOMPARE(spin->locale().name(),QLocale(language).name());
                QCOMPARE(qRound64(spin->value()*1048576),qint64(1));
            }
        }
        QVERIFY(mutations.isEmpty());
    }

    void exactSpeedLimitsSurviveStartupAndProfiles_data() {
        QTest::addColumn<qint64>("bytes");
        QTest::newRow("one-byte")<<qint64(1);QTest::newRow("one-kibibyte")<<qint64(1024);
        QTest::newRow("fractional-mib")<<qint64(1234567);QTest::newRow("near-maximum")<<qint64((1LL<<40)-1);
    }
    /// Une limite fine ne doit pas être arrondie à zéro, ni devenir illimitée lors du démarrage ou d’une capture.
    void exactSpeedLimitsSurviveStartupAndProfiles() {
        QFETCH(qint64,bytes);ProfileSettingsGuard guard;QSettings().setValue("rateLimit",bytes);QSettings().setValue("requestRateLimit",bytes);
        Window window;const auto profile=window.currentTransferProfile();
        QCOMPARE(profile.options["rate_limit"].toInteger(),bytes);QCOMPARE(profile.rate,bytes);
        QVERIFY(!window.transferProfileDescription(profile).contains(QLocale().toString(0.,'f',2)+GAD_TR(" MiB/s")));
        for(const auto &name:QStringList{"globalRateLimit","defaultRequestRateLimit"}) {
            auto *spin=window.findChild<QDoubleSpinBox *>(name);QVERIFY(spin);QCOMPARE(qRound64(spin->value()*1048576),bytes);
            spin->setValue(2.5);QCOMPARE(spin->cleanText(),spin->locale().toString(2.5,'f',2));
        }
    }
    /// Valider une limite existante dans le menu contextuel ne change pas son nombre d’octets.
    void selectedSpeedLimitRetainsSingleByte() {
        ProfileSettingsGuard guard;auto request=job({file("waiting")},"waiting");request["rate_limit"]=1;publish(request);Window window;
        auto *tree=window.findChild<QTreeWidget *>();QVERIFY(tree);tree->topLevelItem(0)->setSelected(true);window.showRateLimit();
        auto *dialog=window.findChild<QDialog *>("requestRateDialog");QVERIFY(dialog);auto *spin=dialog->findChild<QDoubleSpinBox *>("requestRateLimit");QVERIFY(spin);
        QCOMPARE(qRound64(spin->value()*1048576),qint64(1));
    }

    void configurationRejectsInvalidValuesBeforeShortcutChanges_data() {
        QTest::addColumn<QJsonObject>("request");
        const QList<QPair<QString,QJsonValue>> invalidGui{
            {"language","not-a-locale"},{"request_rate_limit",-1},{"autostart","false"},
            {"dolphin_ctrl_v","true"},{"krusader_ctrl_v","true"},
            {"completion_action","invalid"},{"completion_delay",0}};
        for(const auto &field:invalidGui) {
            QJsonObject gui{{"krusader_ctrl_v",true},{"dolphin_ctrl_v",true}};gui[field.first]=field.second;
            QTest::newRow(qPrintable("gui-"+field.first))<<QJsonObject{{"command","configure"},{"gui_settings",gui}};
        }
        for(const auto &field:QList<QPair<QString,QJsonValue>>{{"rate_limit",-1},{"verify_md5","true"},{"conflict_policy","replace"},{"exclusion_patch",QJsonObject{{"files","*.tmp"}}},{"unexpected_setting",true}})
            QTest::newRow(qPrintable("engine-"+field.first))<<QJsonObject{{"command","configure"},{field.first,field.second},{"gui_settings",QJsonObject{{"krusader_ctrl_v",true},{"dolphin_ctrl_v",true}}}};
        QTest::newRow("invalid-gui-object")<<QJsonObject{{"command","configure"},{"gui_settings",QJsonArray{}}};
        QTest::newRow("invalid-with-profile")<<QJsonObject{{"command","configure"},{"profile","Saved"},{"gui_settings",QJsonObject{{"language","not-a-locale"}}}};
    }
    /// Aucun raccourci ni réglage moteur ne change pour une demande invalide.
    void configurationRejectsInvalidValuesBeforeShortcutChanges() {
        QFETCH(QJsonObject,request);ProfileSettingsGuard guard;ConfigurationWindow window;
        if(request.contains("profile")){QString failure;auto profile=sampleProfile("Saved");profile.gui["dolphin_ctrl_v"]=true;profile.gui["krusader_ctrl_v"]=true;QVERIFY(TransferProfiles::store({profile},failure));}
        const auto previous=window.currentTransferProfile();mutations.clear();bool completed=false;QJsonObject response;
        window.handleAsync(request,[&](QJsonObject result){response=result;completed=true;});
        QTRY_VERIFY(completed);QVERIFY(response.contains("error"));
        QVERIFY(window.krusaderCalls.isEmpty());QVERIFY(window.shortcutCalls.isEmpty());QVERIFY(mutations.isEmpty());
        QCOMPARE(window.currentTransferProfile().gui,previous.gui);
        QCOMPARE(window.currentTransferProfile().options,previous.options);
    }

    /// La lecture Qt ne doit pas ajouter gui_settings:null à une demande qui ne contient que des options moteur.
    void configurationWithoutGuiSettingsRemainsValid() {
        ProfileSettingsGuard guard;ConfigurationWindow window;mutations.clear();bool completed=false;QJsonObject response;
        const QJsonObject request{{"command","configure"},{"keep_history",false}};
        window.handleAsync(request,[&](QJsonObject result){response=result;completed=true;});
        QTRY_VERIFY(completed);QVERIFY(!response.contains("error"));QCOMPARE(mutations.size(),1);QCOMPARE(mutations.first(),request);
        QVERIFY(window.krusaderCalls.isEmpty());QVERIFY(window.shortcutCalls.isEmpty());
    }

    void requestRateConfigurationIsSeparateAndValidated() {
        ProfileSettingsGuard guard;Window window;auto *global=window.findChild<QDoubleSpinBox *>("globalRateLimit");const auto previous=global->value();QString failure;
        const auto request=CliOptions::configure({"gad","--configure","--request-rate-limit","3.5"},failure);QVERIFY(failure.isEmpty());mutations.clear();QVERIFY(!window.handle(request).contains("error"));QVERIFY(mutations.isEmpty());QCOMPARE(global->value(),previous);QCOMPARE(window.findChild<QDoubleSpinBox *>("defaultRequestRateLimit")->value(),3.5);QCOMPARE(SourceUrls::requiredProtocol(request),25);
        QVERIFY(window.handle({{"command","configure"},{"gui_settings",QJsonObject{{"request_rate_limit",-1}}}}).contains("error"));QCOMPARE(window.findChild<QDoubleSpinBox *>("defaultRequestRateLimit")->value(),3.5);
        CliOptions::configure({"gad","--configure","--request-rate-limit","-1"},failure);QVERIFY(!failure.isEmpty());
    }

    /// La migration complète une seule fois les options manquantes, puis retire les anciennes destinations.
    void legacyProfilesMigrateWithoutKeepingDestination() {
        ProfileSettingsGuard guard;QSettings().setValue("keepHistory",false);QSettings().setValue("resumeInterrupted",true);QSettings().setValue("rateLimit",3145728);QSettings().setValue("language","en_US");
        const auto sample=sampleProfile();auto old=sample.options;old.remove("verify_fast_move");old.remove("conflict_policy");old.remove("keep_history");old.remove("resume_interrupted");old.remove("rate_limit");old["id"]=sample.id;old["name"]=sample.name;old["destination"]="/private/old-target";old["rate_limit"]=sample.rate;
        QSettings().setValue("transferProfiles",QJsonDocument(QJsonObject{{"version",1},{"profiles",QJsonArray{old}}}).toJson());QString failure;
        const auto migrated=TransferProfiles::load(failure);QVERIFY(failure.isEmpty());QCOMPARE(migrated.size(),1);QCOMPARE(migrated[0].options["rate_limit"].toInteger(),3145728);QCOMPARE(migrated[0].gui["language"].toString(),QString("en_US"));QCOMPARE(migrated[0].rate,sample.rate);
        const auto raw=QSettings().value("transferProfiles").toByteArray();QVERIFY(!raw.contains("destination"));QVERIFY(!raw.contains("/private/old-target"));QCOMPARE(QJsonDocument::fromJson(raw).object()["version"].toInt(),6);
        QSettings().setValue("rateLimit",0);QCOMPARE(TransferProfiles::load(failure)[0].options["rate_limit"].toInteger(),3145728);
    }
    /// Les effets externes sont asynchrones ; la destination reste celle choisie par l’utilisateur, même pendant l’attente.
    void fullProfileIncludesStartupLanguageAndBothShortcuts() {
        ProfileSettingsGuard guard;QString failure;auto profile=sampleProfile();profile.gui={{"progressMessage",true},{"completion_action","none"},{"completion_delay",30},{"language","en_US"},{"autostart",true},{"dolphin_ctrl_v",true},{"konqueror_ctrl_v",true}};profile.options["keep_history"]=true;profile.options["resume_interrupted"]=true;QVERIFY(TransferProfiles::store({profile},failure));
        ProfileWindow window;auto *destination=window.findChild<QLineEdit *>("destinationInput");destination->setText("/tmp/current target");QVERIFY(window.applyTransferProfile(profile.id));QVERIFY(!window.findChild<QToolButton *>("profilesButton")->isEnabled());destination->setText("/tmp/changed while waiting");
        QTRY_VERIFY(window.findChild<QToolButton *>("profilesButton")->isEnabled());QCOMPARE(destination->text(),QString("/tmp/changed while waiting"));QVERIFY(autostartEnabled());QVERIFY(window.findChild<QCheckBox *>("dolphinShortcut")->isChecked());QVERIFY(window.findChild<QCheckBox *>("konquerorShortcut")->isChecked());QVERIFY(window.findChild<QCheckBox *>("keepHistory")->isChecked());QVERIFY(window.findChild<QCheckBox *>("resumeInterrupted")->isChecked());QCOMPARE(LanguageMenuDriver(window)->currentData().toString(),QString("en_US"));QCOMPARE(window.shortcutCalls,QList<bool>{true});
    }
    /// Un échec du raccourci ne change ni la langue, ni le démarrage, ni les options internes.
    void failedShortcutDoesNotApplyOtherProfileOptions() {
        ProfileSettingsGuard guard;QString failure;auto profile=sampleProfile();profile.gui["dolphin_ctrl_v"]=true;profile.gui["autostart"]=true;profile.gui["language"]="en_US";QVERIFY(TransferProfiles::store({profile},failure));ProfileWindow window;window.rejectShortcut=true;
        const auto before=window.currentTransferProfile();mutations.clear();QVERIFY(window.applyTransferProfile(profile.id));QTRY_VERIFY(window.findChild<QToolButton *>("profilesButton")->isEnabled());const auto after=window.currentTransferProfile();QCOMPARE(after.options,before.options);QCOMPARE(after.gui,before.gui);QCOMPARE(after.rate,before.rate);QVERIFY(mutations.isEmpty());QVERIFY(!autostartEnabled());
    }
    /// Un refus moteur restaure les changements externes déjà préparés.
    void engineFailureRollsBackProfileStartupAndShortcut() {
        ProfileSettingsGuard guard;QString failure;auto profile=sampleProfile();profile.gui["dolphin_ctrl_v"]=true;profile.gui["autostart"]=true;QVERIFY(TransferProfiles::store({profile},failure));ProfileWindow window;const auto before=window.currentTransferProfile();rejectExclusions=true;QVERIFY(window.applyTransferProfile(profile.id));QTRY_VERIFY(window.findChild<QToolButton *>("profilesButton")->isEnabled());rejectExclusions=false;
        QVERIFY(!autostartEnabled());QVERIFY(!window.findChild<QCheckBox *>("dolphinShortcut")->isChecked());QCOMPARE(window.currentTransferProfile().options,before.options);QCOMPARE(window.shortcutCalls,(QList<bool>{true,false}));
    }
    /// Le retour arrière restitue exactement une entrée de démarrage personnalisée préexistante.
    void profileFailureRestoresExactAutostartFile() {
        ProfileSettingsGuard guard;QVERIFY(QDir().mkpath(QFileInfo(autostartPath()).absolutePath()));const QByteArray original="[Desktop Entry]\nType=Application\nHidden=true\nName=Custom Gad\nExec=custom-command\n";
        {QFile file(autostartPath());QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(original),original.size());QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner));}
        QString failure;auto profile=sampleProfile();profile.gui["autostart"]=true;QVERIFY(TransferProfiles::store({profile},failure));ProfileWindow window;rejectExclusions=true;QVERIFY(!window.applyTransferProfile(profile.id));rejectExclusions=false;
        QFile file(autostartPath());QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),original);QVERIFY(!autostartEnabled());QVERIFY(!file.permissions().testFlag(QFile::ReadOther));
    }
    /// L’attente externe n’autorise ni une configuration concurrente, ni une fermeture qui couperait le retour arrière.
    void profileApplicationBlocksConcurrentConfigurationAndQuit() {
        ProfileSettingsGuard guard;QString failure;auto profile=sampleProfile();profile.gui["dolphin_ctrl_v"]=true;QVERIFY(TransferProfiles::store({profile},failure));ProfileWindow window;QVERIFY(window.applyTransferProfile(profile.id));
        QVERIFY(window.handle({{"command","configure"},{"verify_md5",false}}).contains("error"));QVERIFY(window.handle({{"command","shutdown"}}).contains("error"));QCloseEvent close;QApplication::sendEvent(&window,&close);QVERIFY(!close.isAccepted());
        QTRY_VERIFY(window.findChild<QToolButton *>("profilesButton")->isEnabled());QVERIFY(window.findChild<QCheckBox *>("verifyMd5")->isChecked());
    }
    /// Une langue retirée ne doit pas conduire à ignorer silencieusement une option sauvegardée.
    void unavailableProfileLanguageIsRejectedBeforeChanges() {
        ProfileSettingsGuard guard;QString failure;auto profile=sampleProfile();profile.gui["language"]="xx_XX";QVERIFY(TransferProfiles::store({profile},failure));ProfileWindow window;const auto before=window.currentTransferProfile();QVERIFY(!window.applyTransferProfile(profile.id));QCOMPARE(window.currentTransferProfile().options,before.options);QVERIFY(window.shortcutCalls.isEmpty());
    }

    /// Les commandes attendent le résultat asynchrone avant d’ajouter des fichiers, et les options explicites restent prioritaires.
    void profileCommandWaitsBeforeQueueingAndKeepsDestination() {
        ProfileSettingsGuard guard;QString failure;auto profile=sampleProfile("CLI profile");profile.gui["dolphin_ctrl_v"]=true;profile.options["verify_md5"]=false;QVERIFY(TransferProfiles::store({profile},failure));ProfileWindow window;
        QJsonObject reply;mutations.clear();window.handleAsync({{"command","enqueue"},{"profile","cli PROFILE"},{"sources",QJsonArray{"/tmp/source.txt"}},{"destination","/tmp/explicit target"},{"verify_md5",true}},[&](QJsonObject value){reply=value;});
        QVERIFY(reply.isEmpty());QVERIFY(mutations.isEmpty());QTRY_VERIFY(!reply.isEmpty());QVERIFY(!reply.contains("error"));QCOMPARE(mutations.last()["command"].toString(),QString("enqueue"));QCOMPARE(mutations.last()["destination"].toString(),QString("/tmp/explicit target"));QCOMPARE(mutations.last()["rate_limit"].toInteger(),profile.rate);QVERIFY(mutations.last()["verify_md5"].toBool());QVERIFY(!window.findChild<QCheckBox *>("verifyMd5")->isChecked());
    }
    /// Aucun transfert n’est ajouté si le nom est inconnu ou si l’application du profil échoue.
    void profileCommandFailuresNeverQueueTransfer() {
        ProfileSettingsGuard guard;QString failure;auto profile=sampleProfile("CLI profile");profile.gui["dolphin_ctrl_v"]=true;QVERIFY(TransferProfiles::store({profile},failure));ProfileWindow window;QJsonObject reply;mutations.clear();
        const QJsonObject request{{"command","enqueue"},{"profile","unknown"},{"sources",QJsonArray{"/tmp/source.txt"}},{"destination","/tmp/target"}};
        window.handleAsync(request,[&](QJsonObject result){reply=result;});QVERIFY(reply.contains("error"));QVERIFY(mutations.isEmpty());
        auto valid=request;valid["profile"]="CLI profile";window.rejectShortcut=true;reply={};window.handleAsync(valid,[&](QJsonObject result){reply=result;});QTRY_VERIFY(reply.contains("error"));QVERIFY(mutations.isEmpty());
    }
    /// Appliquer seul ne change pas la destination et ne nécessite aucun paramètre moteur supplémentaire.
    void standaloneProfileCommandDoesNotQueueOrChangeDestination() {
        ProfileSettingsGuard guard;QString failure;auto profile=sampleProfile("Standalone");QVERIFY(TransferProfiles::store({profile},failure));Window window;window.findChild<QLineEdit *>("destinationInput")->setText("sftp://host/untouched");QJsonObject reply;mutations.clear();
        window.handleAsync({{"command","configure"},{"profile","Standalone"}},[&](QJsonObject result){reply=result;});QVERIFY(reply["ok"].toBool());QCOMPARE(window.findChild<QLineEdit *>("destinationInput")->text(),QString("sftp://host/untouched"));QCOMPARE(mutations.size(),1);QCOMPARE(mutations[0]["command"].toString(),QString("configure"));QCOMPARE(SourceUrls::requiredProtocol({{"profile","Standalone"}}),26);
    }
    /// Le nom est extrait avant les drapeaux, y compris avec espaces, Unicode et forme --profile=.
    void profileCommandParsingValidatesNamesAndCombinations() {
        QString failure;auto request=parse({"gad","--profile","Mon profil"},failure);QVERIFY(failure.isEmpty());QCOMPARE(request["profile"].toString(),QString("Mon profil"));QCOMPARE(request["command"].toString(),QString("configure"));
        request=parse({"gad","--copy","/tmp/a","--to","/tmp/b","--profile=--md5"},failure);QVERIFY(failure.isEmpty());QCOMPARE(request["profile"].toString(),QString("--md5"));QCOMPARE(request["destination"].toString(),QString("/tmp/b"));
        request=CliOptions::configure({"gad","--configure","--profile","Backup"},failure);QVERIFY(failure.isEmpty());QCOMPARE(request["profile"].toString(),QString("Backup"));
        for(const auto &args:QList<QStringList>{{"gad","--profile"},{"gad","--profile="},{"gad","--profile","A","--profile","B"},{"gad","--status","--profile","A"}}){parse(args,failure);QVERIFY(!failure.isEmpty());}
    }

    /// Le profil enregistre la politique et la requête en capture la valeur avant la résolution des URL.
    void profileCapturesConflictPolicyAndMigratesOlderProfiles() {
        ProfileSettingsGuard guard;Window window;auto *policy=window.findChild<QComboBox *>("conflictPolicy");QVERIFY(policy);policy->setCurrentIndex(policy->findData("rename"));
        auto profile=window.currentTransferProfile();profile.name="Keep both";QString failure;QVERIFY(TransferProfiles::store({profile},failure));QCOMPARE(TransferProfiles::load(failure)[0].options["conflict_policy"].toString(),QString("rename"));
        policy->setCurrentIndex(policy->findData("skip"));QVERIFY(window.applyTransferProfile(profile.id));QCOMPARE(policy->currentData().toString(),QString("rename"));window.findChild<QLineEdit *>("destinationInput")->setText("/tmp/target");mutations.clear();window.enqueue({"/tmp/source.txt"});QCOMPARE(mutations.last()["conflict_policy"].toString(),QString("rename"));
        auto old=profile.json();auto options=old["options"].toObject();options.remove("conflict_policy");old["options"]=options;QSettings().setValue("transferProfiles",QJsonDocument(QJsonObject{{"version",2},{"profiles",QJsonArray{old}}}).toJson());
        const auto migrated=TransferProfiles::load(failure);QVERIFY(failure.isEmpty());QCOMPARE(migrated[0].options["conflict_policy"].toString(),QString("ask"));QCOMPARE(QJsonDocument::fromJson(QSettings().value("transferProfiles").toByteArray()).object()["version"].toInt(),6);
        auto invalid=CliOptions::configure({"gad","--configure","--conflict-policy","replace"},failure);QVERIFY(!failure.isEmpty());QVERIFY(invalid.isEmpty());
        const auto request=CliOptions::configure({"gad","--configure","--conflict-policy","newer"},failure);QVERIFY(failure.isEmpty());QCOMPARE(request["conflict_policy"].toString(),QString("newer"));QCOMPARE(SourceUrls::requiredProtocol(request),27);
        QVERIFY(window.handle({{"command","configure"},{"conflict_policy","invalid"},{"verify_md5",false}}).contains("error"));QCOMPARE(policy->currentData().toString(),QString("rename"));
    }

    /// Dates locales inclusives, Unicode et mots littéraux combinés ; les anciens journaux ne sont pas datés artificiellement.
    void historyQueryMatchesDatesStatesAndOriginalPaths() {
        QTreeWidgetItem row;const auto today=QDate::currentDate();row.setData(0,HistorySearch::CreatedRole,QDateTime(today,QTime(0,0)).toSecsSinceEpoch());row.setData(0,HistorySearch::JobStateRole,"error");row.setData(0,Qt::UserRole+3,"done");row.setData(0,HistorySearch::PathsRole,QString::fromUtf8("sftp://host/Photos/Été.jpg\n/home/user/Images/Été.jpg"));
        HistorySearch::Query query;query.enabled=true;query.text=QString::fromUtf8("été HOST");query.state="error";query.dates=true;query.from=today;query.to=today;QVERIFY(query.matches(&row));
        query.text="other";QVERIFY(!query.matches(&row));query.text.clear();query.state="done";QVERIFY(!query.matches(&row));query.state.clear();query.from=today.addDays(1);QVERIFY(!query.matches(&row));query.from=today;
        row.setData(0,HistorySearch::CreatedRole,0);QVERIFY(!query.matches(&row));query.dates=false;QVERIFY(query.matches(&row));row.setData(0,HistorySearch::JobStateRole,"running");QVERIFY(!query.matches(&row));query.historyOnly=false;QVERIFY(query.matches(&row));query.enabled=false;query.text="never";QVERIFY(query.matches(&row));
    }
    /// Les caches suivent les changements directs de requête, de chemins et les copies de critères.
    void historyQueryCachesInvalidateWithoutLosingUnicodeOrLiteralMatching() {
        QTreeWidgetItem row;row.setData(0,HistorySearch::JobStateRole,"done");
        HistorySearch::setPaths(&row,QString::fromUtf8("/Été/image.jpg"));
        HistorySearch::Query query;query.enabled=true;query.text=QString::fromUtf8("été \t image");
        QVERIFY(query.matches(&row));QVERIFY(query.matches(&row));
        auto copy=query;copy.text="other";QVERIFY(!copy.matches(&row));QVERIFY(query.matches(&row));
        HistorySearch::setPaths(&row,"/other/file.txt");QVERIFY(copy.matches(&row));QVERIFY(!query.matches(&row));
        row.setData(0,HistorySearch::PathsRole,QString::fromUtf8("/été/image.jpg"));QVERIFY(query.matches(&row));QVERIFY(!copy.matches(&row));
        HistorySearch::setPaths(&row,QString::fromUtf8("/été/image.jpg"));QCOMPARE(row.data(0,HistorySearch::CachedPathsRole).toString(),QString::fromUtf8("/été/image.jpg"));QVERIFY(query.matches(&row));
        query.text=" \n \t";QVERIFY(query.matches(&row));query.text=".*";QVERIFY(!query.matches(&row));
    }
    /// La barre conserve les parents des résultats et les mêmes items ; fermer la recherche restaure la vue.
    void historySearchCombinesFiltersWithoutChangingQueue() {
        auto entries=nestedEntries();for(int i=0;i<entries.size();++i){auto entry=entries[i].toObject();entry["state"]="done";entries[i]=entry;}
        auto value=job(entries);value["created_at"]=QDateTime(QDate::currentDate(),QTime(12,0)).toSecsSinceEpoch();publish(value);Window window;window.show();window.refresh();
        auto *button=window.findChild<QPushButton *>("historySearchButton");auto *text=window.findChild<QLineEdit *>("historySearchText");auto *view=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());auto *row=window.entryRow(1,2);auto *outside=window.entryRow(1,4);mutations.clear();button->click();text->setText("sub/a.txt");
        QTRY_VERIFY(outside->isHidden());QVERIFY(!row->isHidden());QVERIFY(!row->parent()->isHidden());QVERIFY(row->parent()->isExpanded());QCOMPARE(window.entryRow(1,2),row);QVERIFY(mutations.isEmpty());
        view->setColumnFilter(1,"no-match");QVERIFY(row->isHidden());view->clearHeaderFilters();QVERIFY(!row->isHidden());
        row->setSelected(true);text->setText("outside.txt");QTRY_VERIFY(row->isHidden());QVERIFY(!row->isSelected());
        window.findChild<QToolButton *>("historySearchClose")->click();QVERIFY(!row->isHidden());QVERIFY(!outside->isHidden());QVERIFY(mutations.isEmpty());
    }
    /// Une plage valide est maintenue dans les deux sens ; les entrées sans date sont exclues uniquement avec ce filtre.
    void historySearchDateControlsAndLiveLanguage() {
        publish(job({file("done")}));Window window;window.show();window.refresh();window.findChild<QPushButton *>("historySearchButton")->click();
        auto *row=window.entryRow(1,0);auto *dates=window.findChild<QCheckBox *>("historySearchDates");auto *from=window.findChild<QDateEdit *>("historySearchFrom");auto *to=window.findChild<QDateEdit *>("historySearchTo");QVERIFY(!row->isHidden());dates->setChecked(true);QVERIFY(row->isHidden());
        from->setDate(QDate::currentDate().addDays(2));QCOMPARE(to->date(),from->date());to->setDate(QDate::currentDate().addDays(-2));QCOMPARE(from->date(),to->date());
        window.findChild<QPushButton *>("historySearchReset")->click();QVERIFY(!row->isHidden());QVERIFY(!dates->isChecked());
        const auto restore=qScopeGuard([]{I18n::initialize("fr_FR");});I18n::initialize("en_US");QCOMPARE(window.findChild<QPushButton *>("historySearchButton")->text(),QString("Search history"));QVERIFY(row->toolTip(TableView::OrderColumn).contains("Unknown date"));
    }

    /// Une limite configurée ne suffit pas ; les observations réelles priment, et MD5/conflit/pause restent distincts.
    void diagnosticsDistinguishObservedLimitsFromIdleProgress() {
        TransferMetrics::Tracker meters;TransferDiagnostics::Tracker diagnostics;
        auto entry=file("running");entry["size"]=10000;entry["copied"]=0;auto request=job({entry},"running");request["rate_limit"]=2000;
        QJsonObject telemetry{{"queue_reason","active"},{"global_throttled",false},{"request_throttled",false}};
        QJsonObject state{{"jobs",QJsonArray{request}},{"rate_limit",1000},{"diagnostics",QJsonObject{{"1",telemetry}}}};
        meters.sync(state,{1},1000);diagnostics.sync(state,meters,1000);QVERIFY(diagnostics.items()[0].reason.contains("Aucune attente"));
        meters.sync(state,{},7000);diagnostics.sync(state,meters,7000);QVERIFY(diagnostics.items()[0].reason.contains("Aucune progression"));
        telemetry["global_throttled"]=true;state["diagnostics"]=QJsonObject{{"1",telemetry}};diagnostics.sync(state,meters,7100);QVERIFY(diagnostics.items()[0].reason.contains("budget de débit global"));QVERIFY(!diagnostics.items()[0].reason.contains("Aucune progression"));
        entry["state"]="verifying";request["entries"]=QJsonArray{entry};state["jobs"]=QJsonArray{request};meters.sync(state,{1},7200);diagnostics.sync(state,meters,7200);QVERIFY(diagnostics.items()[0].reason.contains("MD5"));
        entry["state"]="conflict";request["entries"]=QJsonArray{entry};state["jobs"]=QJsonArray{request};meters.sync(state,{1},7300);diagnostics.sync(state,meters,7300);QVERIFY(diagnostics.items()[0].reason.contains("décision"));
        state["paused"]=true;diagnostics.sync(state,meters,7400);QVERIFY(diagnostics.items()[0].reason.contains("pause"));
    }
    /// Les deltas fusionnent leurs compteurs sans perdre les chemins, reçus ni l’ordre et refusent une base absente.
    void sparseSnapshotsMergeCountersAndRatesAndRecoverFromInvalidBases() {
        QueueSnapshot snapshot;auto request=job({file("running"),file("waiting")},"running");
        request["entries"]=QJsonArray{file("running"),file("waiting")};
        const auto full=QJsonObject{{"revision",10},{"reset",true},{"jobs",QJsonArray{request}},{"job_ids",QJsonArray{1}},{"row_order",QJsonArray{1}}};
        snapshot.apply(full);
        const auto patch=QJsonObject{{"id",1},{"rate_limit",123},{"entries",QJsonArray{QJsonObject{{"index",0},{"copied",17},{"verified",8},{"resume_checked",3}}}}};
        const auto changes=snapshot.apply({{"revision",11},{"job_ids",QJsonArray{1}},{"jobs",QJsonArray{}},{"job_patches",QJsonArray{patch}}});
        QCOMPARE(changes,QSet<qint64>{1});auto updated=snapshot.state()["jobs"].toArray()[0].toObject();
        QCOMPARE(updated["rate_limit"].toInt(),123);auto entries=updated["entries"].toArray();auto expected=request["entries"].toArray()[0].toObject();expected["copied"]=17;expected["verified"]=8;expected["resume_checked"]=3;
        QCOMPARE(entries[0].toObject(),expected);QCOMPARE(entries[1],request["entries"].toArray()[1]);QCOMPARE(snapshot.state()["row_order"],full["row_order"]);
        const auto before=snapshot.state();auto invalid=patch;invalid["entries"]=QJsonArray{QJsonObject{{"index",99},{"copied",20}}};
        QVERIFY(snapshot.apply({{"revision",12},{"job_ids",QJsonArray{1}},{"job_patches",QJsonArray{invalid}}}).isEmpty());QCOMPARE(snapshot.revision(),-1);QCOMPARE(snapshot.state(),before);
        snapshot.apply(full);auto missing=patch;missing["id"]=99;snapshot.apply({{"revision",13},{"job_ids",QJsonArray{1}},{"job_patches",QJsonArray{missing}}});QCOMPARE(snapshot.revision(),-1);
        snapshot.apply(full);QCOMPARE(snapshot.revision(),10);QCOMPARE(snapshot.state()["jobs"].toArray()[0],QJsonValue(request));
    }
    /// Une réponse « inchangée » doit néanmoins rafraîchir la télémétrie, sans perdre les lots ni l’ordre.
    void unchangedSnapshotsStillRefreshWaitDiagnostics() {
        QueueSnapshot snapshot;const auto request=job({file("running")},"running");
        snapshot.apply({{"revision",10},{"reset",true},{"jobs",QJsonArray{request}},{"job_ids",QJsonArray{1}},{"row_order",QJsonArray{1}},{"diagnostics",QJsonObject{{"1",QJsonObject{{"global_throttled",false}}}}}});
        const auto changed=snapshot.apply({{"revision",10},{"unchanged",true},{"diagnostics",QJsonObject{{"1",QJsonObject{{"global_throttled",true}}}}}});QVERIFY(changed.isEmpty());QCOMPARE(snapshot.revision(),10);QCOMPARE(snapshot.state()["jobs"].toArray().size(),1);QVERIFY(snapshot.state()["diagnostics"].toObject()["1"].toObject()["global_throttled"].toBool());QCOMPARE(snapshot.state()["row_order"].toArray(),QJsonArray{1});
    }
    /// Une saisie en attente survit aux ticks ; une nouvelle consigne externe reste prioritaire.
    void progressRefreshPreservesUncommittedRateInput() {
        ProfileSettingsGuard guard;publish(job({file("running")},"running"));status["rate_limit"]=1048576;
        Window window;window.refresh();auto *rate=window.findChild<QDoubleSpinBox *>("globalRateLimit");
        auto *editor=rate->findChild<QLineEdit *>();QVERIFY(editor);const auto pending=QString("12")+rate->suffix();
        editor->setText(pending);mutations.clear();window.refresh();
        QCOMPARE(editor->text(),pending);QCOMPARE(rate->value(),1.0);QVERIFY(mutations.isEmpty());
        // Une nouvelle limite reçue d’une commande externe doit en revanche être affichée.
        status["rate_limit"]=2097152;window.refresh();QCOMPARE(rate->value(),2.0);
    }

    /// Les lignes du diagnostic sont stables, traduites à la volée et retirées après la fin d’un transfert.
    void diagnosticsDialogUpdatesWithoutMutatingTransfers() {
        publish(job({file("running")},"running"));status["diagnostics"]=QJsonObject{{"1",QJsonObject{{"queue_reason","active"},{"global_throttled",false}}}};Window window;mutations.clear();window.showDiagnostics();
        auto *dialog=window.findChild<QDialog *>("diagnosticsDialog");QVERIFY(dialog);QCOMPARE(dialog->windowModality(),Qt::WindowModal);auto *rows=dialog->findChild<QTreeWidget *>("diagnosticsTree");QCOMPARE(rows->topLevelItemCount(),1);auto *row=rows->topLevelItem(0);row->setSelected(true);
        status["diagnostics"]=QJsonObject{{"1",QJsonObject{{"queue_reason","active"},{"global_throttled",true}}}};window.refresh();QCOMPARE(rows->topLevelItem(0),row);QVERIFY(row->isSelected());QVERIFY(row->text(2).contains("budget de débit global"));QVERIFY(row->sizeHint(0).height()>24);QVERIFY(mutations.isEmpty());
        const auto restore=qScopeGuard([]{I18n::initialize("fr_FR");});I18n::initialize("en_US");QCOMPARE(dialog->windowTitle(),QString("Transfer diagnostics"));QVERIFY(row->text(2).contains("global speed budget"));
        auto request=job({file("done")});publish(request);window.refresh();QCOMPARE(rows->topLevelItemCount(),0);QVERIFY(dialog->findChild<QLabel *>("diagnosticsEmpty")->isVisible());QVERIFY(mutations.isEmpty());
    }

    /// Un renommage rapide en file ne doit pas être présenté comme un déplacement déjà en cours.
    void diagnosticsQueuedRenameReportsItsRealBlocker() {
        TransferMetrics::Tracker meters;TransferDiagnostics::Tracker diagnostics;auto request=job({file("waiting")},"waiting");request["fast_move"]=true;
        QJsonObject state{{"jobs",QJsonArray{request}},{"diagnostics",QJsonObject{{"1",QJsonObject{{"queue_reason","shared_resources"},{"blockers",QJsonArray{2}}}}}}};
        meters.sync(state,{1},1000);diagnostics.sync(state,meters,1000);QVERIFY(diagnostics.items()[0].reason.contains("même disque"));QVERIFY(diagnostics.items()[0].reason.contains("#2"));QVERIFY(!diagnostics.items()[0].reason.contains("renommage"));
    }

    /// L’option est persistante, capturée dans les demandes et conservée par les profils ; les anciens profils gardent le défaut désactivé.
    void fastMoveVerificationOptionProfilesAndCommandLine() {
        ProfileSettingsGuard guard;Window window;auto *check=window.findChild<QCheckBox *>("verifyFastMove");QVERIFY(check);check->setChecked(true);QVERIFY(QSettings().value("verifyFastMove").toBool());auto profile=window.currentTransferProfile();profile.name="Checked renames";QString failure;QVERIFY(TransferProfiles::store({profile},failure));QVERIFY(TransferProfiles::load(failure)[0].options["verify_fast_move"].toBool());
        check->setChecked(false);QVERIFY(window.applyTransferProfile(profile.id));QVERIFY(check->isChecked());window.findChild<QLineEdit *>("destinationInput")->setText("/tmp/target");mutations.clear();window.enqueue({"/tmp/source.txt"});QVERIFY(mutations.last()["verify_fast_move"].toBool());QCOMPARE(SourceUrls::requiredProtocol(mutations.last()),33);
        mutations.clear();QVERIFY(window.handle({{"command","configure"},{"verify_fast_move","yes"},{"verify_md5",false}}).contains("error"));QVERIFY(mutations.isEmpty());QVERIFY(check->isChecked());
        const auto request=CliOptions::configure({"gad","--configure","--no-verify-fast-move"},failure);QVERIFY(failure.isEmpty());QVERIFY(request["verify_fast_move"].isBool());QVERIFY(!request["verify_fast_move"].toBool());QVERIFY(!window.handle(request).contains("error"));QVERIFY(!check->isChecked());
        auto old=profile.json();auto options=old["options"].toObject();options.remove("verify_fast_move");old["options"]=options;QSettings().setValue("transferProfiles",QJsonDocument(QJsonObject{{"version",3},{"profiles",QJsonArray{old}}}).toJson());const auto migrated=TransferProfiles::load(failure);QVERIFY(failure.isEmpty());QVERIFY(!migrated[0].options["verify_fast_move"].toBool());QCOMPARE(QJsonDocument::fromJson(QSettings().value("transferProfiles").toByteArray()).object()["version"].toInt(),6);
    }
    /// La vérification des métadonnées après renommage ne doit pas être présentée comme une relecture MD5.
    void fastMoveVerificationHasDistinctRowsMetricsAndDiagnostics() {
        auto entry=file("running");entry["fast_move_check"]=QJsonObject{{"status","checking"}};entry["undo"]=QJsonObject{{"rename_move",true}};auto request=job({entry},"running");request["fast_move"]=true;publish(request);Window window;window.refresh();QVERIFY(window.entryRow(1,0)->text(4).contains("Vérification du déplacement rapide"));
        const auto probe=TransferMetrics::Probe::from(request);QCOMPARE(probe.phase,QString("fast_verify"));TransferMetrics::Tracker meters;TransferDiagnostics::Tracker diagnostics;QJsonObject state{{"jobs",QJsonArray{request}}};meters.sync(state,{1},1000);diagnostics.sync(state,meters,1000);QVERIFY(diagnostics.items()[0].reason.contains("pas relu"));QVERIFY(!diagnostics.items()[0].metrics.contains("MD5"));
        entry["state"]="done";entry["fast_move_check"]=QJsonObject{{"status","ok"}};publish(job({entry}));window.refresh();QVERIFY(window.entryRow(1,0)->text(4).contains("Déplacement rapide vérifié"));
        entry["state"]="error";entry["error"]="Fast-move verification failed; moved result kept.";entry["fast_move_check"]=QJsonObject{{"status","error"},{"path","/tmp/<result>"}};publish(job({entry},"error"));window.refresh();QVERIFY(window.entryRow(1,0)->text(4).contains("conservé"));QVERIFY(window.entryRow(1,0)->text(4).contains("/tmp/<result>"));QTextDocument tooltip;tooltip.setHtml(window.entryRow(1,0)->toolTip(4));QVERIFY(tooltip.toPlainText().contains("/tmp/<result>"));
    }

    /// Une file vide ou un ancien historique ne déclenche rien ; une nouvelle opération très rapide est néanmoins prise en compte.
    void completionPlanWaitsForActualWorkAndFiresOnce() {
        Completion::Plan plan;auto previous=job({file("done")});QJsonObject state{{"jobs",QJsonArray{previous}},{"queue_guard",QJsonObject{{"idle",true},{"epoch",1},{"next_id",1},{"last_command","enqueue"}}}};
        plan.arm("poweroff",5,state);QCOMPARE(plan.observe(state,false,10000),Completion::Plan::None);auto next=previous;next["id"]=2;state["jobs"]=QJsonArray{previous,next};state["queue_guard"]=QJsonObject{{"idle",true},{"epoch",2},{"next_id",2},{"last_command","enqueue"}};
        QCOMPARE(plan.observe(state,false,11000),Completion::Plan::Begin);QCOMPARE(plan.remaining(11000),5);QCOMPARE(plan.observe(state,false,15999),Completion::Plan::None);QCOMPARE(plan.observe(state,false,16000),Completion::Plan::Ready);QCOMPARE(plan.observe(state,false,20000),Completion::Plan::None);
    }
    /// Une demande ajoutée pendant le compte à rebours, même déjà terminée au prochain tick, redonne le délai entier.
    void completionPlanRestartsForNewWorkAndWaitsForPreparation() {
        Completion::Plan plan;auto first=job({file("running")},"running");QJsonObject state{{"jobs",QJsonArray{first}},{"queue_guard",QJsonObject{{"idle",false},{"epoch",1},{"next_id",1},{"last_command","enqueue"}}}};plan.arm("suspend",5,state);
        first["state"]="done";state["jobs"]=QJsonArray{first};state["queue_guard"]=QJsonObject{{"idle",true},{"epoch",1},{"next_id",1},{"last_command","enqueue"}};QCOMPARE(plan.observe(state,false,1000),Completion::Plan::Begin);
        auto next=job({file("done")});next["id"]=2;state["jobs"]=QJsonArray{first,next};state["queue_guard"]=QJsonObject{{"idle",true},{"epoch",2},{"next_id",2},{"last_command","enqueue"}};QCOMPARE(plan.observe(state,false,5999),Completion::Plan::Begin);QCOMPARE(plan.remaining(5999),5);
        QCOMPARE(plan.observe(state,true,7000),Completion::Plan::None);QVERIFY(!plan.counting);QCOMPARE(plan.observe(state,false,8000),Completion::Plan::Begin);QCOMPARE(plan.observe(state,false,13000),Completion::Plan::Ready);
    }
    /// Démarrer un lot préparé ou mettre une sélection en pause ne désarme pas sa fin de lot.
    void completionPlanKeepsActionAcrossStartAndSelectionPause() {
        for(const auto &command:QStringList{"start_prepared","pause_selection"}) {
            auto request=job({file("waiting")},"waiting");
            request["start_required"]=command=="start_prepared";
            QJsonObject state{{"jobs",QJsonArray{request}},{"queue_guard",QJsonObject{{"idle",false},{"epoch",1},{"next_id",1},{"last_command","enqueue"}}}};
            Completion::Plan plan;plan.arm("notify",5,state);
            request["start_required"]=false;request["paused"]=command=="pause_selection";
            state["jobs"]=QJsonArray{request};state["queue_guard"]=QJsonObject{{"idle",false},{"epoch",2},{"next_id",1},{"last_command",command}};
            QCOMPARE(plan.observe(state,false,1000),Completion::Plan::None);QVERIFY(plan.armed);
            request["state"]="done";request["entries"]=QJsonArray{file("done")};request["paused"]=false;state["jobs"]=QJsonArray{request};
            auto guard=state["queue_guard"].toObject();guard["idle"]=true;state["queue_guard"]=guard;
            QCOMPARE(plan.observe(state,false,2000),Completion::Plan::Begin);
            QCOMPARE(plan.observe(state,false,7000),Completion::Plan::Ready);
        }
    }
    /// Erreurs, annulations, suppressions et moteur ancien rendent l’action inéligible, sans appel système.
    void completionPlanRejectsErrorsCanceledAndMissingRequests() {
        for(const auto &result:QStringList{"error","cancelled","undone","missing"}) {
            Completion::Plan plan;auto request=job({file("running")},"running");QJsonObject state{{"jobs",QJsonArray{request}},{"queue_guard",QJsonObject{{"idle",false},{"epoch",1},{"next_id",1},{"last_command","enqueue"}}}};plan.arm("poweroff",5,state);
            request["state"]=result;state["jobs"]=result=="missing"?QJsonArray{}:QJsonArray{request};state["queue_guard"]=QJsonObject{{"idle",true},{"epoch",1},{"next_id",1},{"last_command","enqueue"}};QCOMPARE(plan.observe(state,false,10000),Completion::Plan::Abort);QVERIFY(!plan.armed);
        }
        Completion::Plan unsupported;unsupported.arm("quit",5,{});QCOMPARE(unsupported.observe({},false,10000),Completion::Plan::Abort);
    }
    /// Tous les effets sont injectés : la sortie, la veille et l’extinction ne sont jamais réellement exécutées en test.
    void completionWindowDispatchesOnlyAfterCountdownAndOnlyOnce() {
        for(const auto &action:QStringList{"notify","clear","quit","suspend","poweroff"}) {
            ProfileSettingsGuard guard;auto request=job({file("running")},"running");publish(request);status["queue_guard"]=QJsonObject{{"idle",false},{"epoch",1},{"next_id",1},{"last_command","enqueue"}};CompletionWindow window;
            window.handle({{"command","configure"},{"gui_settings",QJsonObject{{"completion_action",action},{"completion_delay",5}}}});window.armCompletion();QVERIFY(window.actions.isEmpty());request["state"]="done";publish(request);status["queue_guard"]=QJsonObject{{"idle",true},{"epoch",1},{"next_id",1},{"last_command","enqueue"}};
            window.refreshCompletion(status,1000);QVERIFY(!window.findChild<QDialog *>("completionCountdown"));QVERIFY(!window.findChild<TransferNotifications *>("completionNotification")->properties().isEmpty());QVERIFY(window.actions.isEmpty());window.refreshCompletion(status,5999);QVERIFY(window.actions.isEmpty());mutations.clear();window.refreshCompletion(status,6000);QCOMPARE(window.actions,QStringList{action});QCOMPARE(mutations.first()["command"].toString(),QString("completion_claim"));QCOMPARE(mutations.last()["command"].toString(),QString("completion_release"));window.refreshCompletion(status,10000);QCOMPARE(window.actions.size(),1);
        }
    }
    /// Le bouton Annuler désarme sans annuler les transferts ; recharger un profil ne peut jamais réarmer.
    void completionCancelAndProfilesNeverArmPowerActions() {
        ProfileSettingsGuard guard;auto request=job({file("running")},"running");publish(request);status["queue_guard"]=QJsonObject{{"idle",false},{"epoch",1},{"next_id",1},{"last_command","enqueue"}};CompletionWindow window;
        window.handle({{"command","configure"},{"gui_settings",QJsonObject{{"completion_action","poweroff"},{"completion_delay",5}}}});auto profile=window.currentTransferProfile();profile.name="Power preset";QString failure;QVERIFY(TransferProfiles::store({profile},failure));window.armCompletion();QVERIFY(window.applyTransferProfile(profile.id));
        request["state"]="done";publish(request);status["queue_guard"]=QJsonObject{{"idle",true},{"epoch",1},{"next_id",1},{"last_command","enqueue"}};window.refreshCompletion(status,10000);QVERIFY(window.actions.isEmpty());QVERIFY(!window.findChild<QDialog *>("completionCountdown"));
        request["state"]="running";publish(request);status["queue_guard"]=QJsonObject{{"idle",false},{"epoch",2},{"next_id",1},{"last_command","retry_errors"}};window.armCompletion();request["state"]="done";publish(request);status["queue_guard"]=QJsonObject{{"idle",true},{"epoch",2},{"next_id",1},{"last_command","retry_errors"}};window.refreshCompletion(status,1000);window.cancelCompletion();QVERIFY(window.findChild<TransferNotifications *>("completionNotification")->properties().isEmpty());window.refreshCompletion(status,20000);QVERIFY(window.actions.isEmpty());
        auto old=profile.json();auto gui=old["gui"].toObject();gui.remove("completion_action");gui.remove("completion_delay");old["gui"]=gui;QSettings().setValue("transferProfiles",QJsonDocument(QJsonObject{{"version",4},{"profiles",QJsonArray{old}}}).toJson());const auto migrated=TransferProfiles::load(failure);QVERIFY(failure.isEmpty());QCOMPARE(migrated[0].gui["completion_action"].toString(),QString("none"));QCOMPARE(migrated[0].gui["completion_delay"].toInt(),30);
    }
    /// L’activation explicite dans les options reste distincte des préférences sauvegardées.
    void completionOptionsShowAndToggleActivation() {
        ProfileSettingsGuard guard;publish(job({file("running")},"running"));status["queue_guard"]=QJsonObject{{"idle",false},{"epoch",1},{"next_id",1},{"last_command","enqueue"}};CompletionWindow window;
        auto *button=window.findChild<QPushButton *>("completionActivate");auto *label=window.findChild<QLabel *>("completionStatus");QVERIFY(button);QVERIFY(label);
        window.handle({{"command","configure"},{"gui_settings",QJsonObject{{"completion_action","none"}}}});QVERIFY(!button->isEnabled());QVERIFY(label->text().contains("Inactif"));
        window.handle({{"command","configure"},{"gui_settings",QJsonObject{{"completion_action","suspend"},{"completion_delay",45}}}});QVERIFY(button->isEnabled());QVERIFY(label->text().contains("Inactif"));
        button->click();QVERIFY(label->text().contains("Actif"));QVERIFY(label->text().contains("45"));QVERIFY(window.actions.isEmpty());
        button->click();QVERIFY(label->text().contains("Inactif"));QCOMPARE(window.findChild<QComboBox *>("completionAction")->currentData().toString(),QString("suspend"));
        button->click();window.handle({{"command","configure"},{"gui_settings",QJsonObject{{"completion_delay",60}}}});QVERIFY(label->text().contains("Inactif"));QVERIFY(window.actions.isEmpty());
    }

    /// L’action réelle conserve la garde jusqu’au nettoyage puis ne se déclenche plus.
    void completionClearsListAfterCountdownAndKeepsSavedPreference() {
        ProfileSettingsGuard guard;auto request=job({file("running")},"running");publish(request);
        status["queue_guard"]=QJsonObject{{"idle",false},{"epoch",1},{"next_id",1},{"last_command","enqueue"}};
        clearHistoryInMock=true;Window window;
        auto *menu=window.findChild<QToolButton *>("completionButton")->menu();
        QVERIFY(QMetaObject::invokeMethod(menu,"aboutToShow",Qt::DirectConnection));
        auto *clear=menu->findChild<QAction *>("completionChoose_clear");QVERIFY(clear);QCOMPARE(clear->text(),QString("Nettoyer la liste"));
        window.findChild<QSpinBox *>("completionDelay")->setValue(5);clear->trigger();
        QCOMPARE(QSettings().value("completionAction").toString(),QString("clear"));
        QCOMPARE(window.currentTransferProfile().gui["completion_action"].toString(),QString("clear"));
        QString failure;const auto cli=CliOptions::configure({"gad","--configure","--completion-action","clear"},failure);QVERIFY(failure.isEmpty());QCOMPARE(SourceUrls::requiredProtocol(cli),35);
        request["state"]="done";request["entries"]=QJsonArray{file("done")};publish(request);
        status["queue_guard"]=QJsonObject{{"idle",true},{"epoch",1},{"next_id",1},{"last_command","enqueue"}};
        mutations.clear();window.refreshCompletion(status,1000);QVERIFY(!window.findChild<QDialog *>("completionCountdown"));QVERIFY(!window.findChild<TransferNotifications *>("completionNotification")->properties().isEmpty());QVERIFY(mutations.isEmpty());
        window.refreshCompletion(status,6000);
        QCOMPARE(mutations.size(),3);QCOMPARE(mutations[0]["command"].toString(),QString("completion_claim"));QCOMPARE(mutations[1]["command"].toString(),QString("completion_clear"));QCOMPARE(mutations[2]["command"].toString(),QString("completion_release"));
        QVERIFY(status["jobs"].toArray().isEmpty());QCOMPARE(window.findChild<QTreeWidget *>()->topLevelItemCount(),0);
        mutations.clear();window.refreshCompletion(status,20000);QVERIFY(mutations.isEmpty());
    }

    /// Les anciens profils conservent le comportement visible ; un choix récent reste durable.
    void progressMessageProfilesMigrateAndValidate() {
        ProfileSettingsGuard guard;QString failure;auto profile=sampleProfile();auto old=profile.json();auto gui=old["gui"].toObject();gui.remove("progressMessage");old["gui"]=gui;
        QSettings().setValue("transferProfiles",QJsonDocument(QJsonObject{{"version",5},{"profiles",QJsonArray{old}}}).toJson());
        auto migrated=TransferProfiles::load(failure);QVERIFY(failure.isEmpty());QCOMPARE(migrated.size(),1);QVERIFY(migrated[0].gui["progressMessage"].toBool());QCOMPARE(QJsonDocument::fromJson(QSettings().value("transferProfiles").toByteArray()).object()["version"].toInt(),6);
        migrated[0].gui["progressMessage"]=false;QVERIFY(TransferProfiles::store(migrated,failure));QVERIFY(!TransferProfiles::load(failure)[0].gui["progressMessage"].toBool());
        const auto saved=QSettings().value("transferProfiles");migrated[0].gui["progressMessage"]="false";QVERIFY(!TransferProfiles::store(migrated,failure));QCOMPARE(QSettings().value("transferProfiles"),saved);
        auto request=CliOptions::configure({"gad","--configure","--no-progress-message"},failure);QVERIFY(failure.isEmpty());QCOMPARE(SourceUrls::requiredProtocol(request),34);
    }

    /// Les paramètres CLI changent uniquement les préférences ; valeurs invalides et fractions sont refusées.
    void completionSettingsValidateAndRemainUnarmed() {
        ProfileSettingsGuard guard;CompletionWindow window;QString failure;auto request=CliOptions::configure({"gad","--configure","--completion-action","suspend","--completion-delay","45"},failure);QVERIFY(failure.isEmpty());QVERIFY(!window.handle(request).contains("error"));QCOMPARE(window.findChild<QComboBox *>("completionAction")->currentData().toString(),QString("suspend"));QCOMPARE(window.findChild<QSpinBox *>("completionDelay")->value(),45);QCOMPARE(SourceUrls::requiredProtocol(request),31);QVERIFY(window.actions.isEmpty());
        for(const auto &args:QList<QStringList>{{"gad","--configure","--completion-action","invalid"},{"gad","--configure","--completion-delay","0"},{"gad","--configure","--completion-delay","2.5"}}){CliOptions::configure(args,failure);QVERIFY(!failure.isEmpty());}
        mutations.clear();QVERIFY(window.handle({{"command","configure"},{"gui_settings",QJsonObject{{"completion_delay",4},{"autostart",true}}}}).contains("error"));QVERIFY(mutations.isEmpty());QCOMPARE(window.findChild<QSpinBox *>("completionDelay")->value(),45);
    }

    /// Pendant l’envoi, les chemins asynchrones et les profils ne peuvent pas démarrer de préparation ni modifier les raccourcis.
    void completionDispatchBlocksAsyncRequestsAndProfiles() {
        ProfileSettingsGuard guard;auto request=job({file("running")},"running");publish(request);status["queue_guard"]=QJsonObject{{"idle",false},{"epoch",1},{"next_id",1},{"last_command","enqueue"}};DelayedCompletionWindow window;window.handle({{"command","configure"},{"gui_settings",QJsonObject{{"completion_action","suspend"},{"completion_delay",5}}}});window.armCompletion();
        request["state"]="done";request["entries"]=QJsonArray{file("done")};publish(request);status["queue_guard"]=QJsonObject{{"idle",true},{"epoch",1},{"next_id",1},{"last_command","enqueue"}};window.refreshCompletion(status,1000);window.refreshCompletion(status,6000);QVERIFY(window.pending);QVERIFY(!window.findChild<QWidget *>("settingsPanel")->isEnabled());QVERIFY(!window.findChild<QWidget *>("ratesPanel")->isEnabled());QVERIFY(!window.findChild<QWidget *>("completionPanel")->isEnabled());
        QJsonObject reply;window.handleAsync({{"command","configure"},{"gui_settings",QJsonObject{{"dolphin_ctrl_v",true}}}},[&](QJsonObject result){reply=result;});QVERIFY(reply.contains("error"));QCOMPARE(window.shortcutCalls,0);QVERIFY(window.handle({{"command","enqueue"},{"sources",QJsonArray{"sftp://host/a"}},{"destination","/tmp"}}).contains("error"));
        QJsonObject profileReply;QVERIFY(!window.applyTransferProfile("unknown",[&](QJsonObject result){profileReply=result;}));QVERIFY(profileReply.contains("error"));window.pending({{"ok",true}});QVERIFY(window.findChild<QWidget *>("settingsPanel")->isEnabled());QVERIFY(window.findChild<QWidget *>("ratesPanel")->isEnabled());QVERIFY(window.findChild<QWidget *>("completionPanel")->isEnabled());
    }

    /// Les lectures IPC n’actualisent pas la fenêtre ; un collage externe ne paie pas la reconstruction des lignes.
    void readOnlyRequestsAndBackgroundAddsAvoidSynchronousRefresh() {
        publish(job({file("running")},"running"));Window window;
        auto *row=window.entryRow(1,0);QVERIFY(row);const auto previous=row->text(2);
        publish(job({file("done")},"done"));
        const auto result=window.handle({{"command","status"}});QCOMPARE(result["jobs"].toArray()[0].toObject()["state"].toString(),QString("done"));
        QCOMPARE(row->text(2),previous);
        auto *timer=window.findChild<QTimer *>("queueRefreshTimer");QVERIFY(timer);
        QVERIFY(!window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/paste-fixture"}},{"destination","/tmp"},{"background",true}}).contains("error"));
        QCOMPARE(row->text(2),previous);QVERIFY(timer->isActive());QCOMPARE(timer->interval(),25);
        QVERIFY(QMetaObject::invokeMethod(timer,"timeout",Qt::DirectConnection));
        QCOMPARE(timer->interval(),400);QCOMPARE(row->text(2),QString(GAD_TR("OK")));
        auto active=job({file("running")},"running");active["fast_move"]=true;publish(active);
        QVERIFY(QMetaObject::invokeMethod(timer,"timeout",Qt::DirectConnection));QCOMPARE(timer->interval(),50);
        status["paused"]=true;QVERIFY(QMetaObject::invokeMethod(timer,"timeout",Qt::DirectConnection));QCOMPARE(timer->interval(),400);
    }

    /// La fréquence de rafraîchissement ne doit jamais entraîner des écritures de configuration au repos.
    void unchangedRefreshDoesNotRewriteRateSettings() {
        Window window;status["rate_limit"]=0;QSettings settings;settings.setValue("rateLimit",qlonglong(0));settings.sync();
        const auto old=QDateTime::fromSecsSinceEpoch(1000000000);
        QFile file(settings.fileName());QVERIFY(file.open(QIODevice::ReadWrite));QVERIFY(file.setFileTime(old,QFileDevice::FileModificationTime));file.close();
        window.refresh();window.refresh();settings.sync();
        QCOMPARE(QFileInfo(settings.fileName()).lastModified().toMSecsSinceEpoch(),old.toMSecsSinceEpoch());
    }

    void initTestCase() {
        qApp->setOrganizationName("GadTests");qApp->setApplicationName("RowActions");
        applicationIcon();I18n::initialize("fr_FR");
        QDesktopServices::setUrlHandler("file",&desktop,"capture");
    }
    void cleanupTestCase(){QDesktopServices::unsetUrlHandler("file");}
    void init(){clearHistoryInMock=false;rejectExclusions=false;errorReport={{"records",QJsonArray{}},{"jobs",QJsonArray{}},{"generated_unix",0}};status={{"jobs",QJsonArray{}},{"paused",false}};mutations.clear();desktop.urls.clear();}

    /// L'historique n'avance pas la barre globale ; les nouveaux octets la font progresser.
    void sessionProgressStartsAtZeroAndTracksNewWork() {
        publish(job({file()}));Window window;auto *bar=window.findChild<QProgressBar *>();QVERIFY(bar);QCOMPARE(bar->value(),0);
        auto next=file("waiting");next["copied"]=0;auto batch=job({next},"waiting");batch["id"]=2;
        status["jobs"]=QJsonArray{job({file()}),batch};window.refresh();QCOMPARE(bar->value(),0);
        next["state"]="running";next["copied"]=1;batch["state"]="running";batch["entries"]=QJsonArray{next};status["jobs"]=QJsonArray{job({file()}),batch};window.refresh();QCOMPARE(bar->value(),250);
        next["state"]="done";next["copied"]=4;batch["state"]="done";batch["entries"]=QJsonArray{next};status["jobs"]=QJsonArray{job({file()}),batch};window.refresh();QCOMPARE(bar->value(),1000);
    }
    void restoredPartialProgressStartsAtZeroAndResetsOnRestart() {
        auto entry=file("waiting");entry["copied"]=2;publish(job({entry},"waiting"));Window window;auto *bar=window.findChild<QProgressBar *>();QCOMPARE(bar->value(),0);
        entry["state"]="running";entry["copied"]=3;publish(job({entry},"running"));window.refresh();QCOMPARE(bar->value(),500);
        status["paused"]=true;window.refresh();QCOMPARE(bar->value(),500);
        entry["copied"]=0;publish(job({entry},"running"));window.refresh();QCOMPARE(bar->value(),0);
        entry["copied"]=1;publish(job({entry},"running"));window.refresh();QCOMPARE(bar->value(),250);
    }
    /// Le menu ne contient que le drapeau dans la barre et synchronise la combobox conservée.
    void languageFlagMenuIsCompactAndReplacesTheOptionsSelector() {
        ProfileSettingsGuard guard;QSettings().setValue("language","fr_FR");I18n::initialize("fr_FR");Window window;window.show();QTest::qWait(20);
        auto *button=window.findChild<QToolButton *>("languageMenuButton");auto *profiles=window.findChild<QToolButton *>("profilesButton");LanguageMenuDriver language(window);
        QVERIFY(button);QVERIFY(profiles);QVERIFY(language);QVERIFY(!window.findChild<QComboBox *>("languageSelector"));
        QCOMPARE(button->height(),profiles->height());QCOMPARE(button->width(),button->height());QVERIFY(button->iconSize().height()<24);
        QCOMPARE(button->toolButtonStyle(),Qt::ToolButtonIconOnly);QVERIFY(button->text().isEmpty());QVERIFY(!button->icon().isNull());QVERIFY(button->menu());
        QCOMPARE(button->parentWidget()->layout()->indexOf(button),profiles->parentWidget()->layout()->indexOf(profiles)+1);
        const auto french=button->icon().pixmap(24,24).toImage();language->setCurrentIndex(language->findData("en_US"));
        QCOMPARE(language->currentData().toString(),QString("en_US"));QCOMPARE(QSettings().value("language").toString(),QString("en_US"));QVERIFY(button->icon().pixmap(24,24).toImage()!=french);
        language->setCurrentIndex(language->findData("de_DE"));
        for(auto *action:button->menu()->actions())QCOMPARE(action->isChecked(),action->data().toString()=="de_DE");
    }
    /// Les éditions installées suivent la langue à chaud et les liens ouvrent de vrais fichiers locaux.
    void aboutDocumentationLinksFollowLanguageAndOpenFiles() {
        QTemporaryDir temporary;QVERIFY(temporary.isValid());const auto previousData=qgetenv("XDG_DATA_HOME"),previousDirs=qgetenv("XDG_DATA_DIRS");const auto previousLanguage=QSettings().value("language");
        const auto restore=qScopeGuard([=]{qputenv("XDG_DATA_HOME",previousData);qputenv("XDG_DATA_DIRS",previousDirs);if(previousLanguage.isValid())QSettings().setValue("language",previousLanguage);else QSettings().remove("language");I18n::initialize("fr_FR");});
        qputenv("XDG_DATA_HOME",QFile::encodeName(temporary.path()));qputenv("XDG_DATA_DIRS",QFile::encodeName(temporary.path()+"/empty"));
        const auto root=temporary.path()+"/gad/docs";QVERIFY(QDir().mkpath(root+"/html"));QJsonArray records;
        const QStringList locales{"fr_FR","en_US","es_ES","pt_PT","de_DE","ja_JP"};
        const QStringList kinds{"quick","user","developer"};
        for(const auto &locale:locales)for(const auto &kind:kinds) {
            const auto name=kind+" guide & "+locale;
            for(const auto &format:QStringList{"html","md"}){QFile file(root+(format=="html"?"/html/":"/")+name+"."+format);QVERIFY(file.open(QIODevice::WriteOnly));file.write("fixture");}
            records.append(QJsonObject{{"kind",kind},{"locale",locale},{"title",kind+" & "+locale},{"html","html/"+name+".html"},{"md",name+".md"}});
        }
        QFile catalog(root+"/catalog.json");QVERIFY(catalog.open(QIODevice::WriteOnly));catalog.write(QJsonDocument(QJsonObject{{"format","gad-documentation"},{"version",1},{"guides",records}}).toJson());catalog.close();
        const auto variant=Documentation::links(QLocale("pt_BR"));QCOMPARE(variant.size(),3);QVERIFY(variant[0].title.endsWith("pt_PT"));
        I18n::initialize("fr_FR");Window window;window.showAbout();auto *dialog=window.findChild<QMessageBox *>("aboutDialog");QVERIFY(dialog);
        LanguageMenuDriver language(window);
        for(const auto &locale:locales) {
            language->setCurrentIndex(language->findData(locale));QCoreApplication::processEvents();
            QCOMPARE(window.findChild<QMessageBox *>("aboutDialog"),dialog);const auto links=Documentation::links();QCOMPARE(links.size(),3);
            for(const auto &link:links){QVERIFY(link.title.endsWith(locale));QVERIFY(QFileInfo(link.html.toLocalFile()).exists());QVERIFY(dialog->informativeText().contains(link.html.toString(QUrl::FullyEncoded).toHtmlEscaped()));}
            QCOMPARE(dialog->informativeText().count("<a href="),6);
            const QMap<QString,QString> closeTexts{{"fr_FR","Fermer"},{"en_US","Close"},{"es_ES","Cerrar"},{"pt_PT","Fechar"},{"de_DE",QString::fromUtf8("Schließen")},{"ja_JP",QString::fromUtf8("閉じる")}};
            QCOMPARE(dialog->findChild<QPushButton *>("aboutCloseButton")->text(),closeTexts.value(locale));
        }
        QLabel *linkLabel=nullptr;for(auto *label:dialog->findChildren<QLabel *>())if(label->text().contains("file:")){linkLabel=label;break;}QVERIFY(linkLabel);
        const auto selected=Documentation::links()[0].html;desktop.urls.clear();QVERIFY(QMetaObject::invokeMethod(linkLabel,"linkActivated",Qt::DirectConnection,Q_ARG(QString,selected.toString(QUrl::FullyEncoded))));QCOMPARE(desktop.urls,QList<QUrl>{selected});
        const auto markdown=Documentation::links()[0].markdown;QVERIFY(QFile::remove(selected.toLocalFile()));auto missingHtml=Documentation::links();QVERIFY(missingHtml[0].html.isEmpty());QCOMPARE(missingHtml[0].markdown,markdown);
        QVERIFY(QFile::remove(markdown.toLocalFile()));const auto fallback=Documentation::links();QCOMPARE(fallback.size(),3);QVERIFY(fallback[0].title.endsWith("en_US"));
        const QPointer<QMessageBox> aboutGuard(dialog);QTest::mouseClick(dialog->findChild<QPushButton *>("aboutCloseButton"),Qt::LeftButton);QVERIFY(!aboutGuard || !aboutGuard->isVisible());
    }

    /// Une variante de langue est résolue et aucun chemin hors du dossier de documentation n’est accepté.
    void documentationRejectsMissingAndEscapingPaths() {
        QTemporaryDir temporary;QVERIFY(temporary.isValid());const auto root=temporary.path()+"/docs";QVERIFY(QDir().mkpath(root+"/html"));
        QFile outside(temporary.path()+"/outside.html");QVERIFY(outside.open(QIODevice::WriteOnly));outside.write("outside");outside.close();
        QFile inside(root+"/html/guide.html");QVERIFY(inside.open(QIODevice::WriteOnly));inside.write("inside");inside.close();
        QVERIFY(!Documentation::localFile(root,"html/guide.html","html").isEmpty());
        QVERIFY(Documentation::localFile(root,"../outside.html","html").isEmpty());QVERIFY(Documentation::localFile(root,outside.fileName(),"html").isEmpty());
        QVERIFY(Documentation::localFile(root,"html/missing.html","html").isEmpty());QVERIFY(Documentation::localFile(root,"html/guide.html","md").isEmpty());
        QVERIFY(QFile::link(outside.fileName(),root+"/html/escaped.html"));QVERIFY(Documentation::localFile(root,"html/escaped.html","html").isEmpty());
        const auto previousData=qgetenv("XDG_DATA_HOME"),previousDirs=qgetenv("XDG_DATA_DIRS");
        const auto restore=qScopeGuard([=]{qputenv("XDG_DATA_HOME",previousData);qputenv("XDG_DATA_DIRS",previousDirs);});
        qputenv("XDG_DATA_HOME",QFile::encodeName(temporary.path()+"/empty"));qputenv("XDG_DATA_DIRS",QFile::encodeName(temporary.path()+"/empty"));
        QVERIFY(Documentation::links().isEmpty());QVERIFY(!Documentation::html().contains("href="));
        QVERIFY(Documentation::html().contains(QString(GAD_TR("Documentation is not installed.")).toHtmlEscaped()));
    }

    /// Les paramètres persistants sont validés avant tout appel au moteur.
    void commandSettingsParseBooleansRatesAndExclusions() {
        QString error;
        const QHash<QString,QString> flags{{"md5","verify_md5"},{"properties","verify_properties"},{"verify-space","verify_space"},{"keep-history","keep_history"},{"resume-interrupted","resume_interrupted"},{"allow-duplication","allow_duplication"}};
        for(auto it=flags.begin();it!=flags.end();++it)for(bool enabled:{false,true}) {
            const auto request=CliOptions::configure({"gad","--configure","--"+QString(enabled?"":"no-")+it.key()},error);QVERIFY(error.isEmpty());QCOMPARE(request[it.value()].toBool(),enabled);
        }
        const auto request=CliOptions::configure({"gad","--configure","--rate-limit","2.5","--exclude-folder","__*","--exclude-folder","node_modules","--no-include-hidden","--autostart","--no-konqueror-ctrl-v","--krusader-ctrl-v","--language","en_US"},error);
        QVERIFY(error.isEmpty());QCOMPARE(request["rate_limit"].toInteger(),2621440);QCOMPARE(request["exclusion_patch"].toObject()["directories"].toArray().size(),2);QVERIFY(!request["exclusion_patch"].toObject()["include_hidden"].toBool());QVERIFY(request["gui_settings"].toObject()["autostart"].toBool());QVERIFY(request["gui_settings"].toObject()["krusader_ctrl_v"].toBool());
        const QList<QStringList> invalid{{},{"--md5","--no-md5"},{"--language"},{"--rate-limit","nan"},{"--rate-limit","-1"},{"--rate-limit","9999999"},{"--unknown"},{"--no-language","fr_FR"}};
        for(const auto &arguments:invalid){QStringList cli{"gad","--configure"};cli.append(arguments);error.clear();CliOptions::configure(cli,error);QVERIFY(!error.isEmpty());}
    }

    /// Une commande reçue par IPC actualise les vrais widgets et préserve les règles non fournies.
    void commandSettingsUpdateWidgetsAndPreserveRules() {
        const QStringList keys{"verifyMd5","verifyProperties","verifySpace","keepHistory","resumeInterrupted","allowDuplication","includeHidden","excludeDirectories","excludeFiles","rateLimit","language","konquerorCtrlV"};
        QHash<QString,QVariant> previous;for(const auto &key:keys)previous[key]=QSettings().value(key);
        const auto restore=qScopeGuard([=]{for(const auto &key:keys){if(previous[key].isValid())QSettings().setValue(key,previous[key]);else QSettings().remove(key);}I18n::initialize("fr_FR");});
        Window window;window.handle({{"command","configure"},{"exclusions",QJsonObject{{"include_hidden",true},{"directories",QJsonArray{"__*"}},{"files",QJsonArray{"*.tmp"}}}}});mutations.clear();
        QString error;auto request=CliOptions::configure({"gad","--configure","--md5","--properties","--verify-space","--keep-history","--resume-interrupted","--allow-duplication","--no-include-hidden","--rate-limit","2.5"},error);
        QVERIFY(!window.handle(request).contains("error"));QCOMPARE(mutations.size(),1);QCOMPARE(mutations.last()["exclusions"].toObject()["directories"].toArray(),QJsonArray{"__*"});
        for(const auto &name:QStringList{"verifyMd5","verifyProperties","verifySpace","keepHistory","resumeInterrupted","allowDuplication"})QVERIFY(window.findChild<QCheckBox *>(name)->isChecked());
        QVERIFY(!window.findChild<QCheckBox *>("includeHidden")->isChecked());QCOMPARE(QSettings().value("rateLimit").toLongLong(),2621440);
        mutations.clear();request=CliOptions::configure({"gad","--configure","--language","en_US","--no-konqueror-ctrl-v"},error);QVERIFY(!window.handle(request).contains("error"));QVERIFY(mutations.isEmpty());QCOMPARE(QSettings().value("language").toString(),QString("en_US"));QVERIFY(!window.findChild<QCheckBox *>("konquerorShortcut")->isChecked());
        request=CliOptions::configure({"gad","--configure","--language","invalid_locale","--no-md5"},error);QVERIFY(window.handle(request).contains("error"));QVERIFY(window.findChild<QCheckBox *>("verifyMd5")->isChecked());QVERIFY(mutations.isEmpty());
    }

    /// La boîte reste ouverte pendant les réglages et conserve ses widgets entre deux ouvertures.
    void settingsDialogPreservesControlsAndUpdatesLanguage() {
        const auto previous=QSettings().value("verifyMd5"),previousLanguage=QSettings().value("language");
        const auto restore=qScopeGuard([previous,previousLanguage]{if(previous.isValid())QSettings().setValue("verifyMd5",previous);else QSettings().remove("verifyMd5");if(previousLanguage.isValid())QSettings().setValue("language",previousLanguage);else QSettings().remove("language");I18n::initialize("fr_FR");});
        publish(job({file("running")},"running"));Window window;window.show();
        QCOMPARE(window.minimumSize(),QSize(1100,600));
        auto *button=window.findChild<QToolButton *>("settingsButton");QVERIFY(button);QVERIFY(!button->menu());
        auto *dialog=window.findChild<QDialog *>("settingsDialog");QVERIFY(dialog);QVERIFY(!dialog->isVisible());QCOMPARE(dialog->minimumSize(),QSize(800,740));
        auto *panel=dialog->findChild<QWidget *>("settingsPanel");QVERIFY(panel);QCOMPARE(dialog->findChildren<QCheckBox *>().size(),13);
        for(auto *check:dialog->findChildren<QCheckBox *>())QVERIFY(dialog->findChild<QLabel *>(check->objectName()+"CommandOption"));
        QCOMPARE(panel->findChild<QLabel *>("verifyMd5CommandOption")->text(),QString("--md5 / --no-md5"));
        QCOMPARE(panel->findChild<QLabel *>("verifyPropertiesCommandOption")->text(),QString("--properties / --no-properties"));
        auto *tabs=dialog->findChild<QTabWidget *>("settingsTabs");QVERIFY(tabs);QCOMPARE(tabs->count(),5);QCOMPARE(tabs->tabText(2),QString("Débits"));QCOMPARE(tabs->tabText(3),QString("Fin de lot"));
        auto *rates=dialog->findChild<QWidget *>("ratesPanel");auto *completion=dialog->findChild<QWidget *>("completionPanel");QVERIFY(rates);QVERIFY(completion);QVERIFY(rates->findChild<QDoubleSpinBox *>("globalRateLimit"));QVERIFY(rates->findChild<QDoubleSpinBox *>("defaultRequestRateLimit"));QVERIFY(completion->findChild<QComboBox *>("completionAction"));QVERIFY(!panel->findChild<QDoubleSpinBox *>("globalRateLimit"));QVERIFY(!panel->findChild<QComboBox *>("completionAction"));
        QTest::mouseClick(button,Qt::LeftButton);QVERIFY(dialog->isVisible());QVERIFY(dialog->isModal());QCOMPARE(dialog->windowModality(),Qt::WindowModal);
        auto *md5=panel->findChild<QCheckBox *>("verifyMd5");const auto checked=md5->isChecked();mutations.clear();
        QTest::mouseClick(md5,Qt::LeftButton,Qt::NoModifier,QPoint(md5->style()->pixelMetric(QStyle::PM_IndicatorWidth)/2,md5->height()/2));QCOMPARE(md5->isChecked(),!checked);QVERIFY(dialog->isVisible());QCOMPARE(mutations.size(),1);
        QVERIFY(!window.findChild<QPushButton *>("settingsRequestRate"));auto *requestRate=window.findChild<QAction *>("rowRateLimit");QVERIFY(requestRate);QVERIFY(!requestRate->isEnabled());
        QVERIFY(window.findChild<QMenu *>("rowContextMenu")->actions().contains(requestRate));
        QVERIFY(dialog->findChild<QWidget *>("exclusionsPanel")->findChild<QLabel *>("exclusionsCommandOption"));QVERIFY(!panel->findChild<QLabel *>("languageCommandOption"));
        window.findChild<QTreeWidget *>()->setCurrentItem(window.entryRow(1,0));QVERIFY(requestRate->isEnabled());
        LanguageMenuDriver language(window);language->setCurrentIndex(language->findData("en_US"));QCOMPARE(dialog->windowTitle(),QString("Settings"));QCOMPARE(tabs->tabText(1),QString("Shortcuts"));QCOMPARE(tabs->tabText(2),QString("Speed limits"));QCOMPARE(tabs->tabText(3),QString("Batch completion"));
        auto *completionMenu=window.findChild<QToolButton *>("completionButton")->menu();QVERIFY(QMetaObject::invokeMethod(completionMenu,"aboutToShow",Qt::DirectConnection));completionMenu->actions().last()->trigger();QCOMPARE(tabs->currentWidget(),dialog->findChild<QScrollArea *>("completionScroll"));tabs->setCurrentIndex(0);
        auto *close=dialog->findChild<QPushButton *>("settingsCloseButton");QCOMPARE(close->text(),QString("Close"));
        tabs->setCurrentIndex(4);QCOMPARE(tabs->tabText(4),QString("Exclusions"));
        QVERIFY(tabs->currentWidget()->findChild<QPlainTextEdit *>("excludedDirectories"));QVERIFY(!window.findChild<QDialog *>("exclusionsDialog"));
        QTest::mouseClick(close,Qt::LeftButton);QVERIFY(!dialog->isVisible());
        QTest::mouseClick(button,Qt::LeftButton);QVERIFY(dialog->isVisible());QCOMPARE(panel,dialog->findChild<QWidget *>("settingsPanel"));QCOMPARE(md5->isChecked(),!checked);
        QTest::keyClick(dialog,Qt::Key_Escape);QVERIFY(!dialog->isVisible());
    }

    /// Un échec du gestionnaire réversible Dolphin rétablit la case sans toucher au moteur.
    void shortcutSettingsHandleHelperFailureAndPersistKonqueror() {
        QTemporaryDir directory;QVERIFY(directory.isValid());const auto oldPath=qgetenv("XDG_DATA_DIRS");const auto oldLanguage=QSettings().value("language"),oldKonqueror=QSettings().value("konquerorCtrlV");
        const auto restore=qScopeGuard([=]{qputenv("XDG_DATA_DIRS",oldPath);for(const auto &pair:QList<QPair<QString,QVariant>>{{"language",oldLanguage},{"konquerorCtrlV",oldKonqueror}}){if(pair.second.isValid())QSettings().setValue(pair.first,pair.second);else QSettings().remove(pair.first);}I18n::initialize("fr_FR");});
        QVERIFY(QDir().mkpath(directory.path()+"/gad/integration"));QFile helper(directory.path()+"/gad/integration/dolphin_shortcut.py");QVERIFY(helper.open(QIODevice::WriteOnly));helper.write("#!/usr/bin/python3\nimport sys\nif '--disable-ctrl-v' in sys.argv:\n print('refused test change',file=sys.stderr)\n sys.exit(1)\n");helper.close();QVERIFY(helper.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
        qputenv("XDG_DATA_DIRS",QFile::encodeName(directory.path())+":"+oldPath);
        Window window;auto *dolphin=window.findChild<QCheckBox *>("dolphinShortcut");QVERIFY(dolphin->isEnabled());QVERIFY(!dolphin->isChecked());mutations.clear();
        dolphin->setChecked(true);QVERIFY(!dolphin->isEnabled());QTRY_VERIFY(dolphin->isEnabled());QVERIFY(dolphin->isChecked());QVERIFY(mutations.isEmpty());
        dolphin->setChecked(false);QTRY_VERIFY(dolphin->isEnabled());QVERIFY(dolphin->isChecked());QVERIFY(window.findChild<QMessageBox *>("errorDialog"));QVERIFY(mutations.isEmpty());
        bool completed=false;QJsonObject reply;window.handleAsync({{"command","configure"},{"gui_settings",QJsonObject{{"dolphin_ctrl_v",false}}}},[&](QJsonObject result){reply=result;completed=true;});
        QTRY_VERIFY(completed);QVERIFY(reply.contains("error"));QVERIFY(dolphin->isChecked());
        completed=false;window.handleAsync({{"command","configure"},{"gui_settings",QJsonObject{{"dolphin_ctrl_v",true}}}},[&](QJsonObject result){reply=result;completed=true;});QTRY_VERIFY(completed);QVERIFY(!reply.contains("error"));QVERIFY(dolphin->isChecked());
        auto *konqueror=window.findChild<QCheckBox *>("konquerorShortcut");const auto before=konqueror->isChecked();konqueror->setChecked(!before);QCOMPARE(QSettings().value("konquerorCtrlV").toBool(),!before);
    }

    /// L’option Krusader utilise le même chemin asynchrone en CLI et dans la boîte d’options.
    void krusaderShortcutHandlesFailureAndProfileStorage() {
        QTemporaryDir directory;QVERIFY(directory.isValid());const auto oldPath=qgetenv("XDG_DATA_DIRS");
        const auto restore=qScopeGuard([=]{qputenv("XDG_DATA_DIRS",oldPath);});
        QVERIFY(QDir().mkpath(directory.path()+"/gad/integration"));QFile helper(directory.path()+"/gad/integration/krusader_shortcut.py");
        QVERIFY(helper.open(QIODevice::WriteOnly));helper.write("import sys\nif '--disable-ctrl-v' in sys.argv:\n print('refused test change',file=sys.stderr)\n sys.exit(1)\n");helper.close();
        qputenv("XDG_DATA_DIRS",QFile::encodeName(directory.path())+":"+oldPath);
        Window window;auto *check=window.findChild<QCheckBox *>("krusaderShortcut");QVERIFY(check);QVERIFY(!check->isChecked());
        bool completed=false;QJsonObject reply;mutations.clear();
        window.handleAsync({{"command","configure"},{"gui_settings",QJsonObject{{"krusader_ctrl_v",true}}}},[&](QJsonObject result){reply=result;completed=true;});
        QTRY_VERIFY(completed);QVERIFY(!reply.contains("error"));QVERIFY(check->isChecked());
        QCOMPARE(window.currentTransferProfile().gui["krusader_ctrl_v"].toBool(),true);
        QString failure;auto profile=window.currentTransferProfile();profile.name="Krusader test";QVERIFY(TransferProfiles::validate(profile,failure));
        completed=false;window.handleAsync({{"command","configure"},{"gui_settings",QJsonObject{{"krusader_ctrl_v",false}}}},[&](QJsonObject result){reply=result;completed=true;});
        QTRY_VERIFY(completed);QVERIFY(reply.contains("error"));QVERIFY(check->isChecked());QVERIFY(mutations.isEmpty());
    }

    /// Le plan demandé par IPC ne doit pas bloquer le thread d’interface.
    void simulationIpcRepliesAsynchronously() {
        Window window;simulationDelay=50;simulationPlan={{"ok",true},{"rows",QJsonArray{}}};mutations.clear();bool finished=false;QJsonObject response;
        window.handleAsync({{"command","simulate"},{"id",1}},[&](QJsonObject result){response=result;finished=true;});QVERIFY(!finished);QTRY_VERIFY(finished);QVERIFY(response["ok"].toBool());QVERIFY(mutations.isEmpty());simulationDelay=0;
    }

    /// Le presse-papiers suit la préparation manuelle dans Gad et le lancement immédiat depuis un gestionnaire de fichiers.
    void manualPreparationAndClipboardUseDifferentStartRules() {
        Window window;window.findChild<QLineEdit *>("destinationInput")->setText("/tmp");mutations.clear();window.enqueue({"/tmp/manual-file"});QVERIFY(!mutations.isEmpty());QVERIFY(mutations.last()["prepare_only"].toBool());
        for(bool cut:{false,true}) {auto *mime=new QMimeData;mime->setUrls({QUrl::fromLocalFile("/tmp/pasted-file")});if(cut)mime->setData("application/x-kde-cutselection","1");QApplication::clipboard()->setMimeData(mime);mutations.clear();window.pasteInto("/tmp",true);QVERIFY(!mutations.isEmpty());QVERIFY(!mutations.last()["prepare_only"].toBool());QCOMPARE(mutations.last()["mode"].toString(),cut?QString("move"):QString("copy"));}
        for(bool cut:{false,true}) {
            auto *mime=new QMimeData;mime->setUrls({QUrl::fromLocalFile("/tmp/prepared-clipboard-file")});if(cut)mime->setData("application/x-kde-cutselection","1");QApplication::clipboard()->setMimeData(mime);
            mutations.clear();window.findChild<QAction *>("clipboardFilesAction")->trigger();QVERIFY(!mutations.isEmpty());QVERIFY(mutations.last()["prepare_only"].toBool());QCOMPARE(mutations.last()["mode"].toString(),cut?QString("move"):QString("copy"));
        }
        QVERIFY(!window.findChild<QPushButton *>("simulateButton"));QVERIFY(!window.findChild<QDialog *>("simulationDialog"));
    }
    /// Un ajout du presse-papiers n’autorise pas implicitement un lot prêt, en pause ou historique.
    void clipboardAdditionKeepsPreparedQueueWaiting() {
        for(const auto &kind:QStringList{"ready","paused","history"}) {
            auto row=job({file(kind=="history"?"done":"waiting")},kind=="history"?"done":"waiting");row["start_required"]=kind!="history";
            status["jobs"]=QJsonArray{row};status["paused"]=kind=="paused";
            Window window;window.findChild<QLineEdit *>("destinationInput")->setText("/tmp");
            auto *mime=new QMimeData;mime->setUrls({QUrl::fromLocalFile("/tmp/manual-clipboard")});QApplication::clipboard()->setMimeData(mime);mutations.clear();
            window.findChild<QAction *>("clipboardFilesAction")->trigger();
            QCOMPARE(mutations.size(),1);QCOMPARE(mutations[0]["command"].toString(),QString("enqueue"));QVERIFY(mutations[0]["prepare_only"].toBool());
        }
    }

    /// Un seul bouton démarre, suspend ou reprend toute la file selon son état.
    void singleQueueButtonStartsPausesAndResumes() {
        auto prepared=job(QJsonArray{file("waiting")});prepared["start_required"]=true;prepared["state"]="waiting";status["jobs"]=QJsonArray{prepared};Window window;window.refresh();
        auto *button=window.findChild<QPushButton *>("queueControlButton");QVERIFY(button);QVERIFY(!window.findChild<QPushButton *>("startPreparedButton"));QVERIFY(button->isEnabled());QCOMPARE(button->text(),QString(GAD_TR("Start")));QCOMPARE(window.entryRow(1,0)->text(2),QString(GAD_TR("Ready to start")));
        mutations.clear();button->click();QCOMPARE(mutations.last()["command"].toString(),QString("pause"));QVERIFY(!mutations.last()["paused"].toBool());
        prepared["start_required"]=false;prepared["state"]="running";status["jobs"]=QJsonArray{prepared};window.refresh();QCOMPARE(button->text(),QString(GAD_TR("Pause all")));mutations.clear();button->click();QVERIFY(mutations.last()["paused"].toBool());
        status["paused"]=true;window.refresh();QCOMPARE(button->text(),QString(GAD_TR("Resume queue")));mutations.clear();button->click();QVERIFY(!mutations.last()["paused"].toBool());
        status["paused"]=false;prepared["start_required"]=true;prepared["state"]="scanning";status["jobs"]=QJsonArray{prepared};window.refresh();QVERIFY(!button->isEnabled());
    }
    /// Un ajout depuis la fenêtre rejoint automatiquement une file déjà active.
    void manualAdditionDuringWorkJoinsTheQueue() {
        publish(job({file("running")},"running"));Window window;window.findChild<QLineEdit *>("destinationInput")->setText("/tmp");mutations.clear();window.enqueue({"/tmp/next-manual-file"});QVERIFY(!mutations.last()["prepare_only"].toBool());
        auto *mime=new QMimeData;mime->setUrls({QUrl::fromLocalFile("/tmp/next-clipboard-file")});QApplication::clipboard()->setMimeData(mime);
        mutations.clear();window.findChild<QAction *>("clipboardFilesAction")->trigger();QVERIFY(!mutations.isEmpty());QVERIFY(!mutations.last()["prepare_only"].toBool());
    }

    /// Une pause mémorisée ne rend aucune action disponible sur une file vide.
    void emptyQueueDisablesTransferAndHistoryActions() {
        status["paused"]=true;Window window;window.refresh();
        for(const auto &name:QStringList{"queueControlButton","cancellationMenu","clearHistoryButton"})QVERIFY2(!window.findChild<QPushButton *>(name)->isEnabled(),qPrintable(name));
        QVERIFY(!window.findChild<QAction *>("cancelAllAction")->isEnabled());QVERIFY(!window.findChild<QAction *>("undoAllAction")->isEnabled());QCOMPARE(window.findChild<QPushButton *>("queueControlButton")->text(),QString(GAD_TR("Start")));
        window.refresh();QVERIFY(!window.findChild<QPushButton *>("queueControlButton")->isEnabled());
    }
    /// Une nouvelle préparation affiche Démarrer même si une pause précédente est mémorisée.
    void preparedQueueUsesStartDespiteRememberedPause() {
        auto prepared=job({file("waiting")},"waiting");prepared["start_required"]=true;status["jobs"]=QJsonArray{prepared};status["paused"]=true;Window window;window.refresh();
        auto *button=window.findChild<QPushButton *>("queueControlButton");QVERIFY(button->isEnabled());QCOMPARE(button->text(),QString(GAD_TR("Start")));mutations.clear();button->click();QCOMPARE(mutations.last()["command"].toString(),QString("pause"));QVERIFY(!mutations.last()["paused"].toBool());
    }

    /// Après nettoyage au démarrage, les trois boutons sont recalculés immédiatement.
    void clearingStartupHistoryDisablesAllThreeButtons() {
        status["paused"]=true;publish(job({file("done")}));clearHistoryInMock=true;Window window;window.refresh();
        auto *clear=window.findChild<QPushButton *>("clearHistoryButton");QVERIFY(clear->isEnabled());QVERIFY(window.findChild<QPushButton *>("cancellationMenu")->isEnabled());QVERIFY(!window.findChild<QAction *>("cancelAllAction")->isEnabled());
        clear->click();QVERIFY(status["jobs"].toArray().isEmpty());QVERIFY(!clear->isEnabled());QVERIFY(!window.findChild<QPushButton *>("queueControlButton")->isEnabled());QVERIFY(!window.findChild<QPushButton *>("cancellationMenu")->isEnabled());
    }
    /// La suppression par instantané différentiel ne conserve pas les boutons de l’ancien historique.
    void emptyIncrementalSnapshotDisablesButtonsDespiteSavedPause() {
        status={{"revision",1},{"reset",true},{"job_ids",QJsonArray{1}},{"jobs",QJsonArray{job({file("done")})}},{"paused",true}};Window window;window.refresh();QVERIFY(window.findChild<QPushButton *>("clearHistoryButton")->isEnabled());
        status={{"revision",2},{"reset",false},{"job_ids",QJsonArray{}},{"jobs",QJsonArray{}},{"paused",true}};window.refresh();
        for(const auto &name:QStringList{"queueControlButton","cancellationMenu","clearHistoryButton"})QVERIFY(!window.findChild<QPushButton *>(name)->isEnabled());
        status={{"revision",2},{"unchanged",true}};window.refresh();QVERIFY(!window.findChild<QPushButton *>("queueControlButton")->isEnabled());
    }

    /// Nettoyer les résultats ne désactive pas la pause d’un transfert restant.
    void historyCleanupKeepsActiveQueueControlsAvailable() {
        auto running=job({file("running")},"running");running["id"]=2;status["jobs"]=QJsonArray{job({file("done")}),running};clearHistoryInMock=true;Window window;window.refresh();
        window.findChild<QPushButton *>("clearHistoryButton")->click();QCOMPARE(status["jobs"].toArray().size(),1);QVERIFY(window.findChild<QPushButton *>("queueControlButton")->isEnabled());QVERIFY(window.findChild<QPushButton *>("cancellationMenu")->isEnabled());QVERIFY(!window.findChild<QPushButton *>("clearHistoryButton")->isEnabled());QVERIFY(window.findChild<QAction *>("cancelAllAction")->isEnabled());
    }

    /// Une restauration sans écriture antérieure ne doit pas prétendre avoir restauré un fichier.
    void untouchedRestorationUsesCanceled() {
        auto untouched=file("undone");untouched["copied"]=0;publish(job({untouched},"undone"));Window window;window.refresh();QCOMPARE(window.entryRow(1,0)->text(2),QString(GAD_TR("Canceled")));QCOMPARE(RowTree::owner(window.entryRow(1,0))->text(2),QString(GAD_TR("Canceled")));
        auto restored=untouched;restored["undo"]=QJsonObject{{"result",QJsonObject{{"dev",1}}},{"reverted",true}};publish(job({restored},"undone"));window.refresh();QCOMPARE(window.entryRow(1,0)->text(2),QString(GAD_TR("Restored")));
        restored=untouched;restored["copied"]=4;publish(job({restored},"undone"));window.refresh();QCOMPARE(window.entryRow(1,0)->text(2),QString(GAD_TR("Restored")));
    }
    /// Le dossier reste remis en état si un enfant a été restauré, sans attribuer cet effet à ses frères.
    void restorationLabelsFollowEachFolderBranch() {
        auto entries=nestedEntries();for(int i=0;i<entries.size();++i){auto e=entries[i].toObject();e["state"]="undone";e["copied"]=0;entries[i]=e;}auto changed=entries[2].toObject();changed["copied"]=4;entries[2]=changed;publish(job(entries,"undone"));Window window;window.refresh();
        QCOMPARE(window.entryRow(1,0)->text(2),QString(GAD_TR("Restored")));QCOMPARE(window.entryRow(1,1)->text(2),QString(GAD_TR("Restored")));QCOMPARE(window.entryRow(1,2)->text(2),QString(GAD_TR("Restored")));QCOMPARE(window.entryRow(1,3)->text(2),QString(GAD_TR("Canceled")));QCOMPARE(window.entryRow(1,4)->text(2),QString(GAD_TR("Canceled")));
    }

    /// Une réponse sans lignes modifiées signale les doublons une seule fois, sans boîte modale.
    void duplicateNoticeHandlesMetadataOnlyUpdates() {
        status["duplicates_ignored"]=5;status["revision"]=1;status["reset"]=true;status["job_ids"]=QJsonArray{};
        Window window;window.refresh();
        auto *bar=window.statusBar();QVERIFY(bar);
        QVERIFY(bar->currentMessage().isEmpty());
        status["duplicates_ignored"]=7;status["revision"]=2;status["reset"]=false;
        window.refresh();
        QVERIFY(bar->currentMessage().contains("2 élément(s)"));
        bar->clearMessage();window.refresh();
        QVERIFY(bar->currentMessage().isEmpty());
    }

    /// Le réglage global envoie des octets/s ; plusieurs lignes d’une demande ne la ciblent qu’une fois.
    void rateControlsTargetRequestsAndConvertMib() {
        publish(job({file("waiting"),file("waiting")},"waiting"));Window window;
        auto *global=window.findChild<QDoubleSpinBox *>("globalRateLimit");QVERIFY(global);mutations.clear();
        global->setValue(2.5);
        QVERIFY(!mutations.isEmpty());QCOMPARE(mutations.first()["rate_limit"].toInteger(),qint64(2621440));
        auto *tree=window.findChild<QTreeWidget *>();selectFile(window);window.entryRow(1,1)->setSelected(true);
        window.updateRowActions();auto *action=window.findChild<QAction *>("rowRateLimit");QVERIFY(action->isEnabled());action->trigger();
        auto *dialog=window.findChild<QDialog *>("requestRateDialog");QVERIFY(dialog);
        auto *spin=dialog->findChild<QDoubleSpinBox *>("requestRateLimit");QVERIFY(spin);spin->setValue(1.25);
        tree->clearSelection();mutations.clear(); // La cible reste celle choisie à l’ouverture.
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(mutations.size(),1);QCOMPARE(mutations[0]["command"].toString(),QString("set_rate_limit"));
        QCOMPARE(mutations[0]["ids"].toArray(),QJsonArray{1});QCOMPARE(mutations[0]["bytes_per_second"].toInteger(),qint64(1310720));
        QSettings().remove("rateLimit");
    }

    /// La limite accompagne les détails sans masquer une erreur et disparaît si elle est retirée.
    void requestRateLimitAppearsInDetailsAndUpdatesInPlace() {
        auto entry=file("running");entry["error"]="Test error";auto value=job({entry},"running");value["rate_limit"]=1310720;publish(value);Window window;window.refresh();auto *row=window.entryRow(1,0);const auto cap=QString(GAD_TR("Limit: %1")).arg(TransferMetrics::speed(1310720));
        QVERIFY(row->text(4).contains("Test error"));QVERIFY(row->text(4).contains(cap));QVERIFY(row->toolTip(4).contains(cap));QVERIFY(RowTree::owner(row)->text(4).contains(cap));QVERIFY(!window.findChild<QPushButton *>("settingsRequestRate"));
        value["rate_limit"]=1;publish(value);window.refresh();QCOMPARE(window.entryRow(1,0),row);QVERIFY(row->text(4).contains(TransferMetrics::speed(1)));QVERIFY(!row->text(4).contains(cap));
        value["rate_limit"]=0;publish(value);window.refresh();QCOMPARE(row->text(4),QString("Test error"));QVERIFY(RowTree::owner(row)->text(4).isEmpty());
    }
    /// Les dossiers et leurs enfants affichent la limite reçue de leur demande.
    void requestRateDetailsIncludeFolderBranchesAndPropertyTooltips() {
        auto entries=nestedEntries();auto entry=entries[2].toObject();entry["properties"]=QJsonObject{{"status","different"},{"differences",QJsonArray{"mode"}}};entries[2]=entry;auto value=job(entries,"running");value["rate_limit"]=1048576;publish(value);Window window;window.refresh();const auto cap=QString(GAD_TR("Limit: %1")).arg(TransferMetrics::speed(1048576));
        for(int i=0;i<entries.size();++i){QVERIFY(window.entryRow(1,i)->text(4).contains(cap));}QTextDocument tip;tip.setHtml(window.entryRow(1,2)->toolTip(4));QVERIFY(tip.toPlainText().contains(cap));QVERIFY(tip.toPlainText().contains("mode"));
    }

    /// Les deux demandes ont des compteurs indépendants et une estimation de leur travail restant.
    void metricsIndependentRatesAndEstimates() {
        TransferMetrics::Tracker tracker;
        auto a=file("running");a["size"]=10000;a["copied"]=0;
        auto b=a;
        auto first=job({a},"running"),second=job({b},"running");second["id"]=2;
        auto state=QJsonObject{{"jobs",QJsonArray{first,second}}};
        auto lines=tracker.sync(state,{1,2},0);QCOMPARE(lines.size(),2);QVERIFY(lines[0].contains("calcul du débit"));
        a["copied"]=1000;b["copied"]=2000;first["entries"]=QJsonArray{a};second["entries"]=QJsonArray{b};state["jobs"]=QJsonArray{first,second};
        lines=tracker.sync(state,{1,2},1000);
        QVERIFY(lines[0].contains(TransferMetrics::speed(1000)));QVERIFY(lines[0].contains("9 s"));
        QVERIFY(lines[1].contains(TransferMetrics::speed(2000)));QVERIFY(lines[1].contains("4 s"));
        state["paused"]=true;lines=tracker.sync(state,{},1200);QVERIFY(lines[0].contains("En pause"));QVERIFY(!lines[0].contains("/s"));
        state["paused"]=false;lines=tracker.sync(state,{},1300);QVERIFY(lines[0].contains("à estimer"));
        state["jobs"]=QJsonArray{};QVERIFY(tracker.sync(state,{},1400).isEmpty());
    }

    /// Les ticks sans nouvelle progression effacent un débit ancien, sans recompter les préfixes de reprise.
    void metricsStallResumePrefixAndCounterReset() {
        TransferMetrics::Sample s;s.probe.phase="copy";s.probe.copyLeft=9000;s.probe.copied=5000;
        s.tick(0,false);s.probe.copied=6000;s.tick(1000,false);QCOMPARE(s.copy.speed(),1000.);
        for(int n=2;n<=7;++n)s.tick(n*1000,false);
        QCOMPARE(s.copy.speed(),0.);QVERIFY(TransferMetrics::detail(s,false).contains("0 octet/s"));
        s.probe.copied=1;s.tick(8000,false);QCOMPARE(s.copy.speed(),0.);
        auto e=file("running");e["size"]=10000;e["copied"]=9000;e["resumed_from"]=8500;
        auto p=TransferMetrics::Probe::from(job({e},"running"));QCOMPARE(p.copied,500.);QCOMPARE(p.copyLeft,1000.);
        e["undo"]=QJsonObject{{"rename_move",true}};p=TransferMetrics::Probe::from(job({e},"running"));QCOMPARE(p.copied,0.);
    }

    /// MD5 et copie ont leurs propres débits ; les durées inconnues ne sont pas remplacées par zéro.
    void metricsMd5PhasesAndBlockedEntries() {
        TransferMetrics::Sample s;s.probe.phase="copy";s.probe.copyLeft=2000;s.probe.verifyLeft=6000;
        s.tick(0,false);s.probe.copied=1000;s.tick(1000,false);
        QVERIFY(TransferMetrics::detail(s,false).contains("MD5 à estimer"));
        s.probe.phase="md5";s.tick(1100,false);s.probe.verified=2000;s.tick(2100,false);
        auto text=TransferMetrics::detail(s,false);QVERIFY(text.startsWith("MD5"));QVERIFY(text.contains("5 s"));
        s.probe.blocked=true;QVERIFY(TransferMetrics::detail(s,false).contains("fichiers en pause"));s.probe.blocked=false;
        s.probe.phase="conflict";s.tick(2200,false);QVERIFY(TransferMetrics::detail(s,false).contains("À décider"));
        s.probe.phase="rename";QVERIFY(!TransferMetrics::detail(s,false).contains("/s"));
        QCOMPARE(TransferMetrics::duration(60),QString("1 min 0 s"));QCOMPARE(TransferMetrics::duration(3601),QString("1 h 0 min"));
        QCOMPARE(TransferMetrics::duration(std::numeric_limits<double>::infinity()),QString("indéterminé"));
    }

    /// Le vrai widget se met à jour même quand poll ne contient aucune nouvelle ligne et garde les items.
    void metricsPanelHandlesUnchangedPollWithoutReplacingRows() {
        auto e=file("running");e["copied"]=0;e["size"]=10000;
        publish(job({e},"running"));status["revision"]=1;status["reset"]=true;status["job_ids"]=QJsonArray{1};
        Window window;auto *panel=window.findChild<QLabel *>("requestMetrics");QVERIFY(panel);QVERIFY(panel->text().contains("#1"));
        auto *row=selectFile(window);const auto tip=row->toolTip(0);
        status={{"revision",1},{"unchanged",true}};window.refresh();
        QCOMPARE(window.entryRow(1,0),row);QVERIFY(row->isSelected());QCOMPARE(row->toolTip(0),tip);
        QVERIFY(panel->text().contains("temps restant"));
        status={{"revision",2},{"jobs",QJsonArray{}},{"job_ids",QJsonArray{}}};window.refresh();QVERIFY(panel->isHidden());
    }

    /// Le montage absent est visible sur les lignes et dans le récapitulatif ; les résultats OK restent verts.
    void disconnectedRowsKeepHistoryAndOfferCancellation() {
        auto pending=file("device_waiting"),completed=file("done");publish(job({pending,completed},"device_waiting"));
        Window window;auto *row=selectFile(window);window.updateRowActions();
        QVERIFY(row->text(2).contains("périphérique"));QCOMPARE(window.entryRow(1,1)->text(2),QString("OK"));
        QVERIFY(window.findChild<QLabel *>("requestMetrics")->text().contains("vérification avant reprise"));
        QVERIFY(window.findChild<QAction *>("rowCancel")->isEnabled());
        QVERIFY(!row->data(0,RowTree::CanQueueRole).toBool());
        status["paused"]=true;window.refresh();QVERIFY(row->text(2).contains("en pause"));
    }

    /// L’export conserve les caractères difficiles et produit un JSON relisible ; une écriture impossible est signalée.
    void errorReportExportsUtf8AndAtomicFiles() {
        QTemporaryDir temp;QVERIFY(temp.isValid());
        auto record=QJsonObject{{"job",1},{"index",0},{"scope","entry"},{"mode","copy"},{"state","error"},{"kind","file"},{"source","/tmp/été\n<x>.txt"},{"destination","/tmp/dest"},{"error","échec\n<texte>"},{"retryable",true}};
        QJsonObject report{{"format","gad-error-report"},{"version",1},{"generated_unix",0},{"records",QJsonArray{record}}};
        auto text=ErrorReport::encode(report,false);QVERIFY(text.contains(QString("échec\n<texte>").toUtf8()));QVERIFY(text.contains(QString("été\n<x>").toUtf8()));
        QCOMPARE(QJsonDocument::fromJson(ErrorReport::encode(report,true)).object(),report);
        QString message;const auto path=temp.filePath("rapport.json");QVERIFY(ErrorReport::save(path,report,true,message));QFile saved(path);QVERIFY(saved.open(QIODevice::ReadOnly));QCOMPARE(QJsonDocument::fromJson(saved.readAll()).object(),report);
        const auto directory=temp.filePath("directory");QVERIFY(QDir().mkdir(directory));QVERIFY(!ErrorReport::save(directory,report,false,message));QVERIFY(!message.isEmpty());QVERIFY(QFileInfo(directory).isDir());
    }

    /// Des chemins de montage identiques peuvent appartenir à des origines différentes selon le lot.
    void errorReportIndexesKeepEachRemoteOrigin() {
        QJsonArray owners,records;
        for(int id=1;id<=2;++id) {
            owners.append(QJsonObject{{"id",id},{"sources",QJsonArray{"/mount/source"}},{"source_urls",QJsonArray{QString("sftp://host%1/source").arg(id)}},{"destination","/mount/target"},{"destination_url",QString("ftp://host%1/target").arg(id)}});
            records.append(QJsonObject{{"job",id},{"scope","entry"},{"mode","copy"},{"state","error"},{"kind","file"},{"source","/mount/source/file.txt"},{"destination","/mount/target/file.txt"},{"error","failure"}});
        }
        const QJsonObject report{{"jobs",owners},{"records",records},{"generated_unix",0}};
        const auto text=QString::fromUtf8(ErrorReport::encode(report,false));
        for(int id=1;id<=2;++id) {
            QVERIFY(text.contains(QString("sftp://host%1/source/file.txt").arg(id)));
            QVERIFY(text.contains(QString("ftp://host%1/target/file.txt").arg(id)));
        }
        QVERIFY(!text.contains("/mount/"));
        errorReport=report;Window window;window.showErrorReport();
        auto *rows=window.findChild<QTreeWidget *>("errorReportRows");QCOMPARE(rows->topLevelItemCount(),2);
        for(int i=0;i<2;++i) {
            auto *row=rows->topLevelItem(i);const auto id=row->text(0).toInt();
            QCOMPARE(row->text(2),QString("sftp://host%1/source/file.txt").arg(id));
            QCOMPARE(row->text(3),QString("ftp://host%1/target/file.txt").arg(id));
        }
    }

    /// Le bilan réessaie seulement les erreurs réessayables affichées après filtrage, avec leurs indices stables.
    void errorReportFiltersAndTargetsExactRows() {
        QJsonArray records;
        for(int i=0;i<3;++i)records.append(QJsonObject{{"job",i+1},{"index",0},{"scope","entry"},{"mode","copy"},{"state",i==1?"done":"error"},{"kind","file"},{"source",i==2?"/tmp/second":"/tmp/first"},{"destination","/tmp/dest"},{"error","détail\n<texte>"},{"retryable",i!=1},{"retry_reason",i==1?"finalisation":""}});
        errorReport={{"records",records},{"jobs",QJsonArray{}},{"generated_unix",0}};
        Window window;window.findChild<QPushButton *>("errorReportButton")->click();
        auto *dialog=window.findChild<QDialog *>("errorReportDialog");QVERIFY(dialog);QCOMPARE(dialog->windowModality(),Qt::WindowModal);QCOMPARE(dialog->minimumSize(),QSize(1100,600));
        auto *rows=dialog->findChild<QTreeWidget *>("errorReportRows");QCOMPARE(rows->topLevelItemCount(),3);
        dialog->findChild<QLineEdit *>("errorReportSearch")->setText("second");mutations.clear();
        dialog->findChild<QPushButton *>("retryAllErrors")->click();QCOMPARE(mutations.size(),1);
        QCOMPARE(mutations[0]["command"].toString(),QString("retry_errors"));QCOMPARE(mutations[0]["entries"].toArray(),QJsonArray({QJsonObject{{"job",3},{"index",0}}}));QCOMPARE(mutations[0]["exact"].toBool(),true);
        dialog->findChild<QLineEdit *>("errorReportSearch")->clear();
        auto matching=rows->findItems("2",Qt::MatchExactly,0);QCOMPARE(matching.size(),1);rows->setCurrentItem(matching[0]);matching[0]->setSelected(true);
        QVERIFY(!dialog->findChild<QPushButton *>("retrySelectedErrors")->isEnabled());
        QVERIFY(dialog->findChild<QPlainTextEdit *>("errorReportDetail")->toPlainText().contains("détail\n<texte>"));
        QVERIFY(dialog->findChild<QPushButton *>("exportErrorReport")->isEnabled());
    }

    /// Afficher une erreur dans la liste libère la fenêtre principale bloquée par le bilan.
    void errorReportLocateClosesModalBeforeReturningToList() {
        publish(job({file("error")},"error"));errorReport["records"]=QJsonArray{QJsonObject{{"job",1},{"index",0},{"scope","entry"},{"mode","copy"},{"state","error"},{"error","failure"}}};
        Window window;auto *history=window.findChild<QPushButton *>("historySearchButton");history->setChecked(true);window.findChild<QLineEdit *>("historySearchText")->setText("no-matching-error-file");QTest::qWait(200);QVERIFY(window.entryRow(1,0)->isHidden());
        window.showErrorReport();auto *dialog=window.findChild<QDialog *>("errorReportDialog");QVERIFY(dialog);QVERIFY(dialog->isVisible());
        auto *rows=dialog->findChild<QTreeWidget *>("errorReportRows");rows->setCurrentItem(rows->topLevelItem(0));auto *locate=dialog->findChild<QPushButton *>("locateReportError");QVERIFY(locate->isEnabled());locate->click();
        QVERIFY(!history->isChecked());QVERIFY(!window.entryRow(1,0)->isHidden());QVERIFY(!dialog->isVisible());QVERIFY(window.isVisible());QVERIFY(window.entryRow(1,0)->isSelected());QCOMPARE(window.findChild<QTreeWidget *>()->currentItem(),window.entryRow(1,0));
    }

    /// Un filtre portant sur un message traduit doit suivre la langue et vider le détail masqué.
    void errorReportLanguageChangeReappliesFilter() {
        I18n::initialize("fr_FR");const auto restore=qScopeGuard([]{I18n::initialize("fr_FR");});
        errorReport["records"]=QJsonArray{QJsonObject{{"job",1},{"index",0},{"scope","entry"},{"mode","copy"},{"state","error"},{"source","/tmp/a"},{"error","No files to copy"}}};
        Window window;window.showErrorReport();auto *dialog=window.findChild<QDialog *>("errorReportDialog");auto *rows=dialog->findChild<QTreeWidget *>("errorReportRows");auto *row=rows->topLevelItem(0);auto *detail=dialog->findChild<QPlainTextEdit *>("errorReportDetail");
        dialog->findChild<QLineEdit *>("errorReportSearch")->setText("Aucun fichier");QVERIFY(!row->isHidden());rows->setCurrentItem(row);QVERIFY(!detail->toPlainText().isEmpty());
        I18n::initialize("en_US");QVERIFY(row->isHidden());QVERIFY(!row->isSelected());QVERIFY(detail->toPlainText().isEmpty());QVERIFY(!dialog->findChild<QPushButton *>("locateReportError")->isEnabled());
        I18n::initialize("fr_FR");QVERIFY(!row->isHidden());QVERIFY(!row->isSelected());
    }

    /// Le menu distingue les erreurs réessayables des fichiers OK et des opérations actives.
    void retryErrorsContextOnlyOffersStoppedFailures() {
        auto value=job({file("error"),file("done")},"error");value["journaled"]=true;publish(value);Window window;
        selectFile(window);window.updateRowActions();auto *action=window.findChild<QAction *>("rowRetryErrors");QVERIFY(action->isEnabled());mutations.clear();action->trigger();
        QCOMPARE(mutations.size(),1);QCOMPARE(mutations[0]["command"].toString(),QString("retry_errors"));QCOMPARE(mutations[0]["entries"].toArray(),QJsonArray({QJsonObject{{"job",1},{"index",0}}}));
        window.findChild<QTreeWidget *>()->clearSelection();selectFile(window,1);window.updateRowActions();QVERIFY(!action->isEnabled());
        auto restored=file("undone");restored["error"]="ancien message";value=job({restored},"undone");value["journaled"]=true;publish(value);window.refresh();selectFile(window);window.updateRowActions();QVERIFY(!action->isEnabled());
    }

    /// Chaque bouton garde une icône même si le thème ne fournit aucun nom attendu.
    void errorReportButtonsHaveFallbackIcons() {
        const auto previous=QIcon::themeName(),fallback=QIcon::fallbackThemeName();
        const auto restore=qScopeGuard([previous,fallback]{QIcon::setThemeName(previous);QIcon::setFallbackThemeName(fallback);});
        QIcon::setThemeName("gad-test-missing-theme");QIcon::setFallbackThemeName(QString());
        Window window;window.showErrorReport();auto *dialog=window.findChild<QDialog *>("errorReportDialog");QVERIFY(dialog);
        for(const auto &name:QStringList{"retryAllErrors","retrySelectedErrors","locateReportError","exportErrorReport","refreshErrorReport","closeErrorReport"}) {
            auto *button=dialog->findChild<QPushButton *>(name);QVERIFY(button);QVERIFY2(!button->icon().isNull(),qPrintable(name));
        }
    }

    /// Aucun fichier vide ne doit être proposé à l’export.
    void emptyErrorReportHasNoRetryAction() {
        Window window;window.showErrorReport();auto *dialog=window.findChild<QDialog *>("errorReportDialog");QVERIFY(dialog);
        QVERIFY(!dialog->findChild<QPushButton *>("retryAllErrors")->isEnabled());QVERIFY(!dialog->findChild<QPushButton *>("retrySelectedErrors")->isEnabled());
        QVERIFY(!dialog->findChild<QPushButton *>("exportErrorReport")->isEnabled());
    }

    /// Le filtre ne rend pas vide l’instantané exporté ; Actualiser peut en revanche le vider.
    void exportAvailabilityFollowsSnapshotNotFilter() {
        Window window;window.showErrorReport();auto *dialog=window.findChild<QDialog *>("errorReportDialog");
        auto *button=dialog->findChild<QPushButton *>("exportErrorReport");
        auto *reload=dialog->findChild<QPushButton *>("refreshErrorReport");
        auto *search=dialog->findChild<QLineEdit *>("errorReportSearch");
        QVERIFY(!button->isEnabled());
        errorReport["records"]=QJsonArray{QJsonObject{{"job",1},{"index",0},{"scope","entry"},{"state","error"},{"error","failure"}}};
        reload->click();QVERIFY(button->isEnabled());search->setText("no matching record");QVERIFY(button->isEnabled());
        errorReport["records"]=QJsonArray{};reload->click();QVERIFY(!button->isEnabled());
    }

    /// Le catalogue français et les sources US affichent la même opération sans toucher aux chemins/protocoles.
    void americanEnglishInterfaceAndReport() {
        applicationIcon();I18n::initialize("fr_FR");
        QCOMPARE(label("waiting"),QString("En attente"));
        QCOMPARE(I18n::message("No files to copy"),QString("Aucun fichier à copier"));
        I18n::initialize("en_US");
        const auto restore=qScopeGuard([]{I18n::initialize("fr_FR");QSettings().remove("language");});
        QCOMPARE(QLocale().name(),QString("en_US"));
        QCOMPARE(label("scanning"),QString("Scanning…"));QCOMPARE(label("waiting"),QString("Queued"));QCOMPARE(label("running",true),QString("Paused"));
        QCOMPARE(I18n::resolve("system",QLocale("en_GB")),QString("en_US"));
        QCOMPARE(I18n::resolve("system",QLocale("fr_FR")),QString("fr_FR"));
        QCOMPARE(I18n::message("No files to copy"),QString("No files to copy"));
        const QString filename="/tmp/En pause/Erreur.txt";QCOMPARE(I18n::message(filename),filename);
        const QJsonObject record{{"job",1},{"index",0},{"scope","entry"},{"state","error"},{"source",filename},{"destination","/tmp/target"},{"error","No files to copy"}};
        errorReport["records"]=QJsonArray{record};const auto raw=ErrorReport::encode(errorReport,true);
        QVERIFY(raw.contains("No files to copy"));
        I18n::initialize("fr_FR");
        const auto frenchText=ErrorReport::encode(errorReport,false);QVERIFY(frenchText.contains("Aucun fichier"));
        QCOMPARE(ErrorReport::encode(errorReport,true),raw);
        I18n::initialize("en_US");
        const auto text=ErrorReport::encode(errorReport,false);QVERIFY(text.contains("Error report"));QVERIFY(text.contains("No files to copy"));QVERIFY(text.contains(filename.toUtf8()));
        Window window;QVERIFY(window.windowTitle().contains("Advanced Duplication Manager"));window.showErrorReport();
        auto *dialog=window.findChild<QDialog *>("errorReportDialog");QCOMPARE(dialog->windowTitle(),QString("Gad — Error report"));
        QCOMPARE(dialog->findChild<QPushButton *>("exportErrorReport")->text(),QString("Export…"));
        LanguageMenuDriver language(window);QVERIFY(language);
        for(const auto &locale:QStringList{"system","fr_FR","en_US"}) {
            const auto index=language->findData(locale);QVERIFY(index>=0);QVERIFY(!language->itemIcon(index).pixmap(24,24).isNull());
        }
        language->setCurrentIndex(language->findData("en_US"));
        QCOMPARE(QSettings().value("language").toString(),QString("en_US"));
    }

    /// Le catalogue espagnol embarqué s’applique immédiatement sans recréer les lignes ni commander le moteur.
    void spanishLanguageSwitchUsesEmbeddedCatalog() {
        const auto saved=QSettings().value("language");
        const auto restore=qScopeGuard([saved]{if(saved.isValid())QSettings().setValue("language",saved);else QSettings().remove("language");I18n::initialize("fr_FR");});
        QCOMPARE(I18n::resolve("es_MX"),QString("es_ES"));
        QCOMPARE(I18n::resolve("system",QLocale("es_AR")),QString("es_ES"));
        I18n::initialize("fr_FR");publish(job({file("running")},"running"));Window window;
        LanguageMenuDriver language(window);auto *row=window.entryRow(1,0);
        const auto index=language->findData("es_ES");QVERIFY(index>=0);QVERIFY(!language->itemIcon(index).pixmap(24,24).isNull());
        mutations.clear();language->setCurrentIndex(index);
        QCOMPARE(QSettings().value("language").toString(),QString("es_ES"));
        QCOMPARE(QString(GAD_TR("Source")),QString("Origen"));
        QCOMPARE(QString(GAD_TR("Include hidden items")),QString("Incluir elementos ocultos"));
        QCOMPARE(row,window.entryRow(1,0));QVERIFY(mutations.isEmpty());
        language->setCurrentIndex(language->findData("en_US"));QCOMPARE(QString(GAD_TR("Source")),QString("Source"));
        language->setCurrentIndex(language->findData("fr_FR"));QCOMPARE(QString(GAD_TR("Source")),QString("Source"));
    }

    /// La variante portugaise utilise le catalogue embarqué sans modifier les transferts.
    void portugueseLanguageSwitchUsesEmbeddedCatalog() {
        const auto saved=QSettings().value("language");
        const auto restore=qScopeGuard([saved]{if(saved.isValid())QSettings().setValue("language",saved);else QSettings().remove("language");I18n::initialize("fr_FR");});
        QCOMPARE(I18n::resolve("pt_PT"),QString("pt_PT"));
        QCOMPARE(I18n::resolve("system",QLocale("pt_PT")),QString("pt_PT"));
        I18n::initialize("fr_FR");publish(job({file("running")},"running"));Window window;
        LanguageMenuDriver language(window);auto *row=window.entryRow(1,0);
        const auto index=language->findData("pt_PT");QVERIFY(index>=0);QVERIFY(!language->itemIcon(index).pixmap(24,24).isNull());
        mutations.clear();language->setCurrentIndex(index);
        QCOMPARE(QSettings().value("language").toString(),QString("pt_PT"));
        QCOMPARE(QString(GAD_TR("Source")),QString::fromUtf8("Origem"));
        QCOMPARE(QString(GAD_TR("Include hidden items")),QString::fromUtf8("Incluir itens ocultos"));
        QCOMPARE(row,window.entryRow(1,0));QVERIFY(mutations.isEmpty());
        language->setCurrentIndex(language->findData("en_US"));QCOMPARE(QString(GAD_TR("Source")),QString("Source"));
    }

    /// Le nouveau catalogue allemand est découvert sans ajout de langue dans l’interface.
    void germanLanguageUsesDiscoveredCatalog() {
        const auto saved=QSettings().value("language");
        const auto restore=qScopeGuard([saved]{if(saved.isValid())QSettings().setValue("language",saved);else QSettings().remove("language");I18n::initialize("fr_FR");});
        QCOMPARE(I18n::resolve("de_AT"),QString("de_DE"));
        I18n::initialize("fr_FR");publish(job({file("running")},"running"));Window window;
        LanguageMenuDriver language(window);auto *row=window.entryRow(1,0);
        const auto index=language->findData("de_DE");QVERIFY(index>=0);QVERIFY(!language->itemIcon(index).pixmap(24,24).isNull());
        mutations.clear();language->setCurrentIndex(index);
        QCOMPARE(QSettings().value("language").toString(),QString("de_DE"));
        QCOMPARE(QString(GAD_TR("Source")),QString("Quelle"));QCOMPARE(QString(GAD_TR("Copy")),QString("Kopieren"));
        QCOMPARE(QString(GAD_TR("Include hidden items")),QString::fromUtf8("Versteckte Elemente einschließen"));
        QCOMPARE(row,window.entryRow(1,0));QVERIFY(mutations.isEmpty());
        language->setCurrentIndex(language->findData("en_US"));QCOMPARE(QString(GAD_TR("Source")),QString("Source"));
    }

    /// Le japonais est découvert et appliqué à chaud sans modifier les lignes ni les transferts.
    void japaneseLanguageUsesDiscoveredCatalog() {
        const auto saved=QSettings().value("language");
        const auto restore=qScopeGuard([saved]{if(saved.isValid())QSettings().setValue("language",saved);else QSettings().remove("language");I18n::initialize("fr_FR");});
        QCOMPARE(I18n::resolve("system",QLocale("ja_JP")),QString("ja_JP"));
        I18n::initialize("fr_FR");publish(job({file("running")},"running"));Window window;
        LanguageMenuDriver language(window);auto *row=window.entryRow(1,0);
        const auto index=language->findData("ja_JP");QVERIFY(index>=0);QVERIFY(!language->itemIcon(index).pixmap(24,24).isNull());
        QCOMPARE(language->itemText(index),QString::fromUtf8("日本語"));
        mutations.clear();language->setCurrentIndex(index);
        QCOMPARE(QSettings().value("language").toString(),QString("ja_JP"));
        QCOMPARE(QString(GAD_TR("Source")),QString::fromUtf8("コピー元"));QCOMPARE(QString(GAD_TR("Copy")),QString::fromUtf8("コピー"));
        QCOMPARE(QString(GAD_TR("Include hidden items")),QString::fromUtf8("隠しファイルとフォルダーを含める"));
        QCOMPARE(row,window.entryRow(1,0));QVERIFY(mutations.isEmpty());
        language->setCurrentIndex(language->findData("en_US"));QCOMPARE(QString(GAD_TR("Source")),QString("Source"));
    }

    /// Un nouveau QM suffit à créer une langue avec son image, sans liste codée en dur.
    void discoversExternalLanguagesAndFlags() {
        const auto directory=QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+"/gad/translations";
        QVERIFY(QDir().mkpath(directory));
        const auto catalog=directory+"/gad_de_DE.qm",flag=directory+"/de_DE.png",invalid=directory+"/gad_it_IT.qm";
        const auto cleanup=qScopeGuard([=]{QFile::remove(catalog);QFile::remove(flag);QFile::remove(invalid);I18n::initialize("fr_FR");});
        // Le contenu français sert de fixture valide ; seule sa découverte est exercée ici.
        QVERIFY(QFile::copy(":/i18n/gad_fr_FR.qm",catalog));QVERIFY(QFile::copy(":/flags/fr_FR.png",flag));
        QFile broken(invalid);QVERIFY(broken.open(QIODevice::WriteOnly));broken.write("invalid catalog");broken.close();
        const auto available=I18n::languages();bool found=false;
        for(const auto &entry:available) {
            QVERIFY(entry.locale!="it_IT");
            if(entry.locale=="de_DE"){found=true;QCOMPARE(entry.catalog,catalog);QCOMPARE(entry.flag,flag);QVERIFY(!entry.name.isEmpty());}
        }
        QVERIFY(found);QCOMPARE(I18n::resolve("de_AT"),QString("de_DE"));
        Window window;LanguageMenuDriver selector(window);
        QVERIFY(selector->findData("de_DE")>=0);QVERIFY(!selector->itemIcon(selector->findData("de_DE")).pixmap(24,24).isNull());
        I18n::initialize("de_DE");QCOMPARE(QString(GAD_TR("Include hidden items")),QString::fromUtf8("Inclure les éléments cachés"));
    }

    /// Une traduction à chaud garde les widgets, les données saisies et les références moteur intactes.
    void languageSwitchPreservesLiveWidgetsAndDoesNotWriteEngine() {
        I18n::initialize("fr_FR");const auto restore=qScopeGuard([]{I18n::initialize("fr_FR");QSettings().remove("language");});
        auto running=file("running");running["copied"]=25;publish(job({running},"running"));
        Window window;auto *tree=window.findChild<QTreeWidget *>();auto *item=window.entryRow(1,0);QVERIFY(item);
        tree->setCurrentItem(item);item->setExpanded(false);const auto source=item->text(0);
        auto *destination=window.findChild<QLineEdit *>("destinationInput");destination->setText("/tmp/En pause/Source");
        auto *rate=window.findChild<QDoubleSpinBox *>("globalRateLimit");rate->setValue(12.5);
        window.showAbout();auto *about=window.findChild<QMessageBox *>("aboutDialog");QVERIFY(about);
        errorReport["records"]=QJsonArray{QJsonObject{{"job",7},{"index",0},{"scope","entry"},{"state","error"},{"mode","copy"},{"source","/tmp/fichier"},{"error","No files to copy"}}};
        window.showErrorReport();auto *report=window.findChild<QDialog *>("errorReportDialog");
        auto *reportRows=report->findChild<QTreeWidget *>("errorReportRows");auto *reportRow=reportRows->topLevelItem(0);reportRows->setCurrentItem(reportRow);
        auto *search=report->findChild<QLineEdit *>("errorReportSearch");search->setText("fichier");
        window.error("No files to copy");auto *errorDialog=window.findChild<QMessageBox *>("errorDialog");QVERIFY(errorDialog);
        window.showRateLimit();auto *rateDialog=window.findChild<QDialog *>("requestRateDialog");QVERIFY(rateDialog);
        auto *requestRate=rateDialog->findChild<QDoubleSpinBox *>("requestRateLimit");requestRate->setValue(7.25);
        // L’instantané du bilan reste celui déjà chargé, même si le moteur a changé depuis.
        errorReport["records"]=QJsonArray{};status={{"unchanged",true},{"revision",1}};mutations.clear();
        LanguageMenuDriver language(window);const auto bindingCount=I18n::bindings().size();
        language->setCurrentIndex(language->findData("en_US"));
        QVERIFY(window.windowTitle().contains("Advanced Duplication Manager"));QCOMPARE(tree->headerItem()->text(2),QString("Status"));
        QCOMPARE(window.entryRow(1,0),item);QVERIFY(item->isSelected());QVERIFY(!item->isExpanded());QCOMPARE(item->text(0),source);QCOMPARE(item->text(2),QString("Running"));
        QCOMPARE(destination->text(),QString("/tmp/En pause/Source"));QCOMPARE(rate->value(),12.5);QCOMPARE(rate->suffix(),QString(" MiB/s"));
        QCOMPARE(errorDialog->text(),QString("No files to copy"));
        QCOMPARE(about->windowTitle(),QString("About Gad"));QCOMPARE(report->windowTitle(),QString("Gad — Error report"));
        QCOMPARE(reportRows->topLevelItemCount(),1);QCOMPARE(reportRows->topLevelItem(0),reportRow);QVERIFY(reportRow->isSelected());QCOMPARE(search->text(),QString("fichier"));
        QCOMPARE(reportRow->text(5),QString("No files to copy"));QVERIFY(report->findChild<QPlainTextEdit *>("errorReportDetail")->toPlainText().contains("Source:"));
        QCOMPARE(rateDialog->windowTitle(),QString("Limit request speed"));QCOMPARE(requestRate->value(),7.25);
        bool addTranslated=false;for(auto *action:window.findChildren<QAction *>())addTranslated|=action->text()=="Add files…";QVERIFY(addTranslated);
        for(int i=0;i<4;++i) {language->setCurrentIndex(language->findData(i%2?"en_US":"fr_FR"));QCOMPARE(I18n::bindings().size(),bindingCount);}
        language->setCurrentIndex(language->findData("fr_FR"));
        QCOMPARE(tree->headerItem()->text(2),QString("État"));QCOMPARE(item->text(2),QString("En cours"));
        QCOMPARE(errorDialog->text(),QString("Aucun fichier à copier"));
        QCOMPARE(reportRow->text(5),QString("Aucun fichier à copier"));QCOMPARE(requestRate->value(),7.25);QCOMPARE(rate->value(),12.5);
        QVERIFY(mutations.isEmpty());
        QCOMPARE(QSettings().value("language").toString(),QString("fr_FR"));
    }

    /// Changer la langue ne réinitialise pas le choix « tous les prochains conflits ».
    void languageSwitchPreservesConflictChoice() {
        I18n::initialize("fr_FR");const auto restore=qScopeGuard([]{I18n::initialize("fr_FR");QSettings().remove("language");});
        publish(job({file("conflict")},"conflict"));Window window;
        auto *all=window.findChild<QCheckBox *>("applyToBatchConflicts");QVERIFY(all);all->setChecked(true);
        LanguageMenuDriver language(window);mutations.clear();
        language->setCurrentIndex(language->findData("en_US"));
        QVERIFY(all->isChecked());QVERIFY(all->text().contains("future conflicts"));
        QCOMPARE(qobject_cast<QGroupBox *>(window.findChild<QLabel *>("conflictSourceName")->parentWidget())->title(),QString("File to copy"));
        language->setCurrentIndex(language->findData("fr_FR"));QVERIFY(all->isChecked());QVERIFY(all->text().contains("prochains conflits"));
        QVERIFY(mutations.isEmpty());
    }

    /// Le tri naturel est visuel : identités, sélection, priorité et glisser d’origine sont conservés.
    void headerSortPreservesQueueAndStableRows() {
        QJsonArray entries;for(const auto &name:QStringList{"file10.txt","file2.txt","file1.txt"}){auto e=file("waiting");e["source"]="/tmp/source/"+name;e["destination"]="/tmp/destination/"+name;entries.append(e);}
        publish(job(entries,"waiting"));Window window;auto *view=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());QVERIFY(view);
        auto *selected=selectFile(window,0);std::unique_ptr<QMimeData> initial(view->selectionMime());QVERIFY(!initial->data("application/x-gad-rows").isEmpty());mutations.clear();status={{"unchanged",true},{"revision",1}};
        view->setViewSort(0);QCOMPARE(view->topLevelItem(0),window.entryRow(1,2));QCOMPARE(view->topLevelItem(1),window.entryRow(1,1));
        QCOMPARE(window.entryRow(1,0),selected);QVERIFY(selected->isSelected());QCOMPARE(TransferTree::key(selected)["index"].toInt(),0);
        std::unique_ptr<QMimeData> sorted(view->selectionMime());QVERIFY(sorted->data("application/x-gad-rows").isEmpty());
        view->setViewSort(0,Qt::DescendingOrder);QCOMPARE(view->topLevelItem(0),selected);
        view->setViewSort(-1);QCOMPARE(view->topLevelItem(0),selected);QCOMPARE(view->topLevelItem(2),window.entryRow(1,2));
        std::unique_ptr<QMimeData> queued(view->selectionMime());QVERIFY(!queued->data("application/x-gad-rows").isEmpty());
        QMetaObject::invokeMethod(view->header(),"sectionClicked",Q_ARG(int,0));QCOMPARE(view->presentation().sorts.first().order,Qt::AscendingOrder);QCOMPARE(view->topLevelItem(0),window.entryRow(1,2));
        QMetaObject::invokeMethod(view->header(),"sectionClicked",Q_ARG(int,0));QCOMPARE(view->presentation().sorts.first().order,Qt::DescendingOrder);QCOMPARE(view->topLevelItem(0),selected);QVERIFY(mutations.isEmpty());
    }

    /// Les filtres se combinent entre colonnes et avec le filtre global, en gardant le contexte des dossiers.
    void headerFiltersKeepAncestorsAndCombineState() {
        publish(job(nestedEntries(),"running"));Window window;auto *view=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());
        auto *root=window.entryRow(1,0);auto *sub=window.entryRow(1,1);auto *match=window.entryRow(1,2);mutations.clear();
        view->setColumnFilter(0,"A.TXT");QVERIFY(!root->isHidden());QVERIFY(!sub->isHidden());QVERIFY(!match->isHidden());QVERIFY(window.entryRow(1,3)->isHidden());QVERIFY(window.entryRow(1,4)->isHidden());
        QVERIFY(root->isExpanded());root->setExpanded(false);view->setViewSort(0);QVERIFY(!root->isExpanded());QCOMPARE(match->parent(),sub);QCOMPARE(sub->parent(),root);
        view->setColumnFilter(1,"missing destination");QVERIFY(root->isHidden());view->clearColumnFilter(1);QVERIFY(!root->isHidden());
        view->setStateFilter("done");QVERIFY(root->isHidden());
        view->clearColumnFilter(2);QVERIFY(!match->isHidden());
        view->clearHeaderFilters();QVERIFY(!window.entryRow(1,3)->isHidden());QVERIFY(!window.entryRow(1,4)->isHidden());QVERIFY(mutations.isEmpty());
    }

    /// La progression est un nombre, indépendamment de la taille ou des unités traduites affichées.
    void headerProgressSortAndRangeFollowUpdates() {
        QJsonArray entries;for(const auto &pair:QList<QPair<double,double>>{{90,100},{18,100},{2048,4096}}){auto e=file("running");const auto name=QString::number(entries.size());e["source"]="/tmp/source/"+name;e["destination"]="/tmp/destination/"+name;e["copied"]=pair.first;e["size"]=pair.second;entries.append(e);}
        publish(job(entries,"running"));Window window;auto *view=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());view->setViewSort(3);
        QCOMPARE(view->topLevelItem(0),window.entryRow(1,1));QCOMPARE(view->topLevelItem(1),window.entryRow(1,2));
        view->setProgressFilter(50,100);QVERIFY(window.entryRow(1,1)->isHidden());QVERIFY(!window.entryRow(1,2)->isHidden());
        auto e=entries[1].toObject();e["copied"]=80;entries[1]=e;publish(job(entries,"running"));window.refresh();
        QVERIFY(!window.entryRow(1,1)->isHidden());QCOMPARE(view->topLevelItem(0),window.entryRow(1,2));QCOMPARE(view->topLevelItem(1),window.entryRow(1,1));
        view->setViewSort(-1);QCOMPARE(view->topLevelItem(0),window.entryRow(1,0));
    }

    /// Le menu d’en-tête est réellement relié à la vue et ses états gardent leur clé après traduction.
    void headerMenuFiltersAndTranslatesLive() {
        I18n::initialize("fr_FR");const auto restore=qScopeGuard([]{I18n::initialize("fr_FR");QSettings().remove("language");});
        publish(job({file("error")},"error"));Window window;auto *view=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());auto *menu=view->headerMenu(2);QVERIFY(menu);
        auto *search=menu->findChild<QLineEdit *>("headerColumnFilter");auto *states=menu->findChild<QComboBox *>("headerStateFilter");QVERIFY(search);QVERIFY(states);
        search->setText("Erreur");states->setCurrentIndex(states->findData("error"));QCOMPARE(view->presentation().state,QString("error"));
        auto *sort=menu->findChild<QAction *>("headerSortAscending");sort->trigger();QCOMPARE(view->presentation().sorts.first().column,2);
        I18n::initialize("en_US");QCOMPARE(sort->text(),QString("Sort ascending"));QCOMPARE(search->text(),QString("Erreur"));QCOMPARE(states->currentData().toString(),QString("error"));QCOMPARE(states->currentText(),QString("Error"));
        menu->findChild<QAction *>("headerClearColumn")->trigger();QVERIFY(!view->presentation().filtered(2));QVERIFY(!window.entryRow(1,0)->isHidden());
        menu->findChild<QAction *>("headerQueueOrder")->trigger();QVERIFY(view->presentation().sorts.isEmpty());
    }

    /// Les vrais événements Ctrl+clic construisent un tri lexicographique sans changer la file.
    void ctrlClickCombinesThreeSortColumns() {
        I18n::initialize("fr_FR");const auto restore=qScopeGuard([]{I18n::initialize("fr_FR");QSettings().remove("language");});
        QJsonArray entries;const QStringList names={"file10.txt","file2.txt","file2.txt","file1.txt"};
        const QList<int> copied={80,70,30,100};
        for(int i=0;i<4;++i){auto e=file(i==3?"done":"waiting");e["source"]=QString("/tmp/source/%1/%2").arg(i).arg(names[i]);e["destination"]=QString("/tmp/destination/%1/%2").arg(i).arg(names[i]);e["size"]=100;e["copied"]=copied[i];entries.append(e);}
        publish(job(entries,"waiting"));Window window;window.show();auto *view=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());
        for(int c=0;c<5;++c){view->setColumnWidth(c,150);}
        QCoreApplication::processEvents();
        auto *selected=selectFile(window,2);mutations.clear();
        const auto click=[view](int column,Qt::KeyboardModifiers modifiers){auto *header=view->header();QTest::mouseClick(header->viewport(),Qt::LeftButton,modifiers,QPoint(header->sectionViewportPosition(column)+75,header->height()/2));};
        click(2,Qt::NoModifier);click(0,Qt::ControlModifier);
        QCOMPARE(view->presentation().sorts.size(),2);QCOMPARE(view->topLevelItem(0),window.entryRow(1,1));
        click(3,Qt::ControlModifier);QCOMPARE(view->presentation().sorts.size(),3);
        QCOMPARE(view->topLevelItem(0),selected);QCOMPARE(view->topLevelItem(1),window.entryRow(1,1));QCOMPARE(view->topLevelItem(2),window.entryRow(1,0));
        QVERIFY(view->headerItem()->text(2).endsWith("(1)"));QVERIFY(view->headerItem()->text(0).endsWith("(2)"));QVERIFY(view->headerItem()->text(3).endsWith("(3)"));
        click(0,Qt::ControlModifier);QCOMPARE(view->presentation().sorts.size(),3);QCOMPARE(view->presentation().sorts[1].order,Qt::DescendingOrder);QCOMPARE(view->topLevelItem(0),window.entryRow(1,0));QVERIFY(selected->isSelected());
        I18n::initialize("en_US");QVERIFY(view->headerItem()->text(2).startsWith("Status"));QVERIFY(view->headerItem()->text(2).endsWith("(1)"));QCOMPARE(view->presentation().sorts.size(),3);
        click(1,Qt::NoModifier);QCOMPARE(view->presentation().sorts.size(),1);QCOMPARE(view->presentation().sorts.first().column,1);
        auto *menu=view->headerMenu(1);menu->findChild<QAction *>("headerRemoveSort")->trigger();QVERIFY(view->presentation().sorts.isEmpty());QCOMPARE(view->topLevelItem(0),window.entryRow(1,0));QVERIFY(mutations.isEmpty());
    }

    /// Les options du style portent une flèche native par critère, sans décoration à gauche.
    void multiSortUsesNativeHeaderArrows() {
        class HeaderProbe:public TransferHeader {
        public:
            HeaderProbe():TransferHeader(nullptr){}
            using TransferHeader::initStyleOptionForIndex;
        } header;
        QStandardItemModel model(0,3);header.setModel(&model);
        header.setSortIndicators({{0,Qt::AscendingOrder},{2,Qt::DescendingOrder}});
        QStyleOptionHeader first,second,third;
        header.initStyleOptionForIndex(&first,0);header.initStyleOptionForIndex(&second,1);header.initStyleOptionForIndex(&third,2);
        QCOMPARE(first.sortIndicator,QStyleOptionHeader::SortDown);QCOMPARE(second.sortIndicator,QStyleOptionHeader::None);QCOMPARE(third.sortIndicator,QStyleOptionHeader::SortUp);
        header.setSortIndicators({});header.initStyleOptionForIndex(&first,0);QCOMPARE(first.sortIndicator,QStyleOptionHeader::None);
        Window window;auto *view=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());view->setViewSort(2);view->setViewSort(0,Qt::DescendingOrder,true);
        for(int c=0;c<5;++c)QVERIFY(view->headerItem()->icon(c).isNull());
        QVERIFY(view->headerItem()->text(2).endsWith("(1)"));QVERIFY(view->headerItem()->text(0).endsWith("(2)"));
    }

    /// Les nouvelles actions restent illustrées avec un thème incomplet ; le filtre global est bien retiré.
    void menuIconsAndNoRedundantGlobalFilter() {
        const auto previous=QIcon::themeName(),fallback=QIcon::fallbackThemeName();
        const auto restore=qScopeGuard([previous,fallback]{QIcon::setThemeName(previous);QIcon::setFallbackThemeName(fallback);});
        QIcon::setThemeName("gad-missing-menu-icons");QIcon::setFallbackThemeName(QString());
        Window window;for(auto *combo:window.findChildren<QComboBox *>())QCOMPARE(combo->findData("all"),-1);
        auto *cancel=window.findChild<QPushButton *>("cancellationMenu");for(auto *action:cancel->menu()->actions())if(!action->isSeparator())QVERIFY(!action->icon().isNull());
        for(const auto &name:QStringList{"rowSelectRequest","rowRetryErrors"}){auto *action=window.findChild<QAction *>(name);QVERIFY(action);QVERIFY(!action->icon().isNull());}
        auto *view=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());auto *menu=view->headerMenu(0);
        for(auto *action:menu->actions())if(!action->isSeparator() && !qobject_cast<QWidgetAction *>(action))QVERIFY(!action->icon().isNull());
    }

    /// La numérotation expose la file moteur et reste indépendante des tris et filtres d’affichage.
    void queueOrderColumnTracksPriorityAndBatch() {
        I18n::initialize("fr_FR");const auto restore=qScopeGuard([]{I18n::initialize("fr_FR");});
        auto a=file("waiting"),m=file("waiting"),z=file("waiting");a["source"]="/tmp/source/a.txt";m["source"]="/tmp/source/m.txt";z["source"]="/tmp/source/z.txt";
        auto first=job({a,m},"waiting"),second=job({z},"waiting");first["id"]=7;second["id"]=11;
        const QJsonObject aKey{{"job",7},{"index",0}},mKey{{"job",7},{"index",1}},zKey{{"job",11},{"index",0}};
        status={{"jobs",QJsonArray{first,second}},{"job_ids",QJsonArray{7,11}},{"row_order",QJsonArray{zKey,mKey,aKey}},{"revision",1},{"reset",true},{"paused",false}};
        Window window;auto *view=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());const int column=TableView::OrderColumn;
        QCOMPARE(view->columnCount(),6);QCOMPARE(view->header()->visualIndex(column),0);QCOMPARE(view->treePosition(),0);QCOMPARE(view->headerItem()->text(column),QString("Ordre"));
        auto *aRow=window.entryRow(7,0),*mRow=window.entryRow(7,1),*zRow=window.entryRow(11,0);
        QCOMPARE(zRow->text(column),QString("1"));QCOMPARE(mRow->text(column),QString("2"));QCOMPARE(aRow->text(column),QString("3"));QCOMPARE(aRow->toolTip(column).section('\n',0,0),QString("Lot #7"));
        view->setCurrentItem(aRow);aRow->setSelected(true);mutations.clear();view->setViewSort(0);
        QCOMPARE(view->topLevelItem(0),aRow);QCOMPARE(aRow->text(column),QString("3"));QVERIFY(aRow->isSelected());
        view->setColumnFilter(0,"a.txt");QVERIFY(!aRow->isHidden());QVERIFY(zRow->isHidden());QCOMPARE(aRow->text(column),QString("3"));
        view->clearHeaderFilters();view->setViewSort(column);QCOMPARE(view->topLevelItem(0),zRow);QCOMPARE(view->topLevelItem(1),mRow);
        view->setViewSort(-1);
        // Une priorité modifiée seule doit renuméroter aussi les demandes absentes du delta jobs.
        QSignalSpy changes(view->model(),&QAbstractItemModel::dataChanged);
        status={{"jobs",QJsonArray{}},{"job_ids",QJsonArray{7,11}},{"row_order",QJsonArray{aKey,zKey,mKey}},{"revision",2},{"paused",false}};window.refresh();
        for(const auto &signal:changes){const auto first=qvariant_cast<QModelIndex>(signal[0]);const auto last=qvariant_cast<QModelIndex>(signal[1]);const auto roles=qvariant_cast<QList<int>>(signal[2]);if(roles.contains(Qt::DisplayRole)){QCOMPARE(first.column(),column);QCOMPARE(last.column(),column);}}
        QCOMPARE(aRow->text(column),QString("1"));QCOMPARE(zRow->text(column),QString("2"));QCOMPARE(mRow->text(column),QString("3"));QCOMPARE(view->topLevelItem(0),aRow);QVERIFY(aRow->isSelected());
        view->setColumnFilter(column,"2");QVERIFY(!zRow->isHidden());QVERIFY(aRow->isHidden());I18n::initialize("en_US");
        QCOMPARE(view->headerItem()->text(column),QString("Order"));QCOMPARE(zRow->toolTip(column).section('\n',0,0),QString("Batch #11"));QCOMPARE(zRow->text(column),QString("2"));QVERIFY(mutations.isEmpty());
    }

    /// Les dossiers et les demandes en analyse ont aussi une position ; un filtre garde les ancêtres.
    void queueOrderColumnIncludesFoldersAndScanning() {
        auto nested=job(nestedEntries(),"running"),scanning=job({},"scanning");scanning["id"]=2;
        status["jobs"]=QJsonArray{nested,scanning};Window window;auto *view=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());
        QCOMPARE(window.entryRow(1,0)->text(TableView::OrderColumn),QString("1"));QCOMPARE(window.entryRow(1,1)->text(TableView::OrderColumn),QString("2"));QCOMPARE(window.entryRow(1,2)->text(TableView::OrderColumn),QString("3"));
        QTreeWidgetItem *pending=nullptr;for(int i=0;i<view->topLevelItemCount();++i)if(view->topLevelItem(i)->data(0,Qt::UserRole).toLongLong()==2)pending=view->topLevelItem(i);
        QVERIFY(pending);QCOMPARE(pending->text(TableView::OrderColumn),QString("6"));QCOMPARE(pending->toolTip(TableView::OrderColumn).section('\n',0,0),QString("Lot #2"));
        view->setColumnFilter(TableView::OrderColumn,"3");QVERIFY(!window.entryRow(1,0)->isHidden());QVERIFY(!window.entryRow(1,1)->isHidden());QVERIFY(!window.entryRow(1,2)->isHidden());QVERIFY(pending->isHidden());
        QCOMPARE(window.entryRow(1,2)->parent(),window.entryRow(1,1));
    }

    /// Seules les phrases rédigées pour l’aide sont découpées, jamais les noms de fichiers ou les URL.
    void sourceAndHelpTooltipsStayReadableInBothLanguages() {
        I18n::initialize("fr_FR");const auto restore=qScopeGuard([]{I18n::initialize("fr_FR");});
        auto entry=file("running");const QString path="/tmp/source/version. Finale <b>.txt";entry["source"]=path;publish(job({entry},"running"));Window window;
        auto *row=window.entryRow(1,0);QTextDocument source;source.setHtml(row->toolTip(0));
        QCOMPARE(source.toPlainText(),path);QCOMPARE(row->toolTip(TableView::OrderColumn).section('\n',0,0),QString("Lot #1"));
        auto *md5=window.findChild<QCheckBox *>("verifyMd5");QVERIFY(md5->toolTip().contains(".\n"));
        QVERIFY(!md5->toolTip().contains(". S’applique"));
        auto *space=window.findChild<QCheckBox *>("verifySpace");QCOMPARE(space->toolTip().count('\n'),4);
        I18n::initialize("en_US");source.setHtml(row->toolTip(0));QCOMPARE(source.toPlainText(),path);
        QCOMPARE(row->toolTip(TableView::OrderColumn).section('\n',0,0),QString("Batch #1"));QVERIFY(md5->toolTip().contains(".\nApplies"));QCOMPARE(space->toolTip().count('\n'),4);
    }

    /// Le mode reste visible sur tous les niveaux, sans remplacer les métadonnées dossier/fichier.
    void sourceIconsIdentifyCopyAndMoveWithoutTooltipRepetition() {
        publish(job(nestedEntries(),"running"));Window window;auto *view=window.findChild<QTreeWidget *>();
        auto *folder=window.entryRow(1,0),*leaf=window.entryRow(1,2);QVERIFY(!folder->icon(0).isNull());QVERIFY(!leaf->icon(0).isNull());
        QCOMPARE(folder->data(0,RowTree::OperationRole).toString(),QString("copy"));QCOMPARE(leaf->data(0,RowTree::OperationRole).toString(),QString("copy"));
        QCOMPARE(folder->data(0,RowTree::KindRole).toString(),QString("directory"));QVERIFY(folder->font(0).bold());QCOMPARE(leaf->parent(),window.entryRow(1,1));
        QTextDocument tip;tip.setHtml(leaf->toolTip(0));QCOMPARE(tip.toPlainText(),QString("/tmp/source/root/sub/a.txt"));
        auto moved=job(nestedEntries(),"running");moved["mode"]="move";publish(moved);window.refresh();
        QCOMPARE(window.entryRow(1,2),leaf);QCOMPARE(leaf->data(0,RowTree::OperationRole).toString(),QString("move"));QCOMPARE(folder->data(0,RowTree::OperationRole).toString(),QString("move"));
        tip.setHtml(leaf->toolTip(0));QCOMPARE(tip.toPlainText(),QString("/tmp/source/root/sub/a.txt"));
        moved["verify_md5"]=true;moved["rate_limit"]=1048576;publish(moved);window.refresh();
        tip.setHtml(leaf->toolTip(0));QVERIFY(tip.toPlainText().endsWith("\nMD5"));QVERIFY(!tip.toPlainText().contains("Limite :"));QVERIFY(leaf->text(4).contains("Limite :"));QVERIFY(!tip.toPlainText().contains("Déplacement"));
        QCOMPARE(view->header()->sectionResizeMode(TableView::OrderColumn),QHeaderView::ResizeToContents);
        window.show();QCoreApplication::processEvents();QVERIFY(view->columnWidth(TableView::OrderColumn)<85);
        auto preparation=job({},"scanning");preparation["mode"]="move";publish(preparation);window.refresh();
        auto *pending=view->topLevelItem(0);QVERIFY(pending);QCOMPARE(pending->data(0,RowTree::OperationRole).toString(),QString("move"));QVERIFY(!pending->icon(0).isNull());
    }

    /// Le changement de visibilité des cachés ne réécrit jamais une requête déjà soumise.
    void hiddenOptionIsCapturedForNewRequests() {
        const QStringList keys={"includeHidden","excludeDirectories","excludeFiles"};QMap<QString,QVariant> previous;
        for(const auto &key:keys){previous.insert(key,QSettings().value(key));QSettings().remove(key);}
        const auto restore=qScopeGuard([previous]{for(auto it=previous.cbegin();it!=previous.cend();++it){if(it.value().isValid())QSettings().setValue(it.key(),it.value());else QSettings().remove(it.key());}});
        Window window;auto *hidden=window.findChild<QCheckBox *>("includeHidden");QVERIFY(hidden);QVERIFY(hidden->isChecked());mutations.clear();
        hidden->setChecked(false);QCOMPARE(mutations.size(),1);QCOMPARE(mutations[0]["command"].toString(),QString("configure"));QCOMPARE(mutations[0]["exclusions"].toObject()["include_hidden"].toBool(),false);
        mutations.clear();window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/source"}},{"destination","/tmp/destination"},{"background",true}});
        QCOMPARE(mutations.size(),1);const auto submitted=mutations[0];QCOMPARE(submitted["exclusions"].toObject()["include_hidden"].toBool(),false);
        hidden->setChecked(true);QCOMPARE(submitted["exclusions"].toObject()["include_hidden"].toBool(),false);QVERIFY(QSettings().value("includeHidden").toBool());
        QCOMPARE(SourceUrls::requiredProtocol(submitted),30);
    }

    /// L’option est mémorisée et figée dans chaque nouvelle demande, même en arrière-plan.
    void duplicationOptionIsCapturedForNewRequests() {
        const auto previous=QSettings().value("allowDuplication");QSettings().remove("allowDuplication");
        const auto restore=qScopeGuard([previous]{if(previous.isValid())QSettings().setValue("allowDuplication",previous);else QSettings().remove("allowDuplication");});
        Window window;auto *check=window.findChild<QCheckBox *>("allowDuplication");QVERIFY(check);QVERIFY(!check->isChecked());
        mutations.clear();check->setChecked(true);QCOMPARE(mutations.size(),1);QVERIFY(mutations[0]["allow_duplication"].toBool());
        QVERIFY(QSettings().value("allowDuplication").toBool());
        mutations.clear();window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/source"}},{"destination","/tmp"},{"background",true}});
        QCOMPARE(mutations.size(),1);const auto submitted=mutations[0];QVERIFY(submitted["allow_duplication"].toBool());QCOMPARE(SourceUrls::requiredProtocol(submitted),30);
        check->setChecked(false);QVERIFY(submitted["allow_duplication"].toBool());QVERIFY(!QSettings().value("allowDuplication").toBool());
    }

    /// La perte de focus valide chaque champ ; un refus garde le texte et les règles précédentes.
    void exclusionsTabCommitsOnFocusLossAndTranslates() {
        const QStringList keys={"includeHidden","excludeDirectories","excludeFiles"};QMap<QString,QVariant> previous;
        for(const auto &key:keys){previous.insert(key,QSettings().value(key));QSettings().remove(key);}
        const auto restore=qScopeGuard([previous]{I18n::initialize("fr_FR");for(auto it=previous.cbegin();it!=previous.cend();++it){if(it.value().isValid())QSettings().setValue(it.key(),it.value());else QSettings().remove(it.key());}});
        Window window;auto *dialog=window.findChild<QDialog *>("settingsDialog");QVERIFY(dialog);auto *tabs=dialog->findChild<QTabWidget *>("settingsTabs");tabs->setCurrentIndex(4);dialog->show();mutations.clear();
        QVERIFY(!window.findChild<QDialog *>("exclusionsDialog"));
        auto *directories=dialog->findChild<QPlainTextEdit *>("excludedDirectories"),*files=dialog->findChild<QPlainTextEdit *>("excludedFiles");
        QVERIFY(directories->toPlainText().isEmpty());window.findChild<QCheckBox *>("includeHidden")->setChecked(false);mutations.clear();directories->setPlainText("__*\n\nnode_modules");files->setPlainText("*.tmp\n*~");
        I18n::initialize("en_US");QCOMPARE(tabs->tabText(4),QString("Exclusions"));QCOMPARE(directories->toPlainText(),QString("__*\n\nnode_modules"));
        QVERIFY(!dialog->findChild<QPushButton *>("applyExclusions"));QVERIFY(mutations.isEmpty());
        const auto blur=[](QPlainTextEdit *edit){QFocusEvent event(QEvent::FocusOut,Qt::TabFocusReason);QApplication::sendEvent(edit,&event);};
        rejectExclusions=true;blur(directories);
        QVERIFY(dialog->isVisible());QVERIFY(QSettings().value("excludeDirectories").toStringList().isEmpty());QCOMPARE(directories->toPlainText(),QString("__*\n\nnode_modules"));
        rejectExclusions=false;blur(directories);
        QCOMPARE(files->toPlainText(),QString("*.tmp\n*~"));QVERIFY(QSettings().value("excludeFiles").toStringList().isEmpty());
        blur(files);mutations.clear();blur(files);QVERIFY(mutations.isEmpty());
        QCOMPARE(directories->toPlainText(),QString("__*\nnode_modules"));
        QCOMPARE(QSettings().value("excludeDirectories").toStringList(),QStringList({"__*","node_modules"}));QCOMPARE(QSettings().value("excludeFiles").toStringList(),QStringList({"*.tmp","*~"}));
        mutations.clear();window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/source"}},{"destination","/tmp/destination"},{"background",true}});
        const auto rules=mutations[0]["exclusions"].toObject();QCOMPARE(rules["directories"].toArray(),QJsonArray({"__*","node_modules"}));QCOMPARE(rules["files"].toArray(),QJsonArray({"*.tmp","*~"}));QCOMPARE(rules["include_hidden"].toBool(),false);
    }

    /// Chaque état indique un chemin précis ; les destinations préexistantes ne remplacent pas une source ignorée.
    void pathStates_data() {
        QTest::addColumn<QString>("state");QTest::addColumn<QString>("expected");
        for(const auto &state:QStringList{"waiting","running","verifying","conflict","skipped","cancelled","error","undone","undoing"})
            QTest::newRow(qPrintable(state))<<state<<QString("/tmp/source/a # ; $(ignored).txt");
        QTest::newRow("done")<<QString("done")<<QString("/tmp/destination/a # ; $(ignored).txt");
    }
    void pathStates() {
        QFETCH(QString,state);QFETCH(QString,expected);
        const auto location=RowPaths::entry(file(state));QCOMPARE(location.path,expected);QVERIFY(!location.directory);
    }
    void malformedOrHiddenPathsAreUnavailable() {
        auto entry=file();entry["hidden"]=true;QVERIFY(RowPaths::entry(entry).path.isEmpty());
        entry=file();entry["destination"]="relative";QVERIFY(RowPaths::entry(entry).path.isEmpty());
        QVERIFY(RowPaths::group({{"destination",""}}).path.isEmpty());
    }
    /// L’action lance le résultat et ouvre son parent ; les paramètres spéciaux arrivent inchangés au bureau.
    void realMenuUsesDestinationAndDoesNotMutateTransfers() {
        publish(job({file()}));Window window;selectFile(window);mutations.clear();
        auto *launch=window.findChild<QAction *>("rowLaunchFile");auto *folder=window.findChild<QAction *>("rowOpenFolder");
        QVERIFY(launch->isEnabled());QVERIFY(folder->isEnabled());launch->trigger();folder->trigger();
        QCOMPARE(desktop.urls,QList<QUrl>({QUrl::fromLocalFile(file()["destination"].toString()),QUrl::fromLocalFile("/tmp/destination")}));
        QVERIFY(mutations.isEmpty());
    }
    /// Une restauration survenue depuis le dernier tick ne doit pas ouvrir l’ancienne destination restaurée.
    void clickRefreshesStaleStateBeforeLaunching() {
        publish(job({file()}));Window window;selectFile(window);
        publish(job({file("undone")},"undone"));window.findChild<QAction *>("rowLaunchFile")->trigger();
        QCOMPARE(desktop.urls,QList<QUrl>{QUrl::fromLocalFile(file()["source"].toString())});
    }
    void multipleFilesShareOneFolderAndCannotLaunchTogether() {
        auto other=file();other["source"]="/tmp/source/other.txt";other["destination"]="/tmp/destination/other.txt";
        publish(job({file(),other}));Window window;selectFile(window);window.entryRow(1,1)->setSelected(true);
        QVERIFY(!window.findChild<QAction *>("rowLaunchFile")->isEnabled());
        window.findChild<QAction *>("rowLaunchFile")->trigger();QVERIFY(desktop.urls.isEmpty());
        window.findChild<QAction *>("rowOpenFolder")->trigger();QCOMPARE(desktop.urls,QList<QUrl>{QUrl::fromLocalFile("/tmp/destination")});
    }
    void groupAndDirectoryOpenFoldersButNeverLaunch() {
        auto entry=file();entry["kind"]="directory";entry["destination"]="/tmp/destination/sub";
        publish(job({entry}));Window window;selectFile(window);
        QVERIFY(!window.findChild<QAction *>("rowLaunchFile")->isEnabled());window.findChild<QAction *>("rowOpenFolder")->trigger();
        QCOMPARE(desktop.urls.last(),QUrl::fromLocalFile("/tmp/destination/sub"));
        auto *tree=window.findChild<QTreeWidget *>();tree->clearSelection();tree->setCurrentItem(tree->topLevelItem(0));
        window.findChild<QAction *>("rowOpenFolder")->trigger();QCOMPARE(desktop.urls.last(),QUrl::fromLocalFile("/tmp/destination/sub"));
    }
    /// L’action vise le lot récupérable, sans utiliser la réactivation après annulation.
    void recoveryActionUsesInterruptedJobAndIsUnavailableAfterCancellation() {
        auto value=job({file("error")},"error");value["recovery"]=true;value["resume_pending"]=true;
        publish(value);Window window;selectFile(window);mutations.clear();
        auto *recover=window.findChild<QAction *>("rowRecover");QVERIFY(recover->isEnabled());recover->trigger();
        QCOMPARE(mutations.size(),1);QCOMPARE(mutations[0]["command"].toString(),QString("resume_interrupted"));
        QCOMPARE(mutations[0]["ids"].toArray(),QJsonArray{1});
        value["resume_pending"]=false;value["state"]="cancelled";publish(value);window.refresh();
        QVERIFY(!recover->isEnabled());
    }
    /// La relecture du préfixe possède un libellé propre et respecte l’affichage de pause.
    void recoveryProgressAndPauseArePresentedWithoutRebuildingRow() {
        auto value=file("running");value["resume_checking"]=true;value["resume_checked"]=25;
        value["undo"]=QJsonObject{{"resume",QJsonObject{{"bytes",50}}}};
        publish(job({value},"running"));Window window;auto *item=selectFile(window);
        QCOMPARE(item->text(2),QString("Contrôle de reprise…"));QVERIFY(item->text(3).contains("Reprise"));
        status["paused"]=true;window.refresh();QCOMPARE(item->text(2),QString("En pause"));
        QCOMPARE(window.entryRow(1,0),item);
    }
    /// Le rapport est visible même dans un lot replié et son infobulle reste stable entre deux rafraîchissements.
    void propertyWarningsAreVisibleAndKeepStableTooltips() {
        auto value=file();value["properties"]=QJsonObject{{"status","different"},{"differences",QJsonArray{"ACL d’accès absente","Attribut <b>privé</b> différent"}},{"unavailable",QJsonArray{"Attributs non lisibles"}}};
        publish(job({value}));Window window;auto *item=selectFile(window);
        QVERIFY(item->text(4).contains("Propriétés différentes"));QVERIFY(!item->icon(4).isNull());
        QTextDocument tooltip;tooltip.setHtml(item->toolTip(4));
        QVERIFY(tooltip.toPlainText().contains("ACL d’accès absente"));QVERIFY(item->toolTip(4).contains("&lt;b&gt;"));
        auto *parent=RowTree::owner(item);QVERIFY(parent->text(4).contains("1 avertissement"));QVERIFY(!parent->icon(4).isNull());
        const auto tip=item->toolTip(4);window.refresh();QCOMPARE(item->toolTip(4),tip);
        QCOMPARE(window.entryRow(1,0),item);
        value["properties"]=QJsonObject{{"status","ok"},{"differences",QJsonArray{}},{"unavailable",QJsonArray{}}};
        publish(job({value}));window.refresh();
        QVERIFY(item->text(4).contains("Propriétés visibles identiques"));QVERIFY(item->icon(4).isNull());QVERIFY(parent->icon(4).isNull());
        QVERIFY(!parent->text(4).contains("avertissement"));
    }
    /// L’option se transmet aux nouveaux lots et une commande de configuration la synchronise avec la case.
    void propertyOptionIsCapturedInEnqueueAndConfiguresCheckbox() {
        publish(job({file()}));Window window;auto *check=window.findChild<QCheckBox *>("verifyProperties");QVERIFY(check);
        mutations.clear();window.handle({{"command","configure"},{"verify_properties",true}});QVERIFY(check->isChecked());
        window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/source"}},{"destination","/tmp/target"}});
        QCOMPARE(mutations.last()["verify_properties"].toBool(),true);
        window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/source"}},{"destination","/tmp/target"},{"verify_properties",false}});
        QCOMPARE(mutations.last()["verify_properties"].toBool(),false);
        window.handle({{"command","configure"},{"verify_properties",false}});QVERIFY(!check->isChecked());
    }
    /// Une capacité inconnue s’affiche en avertissement et laisse le lot terminer en OK.
    void unavailableSpaceIsAVisibleWarningRatherThanAFailedJob() {
        auto value=job({file()});value["space_warning"]="Espace disponible non vérifiable";
        publish(value);Window window;auto *parent=window.findChild<QTreeWidget *>()->topLevelItem(0);
        QVERIFY(parent->text(4).contains("Espace disponible non vérifiable"));QVERIFY(!parent->icon(4).isNull());
        QCOMPARE(parent->data(0,Qt::UserRole+3).toString(),QString("done"));
        const auto tip=parent->toolTip(4);window.refresh();QCOMPARE(parent->toolTip(4),tip);
        value["space_warning"]="";publish(value);window.refresh();QVERIFY(parent->icon(4).isNull());
    }
    /// L’option mémorisée s’applique aussi aux collages et peut être remplacée explicitement.
    void spaceOptionFollowsConfigureAndEnqueue() {
        publish(job({file()}));Window window;auto *check=window.findChild<QCheckBox *>("verifySpace");QVERIFY(check);
        window.handle({{"command","configure"},{"verify_space",true}});QVERIFY(check->isChecked());
        window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/source"}},{"destination","/tmp/target"}});
        QCOMPARE(mutations.last()["verify_space"].toBool(),true);
        window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/source"}},{"destination","/tmp/target"},{"verify_space",false}});
        QCOMPARE(mutations.last()["verify_space"].toBool(),false);
        QCOMPARE(SourceUrls::requiredProtocol(QJsonObject{{"verify_space",true}}),14);
        window.handle({{"command","configure"},{"verify_space",false}});QVERIFY(!check->isChecked());
    }
    /// Les actions suivent la sélection visible, y compris ses groupes et ses filtres.
    void priorityAvailabilityForMultipleSiblingSelections() {
        QJsonArray entries;
        for(int i=0;i<4;++i) {
            auto entry=file("waiting");entry["source"]=QString("/tmp/source/file%1.txt").arg(i);entry["destination"]=QString("/tmp/target/file%1.txt").arg(i);entries.append(entry);
        }
        publish(job(entries,"waiting"));Window window;auto *tree=window.findChild<QTreeWidget *>();
        const QList<QList<int>> cases{{0},{3},{1,2},{0,1,2,3},{0,3}};
        const QList<QPair<bool,bool>> expected{{false,true},{true,false},{true,true},{false,false},{true,true}};
        for(int i=0;i<cases.size();++i) {
            {QSignalBlocker blocker(tree);tree->clearSelection();for(int index:cases[i])window.entryRow(1,index)->setSelected(true);}
            window.updateRowActions();
            QCOMPARE(window.findChild<QAction *>("rowUp")->isEnabled(),expected[i].first);
            QCOMPARE(window.findChild<QAction *>("rowDown")->isEnabled(),expected[i].second);
        }
        // Qt omet les lignes cachées de selectedItems ; leur sélection mémorisée n’est pas une nouvelle cible.
        {QSignalBlocker blocker(tree);tree->clearSelection();window.entryRow(1,0)->setSelected(true);window.entryRow(1,3)->setSelected(true);window.entryRow(1,3)->setHidden(true);}
        QCOMPARE(tree->selectedItems().size(),1);window.updateRowActions();
        QVERIFY(!window.findChild<QAction *>("rowUp")->isEnabled());
        QVERIFY(window.findChild<QAction *>("rowDown")->isEnabled());
    }

    /// Une priorité ne remplace aucun widget : sélection, branche ouverte et fichier courant restent stables.
    void priorityReordersOnlyRootsAndKeepsFileSelection() {
        auto entries=nestedEntries();for(int i=0;i<entries.size();++i){auto entry=entries[i].toObject();entry["state"]="waiting";entries[i]=entry;}
        auto first=job(entries,"waiting");auto second=first;second["id"]=2;auto third=first;third["id"]=3;
        status["jobs"]=QJsonArray{first,second,third};status["revision"]=1;status["reset"]=true;status["job_ids"]=QJsonArray{1,2,3};
        Window window;auto *tree=window.findChild<QTreeWidget *>();auto *root=window.entryRow(3,0);
        root->setExpanded(true);auto *folder=window.entryRow(3,1);folder->setExpanded(true);
        auto *leaf=window.entryRow(3,2);tree->setCurrentItem(leaf);leaf->setSelected(true);
        const auto tooltip=leaf->toolTip(0);window.updateRowActions();
        auto *next=window.findChild<QAction *>("rowNext");QVERIFY(next->isEnabled());mutations.clear();next->trigger();
        QCOMPARE(mutations.size(),1);QCOMPARE(mutations.last()["command"].toString(),QString("reorder_rows"));
        QCOMPARE(mutations.last()["rows"].toArray(),QJsonArray{QJsonObject({{"job",3},{"index",2}})});
        QJsonArray order;for(int id:{3,1,2})for(int i=0;i<5;++i)order.append(QJsonObject{{"job",id},{"index",i}});
        status["revision"]=2;status["reset"]=false;status["jobs"]=QJsonArray{};status["row_order"]=order;
        window.refresh();QCOMPARE(tree->topLevelItem(0),root);QCOMPARE(window.entryRow(3,2),leaf);
        QVERIFY(root->isExpanded());QVERIFY(folder->isExpanded());QVERIFY(leaf->isSelected());QCOMPARE(tree->currentItem(),leaf);QCOMPARE(leaf->toolTip(0),tooltip);
    }
    /// Plusieurs feuilles d’un lot ne produisent qu’un identifiant ; le tri suit la file visible.
    void priorityDeduplicatesSelectedFilesAndRejectsActiveOwners() {
        auto value=job(nestedEntries(),"running");publish(value);Window window;auto *tree=window.findChild<QTreeWidget *>();
        window.entryRow(1,1)->setSelected(true);window.entryRow(1,2)->setSelected(true);window.updateRowActions();
        // Ce dossier est déjà préparé : sa branche active ne peut pas être déplacée.
        QVERIFY(!window.findChild<QAction *>("rowNext")->isEnabled());
        tree->clearSelection();tree->setCurrentItem(window.entryRow(1,3));window.updateRowActions();mutations.clear();
        window.findChild<QAction *>("rowNext")->trigger();QCOMPARE(mutations.last()["rows"].toArray(),QJsonArray{QJsonObject({{"job",1},{"index",3}})});
    }
    /// La demande devient une couleur commune, sans ligne supplémentaire au-dessus des fichiers.
    void flatRowsHaveRequestColorsAndSelectWholeRequest() {
        const auto restoreLanguage=qScopeGuard([]{I18n::initialize("fr_FR");});I18n::initialize("fr_FR");
        auto a=file("waiting"),b=a,c=a;b["source"]="/tmp/source/b";b["destination"]="/tmp/destination/b";
        auto first=job({a,b},"waiting"),second=job({c},"waiting");second["id"]=2;
        status["jobs"]=QJsonArray{first,second};Window window;auto *tree=window.findChild<QTreeWidget *>();
        QCOMPARE(tree->topLevelItemCount(),3);auto *row=window.entryRow(1,0);
        QVERIFY(!row->parent());QVERIFY(!RowTree::owner(row)->treeWidget());
        QCOMPARE(row->background(0),window.entryRow(1,1)->background(0));QVERIFY(row->background(0)!=window.entryRow(2,0)->background(0));
        tree->setCurrentItem(row);window.updateRowActions();
        auto *selectBatch=window.findChild<QAction *>("rowSelectRequest");QVERIFY(selectBatch);
        QCOMPARE(selectBatch->text(),QString("Sélectionner le lot entier"));
        I18n::initialize("en_US");QCOMPARE(selectBatch->text(),QString("Select entire batch"));
        I18n::initialize("fr_FR");QCOMPARE(selectBatch->text(),QString("Sélectionner le lot entier"));
        selectBatch->trigger();
        QVERIFY(window.entryRow(1,0)->isSelected());QVERIFY(window.entryRow(1,1)->isSelected());QVERIFY(!window.entryRow(2,0)->isSelected());
        const auto before=row->background(0);QPalette dark=window.palette();dark.setColor(QPalette::Base,QColor("#202020"));window.setPalette(dark);window.refresh();
        QVERIFY(row->background(0)!=before);QVERIFY(row->background(0).color().lightness()<80);
    }
    /// Le dépôt interne émet une priorité ; il ne laisse pas Qt modifier la parenté ou les destinations.
    void internalDropUsesStableReferencesAndForeignDragsAreIgnored() {
        auto first=job({file("waiting")},"waiting"),second=first;second["id"]=2;
        status["jobs"]=QJsonArray{first,second};Window window;window.show();QTest::qWait(20);
        auto *tree=dynamic_cast<TransferTree *>(window.findChild<QTreeWidget *>());QVERIFY(tree);
        auto *source=window.entryRow(1,0),*target=window.entryRow(2,0);tree->setCurrentItem(source);source->setSelected(true);
        std::unique_ptr<QMimeData> mime(tree->selectionMime());
        const QPoint point=tree->visualItemRect(target).bottomLeft()+QPoint(15,-1);
        QDragEnterEvent enter(point,Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(tree->viewport(),&enter);QVERIFY(enter.isAccepted());
        QDragMoveEvent move(point,Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(tree->viewport(),&move);
        mutations.clear();QDropEvent drop(point,Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(tree->viewport(),&drop);
        QVERIFY(drop.isAccepted());QCOMPARE(mutations.size(),1);QCOMPARE(mutations.last()["command"].toString(),QString("reorder_rows"));
        QCOMPARE(mutations.last()["rows"].toArray(),QJsonArray{QJsonObject({{"job",1},{"index",0}})});
        QCOMPARE(mutations.last()["target"].toObject(),QJsonObject({{"job",2},{"index",0}}));QCOMPARE(mutations.last()["action"].toString(),QString("after"));
        QVERIFY(!source->parent());QVERIFY(!target->parent());QCOMPARE(source->data(0,Qt::UserRole+4).toString(),file()["source"].toString());
        QMimeData foreign;foreign.setData("application/x-gad-rows",R"({"origin":"foreign","rows":[]})");
        QDragEnterEvent other(point,Qt::MoveAction,&foreign,Qt::LeftButton,Qt::NoModifier);mutations.clear();QApplication::sendEvent(tree->viewport(),&other);
        QVERIFY(!other.isAccepted());QVERIFY(mutations.isEmpty());
    }
    /// Le résumé d’un dossier ne doit pas reprendre les avertissements d’une racine voisine de même couleur.
    void folderWarningsSummarizeOnlyTheirOwnBranch() {
        auto entries=nestedEntries();for(int i=0;i<entries.size();++i){auto e=entries[i].toObject();e["state"]="done";entries[i]=e;}
        const QJsonObject report{{"status","different"},{"differences",QJsonArray{"Permission différente"}}};
        auto outside=entries[4].toObject();outside["properties"]=report;entries[4]=outside;publish(job(entries));Window window;
        QVERIFY(window.entryRow(1,0)->icon(4).isNull());QVERIFY(!window.entryRow(1,4)->icon(4).isNull());
        auto inside=entries[2].toObject();inside["properties"]=report;entries[2]=inside;publish(job(entries));window.refresh();
        QVERIFY(!window.entryRow(1,0)->icon(4).isNull());QVERIFY(window.entryRow(1,0)->text(4).contains("1 avertissement"));
        QVERIFY(!window.entryRow(1,1)->icon(4).isNull());QVERIFY(window.entryRow(1,1)->text(4).contains("1 avertissement"));
    }
    /// L’imbrication visuelle reflète les dossiers et les noms courts gardent un chemin complet en infobulle.
    void nestedFoldersContainTheirFiles() {
        publish(job(nestedEntries(),"running"));Window window;
        auto *tree=window.findChild<QTreeWidget *>();auto *group=tree->topLevelItem(0);
        QCOMPARE(tree->topLevelItemCount(),2);QCOMPARE(group,window.entryRow(1,0));QCOMPARE(window.entryRow(1,1)->parent(),window.entryRow(1,0));
        QCOMPARE(window.entryRow(1,2)->parent(),window.entryRow(1,1));QCOMPARE(window.entryRow(1,3)->parent(),window.entryRow(1,0));
        QVERIFY(!window.entryRow(1,4)->parent());QCOMPARE(window.entryRow(1,0)->text(0),QString("root"));
        QCOMPARE(window.entryRow(1,2)->text(0),QString("a.txt"));
        QTextDocument tip;tip.setHtml(window.entryRow(1,2)->toolTip(0));QVERIFY(tip.toPlainText().startsWith("/tmp/source/root/sub/a.txt"));QVERIFY(!tip.toPlainText().contains("Demande #1"));QVERIFY(!tip.toPlainText().contains("\nCopie"));QCOMPARE(window.entryRow(1,2)->toolTip(TableView::OrderColumn).section('\n',0,0),QString("Lot #1"));
        QVERIFY(window.entryRow(1,0)->isExpanded());QVERIFY(window.entryRow(1,1)->isExpanded());
    }
    /// La progression ne reconstruit pas les branches et ne rouvre pas un dossier replié manuellement.
    void nestedRowsKeepSelectionIndexesAndExpansionDuringProgress() {
        auto entries=nestedEntries();publish(job(entries,"running"));Window window;
        auto *tree=window.findChild<QTreeWidget *>();auto *leaf=window.entryRow(1,3);auto *folder=window.entryRow(1,1);
        folder->setExpanded(false);tree->setCurrentItem(leaf);leaf->setSelected(true);
        QPersistentModelIndex index=tree->indexFromItem(leaf);const auto tooltip=leaf->toolTip(0);
        auto changed=entries[2].toObject();changed["state"]="running";changed["copied"]=2;entries[2]=changed;
        publish(job(entries,"running"));window.refresh();
        QCOMPARE(window.entryRow(1,3),leaf);QVERIFY(index.isValid());QVERIFY(leaf->isSelected());QVERIFY(!folder->isExpanded());QCOMPARE(leaf->toolTip(0),tooltip);
    }
    /// Une feuille profonde utilise son indice moteur ; sélectionner son dossier évite les doublons.
    void nestedSelectionUsesEngineIndexAndDeduplicatesAncestors() {
        publish(job(nestedEntries(),"running"));Window window;auto *tree=window.findChild<QTreeWidget *>();
        tree->setCurrentItem(window.entryRow(1,2));
        auto request=window.selectionRequest("cancel_selection");QCOMPARE(request["entries"].toArray(),QJsonArray{QJsonObject({{"job",1},{"index",2}})});
        window.entryRow(1,1)->setSelected(true);request=window.selectionRequest("cancel_selection");
        QCOMPARE(request["entries"].toArray(),QJsonArray{QJsonObject({{"job",1},{"index",1}})});
        tree->topLevelItem(0)->setSelected(true);request=window.selectionRequest("cancel_selection");
        QVERIFY(request["jobs"].toArray().isEmpty());QCOMPARE(request["entries"].toArray(),QJsonArray{QJsonObject({{"job",1},{"index",0}})});
    }
    /// Les actions d’ouverture et de pause utilisent une feuille profonde, sans confondre son parent avec le lot.
    void nestedMenuTargetsTheActualFile() {
        publish(job(nestedEntries(),"running"));Window window;auto *tree=window.findChild<QTreeWidget *>();
        tree->setCurrentItem(window.entryRow(1,2));window.updateRowActions();mutations.clear();
        window.findChild<QAction *>("rowOpenFolder")->trigger();QCOMPARE(desktop.urls.last(),QUrl::fromLocalFile("/tmp/source/root/sub"));
        window.findChild<QAction *>("rowLaunchFile")->trigger();QCOMPARE(desktop.urls.last(),QUrl::fromLocalFile("/tmp/source/root/sub/a.txt"));
        window.findChild<QAction *>("rowPause")->trigger();QCOMPARE(mutations.last()["command"].toString(),QString("pause_selection"));
        QCOMPARE(mutations.last()["entries"].toArray(),QJsonArray{QJsonObject({{"job",1},{"index",2}})});
    }
    /// Un filtre garde les ancêtres d’un fichier qui correspond, sans rendre ses voisins visibles.
    void nestedFilterRetainsParentContext() {
        auto entries=nestedEntries();auto done=entries[2].toObject();done["state"]="done";entries[2]=done;
        publish(job(entries,"running"));Window window;auto *filter=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());
        window.entryRow(1,0)->setExpanded(false);filter->setStateFilter("done");
        QVERIFY(!window.entryRow(1,2)->isHidden());QVERIFY(!window.entryRow(1,1)->isHidden());QVERIFY(!window.entryRow(1,0)->isHidden());
        QVERIFY(window.entryRow(1,0)->isExpanded());QVERIFY(window.entryRow(1,3)->isHidden());QVERIFY(window.entryRow(1,4)->isHidden());
        auto *leaf=window.entryRow(1,2);filter->clearColumnFilter(2);
        QCOMPARE(window.entryRow(1,2),leaf);QVERIFY(!window.entryRow(1,3)->isHidden());
    }
    /// Un dossier terminé se replie une seule fois et récupère la sélection d’un fichier désormais caché.
    void completedNestedFolderCollapsesOnce() {
        auto entries=nestedEntries();publish(job(entries,"running"));Window window;auto *tree=window.findChild<QTreeWidget *>();
        tree->setCurrentItem(window.entryRow(1,2));
        for(const int i:{1,2}){auto entry=entries[i].toObject();entry["state"]="done";entries[i]=entry;}
        publish(job(entries,"running"));window.refresh();auto *folder=window.entryRow(1,1);
        QCOMPARE(folder->text(2),QString("OK"));QVERIFY(!folder->isExpanded());QVERIFY(folder->isSelected());
        QVERIFY(!window.entryRow(1,2)->isSelected());QVERIFY(window.entryRow(1,0)->isExpanded());
        folder->setExpanded(true);window.refresh();QVERIFY(folder->isExpanded());
    }
    /// Une réanalyse plus courte supprime ses anciennes branches sans conserver de pointeurs pendants.
    void shrinkingNestedHistoryKeepsSurvivingRowSafe() {
        publish(job(nestedEntries(),"running"));Window window;auto *survivor=window.entryRow(1,0);
        QPersistentModelIndex removed=window.findChild<QTreeWidget *>()->indexFromItem(window.entryRow(1,2));
        publish(job({file("waiting")},"waiting"));window.refresh();
        QCOMPARE(window.entryRow(1,0),survivor);QVERIFY(!removed.isValid());QVERIFY(!window.entryRow(1,1));
        QCOMPARE(survivor->childCount(),0);QVERIFY(!survivor->parent());QCOMPARE(window.findChild<QTreeWidget *>()->topLevelItemCount(),1);
    }
    /// Une racine sélectionnée séparément et un nom de dossier au préfixe similaire restent indépendants.
    void nestedRootsRequireBothSourceAndDestinationAncestry() {
        auto entries=nestedEntries();auto outside=file("waiting");outside["source"]="/tmp/source/root-other/file.txt";entries.append(outside);
        auto separate=file("waiting");separate["source"]="/tmp/source/root/sub/explicit.txt";separate["destination"]="/tmp/destination/explicit.txt";entries.append(separate);
        publish(job(entries,"running"));Window window;
        QVERIFY(!window.entryRow(1,5)->parent());QVERIFY(!window.entryRow(1,6)->parent());
    }
    /// Le lot récupérable est trouvé en remontant tous les dossiers d’une feuille profonde.
    void deepRecoveryUsesJobHeader() {
        auto entries=nestedEntries();for(int i=0;i<entries.size();++i){auto entry=entries[i].toObject();entry["state"]="error";entries[i]=entry;}
        auto value=job(entries,"error");value["recovery"]=true;value["resume_pending"]=true;publish(value);Window window;
        window.findChild<QTreeWidget *>()->setCurrentItem(window.entryRow(1,2));window.updateRowActions();mutations.clear();
        auto *action=window.findChild<QAction *>("rowRecover");QVERIFY(action->isEnabled());action->trigger();
        QCOMPARE(mutations.last()["ids"].toArray(),QJsonArray{1});
    }
    /// Voir le conflit doit ouvrir chaque ancêtre et retrouver le fichier même sous un filtre.
    void conflictNavigationFindsDeepEntry() {
        auto entries=nestedEntries();auto conflict=entries[2].toObject();conflict["state"]="conflict";entries[2]=conflict;
        publish(job(entries,"running"));Window window;auto *filter=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());
        filter->setStateFilter("done");window.entryRow(1,0)->setExpanded(false);
        static_cast<TransferTree *>(window.findChild<QTreeWidget *>())->setColumnFilter(0,"does not match any file");
        window.findChild<QPushButton *>("locateConflict")->click();
        QCOMPARE(window.findChild<QTreeWidget *>()->currentItem(),window.entryRow(1,2));
        QVERIFY(window.entryRow(1,0)->isExpanded());QVERIFY(window.entryRow(1,1)->isExpanded());QVERIFY(filter->presentation().state.isEmpty());
    }
    /// Réactiver un dossier trouve aussi les lignes annulées dans ses sous-dossiers.
    void nestedCancelledDescendantEnablesDirectoryRetry() {
        auto entries=nestedEntries();for(int i=0;i<entries.size();++i){auto entry=entries[i].toObject();entry["state"]=i==2?"cancelled":"error";entries[i]=entry;}
        publish(job(entries,"error"));Window window;window.findChild<QTreeWidget *>()->setCurrentItem(window.entryRow(1,0));window.updateRowActions();mutations.clear();
        auto *action=window.findChild<QAction *>("rowRetry");QVERIFY(action->isEnabled());action->trigger();
        QCOMPARE(mutations.last()["command"].toString(),QString("retry_selection"));QCOMPARE(mutations.last()["entries"].toArray(),QJsonArray{QJsonObject({{"job",1},{"index",0}})});
    }
    /// Une réactivation ajoute parfois un fichier après une autre racine : son parent est retrouvé par chemin.
    void appendedDescendantReturnsToItsExistingFolder() {
        auto entries=nestedEntries();publish(job(entries,"running"));Window window;auto *folder=window.entryRow(1,1);
        auto late=file("waiting");late["source"]="/tmp/source/root/sub/late.txt";late["destination"]="/tmp/destination/root/sub/late.txt";entries.append(late);
        publish(job(entries,"running"));window.refresh();QCOMPARE(window.entryRow(1,5)->parent(),folder);QCOMPARE(window.entryRow(1,1),folder);
        auto *filter=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());late["state"]="done";entries[5]=late;publish(job(entries,"running"));
        filter->setStateFilter("done");QVERIFY(!folder->isHidden());QVERIFY(!window.entryRow(1,0)->isHidden());QVERIFY(!window.entryRow(1,5)->isHidden());
    }
    /// Même un historique dont le parent apparaît plus tard reste navigable sans dépendre des rangs Qt.
    void laterParentStillOwnsItsChild() {
        auto entries=nestedEntries();const auto leaf=entries.takeAt(2);entries.prepend(leaf);
        publish(job(entries,"running"));Window window;
        QCOMPARE(window.entryRow(1,0)->parent(),window.entryRow(1,2));QCOMPARE(window.entryRow(1,2)->parent(),window.entryRow(1,1));
        auto *filter=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());filter->setStateFilter("waiting");
        QVERIFY(!window.entryRow(1,0)->isHidden());QVERIFY(!window.entryRow(1,1)->isHidden());QVERIFY(!window.entryRow(1,2)->isHidden());
    }
    void removedLineOrEmptySelectionDoesNotOpenAnything() {
        publish(job({file()}));Window window;
        QVERIFY(!window.findChild<QAction *>("rowLaunchFile")->isEnabled());QVERIFY(!window.findChild<QAction *>("rowOpenFolder")->isEnabled());
        selectFile(window);status["jobs"]=QJsonArray{};
        window.findChild<QAction *>("rowLaunchFile")->trigger();QVERIFY(desktop.urls.isEmpty());
        QVERIFY(!window.findChild<QAction *>("rowOpenFolder")->isEnabled());
    }
};
QTEST_MAIN(RowActionsTest)
#include "row_actions_test.moc"
