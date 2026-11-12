#pragma once
// Recherche de présentation : ne change ni la file, ni les fichiers, ni les options des profils.
#include "presentation.h"

namespace HistorySearch {
constexpr int CreatedRole=Qt::UserRole+18;
constexpr int JobStateRole=Qt::UserRole+19;
constexpr int PathsRole=Qt::UserRole+20;
constexpr int NormalizedPathsRole=Qt::UserRole+21;
constexpr int CachedPathsRole=Qt::UserRole+22;
/// Les chemins normalisés ne changent pas pendant une simple progression.
inline void setPaths(QTreeWidgetItem *row,const QString &paths) {
    if(row->data(0,PathsRole).toString()==paths && row->data(0,CachedPathsRole)==QVariant(paths))return;
    row->setData(0,PathsRole,paths);
    row->setData(0,CachedPathsRole,paths);
    row->setData(0,NormalizedPathsRole,paths.normalized(QString::NormalizationForm_C).toCaseFolded());
}
struct Query {
    bool enabled=false,historyOnly=true,dates=false;
    QString text,state;
    QDate from,to;
    // Le texte public peut changer directement ; sa valeur invalide explicitement le cache.
    mutable QString compiledText;
    mutable QStringList words;
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
        if(compiledText!=text) {
            static const QRegularExpression spaces("\\s+");
            compiledText=text;words=text.normalized(QString::NormalizationForm_C).toCaseFolded().split(spaces,Qt::SkipEmptyParts);
        }
        if(words.isEmpty())return true;
        const auto raw=row->data(0,PathsRole).toString();
        const auto paths=row->data(0,CachedPathsRole)==QVariant(raw)?row->data(0,NormalizedPathsRole).toString():raw.normalized(QString::NormalizationForm_C).toCaseFolded();
        for(const auto &word:words)
            if(!paths.contains(word))return false;
        return true;
    }
};
}
