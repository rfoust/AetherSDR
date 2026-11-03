"""Opt-in impairment relay: forwards actual FLEX TCP/UDP traffic, never synthesizes firmware.
Run only for an exclusive RX-only qualification session. It rewrites that client's
udpport to a local relay and changes only UDP arrival order / selected drops.
"""
import socket,selectors,time,json,re,pathlib,collections,sys
root=pathlib.Path(sys.argv[1]); radio=sys.argv[2]; root.mkdir(parents=True,exist_ok=True)
probe=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);probe.connect((radio,4992));local_ip=probe.getsockname()[0];probe.close()
sel=selectors.DefaultSelector(); tcp=socket.socket();tcp.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);tcp.bind((local_ip,4992));tcp.listen(1);tcp.setblocking(False);sel.register(tcp,selectors.EVENT_READ,'accept')
udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.bind(('0.0.0.0',0));udp.setblocking(False);sel.register(udp,selectors.EVENT_READ,'udp')
control=root/'relay-control.json';stats=collections.defaultdict(lambda:dict(received=0,forwarded=0,dropped=0,swaps=0,naive_errors=0,input_errors=0,last_seq=None,last_input_seq=None));held={};client=None;peer=None;target=None;buffer=b'';last_save=0;mode='pass';run=True;cfg={}
(root/'relay-ready.json').write_text(json.dumps({'pid':__import__('os').getpid(),'listen':local_ip+':4992','udp_port':udp.getsockname()[1]}))
def send(data,pcc):
 global target
 if target is None:return
 udp.sendto(data,target);s=stats[hex(pcc)+'/'+hex(int.from_bytes(data[4:8],'big'))];q=(int.from_bytes(data[:4],'big')>>16)&15
 if s['last_seq'] is not None and q!=((s['last_seq']+1)&15):s['naive_errors']+=1
 s['last_seq']=q;s['forwarded']+=1
try:
 while run:
  now=time.monotonic()
  try: cfg=json.loads(control.read_text());mode=cfg.get('mode','pass');run=not cfg.get('stop',False)
  except (OSError,ValueError):pass
  for key,_ in sel.select(.005):
   typ=key.data
   if typ=='accept':
    client,addr=tcp.accept();client_ip=addr[0];peer=socket.create_connection((radio,4992),timeout=8);client.setblocking(False);peer.setblocking(False);sel.register(client,selectors.EVENT_READ,'client');sel.register(peer,selectors.EVENT_READ,'peer')
   elif typ in ('client','peer'):
    source=key.fileobj;data=source.recv(65536)
    if not data:
     for sock in (client,peer):
      if sock is not None:
       try:sel.unregister(sock)
       except KeyError:pass
       sock.close()
     client=peer=None;target=None;buffer=b'';held.clear();continue
    if typ=='client':
     buffer+=data
     while b'\n' in buffer:
      line,buffer=buffer.split(b'\n',1)
      if cfg.get('force_opus',False) and b'|stream create type=remote_audio_rx ' in line:
       line=re.sub(rb'compression=\S+',b'compression=opus',line)
      m=re.match(rb'(C\d+\|client udpport )(\d+)',line)
      if m:
       target=(client_ip,int(m[2]));line=m[1]+str(udp.getsockname()[1]).encode()
       (root/'relay-routing.json').write_text(json.dumps({'actual_client_udp':target[1],'relay_udp':udp.getsockname()[1]}))
      peer.sendall(line+b'\n')
      if m:udp.sendto(b'\0',(radio,4992))
    else:client.sendall(data)
   else:
    data,addr=udp.recvfrom(65535)
    if addr[0]!=radio or len(data)<28:continue
    pcc=int.from_bytes(data[14:16],'big');sid=int.from_bytes(data[4:8],'big');s=stats[hex(pcc)+'/'+hex(sid)];s['received']+=1;n=s['received'];q=(int.from_bytes(data[:4],'big')>>16)&15
    if s['last_input_seq'] is not None and q!=((s['last_input_seq']+1)&15):s['input_errors']+=1
    s['last_input_seq']=q
    if (pcc in (0x8005,0x03e3,0x0123) and sid==0x04000008) or pcc in (0x8003,0x8004):
     if mode=='loss' and (n%200==100 or 140<=n%600<146):s['dropped']+=1;continue
     if mode=='swap' and n%80==40 and sid not in held:held[sid]=(now,data,pcc);continue
    send(data,pcc)
    if sid in held:
     _,old,oldpcc=held.pop(sid);send(old,oldpcc);stats[hex(oldpcc)+'/'+hex(sid)]['swaps']+=1
  for sid,(ts,data,pcc) in list(held.items()):
   if now-ts>.02:send(data,pcc);held.pop(sid)
  if now-last_save>.1:
   (root/'relay-stats.json').write_text(json.dumps({'mode':mode,'streams':stats},indent=2));last_save=now
finally:
 for _,data,pcc in held.values():send(data,pcc)
 (root/'relay-stats.json').write_text(json.dumps({'mode':mode,'streams':stats,'stopped':True},indent=2))
 for s in (client,peer,tcp,udp):
  if s:
   try:s.close()
   except OSError:pass
