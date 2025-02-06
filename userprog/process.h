#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "../threads/thread.h"

#define MAX_CMDLINE_LENGTH 128
#define MAX_CMDLINE_TOKENS 32

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
