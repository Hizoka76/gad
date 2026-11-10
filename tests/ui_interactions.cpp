// Parcours utilisateur sur les vrais widgets : souris, clavier et fenêtres modales.
// Seule la frontière Rust est simulée ; aucun fichier, raccourci KDE ou transfert réel n’est modifié.
#include "../native/window.h"
#include "../native/application.h"
#include "../native/cli_options.h"
#include <QtTest>
#include <cstdlib>
#include <cstring>
#include <algorithm>

static QJsonObject snapshot;
static QList<QJsonObject> commands;
extern "C" char *sc_request(const char *raw) {
    const auto request=QJsonDocument::fromJson(raw).object();const auto command=request["command"].toString();QJsonObject result;
    if(command=="status" || command=="poll")result=snapshot;
    else if(command=="error_report")result={{"records",QJsonArray{}},{"jobs",QJsonArray{}}};
    else {
        commands.append(request);
        if(command=="pause") {
            snapshot["paused"]=request["paused"].toBool();auto jobs=snapshot["jobs"].toArray();
            if(!request["paused"].toBool())for(int i=0;i<jobs.size();++i){auto job=jobs[i].toObject();job["start_required"]=false;jobs[i]=job;}
            snapshot["jobs"]=jobs;
        }
        result={{"ok",true}};
    }
    return strdup(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}
extern "C" void sc_free(char *value){free(value);}

/// Les attentes passent par la boucle Qt pour tester les événements effectivement délivrés.
class UiInteractions : public QObject {
    Q_OBJECT
    static QJsonObject prepared() {
        QJsonArray entries;
        for(const auto &name:QStringList{"a.txt","b.txt"})entries.append(QJsonObject{{"source","/tmp/ui/source/"+name},{"destination","/tmp/ui/destination/"+name},{"kind","file"},{"state","waiting"},{"size",100},{"copied",0}});
        return {{"id",1},{"sources",QJsonArray{"/tmp/ui/source/a.txt","/tmp/ui/source/b.txt"}},{"destination","/tmp/ui/destination"},{"mode","copy"},{"state","waiting"},{"start_required",true},{"entries",entries}};
    }
    static void expose(Window &window) {
        window.show();window.activateWindow();QTest::qWait(20);
    }
    static void openSettings(Window &window) {
        QTest::mouseClick(window.findChild<QToolButton *>("settingsButton"),Qt::LeftButton);QCoreApplication::processEvents();
    }
private Q_SLOTS:
    void initTestCase(){qApp->setOrganizationName("GadTests");qApp->setApplicationName("UiInteractions");applicationIcon();}
    void init(){QSettings().clear();I18n::initialize("fr_FR");snapshot={{"jobs",QJsonArray{}},{"paused",false}};commands.clear();}
    void cleanup(){QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QSettings().clear();}

    /// Les raccourcis sont actifs hors menu ; masquer ne demande jamais l’arrêt du moteur.
    void sourceAndWindowShortcuts() {
        Window window;expose(window);
        QCOMPARE(window.findChild<QAction *>("addFolderAction")->shortcut(),QKeySequence(Qt::CTRL | Qt::Key_O));
        QCOMPARE(window.findChild<QAction *>("addFilesAction")->shortcut(),QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
        QVERIFY(window.findChild<QGroupBox *>("sourceBox")->width()<window.findChild<QGroupBox *>("destinationBox")->width());
        commands.clear();
        QTest::keyClick(&window,Qt::Key_W,Qt::ControlModifier | Qt::AltModifier);
        QTRY_VERIFY(!window.isVisible());
        QVERIFY(commands.isEmpty());
        expose(window);
        auto job=prepared();job["start_required"]=false;job["state"]="copying";snapshot["jobs"]=QJsonArray{job};
        bool warningShown=false;
        QTimer::singleShot(0,&window,[&]{if(auto *box=qobject_cast<QMessageBox *>(QApplication::activeModalWidget())){warningShown=true;box->accept();}});
        QTest::keyClick(&window,Qt::Key_Q,Qt::ControlModifier);
        QVERIFY(warningShown);QVERIFY(window.isVisible());
        for(const auto &command:commands)QVERIFY(command["command"]!="shutdown");
    }

    /// Un vrai changement de focus enregistre les motifs sans bouton de confirmation.
    void exclusionsSaveWhenFocusLeavesTheField() {
        Window window;expose(window);openSettings(window);
        auto *dialog=window.findChild<QDialog *>("settingsDialog");
        dialog->findChild<QTabWidget *>("settingsTabs")->setCurrentIndex(4);
        auto *folders=dialog->findChild<QPlainTextEdit *>("excludedDirectories");
        auto *files=dialog->findChild<QPlainTextEdit *>("excludedFiles");
        QTest::mouseClick(folders->viewport(),Qt::LeftButton);folders->setPlainText("__*\nnode_modules");commands.clear();
        QVERIFY(QSettings().value("excludeDirectories").toStringList().isEmpty());
        QTest::mouseClick(files->viewport(),Qt::LeftButton);
        QCOMPARE(QSettings().value("excludeDirectories").toStringList(),QStringList({"__*","node_modules"}));
        files->setPlainText("*.tmp\n*~");
        QTest::mouseClick(dialog->findChild<QPushButton *>("settingsCloseButton"),Qt::LeftButton);
        QCOMPARE(QSettings().value("excludeFiles").toStringList(),QStringList({"*.tmp","*~"}));
        QVERIFY(!dialog->isVisible());
        QCOMPARE(commands.size(),2);
        for(const auto &command:commands)QCOMPARE(command["command"].toString(),QString("configure"));
    }

    /// Le bouton unique lance la préparation, met en pause et reprend avec les événements souris réels.
    void startPauseResumeWithMouse() {
        snapshot["jobs"]=QJsonArray{prepared()};Window window;expose(window);auto *button=window.findChild<QPushButton *>("queueControlButton");commands.clear();
        QVERIFY(button->isEnabled());QCOMPARE(button->text(),QString("Démarrer"));QTest::mouseClick(button,Qt::LeftButton);
        QCOMPARE(commands.last()["command"].toString(),QString("pause"));QVERIFY(!commands.last()["paused"].toBool());QVERIFY(!snapshot["jobs"].toArray()[0].toObject()["start_required"].toBool());
        QTest::mouseClick(button,Qt::LeftButton);QVERIFY(commands.last()["paused"].toBool());
        QTest::mouseClick(button,Qt::LeftButton);QVERIFY(!commands.last()["paused"].toBool());
        QCOMPARE(std::count_if(commands.begin(),commands.end(),[](const auto &r){return r["command"]=="pause";}),3);
    }

    /// Les cinq onglets restent accessibles, et Échap rend la main sans annuler les réglages.
    void optionsKeyboardAndLiveLanguage() {
        Window window;expose(window);openSettings(window);auto *dialog=window.findChild<QDialog *>("settingsDialog");auto *tabs=dialog->findChild<QTabWidget *>("settingsTabs");
        QTRY_COMPARE(QApplication::activeModalWidget(),dialog);QCOMPARE(tabs->count(),5);
        tabs->setFocus();QTest::keyClick(tabs,Qt::Key_Tab,Qt::ControlModifier);QCOMPARE(tabs->currentIndex(),1);
        QTest::keyClick(tabs,Qt::Key_Tab,Qt::ControlModifier);QCOMPARE(tabs->currentIndex(),2);
        QTest::keyClick(tabs,Qt::Key_Tab,Qt::ControlModifier);QCOMPARE(tabs->currentIndex(),3);tabs->setCurrentIndex(0);
        QVERIFY(!dialog->findChild<QComboBox *>("languageSelector"));
        QTest::keyClick(dialog,Qt::Key_Escape);QTRY_VERIFY(!dialog->isVisible());QVERIFY(!QApplication::activeModalWidget());
        auto *flag=window.findChild<QToolButton *>("languageMenuButton");QVERIFY(flag);auto *menu=flag->menu();QVERIFY(menu);
        QTimer::singleShot(0,menu,[menu]{for(auto *action:menu->actions())if(action->data().toString()=="en_US"){action->trigger();break;}menu->close();});
        QTest::mouseClick(flag,Qt::LeftButton);QCOMPARE(QSettings().value("language").toString(),QString("en_US"));
        openSettings(window);QCOMPARE(tabs->tabText(2),QString("Speed limits"));QCOMPARE(tabs->tabText(3),QString("Batch completion"));QTest::keyClick(dialog,Qt::Key_Escape);
    }

    /// Une entrée souris destinée au QWindow principal est bloquée pendant chaque dialogue modal.
    void modalWindowsBlockMainInput_data() {
        QTest::addColumn<QString>("name");QTest::newRow("options")<<"settingsDialog";QTest::newRow("errors")<<"errorReportDialog";QTest::newRow("transfers")<<"diagnosticsDialog";
    }
    void modalWindowsBlockMainInput() {
        QFETCH(QString,name);snapshot["jobs"]=QJsonArray{prepared()};Window window;expose(window);
        if(name=="settingsDialog")openSettings(window);else if(name=="errorReportDialog")QTest::mouseClick(window.findChild<QPushButton *>("errorReportButton"),Qt::LeftButton);else QTest::mouseClick(window.findChild<QPushButton *>("diagnosticsButton"),Qt::LeftButton);
        auto *dialog=window.findChild<QDialog *>(name);QVERIFY(dialog);QTRY_COMPARE(QApplication::activeModalWidget(),dialog);auto *start=window.findChild<QPushButton *>("queueControlButton");commands.clear();
        QTest::mouseClick(window.windowHandle(),Qt::LeftButton,Qt::NoModifier,start->mapTo(&window,start->rect().center()));QCoreApplication::processEvents();
        QVERIFY(std::none_of(commands.begin(),commands.end(),[](const auto &r){return r["command"]=="pause";}));
        QTest::keyClick(dialog,Qt::Key_Escape);QTRY_VERIFY(!dialog->isVisible());QTest::mouseClick(start,Qt::LeftButton);
        QVERIFY(std::any_of(commands.begin(),commands.end(),[](const auto &r){return r["command"]=="pause";}));
    }

    /// Ctrl+clic garde plusieurs lignes sélectionnées ; Ctrl+F et Échap ouvrent et ferment la recherche.
    void multipleSelectionAndSearchShortcuts() {
        snapshot["jobs"]=QJsonArray{prepared()};Window window;expose(window);auto *tree=window.findChild<QTreeWidget *>();auto *a=window.entryRow(1,0),*b=window.entryRow(1,1);QVERIFY(a);QVERIFY(b);
        for(auto *row:{a,b})for(auto *p=row->parent();p;p=p->parent())p->setExpanded(true);
        tree->scrollToItem(a);QCoreApplication::processEvents();QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::NoModifier,tree->visualItemRect(a).center());QTest::mouseClick(tree->viewport(),Qt::LeftButton,Qt::ControlModifier,tree->visualItemRect(b).center());
        QVERIFY(a->isSelected());QVERIFY(b->isSelected());QCOMPARE(tree->selectedItems().size(),2);
        tree->setFocus();QTest::keyClick(tree,Qt::Key_F,Qt::ControlModifier);QTRY_VERIFY(window.findChild<QWidget *>("historySearchPanel")->isVisible());
        auto *historyOnly=window.findChild<QCheckBox *>("historySearchOnly");QTest::mouseClick(historyOnly,Qt::LeftButton);QVERIFY(!a->isHidden());
        auto *text=window.findChild<QLineEdit *>("historySearchText");text->setFocus();QTest::keyClicks(text,"no-match");QTRY_VERIFY_WITH_TIMEOUT(a->isHidden(),1000);QTest::keyClick(text,Qt::Key_Escape);QTRY_VERIFY(!window.findChild<QWidget *>("historySearchPanel")->isVisible());QVERIFY(!a->isHidden());
    }

    /// La case, la propriété Qt, la CLI et les profils utilisent la même préférence durable.
    void progressMessagePreferenceAndProfile() {
        {Window window;expose(window);openSettings(window);auto *dialog=window.findChild<QDialog *>("settingsDialog");auto *check=dialog->findChild<QCheckBox *>("progressMessage");auto *tracking=window.findChild<TransferNotifications *>("desktopTransfers");QVERIFY(check);QVERIFY(check->isChecked());QVERIFY(tracking->property("progressMessage").toBool());
        check->setFocus();QTest::keyClick(check,Qt::Key_Space);QVERIFY(!check->isChecked());QVERIFY(!tracking->property("progressMessage").toBool());QVERIFY(!QSettings().value("progressMessage").toBool());
        auto profile=window.currentTransferProfile();profile.name="UI progress";QVERIFY(!profile.gui["progressMessage"].toBool());QString failure;QVERIFY(TransferProfiles::store({profile},failure));
        auto request=CliOptions::configure({"gad","--configure","--progress-message"},failure);QVERIFY(failure.isEmpty());QVERIFY(!window.handle(request).contains("error"));QVERIFY(check->isChecked());QVERIFY(tracking->property("progressMessage").toBool());
        QVERIFY(window.applyTransferProfile(profile.id));QVERIFY(!check->isChecked());QVERIFY(!tracking->property("progressMessage").toBool());QTest::keyClick(dialog,Qt::Key_Escape);}
        Window reopened;QVERIFY(!reopened.findChild<QCheckBox *>("progressMessage")->isChecked());QVERIFY(!reopened.findChild<TransferNotifications *>("desktopTransfers")->property("progressMessage").toBool());
        QString error;const auto off=CliOptions::configure({"gad","--configure","--no-progress-message"},error);QVERIFY(error.isEmpty());QVERIFY(!off["gui_settings"].toObject()["progressMessage"].toBool());QVERIFY(CliOptions::configure({"gad","--configure","--progress-message","--no-progress-message"},error).isEmpty());QVERIFY(!error.isEmpty());
        commands.clear();QVERIFY(reopened.handle({{"command","configure"},{"gui_settings",QJsonObject{{"progressMessage","false"}}}}).contains("error"));QVERIFY(commands.isEmpty());
    }
};
QTEST_MAIN(UiInteractions)
#include "ui_interactions_test.moc"
