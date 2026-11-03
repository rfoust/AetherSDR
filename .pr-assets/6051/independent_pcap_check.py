from pathlib import Path
import struct,collections,json,hashlib
root=Path(__file__).resolve().parent
base=root/'evidence/issue-6051-evidence'
def read(name):
 data=(base/name).read_bytes(); magic=data[:4]
 formats={b'\xd4\xc3\xb2\xa1':('<',1e6),b'\xa1\xb2\xc3\xd4':('>',1e6),b'\x4d\x3c\xb2\xa1':('<',1e9),b'\xa1\xb2\x3c\x4d':('>',1e9)}
 e,scale=formats[magic];lt=struct.unpack_from(e+'I',data,20)[0];o=24;out=[]
 while o<len(data):
  assert len(data)-o>=16
  sec,frac,n,orig=struct.unpack_from(e+'IIII',data,o);o+=16
  assert n<=orig and o+n<=len(data);b=data[o:o+n];o+=n
  if lt==1: ip=14;typ=struct.unpack_from('>H',b,12)[0]
  elif lt==113:ip=16;typ=struct.unpack_from('>H',b,14)[0]
  elif lt==276:ip=20;typ=struct.unpack_from('>H',b,0)[0]
  elif lt==12 or lt==101:ip=0;typ=0x800
  else: raise ValueError(('unsupported linktype',lt))
  if typ!=0x800 or len(b)<ip+20 or b[ip]>>4!=4 or b[ip+9]!=17:continue
  ihl=(b[ip]&15)*4;v=ip+ihl+8
  if len(b)<v+28 or struct.unpack_from('>H',b,ip+6)[0]&0x1fff:continue
  raw=b[v:];w,sid,oui,cl,tsi,tsf=struct.unpack_from('>IIIIIQ',raw)
  if w>>28 not in (1,3) or not w&0x08000000:continue
  pcc=cl&65535;seq=(w>>16)&15
  if pcc==0x8005:key=(sid,pcc,tsi,tsf)
  elif pcc==0x8003 and len(raw)>=40:key=(sid,pcc,tsi,raw[28:40].hex())
  elif pcc==0x8004 and len(raw)>=64:key=(sid,pcc,tsi,raw[28:64].hex())
  else:key=None
  out.append(dict(ts=sec+frac/scale,raw=raw,key=key,sid=sid,pcc=pcc,seq=seq,tsi=tsi,tsf=tsf))
 return out
allfiles={str(p.name):read(p.name) for p in base.glob('*.pcap')}
client=allfiles['opus_3_client_mac_wifi.pcap'];tunnel=allfiles['opus_2_tunnel_ingress_rutx_wire_vpn.pcap'];radio=allfiles['opus_1_radio_side_rutx_eth0.pcap']
summary={}
for pcc,label in [(0x8005,'Opus'),(0x8003,'FFT'),(0x8004,'Waterfall')]:
 c=[x for x in client if x['pcc']==pcc];t=[x for x in tunnel if x['pcc']==pcc];r=[x for x in radio if x['pcc']==pcc]
 for rows in (c,t,r):assert len({x['key'] for x in rows})==len(rows)
 index={x['key']:i for i,x in enumerate(t)}
 assert all(x['key'] in index for x in c)
 rank=[index[x['key']] for x in c];lo,hi=min(rank),max(rank)
 missing=set(range(lo,hi+1))-set(rank)
 top=-1;late=0;maxbehind=0
 for n in rank:
  if n<top:late+=1;maxbehind=max(maxbehind,top-n)
  top=max(top,n)
 breaks=lambda a:sum(b['seq']!=((a1['seq']+1)&15) for a1,b in zip(a,a[1:]))
 conceal=sum(min((b['seq']-a['seq']-1)&15,8) for a,b in zip(c,c[1:]) if b['seq']!=((a['seq']+1)&15))
 radio_keys={x['key'] for x in r}
 assert all(x['key'] in radio_keys for x in c)
 summary[label]={'client_packets':len(c),'tunnel_packets_in_matching_span':hi-lo+1,'packets_missing_at_client':len(missing),'reordered_packets':late,'max_reorder_distance':maxbehind,'client_sequence_breaks':breaks(c),'tunnel_sequence_breaks_full_capture':breaks(t),'radio_sequence_breaks_full_capture':breaks(r),'predicted_plc_packet_units':conceal if label=='Opus' else None}
# Feed only captured headers to actual production accounting, without decoding truncated payloads.
with (root/'captured-vita-headers.dat').open('wb') as f:
 for p in client:
  if p['pcc'] in (0x8005,0x8003,0x8004,0x8002):f.write(p['raw'][:28])
print(json.dumps(summary,indent=2));(root/'pcap-results.json').write_text(json.dumps(summary,indent=2)+'\n')
