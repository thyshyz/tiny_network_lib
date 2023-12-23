#include <semaphore.h>
#include "CurrentThread.h"
#include "Thread.h"

std::atomic_int32_t Thread::numCreated_(0);

Thread::Thread(ThreadFunc func, const std::string &name):
                started_(false),
                joined_(false),
                tid_(0),
                func_(std::move(func)),
                name_(name){
    setDefaultName();
}

Thread::~Thread(){
    if(started_ && !joined_){
        thread_->detach();
    }
}

void Thread::start(){
    started_ = true;
    sem_t sem;
    sem_init(&sem,false,0);
    thread_ = std::shared_ptr<std::thread>(new std::thread([&](){
        tid_ = CurrentThread::tid();
    
        sem_post(&sem);
        func_();
    }));

    //防止新起的线程还没有获取到tid本线程就运行到这里
    sem_wait(&sem);
}

void Thread::join(){
    joined_ = true;
    //等待线程执行完毕
    thread_->join();
}

void Thread::setDefaultName(){
    int num = ++numCreated_;
    if(name_.empty()){
        char buf[32] = {0};
        snprintf(buf,sizeof(buf),"Thread%d",num);
        name_ = buf;
    }
}