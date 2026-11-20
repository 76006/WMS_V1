"""Multi-line transfers against disposable PostgreSQL databases; no real stock is used."""
import json
import os
from pathlib import Path
import uuid

import lan_integration as base
from legacy_import_integration import running

SERVER = base.BUILD / 'WmsServer.exe'


def main():
    for product in ('ICE_BEAUTY', 'MEI_JING_LING'):
        name = 'wms_transfer_test_' + uuid.uuid4().hex
        role = 'wms_icebeauty_business_demo'
        env = base.ENV | {'WMS_PG_HOST':'127.0.0.1','WMS_PG_PORT':'55432',
            'WMS_PG_DATABASE':name,'WMS_PG_USER':role,'WMS_PG_PASSWORD':base.CREDS['app'],
            'WMS_DEMO_ADMIN_PASSWORD':'123456','WMS_LAN_TEST_PASSWORDS':'1'}
        base.sql(f'CREATE DATABASE {name} OWNER {role}')
        try:
            for flag in ('--init-lan-empty','--init-business'):
                base.run([SERVER,'--product',product,flag],env=env)
            with running(env, product) as port:
                status, login = base.api(port,'/api/v2/login',{'product':product,'username':'admin','password':'123456'})
                assert status == 200
                token = login['token']
                def call(payload, *, request_id=None, fail=False):
                    status, data = base.api(port,'/api/v2/data',payload | {'requestId':request_id or str(uuid.uuid4())},token)
                    assert status == 200, data
                    assert ('error' in data) == fail, data
                    return data
                def rpc(method, arguments, **kwargs):
                    return call({'kind':'rpc','method':'InventoryService.'+method,'arguments':arguments},**kwargs)
                def query(sql,args=()):
                    return call({'kind':'query','sql':sql,'args':list(args)})
                def scalar(sql,args=()):
                    value=query(sql,args)['rows'][0][0]
                    return value.get('integer') if isinstance(value,dict) else value
                def insert(sql,args=()):
                    return int(query(sql,args)['insertId']['integer'])
                def snapshot():
                    # Includes row contents so a failed later line cannot quietly change an earlier one.
                    tables=('business_documents','business_document_items','stock_balances','inventory_ledger',
                            'serial_numbers','inventory_ledger_serials','number_rules','audit_logs')
                    return {t:base.sql(f"SELECT coalesce(json_agg(x ORDER BY x::text)::text,'[]') FROM (SELECT t FROM wms_business.{t} t) x",name) for t in tables}
                wh=insert("INSERT INTO warehouses(code,name) VALUES('TRANSFER-A','调拨源仓')")
                loc=insert("INSERT INTO locations(warehouse_id,code) VALUES(?,'A')",[wh])
                wh2=insert("INSERT INTO warehouses(code,name) VALUES('TRANSFER-B','调拨目标仓')")
                loc2=insert("INSERT INTO locations(warehouse_id,code) VALUES(?,'B')",[wh2])
                mids=[]
                for code,serial in [('TRANSFER-A',0),('TRANSFER-B',0),('TRANSFER-SN',1)]:
                    mids.append(insert('INSERT INTO materials(code,name,unit,require_batch,require_serial,default_warehouse_id,default_location_id) VALUES(?,?,?,?,?,?,?)',
                                       [code,code,'个',1,serial,wh,loc]))
                def line(mid,quantity,serials=None,warehouse=wh,location=loc):
                    return {'materialId':str(mid),'quantity':quantity,'batchNo':'TRANSFER-BATCH',
                            'warehouseId':str(warehouse),'locationId':str(location),'serialNumbers':serials or []}
                def document(lines):
                    return {'documentType':'DB','documentDate':'2026-11-19','handlerName':'调拨测试',
                            'targetWarehouseId':str(wh2),'targetLocationId':str(loc2),'lines':lines}
                rpc('postStockDocument',{'inbound':True,'request':{
                    'documentType':'QTRK','documentDate':'2026-11-19','handlerName':'测试入库',
                    'submissionToken':str(uuid.uuid4()),'lines':[
                        line(mids[0],100),line(mids[1],50),line(mids[2],3,['SN-A','SN-B','SN-C'])]}})
                before=snapshot()
                for invalid in [[],[line(mids[0],1),line(mids[1],1000)],
                                [line(mids[0],60),line(mids[0],60)],
                                [line(mids[0],1),line(mids[1],1,warehouse=wh2,location=loc2)],
                                [line(mids[0],1),line(mids[2],2,['SN-A'])],
                                [line(mids[2],1,['SN-A']),line(mids[2],1,['sn-a'])],
                                [line(mids[0],1),line(mids[2],1,['NOT-HERE'])]]:
                    rpc('postTransfer',{'request':document(invalid)},fail=True)
                    assert snapshot() == before
                base.expect(True, product+': later-line failure, aggregate shortage, invalid locations and SN all roll back the whole transfer')
                req=document([line(mids[0],5),line(mids[2],2,['SN-A','SN-B'])])
                rid=str(uuid.uuid4())
                result=rpc('postTransfer',{'request':req},request_id=rid)
                assert rpc('postTransfer',{'request':req},request_id=rid)==result
                doc=result['postedDocument']['documentId']
                assert int(scalar('SELECT count(*) FROM business_document_items WHERE document_id=?',[doc]))==2
                assert int(scalar('SELECT count(*) FROM inventory_ledger WHERE document_id=?',[doc]))==4
                assert base.sql(f'SELECT line_number FROM wms_business.business_document_items WHERE document_id={int(doc)} ORDER BY line_number',name)=='1\n2'
                assert int(scalar("SELECT count(*) FROM serial_numbers WHERE warehouse_id=?",[wh2]))==2
                edit=rpc('loadPostedDocument',{'documentId':doc})['document']
                assert len(edit['lines'])==2
                edit['lines'][0]['quantity']=6
                rpc('revisePostedDocument',{'document':edit})
                edit=rpc('loadPostedDocument',{'documentId':doc})['document']
                assert float(edit['lines'][0]['quantity'])==6
                def reverse(item,quantity,serials=()):
                    rpc('reverseTransfer',{'request':{'sourceItemId':str(item),'documentDate':'2026-11-19',
                        'quantity':quantity,'handlerName':'调拨测试','notes':'按明细撤销','serialNumbers':list(serials)}})
                reverse(edit['lines'][0]['itemId'],6)
                assert scalar('SELECT status FROM business_documents WHERE id=?',[doc])=='PARTIALLY_REVERSED'
                reverse(edit['lines'][1]['itemId'],1,['SN-A'])
                assert scalar('SELECT status FROM business_documents WHERE id=?',[doc])=='PARTIALLY_REVERSED'
                reverse(edit['lines'][1]['itemId'],1,['SN-B'])
                assert scalar('SELECT status FROM business_documents WHERE id=?',[doc])=='REVERSED'
                assert float(scalar('SELECT quantity FROM stock_balances WHERE material_id=? AND location_id=?',[mids[0],loc]))==100
                assert int(scalar('SELECT count(*) FROM serial_numbers WHERE location_id=?',[loc]))==3
                base.expect(True,product+': one document, ordered lines, paired ledgers, deduplication, revision and per-line SN reversal')
                full=rpc('postTransfer',{'request':document([line(mids[0],4),line(mids[1],7)])})['postedDocument']['documentId']
                rpc('reversePostedDocument',{'documentId':full,'handlerName':'测试','reason':'整单撤销'})
                assert scalar('SELECT status FROM business_documents WHERE id=?',[full])=='REVERSED'
                assert float(scalar('SELECT quantity FROM stock_balances WHERE material_id=? AND location_id=?',[mids[1],loc]))==50
                old=line(mids[0],1) | document([])
                old.pop('lines')
                old_doc=rpc('postTransfer',{'request':old})['postedDocument']['documentId']
                assert int(scalar('SELECT count(*) FROM business_document_items WHERE document_id=?',[old_doc]))==1
                base.expect(True,product+': complete reversal and older single-line client remain supported')
                if os.environ.get('WMS_TRANSFER_UI_TEST'):
                    ui_env=env | {'WMS_TRANSFER_TEST_TOKEN':token,'WMS_TRANSFER_TEST_URL':f'http://127.0.0.1:{port}',
                        'WMS_TRANSFER_TEST_PRODUCT':product,'WMS_TRANSFER_TEST_MATERIALS':','.join(map(str,mids[:2])),
                        'WMS_TRANSFER_TEST_TARGET':str(wh2)}
                    run=base.run([Path(os.environ['WMS_TRANSFER_UI_TEST'])],env=ui_env,timeout=120,check=False)
                    assert run.returncode==0,(run.returncode,run.stdout,run.stderr)
                    base.expect(True,product+': actual TransferPage submits two materials and displays both under one document number')
        finally:
            base.sql(f'DROP DATABASE {name} WITH (FORCE)')


if __name__=='__main__':
    main()
