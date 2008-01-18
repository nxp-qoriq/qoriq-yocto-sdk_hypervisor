#ifndef LIBOS_CLIENT_H
#define LIBOS_CLIENT_H

#define CCSRBAR_VA              0xfe000000

#define PHYSBASE 0
#define BASE_TLB_ENTRY 15

#ifndef _ASM
typedef int client_cpu_t;
#endif

#define EXC_DECR_HANDLER dec_handler
#define EXC_EXT_INT_HANDLER ext_int_handler

#endif
