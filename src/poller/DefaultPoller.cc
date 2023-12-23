#include "Poller.h"
#include "EpollPoller.h"

#include <stdlib.h>
// 获取默认的Poller实现方式
Poller* Poller::newDefaultPoller(EventLoop *loop)
{
    if (::getenv("MUDUO_USE_POLL"))
    {
        return nullptr; // 生成poll实例
    }
    else
    {
        return new EpollPoller(loop); // 生成epoll实例
    }
}