// Capture de la véritable fenêtre Gad, avec un moteur de démonstration et des fichiers temporaires.
// Aucun serveur local, presse-papiers réel, transfert, notification Plasma ou programme externe n’est utilisé.
#include "window.h"
#include <QtWidgets>
#include <QtCore>
#include <cstdlib>
#include <cstring>

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
    return {{"id",id},{"sources",QJsonArray{source}},{"destination",destination},{"state",state},{"entries",entries},{"mode",mode},
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
    Window window;window.resize(1240,620);window.findChild<QLineEdit *>("destinationInput")->setText(destination);window.show();app.processEvents();
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
    QJsonObject metadata{{"demo",true},{"qt_version",qVersion()},{"style",app.style()->objectName()},{"font",app.font().family()},{"icon_theme",QIcon::themeName()},{"window_color",app.palette().color(QPalette::Window).name()},{"device_pixel_ratio",app.devicePixelRatio()},{"locale",QLocale().name()},{"theme","light"},
                         {"description","Vrais widgets Gad ; états simulés, fichiers temporaires, aucun bureau réel capturé."},{"captures",manifest}};
    QFile manifestFile(output+"/captures.json");if(!manifestFile.open(QIODevice::WriteOnly))return 2;
    manifestFile.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));return 0;
}
