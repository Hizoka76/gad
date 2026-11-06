#pragma once
// Filtre local au navigateur : aucune interception globale du clavier.
#include <QtWidgets>
#include <functional>

namespace KonquerorPaste {
/// Le chemin vient directement de la vue active de Konqueror au moment de la frappe.
inline QUrl destination(QObject *host) {
    for(auto *object=host;object;object=object->parent()) {
        const auto value=object->property("currentURL");
        if(!value.isValid())continue;
        const auto text=value.toString();
        const auto url=QDir::isAbsolutePath(text)?QUrl::fromLocalFile(text):QUrl(text);
        if(!QStringList{"file","ftp","sftp","smb","kdeconnect"}.contains(url.scheme()) || !url.isValid())return {};
        if(url.isLocalFile() && !QFileInfo(url.toLocalFile()).isDir())return {};
        return url;
    }
    return {};
}
class Filter : public QObject {
    QPointer<QObject> host;
    std::function<bool(const QUrl &)> paste;
    std::function<bool()> enabled;
public:
    Filter(QObject *owner,std::function<bool(const QUrl &)> submit,std::function<bool()> active=[] {return true;}):QObject(owner),host(owner),paste(std::move(submit)),enabled(std::move(active)){qApp->installEventFilter(this);}
    /// L’édition de texte, les autres fenêtres et Maj+Inser conservent leur comportement Qt.
    bool eventFilter(QObject *,QEvent *event) override {
        if(event->type()!=QEvent::ShortcutOverride && event->type()!=QEvent::KeyPress)return false;
        auto *key=static_cast<QKeyEvent *>(event);
        if(key->key()!=Qt::Key_V || key->modifiers()!=Qt::ControlModifier || !host || !enabled())return false;
        auto *focus=qApp->focusWidget();if(!focus)return false;
        QWidget *window=nullptr;
        for(auto *object=host.data();object;object=object->parent())
            if(object->property("currentURL").isValid()){window=qobject_cast<QWidget *>(object);break;}
        if(!window || focus->window()!=window->window() || !window->isActiveWindow())return false;
        for(auto *widget=focus;widget && widget!=window;widget=widget->parentWidget())
            if(qobject_cast<QLineEdit *>(widget) || qobject_cast<QTextEdit *>(widget) || qobject_cast<QPlainTextEdit *>(widget) || qobject_cast<QAbstractSpinBox *>(widget))return false;
        const auto *mime=qApp->clipboard()->mimeData();
        if(!mime || !(mime->hasUrls() || mime->hasFormat("application/x-kde4-urilist") || mime->hasFormat("x-special/gnome-copied-files")))return false;
        const auto target=destination(host);if(target.isEmpty())return false;
        if(event->type()==QEvent::ShortcutOverride){event->accept();return true;}
        if(key->isAutoRepeat())return true;
        // Après acceptation du raccourci, ne jamais lancer aussi une copie KIO native.
        if(!paste(target))QTimer::singleShot(0,window,[window]{QMessageBox::warning(window,"Gad","Unable to start Gad. Check its installation.");});
        return true;
    }
};
}
