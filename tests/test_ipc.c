#include "../core/ipc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
static void *echo(void *a){ int fd=*(int*)a; // reader: swallow a big frame, answer with a big frame
  unsigned char h[4]; size_t got=0; while(got<4){ssize_t n=read(fd,h+got,4-got); if(n<=0)return 0; got+=n;}
  size_t len=(h[0]<<24)|(h[1]<<16)|(h[2]<<8)|h[3]; char *b=malloc(len); got=0; while(got<len){ssize_t n=read(fd,b+got,len-got); if(n<=0)return 0; got+=n;}
  assert(len==900000);
  unsigned char hh[4]={h[0],h[1],h[2],h[3]}; write(fd,hh,4); size_t w=0; while(w<len){ssize_t n=write(fd,b+w,len-w); if(n<=0)return 0; w+=n;}
  free(b); return 0; }
int main(void){ int sv[2]; socketpair(AF_UNIX,SOCK_STREAM,0,sv);
  IPCBridge ipc; ipc_init(&ipc); ipc.fd=sv[0]; ipc.connected=true; fcntl(sv[0],F_SETFL,O_NONBLOCK);
  pthread_t t; pthread_create(&t,0,echo,&sv[1]);
  char *msg=malloc(900000); memset(msg,'a',900000); msg[0]='{'; msg[899999]='}';
  assert(ipc_send(&ipc,msg,900000)); // > socket buffer: exercises EAGAIN path
  int n=0; for(int i=0;i<2000 && n==0;i++){ n=ipc_poll(&ipc); usleep(1000);} assert(n==1);
  char *r=ipc_read_message(&ipc); assert(r && strlen(r)==900000 && r[0]=='{'); free(r);
  assert(ipc_read_message(&ipc)==NULL);
  pthread_join(t,0); close(sv[1]);
  ipc_poll(&ipc); assert(!ipc.connected);            // EOF -> disconnected, no crash
  assert(!ipc_send(&ipc,"x",1));                      // send on dead link: false, no SIGPIPE
  // oversized
  free(msg); ipc_free(&ipc); puts("ipc ok"); return 0; }
