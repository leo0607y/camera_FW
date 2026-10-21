#include <assert.h>
#include <stdio.h>
#include "main/gallery_files.h"
int main(void){
    bool video;unsigned n;
    assert(gallery_filename("IMG00001.JPG",&video,&n)&&!video&&n==1);
    assert(gallery_filename("VID99999.AVI",&video,&n)&&video&&n==99999);
    const char *bad[]={"../IMG01.JPG","IMG00000.JPG","IMG00001.TMP","IMG00001.JPG/","IMG00001%JPG","VID00001.JPG","IMG00001.AVI","img00001.jpg","IMG0000A.JPG","","/IMG00001.JPG"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(!gallery_filename(bad[i],NULL,NULL));
    puts("PASS: media allowlist, temporary file exclusion, traversal rejection");
}
