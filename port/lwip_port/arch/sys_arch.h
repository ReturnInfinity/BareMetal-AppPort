#ifndef LWIP_ARCH_SYS_ARCH_H
#define LWIP_ARCH_SYS_ARCH_H

// Nothing needed here: with NO_SYS=1, lwip/sys.h itself provides
// stub typedefs (sys_sem_t, sys_mutex_t, sys_mbox_t) and no-op
// macros for every sys_*() primitive -- there is no real OS to
// abstract, and we don't compile anything (api/tcpip.c, api/
// sockets.c) that would actually instantiate a semaphore, mailbox,
// or thread. sys_now() (the one real hook NO_SYS mode requires) is
// implemented in net_glue.c, backed by b_system(TIMECOUNTER, ...).

#endif
