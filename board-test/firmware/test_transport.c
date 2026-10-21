#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "main/usb_transport.h"

static char source[180000], received[180000];
static size_t used, largest;
static int mode;
static int fake_usb(const void *p, size_t n, void *context) {
    (void)context;
    // 実機ドライバの丸ごと書き込みの制限を再現
    if(n>4096 || mode==1) return 0;
    if(mode==2) return -1;
    if(n>largest)largest=n;
    if(mode==3 && n>31)n=31;
    memcpy(received+used,p,n);used+=n;return (int)n;
}
int main(void) {
    memset(source,'J',sizeof(source));
    assert(fake_usb(source,sizeof(source),NULL)==0); // 前の実装だと失敗する
    assert(board_usb_send(source,sizeof(source),fake_usb,NULL));
    assert(used==sizeof(source) && largest<=512 && !memcmp(source,received,used));
    used=0;mode=3;assert(board_usb_send(source,sizeof(source),fake_usb,NULL));
    assert(used==sizeof(source) && !memcmp(source,received,used));
    mode=1;assert(!board_usb_send(source,sizeof(source),fake_usb,NULL));
    mode=2;assert(!board_usb_send(source,sizeof(source),fake_usb,NULL));
    puts("PASS: large JPEG response, partial writes, timeout, driver failure");
}
