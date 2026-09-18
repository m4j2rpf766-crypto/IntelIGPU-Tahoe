#import "pipeline_descriptor_compat.m"
#import "../texture_sync_compat.m"
__attribute__((constructor)) static void installLogic(void){
 Method m=class_getInstanceMethod(objc_getClass("MTLIGAccelDevice"),sel_registerName("initWithAcceleratorPort:"));Dl_info di={0};
 if(!m||!dladdr(method_getImplementation(m),&di)||!reimsInstallRPCompat(di.dli_fbase)||!reimsInstallTextureSync(di.dli_fbase))abort();
 fprintf(stderr,"LOGIC_PROJECTION_INSTALLED\n");
}
