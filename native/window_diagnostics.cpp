#include "window.h"
#include <QScopeGuard>

/// Vue modale de l’activité réelle : lire un diagnostic ne change jamais le transfert.
void Window::showDiagnostics() {
    if(auto *dialog=findChild<QDialog *>("diagnosticsDialog")){dialog->show();dialog->raise();refresh();return;}
    auto *dialog=I18n::localized(new QDialog(this),"windowTitle",[]{return GAD_TR("Transfer diagnostics");});dialog->setObjectName("diagnosticsDialog");dialog->setWindowModality(Qt::WindowModal);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setMinimumSize(1100,600);dialog->resize(1250,650);
    auto *layout=new QVBoxLayout(dialog);
    auto *help=I18n::localized(new QLabel,"text",[]{return GAD_TR("Live observations for active and queued requests. Configured limits do not by themselves prove a slowdown. No disk or network performance benchmark is performed.");});help->setWordWrap(true);layout->addWidget(help);
    auto *rows=new QTreeWidget;rows->setObjectName("diagnosticsTree");rows->setColumnCount(5);rows->setRootIsDecorated(false);rows->setWordWrap(true);rows->setTextElideMode(Qt::ElideNone);rows->setSelectionMode(QAbstractItemView::ExtendedSelection);rows->setUniformRowHeights(false);layout->addWidget(rows,1);
    rows->setColumnWidth(0,200);rows->setColumnWidth(1,110);rows->setColumnWidth(2,430);rows->setColumnWidth(3,210);rows->header()->setStretchLastSection(true);
    auto *empty=I18n::localized(new QLabel,"text",[]{return GAD_TR("No active or queued transfers.");});empty->setObjectName("diagnosticsEmpty");layout->addWidget(empty);
    auto *buttons=new QDialogButtonBox;auto *close=I18n::localized(new QPushButton(QIcon::fromTheme("window-close",style()->standardIcon(QStyle::SP_DialogCloseButton)),QString()),"text",[]{return GAD_TR("Close");});buttons->addButton(close,QDialogButtonBox::RejectRole);connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::close);layout->addWidget(buttons);
    I18n::onChange(dialog,"headers",[rows]{rows->setHeaderLabels({GAD_TR("Request"),GAD_TR("Status"),GAD_TR("Observed activity / wait"),GAD_TR("Speed limits"),GAD_TR("Speed and remaining time")});});
    connect(rows->header(),&QHeaderView::sectionResized,dialog,[this]{refreshDiagnostics();});
    refresh();dialog->show();
    if(!tree->selectedItems().isEmpty()) {const auto id=tree->selectedItems().first()->data(0,Qt::UserRole).toLongLong();for(int i=0;i<rows->topLevelItemCount();++i)if(rows->topLevelItem(i)->data(0,Qt::UserRole).toLongLong()==id){rows->setCurrentItem(rows->topLevelItem(i));rows->scrollToItem(rows->topLevelItem(i));break;}}
}

/// Conserver les items et n’écrire que les valeurs modifiées préserve sélection, survol et défilement.
void Window::refreshDiagnostics() {
    auto *dialog=findChild<QDialog *>("diagnosticsDialog");if(!dialog)return;
    auto *rows=dialog->findChild<QTreeWidget *>("diagnosticsTree");if(!rows || dialog->property("refreshingDiagnostics").toBool())return;
    // Le redimensionnement d’une colonne peut rappeler cette fonction pendant le calcul des hauteurs.
    dialog->setProperty("refreshingDiagnostics",true);
    const auto guard=qScopeGuard([dialog]{dialog->setProperty("refreshingDiagnostics",false);});
    QSet<qint64> live;
    // Un seul index par rafraîchissement, plutôt qu’un parcours complet pour chaque lot.
    QHash<qint64,QTreeWidgetItem *> existing;
    for(int i=0;i<rows->topLevelItemCount();++i) {
        auto *row=rows->topLevelItem(i);existing.insert(row->data(0,Qt::UserRole).toLongLong(),row);
    }
    const QFontMetrics metrics(rows->font());
    for(const auto &item:diagnosticTracker.items()) {
        live.insert(item.id);auto *row=existing.value(item.id,nullptr);
        if(!row){row=new QTreeWidgetItem(rows);row->setData(0,Qt::UserRole,item.id);}
        const QStringList values{QString("#%1 · %2").arg(item.id).arg(item.name),item.status,item.reason,item.limits,item.metrics};
        int height=24;
        for(int c=0;c<values.size();++c) {
            height=qMax(height,metrics.boundingRect(QRect(0,0,qMax(40,rows->columnWidth(c)-16),100000),Qt::TextWordWrap,values[c]).height()+14);
            if(row->text(c)!=values[c])row->setText(c,values[c]);
            const auto tooltip=Qt::convertFromPlainText(values[c]);if(row->toolTip(c)!=tooltip)row->setToolTip(c,tooltip);
        }
        if(row->sizeHint(0).height()!=height)row->setSizeHint(0,QSize(0,height));
    }
    for(int i=rows->topLevelItemCount()-1;i>=0;--i)if(!live.contains(rows->topLevelItem(i)->data(0,Qt::UserRole).toLongLong()))delete rows->takeTopLevelItem(i);
    dialog->findChild<QLabel *>("diagnosticsEmpty")->setVisible(live.isEmpty());
}
