"""Full business HTTP flows against disposable PostgreSQL databases."""
import hashlib,json,os,secrets,socket,subprocess,time,uuid,concurrent.futures
import lan_integration as base

BUILD=base.BUILD
SERVER=base.BUILD/'WmsServer.exe'

def main():
    databases=[];processes=[]
    password=secrets.token_urlsafe(24)
    database_owner = base.sql("SELECT rolname FROM pg_roles WHERE rolname IN "
                              "('wms_icebeauty_business_demo','wms_icebeauty_demo') ORDER BY rolname LIMIT 1")
    if not database_owner:
        raise RuntimeError('Start the local IceBeauty LAN host before running business checks.')
    try:
        for product,exe in [('ICE_BEAUTY','IceBeautyWms.exe'),('MEI_JING_LING','MeiJingLingWms.exe')]:
            name='wms_business_test_'+uuid.uuid4().hex
            base.sql(f'CREATE DATABASE {name} OWNER {database_owner};');databases.append(name)
            env=base.ENV|{'WMS_PG_HOST':'127.0.0.1','WMS_PG_PORT':base.PG_PORT,'WMS_PG_DATABASE':name,
                'WMS_PG_USER':database_owner,'WMS_PG_PASSWORD':base.CREDS['app'],'WMS_DEMO_ADMIN_PASSWORD':password}
            for flag in ['--init-demo','--init-business']:
                result=base.run([SERVER,'--product',product,flag],env=env,check=False)
                assert result.returncode==0,result.stderr
            with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
            server_args=[str(SERVER),'--product',product,'--port',str(port)]
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
            public_status, public_stock = base.api(port, '/stock?q=TEST-RAW')
            base.expect(public_status == 200 and len(public_stock['items']) == 1
                        and float(public_stock['items'][0]['quantity']) == 97,
                        'anonymous inventory reads the current business balance')
            base.expect(base.api(port, '/stock?q=%25')[1]['items'] == [],
                        'inventory search treats percent as a literal')
            base.expect(base.api(port, '/api/v2/data', {'kind': 'rpc'})[0] == 401,
                        'public inventory access does not authorize business operations')
            serial_material = inserted('INSERT INTO materials(code,name,unit,category_id,require_batch,require_serial) '
                                       'VALUES(?,?,?,?,1,1)', ['FLEX-SN', 'Serialized part', 'pcs', cat])
            other_product = inserted("INSERT INTO materials(code,name,unit,category_id) VALUES(?,?,?,?)",
                                     ['FLEX-FG', 'Product B', 'pcs', fgcat])
            def serial_line(serials):
                return line(len(serials), serial_material, 'SERIAL-LOT') | {'serialNumbers':serials}
            def serial_issue(product_id, serials):
                slip = rpc('InventoryService.postProductionIssue', {
                    'run':{'batchNo':'SHARED-SN', 'productMaterialId':str(product_id), 'plannedQuantity':1},
                    'document':document('SCLL', [serial_line(serials) | {'unitUsage':7}])})['postedDocument']['documentId']
                return slip, str(scalar('SELECT id FROM business_document_items WHERE document_id=?', [slip]))
            def return_serial(source, serial, **kwargs):
                return rpc('InventoryService.postProductionReturn', {'request':{
                    'sourceDocumentId':source[0], 'documentDate':date, 'submissionToken':str(uuid.uuid4()),
                    'lines':[{'sourceItemId':source[1], 'quantity':1, 'warehouseId':str(wh),
                              'locationId':str(loc), 'serialNumbers':[serial]}]}}, **kwargs)
            rpc('InventoryService.postStockDocument', {'request':document('QTRK',[serial_line(['FLEX-1','FLEX-2'])]), 'inbound':True})
            original = serial_issue(finished, ['FLEX-1','FLEX-2'])
            old_return = return_serial(original, 'FLEX-1')['postedDocument']['documentId']
            old_return_item = str(scalar('SELECT id FROM business_document_items WHERE document_id=?', [old_return]))
            edited_return = rpc('InventoryService.loadPostedDocument', {'documentId':old_return})['document']
            edited_return['notes'] = 'Revised return notes'
            rpc('InventoryService.revisePostedDocument', {'document':edited_return})
            base.expect(float(scalar('SELECT unit_usage FROM issue_line_usage WHERE document_item_id=?', [old_return_item])) == 7,
                        'editing return notes preserves the source issue line unit usage')
            current = serial_issue(other_product, ['FLEX-1'])
            base.expect('error' in return_serial(original, 'FLEX-1', allow_error=True),
                        'a reissued serial cannot be returned against its old issue')
            base.expect(rpc('InventoryService.reversibleSerialNumbers', {'sourceItemId':original[1]})['result'] == ['FLEX-2'],
                        'only currently issued serials can be reversed')
            returned = return_serial(current, 'FLEX-1')['postedDocument']['documentId']
            returned_item = str(scalar('SELECT id FROM business_document_items WHERE document_id=?', [returned]))
            def undo_return(item, **kwargs):
                return rpc('InventoryService.reverseItem', {'request':{'sourceItemId':item, 'documentDate':date, 'quantity':1,
                                                                        'serialNumbers':['FLEX-1'],'notes':'Undo return'}}, **kwargs)
            old_choices = rpc('InventoryService.reversibleSerialNumbers', {'sourceItemId':old_return_item})['result']
            old_undo = undo_return(old_return_item, allow_error=True)
            base.expect(old_choices == [] and 'error' in old_undo
                        and float(scalar('SELECT returned_quantity FROM business_document_items WHERE id=?', [original[1]])) == 1,
                        'an earlier return cannot offer or reverse a serial returned on another issue')
            # Imported data may have ledger links without a last-document pointer.
            base.sql("UPDATE wms_business.serial_numbers SET last_document_id=NULL WHERE serial_no='FLEX-1'", name)
            base.expect(rpc('InventoryService.reversibleSerialNumbers', {'sourceItemId':old_return_item})['result'] == []
                        and rpc('InventoryService.reversibleSerialNumbers', {'sourceItemId':returned_item})['result'] == ['FLEX-1'],
                        'legacy serials resolve to their latest return through the ledger')
            undo_return(returned_item)
            base.expect(rpc('InventoryService.reversibleSerialNumbers', {'sourceItemId':current[1]})['result'] == ['FLEX-1'],
                        'undoing a return restores that serial to the correct issue')
            return_serial(current, 'FLEX-1')
            editable_serials = document('QTRK', [serial_line(['FLEX-3','FLEX-4'])])
            rpc('InventoryService.postStockDocument', {'request':editable_serials,'inbound':True})
            revised_issue, _ = serial_issue(finished, ['FLEX-3'])
            revise_sn = rpc('InventoryService.loadPostedDocument', {'documentId':revised_issue})['document']
            revise_sn['lines'][0]['serialNumbers'] = ['FLEX-4']
            rpc('InventoryService.revisePostedDocument', {'document':revise_sn})
            base.expect(scalar("SELECT production_batch FROM serial_numbers WHERE serial_no='FLEX-4'") == 'SHARED-SN'
                        and not scalar("SELECT production_batch FROM serial_numbers WHERE serial_no='FLEX-3'"),
                        'replacing a serial on an issue moves its production batch association')
            # Two groups per machine consume one component in total on this saved plan of ten.
            component = inserted("INSERT INTO materials(code,name,unit,category_id,require_batch) VALUES(?,?,?,?,?)",
                                 ['FLEX-RAW', 'Part', 'pcs', cat, 1])
            base.sql(f"INSERT INTO wms_business.production_runs(batch_no,product_material_id,product_name,"
                     f"planned_quantity,created_by) VALUES('SHARED-BATCH',{finished},'Product A',10,1)", name)
            task = int(scalar("SELECT id FROM production_runs WHERE batch_no='SHARED-BATCH'"))
            nodes = [
                dict(key='0', parent='', material=str(finished), code='TEST-FG', name='Product A',
                     model='', category='FINISHED', quantity=1, perMachine=1, processBom=True),
                dict(kind='processGroup', key='process:1', parent='0', name='Assembly', code='ASM',
                     model='', perMachine=2, issueError=''),
                dict(kind='processMaterial', parent='process:1', material=str(component),
                     code='FLEX-RAW', name='Part', quantity=1)]
            snapshot = json.dumps(nodes, sort_keys=True, separators=(',', ':'))
            base.sql(f"INSERT INTO wms_business.production_task_boms(production_run_id,payload) "
                     f"VALUES({task},'{snapshot}')", name)
            stage = {'productionStageKey':'process:1', 'productionBomFingerprint':hashlib.sha256(snapshot.encode()).hexdigest()}
            def issue_request(count, quantities, product_id=finished):
                slip = document('SCLL', [line(q, component, 'FLEX-LOT') for q in quantities])
                if product_id == finished:
                    slip.update(stage)
                return {'run':{'batchNo':'SHARED-BATCH', 'productMaterialId':str(product_id), 'plannedQuantity':count},
                        'document':slip}
            def component_balance():
                return float(scalar('SELECT SUM(quantity) FROM stock_balances WHERE material_id=?', [component]))
            issue_method = 'InventoryService.postProductionIssue'
            rpc('InventoryService.postStockDocument', {'request':document('QTRK',[line(20, component, 'FLEX-LOT')]), 'inbound':True})
            first = rpc(issue_method, issue_request(10, [8]))['postedDocument']['documentId']
            second_request = issue_request(3, [2, 1])
            replay_id = str(uuid.uuid4())
            second = rpc(issue_method, second_request, request_id=replay_id)
            second_id = second['postedDocument']['documentId']
            base.expect(second == rpc(issue_method, second_request, request_id=replay_id)
                        and 'error' in rpc(issue_method, second_request, allow_error=True)
                        and component_balance() == 9, '8 + 3 issues against plan 10 debit stock once')
            base.expect(float(scalar('SELECT planned_quantity FROM production_runs WHERE id=?',[task])) == 10
                        and float(scalar('SELECT issue_unit_count FROM business_documents WHERE id=?',[first])) == 10
                        and float(scalar('SELECT issue_unit_count FROM business_documents WHERE id=?',[second_id])) == 3
                        and int(scalar('SELECT COUNT(*) FROM business_document_items WHERE document_id=?',[second_id])) == 2,
                        'each issue retains its unit count and split stock rows')
            oversized = issue_request(2, [4, 6])
            base.expect('error' in rpc(issue_method, oversized, allow_error=True) and component_balance() == 9
                        and int(scalar('SELECT COUNT(*) FROM business_documents WHERE submission_token=?',
                                       [oversized['document']['submissionToken']])) == 0,
                        'insufficient stock on the last split row rolls back all rows')
            foreign = issue_request(1, [1])
            foreign['document']['lines'] = [line(1)]
            stale = issue_request(1, [1])
            stale['document']['productionBomFingerprint'] = 'expired'
            base.expect('error' in rpc(issue_method, foreign, allow_error=True)
                        and 'error' in rpc(issue_method, stale, allow_error=True), 'stage membership and snapshot are enforced')
            other = rpc(issue_method, issue_request(1, [1], other_product))
            base.expect(int(other['productionRunId']) != task and component_balance() == 8,
                        'two finished products sharing a batch have distinct tasks')
            revision = rpc('InventoryService.loadPostedDocument', {'documentId':second_id})['document']
            revision['lines'][0]['quantity'] = 3
            rpc('InventoryService.revisePostedDocument', {'document':revision})
            base.expect(component_balance() == 7 and float(scalar(
                'SELECT issue_unit_count FROM business_documents WHERE id=?', [second_id])) == 3,
                'revising a stage issue above plan preserves its unit count')
            rpc('InventoryService.reversePostedDocument', {'documentId':second_id, 'handlerName':'测试', 'reason':'撤销分行领料'})
            base.expect(component_balance() == 11, 'reversal restores the actual split row quantities')
            manual = document('QTLL', [line(1, component, 'FLEX-LOT'), line(2, component, 'FLEX-LOT')])
            rpc('InventoryService.postStockDocument', {'request':manual, 'inbound':False})
            duplicate_receipt = document('QTRK', manual['lines'])
            base.expect(component_balance() == 8 and 'error' in rpc('InventoryService.postStockDocument',
                        {'request':duplicate_receipt, 'inbound':True}, allow_error=True) and component_balance() == 8,
                        'manual issues support split stock rows and duplicate receipt rows still roll back')
            # Pre-migration slips counted stage groups. Their saved BOM retained two groups per machine.
            saved_slip = rpc('InventoryService.loadPostedDocument', {'documentId':first})['document']
            base.expect(saved_slip['lines'][0]['unitUsage'] == 0.8, 'a saved slip count already identifies its ten units')
            base.sql(f'UPDATE wms_business.business_documents SET issue_unit_count=NULL WHERE id={int(first)}', name)
            historical = rpc('InventoryService.loadPostedDocument', {'documentId':first})['document']
            original_line = historical['lines'][0]
            old_stage_return = rpc('InventoryService.postProductionReturn', {'request':{
                'sourceDocumentId':first, 'documentDate':date, 'submissionToken':str(uuid.uuid4()),
                'lines':[{'sourceItemId':original_line['itemId'], 'warehouseId':str(wh), 'locationId':str(loc), 'quantity':1}]}})
            old_edit = rpc('InventoryService.loadPostedDocument', {'documentId':old_stage_return['postedDocument']['documentId']})['document']
            old_edit['notes'] = 'Historical stage return'
            rpc('InventoryService.revisePostedDocument', {'document':old_edit})
            base.expect(original_line['unitUsage'] == 0.4 and float(scalar(
                'SELECT unit_usage FROM issue_line_usage WHERE document_item_id=?', [old_edit['lines'][0]['itemId']])) == 0.4,
                'historical issue and return revisions use eight parts divided by twenty stage groups')
            issue=rpc('InventoryService.postProductionIssue',{'run':{'batchNo':'PROD-1','productMaterialId':str(finished),'plannedQuantity':5},'document':document('SCLL',[line(10)])})
            issue_id=issue['postedDocument']['documentId']
            item=scalar('SELECT id FROM business_document_items WHERE document_id=?',[issue_id])
            returned = rpc('InventoryService.postProductionReturn',{'request':{'sourceDocumentId':issue_id,'documentDate':date,'handlerName':'联调人员','submissionToken':str(uuid.uuid4()),'lines':[{'sourceItemId':str(item),'quantity':2,'warehouseId':str(wh),'locationId':str(loc)}]}})
            edit_return = rpc('InventoryService.loadPostedDocument', {'documentId':returned['postedDocument']['documentId']})['document']
            base.expect(edit_return['lines'][0]['unitUsage'] == 2,
                        'without an explicit unit usage the return uses the original issue quantity and slip count')
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
            # Shared and nested components must not multiply stock balance rows.
            bom = {'productCode':'TEST-FG','bomType':'process','groups':[
                {'name':'Assembly A','items':[{'code':'TEST-SUB','quantity':1,'items':[
                    {'code':'test-raw','quantity':1}]}]},
                {'name':'Assembly B','items':[{'code':'TEST-RAW','quantity':2}]}]}
            base.sql(f"INSERT INTO wms_business.process_boms(product_material_id,payload) "
                     f"VALUES ({finished},'{json.dumps(bom)}')",name)
            status, filters = base.api(port,'/stock/filters')
            groups = {g['label']:g['id'] for g in filters['modules'] if g['product']=='TEST-FG'}
            base.expect(status==200 and len(groups)==2 and any(
                l['id']==str(loc) and l['warehouse']==str(wh) for l in filters['locations']),
                'filter options identify process groups and location owners')
            criteria = f'/stock?q=TEST-RAW&category={cat}&warehouse={wh}&location={loc}'
            unfiltered_rows = base.api(port,criteria)[1]['items']
            base.expect(bool(unfiltered_rows) and all(row['modules']=='Assembly A、Assembly B'
                for row in unfiltered_rows), 'balances show all process group memberships')
            for group_id in groups.values():
                base.expect(base.api(port,criteria+'&module='+group_id)[1]['items']==unfiltered_rows,
                            'nested and shared module membership preserves each balance')
            for search in (criteria+'&module=none', f'/stock?warehouse={wh2}&location={loc}',
                           f'/stock?q=TEST-RAW&category={fgcat}'):
                base.expect(base.api(port,search)[1]['items']==[], 'combined filters exclude mismatches')
        print(f'Completed {base.CHECKS} full business checks.',flush=True)
    finally:
        for p in processes:
            if p.poll() is None:p.terminate();p.wait(timeout=10)
        for name in databases:base.sql(f'DROP DATABASE {name} WITH (FORCE);')

if __name__=='__main__':main()
