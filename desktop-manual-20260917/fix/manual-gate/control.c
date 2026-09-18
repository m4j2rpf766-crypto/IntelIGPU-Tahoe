#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
 if(argc!=2 || (strcmp(argv[1],"SelfTest") && strcmp(argv[1],"RemoveSessionRules") && strcmp(argv[1],"PrepareRuntime") && strcmp(argv[1],"PrepareDisplay") && strcmp(argv[1],"CommitDisplay"))) {
  fprintf(stderr,"usage: %s SelfTest|RemoveSessionRules|PrepareRuntime|PrepareDisplay|CommitDisplay\n",argv[0]); return 64;
 }
 io_service_t s=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("ReimsADLManualActivation"));
 if(!s){fprintf(stderr,"Dormant controller is not loaded.\n");return 69;}
 CFStringRef action=CFStringCreateWithCString(NULL,argv[1],kCFStringEncodingUTF8);
 const void *keys[]={CFSTR("ManualAction")},*values[]={action};
 CFDictionaryRef properties=CFDictionaryCreate(NULL,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
 kern_return_t result=IORegistryEntrySetCFProperties(s,properties);
 printf("%s result=0x%08x\n",argv[1],result);
 CFRelease(properties);CFRelease(action);IOObjectRelease(s);
 return result==KERN_SUCCESS?0:1;
}
