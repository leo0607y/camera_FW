#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef int (*board_usb_writer)(const void *, size_t, void *);

// IDFは書き込みを丸ごとキューに入れるので、4096バイトのリングより小さく分ける
static inline bool board_usb_send(const char *data, size_t size,
                                  board_usb_writer write, void *context) {
    size_t sent = 0;
    while (sent < size) {
        size_t chunk = size - sent;
        if (chunk > 512) chunk = 512;
        int n = write(data + sent, chunk, context);
        if (n <= 0 || (size_t)n > chunk) return false;
        sent += (size_t)n;
    }
    return true;
}
