"""Run after building json_form_smoke with build_transfer_page_smoke.py."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "outputs/json-form-test"
EXE = OUT / "json_form_smoke.exe"
NS = {"s": "http://schemas.openxmlformats.org/spreadsheetml/2006/main"}
ENV = os.environ.copy()
ENV["PATH"] = os.pathsep.join(["D:/Qt/6.5.3/mingw_64/bin", "D:/Qt/Tools/mingw1120_64/bin", ENV["PATH"]])


def inspect(path):
    with ZipFile(path) as archive:
        assert archive.testzip() is None
        sheet = ET.fromstring(archive.read("xl/worksheets/sheet1.xml"))
        book = ET.fromstring(archive.read("xl/workbook.xml"))
        values = {}
        for cell in sheet.findall(".//s:sheetData/s:row/s:c", NS):
            value = cell.findtext("s:is/s:t", namespaces=NS)
            if value is None:
                number = cell.findtext("s:v", namespaces=NS)
                value = float(number) if number is not None else None
            values[cell.attrib["r"]] = value
        return sheet, book, values


samples = OUT / "samples"
subprocess.run([EXE, samples], check=True, cwd=ROOT, env=ENV)
forms = sorted(samples.glob("*-*.xlsx"))
assert len(forms) == 36
for path in forms:
    brand, kind, count = path.stem.split("-")
    count = int(count)
    sheet, book, cells = inspect(path)
    assert not any("不得回填" in str(v) or "{" in str(v) for v in cells.values())
    assert sheet.find("s:mergeCells", NS) is not None
    assert sheet.find("s:pageSetup", NS).get("fitToWidth") == "1"
    names = {n.get("name"): n.text for n in book.findall("s:definedNames/s:definedName", NS)}
    assert "_xlnm.Print_Area" in names
    if kind in ("rawInbound", "finishedInbound"):
        assert cells["D5"] == "000123" and cells["H5"] == 1 and cells[f"H{4+count}"] == count
        if kind == "rawInbound":
            assert cells["I4"] == "供应商" and cells["J5"] == "备注0"
            assert cells[f"A{5+max(count,2)}"] == "入  库  人："
            assert names["_xlnm.Print_Titles"].endswith("$1:$4")
            if count > 1:
                assert cells["I6"] == "测试供应商乙"
    elif kind == "inspection":
        assert cells["B8"] == count*(count+1)/2
        assert any("☑" in str(v) and "7天内" in str(v) for v in cells.values())
    elif kind == "productionIssue":
        assert cells["J6"] == (0 if count == 45 else None)
        assert "首次领用人" in cells.values()
        # Excel needs edge cells for the E:I merged batch column to print its entire border.
        assert all(f"{letter}6" in cells for letter in "EFGHI")
    elif kind == "deliveryConfirmation":
        assert cells["B5"] == "TEST-001"
        assert ("美鲸灵" if brand == "MEI_JING_LING" else "冰美肌") in cells["A1"]
    else:
        assert kind == "stockOutbound" and cells["A3"] == "发往单位：测试客户"

with ZipFile(samples / "list.xlsx") as archive:
    book = ET.fromstring(archive.read("xl/workbook.xml"))
    assert len(book.findall("s:sheets/s:sheet", NS)) == 2
    second = ET.fromstring(archive.read("xl/worksheets/sheet2.xml"))
    assert second.findtext(".//s:c[@r='A2']/s:is/s:t", namespaces=NS) == "001"

# Edit a private copy. The built executable must read changes without rebuilding.
with tempfile.TemporaryDirectory(dir=OUT) as directory:
    work = Path(directory)
    folder = work / "仓库系统表单模板"
    folder.mkdir()
    template = folder / "原材料入库单.json"
    shutil.copy(ROOT / folder.name / template.name, template)
    spec = json.loads(template.read_text(encoding="utf-8"))
    cols = spec["details"]["columns"]
    del cols[1:3]
    cols.insert(0, cols.pop(-2))
    cols.append({"label": "批号副本", "field": "line.batchNo", "width": 15})
    template.write_text(json.dumps(spec, ensure_ascii=False), encoding="utf-8")
    command = [EXE, work, "ICE_BEAUTY", "rawInbound", "1"]
    subprocess.run(command, check=True, cwd=work, env=ENV)
    output = work / "ICE_BEAUTY-rawInbound-1.xlsx"
    _, _, cells = inspect(output)
    headers = {v: k[:-1] for k, v in cells.items() if k.rstrip("0123456789")+"4" == k and v}
    assert next(iter(headers)) == "供应商" and "物料编码" not in headers
    assert cells[headers["批号副本"]+"5"] == "000123"
    prior = output.read_bytes()
    for invalid in ("{", json.dumps({**spec, "layoutColumns": 1})):
        template.write_text(invalid, encoding="utf-8")
        assert subprocess.run(command, cwd=work, env=ENV, capture_output=True).returncode != 0
        assert output.read_bytes() == prior
print("PASS: 36 business forms, list export, JSON column edits and atomic failure handling")
