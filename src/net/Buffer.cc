#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>
#include "Buffer.h"

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

//用户将要输出的数据写入outputbuffer(readindex移动)
//之后写事件触发时outputbuffer将数据写入socket(writeindex移动)
//移动readindex这一步在函数外进行
ssize_t Buffer::writeFd(int fd, int *saveErrno){
    ssize_t n = ::write(fd,peek(),readableBytes());
    if(n < 0){
        *saveErrno = errno;
    }
    return n;
}