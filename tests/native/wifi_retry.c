#include "../../servers/net/wifi_retry.h"
#include <assert.h>
#include <stdio.h>
static unsigned attempts;
static void connect(void *ctx) { assert(ctx==&attempts); attempts++; }
static void poll(struct wifi_retry *r,uint32_t now,bool enabled,bool up,bool busy,bool bad_key)
{ wifi_retry_poll(r,now,WIFI_RETRY_MAX_MS,enabled,up,busy,bad_key,connect,&attempts); }
int main(void)
{
 struct wifi_retry r;wifi_retry_reset(&r);
 poll(&r,100,false,false,false,false);poll(&r,10000,false,false,false,false);assert(!attempts);
 poll(&r,10000,true,true,false,false);assert(!attempts);
 /* A link-loss event waits two seconds, then retries the saved network. */
 poll(&r,10100,true,false,false,false);poll(&r,12099,true,false,false,false);assert(!attempts);
 poll(&r,12100,true,false,false,false);assert(attempts==1);
 /* Association owns the card: no overlapping scan/connect, even after due. */
 poll(&r,30000,true,false,true,false);assert(attempts==1);
 poll(&r,30000,true,false,false,false);assert(attempts==2);
 poll(&r,37999,true,false,false,false);assert(attempts==2);
 poll(&r,38000,true,false,false,false);assert(attempts==3);
 for(unsigned i=0;i<8;i++) {uint32_t due=r.at;poll(&r,due,true,false,false,false);assert(r.at-due<=WIFI_RETRY_MAX_MS);}
 assert(r.delay==WIFI_RETRY_MAX_MS);
 unsigned before=attempts;
 /* A rejected key is not retried; manual disconnect and forget also stop it. */
 poll(&r,r.at,true,false,false,true);poll(&r,r.at+60000,true,false,false,true);assert(attempts==before);
 poll(&r,500000,false,false,false,false);poll(&r,600000,false,false,false,false);assert(attempts==before);
 /* A recovered link starts the next outage with a short delay again. */
 poll(&r,600000,true,true,false,false);poll(&r,600100,true,false,false,false);
 assert(r.at==602100);poll(&r,602100,true,false,false,false);assert(attempts==before+1);
 /* Tick wrap preserves the wait, without a retry storm or a long stall. */
 wifi_retry_reset(&r);before=attempts;
 poll(&r,UINT32_MAX-999,true,false,false,false);assert(r.at==1000);
 poll(&r,999,true,false,false,false);assert(attempts==before);
 poll(&r,1000,true,false,false,false);assert(attempts==before+1);
 puts("wifi-retry: link loss, backoff, in-flight association, auth rejection, explicit disconnect, recovery and tick wrap PASS");
}
