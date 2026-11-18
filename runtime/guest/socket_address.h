#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>

// Guest addresses are x86 Winsock SOCKADDR_IN, never native sockaddr bytes.
namespace fo2net {
constexpr size_t guestIPv4Size=16;
inline socklen_t decodeAddress(const void* guest,size_t size,sockaddr_storage& native){
    if(!guest||size<guestIPv4Size)return 0;
    const auto* bytes=static_cast<const uint8_t*>(guest);
    const unsigned family=unsigned(bytes[0])|(unsigned(bytes[1])<<8);
    if(family!=2&&family!=0)return 0; // IPv4 or AF_UNSPEC (UDP disconnect)
    std::memset(&native,0,sizeof native);
    auto* out=reinterpret_cast<sockaddr_in*>(&native);
#ifdef __APPLE__
    out->sin_len=sizeof(sockaddr_in);
#endif
    out->sin_family=family==2?AF_INET:AF_UNSPEC;
    std::memcpy(&out->sin_port,bytes+2,2);
    std::memcpy(&out->sin_addr,bytes+4,4);
    return sizeof(sockaddr_in);
}
inline bool encodeAddress(void* guest,size_t capacity,const sockaddr* native,socklen_t size){
    if(!guest||capacity<guestIPv4Size||!native||size<sizeof(sockaddr_in)||
       (native->sa_family!=AF_INET&&native->sa_family!=AF_UNSPEC))return false;
    auto* bytes=static_cast<uint8_t*>(guest);
    const auto* in=reinterpret_cast<const sockaddr_in*>(native);
    std::memset(bytes,0,guestIPv4Size);
    bytes[0]=native->sa_family==AF_INET?2:0;
    std::memcpy(bytes+2,&in->sin_port,2);
    std::memcpy(bytes+4,&in->sin_addr,4);
    return true;
}
}
