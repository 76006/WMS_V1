-- All work runs on the import connection inside its locked transaction.
CREATE TEMP TABLE import_ids (
    tab text NOT NULL, old_id bigint NOT NULL, new_id bigint NOT NULL,
    PRIMARY KEY(tab,old_id)
) ON COMMIT DROP;
-- @statement
CREATE FUNCTION pg_temp.mapped(t text, original bigint) RETURNS bigint
LANGUAGE sql STABLE STRICT AS $$
    SELECT new_id FROM pg_temp.import_ids WHERE tab=t AND old_id=original
$$;
-- @statement
-- A pre-v25 document had no unit-count key. Keep its original digest when a
-- snapshot still has no count, so a repeat import preserves later host edits.
CREATE FUNCTION pg_temp.source_digest(row_data jsonb) RETURNS text
LANGUAGE sql IMMUTABLE STRICT AS $$
    SELECT encode(sha256(convert_to((CASE WHEN row_data->'issue_unit_count'='null'::jsonb
        THEN row_data-'issue_unit_count' ELSE row_data END)::text,'UTF8')),'hex')
$$;
-- @statement
DO $$
DECLARE
    t text; staged text; pk text; collist text; origin_id text;
    id_table boolean; key_expr text; amount_expr text; id_expr text;
    upper_id bigint; join_on text; new_id_expr text; same_install boolean;
    merging boolean := current_setting('wms.import_mode') = 'merge';
BEGIN
    -- The creation stamp survives moving/renaming the file, WAL checkpoints and later backups.
    -- Pre-identity IceBeauty snapshots acquire a new identity stamp during upgrade, so use the
    -- original first account's creation stamp for their lineage too.
    SELECT profile_id || '/' || coalesce((SELECT created_at FROM incoming_users ORDER BY id LIMIT 1),created_at)
      INTO STRICT origin_id FROM incoming_system_identity WHERE singleton_id=1;
    PERFORM set_config('wms.import_origin',origin_id,true);
    SELECT EXISTS(SELECT 1 FROM system_identity dst JOIN incoming_system_identity src
       ON dst.profile_id=src.profile_id AND dst.created_at=src.created_at
       WHERE (SELECT created_at FROM users ORDER BY id LIMIT 1) =
             (SELECT created_at FROM incoming_users ORDER BY id LIMIT 1)) AND NOT EXISTS (SELECT 1 FROM legacy_merge_rows WHERE origin=origin_id) INTO same_install;
    -- Older hosts have no per-row lineage. A common installation stamp can also
    -- mean two branches of a backup. Reuse raw IDs only for an identical dataset.
    IF same_install AND merging THEN
        FOR t IN SELECT tab FROM import_tables LOOP
            EXECUTE format('SELECT NOT EXISTS ((TABLE %1$I EXCEPT TABLE %2$I)
                UNION ALL (TABLE %2$I EXCEPT TABLE %1$I))',t,'incoming_'||t) INTO same_install;
            EXIT WHEN NOT same_install;
        END LOOP;
    END IF;
    -- Stage and purchase detail tables have immediate foreign keys. Their parents
    -- must be written first in empty, replace and merge mode alike.
    ALTER TABLE import_tables ADD COLUMN needs_parents boolean DEFAULT false;
    UPDATE import_tables i SET needs_parents=true WHERE EXISTS (
        SELECT 1 FROM pg_constraint c WHERE c.conrelid=format('wms_business.%I',i.tab)::regclass
        AND c.contype='f' AND NOT c.condeferrable);
    FOR t IN SELECT tab FROM import_tables ORDER BY needs_parents, CASE tab
        WHEN 'materials' THEN 1 WHEN 'warehouses' THEN 2 WHEN 'locations' THEN 3
        WHEN 'batches' THEN 4 WHEN 'stock_balances' THEN 5 ELSE 6 END,tab LOOP
        staged := 'incoming_'||t;
        SELECT string_agg(format('s.%I',a.attname),',' ORDER BY k.ord)
          INTO pk FROM pg_index i CROSS JOIN LATERAL unnest(i.indkey) WITH ORDINALITY k(num,ord)
          JOIN pg_attribute a ON a.attrelid=i.indrelid AND a.attnum=k.num
          WHERE i.indrelid=format('wms_business.%I',t)::regclass AND i.indisprimary;
        SELECT string_agg(format('%I',column_name),',' ORDER BY ordinal_position),
               bool_or(is_identity='YES') INTO collist,id_table
          FROM information_schema.columns WHERE table_schema='wms_business' AND table_name=t;
        key_expr := CASE WHEN id_table THEN 's.id::text' ELSE 'jsonb_build_array('||pk||')::text' END;
        id_expr := CASE WHEN id_table THEN 's.id' ELSE 'NULL::bigint' END;
        amount_expr := CASE WHEN t='stock_balances' THEN 's.quantity' ELSE 'NULL::numeric' END;
        IF NOT merging THEN
            EXECUTE format('INSERT INTO %I (%s) SELECT %s FROM %I',t,collist,collist,staged);
            EXECUTE format('INSERT INTO legacy_merge_rows SELECT $1,$2,%s,%s,
                pg_temp.source_digest(to_jsonb(s)),%s,false FROM %I s',
                key_expr,id_expr,amount_expr,staged) USING origin_id,t;
            CONTINUE;
        END IF;
        -- Fingerprint the original record before rewriting any references.
        EXECUTE format('ALTER TABLE %I ADD _key text, ADD _digest text, ADD _old_id bigint,
            ADD _prev numeric, ADD _amount numeric, ADD _seen boolean DEFAULT false,
            ADD _shared boolean DEFAULT false, ADD _skip boolean DEFAULT false',staged);
        EXECUTE format('UPDATE %I s SET _key=%s,_old_id=%s,_amount=%s,
            _digest=pg_temp.source_digest(to_jsonb(s)-ARRAY[''_key'',''_digest'',''_old_id'',
               ''_prev'',''_amount'',''_seen'',''_shared'',''_skip''])',
            staged,key_expr,id_expr,amount_expr);
        EXECUTE format('UPDATE %I s SET _prev=p.amount,_seen=true,_shared=p.shared,_skip=p.digest=s._digest
            FROM legacy_merge_rows p WHERE p.origin=$1 AND p.tab=$2 AND p.src_key=s._key',staged)
            USING origin_id,t;
        IF NOT id_table THEN CONTINUE; END IF;
        EXECUTE format('CREATE INDEX ON %I(id)',staged);

        EXECUTE format('INSERT INTO import_ids SELECT $2,s.id,p.dst_id FROM %I s JOIN legacy_merge_rows p
            ON p.origin=$1 AND p.tab=$2 AND p.src_key=s._key',staged) USING origin_id,t;
        join_on := CASE
          WHEN t IN ('materials','material_projects','roles','warehouses','material_categories') THEN 'd.code=s.code'
          WHEN t='users' THEN 'd.username=s.username'
          WHEN t='locations' THEN 'd.warehouse_id=pg_temp.mapped(''warehouses'',s.warehouse_id) AND d.code=s.code'
          WHEN t='batches' THEN 'd.material_id=pg_temp.mapped(''materials'',s.material_id) AND d.batch_no=s.batch_no'
          WHEN t='stock_balances' THEN 'd.material_id=pg_temp.mapped(''materials'',s.material_id)
            AND d.warehouse_id=pg_temp.mapped(''warehouses'',s.warehouse_id)
            AND d.location_id=pg_temp.mapped(''locations'',s.location_id) AND d.batch_no=s.batch_no'
          ELSE NULL END;
        IF join_on IS NOT NULL THEN
            EXECUTE format('INSERT INTO import_ids SELECT $1,s.id,d.id FROM %I s JOIN LATERAL
                (SELECT d.id FROM %I d WHERE %s ORDER BY d.id LIMIT 1) d ON true WHERE NOT s._seen',
                staged,t,join_on) USING t;
            EXECUTE format('UPDATE %I s SET _shared=true FROM import_ids m
               WHERE m.tab=$1 AND m.old_id=s.id AND NOT s._seen',staged) USING t;
        END IF;
        IF same_install THEN
            -- An installation imported with an earlier release can already contain these rows.
            EXECUTE format('INSERT INTO import_ids SELECT $1,s.id,d.id FROM %I s JOIN %I d ON d.id=s.id
                WHERE NOT s._seen AND s._digest=pg_temp.source_digest(to_jsonb(d))
                ON CONFLICT DO NOTHING',staged,t) USING t;
            EXECUTE format('UPDATE %I s SET _shared=true,_prev=_amount FROM import_ids m
                WHERE m.tab=$1 AND m.old_id=s.id AND NOT s._seen',staged) USING t;
        END IF;
        EXECUTE format('SELECT greatest(coalesce((SELECT max(id) FROM %I),0),
            coalesce((SELECT max(id) FROM %I),0),coalesce((SELECT max(dst_id) FROM legacy_merge_rows WHERE tab=$1),0))',
            t,staged) INTO upper_id USING t;
        new_id_expr := 'CASE WHEN d.id IS NULL AND NOT EXISTS(SELECT 1 FROM legacy_merge_rows p WHERE p.tab=$1
            AND p.dst_id=s.id) THEN s.id ELSE $2+row_number() OVER(ORDER BY s.id) END';
        EXECUTE format('INSERT INTO import_ids SELECT $1,s.id,%s FROM %I s LEFT JOIN %I d ON d.id=s.id
            LEFT JOIN import_ids m ON m.tab=$1 AND m.old_id=s.id WHERE m.old_id IS NULL',new_id_expr,staged,t)
            USING t,upper_id;
    END LOOP;
END $$;
-- @statement
-- Give independent, colliding document numbers a distinct visible number. Use the same
-- spelling on later imports, and never take a number used elsewhere in this source.
CREATE FUNCTION pg_temp.rename_collisions(t text, col text, label text, scope_col text DEFAULT NULL) RETURNS void LANGUAGE plpgsql AS $$
DECLARE r record; candidate text; occupied boolean; n integer; staged text := 'incoming_'||t;
    scope_value text := 'NULL::bigint'; scope_match text := ''; scope_filter text := '';
BEGIN
    IF scope_col IS NOT NULL THEN
        scope_value := format('s.%I::bigint',scope_col);
        scope_match := format(' AND used.%1$I=s.%1$I',scope_col);
        scope_filter := format(' AND %I=$2',scope_col);
    END IF;
    FOR r IN EXECUTE format('SELECT s._key,s.id,s._skip,s.%1$I::text original,old.%1$I::text previous
        ,%4$s scope_id FROM %2$I s JOIN %3$I used ON used.%1$I=s.%1$I AND used.id<>s.id %5$s
        LEFT JOIN %3$I old ON old.id=s.id WHERE NOT s._shared ORDER BY s.id',col,staged,t,scope_value,scope_match)
    LOOP
        candidate := r.previous;
        IF candidate IS NULL THEN
            n := 0;
            LOOP
                n := n+1;
                candidate := r.original||'-IMPORT-'||r.id||CASE WHEN n>1 THEN '-'||n ELSE '' END;
                EXECUTE format('SELECT EXISTS(SELECT 1 FROM %I WHERE %I=$1::public.citext %s UNION ALL
                              SELECT 1 FROM %I WHERE %I=$1::public.citext %s)',t,col,scope_filter,staged,col,scope_filter)
                    INTO occupied USING candidate,r.scope_id;
                EXIT WHEN NOT occupied;
            END LOOP;
        END IF;
        IF NOT r._skip THEN
            INSERT INTO import_conflicts VALUES(format('%s %s 重复：主机保留原记录，旧库记录另存为 %s。',label,r.original,candidate));
        END IF;
        EXECUTE format('UPDATE %I SET %I=$1 WHERE _key=$2',staged,col) USING candidate,r._key;
    END LOOP;
END $$;
-- @statement
DO $$
DECLARE t text; fk record; staged text; pk text; collist text; updates text; pair record; r record;
    orig text := current_setting('wms.import_origin'); id_expr text; spec text; applied bigint; rules_changed bigint := 0;
BEGIN
    IF current_setting('wms.import_mode') <> 'merge' THEN RETURN; END IF;
    FOR fk IN SELECT cl.relname AS child,a.attname AS col, parent.relname AS parent
        FROM pg_constraint c JOIN pg_class cl ON cl.oid=c.conrelid JOIN pg_class parent ON parent.oid=c.confrelid
        JOIN pg_attribute a ON a.attrelid=cl.oid AND a.attnum=c.conkey[1]
        JOIN pg_attribute b ON b.attrelid=parent.oid AND b.attnum=c.confkey[1]
        JOIN import_tables i ON i.tab=cl.relname
        WHERE c.contype='f' AND c.connamespace='wms_business'::regnamespace AND b.attname='id'
    LOOP
        EXECUTE format('UPDATE %I s SET %I=m.new_id FROM import_ids m WHERE m.tab=$1 AND m.old_id=s.%I
                       AND m.new_id<>m.old_id','incoming_'||fk.child,fk.col,fk.col) USING fk.parent;
    END LOOP;
    FOR t IN SELECT DISTINCT tab FROM import_ids LOOP
        EXECUTE format('UPDATE %I s SET id=m.new_id FROM import_ids m WHERE m.tab=$1 AND m.old_id=s._old_id
            AND m.new_id<>m.old_id','incoming_'||t) USING t;
    END LOOP;
    -- These links are polymorphic and therefore have no declared foreign key.
    UPDATE incoming_attachments s SET business_id=m.new_id FROM import_ids m
      WHERE m.tab=CASE s.business_type WHEN 'inspection_notice' THEN 'inspection_notices'
        WHEN 'inspection_result' THEN 'inspection_results' ELSE 'business_documents' END AND m.old_id=s.business_id;
    UPDATE incoming_audit_logs s SET entity_id=m.new_id FROM import_ids m WHERE m.tab=CASE s.entity_type
        WHEN 'document' THEN 'business_documents' WHEN 'inventory_count' THEN 'inventory_counts'
        WHEN 'material_bom' THEN 'materials' WHEN 'process_bom' THEN 'materials'
        WHEN 'material_category' THEN 'material_categories' ELSE s.entity_type||'s' END AND m.old_id=s.entity_id;
    UPDATE incoming_production_task_boms s SET payload=(SELECT jsonb_agg(
        CASE WHEN m.new_id IS NULL THEN node ELSE jsonb_set(node,'{material}',to_jsonb(m.new_id::text)) END ORDER BY ord)
        FROM jsonb_array_elements(s.payload::jsonb) WITH ORDINALITY e(node,ord)
        LEFT JOIN import_ids m ON m.tab='materials' AND m.old_id::text=node->>'material')::text
      WHERE NOT s._skip;
    CREATE TEMP TABLE import_original_names ON COMMIT DROP AS
        SELECT 'serial' kind,id,serial_no::text val FROM incoming_serial_numbers UNION ALL
        SELECT 'run',id,batch_no FROM incoming_production_runs UNION ALL
        SELECT 'notice',id,inspection_no FROM incoming_inspection_notices;
    INSERT INTO import_conflicts
      SELECT format('同号单据 %s ｜ 主机 %s %s，明细 [%s]，备注 %s ｜ 旧库 %s %s，明细 [%s]，备注 %s。',
        d.document_no,d.document_type,d.document_date,
        (SELECT string_agg(format('%s × %s（%s）',m.code,i.quantity,i.batch_no),'、' ORDER BY i.line_number)
         FROM business_document_items i JOIN materials m ON m.id=i.material_id WHERE i.document_id=d.id),d.notes,
        s.document_type,s.document_date,
        (SELECT string_agg(format('%s × %s（%s）',m.code,i.quantity,i.batch_no),'、' ORDER BY i.line_number)
         FROM incoming_business_document_items i JOIN incoming_materials m ON m.id=i.material_id WHERE i.document_id=s.id),s.notes)
      FROM incoming_business_documents s JOIN business_documents d ON d.document_no=s.document_no AND d.id<>s.id
      WHERE NOT s._skip AND NOT s._shared;
    PERFORM pg_temp.rename_collisions('warehouses','name','仓库名称');
    PERFORM pg_temp.rename_collisions('material_categories','name','物料分类名称');
    PERFORM pg_temp.rename_collisions('business_documents','document_no','单号');
    PERFORM pg_temp.rename_collisions('business_documents','submission_token','单据提交编号');
    -- A v24 host could rename this run because a *different* product used the
    -- same batch. Keep that assigned name when a later source adds documents/SNs.
    UPDATE incoming_production_runs s SET batch_no=d.batch_no FROM production_runs d
        WHERE d.id=s.id AND s._seen AND (s._skip OR (
            left(d.batch_no::text,length(s.batch_no))::public.citext=s.batch_no AND
            substr(d.batch_no::text,length(s.batch_no)+1) ~ ('^-IMPORT-'||s.id||'(-[0-9]+)?$')));
    PERFORM pg_temp.rename_collisions('production_runs','batch_no','生产批号','product_material_id');
    PERFORM pg_temp.rename_collisions('inspection_notices','inspection_no','送检单号');
    PERFORM pg_temp.rename_collisions('serial_numbers','serial_no','序列号');
    UPDATE incoming_inventory_ledger l SET serial_no=s.serial_no FROM incoming_serial_numbers s
        JOIN import_original_names o ON o.kind='serial' AND o.id=s.id WHERE o.val=l.serial_no;
    -- Prefer the serial's actual document/run after FK remapping. Older serials
    -- without document links can still be renamed when the original name is unique.
    WITH run_names AS (
        SELECT run.id,run.batch_no,o.val::public.citext original,
               count(*) OVER (PARTITION BY o.val::public.citext) siblings
        FROM incoming_production_runs run JOIN import_original_names o ON o.kind='run' AND o.id=run.id
    ), choices AS (
        SELECT s.id,names.batch_no,1 priority,0::bigint ledger_id FROM incoming_serial_numbers s
        JOIN incoming_business_documents d ON d.id=s.last_document_id
        JOIN run_names names ON names.id=d.production_run_id AND names.original=s.production_batch::public.citext
        UNION ALL
        SELECT s.id,names.batch_no,2,l.id FROM incoming_serial_numbers s
        JOIN incoming_inventory_ledger_serials x ON x.serial_id=s.id
        JOIN incoming_inventory_ledger l ON l.id=x.ledger_id
        JOIN incoming_business_documents d ON d.id=l.document_id
        JOIN run_names names ON names.id=d.production_run_id AND names.original=s.production_batch::public.citext
        UNION ALL
        SELECT s.id,names.batch_no,3,0 FROM incoming_serial_numbers s
        JOIN run_names names ON names.siblings=1 AND names.original=s.production_batch::public.citext
    ), resolved AS (
        SELECT DISTINCT ON (id) id,batch_no FROM choices ORDER BY id,priority,ledger_id DESC
    )
    UPDATE incoming_serial_numbers s SET production_batch=resolved.batch_no FROM resolved
        WHERE resolved.id=s.id AND s.production_batch<>resolved.batch_no::text;
    UPDATE incoming_inbound_inspection_details d SET inspection_no=n.inspection_no FROM incoming_inspection_notices n
        WHERE d.inspection_notice_id=n.id;
    INSERT INTO import_conflicts
       SELECT format('物料编码 %s：主机 %s / %s / 单位 %s；旧库 %s / %s / 单位 %s。合并沿用主机档案，库存数量按原数值处理。',
           d.code,d.name,d.specification,d.unit,s.name,s.specification,s.unit)
       FROM incoming_materials s JOIN materials d USING(id) WHERE s._shared AND NOT s._skip
         AND (d.name,d.specification,d.unit) IS DISTINCT FROM (s.name,s.specification,s.unit);
    -- Only one current BOM is selectable per product. Keep the host tree together.
    FOR r IN SELECT DISTINCT b.product_material_id,m.code FROM incoming_material_bom_items b
        JOIN incoming_materials m ON m.id=b.product_material_id
        WHERE NOT b._seen AND EXISTS(SELECT 1 FROM material_bom_items d WHERE d.product_material_id=b.product_material_id)
        AND NOT EXISTS(SELECT 1 FROM incoming_material_bom_items prior WHERE prior.product_material_id=b.product_material_id AND prior._seen)
    LOOP
        UPDATE incoming_material_bom_items SET _shared=true,_skip=true WHERE product_material_id=r.product_material_id;
        INSERT INTO import_conflicts VALUES(format('物料 %s 两边均有成品 BOM：沿用主机的完整 BOM。',r.code));
    END LOOP;
    -- Independent opening balances add. For a later snapshot apply only its change.
    CREATE TEMP TABLE import_balance_offsets ON COMMIT DROP AS
       SELECT s.id,d.quantity-coalesce(s._prev,0) AS delta FROM incoming_stock_balances s
       JOIN stock_balances d USING(id);
    INSERT INTO import_conflicts
       SELECT format('库存 %s / %s / %s / 批次 %s：主机 %s，旧库 %s，合并后 %s。',
             m.code,w.code,l.code,s.batch_no,d.quantity,s.quantity,s.quantity+o.delta)
       FROM incoming_stock_balances s JOIN stock_balances d USING(id) JOIN import_balance_offsets o USING(id)
       JOIN incoming_materials m ON m.id=s.material_id JOIN incoming_warehouses w ON w.id=s.warehouse_id
       JOIN incoming_locations l ON l.id=s.location_id WHERE NOT s._skip;
    UPDATE incoming_inventory_ledger l SET quantity_before=l.quantity_before+o.delta,
        quantity_after=l.quantity_after+o.delta FROM incoming_stock_balances s JOIN import_balance_offsets o USING(id)
        WHERE l.material_id=s.material_id AND l.warehouse_id=s.warehouse_id AND l.location_id=s.location_id
          AND l.batch_no=s.batch_no AND NOT l._skip;
    UPDATE incoming_stock_balances s SET quantity=s.quantity+o.delta FROM import_balance_offsets o WHERE o.id=s.id;
    FOR t IN SELECT tab FROM import_tables ORDER BY needs_parents,tab LOOP
        staged := 'incoming_'||t;
        SELECT string_agg(format('%I',a.attname),',' ORDER BY k.ord),
               string_agg(format('s.%1$I=d.%1$I',a.attname),' AND ' ORDER BY k.ord)
          INTO pk,spec FROM pg_index i CROSS JOIN LATERAL unnest(i.indkey) WITH ORDINALITY k(num,ord)
          JOIN pg_attribute a ON a.attrelid=i.indrelid AND a.attnum=k.num
          WHERE i.indrelid=format('wms_business.%I',t)::regclass AND i.indisprimary;
        SELECT string_agg(format('%I',column_name),',' ORDER BY ordinal_position),
               string_agg(format('%1$I=excluded.%1$I',column_name),',' ORDER BY ordinal_position)
                  FILTER (WHERE column_name<>ALL(string_to_array(pk,','))),
               CASE WHEN bool_or(is_identity='YES') THEN 's.id' ELSE 'NULL::bigint' END
          INTO collist,updates,id_expr FROM information_schema.columns
          WHERE table_schema='wms_business' AND table_name=t;
        IF id_expr='NULL::bigint' THEN
            EXECUTE format('UPDATE %I s SET _shared=true FROM %I d WHERE %s AND NOT s._seen',staged,t,spec);
        END IF;
        IF t='role_permissions' THEN
            INSERT INTO import_conflicts
                SELECT format('角色 %s 的权限 %s：沿用主机当前权限。',role_row.code,p.permission_code)
                FROM incoming_role_permissions p JOIN incoming_roles role_row ON role_row.id=p.role_id
                WHERE role_row._shared AND NOT p._seen AND NOT EXISTS(SELECT 1 FROM role_permissions d
                    WHERE d.role_id=p.role_id AND d.permission_code=p.permission_code);
            UPDATE incoming_role_permissions p SET _shared=true FROM incoming_roles role_row
                WHERE role_row.id=p.role_id AND role_row._shared;
        END IF;
        IF t='number_rules' THEN
            -- The next document must use the highest counter on the most recent sequence date.
            UPDATE number_rules d SET sequence_date=s.sequence_date,current_sequence=s.current_sequence
              FROM incoming_number_rules s WHERE d.document_type=s.document_type AND d.prefix=s.prefix
              AND (s.sequence_date,s.current_sequence)>(d.sequence_date,d.current_sequence);
            GET DIAGNOSTICS rules_changed = ROW_COUNT;
        END IF;
        IF t<>'stock_balances' THEN
            -- Matching master records and one-per-product settings belong to the host.
            -- Compare meaningful fields, excluding timestamps and credential hashes from output.
            EXECUTE format('INSERT INTO import_conflicts SELECT format(''%%s %%s：双方记录不同，沿用主机记录。'',
                $1,coalesce(to_jsonb(s)->>''code'',to_jsonb(s)->>''username'',s._key))
                FROM %I s JOIN %I d ON %s WHERE s._shared AND NOT s._skip AND
                (to_jsonb(s)-ARRAY[''_key'',''_digest'',''_old_id'',''_prev'',''_amount'',''_seen'',''_shared'',
                ''_skip'',''created_at'',''updated_at'',''last_login_at'']) IS DISTINCT FROM
                (to_jsonb(d)-ARRAY[''created_at'',''updated_at'',''last_login_at''])',staged,t,spec) USING t;
            EXECUTE format('UPDATE %I SET _skip=true WHERE _shared',staged);
        END IF;
        EXECUTE format('INSERT INTO import_conflicts SELECT format(''%%s %%s：先前导入的记录发生变化，按本次旧库更新。'',
              $1,s._key) FROM %I s WHERE _seen AND NOT _skip',staged) USING t;
        EXECUTE format('INSERT INTO %I (%s) SELECT %s FROM %I WHERE NOT _skip
            ON CONFLICT (%s) DO %s',t,collist,collist,staged,pk,
            CASE WHEN updates IS NULL THEN 'NOTHING' ELSE 'UPDATE SET '||updates END);
        GET DIAGNOSTICS applied = ROW_COUNT;
        UPDATE import_tables SET written=applied+CASE WHEN t='number_rules' THEN rules_changed ELSE 0 END WHERE tab=t;
        EXECUTE format('INSERT INTO legacy_merge_rows SELECT $1,$2,s._key,%s,s._digest,s._amount,s._shared
            FROM %I s ON CONFLICT(origin,tab,src_key) DO UPDATE SET digest=excluded.digest,
            dst_id=excluded.dst_id,amount=excluded.amount,shared=excluded.shared',id_expr,staged) USING orig,t;
    END LOOP;
END $$;
