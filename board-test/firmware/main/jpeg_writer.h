#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef const uint8_t *(*jpeg_writer_row)(void *arg,unsigned y); // returns width*3 RGB bytes, NULL aborts
typedef bool (*jpeg_writer_output)(void *arg,const void *data,int len);
bool jpeg_write_rows(unsigned width,unsigned height,int quality,jpeg_writer_row row,void *row_arg,jpeg_writer_output out,void *out_arg);
#ifdef __cplusplus
}
#endif
