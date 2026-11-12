#!/usr/bin/env python3
"""Tester la CLI et ses délais dans un compte et un bus D-Bus temporaires."""
import json,os,subprocess,tempfile,signal,time,re,socket,threading,sys
from pathlib import Path
from private_test_session import run as run_private_session
project=Path(__file__).resolve().parent.parent
binary=str(Path(os.environ.get('GAD_TEST_BINARY',project/'target/debug/gad')).resolve())
protocol=int(re.search(r'GAD_PROTOCOL_VERSION\((\d+)\)',(project/'native/protocol.def').read_text())[1])
def main():
    """Exécuter les clients et le serveur factice dans la session D-Bus privée."""
    with tempfile.TemporaryDirectory(prefix='gad-cli-test-') as temporary:
     root=Path(temporary);runtime=root/'runtime';runtime.mkdir(mode=0o700)
     env=dict(os.environ,QT_QPA_PLATFORM='offscreen',HOME=str(root),XDG_RUNTIME_DIR=str(runtime),XDG_CONFIG_HOME=str(root/'config'),XDG_DATA_HOME=str(root/'data'),XDG_CACHE_HOME=str(root/'cache'),XDG_DATA_DIRS='/usr/share')
     def cli(*args):
      r=subprocess.run([binary,*args],env=env,capture_output=True,text=True,timeout=15)
      assert r.returncode==0,(args,r.returncode,r.stderr,r.stdout)
      return json.loads(r.stdout)
     # Un serveur interrompu ne doit pas faire passer une réponse JSON invalide pour un succès.
     for response in [b'{', b'not-json\n', b'[]\n', b'{"ok":true}']:
      endpoint=runtime/'gad.sock'
      with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
       server.bind(str(endpoint));server.listen(1);server.settimeout(10)
       def respond():
        with server.accept()[0] as connection:
         connection.settimeout(5);request=b''
         while b'\n' not in request:
          chunk=connection.recv(4096)
          if not chunk:raise AssertionError('IPC client disconnected before its request')
          request+=chunk
         connection.sendall(response)
       peer=threading.Thread(target=respond,daemon=True);peer.start()
       result=subprocess.run([binary,'--status'],env=env,capture_output=True,text=True,timeout=15)
       peer.join(timeout=10)
       assert not peer.is_alive(), 'Unfinished fake IPC server'
      endpoint.unlink()
      assert result.returncode==1, ('Invalid IPC response accepted',response,result)
     # Une sonde de compatibilité corrompue n’est pas une ancienne version de Gad.
     for response in [b'{', b'not-json\n', b'[]\n', b'{}\n', b'{"protocol_version":1.5}\n', b'{"protocol_version":"35"}\n']:
      endpoint=runtime/'gad.sock'
      with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as server:
       server.bind(str(endpoint));server.listen(2);server.settimeout(5)
       def invalid_probe():
        with server.accept()[0] as primary:
         with server.accept()[0] as probe:
          probe.settimeout(5);probe.recv(4096);probe.sendall(response)
       peer=threading.Thread(target=invalid_probe,daemon=True);peer.start()
       result=subprocess.run([binary,'--configure','--rate-limit','0'],env=env,capture_output=True,text=True,timeout=5)
       peer.join(timeout=5);assert not peer.is_alive()
      endpoint.unlink()
      assert result.returncode==1 and not result.stdout, ('Invalid compatibility probe',response,result)
     # Les configurations et la résolution KIO peuvent dépasser dix secondes.
     for arguments, command in [(['--configure','--rate-limit','0'],'configure'),
                                (['--copy','sftp://localhost/example.txt','--to',str(root)],'enqueue')]:
      endpoint=runtime/'gad.sock';peer_errors=[]
      with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as server:
       server.bind(str(endpoint));server.listen(2);server.settimeout(15)
       def receive_line(connection):
        connection.settimeout(15);message=b''
        while b'\n' not in message:
         chunk=connection.recv(4096)
         if not chunk:raise ConnectionError('IPC client disconnected')
         message+=chunk
        return json.loads(message)
       def delayed_response():
        try:
         with server.accept()[0] as primary:
          with server.accept()[0] as probe:
           assert receive_line(probe)['command']=='status'
           probe.sendall(json.dumps({'protocol_version':protocol}).encode()+b'\n')
          assert receive_line(primary)['command']==command
          time.sleep(10.2)
          primary.sendall(b'{"ok":true}\n')
        except (OSError,AssertionError) as error:peer_errors.append(str(error))
       peer=threading.Thread(target=delayed_response,daemon=True);peer.start()
       result=subprocess.run([binary,*arguments],env=env,capture_output=True,text=True,timeout=20)
       peer.join(timeout=15);assert not peer.is_alive()
      endpoint.unlink()
      assert result.returncode==0 and json.loads(result.stdout).get('ok'), ('IPC reply timed out',command,result,peer_errors)
      assert not peer_errors,peer_errors
     # Aucun appel système de test ne peut atteindre logind sur la machine réelle.
     env["DBUS_SYSTEM_BUS_ADDRESS"]=env["DBUS_SESSION_BUS_ADDRESS"]
     # Préférences Qt initiales privées : un profil complet ne contient aucun chemin de destination.
     profile={'id':'11111111-1111-4111-8111-111111111111','name':'CLI profile',
      'options':{'verify_md5':False,'verify_properties':False,'verify_space':False,'allow_duplication':False,'keep_history':True,'resume_interrupted':False,'rate_limit':2097152,'exclusions':{'include_hidden':True,'directories':[],'files':['*.tmp']}},
      'gui':{'language':'en_US','autostart':False,'dolphin_ctrl_v':False,'konqueror_ctrl_v':False},'request_rate_limit':1310720}
     config=root/'config/Gad/Gad.conf';config.parent.mkdir(parents=True)
     encoded=json.dumps({'version':2,'profiles':[profile]},ensure_ascii=False,separators=(',',':')).replace('\\','\\\\').replace('"','\\"')
     config.write_text('[General]\ntransferProfiles="'+encoded+'"\n')
     try:
      assert cli('--profile','CLI profile')['ok']
      # -h est ici un motif d’exclusion, pas une demande d’aide.
      assert cli('--configure','--exclude-file','-h')['ok']
      assert '-h' in cli('--status')['exclusions']['files']
      # La demande combinée est refusée avant toute installation de raccourci.
      invalid_shortcut=subprocess.run([binary,'--configure','--krusader-ctrl-v','--language','not-a-locale'],env=env,capture_output=True,text=True,timeout=15)
      assert invalid_shortcut.returncode==1 and 'error' in json.loads(invalid_shortcut.stdout),invalid_shortcut
      assert not (root/'data/gad/krusader-paste.json').exists(), 'Krusader changed before configuration validation'

      # Enregistrer le nettoyage ne doit pas prendre la garde ni effacer l’historique.
      assert cli('--configure','--completion-action','clear')['ok']
      assert 'completionAction=clear' in config.read_text()
      assert cli('--status')['queue_guard']['claimed'] is False
      result=cli('--configure','--dolphin-ctrl-v','--md5','--properties','--verify-space','--no-include-hidden','--allow-duplication','--keep-history','--resume-interrupted','--rate-limit','2.5','--completion-action','notify','--completion-delay','45','--verify-fast-move','--conflict-policy','newer','--exclude-folder','__*','--exclude-file','*.tmp')
      assert result['ok'],result
      cli('--configure','--no-progress-message');assert 'progressMessage=false' in config.read_text()
      cli('--configure','--progress-message');assert 'progressMessage=true' in config.read_text()
      cli('--configure','--no-progress-message')
      receipt=root/'data/gad/dolphin-paste.json';assert json.loads(receipt.read_text())['files']
      state=cli('--status');assert state['protocol_version']==protocol,state
      assert 'completionAction=notify' in config.read_text() and 'completionDelay=45' in config.read_text()
      assert state['queue_guard']['claimed'] is False
      assert state['verify_md5'] and state['verify_properties'] and state['verify_space'] and state['allow_duplication'],state
      assert state['conflict_policy']=='newer' and state['verify_fast_move'] is True,state
      assert state['rate_limit']==2621440 and state['exclusions']['include_hidden'] is False,state
      assert state['exclusions']['directories']==['__*'],state
      cli('--configure','--include-hidden','--language','en_US','--no-konqueror-ctrl-v','--autostart')
      state=cli('--status');assert state['exclusions']['include_hidden'] and state['exclusions']['directories']==['__*']
      assert list((root/'config/autostart').glob('*.desktop'))
      cli('--configure','--no-dolphin-ctrl-v','--no-autostart','--clear-exclusions','--no-md5','--no-properties','--no-verify-space','--no-verify-fast-move','--no-allow-duplication','--no-keep-history','--no-resume-interrupted','--rate-limit','0')
      state=cli('--status');assert not state['verify_md5'] and not state['verify_properties'] and not state['verify_space'] and not state['allow_duplication']
      assert state['exclusions']['directories']==[] and state['rate_limit']==0
      assert json.loads(receipt.read_text())['files']=={}
      assert not state['keep_history'] and not state['resume_interrupted']
      saved_receipt=receipt.read_bytes();receipt.write_text('invalid receipt')
      rejected=subprocess.run([binary,'--configure','--dolphin-ctrl-v'],env=env,capture_output=True,text=True,timeout=15)
      assert rejected.returncode==1 and 'error' in json.loads(rejected.stdout),rejected
      receipt.write_bytes(saved_receipt)
      invalid=subprocess.run([binary,'--configure','--rate-limit','nan'],env=env,capture_output=True,text=True,timeout=5);assert invalid.returncode==2
      cli('--configure','--request-rate-limit','1.25')
      source=root/'source.txt';source.write_text('profile rate fixture')
      target=root/'target';target.mkdir()
      cli('--copy',str(source),'--to',str(target))
      state=cli('--status');assert state['jobs'][-1]['created_at']>0,state
      assert state['jobs'][-1]['policy']=='newer' and state['jobs'][-1]['rate_limit']==1310720 and state['rate_limit']==0,state
      cli('--configure','--request-rate-limit','0')
      before=cli('--status');assert cli('--profile=CLI profile')['ok']
      state=cli('--status');assert len(state['jobs'])==len(before['jobs']) and state['rate_limit']==2097152,state
      target_profile=root/'profile-target';target_profile.mkdir()
      cli('--copy',str(source),'--to',str(target_profile),'--profile','CLI profile','--md5')
      state=cli('--status');job=state['jobs'][-1];assert job['rate_limit']==1310720 and job['verify_md5'] and job['destination']==str(target_profile),state
      assert state['verify_md5'] is False and job['policy']=='ask',state
      count=len(state['jobs']);missing=subprocess.run([binary,'--copy',str(source),'--to',str(target_profile),'--profile','missing'],env=env,capture_output=True,text=True,timeout=15)
      assert missing.returncode==1 and 'error' in json.loads(missing.stdout),(missing.stdout,missing.stderr)
      assert len(cli('--status')['jobs'])==count
      print('CLI isolée : démarrage automatique, configuration, persistance, activation/désactivation et validations OK.')
     finally:
      lock=runtime/'gad.lock'
      if lock.exists():
       pid=int(lock.read_text().splitlines()[0]);os.kill(pid,signal.SIGTERM)
       for _ in range(40):
        if not Path('/proc',str(pid)).exists():break
        time.sleep(.05)


if __name__ == "__main__":
    if sys.argv[1:] == ["--private-session"]:
        main()
    else:
        run_private_session(__file__)
