// jpgeで1行ずつ圧縮する（別のコアが行を作っている間に圧縮できる）。4:2:2はOV2640に合わせた
#include "jpeg_writer.h"
#include "jpge.h"
#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {
class callback_stream : public jpge::output_stream {
    jpeg_writer_output out;void *arg;jpge::uint size=0;
public:
    callback_stream(jpeg_writer_output out,void *arg):out(out),arg(arg){}
    bool put_buf(const void *data,int len) override{if(data&&len>0&&!out(arg,data,len))return false;size+=len>0?len:0;return true;}
    jpge::uint get_size() const override{return size;}
};
}

static bool encode(unsigned width,unsigned height,int quality,jpeg_writer_row row,void *row_arg,jpeg_writer_output out,void *out_arg);

extern "C" bool jpeg_write_rows(unsigned width,unsigned height,int quality,jpeg_writer_row row,void *row_arg,jpeg_writer_output out,void *out_arg){
    // jpgeはテーブルをグローバルに持つので同時に1枚だけ
    static StaticSemaphore_t lock_storage;static SemaphoreHandle_t lock=xSemaphoreCreateMutexStatic(&lock_storage);
    xSemaphoreTake(lock,portMAX_DELAY);
    bool ok=encode(width,height,quality,row,row_arg,out,out_arg);
    xSemaphoreGive(lock);return ok;
}
static bool encode(unsigned width,unsigned height,int quality,jpeg_writer_row row,void *row_arg,jpeg_writer_output out,void *out_arg){
    callback_stream stream(out,out_arg);
    jpge::params params;params.m_quality=quality;params.m_subsampling=jpge::H2V1;
    jpge::jpeg_encoder *encoder=new(std::nothrow) jpge::jpeg_encoder();
    if(!encoder)return false;
    bool ok=encoder->init(&stream,width,height,3,params);
    for(unsigned y=0;ok&&y<height;y++){const uint8_t *line=row(row_arg,y);ok=line&&encoder->process_scanline(line);}
    if(ok)ok=encoder->process_scanline(nullptr);
    encoder->deinit();delete encoder;
    return ok;
}
