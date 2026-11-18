#pragma once
#include <poll.h>
#include <cstddef>
#include <map>
#include <vector>

namespace fo2net {
// Darwin poll registers only the last event mask for repeated descriptors.
// Winsock select can place one socket in three sets, so poll it once with ORed events.
struct PollSet {
    std::vector<pollfd> items;
    std::map<int,size_t> indices;
    size_t add(int fd,short events){
        auto found=indices.find(fd);
        if(found!=indices.end()){
            items[found->second].events|=events;return found->second;
        }
        size_t index=items.size();indices.emplace(fd,index);items.push_back({fd,events,0});return index;
    }
};
inline bool selectReady(unsigned set,short events,bool connectCompleted,int connectError){
    if(connectCompleted&&connectError)return set==2;
    if(set==0)return (events&(POLLIN|POLLHUP|POLLERR))!=0;
    if(set==1)return (events&POLLOUT)!=0;
    return (events&POLLPRI)!=0;
}
}
