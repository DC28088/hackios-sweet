#!/usr/bin/env python3
"""Package compiled Sweet UEFI + resident HackiOS loader; never runs device commands."""
from pathlib import Path
import argparse,gzip,struct,hashlib,json,os,tempfile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--firmware',required=True,type=Path)
p.add_argument('--loader',required=True,type=Path)
p.add_argument('--kernel',type=Path)
p.add_argument('--template',required=True,type=Path)
p.add_argument('--out',required=True,type=Path)
a=p.parse_args();fw=a.firmware
fd=(fw/'SWEET_UEFI.fd').read_bytes();shim=(fw/'BootShim.bin').read_bytes();dtb=(fw/'sweet.dtb').read_bytes()
assert len(fd)==0x300000 and shim[56:60]==b'ARM\x64' and dtb[:4]==b'\xd0\x0d\xfe\xed'
# Match the measured Sweetin bootloader board ID. This changes only the
# public identity cells in the generic published Sweet DTB, no device dump.
dtb=bytearray(dtb);pos,strings=struct.unpack_from('>II',dtb,8);depth=-1;bound=False
while True:
 token=struct.unpack_from('>I',dtb,pos)[0];pos+=4
 if token==1:
  depth+=1;end=dtb.index(0,pos);pos=(end+4)&~3
 elif token==2:depth-=1
 elif token==3:
  size,key=struct.unpack_from('>II',dtb,pos);pos+=8;end=dtb.index(0,strings+key);name=bytes(dtb[strings+key:end])
  if depth==0 and name==b'qcom,board-id':
   assert size==8;struct.pack_into('>II',dtb,pos,46,0);bound=True
  pos=(pos+size+3)&~3
 elif token==9:break
 elif token!=4:raise ValueError('Unexpected DTB token')
assert bound,'Missing root board identity'
dtb=bytes(dtb)
loader=a.loader.read_bytes();assert loader[:2]==b'MZ'
off=struct.unpack_from('<I',loader,60)[0];assert loader[off:off+4]==b'PE\0\0' and struct.unpack_from('<H',loader,off+4)[0]==0xaa64
items={'loader.efi':loader,'usb':b'1\0'}
if a.kernel:
 k=a.kernel.read_bytes();assert k[:4]==b'\xcf\xfa\xed\xfe';items['kernel']=k
body=bytearray()
for name,data in items.items():
 body.extend(struct.pack('<16sII',name.encode(),len(data),0));body.extend(data);body.extend(bytes(-len(data)%16))
body.extend(bytes(24));payload=struct.pack('<II',0x50554e58,len(body))+body
assert len(payload)<0x03fff000
compressed=bytearray(gzip.compress(shim+fd+payload+bytes(-(len(shim)+len(fd)+len(payload))%16),compresslevel=6,mtime=0))
# Python3.12 sets OS=Unix, 3.13+ sets OS=unknown. Pin the header across hosts.
compressed[9]=3
kernel=bytes(compressed)+dtb
base=a.template.read_bytes();assert base[:8]==b'ANDROID!' and struct.unpack_from('<I',base,40)[0]==1
page=struct.unpack_from('<I',base,36)[0];header=bytearray(base[:page]);assert page>=1648
struct.pack_into('<I',header,8,len(kernel));struct.pack_into('<I',header,16,0);struct.pack_into('<I',header,24,0)
struct.pack_into('<IQI',header,1632,0,0,1648)
id=hashlib.sha1(kernel+struct.pack('<I',len(kernel))+struct.pack('<I',0)+struct.pack('<I',0)).digest()
header[576:608]=id+bytes(12)
a.out.parent.mkdir(parents=True,exist_ok=True)
# xl enter rebuilds the same generated filename on every session. Replace it
# only after a complete image has been written beside it.
temporary = None
try:
 with tempfile.NamedTemporaryFile(dir=a.out.parent,prefix=a.out.name+'.',suffix='.partial',delete=False) as f:
  temporary=Path(f.name);f.write(header);f.write(kernel);f.write(bytes(-len(kernel)%page))
 os.replace(temporary,a.out)
finally:
 if temporary is not None:temporary.unlink(missing_ok=True)
report={'device':'sweet','mode':'resident HackiOS USB loader','ios_userland_required':'iOS16.5 APFS system and helper images; prepared separately','hardware_booted':False,'embedded_dtb_board_id':[46,0],'embedded_dtb_sha256':hashlib.sha256(dtb).hexdigest(),'gpu_rendering_verified':False,'files':{}}
for name,path in {'boot':a.out,'uefi':fw/'SWEET_UEFI.fd','shim':fw/'BootShim.bin','dtb':fw/'sweet.dtb','loader':a.loader,**({'kernel':a.kernel} if a.kernel else {})}.items():
 report['files'][name]={'path':str(path),'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
a.out.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
