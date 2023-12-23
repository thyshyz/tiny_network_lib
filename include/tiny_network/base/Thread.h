#ifndef THREAD_H
#define THREAD_H
#include <thread>
#include <functional>
#include <memory>
#include <string>
#include <atomic>
#include "noncopyable.h"

class Thread : noncopyable{
public:
    using ThreadFunc = std::function<void()>;
    explicit Thread(ThreadFunc, const std::string &name = std::string());
    ~Thread();
    void start();
    void join();
    bool started() const {  return started_;};
    pid_t tid() const { return tid_;};
    const std::string &name(){  return name_;};
    static int numCreated(){    return numCreated_;};


private:
    bool started_;   //线程启动标志
    bool joined_;      //线程等待标志
    std::shared_ptr<std::thread> thread_;  
    pid_t tid_;     //线程tid
    //Thread::start()调用的回调函数，实际保存
    ThreadFunc func_;
    std::string name_;      //线程名
    static std::atomic_int32_t numCreated_;     //线程索引
    void setDefaultName();
};


#endif