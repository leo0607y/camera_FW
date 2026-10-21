#include "backup_store.h"
#include "sd_card.h"
#include "storage_lock.h"
#include <esp_heap_caps.h>
#include <cstring>

namespace {
constexpr size_t MAX_PHOTO_BYTES=2*1024*1024;
constexpr char RECEIPT[]="verified-v1\n";
bool hexDigest(const std::string &s) {
    if (s.size()!=64) return false;
    for (char c:s) if (!((c>='0' && c<='9') || (c>='a' && c<='f'))) return false;
    return true;
}
std::string receiptPath(const std::string &destination,const std::string &digest) {
    return "/.backup/"+destination+"-"+digest+".ok";
}
}

SnapshotResult nextBackupPhoto(const std::string &after, PhotoSnapshot &out) {
    out=PhotoSnapshot{};
    {
        StorageGuard guard;
        if (!isSDCardMounted()) return SnapshotResult::Unavailable;
        File root=SD.open("/");
        if (!root) return SnapshotResult::Unavailable;
        std::string next;
        for (File file=root.openNextFile(); file; file=root.openNextFile()) {
            std::string name=file.name();
            if (!name.empty() && name[0]!='/') name="/"+name;
            const bool jpeg=name.size()>4 && (name.compare(name.size()-4,4,".jpg")==0 || name.compare(name.size()-4,4,".JPG")==0);
            if (!file.isDirectory() && jpeg && name>after && (next.empty() || name<next)) next=name;
            file.close();
        }
        root.close();
        if (next.empty()) return SnapshotResult::End;
        out.source=next;
        out.generation=sdGeneration();
        File photo=SD.open(next.c_str(),FILE_READ);
        if (!photo) return SnapshotResult::Skipped;
        out.size=photo.size();
        photo.close();
        if (out.size==0 || out.size>MAX_PHOTO_BYTES) return SnapshotResult::Skipped;
    }
    out.data.reset(static_cast<uint8_t *>(heap_caps_malloc(out.size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)));
    if (!out.data) return SnapshotResult::Unavailable;
    size_t offset=0;
    while (offset<out.size) {
        {
            // 16KBごとにロックを外して撮影側に譲る。リマウントをまたいでFileを持たない
            StorageGuard guard;
            if (!isSDCardMounted() || sdGeneration()!=out.generation) return SnapshotResult::Unavailable;
            File photo=SD.open(out.source.c_str(),FILE_READ);
            if (!photo || photo.size()!=out.size || !photo.seek(offset)) {
                photo.close();out.data.reset();return SnapshotResult::Skipped;
            }
            const size_t remaining=out.size-offset;
            const size_t chunk=remaining<16384 ? remaining : 16384;
            const size_t count=photo.read(out.data.get()+offset,chunk);
            photo.close();
            if (count!=chunk) { out.data.reset();return SnapshotResult::Skipped; }
            offset+=count;
        }
        delay(1);
    }
    return SnapshotResult::Loaded;
}

bool hasBackupReceipt(const std::string &destination,const std::string &digest) {
    StorageGuard guard;
    if (!isSDCardMounted() || !hexDigest(destination) || !hexDigest(digest)) return false;
    File file=SD.open(receiptPath(destination,digest).c_str(),FILE_READ);
    char contents[sizeof(RECEIPT)]={};
    const bool ok=file && file.size()==sizeof(RECEIPT)-1 &&
        file.read(reinterpret_cast<uint8_t *>(contents),sizeof(RECEIPT)-1)==sizeof(RECEIPT)-1 &&
        std::memcmp(contents,RECEIPT,sizeof(RECEIPT)-1)==0;
    file.close();
    return ok;
}

bool writeBackupReceipt(const PhotoSnapshot &photo,const std::string &destination,const std::string &digest) {
    StorageGuard guard;
    if (!isSDCardMounted() || sdGeneration()!=photo.generation || !hexDigest(destination) || !hexDigest(digest)) return false;
    if (!SD.exists("/.backup") && !SD.mkdir("/.backup")) return false;
    const std::string path=receiptPath(destination,digest);
    if (hasBackupReceipt(destination,digest)) return true;
    const std::string temporary=path+".tmp";
    File file=SD.open(temporary.c_str(),FILE_WRITE);
    if (!file) return false;
    const size_t written=file.write(reinterpret_cast<const uint8_t *>(RECEIPT),sizeof(RECEIPT)-1);
    file.flush(); file.close();
    if (written!=sizeof(RECEIPT)-1) return false;
    if (SD.exists(path.c_str()) && !SD.remove(path.c_str())) return false;
    if (!SD.rename(temporary.c_str(),path.c_str())) return false;
    return hasBackupReceipt(destination,digest);
}
