// Tests unitaires Qt des chemins et actions d’ouverture ; aucune application externe n’est lancée.
#include "../native/window.h"
#include "../native/cli_options.h"
#include "../native/documentation.h"
#include "../native/kio_sources.h"
#include "../native/error_report.h"
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
static QList<QJsonObject> mutations;
static bool rejectExclusions=false;
/// Remplace uniquement la frontière Rust ; les vrais widgets et connexions du menu sont conservés.
extern "C" char *sc_request(const char *input) {
    const auto request=QJsonDocument::fromJson(input).object();QJsonObject result;
    if(request["command"]=="error_report")result=errorReport;
    else if(request["command"]=="status" || request["command"]=="poll")result=status;
    else {mutations.append(request);result=rejectExclusions && request["command"]=="configure" && request.contains("exclusions")?QJsonObject{{"error","Invalid exclusion rules"}}:QJsonObject{{"ok",true},{"id",1}};}
    return strdup(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}
extern "C" void sc_free(char *value){free(value);}

/// Intercepte le protocole file de QDesktopServices, y compris les ouvertures de dossiers.
class DesktopCapture : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public Q_SLOTS:
    void capture(const QUrl &url){urls.append(url);}
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
    void initTestCase() {
        qApp->setOrganizationName("GadTests");qApp->setApplicationName("RowActions");
        applicationIcon();I18n::initialize("fr_FR");
        QDesktopServices::setUrlHandler("file",&desktop,"capture");
    }
    void cleanupTestCase(){QDesktopServices::unsetUrlHandler("file");}
    void init(){rejectExclusions=false;errorReport={{"records",QJsonArray{}},{"jobs",QJsonArray{}},{"generated_unix",0}};status={{"jobs",QJsonArray{}},{"paused",false}};mutations.clear();desktop.urls.clear();}

    /// Les éditions installées suivent la langue à chaud et les liens ouvrent de vrais fichiers locaux.
    void aboutDocumentationLinksFollowLanguageAndOpenFiles() {
        QTemporaryDir temporary;QVERIFY(temporary.isValid());const auto previousData=qgetenv("XDG_DATA_HOME"),previousDirs=qgetenv("XDG_DATA_DIRS");const auto previousLanguage=QSettings().value("language");
        const auto restore=qScopeGuard([=]{qputenv("XDG_DATA_HOME",previousData);qputenv("XDG_DATA_DIRS",previousDirs);if(previousLanguage.isValid())QSettings().setValue("language",previousLanguage);else QSettings().remove("language");I18n::initialize("fr_FR");});
        qputenv("XDG_DATA_HOME",QFile::encodeName(temporary.path()));qputenv("XDG_DATA_DIRS",QFile::encodeName(temporary.path()+"/empty"));
        const auto root=temporary.path()+"/gad/docs";QVERIFY(QDir().mkpath(root+"/html"));QVERIFY(QDir().mkpath(root+"/pdf"));QJsonArray records;
        const QStringList locales{"fr_FR","en_US","es_ES","pt_PT","de_DE","ja_JP"};
        const QStringList kinds{"quick","user","developer"};
        for(const auto &locale:locales)for(const auto &kind:kinds) {
            const auto name=kind+" guide & "+locale;
            for(const auto &format:QStringList{"html","pdf"}){QFile file(root+"/"+format+"/"+name+"."+format);QVERIFY(file.open(QIODevice::WriteOnly));file.write("fixture");}
            records.append(QJsonObject{{"kind",kind},{"locale",locale},{"title",kind+" & "+locale},{"html","html/"+name+".html"},{"pdf","pdf/"+name+".pdf"}});
        }
        QFile catalog(root+"/catalog.json");QVERIFY(catalog.open(QIODevice::WriteOnly));catalog.write(QJsonDocument(QJsonObject{{"format","gad-documentation"},{"version",1},{"guides",records}}).toJson());catalog.close();
        const auto variant=Documentation::links(QLocale("pt_BR"));QCOMPARE(variant.size(),3);QVERIFY(variant[0].title.endsWith("pt_PT"));
        I18n::initialize("fr_FR");Window window;window.showAbout();auto *dialog=window.findChild<QMessageBox *>("aboutDialog");QVERIFY(dialog);
        auto *language=window.findChild<QComboBox *>("languageSelector");
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
        const auto pdf=Documentation::links()[0].pdf;QVERIFY(QFile::remove(selected.toLocalFile()));auto missingHtml=Documentation::links();QVERIFY(missingHtml[0].html.isEmpty());QCOMPARE(missingHtml[0].pdf,pdf);
        QVERIFY(QFile::remove(pdf.toLocalFile()));const auto fallback=Documentation::links();QCOMPARE(fallback.size(),3);QVERIFY(fallback[0].title.endsWith("en_US"));
        const QPointer<QMessageBox> aboutGuard(dialog);QTest::mouseClick(dialog->findChild<QPushButton *>("aboutCloseButton"),Qt::LeftButton);QVERIFY(!aboutGuard || !aboutGuard->isVisible());
    }

    /// Une variante de langue est résolue et aucun chemin hors du dossier de documentation n’est accepté.
    void documentationRejectsMissingAndEscapingPaths() {
        QTemporaryDir temporary;QVERIFY(temporary.isValid());const auto root=temporary.path()+"/docs";QVERIFY(QDir().mkpath(root+"/html"));
        QFile outside(temporary.path()+"/outside.html");QVERIFY(outside.open(QIODevice::WriteOnly));outside.write("outside");outside.close();
        QFile inside(root+"/html/guide.html");QVERIFY(inside.open(QIODevice::WriteOnly));inside.write("inside");inside.close();
        QVERIFY(!Documentation::localFile(root,"html/guide.html","html").isEmpty());
        QVERIFY(Documentation::localFile(root,"../outside.html","html").isEmpty());QVERIFY(Documentation::localFile(root,outside.fileName(),"html").isEmpty());
        QVERIFY(Documentation::localFile(root,"html/missing.html","html").isEmpty());QVERIFY(Documentation::localFile(root,"html/guide.html","pdf").isEmpty());
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
        const auto request=CliOptions::configure({"gad","--configure","--rate-limit","2.5","--exclude-folder","__*","--exclude-folder","node_modules","--no-include-hidden","--autostart","--no-konqueror-ctrl-v","--language","en_US"},error);
        QVERIFY(error.isEmpty());QCOMPARE(request["rate_limit"].toInteger(),2621440);QCOMPARE(request["exclusion_patch"].toObject()["directories"].toArray().size(),2);QVERIFY(!request["exclusion_patch"].toObject()["include_hidden"].toBool());QVERIFY(request["gui_settings"].toObject()["autostart"].toBool());
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
        auto *panel=dialog->findChild<QWidget *>("settingsPanel");QVERIFY(panel);QCOMPARE(dialog->findChildren<QCheckBox *>().size(),10);
        for(auto *check:dialog->findChildren<QCheckBox *>())QVERIFY(dialog->findChild<QLabel *>(check->objectName()+"CommandOption"));
        QCOMPARE(panel->findChild<QLabel *>("verifyMd5CommandOption")->text(),QString("--md5 / --no-md5"));
        QCOMPARE(panel->findChild<QLabel *>("verifyPropertiesCommandOption")->text(),QString("--properties / --no-properties"));
        auto *tabs=dialog->findChild<QTabWidget *>("settingsTabs");QVERIFY(tabs);QCOMPARE(tabs->count(),2);
        QTest::mouseClick(button,Qt::LeftButton);QVERIFY(dialog->isVisible());QVERIFY(!dialog->isModal());
        auto *md5=panel->findChild<QCheckBox *>("verifyMd5");const auto checked=md5->isChecked();mutations.clear();
        QTest::mouseClick(md5,Qt::LeftButton,Qt::NoModifier,QPoint(md5->style()->pixelMetric(QStyle::PM_IndicatorWidth)/2,md5->height()/2));QCOMPARE(md5->isChecked(),!checked);QVERIFY(dialog->isVisible());QCOMPARE(mutations.size(),1);
        auto *requestRate=window.findChild<QPushButton *>("settingsRequestRate");QVERIFY(requestRate);QVERIFY(!requestRate->isEnabled());
        QVERIFY(!dialog->isAncestorOf(requestRate));auto *cancel=window.findChild<QPushButton *>("cancellationMenu");QVERIFY(cancel);
        QCOMPARE(requestRate->y(),cancel->y());QVERIFY(requestRate->x()<cancel->x());
        for(const auto &name:QStringList{"exclusionsCommandOption","globalRateCommandOption","languageCommandOption"})QVERIFY(panel->findChild<QLabel *>(name));
        window.findChild<QTreeWidget *>()->setCurrentItem(window.entryRow(1,0));QVERIFY(requestRate->isEnabled());
        auto *language=panel->findChild<QComboBox *>("languageSelector");language->setCurrentIndex(language->findData("en_US"));QCOMPARE(dialog->windowTitle(),QString("Settings"));QCOMPARE(tabs->tabText(1),QString("Shortcuts"));
        auto *close=dialog->findChild<QPushButton *>("settingsCloseButton");QCOMPARE(close->text(),QString("Close"));
        QTest::mouseClick(panel->findChild<QPushButton *>("exclusionsButton"),Qt::LeftButton);
        auto *exclusions=dialog->findChild<QDialog *>("exclusionsDialog");QVERIFY(exclusions);QVERIFY(exclusions->isVisible());exclusions->reject();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
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

    /// Le bilan réessaie seulement les erreurs réessayables affichées après filtrage, avec leurs indices stables.
    void errorReportFiltersAndTargetsExactRows() {
        QJsonArray records;
        for(int i=0;i<3;++i)records.append(QJsonObject{{"job",i+1},{"index",0},{"scope","entry"},{"mode","copy"},{"state",i==1?"done":"error"},{"kind","file"},{"source",i==2?"/tmp/second":"/tmp/first"},{"destination","/tmp/dest"},{"error","détail\n<texte>"},{"retryable",i!=1},{"retry_reason",i==1?"finalisation":""}});
        errorReport={{"records",records},{"jobs",QJsonArray{}},{"generated_unix",0}};
        Window window;window.findChild<QPushButton *>("errorReportButton")->click();
        auto *dialog=window.findChild<QDialog *>("errorReportDialog");QVERIFY(dialog);QCOMPARE(dialog->minimumSize(),QSize(1100,600));
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
        auto *language=window.findChild<QComboBox *>("languageSelector");QVERIFY(language);
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
        auto *language=window.findChild<QComboBox *>("languageSelector");auto *row=window.entryRow(1,0);
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
        auto *language=window.findChild<QComboBox *>("languageSelector");auto *row=window.entryRow(1,0);
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
        auto *language=window.findChild<QComboBox *>("languageSelector");auto *row=window.entryRow(1,0);
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
        auto *language=window.findChild<QComboBox *>("languageSelector");auto *row=window.entryRow(1,0);
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
        Window window;auto *selector=window.findChild<QComboBox *>("languageSelector");
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
        auto *language=window.findChild<QComboBox *>("languageSelector");const auto bindingCount=I18n::bindings().size();
        language->setCurrentIndex(language->findData("en_US"));
        QVERIFY(window.windowTitle().contains("Advanced Duplication Manager"));QCOMPARE(tree->headerItem()->text(2),QString("Status"));
        QCOMPARE(window.entryRow(1,0),item);QVERIFY(item->isSelected());QVERIFY(!item->isExpanded());QCOMPARE(item->text(0),source);QCOMPARE(item->text(2),QString("Running"));
        QCOMPARE(destination->text(),QString("/tmp/En pause/Source"));QCOMPARE(rate->value(),12.5);QCOMPARE(rate->suffix(),QString(" MiB/s"));
        QCOMPARE(errorDialog->text(),QString("No files to copy"));
        QCOMPARE(about->windowTitle(),QString("About Gad"));QCOMPARE(report->windowTitle(),QString("Gad — Error report"));
        QCOMPARE(reportRows->topLevelItemCount(),1);QCOMPARE(reportRows->topLevelItem(0),reportRow);QVERIFY(reportRow->isSelected());QCOMPARE(search->text(),QString("fichier"));
        QCOMPARE(reportRow->text(5),QString("No files to copy"));QVERIFY(report->findChild<QPlainTextEdit *>("errorReportDetail")->toPlainText().contains("Source:"));
        QCOMPARE(rateDialog->windowTitle(),QString("Limit request speed"));QCOMPARE(requestRate->value(),7.25);
        bool addTranslated=false;for(auto *button:window.findChildren<QPushButton *>())addTranslated|=button->text()=="Add files…";QVERIFY(addTranslated);
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
        auto *language=window.findChild<QComboBox *>("languageSelector");mutations.clear();
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
        QCOMPARE(zRow->text(column),QString("1"));QCOMPARE(mRow->text(column),QString("2"));QCOMPARE(aRow->text(column),QString("3"));QCOMPARE(aRow->toolTip(column),QString("Lot #7"));
        view->setCurrentItem(aRow);aRow->setSelected(true);mutations.clear();view->setViewSort(0);
        QCOMPARE(view->topLevelItem(0),aRow);QCOMPARE(aRow->text(column),QString("3"));QVERIFY(aRow->isSelected());
        view->setColumnFilter(0,"a.txt");QVERIFY(!aRow->isHidden());QVERIFY(zRow->isHidden());QCOMPARE(aRow->text(column),QString("3"));
        view->clearHeaderFilters();view->setViewSort(column);QCOMPARE(view->topLevelItem(0),zRow);QCOMPARE(view->topLevelItem(1),mRow);
        view->setViewSort(-1);
        // Une priorité modifiée seule doit renuméroter aussi les demandes absentes du delta jobs.
        status={{"jobs",QJsonArray{}},{"job_ids",QJsonArray{7,11}},{"row_order",QJsonArray{aKey,zKey,mKey}},{"revision",2},{"paused",false}};window.refresh();
        QCOMPARE(aRow->text(column),QString("1"));QCOMPARE(zRow->text(column),QString("2"));QCOMPARE(mRow->text(column),QString("3"));QCOMPARE(view->topLevelItem(0),aRow);QVERIFY(aRow->isSelected());
        view->setColumnFilter(column,"2");QVERIFY(!zRow->isHidden());QVERIFY(aRow->isHidden());I18n::initialize("en_US");
        QCOMPARE(view->headerItem()->text(column),QString("Order"));QCOMPARE(zRow->toolTip(column),QString("Batch #11"));QCOMPARE(zRow->text(column),QString("2"));QVERIFY(mutations.isEmpty());
    }

    /// Les dossiers et les demandes en analyse ont aussi une position ; un filtre garde les ancêtres.
    void queueOrderColumnIncludesFoldersAndScanning() {
        auto nested=job(nestedEntries(),"running"),scanning=job({},"scanning");scanning["id"]=2;
        status["jobs"]=QJsonArray{nested,scanning};Window window;auto *view=static_cast<TransferTree *>(window.findChild<QTreeWidget *>());
        QCOMPARE(window.entryRow(1,0)->text(TableView::OrderColumn),QString("1"));QCOMPARE(window.entryRow(1,1)->text(TableView::OrderColumn),QString("2"));QCOMPARE(window.entryRow(1,2)->text(TableView::OrderColumn),QString("3"));
        QTreeWidgetItem *pending=nullptr;for(int i=0;i<view->topLevelItemCount();++i)if(view->topLevelItem(i)->data(0,Qt::UserRole).toLongLong()==2)pending=view->topLevelItem(i);
        QVERIFY(pending);QCOMPARE(pending->text(TableView::OrderColumn),QString("6"));QCOMPARE(pending->toolTip(TableView::OrderColumn),QString("Lot #2"));
        view->setColumnFilter(TableView::OrderColumn,"3");QVERIFY(!window.entryRow(1,0)->isHidden());QVERIFY(!window.entryRow(1,1)->isHidden());QVERIFY(!window.entryRow(1,2)->isHidden());QVERIFY(pending->isHidden());
        QCOMPARE(window.entryRow(1,2)->parent(),window.entryRow(1,1));
    }

    /// Seules les phrases rédigées pour l’aide sont découpées, jamais les noms de fichiers ou les URL.
    void sourceAndHelpTooltipsStayReadableInBothLanguages() {
        I18n::initialize("fr_FR");const auto restore=qScopeGuard([]{I18n::initialize("fr_FR");});
        auto entry=file("running");const QString path="/tmp/source/version. Finale <b>.txt";entry["source"]=path;publish(job({entry},"running"));Window window;
        auto *row=window.entryRow(1,0);QTextDocument source;source.setHtml(row->toolTip(0));
        QCOMPARE(source.toPlainText(),path);QCOMPARE(row->toolTip(TableView::OrderColumn),QString("Lot #1"));
        auto *md5=window.findChild<QCheckBox *>("verifyMd5");QVERIFY(md5->toolTip().contains(".\n"));
        QVERIFY(!md5->toolTip().contains(". S’applique"));
        auto *space=window.findChild<QCheckBox *>("verifySpace");QCOMPARE(space->toolTip().count('\n'),4);
        I18n::initialize("en_US");source.setHtml(row->toolTip(0));QCOMPARE(source.toPlainText(),path);
        QCOMPARE(row->toolTip(TableView::OrderColumn),QString("Batch #1"));QVERIFY(md5->toolTip().contains(".\nApplies"));QCOMPARE(space->toolTip().count('\n'),4);
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
        tip.setHtml(leaf->toolTip(0));QVERIFY(tip.toPlainText().contains("\nMD5\n"));QVERIFY(tip.toPlainText().contains("Limite :"));QVERIFY(!tip.toPlainText().contains("Déplacement"));
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
        QCOMPARE(SourceUrls::requiredProtocol(submitted),22);
    }

    /// L’option est mémorisée et figée dans chaque nouvelle demande, même en arrière-plan.
    void duplicationOptionIsCapturedForNewRequests() {
        const auto previous=QSettings().value("allowDuplication");QSettings().remove("allowDuplication");
        const auto restore=qScopeGuard([previous]{if(previous.isValid())QSettings().setValue("allowDuplication",previous);else QSettings().remove("allowDuplication");});
        Window window;auto *check=window.findChild<QCheckBox *>("allowDuplication");QVERIFY(check);QVERIFY(!check->isChecked());
        mutations.clear();check->setChecked(true);QCOMPARE(mutations.size(),1);QVERIFY(mutations[0]["allow_duplication"].toBool());
        QVERIFY(QSettings().value("allowDuplication").toBool());
        mutations.clear();window.handle({{"command","enqueue"},{"sources",QJsonArray{"/tmp/source"}},{"destination","/tmp"},{"background",true}});
        QCOMPARE(mutations.size(),1);const auto submitted=mutations[0];QVERIFY(submitted["allow_duplication"].toBool());QCOMPARE(SourceUrls::requiredProtocol(submitted),22);
        check->setChecked(false);QVERIFY(submitted["allow_duplication"].toBool());QVERIFY(!QSettings().value("allowDuplication").toBool());
    }

    /// Annuler conserve les préférences ; appliquer valide les règles puis les persiste atomiquement côté interface.
    void exclusionsDialogStagesValidatesAndTranslates() {
        const QStringList keys={"includeHidden","excludeDirectories","excludeFiles"};QMap<QString,QVariant> previous;
        for(const auto &key:keys){previous.insert(key,QSettings().value(key));QSettings().remove(key);}
        const auto restore=qScopeGuard([previous]{I18n::initialize("fr_FR");for(auto it=previous.cbegin();it!=previous.cend();++it){if(it.value().isValid())QSettings().setValue(it.key(),it.value());else QSettings().remove(it.key());}});
        Window window;window.showExclusions();auto *dialog=window.findChild<QDialog *>("exclusionsDialog");QVERIFY(dialog);mutations.clear();
        dialog->findChild<QPlainTextEdit *>("excludedDirectories")->setPlainText("__*");dialog->findChild<QDialogButtonBox *>("exclusionsButtons")->button(QDialogButtonBox::Cancel)->click();QVERIFY(mutations.isEmpty());
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);window.showExclusions();dialog=window.findChild<QDialog *>("exclusionsDialog");QVERIFY(dialog);
        auto *directories=dialog->findChild<QPlainTextEdit *>("excludedDirectories"),*files=dialog->findChild<QPlainTextEdit *>("excludedFiles");
        QVERIFY(directories->toPlainText().isEmpty());directories->setPlainText("__*\n\nnode_modules");files->setPlainText("*.tmp\n*~");dialog->findChild<QCheckBox *>("exclusionsIncludeHidden")->setChecked(false);
        I18n::initialize("en_US");QCOMPARE(dialog->windowTitle(),QString("Transfer exclusions"));QCOMPARE(directories->toPlainText(),QString("__*\n\nnode_modules"));
        auto *buttons=dialog->findChild<QDialogButtonBox *>("exclusionsButtons");rejectExclusions=true;buttons->button(QDialogButtonBox::Ok)->click();
        QVERIFY(dialog->isVisible());QVERIFY(!QSettings().contains("excludeDirectories"));QVERIFY(window.findChild<QCheckBox *>("includeHidden")->isChecked());
        rejectExclusions=false;buttons->button(QDialogButtonBox::Ok)->click();QVERIFY(!window.findChild<QCheckBox *>("includeHidden")->isChecked());
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
        auto a=file("waiting"),b=a,c=a;b["source"]="/tmp/source/b";b["destination"]="/tmp/destination/b";
        auto first=job({a,b},"waiting"),second=job({c},"waiting");second["id"]=2;
        status["jobs"]=QJsonArray{first,second};Window window;auto *tree=window.findChild<QTreeWidget *>();
        QCOMPARE(tree->topLevelItemCount(),3);auto *row=window.entryRow(1,0);
        QVERIFY(!row->parent());QVERIFY(!RowTree::owner(row)->treeWidget());
        QCOMPARE(row->background(0),window.entryRow(1,1)->background(0));QVERIFY(row->background(0)!=window.entryRow(2,0)->background(0));
        tree->setCurrentItem(row);window.updateRowActions();window.findChild<QAction *>("rowSelectRequest")->trigger();
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
        QTextDocument tip;tip.setHtml(window.entryRow(1,2)->toolTip(0));QVERIFY(tip.toPlainText().startsWith("/tmp/source/root/sub/a.txt"));QVERIFY(!tip.toPlainText().contains("Demande #1"));QVERIFY(!tip.toPlainText().contains("\nCopie"));QCOMPARE(window.entryRow(1,2)->toolTip(TableView::OrderColumn),QString("Lot #1"));
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
