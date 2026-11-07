// Plugin générique Qt chargé uniquement par le lanceur Dolphin de Gad.
#include <QGenericPlugin>
#include "watchdog.h"
class GadDolphinPlugin:public QGenericPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QGenericPluginFactoryInterface_iid FILE "plugin.json")
public:
    QObject *create(const QString &key,const QString &) override {
        return key=="gad-dolphin-watchdog"?new DolphinWatchdog::Monitor:nullptr;
    }
};
#include "plugin.moc"
