#!/usr/bin/python3
"""Root-owned, bounded on-disk GPU flight recorder. No GPU reset/load actions."""
import os, sys, json, time, pathlib, selectors, subprocess, shutil, signal, threading
ROOT=pathlib.Path('/Library/Logs/ReimsGPURecorder')
BIN=pathlib.Path('/Library/Application Support/ReimsGPURecorder')
PRED='(process == "kernel" AND (eventMessage CONTAINS[c] "GPURestart" OR eventMessage CONTAINS[c] "GPU hang" OR eventMessage CONTAINS[c] "Signaling hardware error")) OR (process == "WindowServer" AND (eventMessage CONTAINS[c] "timed out fence" OR eventMessage CONTAINS[c] "regained readiness"))'
STOP=False

def atomic(path,obj):
    tmp=path.with_suffix('.tmp')
    with tmp.open('w') as f: json.dump(obj,f); f.flush(); os.fsync(f.fileno())
    os.replace(tmp,path)

def save_snapshot(dest,name,args):
    try:
        r=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=5)
        (dest/name).write_bytes(r.stdout[:1048576])
    except subprocess.TimeoutExpired as e:(dest/name).write_bytes((e.output or b'')[:1048576]+b'\nTIMEOUT')
    except OSError as e:(dest/(name+'.error')).write_text(str(e))

def context_snapshot(dest):
    start=time.monotonic()
    # Triage appears after the first timeout, not necessarily at initial CAT_ERROR.
    for delay in (0,10,30):
        remaining=start+delay-time.monotonic()
        if remaining>0:time.sleep(remaining)
        if not dest.exists():return
        save_snapshot(dest,'triage-%02d.plist'%delay,
                      ['/usr/sbin/ioreg','-r','-n','IOResources','-d','1','-l','-a'])
        if delay==0:
            commands={'processes.txt':['/bin/ps','-axo','pid,ppid,state,%cpu,rss,command'],
                      'gpu.plist':['/usr/sbin/ioreg','-r','-c','IntelAccelerator','-l','-a'],
                      'framebuffer.plist':['/usr/sbin/ioreg','-r','-c','ReimsIntelADLFramebuffer','-l','-a']}
            for name,args in commands.items():save_snapshot(dest,name,args)

def capture_event(record):
    m=record.get('eventMessage','').lower()
    return any(x in m for x in ('timed out fence','signaling hardware error','gpu hang','gpurestartbegin','gpurestartskipped'))

class CaptureGate:
    def __init__(self):self.ready_pid=None;self.last=-float('inf')
    def request(self,p,now,send=os.kill):
        if not p or p.poll() is not None or self.ready_pid!=p.pid:return 'worker_unavailable'
        if now-self.last<60:return 'rate_limited'
        try:send(p.pid,signal.SIGUSR1)
        except ProcessLookupError:return 'worker_exited'
        self.last=now;return 'signalled'

def watchdog_report(path):
    try:
        with path.open('rb') as f:raw=f.read(2*1024*1024)
        d=json.loads(raw.split(b'\n',1)[1]);t=d.get('termination',{})
        # Watchdog stackshots use UUIDs: never depend on a driver name string.
        if t.get('namespace')=='WATCHDOG' and d.get('procName')=='WindowServer':
            return {'path':str(path),'pid':d.get('pid'),'captureTime':d.get('captureTime'),'termination':t}
    except (OSError,ValueError,IndexError):pass
    return None

class Store:
    def __init__(self,root):
        self.root=root; self.rolling=root/'rolling'; self.incidents=root/'incidents'
        for d in (self.rolling,self.incidents):d.mkdir(parents=True,exist_ok=True)
        self.active=None; self.until=0; self.lastsync=0; self.handles={}; self.lastlight=-1
    def append(self,kind,obj,now):
        bucket=int(now//5)*5; path=self.rolling/f'{kind}-{bucket}.jsonl'
        if path not in self.handles:
            for old in list(self.handles):
                if old.name.startswith(kind+'-'):
                    self.handles.pop(old).close()
            self.handles[path]=path.open('a',buffering=1)
        f=self.handles[path]
        if f.tell()<131072: f.write(json.dumps(obj,separators=(',',':'))+'\n')
        if self.active and now<=self.until:
            p=self.active/'after.jsonl'
            if not p.exists() or p.stat().st_size<8*1024*1024:
                with p.open('a') as a:a.write(json.dumps({'kind':kind,'data':obj},separators=(',',':'))+'\n')
    def sync(self):
        for f in self.handles.values():f.flush();os.fsync(f.fileno())
        if self.active:
            p=self.active/'after.jsonl'
            if p.exists():
                with p.open('rb') as f:os.fsync(f.fileno())
    def trigger(self,reason,now):
        if self.active and now<=self.until:
            self.append('event',{'reason':reason,'wall':now},now);return self.active
        self.sync()
        self.active=self.incidents/(time.strftime('%Y%m%d-%H%M%S',time.localtime(now))+'-'+str(time.time_ns()%1000000))
        self.active.mkdir();self.until=now+60
        # The current segment is copied too; originals remain writable.
        for f in self.rolling.glob('*.jsonl'):shutil.copyfile(f,self.active/f.name)
        atomic(self.active/'event.json',{'reason':reason,'at':now,'post_seconds':60,'first_root_cause_proven':False})
        self.trim_incidents()
        if self.root==ROOT:threading.Thread(target=context_snapshot,args=(self.active,),daemon=True).start()
        return self.active
    def trim_incidents(self):
        dirs=sorted(p for p in self.incidents.iterdir() if p.is_dir())
        def size():return sum(p.stat().st_size for p in self.root.rglob('*') if p.is_file())
        # Keep the earliest incident as well as recent ones, within a hard budget.
        while len(dirs)>5 or size()>128*1024*1024:
            choices=[d for d in dirs[1:] if d!=self.active]
            if not choices:break
            victim=choices[0];shutil.rmtree(victim);dirs.remove(victim)
    def tick(self,now):
        self.sync()
        for p in self.rolling.glob('*.jsonl'):
            kind,bucket=p.stem.split('-');keep=60 if kind=='detail' else 600
            if now-int(bucket)>keep+5 and p not in self.handles:p.unlink()
        if self.active and now>self.until:
            atomic(self.active/'complete.json',{'at':now});self.active=None
        self.trim_incidents()
    def close(self):
        self.sync()
        for f in self.handles.values():f.close()

def main():
    global STOP
    os.umask(0o077); ROOT.mkdir(parents=True,exist_ok=True)
    import fcntl
    lock=(ROOT/'daemon.lock').open('w');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    store=Store(ROOT);now=time.time()
    if any(store.rolling.glob('*.jsonl')):
        store.trigger('collector_restart_preserved_previous_boot_or_run',now)
        store.active=None # prior files are already frozen; do not mix startup into old event
    scratch=ROOT/'scratch';scratch.mkdir(exist_ok=True)
    sel=selectors.DefaultSelector();children={};buffers={};next_start={};last_sample=now;worker_started=now;gap=False;sample_count=0;last_tick=0;last_coarse=-1;last_exit={};gate=CaptureGate();seen_reports=set(pathlib.Path("/Library/Logs/DiagnosticReports").glob("WindowServer-*.ips"));last_watch=0
    def spawn(tag,args):
        p=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,cwd=scratch)
        os.set_blocking(p.stdout.fileno(),False);sel.register(p.stdout,selectors.EVENT_READ,tag)
        children[tag]=p;buffers[tag]=b'';return p
    def request(reason,now):
        result=gate.request(children.get('worker'),time.monotonic())
        store.append('event',{'capture_request':reason,'result':result,'at':now},now)
        store.sync()
    def stop(*_):
        global STOP; STOP=True
    signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
    while not STOP:
        now=time.time()
        for tag,p in list(children.items()):
            if p.poll() is not None:
                try:sel.unregister(p.stdout)
                except (KeyError,ValueError):pass
                p.stdout.close();children.pop(tag);last_exit[tag]=p.returncode;next_start[tag]=now+10
        for tag,args in [('worker',[str(BIN/'worker'),'-','0','250']),('log',['/usr/bin/log','stream','--style','ndjson','--predicate',PRED])]:
            if tag not in children and now>=next_start.get(tag,0):
                spawn(tag,args)
                if tag=='worker':worker_started=last_sample=now;gap=False
        for key,_ in sel.select(.2):
            tag=key.data;p=children[tag]
            try:data=os.read(key.fd,65536)
            except BlockingIOError:continue
            if not data:
                sel.unregister(key.fileobj);key.fileobj.close();p.poll()
                # Do not restart or kill a still-running kernel-blocked worker.
                if p.returncode is not None:
                    last_exit[tag]=p.returncode;children.pop(tag);next_start[tag]=now+10
                continue
            buffers[tag]+=data
            while b'\n' in buffers[tag]:
                line,buffers[tag]=buffers[tag].split(b'\n',1)
                try:record=json.loads(line)
                except (ValueError,UnicodeError):continue
                now=time.time()
                if tag=='log':
                    store.append('event',record,now)
                    if capture_event(record):
                        store.trigger('system_gpu_or_display_timeout',now);request(record.get('eventMessage','log'),now)
                elif 'worker_ready' in record:
                    gate.ready_pid=record['pid'];store.append('event',record,now)
                elif 'raw36' in record:
                    record['collector_received_ns']=time.time_ns();last_sample=now;gap=False;sample_count+=1
                    store.append('detail',record,now)
                    if int(now)!=last_coarse:
                        store.append('summary',record,now);last_coarse=int(now)
                    if record.get('trigger'):store.trigger('stalled_ring_or_cat_error',now)
                    if record['raw36'][20]&3:store.trigger('reset_requested_or_ready',now)
                elif 'queue_snapshot' in record:
                    store.append('queues',record,now)
                elif 'capture_started' in record:
                    store.trigger('capture_started',now);store.append('event',record,now);store.sync()
                elif 'capture' in record:
                    dest=store.trigger('command_page_capture',now)
                    cap=scratch/pathlib.Path(record['capture']).name
                    if cap.exists():shutil.move(str(cap),dest/cap.name)
                    store.append('event',record,now)
                else:store.append('event',record,now)
            if len(buffers[tag])>1048576:buffers[tag]=b''
        now=time.time()
        # This process can preserve history even when the worker is blocked in IOKit.
        if 'worker' in children and now-last_sample>3 and now-worker_started>3 and not gap:
            store.trigger('sampler_gap_over_3s_possible_IOKit_delay_or_system_sleep',now);request('sampler_gap',now);gap=True
        marker=pathlib.Path('/Users/Shared/ReimsGPURecorderRequest/mark.request')
        if marker.exists():marker.unlink();store.trigger('manual_mark',now);request('manual_mark',now)
        if now-last_watch>=1:
            last_watch=now
            for report in set(pathlib.Path('/Library/Logs/DiagnosticReports').glob('WindowServer-*.ips'))-seen_reports:
                info=watchdog_report(report)
                if info:
                    seen_reports.add(report);dest=store.trigger('windowserver_watchdog',now)
                    shutil.copyfile(report,dest/report.name);store.append('event',{'watchdog':info},now);request('windowserver_watchdog',now)
                else:
                    try:
                        if now-report.stat().st_mtime>30:seen_reports.add(report)
                    except FileNotFoundError:seen_reports.add(report)
        if now-last_tick>=1:
            store.tick(now);last_tick=now
            atomic(ROOT/'status.json',{'version':5,'at':now,'pid':os.getpid(),'worker_ready_pid':gate.ready_pid,'samples':sample_count,'last_sample':last_sample,'worker_pid':children.get('worker').pid if 'worker' in children else None,'log_pid':children.get('log').pid if 'log' in children else None,'active_incident':str(store.active) if store.active else None,'summary_seconds':600,'detail_seconds':60,'sample_interval_ms':250,'waiting_for_driver': 'worker' not in children,'gap':gap,'last_exit':last_exit})
    store.close()
    for p in children.values():
        if p.poll() is None:p.terminate()
    # No SIGKILL: a kernel-blocked sampler must not be replaced by more samplers.

if __name__=='__main__':
    try:main()
    except Exception as e:
        try:atomic(ROOT/'last-error.json',{'at':time.time(),'error':repr(e)})
        except OSError:pass
        raise

