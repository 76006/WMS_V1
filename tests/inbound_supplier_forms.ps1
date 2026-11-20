param([string]$OutputDirectory='outputs/inbound-supplier/forms')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$out=[IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$null=New-Item -ItemType Directory -Path $out -Force
$env:Path="D:\Qt\Tools\mingw1120_64\bin;D:\Qt\6.5.3\mingw_64\bin;$env:Path"
& python (Join-Path $PSScriptRoot 'build_transfer_page_smoke.py') json_form_smoke
if ($LASTEXITCODE -ne 0) { throw 'Could not build JSON form test.' }
& (Join-Path $root 'outputs/json-form-test/json_form_smoke.exe') $out
if ($LASTEXITCODE -ne 0) { throw 'JSON render failed.' }
$cases=@()
foreach($profile in @('ICE_BEAUTY','MEI_JING_LING')) {
    foreach($kind in @('rawInbound','finishedInbound')) {
        foreach($count in @(1,22)) {
            $id="$profile-$kind-$count"
            $lines=@(for($i=0;$i -lt $count;$i++) {
                @{code=('MAT-{0:d3}' -f $i);name='测试物料';specification='SPEC-001';unit='个';quantity=($i+1);
                  batchNo=('BATCH-{0:d3}' -f $i);notes=('备注'+$i);supplier=$(if($i%2 -eq 0){'测试供应商甲（用于版式验证，不是业务数据）'}else{''})}
            })
            $data=@{outputPath=(Join-Path $out "$id.xlsx");lines=$lines}
            $cases+=@{id=$id;data=$data;count=$count;kind=$kind}
        }
    }
}
$excel=$null
$book=$null
try {
    $excel=New-Object -ComObject Excel.Application
    $excel.Visible=$false
    $excel.DisplayAlerts=$false
    $excel.AutomationSecurity=3
    foreach($case in $cases) {
        $book=$excel.Workbooks.Open($case.data.outputPath,0,$true)
        $sheet=$book.Worksheets.Item(1)
        $raw=$case.kind -eq 'rawInbound'
        $lastColumn=if($raw){10}else{9}
        if($raw -and $sheet.Cells.Item(4,9).Text -ne '供应商'){throw 'Missing supplier header'}
        if($sheet.Cells.Item(4,$lastColumn).Text -ne '备注'){throw 'Notes column lost'}
        for($i=0;$i -lt $case.count;$i++) {
            $r=5+$i
            if($sheet.Cells.Item($r,3).Text -ne $case.data.lines[$i].code){throw 'Material shifted'}
            if([double]$sheet.Cells.Item($r,8).Value2 -ne ($i+1)){throw 'Quantity changed'}
            if($sheet.Cells.Item($r,$lastColumn).Text -ne ('备注'+$i)){throw 'Notes changed'}
            if($raw) {
                $expected=if($i%2 -eq 0){$case.data.lines[$i].supplier}else{'测试供应商乙'}
                if($sheet.Cells.Item($r,9).Value2 -ne $expected){throw 'Supplier mismatch'}
            }
        }
        $footer=24+[Math]::Max(0,$case.count-19)
        if($raw -and $case.count -lt 19){$footer-=19-[Math]::Max(2,$case.count)}
        if($sheet.Cells.Item($footer,1).Text -ne '入  库  人：'){throw 'Signature row shifted'}
        if($raw -and $sheet.PageSetup.FitToPagesWide -ne 1){throw 'Supplier may print on separate page'}
        if($raw){$book.ExportAsFixedFormat(0,(Join-Path $out ($case.id+'.pdf')))}
        $book.Close($false);$book=$null
        Write-Output ('PASS '+$case.id)
    }
} finally {
    if($null -ne $book){$book.Close($false)}
    if($null -ne $excel){$excel.Quit();[void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($excel)}
    [GC]::Collect();[GC]::WaitForPendingFinalizers()
}
