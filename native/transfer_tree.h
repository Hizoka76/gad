#pragma once
// Glisser-déposer interne de priorité : aucun chemin ni aucune destination n’est modifié par le widget.
#include "row_tree.h"
#include "table_view.h"
#include <functional>

/// Lit les modificateurs de l’événement réel, même quand un clic n’a pas modifié le clavier global Qt.
class TransferHeader:public QHeaderView {
    Qt::KeyboardModifiers modifiers=Qt::NoModifier;
    QHash<int,Qt::SortOrder> indicators;
protected:
    /// Laisse le thème Qt dessiner sa flèche habituelle, pour chacune des colonnes triées.
    void initStyleOptionForIndex(QStyleOptionHeader *option,int index) const override {
        QHeaderView::initStyleOptionForIndex(option,index);
        const auto found=indicators.constFind(index);
        option->sortIndicator=found==indicators.cend()?QStyleOptionHeader::None
            :found.value()==Qt::AscendingOrder?QStyleOptionHeader::SortDown:QStyleOptionHeader::SortUp;
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        modifiers=event->modifiers();QHeaderView::mouseReleaseEvent(event);modifiers=Qt::NoModifier;
    }
public:
    explicit TransferHeader(QWidget *parent):QHeaderView(Qt::Horizontal,parent){}
    Qt::KeyboardModifiers clickModifiers() const {return modifiers;}
    void setSortIndicators(const QList<TableView::SortKey> &keys) {
        indicators.clear();for(const auto &key:keys)indicators.insert(key.column,key.order);
        setSortIndicatorShown(!keys.isEmpty());
        if(!keys.isEmpty())setSortIndicator(keys.first().column,keys.first().order);
        viewport()->update();
    }
};

class TransferTree:public QTreeWidget {
    static constexpr const char *format="application/x-gad-rows";
    const QString token=QUuid::createUuid().toString();
    QPersistentModelIndex target;
    bool after=false,hasTarget=false;
    QPoint dragPosition;
    TableView::Settings viewSettings;
    quint64 viewRevision=0,filterRevision=0;
    /// Toute modification force une présentation fraîche, même si le moteur répond « inchangé ».
    void changed() {
        ++viewRevision;updateHeaderIndicators();
        if(presentationChanged)presentationChanged();
    }
    QTimer scroll;
    /// Met à jour le repère à partir des lignes réellement visibles après un défilement.
    void locate(const QPoint &position) {
        auto *row=itemAt(position);target=row?indexFromItem(row):QModelIndex();
        after=row && position.y()>visualItemRect(row).center().y();viewport()->update();
    }
    /// Refuse un glisser issu d’une autre instance ou d’une autre application.
    bool internal(const QMimeData *mime) const {
        return mime->hasFormat(format) && QJsonDocument::fromJson(mime->data(format)).object()["origin"]==token;
    }
protected:
    void startDrag(Qt::DropActions) override {
        auto *mime=selectionMime();if(mime->data(format).isEmpty()){delete mime;return;}
        QPointer<TransferTree> keep(this);QPointer<QDrag> drag=new QDrag(this);
        drag->setMimeData(mime);drag->exec(Qt::MoveAction);if(!keep)return;
        if(drag)drag->deleteLater();
        scroll.stop();target=QModelIndex();hasTarget=false;viewport()->update();
    }
    void dragEnterEvent(QDragEnterEvent *event) override {
        if(viewSettings.sorts.isEmpty() && internal(event->mimeData())){event->setDropAction(Qt::MoveAction);event->accept();}else event->ignore();
    }
    void dragMoveEvent(QDragMoveEvent *event) override {
        if(!viewSettings.sorts.isEmpty() || !internal(event->mimeData())){event->ignore();return;}
        auto *row=itemAt(event->position().toPoint());if(!row){target=QModelIndex();event->ignore();viewport()->update();return;}
        dragPosition=event->position().toPoint();hasTarget=true;locate(dragPosition);
        if(dragPosition.y()<20 || dragPosition.y()>viewport()->height()-20){if(!scroll.isActive())scroll.start();}else scroll.stop();
        event->setDropAction(Qt::MoveAction);event->accept();viewport()->update();
    }
    void dragLeaveEvent(QDragLeaveEvent *event) override {scroll.stop();hasTarget=false;target=QModelIndex();viewport()->update();event->accept();}
    void dropEvent(QDropEvent *event) override {
        if(!viewSettings.sorts.isEmpty() || !internal(event->mimeData())){event->ignore();return;}
        scroll.stop();auto *row=itemAt(event->position().toPoint());
        if(hasTarget && (!row || !target.isValid() || indexFromItem(row)!=target)){target=QModelIndex();hasTarget=false;event->ignore();viewport()->update();return;}
        if(row && reorder) {
            auto request=QJsonDocument::fromJson(event->mimeData()->data(format)).object();
            request.remove("origin");request["target"]=key(row);
            request["action"]=event->position().y()>visualItemRect(row).center().y()?"after":"before";
            reorder(request);event->setDropAction(Qt::MoveAction);event->accept();
        }else event->ignore();
        target=QModelIndex();hasTarget=false;viewport()->update();
    }
    void paintEvent(QPaintEvent *event) override {
        QTreeWidget::paintEvent(event);
        if(!target.isValid())return;
        const auto rect=visualRect(target);QPainter painter(viewport());
        painter.setPen(QPen(palette().color(QPalette::Highlight),2));const int y=after?rect.bottom():rect.top();
        painter.drawLine(0,y,viewport()->width(),y);
    }
public:
    std::function<void(QJsonObject)> reorder;
    std::function<void()> presentationChanged;
    quint64 presentationRevision() const {return viewRevision;}
    quint64 headerFilterRevision() const {return filterRevision;}
    void updateHeaderIndicators() {
        static_cast<TransferHeader *>(header())->setSortIndicators(viewSettings.sorts);
        const QStringList labels={GAD_TR("Source"),GAD_TR("Destination"),GAD_TR("Status"),GAD_TR("Progress"),GAD_TR("Details"),GAD_TR("Order")};
        for(int c=0;c<columnCount();++c) {
            QString title=labels.value(c);
            if(viewSettings.sorts.size()>1)for(int i=0;i<viewSettings.sorts.size();++i)if(viewSettings.sorts[i].column==c)
                title+=QString(" (%1)").arg(i+1);
            headerItem()->setText(c,title);
            headerItem()->setIcon(c,QIcon());
            if(c==TableView::OrderColumn)headerItem()->setToolTip(c,GAD_TR("Position in the queue, including retained history, independent of display sorting and filters.\nThis is not a completion order; transfers may run in parallel."));
        }
    }
    const TableView::Settings &presentation() const {return viewSettings;}
    bool matches(const QTreeWidgetItem *row) const {return viewSettings.matches(row);}
    void applyPresentation() {TableView::sort(this,viewSettings);updateHeaderIndicators();}
    /// Un clic simple remplace les clés ; Ctrl ajoute une clé ou inverse son sens sans changer sa priorité.
    void setViewSort(int column,Qt::SortOrder order=Qt::AscendingOrder,bool additive=false) {
        if(column<-1 || column>=columnCount())return;
        if(column<0)viewSettings.sorts.clear();
        else if(!additive)viewSettings.sorts={{column,order}};
        else {
            auto found=std::find_if(viewSettings.sorts.begin(),viewSettings.sorts.end(),[column](const auto &key){return key.column==column;});
            if(found==viewSettings.sorts.end())viewSettings.sorts.append({column,order});else found->order=order;
        }
        changed();
    }
    void toggleViewSort(int column,bool additive) {
        const auto found=std::find_if(viewSettings.sorts.cbegin(),viewSettings.sorts.cend(),[column](const auto &key){return key.column==column;});
        const bool toggle=found!=viewSettings.sorts.cend() && (additive || viewSettings.sorts.size()==1);
        setViewSort(column,toggle && found->order==Qt::AscendingOrder?Qt::DescendingOrder:Qt::AscendingOrder,additive);
    }
    void removeViewSort(int column) {
        auto &keys=viewSettings.sorts;keys.erase(std::remove_if(keys.begin(),keys.end(),[column](const auto &key){return key.column==column;}),keys.end());changed();
    }
    /// Invalide aussi un instantané moteur inchangé pour appliquer la recherche immédiatement.
    void setHistorySearch(const HistorySearch::Query &query) {viewSettings.history=query;++filterRevision;changed();}
    void setColumnFilter(int column,const QString &text) {
        if(column<0 || column>=TableView::ColumnCount)return;
        if(viewSettings.contains[column]==text)return;
        viewSettings.contains[column]=text;++filterRevision;changed();
    }
    void setStateFilter(const QString &state) {viewSettings.state=state;++filterRevision;changed();}
    void setProgressFilter(int minimum,int maximum) {
        viewSettings.minimum=qBound(0,qMin(minimum,maximum),100);
        viewSettings.maximum=qBound(0,qMax(minimum,maximum),100);++filterRevision;changed();
    }
    void clearColumnFilter(int column) {
        if(column<0 || column>=TableView::ColumnCount)return;
        viewSettings.contains[column].clear();if(column==2)viewSettings.state.clear();if(column==3){viewSettings.minimum=0;viewSettings.maximum=100;}++filterRevision;changed();
    }
    void clearHeaderFilters() {viewSettings.contains={};viewSettings.state.clear();viewSettings.minimum=0;viewSettings.maximum=100;++filterRevision;changed();}
    /// Le menu ne modifie que la vue ; les champs restent ouverts pendant le filtrage.
    QMenu *headerMenu(int column) {
        if(column<0 || column>=columnCount())return nullptr;
        auto *menu=new QMenu(this);menu->setObjectName("transferHeaderMenu");menu->setAttribute(Qt::WA_DeleteOnClose);
        connect(menu,&QMenu::aboutToHide,menu,&QObject::deleteLater);
        I18n::onChange(menu,"columnTitle",[this,menu,column]{menu->setTitle(headerItem()->text(column));});
        auto *ascending=I18n::localized(menu->addAction(QIcon::fromTheme("view-sort-ascending",style()->standardIcon(QStyle::SP_ArrowUp)),QString()),"text",[]{return GAD_TR("Sort ascending");});ascending->setObjectName("headerSortAscending");
        connect(ascending,&QAction::triggered,menu,[this,column]{setViewSort(column,Qt::AscendingOrder);});
        auto *descending=I18n::localized(menu->addAction(QIcon::fromTheme("view-sort-descending",style()->standardIcon(QStyle::SP_ArrowDown)),QString()),"text",[]{return GAD_TR("Sort descending");});descending->setObjectName("headerSortDescending");
        connect(descending,&QAction::triggered,menu,[this,column]{setViewSort(column,Qt::DescendingOrder);});
        auto *queue=I18n::localized(menu->addAction(QIcon::fromTheme("view-list-details",style()->standardIcon(QStyle::SP_FileDialogListView)),QString()),"text",[]{return GAD_TR("Transfer queue order");});queue->setObjectName("headerQueueOrder");
        connect(queue,&QAction::triggered,menu,[this]{setViewSort(-1);});
        auto *remove=I18n::localized(menu->addAction(QIcon::fromTheme("edit-clear",style()->standardIcon(QStyle::SP_DialogResetButton)),QString()),"text",[]{return GAD_TR("Remove this column from sorting");});remove->setObjectName("headerRemoveSort");
        connect(remove,&QAction::triggered,menu,[this,column]{removeViewSort(column);});menu->addSeparator();
        auto *widget=new QWidget(menu);auto *layout=new QVBoxLayout(widget);
        auto *search=new QLineEdit(widget);search->setObjectName("headerColumnFilter");search->setText(viewSettings.contains[column]);
        I18n::bindProperty(search,"placeholderText",[]{return GAD_TR("Contains…");});I18n::bindProperty(search,"accessibleName",[]{return GAD_TR("Filter this column");});
        layout->addWidget(search);connect(search,&QLineEdit::textChanged,menu,[this,column](const QString &text){setColumnFilter(column,text);});connect(search,&QLineEdit::returnPressed,menu,&QMenu::close);
        if(column==2) {
            auto *states=new QComboBox(widget);states->setObjectName("headerStateFilter");states->addItem(GAD_TR("All statuses"),QString());
#define GAD_PROTOCOL_VERSION(number)
#define GAD_COMMAND(name, wire)
#define GAD_STATE(name, wire, terminal) states->addItem(label(Protocol::State::name),Protocol::State::name);
#include "protocol.def"
#undef GAD_STATE
#undef GAD_COMMAND
#undef GAD_PROTOCOL_VERSION
            states->setCurrentIndex(qMax(0,states->findData(viewSettings.state)));layout->addWidget(states);
            I18n::onChange(states,"labels",[states]{QSignalBlocker blocker(states);states->setItemText(0,GAD_TR("All statuses"));for(int i=1;i<states->count();++i)states->setItemText(i,label(states->itemData(i).toString()));},false);
            connect(states,&QComboBox::currentIndexChanged,menu,[this,states]{setStateFilter(states->currentData().toString());});
        }
        if(column==3) {
            layout->addWidget(I18n::localized(new QLabel(widget),"text",[]{return GAD_TR("Progress range (%)");}));
            auto *range=new QHBoxLayout;auto *minimum=new QSpinBox(widget);auto *maximum=new QSpinBox(widget);
            minimum->setObjectName("headerProgressMinimum");maximum->setObjectName("headerProgressMaximum");minimum->setRange(0,100);maximum->setRange(0,100);
            minimum->setKeyboardTracking(false);maximum->setKeyboardTracking(false);
            I18n::bindProperty(minimum,"accessibleName",[]{return GAD_TR("Minimum progress");});I18n::bindProperty(maximum,"accessibleName",[]{return GAD_TR("Maximum progress");});
            minimum->setValue(viewSettings.minimum);maximum->setValue(viewSettings.maximum);minimum->setSuffix(" %");maximum->setSuffix(" %");range->addWidget(minimum);range->addWidget(maximum);layout->addLayout(range);
            connect(minimum,&QSpinBox::valueChanged,menu,[this,minimum,maximum](int value){if(value>maximum->value()){QSignalBlocker blocker(maximum);maximum->setValue(value);}setProgressFilter(minimum->value(),maximum->value());});
            connect(maximum,&QSpinBox::valueChanged,menu,[this,minimum,maximum](int value){if(value<minimum->value()){QSignalBlocker blocker(minimum);minimum->setValue(value);}setProgressFilter(minimum->value(),maximum->value());});
        }
        auto *edit=new QWidgetAction(menu);edit->setDefaultWidget(widget);menu->addAction(edit);menu->addSeparator();
        auto *clear=I18n::localized(menu->addAction(QIcon::fromTheme("edit-clear",style()->standardIcon(QStyle::SP_DialogResetButton)),QString()),"text",[]{return GAD_TR("Clear this column's filter");});clear->setObjectName("headerClearColumn");connect(clear,&QAction::triggered,menu,[this,column]{clearColumnFilter(column);});
        auto *clearAll=I18n::localized(menu->addAction(QIcon::fromTheme("edit-clear-all",style()->standardIcon(QStyle::SP_TrashIcon)),QString()),"text",[]{return GAD_TR("Clear header filters");});clearAll->setObjectName("headerClearFilters");connect(clearAll,&QAction::triggered,menu,[this]{clearHeaderFilters();});
        return menu;
    }
    explicit TransferTree(QWidget *parent=nullptr):QTreeWidget(parent) {
        setHeader(new TransferHeader(this));
        header()->setSectionsClickable(true);header()->setContextMenuPolicy(Qt::CustomContextMenu);
        I18n::bindProperty(header(),"toolTip",[]{return GAD_TR("Click to sort; Ctrl+click to add a sort column; right-click for filters.\nNumbers show sort priority.\nRestore transfer queue order to drag rows.");});
        connect(header(),&QHeaderView::sectionClicked,this,[this](int column){toggleViewSort(column,static_cast<TransferHeader *>(header())->clickModifiers().testFlag(Qt::ControlModifier));});
        connect(header(),&QHeaderView::customContextMenuRequested,this,[this](const QPoint &position){if(auto *menu=headerMenu(header()->logicalIndexAt(position)))menu->popup(header()->mapToGlobal(position));});
        scroll.setInterval(40);
        connect(&scroll,&QTimer::timeout,this,[this]{
            const int direction=dragPosition.y()<20?-1:dragPosition.y()>viewport()->height()-20?1:0;
            if(!direction){scroll.stop();return;}
            auto *bar=verticalScrollBar();bar->setValue(bar->value()+direction*bar->singleStep());locate(dragPosition);
        });
        setDragEnabled(true);setAcceptDrops(true);setDropIndicatorShown(false);setDragDropMode(QAbstractItemView::DragDrop);
    }
    /// Identité stable ; index nul pour une demande encore en analyse sans fichiers connus.
    static QJsonObject key(const QTreeWidgetItem *row) {
        const auto index=row->data(0,RowTree::EntryIndexRole);
        return {{"job",row->data(0,Qt::UserRole).toLongLong()},{"index",index.isValid() && index.toInt()>=0?QJsonValue(index.toInt()):QJsonValue(QJsonValue::Null)}};
    }
    /// Déduplique les branches sélectionnées et n’accepte aucune ligne déjà active ou terminée.
    QMimeData *selectionMime() const {
        auto *mime=new QMimeData;QJsonArray rows;
        if(!viewSettings.sorts.isEmpty())return mime;
        for(auto *row:selectedItems()) {
            bool covered=false;for(auto *parent=row->parent();parent;parent=parent->parent())if(parent->isSelected()){covered=true;break;}
            if(covered)continue;
            if(!row->data(0,RowTree::CanQueueRole).toBool())return mime;
            rows.append(key(row));
        }
        if(!rows.isEmpty())mime->setData(format,QJsonDocument(QJsonObject{{"origin",token},{"rows",rows}}).toJson(QJsonDocument::Compact));
        return mime;
    }
};

/// La sélection et le survol du thème gardent la priorité sur la teinte de demande.
class RequestDelegate:public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter,const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        QStyledItemDelegate::paint(painter,option,index);
        if(index.column()!=TableView::OrderColumn)return;
        const auto accent=index.siblingAtColumn(0).data(RowTree::AccentRole).value<QColor>();if(!accent.isValid())return;
        painter->save();painter->fillRect(QRect(option.rect.left(),option.rect.top(),3,option.rect.height()),accent);painter->restore();
    }
};
