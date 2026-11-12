#pragma once
#include "i18n.h"
// Adaptation des URL KDE Connect, FTP, SFTP et SMB vers le système de fichiers exposé par KIO Fuse.
// La résolution est asynchrone : la fenêtre et les transferts déjà actifs restent réactifs.
#include <QtCore>
#include <QtDBus>
#include <functional>

namespace SourceUrls {
/// Distingue une URL d’un chemin POSIX, qui peut lui-même contenir des caractères deux-points.
inline bool isUrl(const QString &value) {
    // Un marqueur au milieu d’un chemin relatif ne définit pas un schéma d’URL.
    static const QRegularExpression prefix(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*://"));
    return !QDir::isAbsolutePath(value) && prefix.match(value).hasMatch();
}
/// Protocoles distants délégués à KIO Fuse, y compris leur gestion de connexion existante.
inline bool remoteScheme(const QString &scheme) { return scheme=="kdeconnect" || scheme=="ftp" || scheme=="sftp" || scheme=="smb"; }
/// Message partagé par le presse-papiers, le glisser-déposer et la préparation des sources.
inline QString unsupportedMessage() { return GAD_TR("Use a local file or a kdeconnect://, ftp://, sftp://, or smb:// address."); }
/// Retire le mot de passe des libellés persistants sans changer l’URL transmise à KIO.
inline QString origin(const QString &value) {
    if(!isUrl(value))return value;
    auto url=QUrl::fromEncoded(value.toUtf8(),QUrl::StrictMode);
    url.setPassword(QString());
    return url.toString(QUrl::FullyEncoded);
}
/// Accepte les sources locales et les protocoles distants prévus, sans requête ni fragment ambigu.
inline bool supported(const QUrl &url) {
    if(!url.isValid() || url.hasQuery() || url.hasFragment())return false;
    // Le pont JSON/POSIX ne doit jamais tronquer un nom distant ni remplacer ses octets non UTF-8.
    const auto pathBytes=QByteArray::fromPercentEncoding(url.path(QUrl::FullyEncoded).toUtf8());
    if(pathBytes.contains('\0') || QString::fromUtf8(pathBytes).toUtf8()!=pathBytes)return false;
    if(url.isLocalFile()) {
        if((!url.host().isEmpty() && url.host()!="localhost") || !url.userInfo().isEmpty() || url.port()!=-1)return false;
        // Refuser les octets non UTF-8 avant leur conversion en QString, qui pourrait désigner un autre fichier.
        const auto bytes=QByteArray::fromPercentEncoding(url.path(QUrl::FullyEncoded).toUtf8());
        return QDir::isAbsolutePath(url.path()) && !bytes.contains('\0') && QString::fromUtf8(bytes).toUtf8()==bytes;
    }
    if(!remoteScheme(url.scheme()) || url.host().isEmpty())return false;
    if(url.scheme()=="kdeconnect")return url.userInfo().isEmpty() && url.port()==-1;
    return true;
}
/// localhost est un hôte local, pas un préfixe UNC sur Linux ; ne jamais résoudre la cible d’un lien ici.
inline QString localPath(QUrl url) {
    if(url.host()=="localhost")url.setHost(QString());
    return url.toLocalFile();
}
/// Conserve les URL invalides pour leur rejet explicite avant toute conversion en chemin de fichier.
inline QString argument(const QString &value) {
    if(value.isEmpty())return value;
    if(!isUrl(value))return QFileInfo(value).absoluteFilePath();
    const auto url=QUrl::fromEncoded(value.toUtf8(),QUrl::StrictMode);
    if(!supported(url))return value;
    return url.isLocalFile()?localPath(url):url.toString(QUrl::FullyEncoded);
}
/// Détecte les requêtes nécessitant l’adaptateur avant de les transmettre au moteur POSIX.
inline bool needsResolution(const QJsonObject &request) {
    for(const auto &value:request["sources"].toArray())if(isUrl(value.toString()))return true;
    return isUrl(request["destination"].toString()) || request.contains("destination_context");
}

/// Exige une instance connaissant le protocole distant demandé.
inline int requiredProtocol(const QJsonObject &request) {
    int version=(isUrl(request["destination"].toString()) || request.contains("destination_context"))?12:0;
    for(const auto &value:request["sources"].toArray()) {
        if(!isUrl(value.toString()))continue;
        const auto scheme=QUrl::fromEncoded(value.toString().toUtf8(),QUrl::StrictMode).scheme();
        version=qMax(version,scheme=="smb"?9:(scheme=="ftp" || scheme=="sftp"?8:7));
    }
    if(request["gui_settings"].toObject().contains("completion_action") || request["gui_settings"].toObject().contains("completion_delay"))version=qMax(version,31);
    if(request["gui_settings"].toObject()["completion_action"]=="clear")version=qMax(version,35);
    if(request["gui_settings"].toObject().contains("progressMessage"))version=qMax(version,34);
    if(request["prepare_only"].toBool() || request["command"]=="start_prepared")version=qMax(version,33);
    if(request.contains("verify_fast_move"))version=qMax(version,30);
    if(request.contains("conflict_policy"))version=qMax(version,27);
    if(request.contains("profile"))version=qMax(version,26);
    if(request.contains("rate_limit") || request["gui_settings"].toObject().contains("request_rate_limit"))version=qMax(version,25);
    if(request.contains("allow_duplication"))version=qMax(version,22);
    if(request.contains("exclusions"))version=qMax(version,21);
    if(request["verify_space"].toBool())version=qMax(version,14);
    if(request["verify_properties"].toBool())version=qMax(version,13);
    return version;
}

/// Résout tout le lot avant soumission : un échec ne copie pas seulement une partie de la sélection.
class Resolver : public QObject {
    // Requête de départ : options du lot figées avant toute attente réseau.
    QJsonObject request;
    // Les deux tableaux restent dans le même ordre pour relier chemins de travail et URL affichées.
    QJsonArray original, inputs, resolved;
    QString destinationInput;
    bool hasDestination=false;
    QJsonArray context;
    int position=0;
    bool remote=false;
    // Délai commun au lot afin qu’une sélection ne multiplie pas indéfiniment les attentes.
    QElapsedTimer deadline;
    std::function<void(QJsonObject)> done;
    /// Termine l’adaptation et libère l’objet après livraison de la réponse.
    void finish(QJsonObject result) { auto callback=std::move(done);deleteLater();callback(std::move(result)); }
    /// Passe à la source suivante ; seul KIO décide du montage utilisé pour la source distante.
    void next() {
        // Consommer les chemins locaux dans une boucle évite une pile proportionnelle à la sélection.
        while(position<inputs.size()) {
            const bool isDestination=hasDestination && position>=original.size();
            const auto input=inputs[position++].toString();
            if(!isUrl(input)) { resolved.append(input);continue; }
            const auto url=QUrl::fromEncoded(input.toUtf8(),QUrl::StrictMode);
            if(url.isLocalFile()) { resolved.append(localPath(url));continue; }
            if(!isDestination)remote=true;
            const int remaining=30000-int(deadline.elapsed());
            if(remaining<=0){finish({{"error",GAD_TR("Remote access is taking too long to respond. Check the connection.")}});return;}
            auto message=QDBusMessage::createMethodCall("org.kde.KIOFuse","/org/kde/KIOFuse","org.kde.KIOFuse.VFS","mountUrl");
            message<<url.toString(QUrl::FullyEncoded);
            auto *watcher=new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message,remaining),this);
            connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,watcher] {
                QDBusPendingReply<QString> reply=*watcher;watcher->deleteLater();
                if(reply.isError()) {
                    finish({{"error",GAD_TR("Unable to access remote files through KIO Fuse: ")+reply.error().message()+GAD_TR("\nCheck the connection and whether the kio-fuse package is installed.")}});return;
                }
                if(!QDir::isAbsolutePath(reply.value())) {finish({{"error",GAD_TR("KIO Fuse did not provide a valid file path.")}});return;}
                resolved.append(reply.value());next();
            });
            return;
        }
        if(hasDestination) {
            QString target;
            if(context.isEmpty())target=resolved.takeAt(resolved.size()-1).toString();
            else {
                const bool directory=context.size()==1 && QFileInfo(resolved[original.size()].toString()).isDir();
                for(int i=0;i<context.size();++i) {
                    const auto item=resolved[original.size()+i].toString();
                    const auto parent=directory?item:QFileInfo(item).absolutePath();
                    if(!target.isEmpty() && parent!=target) {
                        finish({{"error",GAD_TR("The selection does not share a single destination folder")}});return;
                    }
                    target=parent;
                }
                destinationInput=context[0].toString();
                if(!directory && isUrl(destinationInput))destinationInput=QUrl(destinationInput).adjusted(QUrl::RemoveFilename|QUrl::StripTrailingSlash).toString(QUrl::FullyEncoded);
                while(resolved.size()>original.size())resolved.removeLast();
            }
            request["destination"]=target;
            request.remove("destination_context");
            // L’adresse affichée ne contient jamais le mot de passe fourni au service KDE.
            if(isUrl(destinationInput) && !QUrl(destinationInput).isLocalFile())
                request["destination_url"]=origin(destinationInput);
            else request.remove("destination_url");
        }
        request["sources"]=resolved;
        if(remote) {
            QJsonArray labels;for(const auto &value:original)labels.append(origin(value.toString()));
            request["source_urls"]=labels;
        }
        finish(request);
    }
public:
    /// Le parent Qt maintient la résolution en vie pendant les réponses D-Bus.
    Resolver(QJsonObject value,QObject *parent,std::function<void(QJsonObject)> callback)
        :QObject(parent),request(std::move(value)),original(request["sources"].toArray()),done(std::move(callback)){}
    /// Valide toute la liste avant de demander un montage distant.
    void start() {
        for(const auto &value:original) {
            if(!value.isString() || value.toString().isEmpty()) {finish({{"error",GAD_TR("Invalid copy source.")}});return;}
            if(isUrl(value.toString()) && !supported(QUrl::fromEncoded(value.toString().toUtf8(),QUrl::StrictMode))) {
                finish({{"error",unsupportedMessage()}});return;
            }
        }
        hasDestination=request.contains("destination");
        destinationInput=request["destination"].toString();
        if(hasDestination && (destinationInput.isEmpty() || (isUrl(destinationInput) && !supported(QUrl::fromEncoded(destinationInput.toUtf8(),QUrl::StrictMode))))) {
            finish({{"error",unsupportedMessage()}});return;
        }
        context=request.value("destination_context").toArray();
        if(request.contains("destination_context") && context.isEmpty()) {finish({{"error",GAD_TR("Empty destination context")}});return;}
        for(const auto &item:context) {
            if(!item.isString() || item.toString().isEmpty() || (isUrl(item.toString()) && !supported(QUrl::fromEncoded(item.toString().toUtf8(),QUrl::StrictMode)))) {
                finish({{"error",unsupportedMessage()}});return;
            }
        }
        inputs=original;
        if(hasDestination) {
            if(context.isEmpty())inputs.append(destinationInput);
            else for(const auto &item:context)inputs.append(item);
        }
        deadline.start();next();
    }
};
}
