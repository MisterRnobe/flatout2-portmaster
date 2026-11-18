#include "socket_address.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <string>

static void require(bool ok,const char* what){if(!ok)throw std::runtime_error(std::string(what)+": "+std::strerror(errno));}
struct Socket {
    int fd;
    explicit Socket(int type):fd(::socket(AF_INET,type,0)){require(fd>=0,"socket");
        timeval timeout{3,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);}
    ~Socket(){if(fd>=0)::close(fd);}
    Socket(const Socket&)=delete;
};
static sockaddr_storage address(uint16_t port){
    const uint8_t bytes[16]={2,0,uint8_t(port>>8),uint8_t(port),127,0,0,1};
    sockaddr_storage out{};require(fo2net::decodeAddress(bytes,16,out)==16,"decode loopback");return out;
}
static sockaddr_storage local(int fd){sockaddr_storage out{};socklen_t size=sizeof out;
    require(getsockname(fd,reinterpret_cast<sockaddr*>(&out),&size)==0,"getsockname");return out;}
static sockaddr_storage roundtrip(const sockaddr_storage& native){
    std::array<uint8_t,24> guest;guest.fill(0xa5);
    require(fo2net::encodeAddress(guest.data()+4,16,reinterpret_cast<const sockaddr*>(&native),16),"encode address");
    require(guest[4]==2&&guest[5]==0,"Winsock family");
    for(unsigned i=0;i<4;++i)require(guest[i]==0xa5&&guest[i+20]==0xa5,"address guards");
    sockaddr_storage out{};require(fo2net::decodeAddress(guest.data()+4,16,out)==16,"decode address");return out;
}
static void offline(){
    uint8_t guest[16]={2,0,0x70,0xee,192,0,2,7};sockaddr_storage native{};
    require(fo2net::decodeAddress(guest,16,native)==16,"golden IPv4 decode");
    const auto& in=*reinterpret_cast<const sockaddr_in*>(&native);
    require(in.sin_family==AF_INET&&ntohs(in.sin_port)==28910&&ntohl(in.sin_addr.s_addr)==0xc0000207,"IPv4 fields");
#ifdef __APPLE__
    require(in.sin_len==16,"Darwin sin_len");
#endif
    auto back=roundtrip(native);require(std::memcmp(&native,&back,16)==0,"roundtrip");
    require(!fo2net::decodeAddress(guest,15,back),"short input rejected");
    guest[0]=23;require(!fo2net::decodeAddress(guest,16,back),"unsupported family rejected");
    guest[0]=0;require(fo2net::decodeAddress(guest,16,back)==16&&back.ss_family==AF_UNSPEC,"UDP AF_UNSPEC");
    std::array<uint8_t,16> guard;guard.fill(0x5a);
    require(!fo2net::encodeAddress(guard.data(),15,reinterpret_cast<const sockaddr*>(&native),16),"short output rejected");
    for(auto byte:guard)require(byte==0x5a,"short output untouched");
    require(!fo2net::encodeAddress(guard.data(),16,reinterpret_cast<const sockaddr*>(&native),15),"short native input rejected");
    std::puts("PASS address ABI: family/length, IPv4, port, AF_UNSPEC, roundtrip, sizes and guards");
}
static void loopback(){
    Socket server(SOCK_STREAM),client(SOCK_STREAM);auto bindAddress=address(0);
    require(bind(server.fd,reinterpret_cast<sockaddr*>(&bindAddress),16)==0,"TCP bind");
    require(listen(server.fd,1)==0,"listen");auto target=roundtrip(local(server.fd));
    require(connect(client.fd,reinterpret_cast<sockaddr*>(&target),16)==0,"TCP connect");
    sockaddr_storage peer{};socklen_t length=sizeof peer;int accepted=accept(server.fd,reinterpret_cast<sockaddr*>(&peer),&length);
    require(accepted>=0,"accept");roundtrip(peer);
    const char data[]="fo2-tcp";char reply[sizeof data]{};
    require(send(client.fd,data,sizeof data,0)==sizeof data,"TCP send");
    require(recv(accepted,reply,sizeof reply,MSG_WAITALL)==sizeof reply&&!std::memcmp(data,reply,sizeof data),"TCP receive");
    close(accepted);
    Socket udpServer(SOCK_DGRAM),udpClient(SOCK_DGRAM);
    require(bind(udpServer.fd,reinterpret_cast<sockaddr*>(&bindAddress),16)==0,"UDP bind");target=roundtrip(local(udpServer.fd));
    require(sendto(udpClient.fd,data,sizeof data,0,reinterpret_cast<sockaddr*>(&target),16)==sizeof data,"UDP sendto");
    length=sizeof peer;
    require(recvfrom(udpServer.fd,reply,sizeof reply,0,reinterpret_cast<sockaddr*>(&peer),&length)==sizeof reply,"UDP recvfrom");
    auto response=roundtrip(peer);
    require(sendto(udpServer.fd,reply,sizeof reply,0,reinterpret_cast<sockaddr*>(&response),16)==sizeof reply,"UDP reply sendto");
    require(recvfrom(udpClient.fd,reply,sizeof reply,0,nullptr,nullptr)==sizeof reply&&!std::memcmp(data,reply,sizeof data),"UDP reply recvfrom");
    std::puts("PASS TCP/UDP loopback through guest address conversions, including ephemeral ports and returned peer addresses");
}
static void openspy(){
    addrinfo hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;addrinfo* entries=nullptr;
    int status=getaddrinfo("flatout2pc.ms8.openspy.net","28910",&hints,&entries);
    if(status)throw std::runtime_error(std::string("OpenSpy DNS: ")+gai_strerror(status));
    sockaddr_storage native{};std::memcpy(&native,entries->ai_addr,entries->ai_addrlen);freeaddrinfo(entries);
    auto target=roundtrip(native);Socket connection(SOCK_STREAM);
    require(connect(connection.fd,reinterpret_cast<sockaddr*>(&target),16)==0,"OpenSpy TCP connect");
    char text[INET_ADDRSTRLEN]{};auto* in=reinterpret_cast<sockaddr_in*>(&target);inet_ntop(AF_INET,&in->sin_addr,text,sizeof text);
    std::printf("PASS OpenSpy TCP handshake %s:%u (server-list protocol/join not tested)\n",text,ntohs(in->sin_port));
}
int main(int argc,char**argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    bool failed=false;
    auto check=[&](void (*test)()){
        try{test();}catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());failed=true;}
    };
    check(offline);
    if(argc>1&&std::string(argv[1])!="--offline")check(loopback);
    if(argc>1&&std::string(argv[1])=="--openspy")check(openspy);
    return failed?1:0;
}
