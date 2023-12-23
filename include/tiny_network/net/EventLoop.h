#ifndef EVENTLOOP_H
#define EVENTLOOP_H
#include "noncopyable.h"
#include "Timestamp.h"
#include "CurrentThread.h"
#include <functional>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>

class Channel;
class Poller;

class EventLoop{
public:
    using Functor = std::function<void()>;
    EventLoop();
    ~EventLoop();
    void loop();
    void quit();
    Timestamp pollReturnTime(){ return pollReturnTime_;  }
    //在当前线程中同步调用
    void runInLoop(Functor cb);
    //将回调函数放入eventloop的等待队列，本线程唤醒该eventloop所在线程执行等待队列中的函数
    void queueInLoop(Functor cb);
    void wakeup();  //唤醒loop所在线程
    bool isInLoopThread() const {   return threadId_ == CurrentThread::tid();   }
    void updateChannel(Channel *channel);
    void removeChannel(Channel *channel);
    bool hasChannel(Channel *channel);
private:
    using ChannelList = std::vector<Channel *>;
    std::atomic_bool looping_;  // 原子操作，通过CAS实现
    std::atomic_bool quit_;     // 标志退出事件循环
    std::atomic_bool callingPendingFunctors_; // 标志当前loop是否正在执行的回调操作
    const pid_t threadId_;      // 记录当前loop所在线程的id
    Timestamp pollReturnTime_;  // poller返回发生事件的channels的返回时间
    std::unique_ptr<Poller> poller_;    //一个EventLoop对应一个poller

    int wakeupFd_;  //用于唤醒线程
    std::unique_ptr<Channel> wakeupChannel_;

    ChannelList activeChannels_;            // 活跃的Channel
    Channel* currentActiveChannel_;         // 当前处理的活跃channel
    std::mutex mutex_;                      // 用于保护pendingFunctors_线程安全操作
    std::vector<Functor> pendingFunctors_;  // 存储loop跨线程需要执行的所有回调操作
    void handleRead();
    void doPendingFunctors();
};




#endif