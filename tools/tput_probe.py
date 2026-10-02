#!/usr/bin/env python3
"""TCP throughput through the Wi-Fi interface, without the internet in the way.

Usage: python3 tools/tput_probe.py WIFI_IF WIRED_IF [SECONDS [RUNS]]
       e.g. python3 tools/tput_probe.py en4 en0 5 3

Needs the machine to be on the same network twice: through the kext's interface
and through a wired port (see tools/rtt_probe.py). A TCP connection is made from
the Wi-Fi interface to the wired port's address, both ends bound to their
interface (IP_BOUND_IF), so the data crosses the air once and the wire once.
"up" is Wi-Fi to wire (the card transmits), "down" is wire to Wi-Fi. Prints the
rate and what the kext's counters (build/out/rtw89ctl status) did meanwhile; if
the frame counters do not move, the data did not go through the card. No root
needed.
"""
import re, socket, subprocess, sys, threading, time
IP_BOUND_IF=25
CHUNK=b'\xa5'*262144
def addr(ifname):
    out=subprocess.run(['ipconfig','getifaddr',ifname],capture_output=True,text=True).stdout.strip()
    if not out: sys.exit(f"{ifname} has no IPv4 address")
    return out
def bound(ifname, ip, port):
    s=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    s.setsockopt(socket.IPPROTO_IP,IP_BOUND_IF,socket.if_nametoindex(ifname))
    s.bind((ip,port)); return s
def counters():
    out=subprocess.run(['build/out/rtw89ctl','status'],capture_output=True,text=True).stdout
    m=re.search(r'sent (\d+) \(dropped (\d+)\), received (\d+) \(dropped (\d+): (\d+) not decrypted, (\d+) replayed, (\d+) duplicates',out)
    t=re.search(r'\((\d+) frame\(s\) released',out)
    return [int(m.group(i)) for i in (1,2,3,7)]+[int(t.group(1))]
def pump(src, dst, secs):
    """Send from src for secs seconds; return bytes that reached dst and the time taken."""
    got=[0]; first=[0.0]; last=[0.0]
    def reader():
        while True:
            try: d=dst.recv(1048576)
            except socket.timeout: break
            if not d: break
            now=time.monotonic()
            if not got[0]: first[0]=now
            got[0]+=len(d); last[0]=now
            if d.endswith(b'\x00'): break
    th=threading.Thread(target=reader); th.start()
    end=time.monotonic()+secs
    while time.monotonic()<end: src.sendall(CHUNK)
    src.sendall(b'\x00')
    th.join()
    return got[0], max(last[0]-first[0],1e-6)

WIFI,WIRED=sys.argv[1],sys.argv[2]
SECS=float(sys.argv[3]) if len(sys.argv)>3 else 5
RUNS=int(sys.argv[4]) if len(sys.argv)>4 else 3
wifi_ip,wired_ip=addr(WIFI),addr(WIRED)
srv=bound(WIRED,wired_ip,47032); srv.listen(1)
cli=bound(WIFI,wifi_ip,0); cli.settimeout(5)
try: cli.connect((wired_ip,47032))
except OSError as e: sys.exit(f"no connection from {WIFI} to {WIRED}: {e}")
wire,_=srv.accept()
wire.setsockopt(socket.IPPROTO_IP,IP_BOUND_IF,socket.if_nametoindex(WIRED))
for s in (cli,wire):
    s.settimeout(10)
    s.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
print(f"{WIFI} {wifi_ip} <-> {WIRED} {wired_ip}, {SECS:g} s per run")
for name,src,dst in (("down",wire,cli),("up",cli,wire)):
    for run in range(RUNS):
        c0=counters()
        n,t=pump(src,dst,SECS)
        time.sleep(0.3)
        c=[b-a for a,b in zip(c0,counters())]
        print(f"{name:4} {run+1}: {n*8/t/1e6:6.1f} Mb/s ({n/1e6:.0f} MB); frames sent +{c[0]} (dropped +{c[1]}), "
              f"received +{c[2]}, duplicates +{c[3]}, reorder timeouts +{c[4]}")
