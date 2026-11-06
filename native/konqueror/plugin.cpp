// Module propre à Konqueror ; le moteur de transfert reste dans le processus Gad.
#include <konq_kpart_plugin.h>
#include <KPluginFactory>
#include <QtDBus>
#include "paste_filter.h"

/// Le menu « dossier ouvert » peut demander la destination au processus réellement actif.
class GadKonquerorContext : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.hizoka.Gad.Konqueror")
public:
    using QObject::QObject;
public Q_SLOTS:
    QString currentDirectory() const {
        auto *window=qApp->activeWindow();
        if(!window || !window->isActiveWindow())return {};
        return KonquerorPaste::destination(window).toString(QUrl::FullyEncoded);
    }
};

class GadKonquerorPlugin : public KonqParts::Plugin {
    Q_OBJECT
public:
    GadKonquerorPlugin(QObject *parent,const QVariantList &):KonqParts::Plugin(parent) {
        static QPointer<GadKonquerorContext> context;
        if(!context) {
            context=new GadKonquerorContext(qApp);
            auto bus=QDBusConnection::sessionBus();
            bus.registerService("org.hizoka.Gad.Konqueror.p"+QString::number(QCoreApplication::applicationPid()));
            bus.registerObject("/GadKonqueror",context,QDBusConnection::ExportAllSlots);
        }
        new KonquerorPaste::Filter(this,[](const QUrl &target){
            return QProcess::startDetached(QString::fromUtf8(GAD_BINARY),{"--paste",target.toString(QUrl::FullyEncoded)});
        },[]{QSettings settings("Gad","Gad");settings.sync();return settings.value("konquerorCtrlV",true).toBool();});
    }
};
K_PLUGIN_CLASS_WITH_JSON(GadKonquerorPlugin,"plugin.json")
#include "plugin.moc"
