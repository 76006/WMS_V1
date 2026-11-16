"""Check packaged network-only clients without touching any existing desktop database."""
import argparse
import hashlib
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import lan_integration as base


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--package',type=Path,required=True)
    package=parser.parse_args().package.resolve()
    assert not (package/'sqldrivers'/'qsqlite.dll').exists()
    with tempfile.TemporaryDirectory(prefix='client-isolation-',dir=base.RUNTIME) as temporary:
        root=Path(temporary)
        assert root.resolve().parent==base.RUNTIME.resolve()
        local=root/'existing-desktop.db'
        with sqlite3.connect(local) as db:
            db.execute('CREATE TABLE sentinel(value TEXT)')
            db.execute('INSERT INTO sentinel VALUES(?)',('existing local stock must stay unchanged',))
        db.close()
        def snapshot():return hashlib.sha256(local.read_bytes()).hexdigest(),local.stat().st_mtime_ns
        original=snapshot()
        env=os.environ.copy()
        for key in list(env):
            if key.startswith(('QT_','QML','WMS_')):env.pop(key)
        system_root=os.environ['SystemRoot']
        env['PATH']=os.pathsep.join([str(Path(system_root)/'System32'),system_root])
        env.update({'QT_PLUGIN_PATH':str(package),'QML_IMPORT_PATH':str(package/'qml'),
                    'QML2_IMPORT_PATH':str(package/'qml'),'QT_QPA_PLATFORM':'windows',
                    'WMS_TEST_USERNAME':'admin','WMS_TEST_PASSWORD':base.CREDS['admin'],
                    'WMS_NETWORK_VERIFY':'1','WMS_NETWORK_LOG':str(root/'network-errors.log')})
        startup=subprocess.STARTUPINFO()
        startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow=0
        for app,port in [('IceBeautyWmsLan',18080),('MeiJingLingWmsLan',18081)]:
            exe=package/(app+'.exe')
            def run(args):
                return subprocess.run([str(exe),*map(str,args)],env=env,cwd=package,
                                      startupinfo=startup,creationflags=base.FLAGS,
                                      capture_output=True,timeout=90)
            for args in [['--local'],['--database',local],['--server',f'http://127.0.0.1:{port}','--database',local]]:
                result=run(args)
                assert result.returncode==8,(app,args,result.returncode)
                assert snapshot()==original,'Local database changed after rejected launch'
            base.expect(True,f'{app}: all local-database entry flags rejected without file changes')
            result=run(['--server',f'http://127.0.0.1:{port}','--network-smoke-test'])
            log=root/'network-errors.log'
            assert result.returncode==0,(app,result.returncode,log.read_text(encoding='utf-8') if log.exists() else '')
            assert snapshot()==original
            assert not (root/'existing-desktop.db-wal').exists()
            assert not (root/'existing-desktop.db-shm').exists()
            base.expect(True,f'{app}: complete network UI loads with packaged runtime; existing SQLite bytes and timestamp unchanged')
        print('Network-only package isolation checks passed.')


if __name__=='__main__':main()
