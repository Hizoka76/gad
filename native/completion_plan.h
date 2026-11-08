#pragma once
// Automate monotone, sans effet système : l’armement reste limité à la session et à une seule file.
#include "presentation.h"
#include <algorithm>
namespace Completion {
inline bool validAction(const QString &action){return QStringList{"none","notify","quit","suspend","poweroff"}.contains(action);}
inline bool validDelay(const QJsonValue &value){const auto n=value.toDouble(-1);return value.isDouble() && n>=5 && n<=3600 && n==qint64(n);}
inline QString label(const QString &action){
    if(action=="notify")return GAD_TR("Notify");
    if(action=="quit")return GAD_TR("Quit Gad");
    if(action=="suspend")return GAD_TR("Suspend the computer");
    if(action=="poweroff")return GAD_TR("Power off the computer");
    return GAD_TR("Do nothing");
}
struct Plan {
    enum Event{None,Begin,Ready,Abort};
    bool armed=false,counting=false,sawPreparation=false;
    QString action="none",reason;
    qint64 delay=30,deadline=0,baseline=0,epoch=0,next=0;
    QSet<qint64> ids;
    void cancel(){armed=false;counting=false;}
    void arm(const QString &choice,qint64 seconds,const QJsonObject &state){
        cancel();action=choice;delay=seconds;ids.clear();reason.clear();sawPreparation=false;
        const auto guard=state["queue_guard"].toObject();baseline=guard["next_id"].toInteger();epoch=guard["epoch"].toInteger();next=baseline;
        armed=choice!="none" && validAction(choice) && seconds>=5 && seconds<=3600;
        for(const auto &value:state["jobs"].toArray()){const auto job=value.toObject();if(!Protocol::terminal(job["state"].toString()))ids.insert(job["id"].toInteger());}
    }
    int remaining(qint64 now)const{return int(qMax<qint64>(0,(deadline-now+999)/1000));}
    QJsonObject claim()const{QJsonArray selected;auto sorted=ids.values();std::sort(sorted.begin(),sorted.end());for(auto id:sorted)selected.append(id);return {{"command",Protocol::Command::CompletionClaim},{"ids",selected},{"epoch",epoch},{"next_id",next}};}
    Event observe(const QJsonObject &state,bool preparing,qint64 now){
        if(!armed)return None;
        const auto abort=[this](const QString &message){reason=message;cancel();return Abort;};
        const auto guard=state["queue_guard"].toObject();if(guard.isEmpty())return abort(GAD_TR("Batch completion guard unavailable. Restart Gad before arming an action."));
        const auto current=guard["epoch"].toInteger();if(current!=epoch){
            const auto command=guard["last_command"].toString();
            if(!QStringList{"enqueue","pause","resolve","retry_selection","retry_errors","resume_interrupted","reorder_queue","reorder_rows"}.contains(command))return abort(GAD_TR("The completion action was canceled because operations were canceled, removed or restored."));
            counting=false;epoch=current;
        }
        next=guard["next_id"].toInteger();sawPreparation |= preparing;
        QHash<qint64,QJsonObject> jobs;
        for(const auto &value:state["jobs"].toArray()){const auto job=value.toObject();const auto id=job["id"].toInteger();jobs.insert(id,job);if(id>baseline || !Protocol::terminal(job["state"].toString()))ids.insert(id);}
        for(auto id:ids){const auto job=jobs.value(id);if(job.isEmpty())return abort(GAD_TR("A tracked request disappeared. The completion action was canceled."));
            const auto status=job["state"].toString();if(Protocol::terminal(status) && (status!=Protocol::State::Done || job["cancel"].toBool()))return abort(GAD_TR("The batch contains an error or cancellation. The completion action was canceled."));}
        const bool busy=preparing || state["paused"].toBool() || !guard["idle"].toBool();
        if(busy){counting=false;return None;}
        if(ids.isEmpty()){if(next>baseline || sawPreparation)return abort(GAD_TR("No successful transfer was observed. The completion action was canceled."));return None;}
        for(auto id:ids)if(jobs.value(id)["state"]!=Protocol::State::Done){counting=false;return None;}
        if(!counting){counting=true;deadline=now+delay*1000;return Begin;}
        if(now>=deadline){armed=false;counting=false;return Ready;}
        return None;
    }
};
}
