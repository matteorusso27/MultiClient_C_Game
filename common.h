#ifndef COMMON_H
#define COMMON_H
#include "errno.h"
#include <stdio.h>

#define SERVER_ADDRESS   "127.0.0.1"
#define TCP_PORT         2727
#define UDP_PORT         8888
#define BUFFER_SIZE      1000000 // Dimensione massima dei buffer utilizzati (grande per evitare di mandare messaggi a pezzi)
#define TCP             "[ TCP ]"
#define UDP             "[ UDP ]" 

// macro to simplify error handling
#define GENERIC_ERROR_HELPER(cond, errCode, msg) do {               \
        if (cond) {                                                 \
            fprintf(stderr, "%s: %s\n", msg, strerror(errCode));    \
            exit(EXIT_FAILURE);                                     \
        }                                                           \
    } while(0)

#define ERROR_HELPER(ret, msg)          GENERIC_ERROR_HELPER((ret < 0), errno, msg)
#define PTHREAD_ERROR_HELPER(ret, msg)  GENERIC_ERROR_HELPER((ret != 0), ret, msg)

/* Configuration parameters */
#define DEBUG           1   // display debug messages

#endif 
