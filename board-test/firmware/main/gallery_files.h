#pragma once
#include <stdbool.h>
#include <string.h>
// ファームが作ったファイル名だけ通す
static inline bool gallery_filename(const char *name,bool *video,unsigned *number){
    if(strlen(name)!=12)return false;
    bool v=!memcmp(name,"VID",3);if(!v&&memcmp(name,"IMG",3))return false;
    if(strcmp(name+8,v?".AVI":".JPG"))return false;
    unsigned n=0;for(int i=3;i<8;i++){if(name[i]<'0'||name[i]>'9')return false;n=n*10+(name[i]-'0');}
    if(!n)return false;
    if(video)*video=v;
    if(number)*number=n;
    return true;
}
