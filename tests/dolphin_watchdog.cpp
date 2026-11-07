// Tests des actions réellement enregistrées et de la surveillance périodique, dans un compte privé.
#include "../native/dolphin/watchdog.h"
#include <QtTest>
class DolphinMainWindow:public KXmlGuiWindow {
    Q_OBJECT
public:
    DolphinMainWindow(){
        for(const auto &name:QStringList{DolphinWatchdog::current,DolphinWatchdog::context,DolphinWatchdog::paste,"unrelated"})actionCollection()->addAction(name,new QAction(this));
        actionCollection()->action(DolphinWatchdog::paste)->setProperty("defaultShortcuts",QVariant::fromValue(QKeySequence::keyBindings(QKeySequence::Paste)));
        actionCollection()->action("unrelated")->setShortcut(QKeySequence("Ctrl+Alt+J"));
    }
    QAction *paste(){return actionCollection()->action(DolphinWatchdog::current);}
    void recreate(){auto *old=paste();actionCollection()->takeAction(old);delete old;actionCollection()->addAction(DolphinWatchdog::current,new QAction(this));}
};
class WatchdogTest:public QObject {
    Q_OBJECT
    QString data,path;
    void config(bool enabled,const QString &custom={}){
        const auto shortcuts=enabled?QString("<Action name=\"%1\" shortcut=\"%2\"/><Action name=\"edit_paste\" shortcut=\"Shift+Ins\"/>").arg(DolphinWatchdog::current,custom.isEmpty()?"Ctrl+V":custom):QString();
        QFile xml(path);QVERIFY(xml.open(QIODevice::WriteOnly));xml.write(("<gui><ActionProperties scheme=\"Default\">"+shortcuts+"</ActionProperties></gui>").toUtf8());xml.close();
        QFile receipt(data+"/gad/dolphin-paste.json");QVERIFY(receipt.open(QIODevice::WriteOnly));receipt.write(QJsonDocument(QJsonObject{{"version",2},{"files",enabled?QJsonObject{{path,QJsonObject()}}:QJsonObject()}}).toJson());
    }
private Q_SLOTS:
    void initTestCase(){qputenv("GAD_DOLPHIN_EXECUTABLE","/bin/true");QCoreApplication::setApplicationName("dolphin");data=QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);QDir().mkpath(data+"/gad");QDir().mkpath(data+"/kxmlgui5/dolphin");path=data+"/kxmlgui5/dolphin/dolphinui.rc";}
    void recreatedActionIsRepaired(){config(true);DolphinMainWindow window;DolphinWatchdog::Monitor monitor;monitor.check();window.recreate();QVERIFY(window.paste()->shortcuts().isEmpty());monitor.check();QCOMPARE(window.paste()->shortcut(),QKeySequence("Ctrl+V"));QCOMPARE(window.actionCollection()->action("unrelated")->shortcut(),QKeySequence("Ctrl+Alt+J"));}
    void periodicTimerActuallyRepairs(){config(true);DolphinMainWindow window;DolphinWatchdog::Monitor monitor;QTRY_COMPARE_WITH_TIMEOUT(window.paste()->shortcut(),QKeySequence("Ctrl+V"),2500);window.recreate();QTRY_COMPARE_WITH_TIMEOUT(window.paste()->shortcut(),QKeySequence("Ctrl+V"),2500);}
    void disablingRestoresNativeAndStopsEnforcing(){config(true);DolphinMainWindow window;DolphinWatchdog::Monitor monitor;monitor.check();config(false);monitor.check();QVERIFY(window.paste()->shortcuts().isEmpty());auto *native=window.actionCollection()->action(DolphinWatchdog::paste);QVERIFY(native->shortcuts().contains(QKeySequence("Ctrl+V")));native->setShortcut(QKeySequence("Ctrl+Y"));monitor.check();QCOMPARE(native->shortcut(),QKeySequence("Ctrl+Y"));}
    void explicitUserChangesArePreserved(){config(true);DolphinMainWindow window;DolphinWatchdog::Monitor monitor;monitor.check();config(true,"Ctrl+Alt+P");window.paste()->setShortcut(QKeySequence("Ctrl+Alt+P"));monitor.check();QCOMPARE(window.paste()->shortcut(),QKeySequence("Ctrl+Alt+P"));}
    void modalDialogDefersRepair(){config(true);DolphinMainWindow window;DolphinWatchdog::Monitor monitor;monitor.check();window.recreate();QDialog dialog(&window);dialog.setModal(true);dialog.show();QCoreApplication::processEvents();monitor.check();QVERIFY(window.paste()->shortcuts().isEmpty());dialog.close();QCoreApplication::processEvents();monitor.check();QCOMPARE(window.paste()->shortcut(),QKeySequence("Ctrl+V"));}
    void missingActionIsNotReportedHealthy(){config(true);DolphinMainWindow window;auto *action=window.paste();window.actionCollection()->takeAction(action);delete action;DolphinWatchdog::Monitor monitor;monitor.check();QVERIFY(!QJsonDocument::fromJson(monitor.status().toUtf8()).object()["healthy"].toBool());}
    void removingIntegrationRestoresNative(){config(true);DolphinMainWindow window;DolphinWatchdog::Monitor monitor;monitor.check();config(false);QFile::remove(data+"/gad/dolphin-paste.json");monitor.check();QVERIFY(window.paste()->shortcuts().isEmpty());QVERIFY(window.actionCollection()->action(DolphinWatchdog::paste)->shortcuts().contains(QKeySequence("Ctrl+V")));}
    void textPastingRemainsNative(){config(true);DolphinMainWindow window;auto *body=new QWidget;auto *layout=new QVBoxLayout(body);auto *view=new QWidget;view->setFocusPolicy(Qt::StrongFocus);view->setMinimumSize(200,50);auto *edit=new QLineEdit;layout->addWidget(view);layout->addWidget(edit);window.setCentralWidget(body);window.addAction(window.paste());int calls=0;connect(window.paste(),&QAction::triggered,&window,[&]{++calls;});DolphinWatchdog::Monitor monitor;monitor.check();window.show();window.activateWindow();view->setFocus();QCoreApplication::processEvents();QTest::keyClick(view,Qt::Key_V,Qt::ControlModifier);QCOMPARE(calls,1);QApplication::clipboard()->setText("test text");edit->setFocus();QCoreApplication::processEvents();QTest::keyClick(edit,Qt::Key_V,Qt::ControlModifier);QCOMPARE(edit->text(),QString("test text"));QCOMPARE(calls,1);}
    void invalidReceiptDoesNotModifyActions(){config(true);QFile receipt(data+"/gad/dolphin-paste.json");QVERIFY(receipt.open(QIODevice::WriteOnly));receipt.write("invalid");receipt.close();DolphinMainWindow window;DolphinWatchdog::Monitor monitor;monitor.check();QVERIFY(window.paste()->shortcuts().isEmpty());}
};
QTEST_MAIN(WatchdogTest)
#include "dolphin_watchdog.moc"
