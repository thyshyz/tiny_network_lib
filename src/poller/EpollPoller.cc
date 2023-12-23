#include "EpollPoller.h"
#include <string.h>

const int kNew = -1;    //channel未添加至poller
const int kAdded = 1;   //已添加
const int kDeleted = 2; //已从poller删除

EpollPoller::EpollPoller(EventLoop *loop):
            Poller(loop),
            epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
            events_(kInitEventListSize){
    if(epollfd_ < 0){
        LOG_FATAL << "epoll_create() error:" << errno;
    }
}

EpollPoller::~EpollPoller(){
    ::close(epollfd_);
}

Timestamp EpollPoller::poll(int timeoutMs, ChannelList *activeChannels){
    size_t numEvents = ::epoll_wait(epollfd_, &(*events_.begin()), static_cast<int>(events_.size()), timeoutMs);
    int saveErrno = errno;
    Timestamp now(Timestamp::now());
    if(numEvents > 0){
        fillActiveChannels(numEvents, activeChannels);
        //如果填充满了那么扩容
        if(numEvents == events_.size()){
            events_.resize(events_.size()*2);
        }
    }
    else if(numEvents == 0){
        LOG_DEBUG << "timeout";
    }
    else{   //出现错误
        if (saveErrno != EINTR){    // 且不是终端错误，那么是poll出错
            errno = saveErrno;
            LOG_ERROR << "EPollPoller::poll() failed";
        }
    }
    return now;
}

void EpollPoller::fillActiveChannels(int numEvents, ChannelList *activeChannels) const{
    for(int i = 0; i < numEvents; ++i){
        Channel *channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        activeChannels->emplace_back(channel);
    }
}

//channel::update -> eventloop::updateChannel ->EpollPoller::updateChannel
void EpollPoller::updateChannel(Channel *channel){
    const int index = channel ->index();
    //未添加或已删除则重新添加到epoll中
    if(index == kNew || index == kDeleted){
        if(index == kNew){
            int fd = channel->fd();
            channels_[fd] = channel;
        }
        channel->set_index(kAdded);
        update(EPOLL_CTL_ADD, channel);
    }
    else{ //channel已经在poller上注册过
        if(channel->isNoneEvent()){ //channel没有感兴趣的事件，可以在epoll对象中删除该channel
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(kDeleted);
        }
        else{   //还有事件存在但是调用了update，说明需要修改事件
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

void EpollPoller::removeChannel(Channel *channel)
{
    int fd = channel->fd();
    channels_.erase(fd);    //从poller的map中删除该channel
    int index = channel->index();
    if(index == kAdded){    //如果fd已经添加到epoll对象中，则从epoll对象中删除
        update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(kNew);   //channel设置为未添加
}

void EpollPoller::update(int operation, Channel *channel){
    epoll_event event;
    ::memset(&event, 0, sizeof(event));
    int fd = channel->fd();
    event.events = channel->events();
    event.data.fd = fd;
    event.data.ptr = channel;   //指针指向channel方便在poll操作之后从发生的events中找到对应的channel

    if(::epoll_ctl(epollfd_, operation, fd, &event) < 0){
        if (operation == EPOLL_CTL_DEL){
            LOG_ERROR << "epoll_ctl() del error:" << errno;
        }
        else{
            LOG_FATAL << "epoll_ctl add/mod error:" << errno;
        }
    }
}

