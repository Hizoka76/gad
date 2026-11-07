#!/usr/bin/env python3
"""Tester la CLI dans un compte temporaire ; lancer sous dbus-run-session."""
import json,os,subprocess,tempfile,signal,time
from pathlib import Path
binary=str(Path('target/release/gad').resolve())
with tempfile.TemporaryDirectory(prefix='gad-cli-test-') as temporary:
 root=Path(temporary);runtime=root/'runtime';runtime.mkdir(mode=0o700)
 env=dict(os.environ,QT_QPA_PLATFORM='offscreen',HOME=str(root),XDG_RUNTIME_DIR=str(runtime),XDG_CONFIG_HOME=str(root/'config'),XDG_DATA_HOME=str(root/'data'),XDG_CACHE_HOME=str(root/'cache'),XDG_DATA_DIRS='/usr/share')
 def cli(*args):
  r=subprocess.run([binary,*args],env=env,capture_output=True,text=True,timeout=15)
  assert r.returncode==0,(args,r.returncode,r.stderr,r.stdout)
  return json.loads(r.stdout)
 try:
  result=cli('--configure','--dolphin-ctrl-v','--md5','--properties','--verify-space','--no-include-hidden','--allow-duplication','--keep-history','--resume-interrupted','--rate-limit','2.5','--exclude-folder','__*','--exclude-file','*.tmp')
  assert result['ok'],result
  receipt=root/'data/gad/dolphin-paste.json';assert json.loads(receipt.read_text())['files']
  state=cli('--status');assert state['protocol_version']==24,state
  assert state['verify_md5'] and state['verify_properties'] and state['verify_space'] and state['allow_duplication'],state
  assert state['rate_limit']==2621440 and state['exclusions']['include_hidden'] is False,state
  assert state['exclusions']['directories']==['__*'],state
  cli('--configure','--include-hidden','--language','en_US','--no-konqueror-ctrl-v','--autostart')
  state=cli('--status');assert state['exclusions']['include_hidden'] and state['exclusions']['directories']==['__*']
  assert list((root/'config/autostart').glob('*.desktop'))
  cli('--configure','--no-dolphin-ctrl-v','--no-autostart','--clear-exclusions','--no-md5','--no-properties','--no-verify-space','--no-allow-duplication','--no-keep-history','--no-resume-interrupted','--rate-limit','0')
  state=cli('--status');assert not state['verify_md5'] and not state['verify_properties'] and not state['verify_space'] and not state['allow_duplication']
  assert state['exclusions']['directories']==[] and state['rate_limit']==0
  assert json.loads(receipt.read_text())['files']=={}
  assert not state['keep_history'] and not state['resume_interrupted']
  saved_receipt=receipt.read_bytes();receipt.write_text('invalid receipt')
  rejected=subprocess.run([binary,'--configure','--dolphin-ctrl-v'],env=env,capture_output=True,text=True,timeout=15)
  assert rejected.returncode==1 and 'error' in json.loads(rejected.stdout),rejected
  receipt.write_bytes(saved_receipt)
  invalid=subprocess.run([binary,'--configure','--rate-limit','nan'],env=env,capture_output=True,text=True,timeout=5);assert invalid.returncode==2
  print('CLI isolée : démarrage automatique, configuration, persistance, activation/désactivation et validations OK.')
 finally:
  lock=runtime/'gad.lock'
  if lock.exists():
   pid=int(lock.read_text().splitlines()[0]);os.kill(pid,signal.SIGTERM)
   for _ in range(40):
    if not Path('/proc',str(pid)).exists():break
    time.sleep(.05)
