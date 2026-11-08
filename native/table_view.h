#pragma once
// Présentation seulement : aucune commande moteur, aucun identifiant métier modifié.
#include "presentation.h"
#include "history_search.h"
#include "row_tree.h"
#include <array>
#include <algorithm>

namespace TableView {
constexpr int OrderColumn=5;
constexpr int ColumnCount=6;
constexpr int ProgressRole=Qt::UserRole+15;
constexpr int QueueRankRole=Qt::UserRole+16;
/// Retient des filtres littéraux, un état indépendant de sa traduction et une plage numérique.
struct SortKey {int column;Qt::SortOrder order;};
struct Settings {
    HistorySearch::Query history;
    std::array<QString,ColumnCount> contains;
    QString state;
    int minimum=0,maximum=100;
    QList<SortKey> sorts;
    bool filtered(int c) const {if(c<0 || c>=ColumnCount)return false;return !contains[c].isEmpty() || (c==2 && !state.isEmpty()) || (c==3 && (minimum>0 || maximum<100));}
    bool filtered() const {if(history.enabled)return true;for(int c=0;c<ColumnCount;++c)if(filtered(c))return true;return false;}
    bool matches(const QTreeWidgetItem *row) const {
        if(!history.matches(row))return false;
        for(int c=0;c<ColumnCount;++c)if(!contains[c].isEmpty() && !row->text(c).contains(contains[c],Qt::CaseInsensitive)
            && !(c==0 && row->toolTip(c).contains(contains[c],Qt::CaseInsensitive)))return false;
        if(!state.isEmpty() && row->data(0,Qt::UserRole+3).toString()!=state)return false;
        if(minimum>0 || maximum<100) {
            const auto value=row->data(3,ProgressRole);
            if(!value.isValid() || value.toDouble()<minimum/100.0 || value.toDouble()>maximum/100.0)return false;
        }
        return true;
    }
};
/// Trie les frères en réattachant les mêmes items ; les descendants suivent toujours leur dossier.
inline void sort(QTreeWidget *tree,const Settings &settings) {
    if(settings.sorts.isEmpty())return;
    QCollator collator;collator.setNumericMode(true);collator.setCaseSensitivity(Qt::CaseInsensitive);
    QSignalBlocker blocker(tree);const int scroll=tree->verticalScrollBar()->value();
    QList<QTreeWidgetItem *> parents{nullptr};
    while(!parents.isEmpty()) {
        auto *parent=parents.takeLast();QList<QTreeWidgetItem *> rows;
        for(int i=0;i<(parent?parent->childCount():tree->topLevelItemCount());++i)rows.append(parent?parent->child(i):tree->topLevelItem(i));
        std::stable_sort(rows.begin(),rows.end(),[&](const auto *a,const auto *b){
            for(const auto &key:settings.sorts) {
                int comparison=0;
                if(key.column==OrderColumn) {
                    const auto av=a->data(0,QueueRankRole).toLongLong(),bv=b->data(0,QueueRankRole).toLongLong();
                    comparison=av<bv?-1:av>bv?1:0;
                }else if(key.column==3) {
                    const double av=a->data(3,ProgressRole).isValid()?a->data(3,ProgressRole).toDouble():-1;
                    const double bv=b->data(3,ProgressRole).isValid()?b->data(3,ProgressRole).toDouble():-1;
                    comparison=av<bv?-1:av>bv?1:0;
                }else comparison=collator.compare(a->text(key.column),b->text(key.column));
                if(comparison)return key.order==Qt::AscendingOrder?comparison<0:comparison>0;
            }
            return a->data(0,QueueRankRole).toLongLong()<b->data(0,QueueRankRole).toLongLong();
        });
        for(int i=0;i<rows.size();++i){if((parent?parent->child(i):tree->topLevelItem(i))!=rows[i])RowTree::attach(rows[i],parent,tree,i);if(rows[i]->childCount())parents.append(rows[i]);}
    }
    tree->verticalScrollBar()->setValue(scroll);
}
}
