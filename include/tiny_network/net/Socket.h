#pragma once
#include "noncopyable.h"

class InetAddress;

class Socket : noncopyable{
public:
    explicit Socket(int sockfd):
            sockfd_(sockfd){
    }
    ~Socket();
    int fd() const{ return sockfd_; }
    //绑定
    void bindAddress(const InetAddress &localaddr);
    //监听
    void listen();
    //接受连接
    int accept(InetAddress *peeraddr);
    // 设置半关闭
    void shutdownWrite();

    void setTcpNoDelay(bool on);    // 设置Nagel算法 
    void setReuseAddr(bool on);     // 设置地址复用
    void setReusePort(bool on);     // 设置端口复用
    void setKeepAlive(bool on);     // 设置长连接

private:
    const int sockfd_;
};