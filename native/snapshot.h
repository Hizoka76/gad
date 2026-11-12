#pragma once
// Cache local des lots : seuls les lots révisés traversent la frontière Rust/Qt.
#include <QtCore>

class QueueSnapshot {
    QJsonObject current;
    QHash<qint64,QJsonObject> jobs;
    qint64 version=-1;
public:
    /// -1 demande un premier état complet ; les révisions ne sont jamais persistées.
    qint64 revision() const { return version; }
    const QJsonObject &state() const { return current; }
    /// Fusionne une réponse différentielle en conservant l’ordre et les objets inchangés.
    QSet<qint64> apply(const QJsonObject &delta) {
        QSet<qint64> changed;
        if(delta["unchanged"].toBool()){if(delta.contains("diagnostics"))current["diagnostics"]=delta["diagnostics"];if(delta.contains("queue_guard"))current["queue_guard"]=delta["queue_guard"];return changed;}
        // Le repli complet permet aussi d’utiliser un double de moteur ou un client historique.
        const bool legacy=!delta.contains("revision");
        auto nextJobs=legacy || delta["reset"].toBool()?QHash<qint64,QJsonObject>{}:jobs;
        QJsonArray ids=delta["job_ids"].toArray();
        for(const auto &value:delta["jobs"].toArray()) {
            const auto job=value.toObject();const auto id=job["id"].toInteger();
            nextJobs[id]=job;changed.insert(id);if(legacy)ids.append(id);
        }
        // Une anomalie impose un état complet au prochain poll, sans publier de fusion partielle.
        for(const auto &value:delta["job_patches"].toArray()) {
            const auto patch=value.toObject();const auto id=patch["id"].toInteger();
            if(!nextJobs.contains(id)){version=-1;return {};}
            auto job=nextJobs.value(id);
            if(patch.contains("rate_limit"))job["rate_limit"]=patch["rate_limit"];
            if(patch.contains("entries")) {
                auto entries=job["entries"].toArray();
                for(const auto &change:patch["entries"].toArray()) {
                    const auto update=change.toObject();const int index=update["index"].toInt(-1);
                    if(index<0 || index>=entries.size()){version=-1;return {};}
                    auto entry=entries[index].toObject();
                    for(const auto *field:{"copied","verified","resume_checked"})if(update.contains(field))entry[field]=update[field];
                    entries[index]=entry;
                }
                job["entries"]=entries;
            }
            nextJobs[id]=job;changed.insert(id);
        }
        jobs=std::move(nextJobs);
        QJsonArray ordered;QSet<qint64> live;
        for(const auto &value:ids){const auto id=value.toInteger();live.insert(id);if(jobs.contains(id))ordered.append(jobs[id]);}
        for(auto it=jobs.begin();it!=jobs.end();) {if(!live.contains(it.key()))it=jobs.erase(it);else ++it;}
        const QJsonValue previousOrder=current.value("row_order");
        current=delta;current["jobs"]=ordered;
        if(!delta.contains("row_order") && !legacy && !delta["reset"].toBool() && previousOrder.isArray())current["row_order"]=previousOrder;
        version=legacy?-1:delta["revision"].toInteger();
        return changed;
    }
};

/// Compteurs mémorisés par lot pour ne pas reparcourir les fichiers de l’ancien historique.
struct JobTotals {
    int waiting=0,running=0,done=0,errors=0;
    double bytes=0,copied=0;
    QJsonArray conflicts;
};
