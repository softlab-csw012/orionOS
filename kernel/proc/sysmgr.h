#ifndef SYSMGR_H
#define SYSMGR_H

__attribute__((noreturn)) void sysmgr_thread(void);
__attribute__((noreturn)) void sysmgr_idle_loop(void);
void sysmgr_note_prompt(void);
void sysmgr_request_prompt(void);

#endif
