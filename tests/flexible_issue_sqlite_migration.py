"""Exercise both v22 layouts and populated LAN v23/v24 databases via the desktop migrator."""
import os
from contextlib import closing
from pathlib import Path
import sqlite3
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def historical_fixture(path, version, standalone=False):
    with closing(sqlite3.connect(path)) as db, db:
        db.executescript((ROOT / 'database/schema.sql').read_text(encoding='utf-8'))
        for number in range(2, version + 1):
            script = next((ROOT / 'database/migrations').glob(f'{number:03d}_*.sql')).read_text(encoding='utf-8')
            if standalone and number == 2:
                script = script.replace('COLLATE NOCASE UNIQUE,', 'COLLATE NOCASE,', 1)
                script = script.replace('\n);', ',\n    UNIQUE (batch_no, product_material_id)\n);', 1)
            if standalone and number == 22:
                script = ('ALTER TABLE business_documents ADD COLUMN issue_unit_count NUMERIC CHECK (issue_unit_count>0);'
                          'INSERT INTO schema_migrations(version) VALUES(22);')
            db.executescript(script)
        db.execute("INSERT INTO users(id,role_id,username,display_name,password_hash) VALUES(1,1,'admin','Admin','test')")
        db.execute("INSERT INTO system_identity(singleton_id,profile_id,display_name) VALUES(1,'ICE_BEAUTY','fixture')")
        db.executemany("INSERT INTO materials(id,code,name,unit) VALUES(?,?,?,'ea')",
                       [(71, 'P1', 'one'), (72, 'P2', 'two')])
        db.execute("INSERT INTO production_runs(id,batch_no,product_material_id,product_name,planned_quantity,created_by) "
                   "VALUES(81,'shared',71,'one',10,1)")
        db.execute("UPDATE sqlite_sequence SET seq=200 WHERE name='production_runs'")
        db.execute("INSERT INTO business_documents(id,document_no,document_type,stock_direction,document_date,created_by,production_run_id) "
                   "VALUES(91,'old','SCLL','OUT','2026-01-01',1,81)")
        if standalone:
            db.execute("UPDATE business_documents SET issue_unit_count=3 WHERE id=91")
        if version >= 23:
            db.execute("INSERT INTO production_task_boms VALUES(81,'[]')")
            db.execute("INSERT INTO production_document_stages VALUES(91,81,'assembly')")


def main():
    exe = Path(os.environ.get('WMS_TEST_SQLITE_BUILD', ROOT / 'build-lan-business')) / 'IceBeautyWms.exe'
    env = os.environ | {'QT_QPA_PLATFORM': 'offscreen'}
    env['PATH'] = os.environ.get('WMS_TEST_QT_BIN', 'D:/Qt/6.5.3/mingw_64/bin') + os.pathsep + env['PATH']
    with tempfile.TemporaryDirectory() as folder:
        for version, standalone in [(22, True), (22, False), (23, False), (24, False)]:
            path = Path(folder) / f'{version}-{standalone}.db'
            historical_fixture(path, version, standalone)
            for _ in range(2):
                subprocess.run([exe, '--database', path, '--smoke-test'], env=env, check=True, timeout=30)
            with closing(sqlite3.connect(path)) as db, db:
                assert db.execute('SELECT max(version) FROM schema_migrations').fetchone() == (25,), 'expected migration 25'
                assert db.execute('SELECT count(*) FROM process_boms').fetchone() == (0,)
                assert db.execute('SELECT production_run_id,issue_unit_count FROM business_documents WHERE id=91').fetchone() == (81, 3 if standalone else None)
                if version >= 23:
                    assert db.execute('SELECT * FROM production_task_boms').fetchone() == (81, '[]')
                    assert db.execute('SELECT * FROM production_document_stages').fetchone() == (91, 81, 'assembly')
                assert db.execute('PRAGMA foreign_key_check').fetchall() == []
                insert = ("INSERT INTO production_runs(batch_no,product_material_id,product_name,planned_quantity,created_by) "
                          "VALUES('SHARED',?,'two',1,1)")
                assert db.execute(insert, (72,)).lastrowid == 201
                try:
                    db.execute(insert, (71,))
                except sqlite3.IntegrityError:
                    pass
                else:
                    raise AssertionError('duplicate product and batch accepted')
            print(f'PASS SQLite v{version} (standalone={standalone})')
        path = Path(folder) / 'broken-stage.db'
        historical_fixture(path, 24)
        with closing(sqlite3.connect(path)) as db, db:
            db.execute('UPDATE production_document_stages SET production_run_id=999')
        failed = subprocess.run([exe, '--database', path, '--smoke-test'], env=env, timeout=30, capture_output=True)
        assert failed.returncode != 0
        with closing(sqlite3.connect(path)) as db:
            assert db.execute('SELECT max(version) FROM schema_migrations').fetchone() == (24,)
            assert 'issue_unit_count' not in {row[1] for row in db.execute('PRAGMA table_info(business_documents)')}
            assert db.execute('SELECT * FROM production_document_stages').fetchone() == (91, 999, 'assembly')
        print('PASS invalid stage reference rolls back the entire v25 migration')


if __name__ == '__main__':
    main()
