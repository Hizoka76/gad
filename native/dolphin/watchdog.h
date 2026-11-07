#pragma once
// Contrôle les QAction réelles de Dolphin ; aucune lecture ni manipulation du presse-papiers.
#include <QtWidgets>
#include <QtDBus>
#include <KXmlGuiWindow>
#include <KActionCollection>

namespace DolphinWatchdog {
inline const QString current="servicemenu_gad-paste-current.desktop::GadPasteCurrent";
inline const QString context="servicemenu_gad-paste.desktop::GadPaste";
inline const QString paste="edit_paste";

struct Configuration {
    bool valid=false,enabled=false;
    QHash<QString,QList<QKeySequence>> shortcuts;
};

/// Lire seulement les fichiers de configuration attendus dans les données du compte courant.
inline Configuration configuration() {
    const auto data=QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QFile receipt(data+"/gad/dolphin-paste.json");
    Configuration result;
    QJsonObject files;
    if(receipt.exists()) {
        if(!receipt.open(QIODevice::ReadOnly) || receipt.size()>1024*1024)return result;
        QJsonParseError error;const auto state=QJsonDocument::fromJson(receipt.readAll(),&error).object();
        if(error.error!=QJsonParseError::NoError || state["version"].toInt()!=2 || !state["files"].isObject())return result;
        files=state["files"].toObject();
    }
    result.enabled=!files.isEmpty();
    const QStringList candidates{data+"/kxmlgui5/dolphin/dolphinui.rc",data+"/kxmlgui6/dolphin/dolphinui.rc"};
    for(auto it=files.begin();it!=files.end();++it)if(!candidates.contains(it.key()))return Configuration();
    for(const auto &path:candidates) {
        if(result.enabled && !files.contains(path))continue;
        QFile file(path);if(!file.open(QIODevice::ReadOnly) || file.size()>4*1024*1024)continue;
        QXmlStreamReader xml(&file);bool group=false;int groups=0;
        while(!xml.atEnd()) {
            xml.readNext();
            if(xml.isStartElement() && xml.name()==QStringLiteral("ActionProperties")){
                group=xml.attributes().value("scheme").isEmpty() || xml.attributes().value("scheme")==QStringLiteral("Default");if(group)++groups;
            } else if(xml.isEndElement() && xml.name()==QStringLiteral("ActionProperties"))group=false;
            else if(group && xml.isStartElement() && xml.name()==QStringLiteral("Action")) {
                const auto name=xml.attributes().value("name").toString();
                if(name!=current && name!=context && name!=paste)continue;
                if(!xml.attributes().hasAttribute("shortcut"))continue;
                if(result.shortcuts.contains(name))return Configuration();
                QList<QKeySequence> keys;
                for(const auto &part:xml.attributes().value("shortcut").toString().split(';',Qt::SkipEmptyParts)) {
                    const auto key=QKeySequence::fromString(part.trimmed(),QKeySequence::PortableText);if(!key.isEmpty())keys.append(key);
                }
                result.shortcuts[name]=keys;
            }
        }
        if(xml.hasError() || groups>1)return Configuration();
        // Dolphin charge prioritairement son fichier kxmlgui5 ; ne pas mélanger deux schémas.
        result.valid=true;break;
    }
    if(result.enabled)result.valid=result.valid && result.shortcuts.value(current)==QList<QKeySequence>{QKeySequence(Qt::CTRL|Qt::Key_V)} && result.shortcuts.value(paste)==QList<QKeySequence>{QKeySequence(Qt::SHIFT|Qt::Key_Insert)};
    return result;
}

/// Le groupe KIO perd aussi son contexte lors d’un rafraîchissement : exécuter Gad directement.
class Route:public QObject {
    QPointer<QActionGroup> originalGroup;
    QMetaObject::Connection connection;
public:
    explicit Route(QAction *action,std::function<void(bool)> launched):QObject(action),originalGroup(action->actionGroup()) {
        setObjectName("_gadDolphinPasteRoute");
        if(originalGroup)originalGroup->removeAction(action);
        connection=connect(action,&QAction::triggered,this,[launched]{
            auto program=qEnvironmentVariable("GAD_DOLPHIN_EXECUTABLE");if(program.isEmpty())program=QStandardPaths::findExecutable("gad");
            launched(!program.isEmpty() && QProcess::startDetached(program,{"--paste-current"}));
        });
    }
    void restore(QAction *action){disconnect(connection);if(originalGroup)originalGroup->addAction(action);}
};

/// Réparer uniquement les trois actions gérées par Gad et respecter les choix enregistrés.
inline int reconcile(QWidget *window,const Configuration &config,std::function<void(bool)> launched={}) {
    int changed=0;
    auto *main=qobject_cast<KXmlGuiWindow *>(window);if(!main)return changed;
    for(const auto &name:QStringList{current,context,paste}) {
        auto *action=main->actionCollection()->action(name);if(!action)continue;
        auto desired=config.shortcuts.value(name);
        if(!config.shortcuts.contains(name))desired=action->property("defaultShortcuts").value<QList<QKeySequence>>();
        if(name==paste && !config.shortcuts.contains(name) && desired.isEmpty())desired=QKeySequence::keyBindings(QKeySequence::Paste);
        if(action->shortcuts()!=desired){action->setShortcuts(desired);++changed;}
        if(name==current) {
            auto *existing=action->findChild<QObject *>("_gadDolphinPasteRoute",Qt::FindDirectChildrenOnly);
            if(config.enabled && !existing && launched)new Route(action,launched);
            else if(!config.enabled && existing){static_cast<Route *>(existing)->restore(action);delete existing;}
        }
    }
    return changed;
}

class Monitor:public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface","org.hizoka.Gad.DolphinWatchdog")
    bool previouslyEnabled=false;
    qint64 checks=0,repairs=0,launches=0,launchErrors=0;
    int windows=0;bool enabled=false,healthy=false,lastLaunchOk=true;
public:
    explicit Monitor(QObject *parent=nullptr):QObject(parent) {
        auto *timer=new QTimer(this);timer->setInterval(1000);
        connect(timer,&QTimer::timeout,this,&Monitor::check);timer->start();
        auto bus=QDBusConnection::sessionBus();
        bus.registerService("org.hizoka.Gad.DolphinWatchdog.p"+QString::number(QCoreApplication::applicationPid()));
        bus.registerObject("/GadDolphinWatchdog",this,QDBusConnection::ExportAllSlots);
    }
    /// Chaque contrôle relit la préférence ; la désactivation restaure les raccourcis natifs.
    void check() {
        if(QCoreApplication::applicationName()!="dolphin" || !qobject_cast<QApplication *>(QCoreApplication::instance()))return;
        ++checks;healthy=false;windows=0;
        if(QApplication::activeModalWidget())return;
        const auto config=configuration();enabled=config.enabled;
        if(!config.valid)return;
        bool actionsHealthy=true;
        for(auto *window:QApplication::topLevelWidgets()) {
            if(!QString::fromLatin1(window->metaObject()->className()).contains("DolphinMainWindow"))continue;
            auto *main=qobject_cast<KXmlGuiWindow *>(window);if(!main || !main->actionCollection()->action(current))continue;
            ++windows;
            if(config.enabled || previouslyEnabled)repairs+=reconcile(window,config,[guard=QPointer<Monitor>(this)](bool success){if(guard){guard->lastLaunchOk=success;if(success)++guard->launches;else ++guard->launchErrors;}});
            auto *native=main->actionCollection()->action(paste);
            actionsHealthy &= main->actionCollection()->action(current)->shortcuts()==config.shortcuts.value(current) && native && native->shortcuts()==config.shortcuts.value(paste);
        }
        previouslyEnabled=config.enabled;
        auto program=qEnvironmentVariable("GAD_DOLPHIN_EXECUTABLE");if(program.isEmpty())program=QStandardPaths::findExecutable("gad");
        const QFileInfo executable(program);
        healthy=config.enabled && windows>0 && actionsHealthy && executable.isFile() && executable.isExecutable() && lastLaunchOk;
    }
public Q_SLOTS:
    QString status() const {
        return QString::fromUtf8(QJsonDocument(QJsonObject{{"enabled",enabled},{"healthy",healthy},{"windows",windows},{"checks",checks},{"repairs",repairs},{"launches",launches},{"launch_errors",launchErrors},{"interval_ms",1000}}).toJson(QJsonDocument::Compact));
    }
};
}
