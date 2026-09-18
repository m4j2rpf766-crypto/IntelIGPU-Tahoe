import pathlib,json,tempfile,signal
from recorder import CaptureGate,capture_event,watchdog_report,Store
class Child:
 pid=123
 def poll(self):return None
p=Child();g=CaptureGate();sent=[]
assert g.request(p,0,lambda *a:sent.append(a))=='worker_unavailable'
g.ready_pid=p.pid
assert capture_event({'eventMessage':'Synthetic test: timed out fence'})
assert g.request(p,0,lambda *a:sent.append(a))=='signalled'
assert g.request(p,.1,lambda *a:sent.append(a))=='rate_limited'
assert g.request(p,60,lambda *a:sent.append(a))=='signalled'
assert sent==[(123,signal.SIGUSR1)]*2
assert not capture_event({'eventMessage':'display regained readiness'})
with tempfile.TemporaryDirectory() as temp:
 p=pathlib.Path(temp)/'test.ips';p.write_text('{}\n'+json.dumps({'procName':'WindowServer','pid':42,'termination':{'namespace':'WATCHDOG'},'stackshot':{'binaryImages':[['uuid',0,'K']]}}))
 assert watchdog_report(p)['pid']==42
 p.write_text('{}\n{');assert watchdog_report(p) is None
 s=Store(pathlib.Path(temp)/'store');s.append('queues',{'queue_snapshot':[{'lastSubmittedTime':12}]},1000)
 d=s.trigger('system_gpu_or_display_timeout',1000);assert list(d.glob('queues-*'));s.close()
print('PASS: synthetic fence triggers request without ring-pending; rate limit; UUID-only watchdog; partial report; queues preserved')
