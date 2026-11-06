#pragma once
// Navigation dans l’arbre visuel : la profondeur ne change jamais l’identité lot/indice du moteur.
#include <QtWidgets>

namespace RowTree {
constexpr int EntryIndexRole=Qt::UserRole+7;
constexpr int KindRole=Qt::UserRole+9;
constexpr int OwnerRole=Qt::UserRole+10;
constexpr int CanQueueRole=Qt::UserRole+11;
constexpr int AccentRole=Qt::UserRole+12;
constexpr int ErrorRetryRole=Qt::UserRole+13;
constexpr int ErrorRetryEntryRole=Qt::UserRole+14;
constexpr int OperationRole=Qt::UserRole+17;
inline int index(const QTreeWidgetItem *item){const auto value=item->data(0,EntryIndexRole);return value.isValid()?value.toInt():-1;}
/// Les demandes sont conservées hors de l’arbre ; toute ligne porte son propriétaire métier.
inline QTreeWidgetItem *owner(QTreeWidgetItem *item) {
    if(!item)return nullptr;
    const auto address=item->data(0,OwnerRole).value<quintptr>();
    if(address)return reinterpret_cast<QTreeWidgetItem *>(address);
    while(item->parent())item=item->parent();
    return item;
}
/// Réattache une branche vers un dossier ou vers la racine sans perdre sélection ni développement.
inline void attach(QTreeWidgetItem *item,QTreeWidgetItem *parent,QTreeWidget *host=nullptr,int position=-1) {
    auto *tree=host?host:item->treeWidget();if(!tree && parent)tree=parent->treeWidget();
    if(item->parent()==parent && item->treeWidget()==tree && (position<0 || (parent?parent->indexOfChild(item):tree->indexOfTopLevelItem(item))==position))return;
    struct State {QTreeWidgetItem *row;bool selected,expanded;};
    QList<State> states;QList<QTreeWidgetItem *> pending{item};
    auto *current=tree?tree->currentItem():nullptr;const int column=tree?tree->currentColumn():0;
    QTreeWidgetItem *restoreCurrent=nullptr;
    while(!pending.isEmpty()) {
        auto *row=pending.takeLast();states.append({row,row->isSelected(),row->isExpanded()});
        if(row==current)restoreCurrent=row;
        for(int i=0;i<row->childCount();++i)pending.append(row->child(i));
    }
    if(auto *old=item->parent())old->takeChild(old->indexOfChild(item));
    else if(item->treeWidget())item->treeWidget()->takeTopLevelItem(item->treeWidget()->indexOfTopLevelItem(item));
    if(parent){if(position<0)parent->addChild(item);else parent->insertChild(position,item);}
    else if(tree){if(position<0)tree->addTopLevelItem(item);else tree->insertTopLevelItem(position,item);}
    for(const auto &state:states){state.row->setExpanded(state.expanded);state.row->setSelected(state.selected);}
    if(restoreCurrent)tree->setCurrentItem(restoreCurrent,column,QItemSelectionModel::NoUpdate);
}
/// Les dossiers et lots peuvent contenir une annulation plusieurs niveaux plus bas.
inline bool hasCancelled(QTreeWidgetItem *item) {
    QList<QTreeWidgetItem *> pending{item};
    while(!pending.isEmpty()) {
        auto *row=pending.takeLast();
        if(row->data(0,Qt::UserRole+3).toString()=="cancelled")return true;
        for(int i=0;i<row->childCount();++i)pending.append(row->child(i));
    }
    return false;
}
/// Replie une branche terminée sans laisser une sélection cachée dans ses descendants.
inline void collapseCompleted(QTreeWidgetItem *item) {
    QList<QTreeWidgetItem *> pending;
    for(int i=0;i<item->childCount();++i)pending.append(item->child(i));
    bool selected=item->isSelected(),current=false;
    auto *tree=item->treeWidget();
    while(!pending.isEmpty()) {
        auto *row=pending.takeLast();
        if(row->isSelected()){selected=true;row->setSelected(false);}
        if(tree && tree->currentItem()==row)current=true;
        for(int i=0;i<row->childCount();++i)pending.append(row->child(i));
    }
    item->setExpanded(false);
    if(selected)item->setSelected(true);
    if(current)tree->setCurrentItem(item,0,QItemSelectionModel::NoUpdate);
}
}
