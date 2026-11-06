"""L’installation Konqueror reste locale et préserve le lanceur standard et les URL."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))
import konqueror_integration as integration


class KonquerorIntegrationTest(unittest.TestCase):
    def test_launcher_preserves_arguments_and_scopes_plugin_path(self):
        with tempfile.TemporaryDirectory(prefix='gad konqueror ') as tmp:
            home=Path(tmp);data=home/'data';binary=home/'bin/gad';source=home/'plugin.so';source.write_bytes(b'fixture')
            (home/'.local/bin').mkdir(parents=True)
            fakebin=home/'fake';fakebin.mkdir();capture=home/'captured.json'
            executable=fakebin/'konqueror'
            executable.write_text('#!'+sys.executable+'\nimport json,os,sys\nfrom pathlib import Path\nPath(os.environ["GAD_CAPTURE"]).write_text(json.dumps([sys.argv[1:],os.environ["QT_PLUGIN_PATH"]]))\n');executable.chmod(0o755)
            with patch.object(integration,'build',return_value=source),patch.object(integration,'data_home',return_value=data),patch.object(Path,'home',return_value=home):
                integration.install(binary)
            launcher=home/'.local/bin/gad-konqueror';url='sftp://host/folder%20with%23?literal=$value'
            env=dict(os.environ,PATH=str(fakebin)+os.pathsep+os.environ['PATH'],QT_PLUGIN_PATH='/previous/plugins',GAD_CAPTURE=str(capture))
            subprocess.run([launcher,url],env=env,check=True)
            import json
            arguments,plugins=json.loads(capture.read_text());self.assertEqual(arguments,[url]);self.assertEqual(plugins,str(data/'gad/konqueror/plugins')+os.pathsep+'/previous/plugins')
            self.assertEqual((data/'gad/konqueror/plugins/konqueror/kpartplugins/plugin.so').read_bytes(),b'fixture')
            self.assertFalse((home/'.local/bin/konqueror').exists())
            self.assertIn('gad-konqueror',(data/'applications/org.hizoka.gad.konqueror.desktop').read_text())
