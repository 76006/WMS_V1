"""Migrate synthetic SQLite files into disposable PostgreSQL databases only."""
import base64
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import time
import uuid

import lan_integration as base

SERVER = base.BUILD / 'WmsServer.exe'
PASSWORD = 'LegacyAdmin123!'
BLOB = bytes(range(256)) * 31 + b'\x00legacy attachment\xff'
IMAGE = b'\x89PNG\r\n\x1a\n\x00test image'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def password_hash():
    salt = b'legacy-test-salt'
    encode = lambda b: base64.b64encode(b).decode().rstrip('=')
    key = hashlib.pbkdf2_hmac('sha256', PASSWORD.encode(), salt, 10000)
    return f'pbkdf2_sha256$10000${encode(salt)}${encode(key)}'


@contextmanager
def running(env, product):
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    proc = subprocess.Popen([str(SERVER), '--product', product, '--port', str(port)],
                            env=env, creationflags=base.FLAGS,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            if proc.poll() is not None:
                raise RuntimeError('Test server exited')
            try:
                if base.api(port, '/health')[0] == 200:
                    break
            except OSError:
                pass
            time.sleep(.1)
        else:
            raise RuntimeError('Server startup timed out')
        yield port
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=10)


def fixture(path, exe):
    result = base.run([base.ROOT / 'build-lan-business' / exe,
                       '--database', path, '--smoke-test'], check=False)
    assert result.returncode == 0, result.stderr
    with sqlite3.connect(path) as c:
        c.execute('PRAGMA foreign_keys=ON')
        c.execute('UPDATE users SET password_hash=?', (password_hash(),))
        c.execute("INSERT INTO users(id,role_id,username,display_name,password_hash) VALUES(41,1,'old-operator','旧操作员',?)", (password_hash(),))
        c.execute("INSERT INTO warehouses(id,code,name) VALUES(51,'OLD-WH','旧仓库')")
        c.execute("INSERT INTO locations(id,warehouse_id,code) VALUES(61,51,'A01')")
        c.execute("INSERT INTO materials(id,code,name,unit,default_warehouse_id,default_location_id,require_batch) VALUES(71,'OLD-MAT','旧物料','个',51,61,1)")
        c.execute("INSERT INTO production_runs(id,batch_no,product_material_id,product_name,planned_quantity,created_by) VALUES(81,'PROD-OLD',71,'旧物料',20,41)")
        c.execute("INSERT INTO business_documents(id,document_no,document_type,stock_direction,document_date,status,created_by,production_run_id) VALUES(91,'OLD-0001','QTRK','IN','2026-01-01','POSTED',41,81)")
        c.execute("INSERT INTO business_document_items(id,document_id,line_number,material_id,quantity,batch_no,warehouse_id,location_id) VALUES(101,91,1,71,100.125,'B-OLD',51,61)")
        c.execute("INSERT INTO stock_balances(id,material_id,warehouse_id,location_id,batch_no,quantity) VALUES(111,71,51,61,'B-OLD',100.125)")
        c.execute("INSERT INTO inventory_ledger(id,business_type,document_id,document_item_id,material_id,batch_no,quantity_in,quantity_before,quantity_after,warehouse_id,location_id,operator_id) VALUES(121,'QTRK',91,101,71,'B-OLD',100.125,0,100.125,51,61,41)")
        c.execute("INSERT INTO batches(id,material_id,batch_no) VALUES(131,71,'B-OLD')")
        c.execute("INSERT INTO serial_numbers(id,material_id,serial_no,batch_no,status,warehouse_id,location_id,last_document_id) VALUES(141,71,'SN-OLD','B-OLD','IN_STOCK',51,61,91)")
        c.execute("INSERT INTO attachments(id,business_type,business_id,original_file_name,mime_type,file_size,sha256,file_data,uploaded_by) VALUES(151,'QTRK',91,'历史附件.bin','application/octet-stream',?,?,?,41)", (len(BLOB), hashlib.sha256(BLOB).hexdigest(), BLOB))
        c.execute("INSERT INTO material_images(material_id,original_file_name,sha256,image_data,updated_by) VALUES(71,'旧图片.png',?,?,41)", (hashlib.sha256(IMAGE).hexdigest(), IMAGE))
        c.execute("INSERT INTO document_forms(id,document_id,form_kind,payload,status,attachment_id) VALUES(161,91,'INBOUND','{\"legacy\":true}','COMPLETED',151)")
        c.execute("INSERT INTO audit_logs(id,user_id,action,entity_type,entity_id,detail) VALUES(171,41,'POST','document',91,'旧单据审计')")
        c.execute("UPDATE number_rules SET current_sequence=37,sequence_date='20260101' WHERE document_type='QTRK'")
        assert not list(c.execute('PRAGMA foreign_key_check'))
    c.close()


def main():
    folder = base.RUNTIME / ('legacy-import-test-' + uuid.uuid4().hex)
    folder.mkdir()
    databases = []
    def target(product):
        name = 'wms_import_test_' + uuid.uuid4().hex
        role = 'wms_icebeauty_business_demo'
        base.sql(f'CREATE DATABASE {name} OWNER {role}')
        databases.append(name)
        env = base.ENV | {'WMS_PG_HOST':'127.0.0.1', 'WMS_PG_PORT':'55432',
                         'WMS_PG_DATABASE':name, 'WMS_PG_USER':role,
                         'WMS_PG_PASSWORD':base.CREDS['app'],
                         'WMS_DEMO_ADMIN_PASSWORD':'123456', 'WMS_LAN_TEST_PASSWORDS':'1'}
        for flag in ('--init-lan-empty', '--init-business'):
            base.run([SERVER, '--product', product, flag], env=env)
        return name, env
    def migrate(path, product, env, success=True):
        before = digest(path)
        result = base.run([SERVER, '--product', product, '--import-legacy', path], env=env,
                          check=False, timeout=120)
        assert (result.returncode == 0) == success, (result.returncode, result.stdout, result.stderr)
        assert before == digest(path), 'Original SQLite file changed'
        return result
    def scalar(name, query):
        return base.sql('SET search_path=wms_business,public;\n' + query, name).splitlines()[-1]
    try:
        for product, exe in [('ICE_BEAUTY','IceBeautyWms.exe'), ('MEI_JING_LING','MeiJingLingWms.exe')]:
            source = folder / (product + '.db')
            fixture(source, exe)
            name, env = target(product)
            other = 'MEI_JING_LING' if product == 'ICE_BEAUTY' else 'ICE_BEAUTY'
            migrate(source, other, env, False)
            unrelated = folder / 'unrelated.db'
            with sqlite3.connect(unrelated) as c:
                c.execute('CREATE TABLE IF NOT EXISTS unrelated(value TEXT)')
            migrate(unrelated, product, env, False)
            # Refuse customized baseline without deleting it.
            base.sql("UPDATE wms_business.material_categories SET name='自定义分类' WHERE id=1", name)
            migrate(source, product, env, False)
            assert scalar(name, 'SELECT name FROM material_categories WHERE id=1') == '自定义分类'
            base.sql(f'DROP DATABASE {name} WITH (FORCE)')
            databases.remove(name)
            name, env = target(product)
            # A login alone is not business data and must not block first-time import.
            with running(env, product) as port:
                assert base.api(port,'/api/v2/login',{'product':product,'username':'admin','password':'123456'})[0] == 200
            # Cause a failure during copy (PostgreSQL rejects embedded NUL in text).
            broken = folder / (product + '-broken.db')
            shutil.copyfile(source, broken)
            with sqlite3.connect(broken) as c:
                c.execute('UPDATE audit_logs SET detail=?', ('invalid\x00text',))
            migrate(broken, product, env, False)
            assert scalar(name, 'SELECT count(*) FROM materials') == '0'
            assert scalar(name, 'SELECT count(*) FROM users') == '1'
            assert scalar(name, "SELECT count(*) FROM pg_trigger WHERE tgname='authenticated_actor' AND tgenabled='O'") == '4'
            assert scalar(name, "SELECT last_value FROM materials_id_seq") == '1'
            # Include a committed row that still exists only in the source WAL.
            writer = sqlite3.connect(source)
            writer.execute('PRAGMA journal_mode=WAL')
            writer.execute('PRAGMA wal_autocheckpoint=0')
            writer.execute("INSERT INTO materials(id,code,name,unit) VALUES(72,'WAL-MAT','WAL物料','个')")
            writer.commit()
            wal_hash = digest(str(source) + '-wal')
            try:
                result = migrate(source, product, env)
                assert wal_hash == digest(str(source) + '-wal')
            finally:
                writer.close()
            assert scalar(name, 'SELECT count(*) FROM materials') == '2'
            assert scalar(name, "SELECT encode(file_data,'hex') FROM attachments WHERE id=151") == BLOB.hex()
            assert scalar(name, "SELECT encode(image_data,'hex') FROM material_images WHERE material_id=71") == IMAGE.hex()
            assert scalar(name, 'SELECT uploaded_by FROM attachments WHERE id=151') == '41'
            assert scalar(name, 'SELECT operator_id FROM inventory_ledger WHERE id=121') == '41'
            assert scalar(name, 'SELECT attachment_id FROM document_forms WHERE id=161') == '151'
            assert scalar(name, 'SELECT production_run_id FROM business_documents WHERE id=91') == '81'
            assert scalar(name, 'SELECT password_hash FROM users WHERE id=41') == password_hash()
            assert scalar(name, "SELECT current_sequence FROM number_rules WHERE document_type='QTRK'") == '37'
            migrate(source, product, env, False)
            with running(env, product) as port:
                status, login = base.api(port,'/api/v2/login',{'product':product,'username':'old-operator','password':PASSWORD})
                assert status == 200, login
                request = {'documentType':'QTCK','documentDate':'2026-11-19',
                           'handlerName':'导入验证','submissionToken':str(uuid.uuid4()),
                           'lines':[{'materialId':'71','quantity':3,'batchNo':'B-OLD','warehouseId':'51','locationId':'61'}]}
                status, result = base.api(port,'/api/v2/data',{'kind':'rpc','method':'InventoryService.postStockDocument',
                    'arguments':{'request':request,'inbound':False},'requestId':str(uuid.uuid4())},login['token'])
                assert status == 200 and 'error' not in result, result
                assert int(result['postedDocument']['documentId']) > 91
            assert float(scalar(name, 'SELECT quantity FROM stock_balances WHERE id=111')) == 97.125
            base.expect(True, f'{product}: preserved records, IDs, BLOBs, passwords, WAL and source; rejection/rollback; new outbound after migration')
        # Simulate an identified v20 database, and a v19 original IceBeauty database.
        for version in (19,20,21):
            source = folder / f'older-{version}.db'
            shutil.copyfile(folder / 'ICE_BEAUTY.db', source)
            with sqlite3.connect(source) as c:
                c.execute('DELETE FROM schema_migrations WHERE version>?', (version,))
                c.execute('DROP TABLE IF EXISTS process_boms')
                if version == 19:
                    c.execute('DROP TABLE system_identity')
                    c.execute('PRAGMA application_id=0')
            name, env = target('ICE_BEAUTY')
            migrate(source, 'ICE_BEAUTY', env)
            assert scalar(name, 'SELECT max(version) FROM schema_migrations') == '22'
            assert scalar(name, 'SELECT count(*) FROM materials') == '2'
            base.expect(True, f'v{version} snapshot upgrades to v22 without modifying the original')
        print('PASS: complete legacy migration integration suite')
    finally:
        for name in databases:
            base.sql(f'DROP DATABASE {name} WITH (FORCE)')


if __name__ == '__main__':
    main()
