#include "i18n.h"
// Bilan non modal : un instantané exportable, les commandes métier restent validées par Rust.
#include "window.h"
#include "bridge.h"
#include "error_report.h"
#include <memory>

void Window::retryErrorSelection() {
    if(tree->selectedItems().isEmpty())return;
    const auto result=engine(selectionRequest(Protocol::Command::RetryErrors));
    if(result.contains("error"))error(result["error"].toString());
    refresh();
}

void Window::showErrorReport() {
    if(auto *existing=findChild<QDialog *>("errorReportDialog")){existing->show();existing->raise();existing->activateWindow();return;}
    auto *dialog=new QDialog(this);dialog->setObjectName("errorReportDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);
    I18n::bindProperty(dialog,"windowTitle",[=]{return GAD_TR("Gad — Error report");});dialog->setMinimumSize(1100,600);dialog->resize(1150,650);
    auto *layout=new QVBoxLayout(dialog);
    auto *summary=new QLabel;summary->setObjectName("errorReportSummary");summary->setTextFormat(Qt::PlainText);summary->setWordWrap(true);layout->addWidget(summary);
    auto *search=new QLineEdit;search->setObjectName("errorReportSearch");I18n::bindProperty(search,"placeholderText",[=]{return GAD_TR("Filter by request, file, destination, or message…");});layout->addWidget(search);
    auto *rows=new QTreeWidget;rows->setObjectName("errorReportRows");rows->setRootIsDecorated(false);rows->setUniformRowHeights(true);rows->setSelectionMode(QAbstractItemView::ExtendedSelection);
    rows->setHeaderLabels({GAD_TR("Request"),GAD_TR("Operation"),GAD_TR("Source"),GAD_TR("Destination"),GAD_TR("Status"),GAD_TR("Error")});rows->setColumnWidth(0,85);rows->setColumnWidth(1,100);rows->setColumnWidth(2,260);rows->setColumnWidth(3,260);rows->setColumnWidth(4,100);rows->header()->setStretchLastSection(true);layout->addWidget(rows,1);
    auto *detail=new QPlainTextEdit;detail->setObjectName("errorReportDetail");detail->setReadOnly(true);detail->setMaximumHeight(165);layout->addWidget(detail);
    auto *buttons=new QHBoxLayout;
    auto *retryAll=I18n::localized(new QPushButton(QIcon::fromTheme("view-refresh",style()->standardIcon(QStyle::SP_BrowserReload)),QString()),"text",[=]{return GAD_TR("Retry failed files only");});retryAll->setObjectName("retryAllErrors");
    I18n::bindProperty(retryAll,"toolTip",[=]{return GAD_TR("Retries the displayed failed items.\nCompleted, canceled, and hidden files are kept.\nThe global pause still applies.");});
    auto *retrySelected=I18n::localized(new QPushButton(QIcon::fromTheme("edit-redo",style()->standardIcon(QStyle::SP_BrowserReload)),QString()),"text",[=]{return GAD_TR("Retry selection");});retrySelected->setObjectName("retrySelectedErrors");
    auto *locate=I18n::localized(new QPushButton(QIcon::fromTheme("edit-find",style()->standardIcon(QStyle::SP_FileDialogContentsView)),QString()),"text",[=]{return GAD_TR("Show in list");});locate->setObjectName("locateReportError");
    auto *exportButton=I18n::localized(new QPushButton(QIcon::fromTheme("document-save",style()->standardIcon(QStyle::SP_DialogSaveButton)),QString()),"text",[=]{return GAD_TR("Export…");});exportButton->setObjectName("exportErrorReport");exportButton->setEnabled(false);I18n::bindProperty(exportButton,"toolTip",[=]{return GAD_TR("Exports the complete report as text or JSON, including rows hidden by the filter.");});
    auto *reload=I18n::localized(new QPushButton(QIcon::fromTheme("view-refresh",style()->standardIcon(QStyle::SP_BrowserReload)),QString()),"text",[=]{return GAD_TR("Refresh");});reload->setObjectName("refreshErrorReport");auto *close=I18n::localized(new QPushButton(QIcon::fromTheme("window-close",style()->standardIcon(QStyle::SP_DialogCloseButton)),QString()),"text",[=]{return GAD_TR("Close");});close->setObjectName("closeErrorReport");
    for(auto *button:{retryAll,retrySelected,locate,exportButton,reload,close}) {buttons->addWidget(button);}
    layout->addLayout(buttons);
    auto report=std::make_shared<QJsonObject>();
    const auto recordFor=[rows,report](QTreeWidgetItem *row){return report->value("records").toArray().at(row->data(0,Qt::UserRole).toInt()).toObject();};
    const auto updateButtons=[rows,retryAll,retrySelected,locate,recordFor,report,exportButton]{
        exportButton->setEnabled(!report->value("records").toArray().isEmpty());
        bool available=false,selected=false;
        for(int i=0;i<rows->topLevelItemCount();++i){auto *row=rows->topLevelItem(i);if(row->isHidden())continue;const bool enabled=recordFor(row)["retryable"].toBool();available|=enabled;selected|=enabled && row->isSelected();}
        retryAll->setEnabled(available);retrySelected->setEnabled(selected);locate->setEnabled(rows->selectedItems().size()==1);
    };
    const auto applyFilter=[rows,search,updateButtons]{
        for(int i=0;i<rows->topLevelItemCount();++i){auto *row=rows->topLevelItem(i);bool match=search->text().isEmpty();for(int c=0;c<rows->columnCount();++c)match|=row->text(c).contains(search->text(),Qt::CaseInsensitive);row->setHidden(!match);if(!match)row->setSelected(false);}
        updateButtons();
    };
    const auto load=[this,report,summary,rows,detail,applyFilter]{
        const auto fresh=engine({{"command",Protocol::Command::ErrorReport}});
        if(fresh.contains("error")){summary->setText(I18n::message(fresh["error"].toString()));return;}
        QSignalBlocker blocker(rows);rows->setSortingEnabled(false);rows->clear();detail->clear();*report=fresh;
        int retryable=0,index=0;
        for(const auto &value:fresh["records"].toArray()){
            const auto record=value.toObject();auto *row=new QTreeWidgetItem(rows);
            const QStringList values={QString::number(record["job"].toInteger()),record["mode"]=="move"?GAD_TR("Move"):GAD_TR("Copy"),ErrorReport::source(fresh,record),ErrorReport::destination(fresh,record),label(record["state"].toString()),I18n::message(record["error"].toString())};
            for(int c=0;c<values.size();++c){auto display=values[c];display.replace('\n'," ↵ ");display.replace('\r'," ");display.replace('\t'," ");row->setText(c,display);row->setToolTip(c,values[c]);}
            row->setData(0,Qt::UserRole,index++);if(record["retryable"].toBool())++retryable;
        }
        rows->setSortingEnabled(true);I18n::bindProperty(summary,"text",[=]{return QString(GAD_TR("%1 error(s) · %2 retryable item(s). Retrying keeps completed files and respects the global pause.")).arg(index).arg(retryable);});applyFilter();
    };
    connect(rows,&QTreeWidget::itemSelectionChanged,dialog,[rows,detail,recordFor,report,updateButtons]{
        const auto selected=rows->selectedItems();detail->setPlainText(selected.size()==1?ErrorReport::detail(*report,recordFor(selected[0])):QString());updateButtons();
    });
    connect(search,&QLineEdit::textChanged,dialog,[applyFilter]{applyFilter();});connect(reload,&QPushButton::clicked,dialog,[load]{load();});connect(close,&QPushButton::clicked,dialog,&QDialog::close);
    const auto retry=[this,rows,recordFor,load](bool selected){
        QList<QJsonObject> records;for(int i=0;i<rows->topLevelItemCount();++i){auto *row=rows->topLevelItem(i);if(!row->isHidden() && (!selected || row->isSelected()))records.append(recordFor(row));}
        const auto result=engine(ErrorReport::retryRequest(records));if(result.contains("error"))error(result["error"].toString());
        refresh();load();
    };
    connect(retryAll,&QPushButton::clicked,dialog,[retry]{retry(false);});connect(retrySelected,&QPushButton::clicked,dialog,[retry]{retry(true);});
    connect(locate,&QPushButton::clicked,dialog,[this,rows,recordFor]{
        const auto selected=rows->selectedItems();if(selected.size()!=1)return;const auto record=recordFor(selected[0]);
        static_cast<TransferTree *>(tree)->clearHeaderFilters();refresh();auto *row=record["index"].isNull()?jobRows.value(record["job"].toInteger()):entryRow(record["job"].toInteger(),record["index"].toInt());
        if(!row || !row->treeWidget()) {return;}
        for(auto *p=row->parent();p;p=p->parent()) {p->setExpanded(true);}
        tree->clearSelection();tree->setCurrentItem(row);tree->scrollToItem(row);showMainWindow();
    });
    connect(exportButton,&QPushButton::clicked,dialog,[this,dialog,report]{
        if(report->value("records").toArray().isEmpty())return;
        QString selectedFilter=GAD_TR("Text report (*.txt)");
        auto path=QFileDialog::getSaveFileName(dialog,GAD_TR("Export error report"),QDir::homePath()+"/gad-erreurs-"+QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")+".txt",GAD_TR("Text report (*.txt);;JSON report (*.json)"),&selectedFilter);
        if(path.isEmpty()) {return;}
        const bool json=selectedFilter.contains("JSON") || path.endsWith(".json",Qt::CaseInsensitive);
        if(QFileInfo(path).suffix().isEmpty())path+=json?".json":".txt";
        QString message;if(!ErrorReport::save(path,*report,json,message))error(GAD_TR("Export failed: ")+message);
    });
    I18n::onChange(rows,"headers",[rows]{rows->setHeaderLabels({GAD_TR("Request"),GAD_TR("Operation"),GAD_TR("Source"),GAD_TR("Destination"),GAD_TR("Status"),GAD_TR("Error")});},false);
    I18n::onChange(dialog,"reportLanguage",[rows,report,recordFor,detail]{
        QSignalBlocker blocker(rows);const bool sorting=rows->isSortingEnabled();rows->setSortingEnabled(false);
        for(int i=0;i<rows->topLevelItemCount();++i) {
            auto *row=rows->topLevelItem(i);const auto record=recordFor(row);
            const QList<QPair<int,QString>> values={{1,record["mode"]=="move"?GAD_TR("Move"):GAD_TR("Copy")},{4,label(record["state"].toString())},{5,I18n::message(record["error"].toString())}};
            for(const auto &value:values){auto text=value.second;text.replace('\n'," ↵ ");text.replace('\r'," ");text.replace('\t'," ");row->setText(value.first,text);row->setToolTip(value.first,value.second);}
        }
        rows->setSortingEnabled(sorting);
        const auto selected=rows->selectedItems();detail->setPlainText(selected.size()==1?ErrorReport::detail(*report,recordFor(selected[0])):QString());
    },false);
    load();dialog->show();
}
