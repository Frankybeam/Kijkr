#ifndef EVO_SYSEXIT_H
#define EVO_SYSEXIT_H
/*
 * Module: evo_sysexit
 *
 * Asks the console to close EVO the way the system closes an app
 * (sceSystemServiceLoadExec("exit")), looked up at run time so a firmware
 * without it is no crash. Only called once everything is released and the GPU
 * is idle (the soft close has parked). Returns only when it did not work.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* -1 when the system call is not there or refused; otherwise does not return. */
int evo_system_exit(void);

#ifdef __cplusplus
}
#endif
#endif
