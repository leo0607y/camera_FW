#include "avi_writer.h"
#include <string.h>
#include <limits.h>
static uint32_t crc_update(uint32_t crc,const void *data,size_t size){
    const uint8_t *p=data;crc=~crc;
    while(size--){crc^=*p++;for(int i=0;i<8;i++)crc=(crc>>1)^(0xedb88320u&-(crc&1));}
    return ~crc;
}
static void put32(uint8_t *p,uint32_t v){for(int i=0;i<4;i++)p[i]=v>>(8*i);}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static bool word(FILE *f,uint32_t v){uint8_t b[4];put32(b,v);return fwrite(b,1,4,f)==4;}
// Fixed 224-byte RIFF/hdrl/strl header followed by LIST movi (data at 224).
static void header(uint8_t *b,const avi_writer *w,uint32_t period,uint32_t size,uint32_t movi_size){
    memset(b,0,224);memcpy(b,"RIFF",4);put32(b+4,size-8);memcpy(b+8,"AVI LIST",8);put32(b+16,192);memcpy(b+20,"hdrlavih",8);put32(b+28,56);
    put32(b+32,period);put32(b+36,period?(uint64_t)w->max_frame*1000000/period:0);put32(b+44,0x10);put32(b+48,w->frames);put32(b+56,1);put32(b+60,w->max_frame);put32(b+64,w->width);put32(b+68,w->height);
    memcpy(b+88,"LIST",4);put32(b+92,116);memcpy(b+96,"strlstrh",8);put32(b+104,56);memcpy(b+108,"vidsMJPG",8);
    put32(b+128,period);put32(b+132,1000000);put32(b+140,w->frames);put32(b+144,w->max_frame);put32(b+148,0xffffffffu);
    b[160]=w->width;b[161]=w->width>>8;b[162]=w->height;b[163]=w->height>>8;
    memcpy(b+164,"strf",4);put32(b+168,40);put32(b+172,40);put32(b+176,w->width);put32(b+180,w->height);b[184]=1;b[186]=24;memcpy(b+188,"MJPG",4);put32(b+192,w->max_frame);
    memcpy(b+212,"LIST",4);put32(b+216,movi_size);memcpy(b+220,"movi",4);
}
bool avi_begin(avi_writer *w,FILE *file,unsigned width,unsigned height,avi_index *index,unsigned capacity){
    *w=(avi_writer){.file=file,.index=index,.capacity=capacity,.width=width,.height=height};uint8_t b[224];header(b,w,100000,224,4);return fwrite(b,1,sizeof(b),file)==sizeof(b);
}
bool avi_append(avi_writer *w,const uint8_t *jpeg,size_t size){
    if(w->frames>=w->capacity||size<4||size>UINT32_MAX||jpeg[0]!=255||jpeg[1]!=216||jpeg[size-2]!=255||jpeg[size-1]!=217)return false;
    long pos=ftell(w->file);if(pos<224||pos>256*1024*1024)return false;
    if(fwrite("00dc",1,4,w->file)!=4||!word(w->file,size)||fwrite(jpeg,1,size,w->file)!=size)return false;
    if((size&1)&&fputc(0,w->file)==EOF)return false;
    w->index[w->frames++]=(avi_index){pos-220,size};if(size>w->max_frame)w->max_frame=size;
    w->payload_crc=crc_update(w->payload_crc,jpeg,size);return true;
}
bool avi_finish(avi_writer *w,uint32_t period){
    long end=ftell(w->file);if(end<224||!w->frames||!period)return false;
    if(fwrite("idx1",1,4,w->file)!=4||!word(w->file,w->frames*16))return false;
    for(unsigned i=0;i<w->frames;i++)if(fwrite("00dc",1,4,w->file)!=4||!word(w->file,0x10)||!word(w->file,w->index[i].offset)||!word(w->file,w->index[i].size))return false;
    long size=ftell(w->file);uint8_t b[224];header(b,w,period,size,end-220);
    return fseek(w->file,0,SEEK_SET)==0&&fwrite(b,1,sizeof(b),w->file)==sizeof(b)&&fflush(w->file)==0;
}
bool avi_verify(FILE *f,const avi_writer *w,uint32_t *file_crc,uint32_t *file_size){
    uint8_t b[2048];uint32_t crc=0,total=0,payload=0;
    if(fseek(f,0,SEEK_SET))return false;
    size_t n;while((n=fread(b,1,sizeof(b),f))){crc=crc_update(crc,b,n);total+=n;}if(ferror(f))return false;
    if(fseek(f,0,SEEK_SET)||fread(b,1,224,f)!=224||memcmp(b,"RIFF",4)||get32(b+4)!=total-8||get32(b+48)!=w->frames)return false;
    for(unsigned i=0;i<w->frames;i++){
        if(fseek(f,220+w->index[i].offset,SEEK_SET)||fread(b,1,8,f)!=8||memcmp(b,"00dc",4)||get32(b+4)!=w->index[i].size)return false;
        unsigned left=w->index[i].size;while(left){n=left<sizeof(b)?left:sizeof(b);if(fread(b,1,n,f)!=n)return false;payload=crc_update(payload,b,n);left-=n;}
    }
    *file_crc=crc;*file_size=total;return payload==w->payload_crc;
}
