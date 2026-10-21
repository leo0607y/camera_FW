#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
typedef struct {uint32_t offset,size;} avi_index;
typedef struct {FILE *file;avi_index *index;uint32_t frames,capacity,max_frame;unsigned width,height;uint32_t payload_crc;} avi_writer;
bool avi_begin(avi_writer *w,FILE *file,unsigned width,unsigned height,avi_index *index,unsigned capacity);
bool avi_append(avi_writer *w,const uint8_t *jpeg,size_t size);
bool avi_finish(avi_writer *w,uint32_t frame_period_us);
bool avi_verify(FILE *file,const avi_writer *w,uint32_t *file_crc,uint32_t *file_size);
