#ifndef CHANNEL_H
#define CHANNEL_H

#include <functional>
#include <memory>
#include <sys/epoll.h>
#include "noncopyable.h"
#include "Timestamp.h"
#include "Logging.h"

//前向声明
class EventLoop;
class Timestamp;

class Channel:noncopyable{
public:
    using EventCallback = std::function<void()>;
    using ReadEventCallback =std::function<void(Timestamp)>;

    Channel (EventLoop *loop, int fd);
    ~Channel(){};
    //用于处理事件的回调函数
    void handleEvent(Timestamp receiveTime);

    // 设置回调函数对象
    void setReadCallback(ReadEventCallback cb) { readCallback_ = std::move(cb); }
    void setWriteCallback(EventCallback cb) { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb) { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb) { errorCallback_ = std::move(cb); }

    //将tcpConnection多一份引用计数，防止channel在调用来自tcpConnection的回调函数时该对象被析构
    void tie(const std::shared_ptr<void>&);
    int fd() const{  return fd_;};
    int events() const{ return events_; };
    void set_revents(int revt) {    revents_ = revt;    };

    void enableReading(){   events_ |= kReadEvent;  update();   };
    void disableReading(){   events_ &= ~kReadEvent;  update();   };
    void enableWriting(){   events_ |= kWriteEvent;  update();   };
    void disableWriting(){   events_ &= kWriteEvent;  update();   };
    void disableAll(){  events_ &= kNoneEvent; update();   };

    bool isNoneEvent() const{   return events_ == kNoneEvent;   };
    bool isWriting() const{ return events_ & kWriteEvent;   };
    bool isReading() const{ return events_ & kReadEvent;    };
    int index() const{    return index_;   };
    void set_index(int idx){    index_ = idx;   };

    //one loop per thread
    //one loop can have multiple channels
    EventLoop* onwerLoop(){ return loop_;   };
    void remove();

private:
    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

    EventLoop *loop_;
    const int fd_;
    int events_;
    int revents_;
    int index_;
    //用于延长TcpConnection的生命周期
    std::weak_ptr<void> tie_;
    bool tied_;
    ReadEventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;

    void update();
    void handleEventWithGuard(Timestamp receiveTime);
};

#endif