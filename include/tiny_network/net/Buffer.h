#ifndef BUFFER_H
#define BUFFER_H
#include <vector>
#include <string>
#include <algorithm>

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

    //debug用，读出buffer所有内容但不置位
    std::string getAllAsString(){
        size_t len = readableBytes();
        std::string res(peek(),len);
        return res;
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

    void ensureWriteableBytes(size_t len){
        if(writeableBytes() < len){
            resize_(len);
        }
    }

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

    //返回buffer上writeindex的地址
    const char *beginWrite() const{
        return begin_() + writeIndex_;
    }
    
    char *beginWrite(){
        return begin_() + writeIndex_;
    }

    ssize_t readFd(int fd, int *saveErrno);
    ssize_t writeFd(int fd, int *saveErrno);

private:
    std::vector<char> buffer_;
    size_t readIndex_;
    size_t writeIndex_;
    inline size_t size_() const{
        return buffer_.size();
    }

    //返回buffer起始地址
    char *begin_(){
        return &(*buffer_.begin());
    }

    const char *begin_() const{
        return &(*buffer_.begin());
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
};

#endif