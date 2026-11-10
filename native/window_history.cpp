#include "window.h"
#include "protocol.h"

/// Barre repliable : les critères se combinent aux filtres de colonnes, sans requête de mutation moteur.
void Window::setupHistorySearch(QVBoxLayout *layout,QHBoxLayout *controls) {
    auto *toggle=I18n::localized(new QPushButton(QIcon::fromTheme("edit-find",style()->standardIcon(QStyle::SP_FileDialogContentsView)),QString()),"text",[]{return GAD_TR("Search history");});
    toggle->setObjectName("historySearchButton");toggle->setCheckable(true);controls->insertWidget(0,toggle);
    I18n::bindProperty(toggle,"toolTip",[]{return GAD_TR("Show or hide search by file name, path, date or status. (Ctrl+F)");});
    auto *panel=new QWidget;panel->setObjectName("historySearchPanel");auto *box=new QVBoxLayout(panel);box->setContentsMargins(0,0,0,0);
    auto *top=new QHBoxLayout;
    auto *text=new QLineEdit;text->setObjectName("historySearchText");text->setClearButtonEnabled(true);
    I18n::bindProperty(text,"placeholderText",[]{return GAD_TR("File name or source/destination path…");});
    I18n::bindProperty(text,"accessibleName",[]{return GAD_TR("Search history");});top->addWidget(text,1);
    auto *states=new QComboBox;states->setObjectName("historySearchState");states->addItem(QString(),QString());
#define GAD_PROTOCOL_VERSION(number)
#define GAD_COMMAND(name, wire)
#define GAD_STATE(name, wire, terminal) states->addItem(QString(),Protocol::State::name);
#include "protocol.def"
#undef GAD_STATE
#undef GAD_COMMAND
#undef GAD_PROTOCOL_VERSION
    I18n::onChange(states,"labels",[states]{QSignalBlocker blocker(states);states->setItemText(0,GAD_TR("All statuses"));for(int i=1;i<states->count();++i)states->setItemText(i,label(states->itemData(i).toString()));});top->addWidget(states);
    auto *history=I18n::localized(new QCheckBox,"text",[]{return GAD_TR("History only");});history->setObjectName("historySearchOnly");history->setChecked(true);top->addWidget(history);
    auto *reset=I18n::localized(new QPushButton(QIcon::fromTheme("edit-clear"),QString()),"text",[]{return GAD_TR("Reset search");});reset->setObjectName("historySearchReset");top->addWidget(reset);
    auto *close=new QToolButton;close->setObjectName("historySearchClose");close->setIcon(QIcon::fromTheme("window-close",style()->standardIcon(QStyle::SP_DialogCloseButton)));I18n::bindProperty(close,"toolTip",[]{return GAD_TR("Close search");});top->addWidget(close);box->addLayout(top);
    auto *bottom=new QHBoxLayout;
    auto *dates=I18n::localized(new QCheckBox,"text",[]{return GAD_TR("Added between:");});dates->setObjectName("historySearchDates");bottom->addWidget(dates);
    auto *from=new QDateEdit(QDate::currentDate().addMonths(-1));from->setObjectName("historySearchFrom");from->setCalendarPopup(true);bottom->addWidget(from);
    bottom->addWidget(I18n::localized(new QLabel,"text",[]{return GAD_TR("and");}));
    auto *to=new QDateEdit(QDate::currentDate());to->setObjectName("historySearchTo");to->setCalendarPopup(true);bottom->addWidget(to);bottom->addStretch();
    I18n::onChange(panel,"dates",[from,to]{const auto locale=QLocale();for(auto *edit:{from,to}){edit->setLocale(locale);edit->setDisplayFormat(locale.dateFormat(QLocale::ShortFormat));}});
    I18n::bindProperty(dates,"toolTip",[]{return GAD_TR("Inclusive local dates when the request was added.\nOlder history without a recorded date is excluded only while the date filter is enabled.\nHistory must be retained; cleared operations cannot be searched.");});
    box->addLayout(bottom);layout->addWidget(panel);panel->hide();
    const auto apply=[=]{HistorySearch::Query query;query.enabled=toggle->isChecked();query.text=text->text();query.state=states->currentData().toString();query.historyOnly=history->isChecked();query.dates=dates->isChecked();query.from=from->date();query.to=to->date();from->setEnabled(query.dates);to->setEnabled(query.dates);static_cast<TransferTree *>(tree)->setHistorySearch(query);};
    // Temporiser la frappe évite de reparcourir un grand historique à chaque touche.
    auto *timer=new QTimer(panel);timer->setSingleShot(true);timer->setInterval(150);connect(timer,&QTimer::timeout,panel,apply);
    connect(text,&QLineEdit::textChanged,panel,[timer]{timer->start();});
    connect(toggle,&QPushButton::toggled,panel,[=](bool visible){panel->setVisible(visible);timer->stop();apply();if(visible){text->setFocus();text->selectAll();}});
    connect(states,&QComboBox::currentIndexChanged,panel,[=]{timer->stop();apply();});
    connect(history,&QCheckBox::toggled,panel,[=]{timer->stop();apply();});connect(dates,&QCheckBox::toggled,panel,[=]{timer->stop();apply();});
    connect(from,&QDateEdit::dateChanged,panel,[=](const QDate &date){if(date>to->date()){QSignalBlocker blocker(to);to->setDate(date);}apply();});
    connect(to,&QDateEdit::dateChanged,panel,[=](const QDate &date){if(date<from->date()){QSignalBlocker blocker(from);from->setDate(date);}apply();});
    connect(reset,&QPushButton::clicked,panel,[=]{QSignalBlocker a(text),b(states),c(history),d(dates);timer->stop();text->clear();states->setCurrentIndex(0);history->setChecked(true);dates->setChecked(false);apply();text->setFocus();});
    connect(close,&QToolButton::clicked,toggle,[toggle]{toggle->setChecked(false);});
    auto *find=new QShortcut(QKeySequence::Find,this);connect(find,&QShortcut::activated,panel,[=]{toggle->setChecked(true);text->setFocus();text->selectAll();});
    auto *escape=new QShortcut(QKeySequence(Qt::Key_Escape),panel);escape->setContext(Qt::WidgetWithChildrenShortcut);connect(escape,&QShortcut::activated,toggle,[toggle]{toggle->setChecked(false);});
    from->setEnabled(false);to->setEnabled(false);
}
