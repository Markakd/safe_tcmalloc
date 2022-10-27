// gcc gprof.c -c
// if we wnat to get trace info with crtl+C, link gprof.o in v++ 
// ${BASE}/build/src/safe_tcmalloc/tcmalloc/static/gprof.o
#include <signal.h>
#include <unistd.h>
extern void _mcleanup(void);
void record_gmon(int sig){
    _mcleanup();
    _exit(0);
}
void __attribute__((constructor)) gmon_record_signal_init(void) {
    signal(SIGINT, record_gmon);
}
