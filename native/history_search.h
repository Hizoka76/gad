#pragma once
// Recherche de présentation : ne change ni la file, ni les fichiers, ni les options des profils.
#include "presentation.h"

namespace HistorySearch {
constexpr int CreatedRole=Qt::UserRole+18;
constexpr int JobStateRole=Qt::UserRole+19;
constexpr int PathsRole=Qt::UserRole+20;
struct Query {
    bool enabled=false,historyOnly=true,dates=false;
    QString text,state;
    QDate from,to;
    /// Les mots se combinent par ET ; les chemins complets restent littéraux (aucune expression régulière).
    bool matches(const QTreeWidgetItem *row) const {
        if(!enabled)return true;
        if(historyOnly && !finished(row->data(0,JobStateRole).toString()))return false;
        if(!state.isEmpty() && row->data(0,JobStateRole).toString()!=state)return false;
        if(dates) {
            const auto seconds=row->data(0,CreatedRole).toLongLong();
            if(seconds<=0 || !from.isValid() || !to.isValid())return false;
            const auto date=QDateTime::fromSecsSinceEpoch(seconds).date();
            if(date<from || date>to)return false;
        }
        const auto paths=row->data(0,PathsRole).toString().normalized(QString::NormalizationForm_C).toCaseFolded();
        for(const auto &word:text.normalized(QString::NormalizationForm_C).toCaseFolded().split(QRegularExpression("\\s+"),Qt::SkipEmptyParts))
            if(!paths.contains(word))return false;
        return true;
    }
};
}
