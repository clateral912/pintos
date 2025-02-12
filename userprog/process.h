#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "../threads/thread.h"

#define MAX_CMDLINE_LENGTH 128
#define MAX_CMDLINE_TOKENS 32
#define FORCE_EXIT 1 
#define NORMAL_EXIT 0

extern bool load_failed;
extern struct lock load_failure_lock;
tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);
void process_destroy_fd_list(struct thread *);
uint32_t process_create_fd_node(struct thread *, struct file *);
bool process_remove_fd_node(struct thread *, uint32_t);
struct file * process_from_fd_get_file(struct thread *, uint32_t);
#endif /* userprog/process.h */
