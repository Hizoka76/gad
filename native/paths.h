#pragma once
#include "i18n.h"
// Les destinations distantes restent des URL jusqu’à leur résolution asynchrone par KIO Fuse.
#include "kio_sources.h"
namespace LocalPaths {
/// Normalise les destinations de la fenêtre et de la CLI sans interpréter une URL distante comme un chemin POSIX.
inline QString destination(const QString &value, QString &error) {
    error.clear();QString path=value;
    if(SourceUrls::isUrl(value)) {
        const auto url=QUrl::fromEncoded(value.toUtf8(),QUrl::StrictMode);
        if(!SourceUrls::supported(url)) {
            error=SourceUrls::unsupportedMessage();return {};
        }
        if(!url.isLocalFile())return url.toString(QUrl::FullyEncoded);
        path=SourceUrls::localPath(url);
    }
    if(path.isEmpty()){error=GAD_TR("The destination is empty.");return {};}
    return QFileInfo(path).absoluteFilePath();
}
}
