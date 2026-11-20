"""Actual inbound UI and form regeneration against disposable PostgreSQL databases."""
import os
import uuid
import lan_integration as base
from legacy_import_integration import running

for product in ('ICE_BEAUTY', 'MEI_JING_LING'):
    name='wms_supplier_test_'+uuid.uuid4().hex
    role='wms_icebeauty_business_demo'
    env=base.ENV | {'WMS_PG_HOST':'127.0.0.1','WMS_PG_PORT':'55432','WMS_PG_DATABASE':name,
        'WMS_PG_USER':role,'WMS_PG_PASSWORD':base.CREDS['app'],'WMS_DEMO_ADMIN_PASSWORD':'123456','WMS_LAN_TEST_PASSWORDS':'1'}
    base.sql(f'CREATE DATABASE {name} OWNER {role}')
    try:
        for flag in ('--init-lan-empty','--init-business'):
            base.run([base.BUILD/'WmsServer.exe','--product',product,flag],env=env)
        # A unique test number prevents collisions with real document archives.
        base.sql("UPDATE wms_business.number_rules SET prefix='SUPTEST"+uuid.uuid4().hex[:10]+"';",name)
        with running(env,product) as port:
            status,login=base.api(port,'/api/v2/login',{'product':product,'username':'admin','password':'123456'})
            assert status==200
            token=login['token']
            def query(sql,args=()):
                status,data=base.api(port,'/api/v2/data',{'requestId':str(uuid.uuid4()),'kind':'query','sql':sql,'args':list(args)},token)
                assert status==200 and 'error' not in data,data
                return data
            def insert(sql,args=()):return int(query(sql,args)['insertId']['integer'])
            wh=insert("INSERT INTO warehouses(code,name) VALUES('SUPPLIER','测试仓')")
            loc=insert("INSERT INTO locations(warehouse_id,code) VALUES(?,'SUPPLIER')",[wh])
            materials=[]
            for i in range(2):
                mid=insert('INSERT INTO materials(code,name,unit,require_batch,default_warehouse_id,default_location_id) VALUES(?,?,?,?,?,?)',
                    ['SUPPLIER-'+str(i),'测试物料'+str(i),'个',1,wh,loc])
                materials.append(mid)
                base.sql(f"INSERT INTO wms_business.batches(material_id,batch_no,supplier,first_in_at) VALUES({mid},'SUPPLIER-{i}','原批次供应商{i}','2026-11-19')",name)
            ui=base.run([base.ROOT/'outputs/inbound-supplier/inbound_supplier_smoke.exe'],env=env | {
                'WMS_SUPPLIER_PRODUCT':product,'WMS_SUPPLIER_URL':f'http://127.0.0.1:{port}',
                'WMS_SUPPLIER_TOKEN':token,'WMS_SUPPLIER_MATERIALS':','.join(map(str,materials))},timeout=240,check=False)
            print(ui.stdout,flush=True)
            assert ui.returncode==0,ui.stderr
            base.expect(True,product+': inbound supplier UI / persistence / regeneration')
    finally:
        base.sql(f'DROP DATABASE IF EXISTS {name} WITH (FORCE)')
