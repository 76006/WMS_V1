param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$DataFile
)

$ErrorActionPreference = 'Stop'
$excel = $null
$workbook = $null
$officeEngine = ''
$automationProcessId = 0
$processIdPath = ''

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class IceBeautySpreadsheetNativeMethods {
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
}
"@

function Copy-TemplateToOutput([string]$templatePath, [string]$outputPath) {
    if ([string]::IsNullOrWhiteSpace($templatePath)) {
        throw '没有收到模板文件路径，无法生成表单。'
    }
    if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) {
        throw ('找不到模板文件：' + $templatePath)
    }
    if ([string]::IsNullOrWhiteSpace($outputPath)) {
        throw '没有收到表单输出路径，无法生成表单。'
    }
    $directory = [IO.Path]::GetDirectoryName($outputPath)
    if (-not [string]::IsNullOrWhiteSpace($directory)) {
        [IO.Directory]::CreateDirectory($directory) | Out-Null
    }
    Copy-Item -LiteralPath $templatePath -Destination $outputPath -Force
}

function Release-ComReference($value) {
    if ($null -ne $value -and [Runtime.InteropServices.Marshal]::IsComObject($value)) {
        try { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($value) } catch {}
    }
}

function Get-SpreadsheetProcessId($application) {
    if ($null -eq $application) { return 0 }
    try {
        $handle = [IntPtr]$application.Hwnd
        if ($handle -eq [IntPtr]::Zero) { return 0 }
        [uint32]$identifier = 0
        [void][IceBeautySpreadsheetNativeMethods]::GetWindowThreadProcessId($handle, [ref]$identifier)
        return [int]$identifier
    }
    catch { return 0 }
}

function Write-AutomationProcessId([int]$identifier) {
    if ($identifier -le 0 -or [string]::IsNullOrWhiteSpace($processIdPath)) { return }
    try { [IO.File]::WriteAllText($processIdPath, [string]$identifier) } catch {}
}

function Clear-AutomationProcessId {
    if ([string]::IsNullOrWhiteSpace($processIdPath)) { return }
    try { Remove-Item -LiteralPath $processIdPath -Force -ErrorAction SilentlyContinue } catch {}
}

function Stop-OwnedSpreadsheetProcess([int]$identifier) {
    if ($identifier -le 0) { return }
    try {
        $process = Get-Process -Id $identifier -ErrorAction SilentlyContinue
        if ($null -ne $process) {
            try { $process.WaitForExit(1500) } catch {}
            if (-not $process.HasExited) { Stop-Process -Id $identifier -Force -ErrorAction SilentlyContinue }
        }
    }
    catch {}
}

function Close-Spreadsheet([bool]$save, $sheet, $book, $application,
                           [bool]$quitApplication, [int]$processId) {
    Release-ComReference $sheet
    if ($null -ne $book) {
        try { $book.Close($save) } catch {}
    }
    if ($null -ne $application -and $quitApplication) {
        try { $application.Quit() } catch {}
    }
    Release-ComReference $book
    Release-ComReference $application
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    Stop-OwnedSpreadsheetProcess $processId
    Clear-AutomationProcessId
}

function Find-Worksheet($book, [string]$expectedName) {
    $availableNames = @()
    $matchedSheet = $null
    $count = [int]$book.Worksheets.Count
    for ($index = 1; $index -le $count; $index++) {
        $candidateSheet = $book.Worksheets.Item([int]$index)
        $candidateName = [string]$candidateSheet.Name
        $availableNames += $candidateName
        if ([string]::Equals($candidateName.Trim(), $expectedName.Trim(),
                             [StringComparison]::OrdinalIgnoreCase)) {
            $matchedSheet = $candidateSheet
            break
        }
        # 不在查找过程中主动释放工作表。Office 可能为同一工作表复用 RCW，
        # 提前 FinalRelease 会让稍后取得的工作表引用也立即失效。
        $candidateSheet = $null
    }
    # 单工作表模板允许名称被 Excel/WPS 自动修正，直接采用唯一工作表。
    if ($null -eq $matchedSheet -and $count -eq 1) {
        $matchedSheet = $book.Worksheets.Item([int]1)
    }
    if ($null -eq $matchedSheet) {
        throw ('模板中找不到工作表：' + $expectedName + '；实际工作表：' +
               ($availableNames -join '、'))
    }
    return $matchedSheet
}

function TextValue($value) {
    if ($null -eq $value) { return '' }
    return [string]$value
}

function NumberValue($value) {
    if ($null -eq $value -or [string]::IsNullOrWhiteSpace([string]$value)) { return 0 }
    return [double]$value
}

function Set-Cell($sheet, [int]$row, [int]$column, $value) {
    if ($value -is [byte] -or $value -is [int16] -or $value -is [int32] -or
        $value -is [int64] -or $value -is [uint16] -or $value -is [uint32] -or
        $value -is [uint64] -or $value -is [single] -or $value -is [double] -or
        $value -is [decimal]) {
        $value = [Convert]::ToString($value, [Globalization.CultureInfo]::InvariantCulture)
    }
    $sheet.Cells.Item($row, $column).Value2 = $value
}

function Get-Field($data, [string]$name) {
    $property = $data.fields.PSObject.Properties[$name]
    if ($null -eq $property) { return '' }
    return TextValue $property.Value
}

function Clear-TableRows($sheet, [int]$firstRow, [int]$rowCount, [int]$columnCount) {
    $sheet.Range($sheet.Cells.Item($firstRow, 1),
                 $sheet.Cells.Item($firstRow + $rowCount - 1, $columnCount)).ClearContents()
}

function Expand-Table($sheet, [int]$firstRow, [int]$baseRows, [int]$requiredRows,
                      [int]$columnCount, [bool]$mergeIssueCells, [bool]$mergeInspectionCells) {
    if ($requiredRows -le $baseRows) { return 0 }
    $extra = $requiredRows - $baseRows
    $insertAt = $firstRow + $baseRows
    $lastInsertRow = $insertAt + $extra - 1
    $sheet.Range("$insertAt`:$lastInsertRow").EntireRow.Insert() | Out-Null
    $source = $sheet.Range($sheet.Cells.Item($insertAt - 1, 1),
                           $sheet.Cells.Item($insertAt - 1, $columnCount))
    $target = $sheet.Range($sheet.Cells.Item($insertAt, 1),
                           $sheet.Cells.Item($insertAt + $extra - 1, $columnCount))
    $source.Copy()
    $target.PasteSpecial(-4122) | Out-Null
    $excel.CutCopyMode = 0
    for ($row = $insertAt; $row -lt ($insertAt + $extra); $row++) {
        $sheet.Rows.Item($row).RowHeight = $sheet.Rows.Item($insertAt - 1).RowHeight
        if ($mergeIssueCells) {
            $sheet.Range($sheet.Cells.Item($row, 5), $sheet.Cells.Item($row, 9)).Merge()
        }
        if ($mergeInspectionCells) {
            $sheet.Range($sheet.Cells.Item($row, 5), $sheet.Cells.Item($row, 6)).Merge()
            $sheet.Range($sheet.Cells.Item($row, 7), $sheet.Cells.Item($row, 8)).Merge()
        }
    }
    return $extra
}

function Fill-Inspection($sheet, $data) {
    $required = @($data.lines).Count
    $extra = Expand-Table $sheet 12 15 $required 8 $false $true
    Clear-TableRows $sheet 12 ($required + [Math]::Max(0, 15 - $required)) 8
    Set-Cell $sheet 2 6 ("编号：" + (TextValue $data.documentNumber))
    Set-Cell $sheet 3 2 (Get-Field $data 'entrustedBy')
    Set-Cell $sheet 4 2 (Get-Field $data 'notificationDepartment')
    Set-Cell $sheet 5 2 (Get-Field $data 'arrivalDate')
    Set-Cell $sheet 6 2 (TextValue $data.documentDate)
    $urgency = (Get-Field $data 'urgency').ToUpperInvariant()
    Set-Cell $sheet 7 2 ($(if ($urgency -eq 'EXPEDITED') { '☑' } else { '□' }) + ' 加急（常规1-3天）')
    Set-Cell $sheet 7 4 ($(if ($urgency -eq 'URGENT') { '☑' } else { '□' }) + ' 急（常规7天内）')
    Set-Cell $sheet 7 7 ($(if ($urgency -eq 'NORMAL' -or [string]::IsNullOrWhiteSpace($urgency)) { '☑' } else { '□' }) + ' 正常（常规7-15天）')
    Set-Cell $sheet 8 2 ((@($data.lines) | Measure-Object -Property quantity -Sum).Sum)
    # 检验日期、检验结果、结论和签字属于检验完成后的手写/回填区域，通知单创建时必须留空。
    Set-Cell $sheet 9 2 ''
    for ($index = 0; $index -lt $required; $index++) {
        $line = @($data.lines)[$index]
        $row = 12 + $index
        Set-Cell $sheet $row 1 (TextValue $line.code)
        Set-Cell $sheet $row 2 (TextValue $line.name)
        Set-Cell $sheet $row 3 (NumberValue $line.quantity)
        Set-Cell $sheet $row 4 (TextValue $line.orderNumber)
        Set-Cell $sheet $row 5 (TextValue $line.batchNo)
        Set-Cell $sheet $row 7 (TextValue $line.supplier)
    }
    $sheet.PageSetup.PrintArea = '$A$1:$H$' + (35 + $extra)
}

function Fill-Inbound($sheet, $data) {
    $required = @($data.lines).Count
    $extra = Expand-Table $sheet 5 19 $required 9 $false $false
    Clear-TableRows $sheet 5 ($required + [Math]::Max(0, 19 - $required)) 9
    $date = [datetime]::ParseExact((TextValue $data.documentDate), 'yyyy-MM-dd', $null)
    Set-Cell $sheet 3 1 ($date.ToString('yyyy年MM月') + "　单号：" + (TextValue $data.documentNumber))
    for ($index = 0; $index -lt $required; $index++) {
        $line = @($data.lines)[$index]
        $row = 5 + $index
        Set-Cell $sheet $row 1 ($index + 1)
        Set-Cell $sheet $row 2 (TextValue $data.documentDate)
        Set-Cell $sheet $row 3 (TextValue $line.code)
        Set-Cell $sheet $row 4 (TextValue $line.batchNo)
        Set-Cell $sheet $row 5 (TextValue $line.name)
        Set-Cell $sheet $row 6 (TextValue $line.specification)
        Set-Cell $sheet $row 7 (TextValue $line.unit)
        Set-Cell $sheet $row 8 (NumberValue $line.quantity)
        Set-Cell $sheet $row 9 (TextValue $line.notes)
    }
    Set-Cell $sheet (24 + $extra) 1 "入  库  人："
    Set-Cell $sheet (24 + $extra) 6 "日期："
    Set-Cell $sheet (25 + $extra) 1 "部门负责人："
    Set-Cell $sheet (25 + $extra) 6 "日期："
    $sheet.PageSetup.PrintArea = '$A$1:$I$' + (25 + $extra)
}

function Fill-ProductionIssue($sheet, $data) {
    $required = @($data.lines).Count
    $extra = Expand-Table $sheet 6 40 $required 16 $true $false
    Clear-TableRows $sheet 6 ($required + [Math]::Max(0, 40 - $required)) 16
    Set-Cell $sheet 2 3 (Get-Field $data 'receivingDepartment')
    Set-Cell $sheet 2 6 (TextValue $data.documentNumber)
    Set-Cell $sheet 2 14 (TextValue $data.documentDate)
    Set-Cell $sheet 3 3 (Get-Field $data 'productName')
    Set-Cell $sheet 3 6 (Get-Field $data 'productionBatch')
    Set-Cell $sheet 3 14 (Get-Field $data 'plannedQuantity')
    Set-Cell $sheet 4 3 (Get-Field $data 'productModel')
    for ($index = 0; $index -lt $required; $index++) {
        $line = @($data.lines)[$index]
        $row = 6 + $index
        Set-Cell $sheet $row 1 ($index + 1)
        Set-Cell $sheet $row 2 (TextValue $line.code)
        Set-Cell $sheet $row 3 (TextValue $line.name)
        Set-Cell $sheet $row 4 (NumberValue $line.unitUsage)
        Set-Cell $sheet $row 5 (TextValue $line.batchNo)
        Set-Cell $sheet $row 10 (NumberValue $line.externalQuantity)
        Set-Cell $sheet $row 11 (NumberValue $line.reworkQuantity)
        Set-Cell $sheet $row 12 (NumberValue $line.lossQuantity)
        Set-Cell $sheet $row 13 (NumberValue $line.returnQuantity)
        $note = "领用：" + (TextValue $line.quantity)
        if (-not [string]::IsNullOrWhiteSpace((TextValue $line.notes))) {
            $note += "；" + (TextValue $line.notes)
        }
        Set-Cell $sheet $row 14 $note
    }
    Set-Cell $sheet (47 + $extra) 1 "首次领用人"
    Set-Cell $sheet (47 + $extra) 7 "批准"
    Set-Cell $sheet (48 + $extra) 1 "二次领用"
    Set-Cell $sheet (48 + $extra) 7 "批准"
    Set-Cell $sheet (49 + $extra) 1 "退料人"
    Set-Cell $sheet (49 + $extra) 7 "批准"
    $sheet.PageSetup.PrintArea = '$A$1:$P$' + (49 + $extra)
}

function Fill-Outbound($sheet, $data) {
    $required = @($data.lines).Count
    $extra = Expand-Table $sheet 5 10 $required 8 $false $false
    Clear-TableRows $sheet 5 ($required + [Math]::Max(0, 10 - $required)) 8
    $company = Get-Field $data 'customerCompany'
    if ([string]::IsNullOrWhiteSpace($company)) { $company = Get-Field $data 'destination' }
    if ([string]::IsNullOrWhiteSpace($company)) { $company = Get-Field $data 'purpose' }
    Set-Cell $sheet 2 6 ("　单号：" + (TextValue $data.documentNumber))
    Set-Cell $sheet 3 1 ("发往单位：" + $company)
    Set-Cell $sheet 3 3 ("　收货人：" + (Get-Field $data 'customerContact'))
    Set-Cell $sheet 3 6 ("出库日期：" + (TextValue $data.documentDate))
    for ($index = 0; $index -lt $required; $index++) {
        $line = @($data.lines)[$index]
        $row = 5 + $index
        Set-Cell $sheet $row 1 ($index + 1)
        Set-Cell $sheet $row 2 (TextValue $line.name)
        Set-Cell $sheet $row 3 (TextValue $line.specification)
        Set-Cell $sheet $row 4 (TextValue $line.unit)
        Set-Cell $sheet $row 5 (NumberValue $line.quantity)
        Set-Cell $sheet $row 6 (TextValue $line.batchNo)
        Set-Cell $sheet $row 7 (TextValue $line.serialNumbers)
        Set-Cell $sheet $row 8 (TextValue $line.notes)
    }
    $sheet.PageSetup.PrintArea = '$A$1:$H$' + (20 + $extra)
}

function Fill-Delivery($sheet, $data) {
    $required = @($data.lines).Count
    $extra = Expand-Table $sheet 5 3 $required 9 $false $false
    Clear-TableRows $sheet 5 ($required + [Math]::Max(0, 3 - $required)) 9
    Set-Cell $sheet 3 1 ("发往单位：" + (Get-Field $data 'customerCompany') +
                         "　送货日期：" + (TextValue $data.documentDate) +
                         "　单号：" + (TextValue $data.documentNumber))
    for ($index = 0; $index -lt $required; $index++) {
        $line = @($data.lines)[$index]
        $row = 5 + $index
        Set-Cell $sheet $row 1 ($index + 1)
        $order = TextValue $line.orderNumber
        if ([string]::IsNullOrWhiteSpace($order)) { $order = Get-Field $data 'salesOrderNumber' }
        Set-Cell $sheet $row 2 $order
        Set-Cell $sheet $row 3 (TextValue $line.name)
        Set-Cell $sheet $row 4 (TextValue $line.specification)
        Set-Cell $sheet $row 5 (TextValue $line.unit)
        Set-Cell $sheet $row 6 (NumberValue $line.quantity)
        Set-Cell $sheet $row 7 (TextValue $line.batchNo)
        Set-Cell $sheet $row 8 (TextValue $line.serialNumbers)
        Set-Cell $sheet $row 9 (TextValue $line.notes)
    }
    Set-Cell $sheet (8 + $extra) 1 ("收货人信息：" + (Get-Field $data 'destination') +
                                    "；联系人：" + (Get-Field $data 'customerContact') +
                                    "；联系电话：" + (Get-Field $data 'customerPhone') +
                                    "；物流：" + (Get-Field $data 'logisticsCompany') +
                                    "；运单号：" + (Get-Field $data 'trackingNumber'))
    Set-Cell $sheet (9 + $extra) 1 "收货人："
    Set-Cell $sheet (9 + $extra) 6 "日期："
    $sheet.PageSetup.PrintArea = '$A$1:$I$' + (9 + $extra)
}

try {
    $data = Get-Content -LiteralPath $DataFile -Raw -Encoding UTF8 | ConvertFrom-Json
}
catch {
    [Console]::Error.WriteLine('无法读取表单填写数据：' + $_.Exception.Message)
    exit 1
}

$templatePath = TextValue $data.templatePath
$outputPath = TextValue $data.outputPath
$expectedSheet = TextValue $data.sheetName
$processIdPath = TextValue $data.processIdPath
if ([string]::IsNullOrWhiteSpace($templatePath) -or [string]::IsNullOrWhiteSpace($outputPath)) {
    [Console]::Error.WriteLine('表单模板路径或输出路径为空，无法生成表单。')
    exit 1
}

# 依次完整尝试每个自动化引擎：任一环节失败都换下一个引擎重新开始，而不是只判断组件能否创建。
$candidates = @(
    @{ ProgId = 'Ket.Application'; Name = 'WPS 表格'; ProcessName = 'et' },
    @{ ProgId = 'Excel.Application'; Name = 'Microsoft Excel'; ProcessName = 'EXCEL' },
    @{ ProgId = 'ET.Application'; Name = 'WPS 表格（兼容模式）'; ProcessName = 'et' }
)
$errors = @()

foreach ($candidate in $candidates) {
    $excel = $null
    $workbook = $null
    $sheet = $null
    $officeEngine = $candidate.Name
    $completed = $false
    $automationProcessId = 0
    $quitApplication = $true
    $existingProcessIds = @(Get-Process -Name $candidate.ProcessName -ErrorAction SilentlyContinue |
                            Select-Object -ExpandProperty Id)
    try {
        $excel = New-Object -ComObject $candidate.ProgId
        $automationProcessId = Get-SpreadsheetProcessId $excel
        if ($automationProcessId -gt 0 -and $existingProcessIds -contains $automationProcessId) {
            # 某些 WPS 版本会复用用户已经打开的进程；这种情况下只关闭工作簿，不退出或强杀用户进程。
            $quitApplication = $false
            $automationProcessId = 0
        }
        Write-AutomationProcessId $automationProcessId
        $excel.Visible = $false
        $excel.DisplayAlerts = $false
        # 每次尝试都从原始模板重新复制，上一次失败的表单不会影响本次填写。
        Copy-TemplateToOutput $templatePath $outputPath
        $workbook = $excel.Workbooks.Open($outputPath, 0, $false)

        $sheet = Find-Worksheet $workbook $expectedSheet

        for ($index = $workbook.Worksheets.Count; $index -ge 1; $index--) {
            $other = $workbook.Worksheets.Item($index)
            if ($other.Name -ne $sheet.Name) {
                try { $other.Delete() }
                finally { Release-ComReference $other }
            }
            else {
                # $other 与 $sheet 可能是同一个 RCW，不能在填写前释放。
                $other = $null
            }
        }
        if ((TextValue $data.kind) -eq 'deliveryConfirmation') { $sheet.Name = '送货确认单' }

        switch (TextValue $data.kind) {
            'inspection' { Fill-Inspection $sheet $data }
            'rawInbound' { Fill-Inbound $sheet $data }
            'finishedInbound' { Fill-Inbound $sheet $data }
            'productionIssue' { Fill-ProductionIssue $sheet $data }
            'stockOutbound' { Fill-Outbound $sheet $data }
            'deliveryConfirmation' { Fill-Delivery $sheet $data }
            default { throw ('不支持的表单类型：' + (TextValue $data.kind)) }
        }

        $workbook.Save()
        Close-Spreadsheet $true $sheet $workbook $excel $quitApplication $automationProcessId
        $sheet = $null
        $workbook = $null
        $excel = $null
        $completed = $true
    }
    catch {
        $message = $_.Exception.Message
        if (-not [string]::IsNullOrWhiteSpace($officeEngine)) {
            $message = $officeEngine + '：' + $message
        }
        $errors += $message
        # 不保存地关闭本次工作簿并退出该组件，再还原输出文件，避免泄漏和污染下一次尝试。
        Close-Spreadsheet $false $sheet $workbook $excel $quitApplication $automationProcessId
        $workbook = $null
        $excel = $null
        $sheet = $null
        try { Copy-TemplateToOutput $templatePath $outputPath } catch {}
    }
    if ($completed) { exit 0 }
}

[Console]::Error.WriteLine(
    'Microsoft Excel 与 WPS 表格均无法完成表单填写，已依次完整尝试以下组件：' +
    [Environment]::NewLine + ($errors -join [Environment]::NewLine))
exit 1
