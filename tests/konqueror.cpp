// Vérifie les frappes locales sans toucher au navigateur ni au presse-papiers du bureau.
#include "../native/konqueror/paste_filter.h"
#include <QtTest>
class KonquerorTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void filesUseActiveFolderAndKeepClipboard() {
        QTemporaryDir directory;QVERIFY(directory.isValid());QDir().mkpath(directory.path()+"/second # é");
        QWidget window;auto *layout=new QVBoxLayout(&window);auto *view=new QListWidget;view->addItem("file");layout->addWidget(view);
        window.setProperty("currentURL",QUrl::fromLocalFile(directory.path()).toString(QUrl::FullyEncoded));
        QList<QUrl> submitted;KonquerorPaste::Filter filter(&window,[&](const QUrl &url){submitted.append(url);return true;});
        window.show();window.activateWindow();view->setFocus();QTRY_VERIFY(window.isActiveWindow());
        auto *mime=new QMimeData;mime->setUrls({QUrl::fromLocalFile(directory.path()+"/source")});mime->setData("application/x-kde-cutselection","1");qApp->clipboard()->setMimeData(mime);
        QTest::keyClick(view,Qt::Key_V,Qt::ControlModifier);QCOMPARE(submitted.size(),1);QCOMPARE(submitted.last(),QUrl::fromLocalFile(directory.path()));
        window.setProperty("currentURL",QUrl::fromLocalFile(directory.path()+"/second # é").toString(QUrl::FullyEncoded));
        QTest::keyClick(view,Qt::Key_V,Qt::ControlModifier);QCOMPARE(submitted.size(),2);QCOMPARE(submitted.last(),QUrl::fromLocalFile(directory.path()+"/second # é"));
        QCOMPARE(qApp->clipboard()->mimeData()->data("application/x-kde-cutselection"),QByteArray("1"));
        QTest::keyClick(view,Qt::Key_Insert,Qt::ShiftModifier);QCOMPARE(submitted.size(),2);
        window.setProperty("currentURL","https://example.org");QTest::keyClick(view,Qt::Key_V,Qt::ControlModifier);QCOMPARE(submitted.size(),2);
        window.setProperty("currentURL","sftp://alice@host/folder%20with%23");QTest::keyClick(view,Qt::Key_V,Qt::ControlModifier);QCOMPARE(submitted.size(),3);QCOMPARE(submitted.last().toString(QUrl::FullyEncoded),QString("sftp://alice@host/folder%20with%23"));
    }
    void disabledShortcutKeepsNativeBehavior() {
        QTemporaryDir directory;QWidget window;auto *layout=new QVBoxLayout(&window);auto *view=new QListWidget;layout->addWidget(view);
        window.setProperty("currentURL",QUrl::fromLocalFile(directory.path()).toString());bool enabled=false;int submitted=0;
        KonquerorPaste::Filter filter(&window,[&](const QUrl &){++submitted;return true;},[&]{return enabled;});
        window.show();window.activateWindow();view->setFocus();QTRY_VERIFY(window.isActiveWindow());
        auto *mime=new QMimeData;mime->setUrls({QUrl::fromLocalFile(directory.path()+"/source")});qApp->clipboard()->setMimeData(mime);
        QTest::keyClick(view,Qt::Key_V,Qt::ControlModifier);QCOMPARE(submitted,0);
        enabled=true;QTest::keyClick(view,Qt::Key_V,Qt::ControlModifier);QCOMPARE(submitted,1);
        enabled=false;QTest::keyClick(view,Qt::Key_V,Qt::ControlModifier);QCOMPARE(submitted,1);
    }
    void addressBarAndPlainTextKeepNativePaste() {
        QTemporaryDir directory;QWidget window;auto *layout=new QVBoxLayout(&window);auto *edit=new QLineEdit;auto *view=new QListWidget;layout->addWidget(edit);layout->addWidget(view);
        window.setProperty("currentURL",QUrl::fromLocalFile(directory.path()).toString());
        int submitted=0;KonquerorPaste::Filter filter(&window,[&](const QUrl &){++submitted;return true;});
        window.show();window.activateWindow();edit->setFocus();QTRY_VERIFY(window.isActiveWindow());
        auto *mime=new QMimeData;mime->setUrls({QUrl::fromLocalFile("/tmp/source")});mime->setText("native text");qApp->clipboard()->setMimeData(mime);
        QTest::keyClick(edit,Qt::Key_V,Qt::ControlModifier);QCOMPARE(edit->text(),QString("native text"));QCOMPARE(submitted,0);
        view->setFocus();qApp->clipboard()->setText("text only");QTest::keyClick(view,Qt::Key_V,Qt::ControlModifier);QCOMPARE(submitted,0);
    }
};
QTEST_MAIN(KonquerorTest)
#include "konqueror_test.moc"
