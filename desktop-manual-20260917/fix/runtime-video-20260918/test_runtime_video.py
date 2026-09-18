import copy,importlib.util,pathlib,tempfile,unittest
from unittest.mock import patch
base=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('runtime_video',base/'runtime_video.py');v=importlib.util.module_from_spec(spec);spec.loader.exec_module(v)
PROPS={'IOGVAHEVCEncode':'1','IODVDBundleName':'AppleIntelTGLGraphicsVADriver'}
def objects(root,name):
    result=[root] if root.get('IOObjectClass')==name else []
    for child in root.get('IORegistryEntryChildren',[]):result+=objects(child,name)
    return result
def tree(ready):
    a=dict(IOObjectClass='IntelAccelerator',IOServiceState=2,IORegistryEntryChildren=[])
    if ready:a.update(PROPS);a['IORegistryEntryChildren']=[dict(IOObjectClass='ReimsVideoDiscovery',CFBundleIdentifier=v.IDENTIFIER,PhysicalIdentityVerified=True,Published=True)]
    return a
class Cases(unittest.TestCase):
    def execute(self,loaded,ready,load_publishes=True,wrong=False,active=True):
        state={'loaded':loaded,'pci':tree(ready)};calls=[]
        if not active:state['pci']['IOServiceState']=0
        def run(args,timeout=10):
            calls.append([str(x) for x in args])
            if args[:2]==['kmutil','showloaded']:return ((v.IDENTIFIER+' '+('WRONG' if wrong else v.UUID)) if state['loaded'] else '').encode()
            if args[:2]==['kmutil','load']:
                state['loaded']=True
                if load_publishes:state['pci']=tree(True)
                return b'loaded'
            raise AssertionError(args)
        with tempfile.TemporaryDirectory() as d,patch.object(v,'verify_bundle',return_value=PROPS),patch.object(v.time,'monotonic',side_effect=[0,10,20]):
            try:v.ensure_video(run,lambda:copy.deepcopy(state['pci']),objects,pathlib.Path(d));error=None
            except AssertionError as e:error=str(e)
        return calls,error
    def test_already_ready_does_not_load(self):
        calls,error=self.execute(True,True);self.assertIsNone(error);self.assertFalse(any(x[:2]==['kmutil','load'] for x in calls))
    def test_missing_publisher_is_loaded_once(self):
        # Return the newly published tree on the first post-load read.
        state={'loaded':False,'pci':tree(False)};loads=[]
        def run(args,timeout=10):
            if args[:2]==['kmutil','load']:state.update(loaded=True,pci=tree(True));loads.append(args);return b''
            return ((v.IDENTIFIER+' '+v.UUID) if state['loaded'] else '').encode()
        with tempfile.TemporaryDirectory() as d,patch.object(v,'verify_bundle',return_value=PROPS),patch.object(v.time,'sleep'):
            v.ensure_video(run,lambda:copy.deepcopy(state['pci']),objects,pathlib.Path(d))
        self.assertEqual(len(loads),1)
    def test_load_without_publication_fails(self):
        calls,error=self.execute(False,False,load_publishes=False);self.assertIn('did not publish',error)
    def test_wrong_loaded_version_refused(self):
        calls,error=self.execute(True,True,wrong=True);self.assertIn('Unexpected loaded',error);self.assertEqual(len(calls),1)
    def test_unpublished_accelerator_never_activated(self):
        calls,error=self.execute(False,False,active=False);self.assertIn('already-published',error);self.assertEqual(calls,[])
    def test_publisher_flag_without_capabilities_is_not_ready(self):
        pci=tree(True);pci.pop('IOGVAHEVCEncode');self.assertFalse(v.published(pci,objects,PROPS))
unittest.main()
