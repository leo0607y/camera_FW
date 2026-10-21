#pragma once
#include "Arduino.h"
#include "SPI.h"
#include <map>
#include <vector>
#include <algorithm>
#include <limits>
#include <cctype>
#include <functional>
#include <set>

#define FILE_WRITE "w"
#define FILE_READ "r"
constexpr int CARD_NONE=0, CARD_MMC=1, CARD_SD=2, CARD_SDHC=3;
inline std::map<std::string, std::vector<uint8_t>> mock_files;
inline std::set<std::string> mock_directories;
inline size_t mock_write_limit = std::numeric_limits<size_t>::max();
inline bool mock_mount_ok=true, mock_open_ok=true, mock_rename_ok=true;
inline bool mock_truncate_on_flush=false;
inline size_t mock_corrupt_at=std::numeric_limits<size_t>::max();
inline size_t mock_read_limit=std::numeric_limits<size_t>::max();
inline std::function<void()> mock_on_write;
inline int mock_mounts=0;
inline std::string key(const char *path) {
    std::string s(path);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}
class File {
    std::string path;
    bool valid=false;
    size_t cursor=0;
    size_t read_offset=0;
public:
    File()=default;
    explicit File(std::string p): path(p), valid(true) {}
    explicit operator bool() const { return valid; }
    bool isDirectory() const { return path == "/"; }
    const char *name() const { return path.c_str(); }
    File openNextFile() {
        if (cursor >= mock_files.size()) return {};
        auto it=mock_files.begin();
        std::advance(it, cursor++);
        return File(it->first);
    }
    size_t write(const uint8_t *data, size_t length) {
        if (mock_on_write) mock_on_write();
        size_t n=std::min(length, mock_write_limit);
        mock_files[path].assign(data, data+n);
        return n;
    }
    bool seek(size_t offset) { read_offset=offset;return offset<=mock_files.at(path).size(); }
    size_t read(uint8_t *buffer, size_t length) {
        const auto &data=mock_files.at(path);
        const size_t n=std::min({length, data.size()-read_offset, mock_read_limit});
        std::copy_n(data.begin()+read_offset, n, buffer);
        read_offset+=n;
        return n;
    }
    size_t size() const { return mock_files.at(path).size(); }
    void flush() {
        if (mock_truncate_on_flush) mock_files[path].clear();
        if (mock_corrupt_at < mock_files[path].size()) mock_files[path][mock_corrupt_at] ^= 0xff;
    }
    void close() { valid=false; }
};
struct MockSD {
    void end() {}
    bool begin(int, MockSPI &, int) { ++mock_mounts; return mock_mount_ok; }
    int cardType() { return mock_mount_ok ? CARD_SDHC : CARD_NONE; }
    uint64_t cardSize() { return 1024*1024; }
    bool exists(const char *path) { return mock_files.count(key(path)) != 0 || mock_directories.count(key(path)) != 0; }
    bool mkdir(const char *path) { mock_directories.insert(key(path)); return true; }
    File open(const char *path, const char *mode=FILE_READ) {
        if (!mock_open_ok) return {};
        std::string p=key(path);
        if (p == "/") return File(p);
        if (std::string(mode) == FILE_WRITE) mock_files[p].clear();
        if (!mock_files.count(p)) return {};
        return File(p);
    }
    bool remove(const char *path) { return mock_files.erase(key(path)) != 0; }
    bool rename(const char *from, const char *to) {
        if (!mock_rename_ok) return false;
        mock_files[key(to)]=mock_files.at(key(from));
        mock_files.erase(key(from));
        return true;
    }
};
inline MockSD SD;
