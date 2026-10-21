#pragma once
#include "network_settings.h"
#include <cstdint>
#include <cstddef>
#include <string>

std::string backupDigest(const uint8_t *data,size_t length);
std::string backupDestinationId(const NetworkSettings &settings);
// PUTしてGETで一致したらtrue
bool uploadBackup(const NetworkSettings &settings,const uint8_t *data,size_t length,
                  const std::string &digest,std::string &error);
