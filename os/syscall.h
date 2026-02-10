#ifndef SYSCALL_H
#define SYSCALL_H

#include "proc.h"

/**
 * sys_task_info() - System call to retrieve task information
 * @ti: Pointer to TaskInfo structure to fill
 * 
 * Returns: 0 on success, -1 on error
 */
int sys_task_info(TaskInfo *ti);

/**
 * syscall() - Main system call dispatcher
 * 
 * Called from trap handler to process system calls from user space.
 * Reads syscall number and arguments, dispatches to handler, returns result.
 */
void syscall();

#endif // SYSCALL_H