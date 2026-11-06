// Vérifie les vrais raccourcis KXMLGUI sans lancer Dolphin ni copier de fichiers.
#include <QtWidgets>
#include <QtTest>
#include <KXmlGuiWindow>
#include <KActionCollection>
#include <KXMLGUIFactory>
#include <cstdlib>
#include <KService>
#include <KIO/DesktopExecParser>

class Window:public KXmlGuiWindow {
public:
 int gad=0,native=0;
 QWidget *view;QLineEdit *edit;
 /// Charge les mêmes propriétés XML et les mêmes raccourcis standard que Dolphin.
 Window(const QString &path) {
  setComponentName("dolphin","Dolphin");
  auto *central=new QWidget;auto *layout=new QVBoxLayout(central);
  view=new QWidget;view->setFocusPolicy(Qt::StrongFocus);view->setMinimumSize(300,100);edit=new QLineEdit;
  layout->addWidget(view);layout->addWidget(edit);setCentralWidget(central);
  auto *paste=new QAction("Coller KIO",this);actionCollection()->addAction("edit_paste",paste);
  actionCollection()->setDefaultShortcuts(paste,QKeySequence::keyBindings(QKeySequence::Paste));
  connect(paste,&QAction::triggered,this,[this]{++native;});
  auto *copy=new QAction("Coller avec Gad",this);actionCollection()->addAction("servicemenu_gad-paste-current.desktop::GadPasteCurrent",copy);
  actionCollection()->setDefaultShortcuts(copy,{});
  connect(copy,&QAction::triggered,this,[this]{++gad;});
  auto *context=new QAction("Coller avec Gad (contexte)",this);actionCollection()->addAction("servicemenu_gad-paste.desktop::GadPaste",context);
  actionCollection()->setDefaultShortcuts(context,{});connect(context,&QAction::triggered,this,[this]{++gad;});
  setupGUI(KXmlGuiWindow::Create,path);show();activateWindow();
 }
};
int main(int argc,char **argv) {
 QApplication app(argc,argv);Window window(QString::fromLocal8Bit(argv[1]));QTest::qWait(100);window.view->setFocus();app.processEvents();
 auto check=[](bool ok,const char *msg){if(!ok){qFatal("%s",msg);}};
 // Un %U explicite transmet le contexte sans téléchargement ; Gad l’ignore ensuite.
 KService service("Gad", "/bin/true --paste-dolphin %U", "gad");
 for(const QList<QUrl> urls:{QList<QUrl>{QUrl::fromLocalFile("/ancienne selection")},QList<QUrl>{QUrl::fromLocalFile("/ancienne selection"),QUrl("sftp://serveur/dossier")}}) {
  KIO::DesktopExecParser parser(service,urls);
  const auto arguments=parser.resultingArguments();
  check(arguments.size()==2+urls.size() && arguments[0]=="/bin/true" && arguments[1]=="--paste-dolphin","KIO doit lancer Gad directement, sans kioexec ni montage du contexte");
 }
 const bool restored=argc>2;
 if(restored) {
  QTest::keyClick(window.view,Qt::Key_V,Qt::ControlModifier);check(window.native==1 && window.gad==0,"Ctrl+V natif n’est pas restauré");
  QTest::keyClick(window.view,Qt::Key_V,Qt::ControlModifier|Qt::ShiftModifier);check(window.gad==0,"Le raccourci secondaire Gad doit rester supprimé après restauration");
 } else {
  QTest::keyClick(window.view,Qt::Key_V,Qt::ControlModifier);check(window.gad==1 && window.native==0,"Ctrl+V ne déclenche pas Gad");
  QTest::keyClick(window.view,Qt::Key_V,Qt::ControlModifier|Qt::ShiftModifier);check(window.gad==1,"Ctrl+Maj+V doit rester libre");
  QTest::keyClick(window.view,Qt::Key_V,Qt::ControlModifier|Qt::AltModifier);check(window.native==0 && window.gad==1,"Ctrl+Alt+V doit rester désactivé pour le collage");
  QTest::keyClick(window.view,Qt::Key_Insert,Qt::ShiftModifier);check(window.native==1,"Maj+Inser est indisponible");
 }
 const auto beforeGad=window.gad,beforeNative=window.native;
 QApplication::clipboard()->setText("Texte à coller");window.edit->setFocus();app.processEvents();
 QTest::keyClick(window.edit,Qt::Key_V,Qt::ControlModifier);
 check(window.edit->text()=="Texte à coller" && window.gad==beforeGad && window.native==beforeNative,"Le collage de texte est intercepté");
 qInfo("OK : Ctrl+V Gad, raccourci secondaire libre, Maj+Inser KIO, Ctrl+Alt+V libre, texte natif dans QLineEdit.");
}
