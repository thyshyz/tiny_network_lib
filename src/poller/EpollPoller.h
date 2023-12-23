#ifndef EPOLLPOLLER_H
#define EPOLLPOLLER_H

#include <vector>
#include <sys/epoll.h>
#include <unistd.h>

#include "Logging.h"
#include "Poller.h"
#include "Timestamp.h"

class EpollPoller : public Poller{
    using EventList = std::vector<epoll_event>;
public:
    EpollPoller(EventLoop *Loop);
    ~EpollPoller() override;
    Timestamp poll(int timeoutMs, ChannelList *activeChannels) override;
    void updateChannel(Channel *channel) override;
    void removeChannel(Channel *channel) override;

private:
    static const int kInitEventListSize = 16;
    int epollfd_;   //epoll_create返回
    EventList events_;   //存放epoll_wait返回的所有发生事件的文件描述符
    
    void fillActiveChannels(int numEvents, ChannelList *activeChannels) const;
    void update(int opreation, Channel *channel);
};

#endif