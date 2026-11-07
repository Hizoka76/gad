// Instrumentation du seul Dolphin de test, sous un bus et des répertoires privés.
#include <QtWidgets>
#include <KConfig>
#include <QtDBus>
#include <QtTest>
#include <KConfigGroup>
#include <cstdio>
static void dump(const char *stage){
 for(auto *w:QApplication::topLevelWidgets()){
  if(!QString::fromLatin1(w->metaObject()->className()).contains("Dolphin"))continue;
  for(auto *a:w->findChildren<QAction *>())if(a->objectName().contains("GadPaste") || a->objectName()=="edit_paste"){
   QStringList keys;for(auto k:a->shortcuts())keys<<k.toString();
   std::fprintf(stderr,"PROBE %s %s %s\n",stage,a->objectName().toUtf8().constData(),keys.join(';').toUtf8().constData());
  }
 }
}
static void startup(){QTimer::singleShot(2000,[]{dump("before");KConfig config("kservicemenurc");KConfigGroup group(&config,"Show");group.writeEntry("GadProbe",true,KConfigBase::Notify);config.sync();QTimer::singleShot(2500,[]{dump("after");
for(auto *window:QApplication::topLevelWidgets())if(QString::fromLatin1(window->metaObject()->className()).contains("DolphinMainWindow")){
 window->activateWindow();window->setFocus();QCoreApplication::processEvents();QTest::keyClick(window,Qt::Key_V,Qt::ControlModifier);
}
QTimer::singleShot(500,[]{QString status;auto *monitor=QDBusConnection::sessionBus().objectRegisteredAt("/GadDolphinWatchdog");if(monitor)QMetaObject::invokeMethod(monitor,"status",Qt::DirectConnection,Q_RETURN_ARG(QString,status));std::fprintf(stderr,"WATCHDOG %s\n",status.toUtf8().constData());qApp->quit();});});});}
Q_COREAPP_STARTUP_FUNCTION(startup)
