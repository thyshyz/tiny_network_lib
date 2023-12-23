#ifndef CURRENT_THREAD_H
#define CURRENT_THREAD_H

#include <unistd.h>
#include <sys/syscall.h>

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

#endif