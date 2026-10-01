#!/usr/bin/env python3
"""Round-trip timing through the Wi-Fi interface, with chosen request and reply sizes.

Usage: python3 tools/rtt_probe.py WIFI_IF WIRED_IF REQ,REPLY,GAP [REQ,REPLY,GAP ...]
       e.g. python3 tools/rtt_probe.py en4 en0 40,40,0.2 40,1000,0.02

Needs the machine to be on the same network twice: through the kext's interface
and through a wired port. Each probe is a UDP datagram of REQ bytes sent out of
the Wi-Fi interface to the wired port's address; a thread on the wired side
answers with REPLY bytes, which come back over Wi-Fi. Both sockets are bound to
their interface (IP_BOUND_IF), so the frames really cross the air. GAP is the
pause between probes in seconds. Prints the times and what the kext's counters
(build/out/rtw89ctl status) did meanwhile. No root needed.
"""
import re, socket, statistics, struct, subprocess, sys, threading, time
IP_BOUND_IF=25
def sock(ifname, ip, port):
    s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
    s.setsockopt(socket.IPPROTO_IP,IP_BOUND_IF,socket.if_nametoindex(ifname))
    s.bind((ip,port)); s.settimeout(0.5); return s
def addr(ifname):
    out=subprocess.run(['ipconfig','getifaddr',ifname],capture_output=True,text=True).stdout.strip()
    if not out: sys.exit(f"{ifname} has no IPv4 address")
    return out
WIFI,WIRED=sys.argv[1],sys.argv[2]
wired_ip=addr(WIRED)
wifi=sock(WIFI,addr(WIFI),47021); wire=sock(WIRED,wired_ip,47022)
stop=False
def responder():
    while not stop:
        try: d,a=wire.recvfrom(4096)
        except Exception: continue
        n=struct.unpack('>H',d[8:10])[0]
        wire.sendto(d[:8]+b'y'*(n-8),a)
threading.Thread(target=responder,daemon=True).start()
def counters():
    out=subprocess.run(['build/out/rtw89ctl','status'],capture_output=True,text=True).stdout
    m=re.search(r'received (\d+) \(dropped (\d+): (\d+) not decrypted, (\d+) replayed, (\d+) duplicates',out)
    t=re.search(r'\((\d+) frame\(s\) released',out)
    return int(m.group(1)),int(m.group(5)),int(t.group(1))
def rtt(req,rep,n,gap):
    ts=[]
    for i in range(n):
        t=time.time()
        wifi.sendto(struct.pack('>dH',t,rep)+b'x'*(req-10),(wired_ip,47022))
        try:
            while True:
                d,_=wifi.recvfrom(4096)
                if struct.unpack('>d',d[:8])[0]==t: break
            ts.append((time.time()-t)*1000)
        except Exception: ts.append(999)
        time.sleep(gap)
    return ts
for req,rep,gap in [(int(a),int(b),float(c)) for a,b,c in (x.split(',') for x in sys.argv[3:])]:
    r0,d0,t0=counters(); ts=rtt(req,rep,60,gap); r1,d1,t1=counters()
    slow=sum(1 for t in ts if t>8)
    print(f"req {req:4d} rep {rep:4d} gap {gap*1000:4.0f} ms: median {statistics.median(ts):5.1f}, avg {statistics.mean(ts):5.1f}, max {max(ts):5.1f}; {slow}/60 over 8 ms; frames received +{r1-r0}, duplicates +{d1-d0}, reorder timeouts +{t1-t0}")
stop=True
