# 网络程序设计期末大作业——网络并发处理

## 项目总览
项目地址：https://github.com/thyshyz/tiny_network_lib

本项目参考了[muduo网络库](https://github.com/chenshuo/muduo)，实现了基于Reactor的多线程网络库，使用C++ 11标准，不依赖第三方库。项目具有以下特点：
- 项目底层使用了Epoll+LT模式的非阻塞I/O复用模型。
- 采用eventfd作为事件通知机制，避免使用信号，能够高效的唤醒其他线程执行异步任务。
- 使用‘One loop per thread’的线程模型，每个线程独立运行一个事件循环，并且实现了一个线程池来减少线程创建和销毁所带来的性能损失。
- 基于自实现的双缓冲区实现异步日志，由后端线程负责定时向磁盘写入前端日志信息，避免数据落盘时阻塞网络服务。
- 基于红黑树实现定时器管理结构，内部使用 Linux 的 timerfd 通知到期任务，高效管理定时任务。
- 使用智能指针管理内存数据，避免直接分配堆内存，采用RAII的方式防止内存泄漏。

## 开发环境
- 操作系统：`Ubuntu 20.04.1 LTS`
- 编译器：`g++ 9.4.0`
- 编辑器：`vscode`
- 项目构建：`cmake 3.16.3`

## 构建项目并快速使用
```
mkdir build
cd build
cmake ..
make install
cd ..
cd example
./echoServer

另起一个终端:
nc 127.0.0.1 8080
hello
```

## 模块说明
### 1. Buffer模块
#### 设计缓冲区的原因
- 非阻塞网络编程中应用层缓冲区是必要的，因为非阻塞IO的核心思想是避免阻塞在`read()`，`write()`等`I/O`系统调用上，这样可以最大限度利用线程，让一个线程能服务于多个`socket`连接。`I/O`线程只能阻塞在`select()/poll()/epoll_wait()`等函数上。因此每个`TCP socket`都要有`inputBuffer`和`outputBuffer`。
- Output buffer的作用是使程序在`write()`操作上不会产生阻塞，当`write()`操作后，操作系统一次性没有接受完时，网络库把剩余数据则放入`outputBuffer`中，然后注册`POLLOUT`事件，一旦`socket fd`变得可写，则立刻调用`write()`进行写入数据。
- Input buffer的作用是：发送方`send`数据后，接收方收到数据不一定是整个的数据，网络库在处理`socket`可读事件的时候，必须一次性把`socket`里的数据读完，否则会反复触发`POLLIN`事件，造成`busy-loop`。所以网路库为了应对数据不完整的情况，收到的数据先放到`inputBuffer`里。

#### 缓冲区的设计
作为应用层，TcpConnection类应该具有input buffer和output buffer两个缓冲区，两个缓冲区都是Buffer类对象。Buffer类的设计有以下特点：
- 内部使用`vector<char>`保存数据，通过类似于指针的方式进行读写操作。
- Buffer对象的内部空间人为划分为3个部分，分别为头部的预留空间（用于保存一些关于BUffer的元数据）、可读区域以及可写区域。区域的划分通过索引实现：从分配空间的起始到readIndex为头部预留空间，从readIndex到writeIndex为可读空间，从writeIndex到分配空间的末尾为可写空间。
- 输入时socket首先写入input buffer，然后用户从input buffer中读取写入到数据；输出时用户首先将数据写入output buffer，然后socket从output buffer中读入数据。
- 由于底层数据存储采用`vector`，当整体空间不足时会考虑扩容，可能会导致原本的指针或者迭代器失效，因此这里使用类似于指针的整数索引来实现区域划分。
- 每次向Buffer中写入数据，`writeIndex`向后移动，可写空间减少。每次读取数据，`readIndex`向后移动，可读空间减少。

#### Buffer类的成员
```cpp
class Buffer{
public:
    //buffer前的保留空间，初始8字节，用于记录长度等信息
    static const int InitialPrependSize = 8;
    //初始可读写空间
    static const int InitialSize = 1024;
    //构造函数
    explicit Buffer(size_t initialSize = InitialSize)
        :   buffer_(InitialPrependSize + initialSize),
            readIndex_(InitialPrependSize),
            writeIndex_(InitialPrependSize){

            }
    
    size_t readableBytes() const{
        return writeIndex_ - readIndex_;
    } 

    size_t writeableBytes() const{
        return size_() - writeIndex_;
    }  

    size_t prependableBytes() const{
        return readIndex_;
    }

    //返回Buffer中起始可读区域
    const char *peek() const{
        return begin_()+readIndex_;
    }

    void retrieveUtil(const char *end){
        retrieve(end-peek());
    }

    //从buffer中取出string之后对readindex和writeindex进行置位
    void retrieve(int len){
        //没有读完
        if(len < readableBytes()){
            readIndex_ += len;
        }
        else{
            retrieveAll();
        }
    }

    //全部读完，readindex和writeindex全部回到初始的initialprependsize处
    void retrieveAll(){
        readIndex_ = InitialPrependSize;
        writeIndex_ = InitialPrependSize;
    }

    //将buffer数据全部作为string返回，并且置位
    std::string retrieveAllAsString(){
        return retrieveAsString(readableBytes());
    }

    //将buffer数据全部作为string返回，并且置位
    std::string retrieveAsString(size_t len){
        std::string res(peek(),len);
        retrieve(len);
        return res;
    }

    ssize_t readFd(int fd, int *saveErrno);
    ssize_t writeFd(int fd, int *saveErrno);

private:
    std::vector<char> buffer_;
    size_t readIndex_;
    size_t writeIndex_;
};
```
#### 向Buffer写入数据
`readFd(int fd, int *saveErrno)`代表从fd中读取数据到buffer_中。对于buffer来说这是写入数据的操作，会改变`writeIndex`。

1. 考虑到 buffer_ 的 writableBytes 空间大小，不能够一次性读完数据，于是内部还在栈上创建了一个临时缓冲区 `char extrabuf[65536];`。如果有多余的数据，就将其读入到临时缓冲区中。如果可写区域比栈空间大，那么就不用栈空间接受.
2. 因为可能要写入两个缓冲区，所以使用了更加高效`readv`函数，可以向多个地址写入数据。刚开始会判断需要写入的大小。
   - 如果一个缓冲区足够，就不必再往临时缓冲区`extrabuf`写入数据了。写入后需要更新`writeIndex`位置，`writerIndex_ += n;`。
   - 如果一个缓冲区不够，则还需往临时缓冲区`extrabuf`写入数据。原缓冲区直接写满，`writeIndex_ = buffer_.size()`。然后往临时缓冲区写入数据，`append(extrabuf, n - writable);`。
   - 如果还是没有读完那么就等待下一次触发读事件继续从socket fd中读取数据到缓冲区。

```cpp
//从fd上读取数据到buffer上，然后用户再从buffer读走数据
//从socket读到buffer上的方法是使用readv先读到buffer上
//如果buffer空间不够就用一个栈上的空间额外接受
//然后append到buffer上
ssize_t Buffer::readFd(int fd, int *saveErrno){
    char extrabuf[65536] = {0};
    //iovec.iov_base表示用于读/写的起始位置
    //iovec.iov_len表示这段空间的可读/写大小
    struct iovec vec[2];
    const size_t writeablebytes = writeableBytes();
    vec[0].iov_base = begin_() + writeIndex_;
    vec[0].iov_len = writeablebytes;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof(extrabuf);

    //如果可写区域比栈空间大，那么就不用栈空间接受
    //同样readv读数据是有先后读，如果在buffer上读完了那么就不会读到栈空间上
    const int iovcnt = (writeablebytes < sizeof(extrabuf)) ? 2:1;
    const ssize_t n = ::readv(fd, vec, iovcnt);

    if(n < 0){
        *saveErrno = errno;
    }
    else if(n <= writeablebytes){
        writeIndex_ += n;
    }
    else{
        writeIndex_ = buffer_.size();
        append(extrabuf,n-writeablebytes);
    }
    return n;
}
```
如果buffer_空间不足，数据有一部分读到了栈空间`extrabuf`上，那么调用`append`函数将栈空间上读到的数据复制到buffer_上
```cpp
    //将string添加到缓冲区
    void append(const std::string &str){
        append(str.data(),str.size());
    }

    //将[data,data+len]的数据添加到缓冲区，置位writeindex
    void append(const char *data,size_t len){
        ensureWriteableBytes(len);
        std::copy(data,data+len,beginWrite());
        writeIndex_ += len;
    }
```
如果从栈空间`extrabuf`复制到buffer_上时可写空间不足，那么我们需要对缓冲区进行调整
- 由于每次读取数据都会使得`readIndex`向后移动，因此一般来说头部的预留空间不止一开始的8字节，如果在头部能够保留至少8字节的情况下将整体缓冲区的可读和可写空间向前移动，且移动后可写空间能够接收写入的数据，那么就不需要重新分配空间，只需要简单的移动数据即可。
- 如果移动数据还是无法获得足够的可写空间，那么就将底层的`vector<char>`扩容，由于`vector`扩容后可能已经离开了原有的内存地址，因此扩容后原本的指针和迭代器可能失效，因此底层仅采用整数索引来划分空间。扩容之后可写空间增加。
```cpp
    void ensureWriteableBytes(size_t len){
        if(writeableBytes() < len){
            resize_(len);
        }
    }

    void resize_(int len){
    //一定要保留最开始8个字节的情况下，如果将可读写区域向前提还不够的话，
    //那么就扩容到不用提前可读写区域位置正好能写下的大小
    if(prependableBytes() + writeableBytes() < len + InitialPrependSize){
        buffer_.resize(writeIndex_ + len);
    }
    else{
    //否则就保留一开始8字节，将可读写区域向前提
        size_t readablebytes = readableBytes();
        std::copy(begin_() + readIndex_,
                begin_() + writeIndex_,
                begin_() + InitialPrependSize);
        readIndex_ = InitialPrependSize;
        writeIndex_ = readablebytes + readIndex_;
    }
}
```
#### 从Buffer读取数据
读取数据会直接或者间接调用`retrieve(size_t len)`函数，如果len小于可读空间长度则直接读出，然后`readIndex`向后len个字节即可；如果大于等于空间长度则说明要全部读出，那么将buffer置为初始状态：`readIndex`和`writeIndex`都回到头部预留空间的末尾。
```cpp
    //将buffer数据全部作为string返回，并且置位
    std::string retrieveAllAsString(){
        return retrieveAsString(readableBytes());
    }

    //将buffer数据全部作为string返回，并且置位
    std::string retrieveAsString(size_t len){
        std::string res(peek(),len);
        retrieve(len);
        return res;
    }

        //从buffer中取出string之后对readindex和writeindex进行置位
    void retrieve(int len){
        //没有读完
        if(len < readableBytes()){
            readIndex_ += len;
        }
        else{
            retrieveAll();
        }
    }

    //全部读完，readindex和writeindex全部回到初始的initialprependsize处
    void retrieveAll(){
        readIndex_ = InitialPrependSize;
        writeIndex_ = InitialPrependSize;
    }
```
#### TcpConnection使用Buffer
TcpConnection 拥有 inputBuffer 和 outputBuffer 两个缓冲区成员。

1. 当服务端接收客户端数据，EventLoop 返回发生事件的 Channel，并调用对应的读事件处理函数，即 TcpConnection 调用 handleRead 方法从相应的 fd 中读取数据到 inputBuffer 中。在 Buffer 内部 inputBuffer 中的 writeIndex 向后移动。
2. 当服务端向客户端发送数据，TcpConnection 调用 handleWrite 方法将 outputBuffer 的数据写入到 TCP 发送缓冲区。outputBuffer 内部调用 `retrieve` 方法移动 readIndex 索引。

```cpp
void TcpConnection::handleRead(Timestamp receiveTime){
    int savedErrno = 0;
    // TcpConnection会从socket读取数据，然后写入inpuBuffer
    ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
    if (n > 0){
        // 已建立连接的用户，有可读事件发生，调用用户传入的回调操作
        messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
    }
    else if (n == 0){
        // 没有数据，说明客户端关闭连接
        handleClose();
    }
    else{
        // 出错情况
        errno = savedErrno;
        LOG_ERROR << "TcpConnection::handleRead() failed";
        handleError();
    }
}

void TcpConnection::handleWrite(){
    if (channel_->isWriting()){
        int saveErrno = 0;
        ssize_t n = outputBuffer_.writeFd(channel_->fd(), &saveErrno);
        // 正确读取数据
        if (n > 0)
        {
            outputBuffer_.retrieve(n);
            // 说明buffer可读数据都被TcpConnection读取完毕并写入给了客户端
            // 此时就可以关闭连接，否则还需继续提醒写事件
            if (outputBuffer_.readableBytes() == 0){
                channel_->disableWriting();
                // 调用用户自定义的写完数据处理函数
                if (writeCompleteCallback_){
                    // 唤醒loop_对应得thread线程，执行写完成事件回调
                    loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
                }
                if (state_ == kDisconnecting){
                    shutdownInLoop();
                }
            }
        }
        else{
            LOG_ERROR << "TcpConnection::handleWrite() failed";
        }
    }
    // state_不为写状态
    else{
        LOG_ERROR << "TcpConnection fd=" << channel_->fd() << " is down, no more writing";
    }
}
```

### 2. Channel模块
#### 设计Channel类的原因
Channel 对文件描述符和事件进行了一层封装，Channel 类将文件描述符和其感兴趣的事件（需要监听的事件）封装到了一起。而事件监听相关的代码放到了 Poller/EPollPoller 类中。
#### Channel类基本成员
```cpp
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
```
下面说明其中一些数据成员的用途：
- `int fd_`：这个Channel对象照看的文件描述符
- `int events_`：代表fd感兴趣的事件类型集合
- `int revents_`：代表事件监听器实际监听到该fd发生的事件类型集合。
- `EventLoop* loop_`：这个 Channel 属于哪个EventLoop对象，本项目仿照 muduo 采用的是 one loop per thread 模型，所以我们有不止一个 EventLoop。我们的 mainLoop 接收新连接，将新连接相关事件注册到线程池中的某一线程的 subLoop 上（轮询）。我们不希望跨线程的处理函数，所以每个 Channel 都需要记录是哪个 EventLoop 在处理自己的事情。
- `read_callback_` 、`write_callback_`、`close_callback_`、`error_callback_`：这些是 std::function 类型，代表着这个Channel为这个文件描述符保存的各事件类型发生时的处理函数。
- `index `：我们使用 index 来记录 channel 与 Poller 相关的几种状态，Poller 类会判断当前 channel 的状态然后处理不同的事情。
   - `kNew`：是否还未被poll监视 
   - `kAdded`：是否已在被监视中 
   - `kDeleted`：是否已被移除
- `kNoneEvent`、`kReadEvent`、`kWriteEvent`：事件状态设置会使用的变量

#### 向Poller更新Channel
设置好该 Channel 的监视事件的类型，调用 update 私有函数向 Poller 注册。最终在poller调用 epoll_ctl完成注册
```cpp
void Channel::update()
{
    // 通过该channel所属的EventLoop，调用poller对应的方法，注册fd的events事件
    loop_->updateChannel(this);
}

void EventLoop::updateChannel(Channel *channel){
    poller_->updateChannel(channel);
}

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
```

#### 移除Channel
```cpp
void Channel::remove(){
    loop_->removeChannel(this);
}

void EventLoop::removeChannel(Channel *channel){
    poller_->removeChannel(channel);
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
```

#### 通过Shared_ptr增加TcpConnection对象的生命周期
使用网络库的时候，会利用到TcpConnection，且该对象对用户是可见的，需要防止用户注册了要监视的事件和处理的回调函数，并在处理 subLoop 处理过程中误删了TcpConnection。实现方案是在处理事件时，如果对被调用了`tie()`方法的Channel对象，我们让一个共享型智能指针指向它，在处理事件期间延长它的生命周期。哪怕外面误删了此对象，也会因为多出来的引用计数而避免销毁操作，在TcpConnection建立的时候调用回调函数，传递的是 this 指针，所以是在 Channel 的内部增加对 TcpConnection 对象的引用计数。
```cpp
void Channel::tie(const std::shared_ptr<void> &obj){
    tie_ = obj;
    tied_ = true;
}

void Channel::handleEvent(Timestamp receiveTime){
    if(tied_){
        //增加一个guard引用计数，防止Channel在调用tcpconnect的回调函数的时候tcpconnection对象被删除
        std::shared_ptr<void> guard = tie_.lock();
        if(guard){
            handleEventWithGuard(receiveTime);
        }
    }
    else{
        handleEventWithGuard(receiveTime);
    }
}
```

#### 执行回调函数
Channel里面保存了回调函数，这些都是在对应的事件下被调用的。用户提前设置好事件的回调函数，并绑定到Channel的成员里。等到事件发生时，Channel调用事件处理方法。借由回调操作实现了异步的操作。
```cpp
void Channel::handleEventWithGuard(Timestamp receiveTime){
    // 对端关闭事件
    // 当TcpConnection对应Channel，通过shutdown关闭写端，epoll触发EPOLLHUP
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)){
        // 确认是否拥有回调函数
        if (closeCallback_)
        {
            closeCallback_();
        }
    }

    // 错误事件
    if (revents_ & (EPOLLERR)){
        LOG_ERROR << "the fd = " << this->fd();
        if (errorCallback_)
        {
            errorCallback_();
        }
    }

    // 读事件
    if (revents_ & (EPOLLIN | EPOLLPRI)){
        LOG_DEBUG << "channel have read events, the fd = " << this->fd();
        if (readCallback_)
        {
            LOG_DEBUG << "channel call the readCallback_(), the fd = " << this->fd();
            readCallback_(receiveTime);
        }
    }

    // 写事件
    if (revents_ & EPOLLOUT){
        if (writeCallback_)
        {
            writeCallback_();
        }
    }
}
```

### 3. Poller模块
#### Poller模块的基类
编写网络编程代码的时候少不了使用IO复用系列函数，网络库也为提供了对此的封装。muduo 有 Poller 和 EPollPoller 类分别对应着`epoll`和`poll`。
而我们使用的接口是`Poller`，muduo 以Poller 为虚基类，派生出 Poller 和 EPollPoller 两个子类，用不同的形式实现 IO 复用。这里我们仅实现了EpollPoller子类。
```cpp
class Poller:noncopyable{
public:
    using ChannelList = std::vector<Channel*>;
    Poller(EventLoop *Loop);
    virtual ~Poller() = default;
    virtual Timestamp poll(int timeoutMs, ChannelList *activeChannels) = 0;
    virtual void updateChannel(Channel *channel) = 0;
    virtual void removeChannel(Channel *channel) = 0;
    bool hasChannel(Channel *channel) const;

    static Poller *newDefaultPoller(EventLoop *Loop);

protected:
    using ChannelMap = std::unordered_map<int, Channel *>;
    ChannelMap channels_;

private:
    EventLoop *ownerLoop_;
};
```

- `ChannelMap channels_` 需要存储从 fd -> channel 的映射
- `ownerLoop_` 定义 Poller 所属的事件循环 EventLoop，一个EventLoop对应一个Poller


#### EpollPoller的设计
```cpp
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
```
- `kInitEventListSize` 默监听事件的数量
- `epollfd_` epoll_create 返回的指向 epoll 对象的文件描述符
- `EventList events_` 返回事件的数组

`poll`方法内部调用 epoll_wait 获取发生的事件，并找到这些事件对应的 Channel 并将这些活跃的 Channel 填充入 activeChannels 中，最后返回一个时间戳。 通过 numEvents 的值判断事件情况。
```cpp
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
```

#### 填充活跃的连接
```cpp
void EpollPoller::fillActiveChannels(int numEvents, ChannelList *activeChannels) const{
    for(int i = 0; i < numEvents; ++i){
        Channel *channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        activeChannels->emplace_back(channel);
    }
}
```
通过 epollwait 能够获得发生了事件的 events_ 数组，其中数组元素的定义如下所示：
```cpp
typedef union epoll_data
{
  void *ptr;
  int fd;
  uint32_t u32;
  uint64_t u64;
} epoll_data_t;

struct epoll_event
{
  uint32_t events;	/* Epoll events */
  epoll_data_t data;	/* User data variable */
} __EPOLL_PACKED;
```
一般的网络库会需要epoll_data_t中的 int fd 数据，但是我们已经在Channel对象中存储了，现在我们需要通过events_数组元素找到对应的Channel对象，那么可以通过将epoll_data_t中的void *ptr指向对应的Channel即可，这一步要在update的时候完成。这样调用`epoll_wait`之后就可以通过events_数组找到活跃的Channel对象了。

#### 更新Channel的状态
我们获取 channel 在 EPollPoller 上的状态，根据状态进行不同操作。最后调用 update 私有方法。

- 如果此 channel 还没有被添加到 epoll 上或者是之前已经被 epoll 上注销，那么此 channel 接下来会进行添加操作`index == kNew || index == kDeleted`
   - 如果是未添加状态，则需要在 map 上增加此 channel
   - 设置 channel 状态为 `kAdded` ，然后调用 `update(EPOLL_CTL_ADD, channel);`
- 如果已经在 poller 上注册的状态，则要进行删除或修改操作，需要判断此 channel 是否还有监视的事情（是否还要事件要等着处理）
   - 如果没有则直接删除，调用 `update(EPOLL_CTL_DEL, channel);` 并重新设置状态为 `kDeleted`
   - 如果还有要监视的事情，则说明要进行修改（MOD）操作，调用 `update(EPOLL_CTL_MOD, channel);`

`update`方法本质上就是调用epoll_ctl，将channel关注的事件注册到epoll上，但是这里有一个特殊的操作是让event.data.ptr指向对应的Channel对象，这样调用`epoll_wait`之后才能将事件关联到Channel。
```cpp
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
```

#### 从epoll移除关注的事件
```cpp
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
```

### 4. EventLoop模块
#### EventLoop类基本成员
EventLoop对应事件循环，驱动着Reactor模型，Channel和Poller类是不直接联系的，而是靠EventLoop对象调用。其实 EventLoop 也就是 `Reactor`模型的一个实例，其重点在于循环调用 `epoll_wait` 不断的监听发生的事件，然后调用处理这些对应事件的函数。这里也涉及了线程之间的通信机制:通过eventfd。

```cpp
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
```
1. `wakeupFd_`：如果需要唤醒某个`EventLoop`执行异步操作，就向其`wakeupFd_`写入数据。
2. `activeChannels_`：调用`poller_->poll`时会得到发生了事件的`Channel`，会将其储存到`activeChannels_`中。
3. `pendingFunctors_`：如果涉及跨线程调用函数时，会将函数储存到`pendingFunctors_`这个任务队列中。然后I/O线程通过eventfd唤醒该线程，执行队列中的函数。

#### 判断是否跨线程调用函数
本网络库是主从`EventLoop`模型，主`EventLoop`负责监听连接，然后通过轮询方法将新连接分派到某个从`EventLoop`上进行维护。与muduo一致，本网络库遵从`one loop per thread`的模式，每个线程中只有一个EventLoop，每个EventLoop被创建时都会保存其所在线程的线程值。
```cpp
bool isInLoopThread() const {   return threadId_ == CurrentThread::tid();   }

namespace CurrentThread{
    extern __thread int cachedTid;
    void cacheTid();
    inline int tid(){
        if(__builtin_expect(cachedTid == 0, 0)){
            cacheTid();
        }
        return cachedTid;
    }
}

namespace CurrentThread{
    __thread int cachedTid = 0;

    void cacheTid(){
        if(cachedTid == 0){
            cachedTid = static_cast<pid_t>(::syscall(SYS_gettid));
        }
    }
}
```

#### EventLoop的创建
为了防止一个线程创建多个EventLoop，程序会首先将`t_loopInThisThread`置为空指针，该变量是对线程独立的，只有第一次创建的时候才会将创建完成的this指针赋给`t_loopInThisThread`，非第一次创建则会失败。同时注意到生成EventLoop对象会创建一个wakeupFd_，这是用于通知所在线程事件发生的，所以在构造函数中给`wakeupChannel_`设置了读事件的回调函数。

```cpp
// 防止一个线程创建多个EventLoop (thread_local)
__thread EventLoop *t_loopInThisThread = nullptr;

// 定义默认的Poller IO复用接口的超时时间
const int kPollTimeMs = 10000;

int createEventfd(){
    int evfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evfd < 0){
        LOG_FATAL << "eventfd error: " << errno;
    }
    return evfd;
} 

EventLoop::EventLoop() : 
    looping_(false),
    quit_(false),
    callingPendingFunctors_(false),
    threadId_(CurrentThread::tid()),
    poller_(Poller::newDefaultPoller(this)),
    wakeupFd_(createEventfd()),
    wakeupChannel_(new Channel(this, wakeupFd_)),
    currentActiveChannel_(nullptr){
    LOG_DEBUG << "EventLoop created " << this << " the index is " << threadId_;
    LOG_DEBUG << "EventLoop created wakeupFd " << wakeupChannel_->fd();
    if (t_loopInThisThread){
        LOG_FATAL << "Another EventLoop" << t_loopInThisThread << " exists in this thread " << threadId_;
    }
    else{
        t_loopInThisThread = this;
    }

    // 设置wakeupfd的事件类型以及发生事件的回调函数
    wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleRead, this));
    // 每一个EventLoop都将监听wakeupChannel的EPOLLIN事件
    wakeupChannel_->enableReading();
}
```

#### EventLoop的析构
```cpp
EventLoop::~EventLoop()
{
    // channel移除所有感兴趣事件
    wakeupChannel_->disableAll();
    // 将channel从EventLoop中删除
    wakeupChannel_->remove();
    // 关闭 wakeupFd_
    ::close(wakeupFd_);
    // 指向EventLoop指针为空
    t_loopInThisThread = nullptr;
}
```

#### EventLoop启动循环
调用 EventLoop.loop() 正式开启事件循环，其内部会调用 `Poller::poll -> ::epoll_wait`正式等待活跃的事件发生，然后处理这些事件。

1. 调用 `poller_->poll(kPollTimeMs, &activeChannels_)` 将活跃的 Channel 填充到 activeChannels 容器中。
2. 遍历 activeChannels 调用各个事件的回调函数
3. 调用 `doPengdingFunctiors()`处理跨线程调用的回调函数

```cpp
void EventLoop::loop(){
    looping_ = true;
    quit_ = false;

    LOG_INFO << "EventLoop " << this << " start looping";

    while (!quit_){
        // 清空activeChannels_
        activeChannels_.clear();
        // 获取
        pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
        for (Channel *channel : activeChannels_){
            channel->handleEvent(pollReturnTime_);
        }
        // 执行当前EventLoop事件循环需要处理的回调操作
        /**
         * IO thread：mainLoop accept fd 打包成 chennel 分发给 subLoop
         * mainLoop实现注册一个回调，交给subLoop来执行，wakeup subLoop 之后，让其执行注册的回调操作
         * 这些回调函数在 std::vector<Functor> pendingFunctors_; 之中
         */
        doPendingFunctors();
    }
    looping_ = false;    
}
```

#### EventLoop决定在哪个线程执行任务
EventLoop 使用 `runInLoop(Functor cb)`函数执行任务，传入参数是一个回调函数，让此 EventLoop 去执行任务，可跨线程调用。一般为了保证线程安全，我们可能会使用互斥锁之类的手段来保证线程同步。但是，互斥锁的粗粒度难以把握，如果锁的范围很大，各个线程频繁争抢锁执行任务会大大拖慢网络效率。

而本网络库的处理方法是，保证各个任务在其原有得线程中执行。如果跨线程执行，则将此任务加入到任务队列中，并唤醒应当执行此任务得线程。而原线程唤醒其他线程之后，就可以继续执行别的操作了。可以看到，这是一个异步得操作。
```cpp
// 在I/O线程中调用某个函数，该函数可以跨线程调用
void EventLoop::runInLoop(Functor cb)
{
    if (isInLoopThread())
    {
        // 如果是在当前I/O线程中调用，就同步调用cb回调函数
        cb();
    }
    else
    {
        // 否则在其他线程中调用，就异步将cb添加到任务队列当中，
        // 以便让EventLoop真实对应的I/O线程执行这个回调函数
        queueInLoop(std::move(cb));
    }
}
```
`queueInLoop`的实现:首先在局部区域生成一个互斥锁,然后再进行任务队列加入新任务的操作。
这是因为可能此`EventLoop`会被多个线程所操纵，假设多个线程调用`loop->queueInLoop(cb)`，都向此任务队列加入自己的回调函数，这势必会有线程间的竞争情况。需要在此处用一个互斥锁保证互斥，可以看到这个锁的粒度比较小。

`if (!isInLoopThread() || callingPendingFunctors_)`，第一个意思是不在本线程则唤醒这个 `EventLoop `所在的线程。第二个判断是：`callingPendingFunctors_` 这个标志位在 `EventLoop::doPendingFunctors()` 函数中被标记为 true。 也就是说如果 EventLoop 正在处理当前的 PendingFunctors 函数时有新的回调函数加入，我们也要继续唤醒。因为如果不唤醒，那么新加入的函数就不会得到处理，会因为下一轮的 epoll_wait 而继续阻塞住，这显然会降低效率。
```cpp
void EventLoop::queueInLoop(Functor cb){
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(cb); // 使用了std::move
    }

    if (!isInLoopThread() || callingPendingFunctors_){
        // 唤醒loop所在的线程
        wakeup();
    }
}
```

需要唤醒其他线程时，只要向`wakeupFd_ `写数据就行。`wakeupFd_`在EventLoop对象创建时都被加入到`epoll`对象中注册了读事件，只要写了数据就会触发读事件，`epoll_wait `就会返回。因此`EventLoop::loop`中阻塞的情况被打断，`Reactor`又被事件驱动了起来。
```cpp
void EventLoop::wakeup(){
    uint64_t one = 1;
    ssize_t n = write(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one)){
        LOG_ERROR << "EventLoop::wakeup writes " << n << " bytes instead of 8";
    }
}

void EventLoop::handleRead(){
    uint64_t one = 1;
    ssize_t n = read(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one)){
        LOG_ERROR << "EventLoop::handleRead() reads " << n << " bytes instead of 8";
    }
}
```

#### EventLoop处理事件队列中存储的回调函数
果直接遍历 pendingFunctors，然后在这个过程中别的线程又向这个容器添加新的要被调用的函数，那么这个过程是线程不安全的。如果使用互斥锁，那么在执行回调任务的过程中，都无法添加新的回调函数。这是十分影响效率的。因此我们定义了一个 functors 交换 pendingFunctors 中的元素，然后遍历 functors，这样在执行回调函数时其他线程还能继续访问 pendingFunctors 。
```cpp
void EventLoop::doPendingFunctors(){
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;

    /**
     * 如果没有生成这个局部的 functors
     * 则在互斥锁加持下，我们直接遍历pendingFunctors
     * 其他线程这个时候无法访问，无法向里面注册回调函数，增加服务器时延
     */
    {
        std::unique_lock<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }

    for (const Functor &functor : functors){
        functor();
    }

    callingPendingFunctors_ = false;
}
```

## 结语
高性能服务器的编写有多种方式，此处仅仅采用了基于Reactor模式实现的TCP网络编程库，围绕Multi-reactor模型进行展开，通过one loop per thread的方式工作，在多个细节上提高了运行性能。今后可能会完善http模块，rpc模块以及内存池等。
