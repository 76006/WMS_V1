param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$DataFile
)

$ErrorActionPreference = 'Stop'
$excel = $null
$workbook = $null
$officeEngine = ''

function Start-SpreadsheetApplication {
    $candidates = @(
        @{ ProgId = 'Excel.Application'; Name = 'Microsoft Excel' },
        @{ ProgId = 'Ket.Application'; Name = 'WPS 表格' },
        @{ ProgId = 'ET.Application'; Name = 'WPS 表格（兼容模式）' }
    )
    $errors = @()
    foreach ($candidate in $candidates) {
        try {
            $application = New-Object -ComObject $candidate.ProgId
            return @{
                Application = $application
                Name = $candidate.Name
            }
        }
        catch {
            $errors += ($candidate.Name + '：' + $_.Exception.Message)
        }
    }
    throw ('未检测到可用的 Microsoft Excel 或 WPS 表格自动化组件。' +
           [Environment]::NewLine + ($errors -join [Environment]::NewLine))
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
    Set-Cell $sheet 8 2 ((@($data.lines) | Measure-Object -Property quantity -Sum).Sum)
    Set-Cell $sheet 9 2 (Get-Field $data 'inspectionDate')
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
    $resultRow = 28 + $extra
    $inspectorRow = 32 + $extra
    $managerRow = 34 + $extra
    Set-Cell $sheet $resultRow 1 ("检验结果：" + (Get-Field $data 'inspectionResult') +
                                  "；" + (Get-Field $data 'conclusion'))
    Set-Cell $sheet $inspectorRow 4 "检验者："
    Set-Cell $sheet $inspectorRow 7 "委托人员："
    Set-Cell $sheet $managerRow 4 "部门负责人："
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
    $destination = Get-Field $data 'destination'
    if ([string]::IsNullOrWhiteSpace($destination)) { $destination = Get-Field $data 'purpose' }
    Set-Cell $sheet 3 1 ("发往单位：" + (Get-Field $data 'customerCompany') +
                         "　收货人：" + (Get-Field $data 'customerContact') +
                         "　单号：" + (TextValue $data.documentNumber) +
                         "　出库日期：" + (TextValue $data.documentDate) +
                         "　目的地：" + $destination)
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
    Set-Cell $sheet (15 + $extra) 1 "出库人："
    Set-Cell $sheet (15 + $extra) 5 "日期："
    Set-Cell $sheet (16 + $extra) 1 "复核/检查人："
    Set-Cell $sheet (16 + $extra) 5 "日期："
    Set-Cell $sheet (17 + $extra) 1 "质量负责人："
    Set-Cell $sheet (17 + $extra) 5 "日期："
    Set-Cell $sheet (18 + $extra) 1 "总经理/管理者代表："
    Set-Cell $sheet (18 + $extra) 5 "日期："
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
    $spreadsheet = Start-SpreadsheetApplication
    $excel = $spreadsheet.Application
    $officeEngine = $spreadsheet.Name
    $excel.Visible = $false
    $excel.DisplayAlerts = $false
    $workbook = $excel.Workbooks.Open((TextValue $data.outputPath), 0, $false)

    $sheet = $null
    try { $sheet = $workbook.Worksheets.Item((TextValue $data.sheetName)) } catch {}
    if ($null -eq $sheet) {
        throw "模板中找不到工作表：$($data.sheetName)"
    }

    for ($index = $workbook.Worksheets.Count; $index -ge 1; $index--) {
        $candidate = $workbook.Worksheets.Item($index)
        if ($candidate.Name -ne $sheet.Name) { $candidate.Delete() }
    }
    if ((TextValue $data.kind) -eq 'deliveryConfirmation') { $sheet.Name = '送货确认单' }

    switch (TextValue $data.kind) {
        'inspection' { Fill-Inspection $sheet $data }
        'rawInbound' { Fill-Inbound $sheet $data }
        'finishedInbound' { Fill-Inbound $sheet $data }
        'productionIssue' { Fill-ProductionIssue $sheet $data }
        'stockOutbound' { Fill-Outbound $sheet $data }
        'deliveryConfirmation' { Fill-Delivery $sheet $data }
        default { throw "不支持的表单类型：$($data.kind)" }
    }

    $workbook.Save()
    $workbook.Close($true)
    $workbook = $null
    $excel.Quit()
    $excel = $null
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    exit 0
}
catch {
    $message = $_.Exception.Message
    if (-not [string]::IsNullOrWhiteSpace($officeEngine)) {
        $message = $officeEngine + '：' + $message
    }
    [Console]::Error.WriteLine($message)
    if ($null -ne $workbook) {
        try { $workbook.Close($false) } catch {}
    }
    if ($null -ne $excel) {
        try { $excel.Quit() } catch {}
    }
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    exit 1
}
