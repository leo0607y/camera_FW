#pragma once
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

struct PhotoSnapshot {
    struct Free { void operator()(uint8_t *p) const { std::free(p); } };
    std::unique_ptr<uint8_t,Free> data;
    size_t size=0;
    uint32_t generation=0;
    std::string source;
};
enum class SnapshotResult { Loaded, End, Unavailable, Skipped };
SnapshotResult nextBackupPhoto(const std::string &after, PhotoSnapshot &out);
bool hasBackupReceipt(const std::string &destination, const std::string &digest);
bool writeBackupReceipt(const PhotoSnapshot &photo, const std::string &destination,
                        const std::string &digest);
