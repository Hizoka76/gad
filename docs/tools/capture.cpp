// Capture de la véritable fenêtre Gad, avec un moteur de démonstration et des fichiers temporaires.
// Aucun serveur local, presse-papiers réel, transfert, notification Plasma ou programme externe n’est utilisé.
#include "window.h"
#include <QtWidgets>
#include <QtCore>
#include <cstdlib>
#include <cstring>

// Les effets de fin de file sont toujours simulés dans l’outil de capture, même si le délai expire.
class CaptureWindow:public Window {
public:
    void executeCompletion(const QString &,std::function<void(QJsonObject)> reply) override {reply({{"ok",true}});}
    void completionNotice(const QString &,const QString &) override {}
};
static QJsonObject demoReport;
static QJsonObject demoState{{"jobs",QJsonArray{}},{"paused",false},{"keep_history",true},{"verify_md5",false}};
/// Le double fournit des états figés ; il n’exécute aucune opération sur les chemins affichés.
extern "C" char *sc_request(const char *input) {
    const auto request=QJsonDocument::fromJson(input).object();const auto command=request["command"].toString();
    QJsonObject result{{"ok",true}};
    if(command=="error_report")result=demoReport;
    else if(command=="status" || command=="poll")result=demoState;
    else if(command=="pause")demoState["paused"]=request["paused"];
    else if(command=="configure")for(const auto *name:{"keep_history","verify_md5","verify_properties"})if(request.contains(name))demoState[name]=request[name];
    return strdup(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}
extern "C" void sc_free(char *value){free(value);}

/// Les tailles sont réelles, les positions de progression sont des données de démonstration.
static void fixture(const QString &path,qint64 size) {
    QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);
    if(!file.open(QIODevice::WriteOnly) || !file.resize(size))qFatal("Fixture de documentation impossible");
    file.setFileTime(QDateTime(QDate(2026,11,4),QTime(10,30),QTimeZone::UTC),QFileDevice::FileModificationTime);
}
static QJsonObject entry(const QString &source,const QString &destination,const QString &kind,const QString &state,qint64 size=0,qint64 copied=0) {
    return {{"source",source},{"destination",destination},{"kind",kind},{"state",state},{"size",size},{"copied",copied},{"verified",0},{"error",""}};
}
static QJsonObject job(qint64 id,const QString &source,const QString &destination,const QString &state,const QJsonArray &entries,const QString &mode="copy",bool md5=false) {
    return {{"created_at",QDateTime(QDate(2026,11,7),QTime(12,0)).toSecsSinceEpoch()},{"id",id},{"sources",QJsonArray{source}},{"destination",destination},{"state",state},{"entries",entries},{"mode",mode},
            {"verify_md5",md5},{"journaled",true},{"paused",false},{"error",""}};
}
/// Enregistre directement les pixels de Qt et les informations utiles à leur vérification.
static void save(QWidget *widget,const QString &path,QJsonArray &manifest,const QRect &region=QRect()) {
    // Laisser finir les animations du thème et les fermetures de dialogues avant de capturer les pixels.
    QEventLoop settled;QTimer::singleShot(180,&settled,&QEventLoop::quit);settled.exec();
    qApp->processEvents();const auto image=region.isNull() ? widget->grab() : widget->grab(region);
    if(image.isNull() || !image.save(path))qFatal("Capture impossible");
    manifest.append(QJsonObject{{"file",QFileInfo(path).fileName()},{"width",image.width()},{"height",image.height()}});
}
int main(int argc,char **argv) {
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message){std::fprintf(stderr,"%s\n",qPrintable(message));});
    QApplication app(argc,argv);app.setOrganizationName("GadDocumentation");app.setApplicationName("GadDocumentation");
    app.setQuitOnLastWindowClosed(false); // Style, palette, icônes et police fournis par le thème KDE courant.
    if(argc!=3)return 2;
    const auto language=QString::fromLocal8Bit(argv[2]);QSettings().setValue("language",language);
    applicationIcon();I18n::initialize(language);
    if(app.palette().color(QPalette::Window).lightness()<180)qFatal("A light capture palette is required");
    const QString output=QString::fromLocal8Bit(argv[1]);QDir().mkpath(output);
    QTemporaryDir data(QDir::tempPath()+"/gad-XXXXXX");if(!data.isValid())return 2;
    const auto source=data.path(),destination=data.path()+"/Copies";
    QDir().mkpath(destination);
    for(const auto &name:QStringList{"mer.jpg","montagne.jpg","famille.jpg"})fixture(source+"/Photos/"+name,8*1024*1024);
    fixture(source+"/Documents/bilan.pdf",2*1024*1024);fixture(source+"/Archives/notes.txt",4096);
    fixture(destination+"/Archives/notes.txt",4096);
    const auto photos=source+"/Photos",target=destination+"/Photos";
    const auto first=job(1,photos,destination,"running",{
        entry(photos,target,"directory","running"),
        entry(photos+"/mer.jpg",target+"/mer.jpg","file","done",8*1024*1024,8*1024*1024),
        entry(photos+"/montagne.jpg",target+"/montagne.jpg","file","running",8*1024*1024,3*1024*1024),
        entry(photos+"/famille.jpg",target+"/famille.jpg","file","waiting",8*1024*1024,0)});
    const auto queued=job(2,source+"/Documents",destination,"waiting",{
        entry(source+"/Documents",destination+"/Documents","directory","waiting"),
        entry(source+"/Documents/bilan.pdf",destination+"/Documents/bilan.pdf","file","waiting",2*1024*1024,0)},"move");
    const auto completed=job(3,source+"/Archives",destination,"done",{
        entry(source+"/Archives",destination+"/Archives","directory","done"),
        entry(source+"/Archives/notes.txt",destination+"/Archives/notes.txt","file","done",4096,4096)});
    demoState["jobs"]=QJsonArray{first,queued,completed};
    CaptureWindow window;window.resize(1240,620);window.findChild<QLineEdit *>("destinationInput")->setText(destination);window.show();app.processEvents();
    auto *tree=window.findChild<QTreeWidget *>();tree->setColumnWidth(0,305);tree->setColumnWidth(1,320);tree->setColumnWidth(2,140);tree->setColumnWidth(3,160);
    tree->topLevelItem(0)->setExpanded(true);tree->setCurrentItem(window.entryRow(1,2));
    QJsonArray manifest;
    save(&window,output+"/01-vue-ensemble.png",manifest);
    auto *settingsButton=window.findChild<QToolButton *>("settingsButton");
    settingsButton->click();app.processEvents();
    auto *settings=window.findChild<QDialog *>("settingsDialog");
    save(settings,output+"/02-sources-options.png",manifest);
    auto *settingsTabs=settings->findChild<QTabWidget *>("settingsTabs");settingsTabs->setCurrentIndex(1);app.processEvents();
    save(settings,output+"/14-options-raccourcis.png",manifest);settingsTabs->setCurrentIndex(0);settings->hide();

    window.handle({{"command","pause"},{"paused",true}});save(&window,output+"/03-pause.png",manifest);
    window.handle({{"command","pause"},{"paused",false}});
    auto *cancel=window.findChild<QPushButton *>("cancellationMenu");cancel->menu()->popup(window.mapToGlobal(QPoint(950,170)));
    save(cancel->menu(),output+"/04-menu-annulation.png",manifest);cancel->menu()->hide();
    auto *context=window.findChild<QMenu *>("rowContextMenu");window.updateRowActions();context->popup(window.mapToGlobal(QPoint(500,320)));
    save(context,output+"/05-menu-contextuel.png",manifest);context->hide();

    auto *filter=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());auto *headerMenu=filter->headerMenu(2);headerMenu->popup(QPoint(100,100));app.processEvents();
    save(headerMenu,output+"/06-filtres.png",manifest);headerMenu->close();
    auto verified=first;verified["verify_md5"]=true;auto verification=first["entries"].toArray();
    auto active=verification[2].toObject();active["state"]="verifying";active["copied"]=8*1024*1024;active["verified"]=10*1024*1024;verification[2]=active;
    auto validated=verification[1].toObject();validated["md5_source"]="0123456789abcdef0123456789abcdef";validated["md5_destination"]=QString("0123456789abcdef0123456789abcdef");verification[1]=validated;
    verified["entries"]=verification;demoState["jobs"]=QJsonArray{verified,queued,completed};window.findChild<QCheckBox *>("verifyMd5")->setChecked(true);window.refresh();
    save(&window,output+"/07-verification-md5.png",manifest);

    fixture(source+"/Documents/bilan.pdf",2*1024*1024);fixture(destination+"/Documents/bilan.pdf",512*1024);
    auto conflict=job(4,source+"/Documents/bilan.pdf",destination,"running",{
        entry(source+"/Documents/bilan.pdf",destination+"/Documents/bilan.pdf","file","conflict",2*1024*1024,0)});
    demoState["jobs"]=QJsonArray{conflict,queued,completed};window.refresh();
    // Les lignes sont retrouvées par leur état plutôt que par leur position : l’historique est conservé par la vraie vue.
    for(int i=0;i<tree->topLevelItemCount();++i)if(tree->topLevelItem(i)->data(0,Qt::UserRole).toLongLong()==4){tree->topLevelItem(i)->setExpanded(true);tree->setCurrentItem(window.entryRow(4,0));}
    save(&window,output+"/08-conflit.png",manifest);

    auto *tray=window.findChild<QSystemTrayIcon *>("transferTray");tray->contextMenu()->popup(window.mapToGlobal(QPoint(600,200)));
    save(tray->contextMenu(),output+"/09-menu-zone-notification.png",manifest);tray->contextMenu()->hide();
    window.showAbout();app.processEvents();auto *about=window.findChild<QMessageBox *>("aboutDialog");
    save(about,output+"/10-a-propos.png",manifest);about->close();
    // Rapport de propriétés simulé dans les vrais widgets, avec un avertissement visible sur le lot repliable.
    auto checkedEntry=entry(source+"/Documents/bilan.pdf",destination+"/Documents/bilan.pdf","file","done",2*1024*1024,2*1024*1024);
    checkedEntry["properties"]=QJsonObject{{"status","different"},{"differences",QJsonArray{I18n::message("Access ACL")+": "+I18n::message("different"),QString("user.comment: ")+I18n::message("additional at destination")}},{"unavailable",QJsonArray{}}};
    auto checkedJob=job(5,source+"/Documents/bilan.pdf",destination,"done",{checkedEntry});checkedJob["verify_properties"]=true;
    demoState["jobs"]=QJsonArray{checkedJob};window.findChild<QCheckBox *>("verifyProperties")->setChecked(true);window.refresh();
    tree->setColumnWidth(0,240);tree->setColumnWidth(1,250);tree->setColumnWidth(2,100);tree->setColumnWidth(3,150);
    tree->topLevelItem(0)->setExpanded(true);tree->setCurrentItem(window.entryRow(5,0));
    save(&window,output+"/11-verification-proprietes.png",manifest);
    auto disconnectedEntry=entry(source+"/Photos/montagne.jpg",destination+"/Photos/montagne.jpg","file","device_waiting",8*1024*1024,3*1024*1024);
    disconnectedEntry["error"]="Device unavailable: waiting for reconnection";
    auto disconnected=job(6,source+"/Photos",destination,"device_waiting",{disconnectedEntry});
    demoState["paused"]=false;demoState["jobs"]=QJsonArray{disconnected};window.refresh();
    tree->setColumnWidth(0,240);tree->setColumnWidth(1,250);tree->setColumnWidth(2,190);tree->setColumnWidth(3,150);
    save(&window,output+"/12-reconnexion.png",manifest);
    auto errorA=entry(source+"/Photos/montagne.jpg",destination+"/Photos/montagne.jpg","file","error",8*1024*1024,3*1024*1024);
    errorA["error"]="Access denied";
    auto errorB=entry(source+"/Documents/bilan.pdf",destination+"/Documents/bilan.pdf","file","error",2*1024*1024,0);
    errorB["error"]="Source changed";
    auto errors=job(7,source+"/Photos/montagne.jpg",destination,"error",{errorA,errorB});
    demoState["jobs"]=QJsonArray{errors};window.refresh();
    QJsonArray records;int errorIndex=0;
    for(const auto &value:QJsonArray{errorA,errorB}){auto record=value.toObject();record["job"]=7;record["index"]=errorIndex++;record["scope"]="entry";record["mode"]="copy";record["retryable"]=true;records.append(record);}
    demoReport={{"format","gad-error-report"},{"version",1},{"generated_unix",QDateTime::currentSecsSinceEpoch()},{"records",records},{"jobs",QJsonArray{errors}}};
    window.showErrorReport();app.processEvents();auto *report=window.findChild<QDialog *>("errorReportDialog");report->resize(1320,650);
    auto *reportRows=report->findChild<QTreeWidget *>("errorReportRows");reportRows->setCurrentItem(reportRows->topLevelItem(0));
    save(report,output+"/13-bilan-erreurs.png",manifest);report->close();
    // Profils de démonstration dans le compte de capture uniquement ; aucun secret ni transfert lancé.
    auto photoProfile=window.currentTransferProfile();photoProfile.name="Photos";photoProfile.rate=2621440;
    photoProfile.options["exclusions"]=QJsonObject{{"include_hidden",false},{"directories",QJsonArray{"__*"}},{"files",QJsonArray{"*.tmp"}}};
    auto documentProfile=photoProfile;documentProfile.id=QUuid::createUuid().toString(QUuid::WithoutBraces);documentProfile.name="Documents";documentProfile.rate=0;
    QString profileError;if(!TransferProfiles::store({photoProfile,documentProfile},profileError))qFatal("Cannot create demonstration profiles");
    window.showProfiles();app.processEvents();auto *profiles=window.findChild<QDialog *>("profilesDialog");profiles->resize(1000,750);profiles->findChild<QListWidget *>("profilesList")->setCurrentRow(0);
    save(profiles,output+"/15-profils.png",manifest);profiles->close();
    // Recherche réelle sur un historique de démonstration, avec une plage de dates déterministe.
    demoState["jobs"]=QJsonArray{first,queued,completed};window.refresh();window.resize(1240,700);
    window.findChild<QPushButton *>("historySearchButton")->click();window.findChild<QLineEdit *>("historySearchText")->setText("notes");
    window.findChild<QDateEdit *>("historySearchFrom")->setDate(QDate(2026,11,1));window.findChild<QDateEdit *>("historySearchTo")->setDate(QDate(2026,11,30));window.findChild<QCheckBox *>("historySearchDates")->setChecked(true);
    save(&window,output+"/16-recherche-historique.png",manifest);
    // Plusieurs causes simulées, sans réserver de vrais disques ni démarrer de transfert.
    auto limited=first;limited["rate_limit"]=2621440;auto checking=verified;checking["id"]=8;
    demoState["jobs"]=QJsonArray{limited,queued,checking,disconnected};demoState["rate_limit"]=10485760;
    demoState["diagnostics"]=QJsonObject{{"1",QJsonObject{{"queue_reason","active"},{"request_throttled",true}}},{"2",QJsonObject{{"queue_reason","parallel_limit"},{"blockers",QJsonArray{1,8}}}},{"8",QJsonObject{{"queue_reason","active"}}},{"6",QJsonObject{{"queue_reason","device"}}}};
    window.refresh();window.showDiagnostics();app.processEvents();auto *diagnostics=window.findChild<QDialog *>("diagnosticsDialog");diagnostics->resize(1450,750);
    save(diagnostics,output+"/17-diagnostic-transferts.png",manifest);diagnostics->close();
    // Rapport de vérification rapide dans la vraie liste, sur un déplacement de démonstration.
    auto fastChecked=completed;fastChecked["verify_fast_move"]=true;fastChecked["fast_move"]=true;fastChecked["mode"]="move";
    auto fastEntries=fastChecked["entries"].toArray();for(int i=0;i<fastEntries.size();++i){auto item=fastEntries[i].toObject();item["fast_move_check"]=QJsonObject{{"status","ok"},{"path",QJsonValue::Null}};item["undo"]=QJsonObject{{i==0?"directory_move":"rename_move",true}};fastEntries[i]=item;}fastChecked["entries"]=fastEntries;
    window.findChild<QPushButton *>("historySearchButton")->setChecked(false);demoState["jobs"]=QJsonArray{fastChecked};demoState["diagnostics"]=QJsonObject{};window.refresh();if(auto *root=window.entryRow(3,0))root->setExpanded(true);
    save(&window,output+"/18-verification-deplacement-rapide.png",manifest);
    // Horloge injectée et effets simulés : aucun accès à la veille ou à l’extinction réelles.
    for(auto *timer:window.findChildren<QTimer *>())timer->stop();
    window.handle({{"command","configure"},{"gui_settings",QJsonObject{{"completion_action","notify"},{"completion_delay",30}}}});
    demoState["jobs"]=QJsonArray{first};demoState["queue_guard"]=QJsonObject{{"idle",false},{"epoch",1},{"next_id",1},{"last_command","enqueue"}};window.refresh();window.armCompletion();
    auto finished=first;finished["state"]="done";auto finishedEntries=finished["entries"].toArray();for(int i=0;i<finishedEntries.size();++i){auto item=finishedEntries[i].toObject();item["state"]="done";item["copied"]=item["size"];finishedEntries[i]=item;}finished["entries"]=finishedEntries;
    demoState["jobs"]=QJsonArray{finished};demoState["queue_guard"]=QJsonObject{{"idle",true},{"epoch",1},{"next_id",1},{"last_command","enqueue"}};window.refreshCompletion(demoState,1000);window.refreshCompletion(demoState,6000);
    save(window.findChild<QDialog *>("completionCountdown"),output+"/19-fin-de-file.png",manifest);window.cancelCompletion();
    settingsTabs->setCurrentWidget(settings->findChild<QScrollArea *>("completionScroll"));settings->show();app.processEvents();
    save(settings,output+"/20-options-fin-de-file.png",manifest);settings->hide();
    settingsTabs->setCurrentIndex(2);settings->show();app.processEvents();
    save(settings,output+"/21-options-debits.png",manifest);settings->hide();
    // Préparation dans la liste principale : aucun transfert réel ni lancement automatique.
    auto prepared=first;prepared["state"]="waiting";prepared["start_required"]=true;
    auto preparedEntries=prepared["entries"].toArray();for(int i=0;i<preparedEntries.size();++i){auto item=preparedEntries[i].toObject();item["state"]="waiting";item["copied"]=0;preparedEntries[i]=item;}prepared["entries"]=preparedEntries;
    demoState["jobs"]=QJsonArray{prepared};demoState["paused"]=false;window.refresh();if(auto *root=window.entryRow(1,0))root->setExpanded(true);
    save(&window,output+"/22-preparation.png",manifest);
    QJsonObject metadata{{"demo",true},{"qt_version",qVersion()},{"style",app.style()->objectName()},{"font",app.font().family()},{"icon_theme",QIcon::themeName()},{"window_color",app.palette().color(QPalette::Window).name()},{"device_pixel_ratio",app.devicePixelRatio()},{"locale",QLocale().name()},{"theme","light"},
                         {"description","Vrais widgets Gad ; états simulés, fichiers temporaires, aucun bureau réel capturé."},{"captures",manifest}};
    QFile manifestFile(output+"/captures.json");if(!manifestFile.open(QIODevice::WriteOnly))return 2;
    manifestFile.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));return 0;
}
