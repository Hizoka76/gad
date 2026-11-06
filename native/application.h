#pragma once
// Conversion de la ligne de commande vers les commandes de l’instance unique.
#include <QtCore>
QJsonObject parse(const QStringList &args, QString &error);
