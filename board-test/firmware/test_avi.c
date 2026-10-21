#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "main/avi_writer.h"
int main(int argc,char **argv){
    assert(argc==3);FILE *source=fopen(argv[1],"rb");assert(source);fseek(source,0,SEEK_END);long size=ftell(source);rewind(source);
    uint8_t *jpeg=malloc(size);assert(jpeg&&fread(jpeg,1,size,source)==size);fclose(source);
    FILE *out=fopen(argv[2],"wb+");assert(out);avi_index index[16];avi_writer w;assert(avi_begin(&w,out,640,480,index,16));
    for(int i=0;i<12;i++)assert(avi_append(&w,jpeg,size));assert(avi_finish(&w,100000));
    uint32_t crc,bytes;assert(avi_verify(out,&w,&crc,&bytes));
    // ディスク上のJPEGが壊れたら検出できるか
    fseek(out,240,SEEK_SET);int old=fgetc(out);fseek(out,240,SEEK_SET);fputc(old^1,out);fflush(out);
    assert(!avi_verify(out,&w,&crc,&bytes));fseek(out,240,SEEK_SET);fputc(old,out);fflush(out);
    assert(avi_verify(out,&w,&crc,&bytes));fclose(out);free(jpeg);puts("PASS: indexed AVI, JPEG readback integrity, corruption detection");
}
