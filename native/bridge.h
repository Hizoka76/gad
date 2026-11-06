#pragma once
// Frontière JSON avec le moteur Rust ; chaque réponse est libérée par son allocateur.
#include <QtCore>
extern "C" char *sc_request(const char *);
extern "C" void sc_free(char *);

/// Envoie du JSON au moteur Rust et libère sa réponse avec l’allocateur qui l’a créée.
inline QJsonObject engine(const QJsonObject &request) {
    const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
    char *response = sc_request(bytes.constData());
    const auto result = QJsonDocument::fromJson(response).object();
    sc_free(response);
    return result;
}
