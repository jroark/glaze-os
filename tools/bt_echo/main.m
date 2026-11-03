/* macOS classic Bluetooth SPP endpoint. Transient service; no saved keys. */
#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>
#include <signal.h>
#include <unistd.h>
static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig; stopped=1; }
static NSDictionary *integer(unsigned size,unsigned value)
{ return @{ @"DataElementType":@1, @"DataElementSize":@(size), @"DataElementValue":@(value) }; }
static NSDictionary *service(void)
{
    return @{
        @"1":@[[IOBluetoothSDPUUID uuid16:0x1101]],
        @"4":@[
            @[[IOBluetoothSDPUUID uuid16:0x0100]],
            @[[IOBluetoothSDPUUID uuid16:0x0003],integer(1,3)]],
        @"5":@[[IOBluetoothSDPUUID uuid16:0x1002]],
        @"9":@[@[[IOBluetoothSDPUUID uuid16:0x1101],integer(2,0x0102)]],
        @"100":@"Glaze Serial Echo",
        @"LocalAttributes":@{@"Persistent":@NO}
    };
}
static NSString *normalized(NSString *s)
{ return [[s stringByReplacingOccurrencesOfString:@"-" withString:@":"] lowercaseString]; }
@interface Echo : NSObject <IOBluetoothRFCOMMChannelDelegate, IOBluetoothDevicePairDelegate>
@property(strong) IOBluetoothSDPServiceRecord *record;
@property(strong) IOBluetoothUserNotification *notification;
@property(strong) IOBluetoothRFCOMMChannel *channel;
@property(copy) NSString *allowed;
@property(strong) NSData *pin;
@property(strong) IOBluetoothDevicePair *pairing;
@property(strong) IOBluetoothUserNotification *connectionNotification;
@end
@implementation Echo
- (void)connected:(IOBluetoothUserNotification *)notification device:(IOBluetoothDevice *)device
{
    (void)notification;
    if(![normalized([device addressString]) isEqualToString:self.allowed])return;
    if(!self.pin || self.pairing)return;
    self.pairing=[IOBluetoothDevicePair pairWithDevice:device];self.pairing.delegate=self;
    IOReturn result=[self.pairing start];printf("PAIRING START 0x%x\n",result);fflush(stdout);
    if(result!=kIOReturnSuccess)self.pairing=nil;
}
- (void)devicePairingPINCodeRequest:(id)sender
{
    BluetoothPINCode code={0};memcpy(&code,[self.pin bytes],[self.pin length]);
    [sender replyPINCode:[self.pin length] PINCode:&code];memset(&code,0,sizeof(code));
    puts("PAIRING PIN supplied to macOS");fflush(stdout);
}
- (void)devicePairingFinished:(id)sender error:(IOReturn)error
{
    (void)sender;printf("PAIRING FINISHED 0x%x\n",error);fflush(stdout);self.pairing=nil;
}
- (void)opened:(IOBluetoothUserNotification *)notification channel:(IOBluetoothRFCOMMChannel *)channel
{
    (void)notification;
    NSString *peer=normalized([[channel getDevice] addressString]);
    if (![peer isEqualToString:self.allowed] || self.channel) { [channel closeChannel]; return; }
    self.channel=channel; [channel setDelegate:self];
    printf("CONNECTED %s\n",[peer UTF8String]); fflush(stdout);
}
- (void)rfcommChannelData:(IOBluetoothRFCOMMChannel *)channel data:(void *)dataPointer length:(size_t)dataLength
{
    if (channel!=self.channel) return;
    /* Keep the echo bounded, and split it at the peer's negotiated MTU. */
    if (dataLength>4096) { [channel closeChannel]; return; }
    printf("RECEIVED %zu bytes\n",dataLength);
    size_t at=0; unsigned mtu=[channel getMTU];
    if (!mtu) { [channel closeChannel]; return; }
    while (at<dataLength) {
        size_t n=MIN(dataLength-at,mtu);
        IOReturn result=[channel writeSync:(uint8_t *)dataPointer+at length:(UInt16)n];
        if (result!=kIOReturnSuccess) { fprintf(stderr,"echo failed: 0x%x\n",result); [channel closeChannel]; return; }
        at+=n;
    }
    puts("ECHOED"); fflush(stdout);
}
- (void)rfcommChannelClosed:(IOBluetoothRFCOMMChannel *)channel
{
    if (channel==self.channel) { self.channel=nil; puts("DISCONNECTED"); fflush(stdout); }
}
@end
int main(int argc,const char **argv)
{
    @autoreleasepool {
        if (argc==2 && !strcmp(argv[1],"--self-test")) {
            IOBluetoothSDPDataElement *element=[IOBluetoothSDPDataElement withElementValue:service()[@"4"]];
            if (!element || [element getTypeDescriptor]!=6 || [[element getArrayValue] count]!=2) return 1;
            puts("SPP service dictionary and RFCOMM descriptor PASS"); return 0;
        }
        if ((argc!=3 && argc!=5) || strcmp(argv[1],"--allow") || (argc==5 && strcmp(argv[3],"--pin"))) { fprintf(stderr,"usage: GlazeBluetoothEcho --allow CARD_ADDRESS [--pin TEST_PIN]\n"); return 2; }
        NSString *allowed=normalized([NSString stringWithUTF8String:argv[2]]);
        if ([allowed length]!=17) return 2;
        for (NSUInteger i=0;i<17;i++) {
            unichar ch=[allowed characterAtIndex:i];
            if (i%3==2 ? ch!=':' : !((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f'))) return 2;
        }
        IOBluetoothHostController *host=[IOBluetoothHostController defaultController];
        if (!host || [host powerState]!=kBluetoothHCIPowerStateON) { fputs("Bluetooth unavailable; enable it and grant Bluetooth access if requested.\n",stderr); return 1; }
        Echo *echo=[Echo new]; echo.allowed=allowed;
        if(argc==5){size_t length=strlen(argv[4]);if(!length || length>16)return 2;echo.pin=[NSData dataWithBytes:argv[4] length:length];
            echo.connectionNotification=[IOBluetoothDevice registerForConnectNotifications:echo selector:@selector(connected:device:)];}

        echo.record=[IOBluetoothSDPServiceRecord publishedServiceRecordWithDictionary:service()];
        BluetoothRFCOMMChannelID channel=0;
        if (!echo.record || [echo.record getRFCOMMChannelID:&channel]!=kIOReturnSuccess) { fputs("Cannot publish SPP service.\n",stderr); return 1; }
        echo.notification=[IOBluetoothRFCOMMChannel registerForChannelOpenNotifications:echo selector:@selector(opened:channel:) withChannelID:channel direction:kIOBluetoothUserNotificationChannelDirectionIncoming];
        if (!echo.notification) { [echo.record removeServiceRecord]; return 1; }
        printf("READY address=%s channel=%u allowed=%s helper=GlazeSerialEcho\n",[[host addressAsString] UTF8String],channel,[allowed UTF8String]); fflush(stdout);
        if(echo.pin)[echo connected:nil device:[IOBluetoothDevice deviceWithAddressString:allowed]];
        signal(SIGINT,stop); signal(SIGTERM,stop);
        while (!stopped) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        [echo.pairing stop]; [echo.connectionNotification unregister]; [echo.channel closeChannel]; [echo.notification unregister]; [echo.record removeServiceRecord];
    }
    return 0;
}
