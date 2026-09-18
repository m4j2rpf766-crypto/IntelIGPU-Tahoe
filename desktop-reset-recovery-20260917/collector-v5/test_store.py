import tempfile,pathlib,json
from recorder import Store
with tempfile.TemporaryDirectory() as d:
 s=Store(pathlib.Path(d))
 for t in range(10000,10701,5):
  s.append('summary',{'t':t},t);s.append('detail',{'t':t},t);s.tick(t)
 summary=sorted(s.rolling.glob('summary-*'));detail=sorted(s.rolling.glob('detail-*'))
 assert 120<=len(summary)<=122,len(summary)
 assert 12<=len(detail)<=14,len(detail)
 incident=s.trigger('test',10701);old=(incident/summary[0].name).read_bytes()
 s.append('detail',{'t':10702},10702);s.tick(10702)
 assert (incident/'after.jsonl').exists()
 s.append('summary',{'t':11400},11400);s.append('detail',{'t':11400},11400);s.tick(11400)
 assert (incident/summary[0].name).read_bytes()==old
 for i in range(7):s.trigger('retention_test',11500+i*70)
 assert len(list(s.incidents.iterdir()))==5
 assert incident.exists(),'first incident lost'
 s.close()
 print('PASS: 10-minute/60-second windows, frozen pre-event, post-event append, five-incident cap preserves first')
