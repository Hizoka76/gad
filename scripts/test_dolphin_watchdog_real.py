#!/usr/bin/env python3
"""Reproduire le rafraîchissement des menus dans Dolphin réel, sans toucher au bureau."""
from pathlib import Path
import json,os,shutil,subprocess,tempfile
from dolphin_integration import build
ROOT=Path(__file__).resolve().parent.parent


def main():
    if not shutil.which('dolphin'):
        print('Dolphin absent : essai réel facultatif ignoré.')
        return
    build()
    cmake=ROOT/'target/dolphin-plugin/cmake'
    subprocess.run(['cmake','-S',ROOT/'native/dolphin','-B',cmake,'-DGAD_WATCHDOG_TESTS=ON'],check=True)
    subprocess.run(['cmake','--build',cmake,'--target','watchdog-real-probe','--parallel','2'],check=True)
    with tempfile.TemporaryDirectory(prefix='gad-watchdog-real-') as temporary:
        root=Path(temporary);data=root/'data';runtime=root/'runtime';runtime.mkdir(mode=0o700)
        service=data/'kio/servicemenus';service.mkdir(parents=True)
        for name in ('gad-paste.desktop','gad-paste-current.desktop'):shutil.copy2(ROOT/'packaging'/name,service/name)
        xml=data/'kxmlgui5/dolphin/dolphinui.rc';xml.parent.mkdir(parents=True)
        xml.write_text('<gui name="dolphin"><ActionProperties scheme="Default"><Action name="edit_paste" shortcut="Shift+Ins"/><Action name="servicemenu_gad-paste-current.desktop::GadPasteCurrent" shortcut="Ctrl+V"/><Action name="servicemenu_gad-paste.desktop::GadPaste" shortcut=""/></ActionProperties></gui>')
        (data/'gad').mkdir();(data/'gad/dolphin-paste.json').write_text(json.dumps({'version':2,'files':{str(xml):{}}}))
        fake=root/'gad';fake.write_text('#!/usr/bin/python3\nimport os,sys\nfrom pathlib import Path\nPath(os.environ["GAD_WATCHDOG_MARKER"]).write_text(" ".join(sys.argv[1:]))\n');fake.chmod(0o755)
        marker=root/'called'
        env=dict(os.environ,HOME=str(root),XDG_DATA_HOME=str(data),XDG_CONFIG_HOME=str(root/'config'),XDG_STATE_HOME=str(root/'state'),XDG_CACHE_HOME=str(root/'cache'),XDG_RUNTIME_DIR=str(runtime),XDG_DATA_DIRS='/usr/share',QT_QPA_PLATFORM='offscreen',QT_PLUGIN_PATH=str(ROOT/'target/dolphin-plugin/plugins'),GAD_DOLPHIN_EXECUTABLE=str(fake),GAD_WATCHDOG_MARKER=str(marker))
        log=ROOT/'target/dolphin-watchdog-real.log'
        with log.open('w') as stream:
            subprocess.run(['dbus-run-session','--','env','LD_PRELOAD='+str(cmake/'libwatchdog-real-probe.so'),'dolphin','-plugin','gad-dolphin-watchdog','--new-window',temporary],env=env,stdout=stream,stderr=stream,timeout=20,check=True)
        lines=log.read_text().splitlines();status=json.loads(next(line.split('WATCHDOG ',1)[1] for line in lines if line.startswith('WATCHDOG ')))
        assert status['healthy'] and status['checks']>1 and status['repairs']>=1 and status['launches']==1 and status['launch_errors']==0,status
        assert marker.read_text()=='--paste-current'
        print('Dolphin réel : raccourci réparé après rafraîchissement et Ctrl+V transmis à Gad.',status)


if __name__=='__main__':main()
