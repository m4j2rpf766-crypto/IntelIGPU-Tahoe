#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
static volatile sig_atomic_t requested=0;
static void requestCapture(int n){(void)n;requested=1;}
#include <sys/file.h>
static uint64_t ns(clockid_t id){struct timespec t;clock_gettime(id,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static BOOL saveLine(int fd,NSDictionary *d){
 NSData *b=[NSJSONSerialization dataWithJSONObject:d options:0 error:nil];
 if(!b)return NO;size_t off=0;
 while(off<b.length){ssize_t n=write(fd,(const char*)b.bytes+off,b.length-off);if(n<0&&errno==EINTR)continue;if(n<=0)return NO;off+=(size_t)n;}
 return write(fd,"\n",1)==1;
}
int main(int argc,char **argv){@autoreleasepool{
 if(getuid()!=0 || argc!=4)return 2;
 int singleton=open("/var/run/reims-gpu-recorder-worker.lock",O_CREAT|O_RDWR,0600);
 if(singleton<0||flock(singleton,LOCK_EX|LOCK_NB))return 7;
 char *e;double seconds=strtod(argv[2],&e);if(*e||!isfinite(seconds)||seconds<0||seconds>86400)return 2;
 double ms=strtod(argv[3],&e);if(*e||!isfinite(ms)||ms<25||ms>1000)return 2;
 int fd=!strcmp(argv[1],"-")?dup(STDOUT_FILENO):open(argv[1],O_WRONLY|O_CREAT|O_EXCL,0600); BOOL pipeOutput=!strcmp(argv[1],"-");if(fd<0)return 3;
 struct sigaction sa={0};sa.sa_handler=requestCapture;sa.sa_flags=SA_RESTART;sigemptyset(&sa.sa_mask);sigaction(SIGUSR1,&sa,NULL);
 saveLine(fd,@{@"worker_ready":@YES,@"pid":@(getpid()),@"version":@5});
 io_service_t svc=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("ReimsADLDesktopLink"));
 if(!svc){close(fd);return 4;}
 uint64_t start=ns(CLOCK_MONOTONIC),stableSince=0,lastSync=start,lastCapture=0;uint32_t prev[36]={};BOOL have=NO;unsigned samples=0,attempts=0;uint64_t lastContext=0;int result=0;
 while(seconds==0 || ns(CLOCK_MONOTONIC)-start<(uint64_t)(seconds*1e9)){@autoreleasepool{
  uint64_t before=ns(CLOCK_MONOTONIC);kern_return_t rc=IORegistryEntrySetCFProperties(svc,(__bridge CFDictionaryRef)@{@"CaptureRCS":@YES});
  CFTypeRef raw=IORegistryEntryCreateCFProperty(svc,CFSTR("RCSSnapshotV1"),kCFAllocatorDefault,0);
  NSMutableDictionary *line=[@{@"wall_ns":@(ns(CLOCK_REALTIME)),@"mono_ns":@(before),@"read_end_ns":@(ns(CLOCK_MONOTONIC)),@"rc":@(rc)} mutableCopy];
  line[@"sequence"]=@(samples);line[@"worker_pid"]=@(getpid());
  BOOL valid=raw && CFGetTypeID(raw)==CFDataGetTypeID() && CFDataGetLength(raw)==sizeof(prev);
  uint32_t v[36]={};BOOL trigger=NO;
  if(valid){
   memcpy(v,CFDataGetBytePtr(raw),sizeof(v));NSMutableArray *words=[NSMutableArray array];for(unsigned i=0;i<36;i++)[words addObject:@(v[i])];line[@"raw36"]=words;
   // Preserve every sample including idle, unknown opcode, CAT_ERROR and reset.
   BOOL pending=(v[30]&0x1ffffc)!=(v[31]&0x1ffffc);
   const unsigned fields[]={8,9,13,14,15,16,19,20,30,31,32,33};BOOL same=have;
   for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++)if(v[fields[i]]!=prev[fields[i]])same=NO;
   if(!pending||!same)stableSince=pending?before:0;
   trigger=rc==0 && ((v[20]&4U) || (pending&&stableSince&&before-stableSince>=500000000ULL));
   memcpy(prev,v,sizeof(prev));have=YES;line[@"trigger"]=@(trigger);
  }
  if(raw)CFRelease(raw);
  if(!saveLine(fd,line)){result=5;break;}samples++;
  BOOL external=requested!=0;
  if((trigger||external) && (!lastCapture || before-lastCapture>=60000000000ULL)){
   requested=0;
   saveLine(fd,@{@"capture_started":@YES,@"external_request":@(external),@"wall_ns":@(ns(CLOCK_REALTIME)),@"mono_ns":@(before)});
   lastCapture=before;
   // Persist registers BEFORE asking the bounded page-capture endpoint.
   if(!pipeOutput && fsync(fd)){result=5;break;}attempts++;
   kern_return_t cap=IORegistryEntrySetCFProperties(svc,(__bridge CFDictionaryRef)@{@"CaptureRCSBatch":@YES});
   CFMutableDictionaryRef all=NULL;kern_return_t read=IORegistryEntryCreateCFProperties(svc,&all,kCFAllocatorDefault,0);
   NSDictionary *props=CFBridgingRelease(all);NSMutableDictionary *saved=[NSMutableDictionary dictionary];
   for(NSString *key in props)if([key hasPrefix:@"RCS"]||[key hasPrefix:@"Depth"])saved[key]=props[key];
   saved[@"ExternalRequest"]=@(external);saved[@"TaskCompletionProven"]=@NO;
   saved[@"CaptureResult"]=@(cap);saved[@"ReadResult"]=@(read);saved[@"WallNS"]=@(ns(CLOCK_REALTIME));
   NSString *path=[NSString stringWithFormat:@"capture-%d-%llu-%u.plist",getpid(),(unsigned long long)ns(CLOCK_REALTIME),attempts];
   NSData *data=[NSPropertyListSerialization dataWithPropertyList:saved format:NSPropertyListXMLFormat_v1_0 options:0 error:nil];
   BOOL written=data&&[data writeToFile:path options:NSDataWritingAtomic error:nil];
   if(!saveLine(fd,@{@"capture":path,@"rc":@(cap),@"saved":@(written)} )||!written){result=5;break;}
   // One anomaly window, including recovery; do not hammer a blocked endpoint.
   stableSince=0;have=NO;
  }
  if(before-lastContext>=1000000000ULL){
   lastContext=before;uint64_t begin=ns(CLOCK_MONOTONIC);NSMutableArray*queues=[NSMutableArray array];
   io_registry_entry_t parent=0;io_iterator_t children=0;
   if(!IORegistryEntryGetParentEntry(svc,kIOServicePlane,&parent)){
    if(!IORegistryEntryGetChildIterator(parent,kIOServicePlane,&children)){
     io_registry_entry_t child;unsigned scanned=0;
     while(scanned++<128 && (child=IOIteratorNext(children))){
      if(IOObjectConformsTo(child,"IGAccelCommandQueue")||IOObjectConformsTo(child,"IOAccelContext2")){
       CFMutableDictionaryRef properties=NULL;
       if(!IORegistryEntryCreateCFProperties(child,&properties,kCFAllocatorDefault,0)){
        NSDictionary*all=CFBridgingRelease(properties);NSMutableDictionary*q=[NSMutableDictionary dictionary];uint64_t rid=0;IORegistryEntryGetRegistryEntryID(child,&rid);q[@"registry_id"]=@(rid);
        io_name_t name={0};IORegistryEntryGetName(child,name);q[@"name"]=[NSString stringWithUTF8String:name];
        for(NSString*k in all)if([k isEqualToString:@"IOUserClientCreator"]||[k isEqualToString:@"lastSubmittedTime"]||[k rangeOfString:@"stamp" options:NSCaseInsensitiveSearch].location!=NSNotFound){id v=all[k];if([v isKindOfClass:NSString.class]||[v isKindOfClass:NSNumber.class])q[k]=v;}
        [queues addObject:q];
       }
      }
      IOObjectRelease(child);
      if(ns(CLOCK_MONOTONIC)-begin>50000000ULL)break;
     }
     IOObjectRelease(children);
    }IOObjectRelease(parent);
   }
   saveLine(fd,@{@"queue_snapshot":queues,@"wall_ns":@(ns(CLOCK_REALTIME)),@"duration_ns":@(ns(CLOCK_MONOTONIC)-begin),@"completion_stamps_available":@NO});
  }
  uint64_t now=ns(CLOCK_MONOTONIC);if(now-lastSync>=1000000000ULL){if(!pipeOutput && fsync(fd)){result=5;break;}lastSync=now;}
  if(rc||!valid){result=6;break;}
  usleep((useconds_t)(ms*1000));
 }}
 if(!saveLine(fd,@{@"finished":@YES,@"samples":@(samples),@"attempts":@(attempts),@"result":@(result)})||(!pipeOutput && fsync(fd)))result=5;
 close(fd);IOObjectRelease(svc);return result;
}}
