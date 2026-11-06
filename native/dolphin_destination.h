#pragma once
#include "i18n.h"
// Destination du raccourci : aucun accès ni modification au presse-papiers, aucune déduction depuis l’ancienne sélection.
#include <QtCore>
#include <QtDBus>
#include "kio_sources.h"

namespace DolphinDestination {
struct Result {QString destination,error;};
/// Interface injectable pour tester les changements de fenêtre et les réponses D-Bus sans toucher au bureau.
using Call=std::function<QDBusMessage(const QString &,const QString &,const QString &,const QString &,const QVariantList &)>;

/// Le titre complet est fourni par Dolphin à partir de l’URL de la vue active ; valider qu’elle est encore ouverte.
inline Result resolve(const Call &call) {
    const auto names=call("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","ListNames",{});
    if(names.type()==QDBusMessage::ErrorMessage || names.arguments().isEmpty())return {{},GAD_TR("Unable to query Dolphin windows.")};
    const auto services=qdbus_cast<QStringList>(names.arguments()[0]);
    static const QRegularExpression process("^org\\.kde\\.dolphin(?:-[0-9]+)?$"),window("^Dolphin_[0-9]+$");
    QString activeService,activePath;QSet<QString> owners;
    for(const auto &service:services) {
        if(!process.match(service).hasMatch())continue;
        const auto owner=call("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","GetNameOwner",{service});
        if(owner.arguments().isEmpty() || owner.type()==QDBusMessage::ErrorMessage)continue;
        const auto unique=owner.arguments()[0].toString();if(owners.contains(unique))continue;owners.insert(unique);
        const auto reply=call(service,"/dolphin","org.freedesktop.DBus.Introspectable","Introspect",{});
        if(reply.type()==QDBusMessage::ErrorMessage || reply.arguments().isEmpty())continue;
        QXmlStreamReader xml(reply.arguments()[0].toString());
        while(!xml.atEnd()) {
            xml.readNext();if(!xml.isStartElement() || xml.name()!=QStringLiteral("node"))continue;
            const auto name=xml.attributes().value("name").toString();if(!window.match(name).hasMatch())continue;
            const auto path="/dolphin/"+name;
            const auto active=call(service,path,"org.kde.dolphin.MainWindow","isActiveWindow",{});
            if(active.type()==QDBusMessage::ErrorMessage || active.arguments().isEmpty() || !active.arguments()[0].toBool())continue;
            if(!activePath.isEmpty())return {{},GAD_TR("Multiple Dolphin windows appear to be active. Paste again from the target window.")};
            activeService=service;activePath=path;
        }
    }
    if(activePath.isEmpty())return {{},GAD_TR("No active Dolphin window. Start pasting from the destination folder in Dolphin.")};
    const auto title=[&]() {
        const auto reply=call(activeService,activePath,"org.freedesktop.DBus.Properties","Get",{"org.qtproject.Qt.QWidget","windowTitle"});
        return reply.type()!=QDBusMessage::ErrorMessage && !reply.arguments().isEmpty()?qdbus_cast<QDBusVariant>(reply.arguments()[0]).variant().toString():QString();
    };
    const auto first=title();QUrl url;
    if(QDir::isAbsolutePath(first))url=QUrl::fromLocalFile(first);
    else if(SourceUrls::isUrl(first))url=QUrl(first,QUrl::TolerantMode);
    if(!SourceUrls::supported(url))return {{},GAD_TR("Dolphin did not provide a usable full path. Enable the full path in its title bar, or use Paste with Gad from the context menu.")};
    const auto open=call(activeService,activePath,"org.kde.dolphin.MainWindow","isUrlOpen",{url.toString(QUrl::FullyEncoded)});
    if(open.type()==QDBusMessage::ErrorMessage || open.arguments().isEmpty() || !open.arguments()[0].toBool())return {{},GAD_TR("The folder provided by Dolphin is no longer open. Paste again.")};
    const auto stillActive=call(activeService,activePath,"org.kde.dolphin.MainWindow","isActiveWindow",{});
    if(stillActive.type()==QDBusMessage::ErrorMessage || stillActive.arguments().isEmpty() || !stillActive.arguments()[0].toBool() || title()!=first)
        return {{},GAD_TR("The active folder changed while pasting. Try again from the intended destination.")};
    return {url.isLocalFile()?SourceUrls::localPath(url):url.toString(QUrl::FullyEncoded),{}};
}

/// Le module Konqueror expose l’URL de sa vue active, indépendamment du titre ou de la sélection.
inline Result konqueror(const Call &call) {
    const auto reply=call("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","ListNames",{});
    if(reply.type()==QDBusMessage::ErrorMessage || reply.arguments().isEmpty())return {};
    QString target;
    for(const auto &service:qdbus_cast<QStringList>(reply.arguments()[0])) {
        if(!service.startsWith("org.hizoka.Gad.Konqueror.p"))continue;
        const auto first=call(service,"/GadKonqueror","org.hizoka.Gad.Konqueror","currentDirectory",{});
        if(first.type()==QDBusMessage::ErrorMessage || first.arguments().isEmpty() || first.arguments()[0].toString().isEmpty())continue;
        const auto url=QUrl(first.arguments()[0].toString());if(!SourceUrls::supported(url))continue;
        const auto second=call(service,"/GadKonqueror","org.hizoka.Gad.Konqueror","currentDirectory",{});
        if(second.type()==QDBusMessage::ErrorMessage || second.arguments().isEmpty() || second.arguments()[0].toString()!=first.arguments()[0].toString())
            return {{},GAD_TR("The active folder changed while pasting. Try again from the intended destination.")};
        if(!target.isEmpty())return {{},GAD_TR("The active folder changed while pasting. Try again from the intended destination.")};
        target=url.isLocalFile()?SourceUrls::localPath(url):url.toString(QUrl::FullyEncoded);
    }
    return {target,{}};
}

/// Les appels sont bornés au total ; un Dolphin bloqué ne doit pas choisir un autre dossier par défaut.
inline Result current(const Call &transport={}) {
    // Valider le titre réellement exposé : QSettings ne lit pas le groupe KDE [General]
    // comme KConfig et le fichier peut aussi différer des réglages chargés par Dolphin.
    QElapsedTimer deadline;deadline.start();
    const Call bounded=[&](const QString &service,const QString &path,const QString &interface,const QString &method,const QVariantList &arguments) {
        const auto remaining=3000-int(deadline.elapsed());
        if(remaining<=0)return QDBusMessage::createError("org.hizoka.Gad.Timeout",GAD_TR("Timed out reading Dolphin"));
        if(transport)return transport(service,path,interface,method,arguments);
        auto message=QDBusMessage::createMethodCall(service,path,interface,method);message.setArguments(arguments);
        return QDBusConnection::sessionBus().call(message,QDBus::Block,qMin(remaining,500));
    };
    const auto target=konqueror(bounded);
    if(!target.destination.isEmpty() || !target.error.isEmpty())return target;
    return resolve(bounded);
}
}
