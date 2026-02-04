#ifndef RUN_H
#define RUN_H

void script_set_var(char* L);
void script_additive_or_assign(char* L);
void script_echo(char* L);
void run_script(const char* filename);

#endif