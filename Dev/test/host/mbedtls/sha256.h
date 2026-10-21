#pragma once
// Use Windows' real SHA-256 implementation in host tests; firmware uses mbedTLS.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
inline int mbedtls_sha256(const unsigned char *data,size_t size,unsigned char *digest,int) {
    BCRYPT_ALG_HANDLE handle=nullptr;
    if (BCryptOpenAlgorithmProvider(&handle,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return -1;
    const auto status=BCryptHash(handle,nullptr,0,const_cast<PUCHAR>(data),static_cast<ULONG>(size),digest,32);
    BCryptCloseAlgorithmProvider(handle,0);
    return status<0 ? -1 : 0;
}
