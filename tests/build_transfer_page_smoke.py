"""Link a UI test to the actual, already built IceBeauty client objects."""
from pathlib import Path
import os
import shlex
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
build=ROOT/os.environ.get('WMS_TEST_BUILD', 'build-lan-client')
# Use the current link manifest, excluding stale objects from previous source layouts.
manifest = build/'CMakeFiles/IceBeautyWms.dir/objects1.rsp'
objects = [build/p for p in shlex.split(manifest.read_text(encoding='utf-8')) if not p.endswith('/main.cpp.obj')]
assert len(objects)>40
qt=Path('D:/Qt/6.5.3/mingw_64')
compiler=Path('D:/Qt/Tools/mingw1120_64/bin/g++.exe')
test_name=sys.argv[1] if len(sys.argv)>1 else 'transfer_page_smoke'
destinations = {'transfer_page_smoke': 'transfer-ui-test', 'inbound_supplier_smoke': 'inbound-supplier',
                'json_form_smoke': 'json-form-test', 'process_bom_hierarchy_smoke': 'process-bom-test'}
if test_name not in destinations:
    raise ValueError('Unknown UI test')
out=ROOT/'outputs'/destinations[test_name]
out.mkdir(exist_ok=True)
args=['-std=c++17','-g','-DWMS_NETWORK_CLIENT_ONLY','-DWMS_PROFILE_ICE_BEAUTY=1',
      '-I'+str(ROOT/'src'),'-I'+str(qt/'include')]
for module in ('Core','Gui','Widgets','Network','Sql'):
    args+=['-I'+str(qt/('include/Qt'+module))]
args += [str(ROOT/'tests'/f'{test_name}.cpp'),*[str(p) for p in objects],'-L'+str(qt/'lib')]
args += ['-lQt6'+m for m in ('QuickWidgets','WebView','Quick','QmlModels','OpenGL','Qml','PrintSupport','Widgets','Gui','Network','Sql','Core')]
args += ['-o',str(out/f'{test_name}.exe')]
response=out/'link.rsp'
response.write_text('\n'.join('"'+arg.replace('\\','/')+'"' for arg in args),encoding='utf-8')
env=os.environ.copy()
env['PATH']=str(compiler.parent)+os.pathsep+str(qt/'bin')+os.pathsep+env['PATH']
subprocess.run([str(compiler),'@'+str(response)],env=env,check=True)
print(out/f'{test_name}.exe')
