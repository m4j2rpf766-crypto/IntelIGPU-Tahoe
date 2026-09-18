// TGL16 has no dynamic-library compiler/loader implementation. Report the
// unsupported API contract before the inherited Tahoe loader asserts on nil
// targetDeviceArchitecture. Ordinary AIR library creation stays native.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <string.h>
static id rejectDynamicLibrary(id self,SEL cmd,id input,NSError**error){
 (void)self;(void)cmd;(void)input;
 if(error)*error=[NSError errorWithDomain:MTLDynamicLibraryDomain code:MTLDynamicLibraryErrorUnsupported userInfo:@{NSLocalizedDescriptionKey:@"Dynamic shader libraries are unsupported by this legacy TGL adapter."}];
 return nil;
}
static BOOL dynamicUnsupported(id self,SEL cmd){(void)self;(void)cmd;return NO;}
static BOOL reimsInstallDynamicLibraryCompat(Class device){
 static Class installed;if(installed)return installed==device;
 const char*names[]={"newDynamicLibrary:error:","newDynamicLibraryWithURL:error:"};
 Method methods[2];
 for(unsigned i=0;i<2;i++){methods[i]=class_getInstanceMethod(device,sel_registerName(names[i]));Dl_info info={0};if(!methods[i]||strcmp(method_getTypeEncoding(methods[i]),"@32@0:8@16^@24")||!dladdr((void*)method_getImplementation(methods[i]),&info)||!info.dli_fname||!strstr(info.dli_fname,"/Metal.framework/"))return NO;}
 for(unsigned i=0;i<2;i++)class_replaceMethod(device,sel_registerName(names[i]),(IMP)rejectDynamicLibrary,method_getTypeEncoding(methods[i]));
 class_replaceMethod(device,sel_registerName("supportsDynamicLibraries"),(IMP)dynamicUnsupported,"c16@0:8");
 class_replaceMethod(device,sel_registerName("supportsRenderDynamicLibraries"),(IMP)dynamicUnsupported,"c16@0:8");
 installed=device;return YES;
}
