#include "window.h"
#include "bridge.h"
#include <QtDBus>

/// Les préférences sont persistantes ; choisir une action dans le menu l’arme pour une seule file.
void Window::setupCompletion(QVBoxLayout *settings,QHBoxLayout *controls,QDialog *settingsDialog) {
    auto *group=I18n::localized(new QGroupBox,"title",[]{return GAD_TR("After transfers finish");});group->setObjectName("completionSettings");auto *form=new QFormLayout(group);
    completionAction=new QComboBox;completionAction->setObjectName("completionAction");for(const auto &action:QStringList{"none","notify","quit","suspend","poweroff"})completionAction->addItem(QString(),action);
    I18n::onChange(completionAction,"labels",[this]{QSignalBlocker blocker(completionAction);for(int i=0;i<completionAction->count();++i)completionAction->setItemText(i,Completion::label(completionAction->itemData(i).toString()));});
    completionAction->setCurrentIndex(qMax(0,completionAction->findData(QSettings().value("completionAction","none"))));
    completionDelay=new QSpinBox;completionDelay->setObjectName("completionDelay");completionDelay->setRange(5,3600);completionDelay->setValue(QSettings().value("completionDelay",30).toInt());
    form->addRow(I18n::localized(new QLabel,"text",[]{return GAD_TR("Saved action:");}),completionAction);form->addRow(I18n::localized(new QLabel,"text",[]{return GAD_TR("Time to cancel (seconds):");}),completionDelay);
    auto *hint=new QLabel("--completion-action none|notify|quit|suspend|poweroff\n--completion-delay SECONDS");hint->setTextFormat(Qt::PlainText);hint->setWordWrap(true);auto palette=hint->palette();palette.setColor(QPalette::WindowText,palette.color(QPalette::Disabled,QPalette::WindowText));hint->setPalette(palette);form->addRow(hint);
    auto *help=I18n::localized(new QLabel,"text",[]{return GAD_TR("Choose an action and a delay, then click Activate for this batch. Choosing an action or loading a profile only saves the settings; it does not activate anything.\n\nAfter all transfers finish successfully, a countdown gives you time to cancel. The action runs once. It is not activated again when Gad restarts.\n\nAn error, a canceled transfer or a removed request disables the action. Adding transfers postpones it until those transfers finish, with a new full countdown.");});help->setWordWrap(true);form->addRow(help);auto *activation=new QPushButton;activation->setObjectName("completionActivate");form->addRow(activation);
    auto *status=new QLabel;status->setObjectName("completionStatus");status->setWordWrap(true);status->setTextFormat(Qt::PlainText);form->addRow(status);
    connect(activation,&QPushButton::clicked,this,[this]{if(completionPlan.armed)cancelCompletion();else armCompletion();});
    settings->addWidget(group);I18n::onChange(group,"activation",[this]{updateCompletionControls();});
    const auto configure=[this]{const auto result=handle({{"command",Protocol::Command::Configure},{"gui_settings",QJsonObject{{"completion_action",completionAction->currentData().toString()},{"completion_delay",completionDelay->value()}}}});if(result.contains("error")){QSignalBlocker a(completionAction),b(completionDelay);completionAction->setCurrentIndex(qMax(0,completionAction->findData(QSettings().value("completionAction","none"))));completionDelay->setValue(QSettings().value("completionDelay",30).toInt());error(result["error"].toString());}updateCompletionControls();};
    connect(completionAction,&QComboBox::currentIndexChanged,this,[configure]{configure();});connect(completionDelay,&QSpinBox::valueChanged,this,[configure]{configure();});
    completionButton=new QToolButton;completionButton->setObjectName("completionButton");completionButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);completionButton->setIcon(QIcon::fromTheme("chronometer",style()->standardIcon(QStyle::SP_BrowserReload)));completionButton->setPopupMode(QToolButton::InstantPopup);controls->insertWidget(0,completionButton);
    I18n::bindProperty(completionButton,"toolTip",[]{return GAD_TR("Choose an action after all transfers finish successfully, with a countdown you can cancel.");});
    auto *menu=new QMenu(completionButton);completionButton->setMenu(menu);
    connect(menu,&QMenu::aboutToShow,this,[=]{menu->clear();
        const QStringList choices{"none","notify","quit","suspend","poweroff"},icons{"process-stop","dialog-information","application-exit","system-suspend","system-shutdown"};
        for(int i=0;i<choices.size();++i){const auto choice=choices[i];auto *action=menu->addAction(QIcon::fromTheme(icons[i]),Completion::label(choice));action->setObjectName("completionChoose_"+choice);action->setCheckable(true);action->setChecked(choice=="none"?!completionPlan.armed:completionPlan.armed && completionPlan.action==choice);connect(action,&QAction::triggered,this,[this,choice]{cancelCompletion();completionAction->setCurrentIndex(completionAction->findData(choice));if(choice!="none")armCompletion();});}
        menu->addSeparator();auto *arm=menu->addAction(QIcon::fromTheme("media-playback-start"),GAD_TR("Activate saved action for this batch"));arm->setObjectName("completionArm");arm->setEnabled(completionAction->currentData().toString()!="none");connect(arm,&QAction::triggered,this,&Window::armCompletion);
        auto *cancel=menu->addAction(QIcon::fromTheme("process-stop"),GAD_TR("Cancel completion action"));cancel->setObjectName("completionCancel");cancel->setEnabled(completionPlan.armed);connect(cancel,&QAction::triggered,this,&Window::cancelCompletion);
        auto *options=menu->addAction(QIcon::fromTheme("configure"),QString(GAD_TR("Countdown: %1 s…")).arg(completionDelay->value()));connect(options,&QAction::triggered,this,[this,settingsDialog]{settingsDialog->findChild<QTabWidget *>("settingsTabs")->setCurrentWidget(settingsDialog->findChild<QScrollArea *>("completionScroll"));settingsDialog->show();settingsDialog->raise();if(auto *area=settingsDialog->findChild<QScrollArea *>("completionScroll"))area->ensureWidgetVisible(completionAction);completionAction->setFocus();});
    });
    refreshCompletion(state);
}

/// Les options affichent distinctement la préférence enregistrée et l’activation temporaire.
void Window::updateCompletionControls() {
    if(completionButton)completionButton->setText(completionPlan.armed?QString(GAD_TR("On completion: %1 (active)")).arg(Completion::label(completionPlan.action)):GAD_TR("When batch finishes…"));
    if(auto *button=findChild<QPushButton *>("completionActivate")) {
        button->setText(completionPlan.armed?GAD_TR("Deactivate completion action"):GAD_TR("Activate for this batch"));
        button->setIcon(QIcon::fromTheme(completionPlan.armed?"process-stop":"media-playback-start"));
        button->setEnabled(!completionExecuting && (completionPlan.armed || completionAction->currentData().toString()!="none"));
    }
    if(auto *label=findChild<QLabel *>("completionStatus"))label->setText(completionPlan.armed?QString(GAD_TR("Active for this batch: %1. Cancellation countdown: %2 seconds.")).arg(Completion::label(completionPlan.action)).arg(completionPlan.delay):GAD_TR("Inactive: no completion action is scheduled."));
}

/// Désarmer ne modifie aucun fichier ; accepter le dialogue évite de réémettre son signal d’annulation.
void Window::cancelCompletion() {
    if(completionExecuting)return;
    completionPlan.cancel();if(auto *dialog=findChild<QDialog *>("completionCountdown")){QSignalBlocker blocker(dialog);dialog->setObjectName("completionCountdownClosed");dialog->accept();}
    updateCompletionControls();
}
/// L’armement prend un état frais et ignore l’ancien historique déjà terminé.
void Window::armCompletion() {
    if(completionExecuting)return;
    const auto snapshot=engine({{"command",Protocol::Command::Status}});if(snapshot["queue_guard"].toObject().isEmpty()){error(GAD_TR("Batch completion guard unavailable. Restart Gad before arming an action."));return;}
    completionPlan.arm(completionAction->currentData().toString(),completionDelay->value(),snapshot);refreshCompletion(snapshot);
}

/// Appelé même pour une réponse moteur inchangée : le délai utilise l’horloge monotone de la fenêtre.
void Window::refreshCompletion(const QJsonObject &snapshot,qint64 now) {
    if(now<0)now=metricsClock.elapsed();
    if(!completionButton || completionExecuting || closing)return;
    const auto event=completionPlan.observe(snapshot,resolvingSources || !sourceQueue.isEmpty() || profileApplying,now);
    auto *dialog=findChild<QDialog *>("completionCountdown");
    if(dialog && !completionPlan.counting){QSignalBlocker blocker(dialog);dialog->setObjectName("completionCountdownClosed");dialog->accept();dialog=nullptr;}
    updateCompletionControls();
    if(event==Completion::Plan::Abort){statusBar()->showMessage(completionPlan.reason,10000);statusBar()->show();if(!isVisible() || isMinimized())completionNotice(GAD_TR("Completion action canceled"),completionPlan.reason);return;}
    if(completionPlan.counting && !dialog){
        dialog=I18n::localized(new QDialog(this),"windowTitle",[]{return GAD_TR("Batch finished — action countdown");});dialog->setObjectName("completionCountdown");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setMinimumSize(550,240);
        auto *layout=new QVBoxLayout(dialog);auto *label=new QLabel;label->setObjectName("completionCountdownText");label->setTextFormat(Qt::PlainText);label->setWordWrap(true);layout->addWidget(label);
        auto *progress=new QProgressBar;progress->setObjectName("completionCountdownProgress");progress->setRange(0,completionPlan.delay);progress->setTextVisible(false);layout->addWidget(progress);
        auto *notice=I18n::localized(new QLabel,"text",[]{return GAD_TR("Cancel stops this action without changing your transfers. New work restarts the countdown after the batch finishes again.");});notice->setWordWrap(true);layout->addWidget(notice);
        auto *buttons=new QDialogButtonBox;auto *show=I18n::localized(new QPushButton(QIcon::fromTheme("window"),QString()),"text",[]{return GAD_TR("Show Gad");});buttons->addButton(show,QDialogButtonBox::ActionRole);connect(show,&QPushButton::clicked,this,[this,dialog]{showMainWindow();dialog->raise();});
        auto *cancel=I18n::localized(new QPushButton(QIcon::fromTheme("process-stop"),QString()),"text",[]{return GAD_TR("Cancel action");});cancel->setObjectName("completionCountdownCancel");buttons->addButton(cancel,QDialogButtonBox::RejectRole);connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject);connect(dialog,&QDialog::rejected,this,&Window::cancelCompletion);layout->addWidget(buttons);dialog->show();
        if(!isVisible() || isMinimized())completionNotice(GAD_TR("Batch completion countdown"),QString(GAD_TR("%1 in %2 seconds. Open Gad to cancel.")).arg(Completion::label(completionPlan.action)).arg(completionPlan.remaining(now)));
    }
    if(dialog && completionPlan.counting){dialog->findChild<QLabel *>("completionCountdownText")->setText(QString(GAD_TR("%1 in %2 seconds")).arg(Completion::label(completionPlan.action)).arg(completionPlan.remaining(now)));dialog->findChild<QProgressBar *>("completionCountdownProgress")->setValue(completionPlan.remaining(now));}
    if(event!=Completion::Plan::Ready)return;
    // La garde Rust couvre aussi les commandes qui ne passent pas par la fenêtre et les sauvegardes finales.
    if(resolvingSources || !sourceQueue.isEmpty() || profileApplying){cancelCompletion();return;}
    const auto claimed=engine(completionPlan.claim());if(claimed.contains("error")){error(I18n::message(claimed["error"].toString()));return;}
    completionExecuting=true;completionButton->setEnabled(false);for(const auto &name:QStringList{"profilesButton","languageMenuButton","settingsPanel","shortcutsPanel","ratesPanel","completionPanel"})if(auto *widget=findChild<QWidget *>(name))widget->setEnabled(false);const auto action=completionPlan.action;
    executeCompletion(action,[this](QJsonObject result){engine({{"command",Protocol::Command::CompletionRelease}});completionExecuting=false;completionButton->setEnabled(true);for(const auto &name:QStringList{"profilesButton","languageMenuButton","settingsPanel","shortcutsPanel","ratesPanel","completionPanel"})if(auto *widget=findChild<QWidget *>(name))widget->setEnabled(true);updateCompletionControls();if(result.contains("error")){error(result["error"].toString());completionNotice(GAD_TR("Completion action failed"),result["error"].toString());}});
}

/// Toute notification locale rouvre Gad ; les appels sont neutralisés sur les plateformes de test.
void Window::completionNotice(const QString &title,const QString &message) {
    if(QGuiApplication::platformName()=="offscreen" || QGuiApplication::platformName()=="minimal")return;
    tray->showMessage(title,message+"\n"+GAD_TR("Click to open Gad."),QSystemTrayIcon::Information,10000);
}

/// Utiliser logind et ses règles polkit/inhibiteurs, sans commande shell ni contournement des autres applications.
void Window::executeCompletion(const QString &action,std::function<void(QJsonObject)> reply) {
    if(action=="notify"){completionNotice(GAD_TR("Batch finished"),GAD_TR("All tracked transfers finished successfully."));reply({{"ok",true}});return;}
    if(action=="quit"){const bool previous=quitRequested;quitRequested=true;close();if(!closing)quitRequested=previous;reply(closing?QJsonObject{{"ok",true}}:QJsonObject{{"error",GAD_TR("Gad could not close. No power action was requested.")}});return;}
    if(action!="suspend" && action!="poweroff"){reply({{"error",GAD_TR("Invalid completion action")}});return;}
    const auto method=action=="suspend"?QString("Suspend"):QString("PowerOff");
    const auto message=[](const QString &method){return QDBusMessage::createMethodCall("org.freedesktop.login1","/org/freedesktop/login1","org.freedesktop.login1.Manager",method);};
    auto *can=new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message("Can"+method),10000),this);
    connect(can,&QDBusPendingCallWatcher::finished,this,[this,can,method,message,reply]{QDBusPendingReply<QString> capability=*can;can->deleteLater();if(capability.isError() || (capability.value()!="yes" && capability.value()!="challenge")){reply({{"error",GAD_TR("This session cannot perform the requested power action.")}});return;}
        auto call=message(method);call.setArguments({true});auto *request=new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call,30000),this);
        connect(request,&QDBusPendingCallWatcher::finished,this,[request,reply]{QDBusPendingReply<> result=*request;request->deleteLater();reply(result.isError()?QJsonObject{{"error",GAD_TR("The power action request did not complete.")+"\n"+result.error().message()}}:QJsonObject{{"ok",true}});});
    });
}
