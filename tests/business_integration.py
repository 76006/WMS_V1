"""Full business HTTP flows against disposable PostgreSQL databases."""
import json,os,secrets,socket,subprocess,time,uuid,concurrent.futures
import lan_integration as base

BUILD=base.ROOT/'build-lan-business'

def main():
    databases=[];processes=[]
    password=secrets.token_urlsafe(24)
    try:
        for product,exe in [('ICE_BEAUTY','IceBeautyWms.exe'),('MEI_JING_LING','MeiJingLingWms.exe')]:
            name='wms_business_test_'+uuid.uuid4().hex
            base.sql(f'CREATE DATABASE {name} OWNER wms_icebeauty_demo;');databases.append(name)
            env=base.ENV|{'WMS_PG_HOST':'127.0.0.1','WMS_PG_PORT':'55432','WMS_PG_DATABASE':name,
                'WMS_PG_USER':'wms_icebeauty_demo','WMS_PG_PASSWORD':base.CREDS['app'],'WMS_DEMO_ADMIN_PASSWORD':password}
            for flag in ['--init-demo','--init-business']:
                result=base.run([BUILD/'WmsServer.exe','--product',product,flag],env=env,check=False)
                assert result.returncode==0,result.stderr
            with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
            server_args=[str(BUILD/'WmsServer.exe'),'--product',product,'--port',str(port)]
            if os.environ.get('WMS_TEST_GDB'):
                server_args=['D:/Qt/Tools/mingw1120_64/bin/gdb.exe','--batch','-ex','run','-ex','bt','--args']+server_args
            log=open(base.RUNTIME/f'business-test-{product}.log','w',encoding='utf-8')
            proc=subprocess.Popen(server_args,env=env,creationflags=base.FLAGS,stdout=log,stderr=log);processes.append(proc)
            for _ in range(100):
                try:
                    if base.api(port,'/health')[0]==200:break
                except OSError:time.sleep(.1)
            status,login=base.api(port,'/api/v2/login',{'product':product,'username':'admin','password':password})
            assert status==200,login
            token=login['token']
            def call(payload,session=token,request_id=None,allow_error=False):
                request_id=request_id or str(uuid.uuid4())
                status,result=base.api(port,'/api/v2/data',payload|{'requestId':request_id},session)
                if not allow_error:assert status==200 and 'error' not in result,(payload,result)
                return result
            def query(sql,args=(),**kwargs):return call({'kind':'query','sql':sql,'args':list(args)},**kwargs)
            def rpc(method,args,**kwargs):return call({'kind':'rpc','method':method,'arguments':args},**kwargs)
            def scalar(sql,args=()):
                v=query(sql,args)['rows'][0][0]
                return v.get('integer') if isinstance(v,dict) else v
            def inserted(sql,args=()):return int(query(sql,args)['insertId']['integer'])
            wh=inserted("INSERT INTO warehouses(code,name) VALUES(?,?)",['TEST-A','测试仓库A'])
            loc=inserted("INSERT INTO locations(warehouse_id,code,name) VALUES(?,?,?)",[wh,'A01','A01'])
            wh2=inserted("INSERT INTO warehouses(code,name) VALUES(?,?)",['TEST-B','测试仓库B'])
            loc2=inserted("INSERT INTO locations(warehouse_id,code,name) VALUES(?,?,?)",[wh2,'B01','B01'])
            cat=int(scalar("SELECT id FROM material_categories WHERE code='RAW'"))
            material=inserted("INSERT INTO materials(code,name,unit,category_id,require_batch) VALUES(?,?,?,?,?)",['TEST-RAW','测试原料','个',cat,1])
            fgcat=int(scalar("SELECT id FROM material_categories WHERE code='FINISHED'"))
            finished=inserted("INSERT INTO materials(code,name,unit,category_id,require_batch) VALUES(?,?,?,?,?)",['TEST-FG','测试成品','台',fgcat,1])
            date='2026-11-16'
            def line(qty,mid=material,batch='BATCH-1',warehouse=wh,location=loc):return {'materialId':str(mid),'quantity':qty,'batchNo':batch,'warehouseId':str(warehouse),'locationId':str(location)}
            def document(kind,lines):return {'documentType':kind,'documentDate':date,'handlerName':'联调人员','submissionToken':str(uuid.uuid4()),'lines':lines}
            request=document('QTRK',[line(100)])
            rid=str(uuid.uuid4())
            inbound=rpc('InventoryService.postStockDocument',{'request':request,'inbound':True},request_id=rid)
            repeated=rpc('InventoryService.postStockDocument',{'request':request,'inbound':True},request_id=rid)
            base.expect(inbound==repeated and float(scalar('SELECT SUM(quantity) FROM stock_balances'))==100,f'{product}: inbound and lost-response deduplication')
            outbound=rpc('InventoryService.postStockDocument',{'request':document('QTCK',[line(3)]),'inbound':False})['postedDocument']
            base.expect(float(scalar('SELECT SUM(quantity) FROM stock_balances'))==97,'outbound updates shared stock')
            issue=rpc('InventoryService.postProductionIssue',{'run':{'batchNo':'PROD-1','productMaterialId':str(finished),'plannedQuantity':5},'document':document('SCLL',[line(10)])})
            issue_id=issue['postedDocument']['documentId']
            item=scalar('SELECT id FROM business_document_items WHERE document_id=?',[issue_id])
            rpc('InventoryService.postProductionReturn',{'request':{'sourceDocumentId':issue_id,'documentDate':date,'handlerName':'联调人员','submissionToken':str(uuid.uuid4()),'lines':[{'sourceItemId':str(item),'quantity':2,'warehouseId':str(wh),'locationId':str(loc)}]}})
            fg=document('CPRK',[line(5,finished,'FG-1')]);fg['productionRunId']=issue['productionRunId']
            rpc('InventoryService.postFinishedGoodsInbound',{'document':fg})
            base.expect(float(scalar('SELECT SUM(quantity) FROM stock_balances WHERE material_id=?',[material]))==89,'production issue and return are atomic')
            base.expect(float(scalar('SELECT SUM(quantity) FROM stock_balances WHERE material_id=?',[finished]))==5,'finished goods receipt')
            transfer=line(4)|{'documentDate':date,'documentType':'DB','handlerName':'联调人员','targetWarehouseId':str(wh2),'targetLocationId':str(loc2)}
            transferdoc=rpc('InventoryService.postTransfer',{'request':transfer})['postedDocument']
            transferitem=scalar('SELECT id FROM business_document_items WHERE document_id=?',[transferdoc['documentId']])
            rpc('InventoryService.reverseTransfer',{'request':{'sourceItemId':str(transferitem),'documentDate':date,'quantity':2,'handlerName':'联调人员','notes':'测试部分撤销'}})
            balance=float(scalar('SELECT quantity FROM stock_balances WHERE material_id=? AND warehouse_id=?',[material,wh]))
            rpc('InventoryService.postInventoryCount',{'request':{'documentDate':date,'handlerName':'联调人员','submissionToken':str(uuid.uuid4()),'lines':[line(0)|{'systemQuantity':balance,'actualQuantity':90,'differenceReason':'测试盘点'}]}})
            base.expect(float(scalar('SELECT quantity FROM stock_balances WHERE material_id=? AND warehouse_id=?',[material,wh]))==90,'transfer, partial reversal and inventory count')
            edit=rpc('InventoryService.loadPostedDocument',{'documentId':outbound['documentId']})['document']
            edit['lines'][0]['quantity']=2
            rpc('InventoryService.revisePostedDocument',{'document':edit})
            rpc('InventoryService.reversePostedDocument',{'documentId':outbound['documentId'],'handlerName':'联调人员','reason':'测试整单撤销'})
            base.expect(float(scalar('SELECT quantity FROM stock_balances WHERE material_id=? AND warehouse_id=?',[material,wh]))==93,'posted document revision and full reversal recalculate inventory')
            # Inspection, split qualified receipts and shared binary attachments.
            notice=rpc('InspectionService.createNotice',{'draft':{'notificationDate':date,'arrivalDate':date,'entrustedBy':'采购','notificationDepartment':'仓库','supplier':'测试供应商','lines':[{'materialId':str(material),'quantity':5,'batchNumber':'IQC-1'}]}})['noticeId']
            record=rpc('InspectionService.readNotice',{'noticeId':notice})['notice']
            rpc('InspectionService.recordResult',{'noticeId':notice,'result':{'inspectionDate':date,'inspectorName':'检验员','conclusion':'合格','attachmentFileName':'检验.txt','attachmentMimeType':'text/plain','attachmentData':'aW5zcGVjdGlvbiBvaw==','lines':[{'itemId':record['lines'][0]['id'],'qualifiedQuantity':5,'unqualifiedQuantity':0}]}})
            results=rpc('InspectionService.listResults',{'keyword':''})['records']
            rpc('InspectionService.readResult',{'resultId':results[0]['id']})
            checked=document('CGRK',[line(5,batch='IQC-1')]);checked.update({'requiresInspection':True,'inspectionNoticeId':notice})
            checked_doc=rpc('InventoryService.postStockDocument',{'request':checked,'inbound':True})['postedDocument']
            base.expect(rpc('InspectionService.readNotice',{'noticeId':notice})['notice']['inboundQuantity']==5,'inspection result and qualified receipt share server state')
            import base64
            binary=base64.b64encode(b'attachment-test\x00\xff').decode()
            aid=rpc('AttachmentService.uploadDocumentAttachment',{'documentId':checked_doc['documentId'],'fileName':'test.txt','mimeType':'text/plain','data':binary})['attachmentId']
            base.expect(rpc('AttachmentService.loadAttachment',{'attachmentId':aid})['payload']['data']==binary,'binary attachment round trip')
            rpc('AttachmentService.setDeleted',{'attachmentId':aid,'deleted':True})
            # Multiple SQL calls in a desktop import remain a single transaction.
            query('BEGIN');query("INSERT INTO material_projects(code,name) VALUES('ROLLBACK-TEST','事务测试')");query('ROLLBACK')
            base.expect(int(scalar("SELECT COUNT(*) FROM material_projects WHERE code='ROLLBACK-TEST'"))==0,'desktop maintenance transaction rolls back')
            before=float(scalar('SELECT SUM(quantity) FROM stock_balances WHERE material_id=?',[material]))
            failed=rpc('InventoryService.postStockDocument',{'request':document('QTCK',[line(1),line(1000)]),'inbound':False},allow_error=True)
            base.expect('error' in failed and float(scalar('SELECT SUM(quantity) FROM stock_balances WHERE material_id=?',[material]))==before,'failed multiline outbound rolls back all lines')
            rejected=query('UPDATE stock_balances SET quantity=999',allow_error=True)
            base.expect('error' in rejected,'direct inventory table writes are forbidden even for admin')
            secret=query('SELECT * FROM users')['fields']
            base.expect(all(f['name']!='password_hash' for f in secret),'read models never expose password hashes')
            base.expect('error' in query('SELECT * FROM wms_lan.users',allow_error=True),'cross-schema credential access denied')
            base.expect('error' in query('SELECT * FROM "users"',allow_error=True) and 'error' in query('WITH lo_get(x) AS (SELECT 1) SELECT lo_get(1)',allow_error=True),'quoted identifiers and CTE function bypasses denied')
            base.expect(all(f['name']!='password_hash' for f in query('SELECT u.* FROM materials m, users u LIMIT 1')['fields']),'comma-join users reads also hide credentials')
            base.expect('error' in query('UPDATE materials SET name=u.password_hash FROM users u',allow_error=True),'maintenance cannot read credential tables')
            # BOM recursion and template persistence use the same SQL as the desktop.
            bom=inserted('INSERT INTO material_bom_items(product_material_id,component_material_id,quantity,sort_order) VALUES(?,?,?,?)',[finished,material,2,1])
            bom_rows=query('WITH RECURSIVE bom_tree(id,component_material_id,total_quantity) AS (SELECT id,component_material_id,quantity FROM material_bom_items WHERE product_material_id=? AND parent_item_id IS NULL UNION ALL SELECT c.id,c.component_material_id,p.total_quantity*c.quantity FROM material_bom_items c JOIN bom_tree p ON c.parent_item_id=p.id) SELECT component_material_id,SUM(total_quantity) FROM bom_tree GROUP BY component_material_id',[finished])['rows']
            base.expect(len(bom_rows)==1 and bom_rows[0][1]==2,'BOM expansion query works remotely')
            form_sql="INSERT INTO document_forms(document_id,form_kind,payload,status,last_error) VALUES(?,?,?,'PENDING','') ON CONFLICT(document_id,form_kind) DO UPDATE SET payload=excluded.payload,status='PENDING',last_error='',updated_at=strftime('%Y-%m-%d %H:%M:%f','now','localtime')"
            query(form_sql,[checked_doc['documentId'],'rawMaterialInbound','{}']);query(form_sql,[checked_doc['documentId'],'rawMaterialInbound','{}'])
            base.expect(int(scalar('SELECT COUNT(*) FROM document_forms WHERE document_id=?',[checked_doc['documentId']]))==1,'form retry records upsert without duplicates')
            # Real role boundaries, attribution, and immediate account disable.
            for role in ['QUERY','PRODUCTION','WAREHOUSE']:
                rpc('UserService.createUser',{'username':role.lower(),'displayName':role,'roleCode':role,'password':password})
            def login_as(role):return base.api(port,'/api/v2/login',{'product':product,'username':role.lower(),'password':password})[1]['token']
            viewer=login_as('QUERY');worker=login_as('PRODUCTION');warehouse_user=login_as('WAREHOUSE')
            base.expect('error' in rpc('InventoryService.postStockDocument',{'request':document('QTRK',[line(1)]),'inbound':True},session=viewer,allow_error=True),'read-only role cannot post stock')
            work=rpc('InventoryService.postStockDocument',{'request':document('SCLL',[line(1,batch='IQC-1')]),'inbound':False},session=worker)['postedDocument']
            query(form_sql,[work['documentId'],'productionIssue','{}'],session=worker)
            rpc('AttachmentService.uploadDocumentAttachment',{'documentId':work['documentId'],'fileName':'生产表单.txt','mimeType':'text/plain','data':binary},session=worker)
            base.expect('error' in rpc('InventoryService.postStockDocument',{'request':document('QTRK',[line(1)]),'inbound':True},session=worker,allow_error=True),'production role can issue and archive forms but cannot receive raw stock')
            base.expect('error' in rpc('InventoryService.reversePostedDocument',{'documentId':checked_doc['documentId'],'handlerName':'仓管','reason':'越权测试'},session=warehouse_user,allow_error=True),'warehouse user cannot reverse another operator document')
            worker_id=int(scalar("SELECT id FROM users WHERE username='production'"))
            query("INSERT INTO audit_logs(user_id,action,entity_type) VALUES(?,'UI_TEST','test')",[1],session=worker)
            base.expect(int(scalar("SELECT user_id FROM audit_logs WHERE action='UI_TEST' ORDER BY id DESC LIMIT 1"))==worker_id,'audit operator is set by authenticated server session')
            rpc('UserService.updateUser',{'userId':str(worker_id),'displayName':'停用测试','roleCode':'PRODUCTION','active':False})
            base.expect('error' in query('SELECT COUNT(*) FROM materials',session=worker,allow_error=True),'disabled account loses access immediately')
            base.expect('error' in rpc('UserService.changeOwnPassword',{'currentPassword':'','newPassword':'NewPassword123!'},allow_error=True),'server never permits debug empty-password bypass')
            sn_material=inserted('INSERT INTO materials(code,name,unit,category_id,require_batch,require_serial) VALUES(?,?,?,?,1,1)',['TEST-SN','序列号成品','台',fgcat])
            rpc('InventoryService.postStockDocument',{'request':document('QTRK',[line(2,sn_material,'SN-BATCH')|{'serialNumbers':['TEST-SN-001','TEST-SN-002']}]),'inbound':True})
            sales=document('XSCK',[line(1,sn_material,'SN-BATCH')|{'serialNumbers':['TEST-SN-001']}]);sales.update({'customerCompany':'测试客户','destination':'测试目的地','deliveryDate':date})
            sale=rpc('InventoryService.postStockDocument',{'request':sales,'inbound':False})['postedDocument']
            receipt=rpc('ShipmentReceiptService.confirmSigned',{'documentId':sale['documentId'],'receiptDate':date,'fileName':'签收.txt','mimeType':'text/plain','fileData':binary})
            base.expect(scalar('SELECT receipt_status FROM sales_outbound_details WHERE document_id=?',[sale['documentId']])=='SIGNED','sales shipment and signed receipt persist together')
            rpc('InventoryService.reversePostedDocument',{'documentId':sale['documentId'],'handlerName':'测试','reason':'SN撤销测试'})
            base.expect(scalar("SELECT status FROM serial_numbers WHERE serial_no='TEST-SN-001'")=='IN_STOCK','serial outbound reversal restores lifecycle state')
            stale=rpc('InventoryService.postInventoryCount',{'request':{'documentDate':date,'handlerName':'测试','submissionToken':str(uuid.uuid4()),'lines':[line(0)|{'systemQuantity':1,'actualQuantity':2,'differenceReason':'过期盘点'}]}},allow_error=True)
            base.expect('error' in stale,'stale inventory count cannot overwrite a changed balance')
            initial={'documentDate':date,'handlerName':'测试','sourceFile':'自动测试.xlsx','submissionToken':str(uuid.uuid4()),'warehouseId':str(wh),'locationId':str(loc),'lines':[{'materialCode':'IMPORT-TEST','materialName':'导入测试','categoryCode':'RAW','unit':'个','quantity':7,'batchNo':'INIT-1'}]}
            query('BEGIN')
            rpc('InventoryService.importInitialInventoryInCurrentTransaction',{'request':initial})
            query('COMMIT')
            base.expect(float(scalar("SELECT SUM(s.quantity) FROM stock_balances s JOIN materials m ON m.id=s.material_id WHERE m.code='IMPORT-TEST'"))==7,'initial inventory import commits with desktop transaction')
            sessions=[base.api(port,'/api/v2/login',{'product':product,'username':'admin','password':password})[1]['token'] for _ in range(5)]
            def concurrent_out(session):return rpc('InventoryService.postStockDocument',{'request':document('QTCK',[line(30)]),'inbound':False},session=session,allow_error=True)
            with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:results=list(pool.map(concurrent_out,sessions))
            base.expect(sum('error' not in r for r in results)==3 and float(scalar("SELECT quantity FROM stock_balances WHERE material_id=? AND warehouse_id=? AND batch_no='BATCH-1'",[material,wh]))==3,'concurrent clients cannot oversell or lose updates')
            ui_log=base.RUNTIME/'business-ui-errors.log';ui_log.write_text('',encoding='utf-8')
            ui=base.run([BUILD/exe,'--server',f'http://127.0.0.1:{port}','--network-smoke-test'],env=env|{'WMS_TEST_PASSWORD':password,'WMS_NETWORK_VERIFY':'1','WMS_TEST_UI_WRITE':'1','WMS_NETWORK_LOG':str(ui_log)},check=False,timeout=150)
            if ui.returncode:print(f'UI exit: {ui.returncode}\n{ui.stderr}\n{ui_log.read_text(encoding="utf-8")}',flush=True)
            base.expect(ui.returncode==0,f'{product}: all original desktop pages load from PostgreSQL')
        print(f'Completed {base.CHECKS} full business checks.',flush=True)
    finally:
        for p in processes:
            if p.poll() is None:p.terminate();p.wait(timeout=10)
        for name in databases:base.sql(f'DROP DATABASE {name} WITH (FORCE);')

if __name__=='__main__':main()
