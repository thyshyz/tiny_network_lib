#include "CurrentThread.h"

namespace CurrentThread{
    __thread int cachedTid = 0;

    void cacheTid(){
        if(cachedTid == 0){
            cachedTid = static_cast<pid_t>(::syscall(SYS_gettid));
        }
    }
}