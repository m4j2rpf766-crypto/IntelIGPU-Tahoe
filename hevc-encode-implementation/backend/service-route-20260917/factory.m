#import "../../../wechat-menu-tearing/compat-source/metal_entry_loader.m"
__attribute__((constructor)) static void reimsServiceBootstrap(void){
 if(strcmp(getprogname(),"VTEncoderXPCService"))return;
 Dl_info info={0};if(!dladdr(reimsServiceBootstrap,&info)||!info.dli_fname)return;
 char path[4096];snprintf(path,sizeof(path),"%s",info.dli_fname);char*p=strrchr(path,'/');if(!p)return;snprintf(p+1,sizeof(path)-(p+1-path),"libReimsHEVCService.dylib");
 if(!dlopen(path,RTLD_NOW|RTLD_LOCAL))os_log_error(OS_LOG_DEFAULT,"ReimsHEVCService bootstrap load: %{public}s",dlerror());
}
