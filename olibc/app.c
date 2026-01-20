// for test (use olibc)
#include "syscall.h"
#include "string.h"
#include <stdint.h>

char buf[128];

int main(void) {
    const char* msg = "Hello, World!\n";
    size_t len = strlen(msg);

    int fd_con = sys_open("console");
    sys_write(fd_con, msg, len);

    int fd1 = sys_open("/home/file1.txt");
    sys_write(fd1, msg, len);

    int a = sys_read(fd1, buf, sizeof(buf) - 1);
    if (a  >  0) {
        sys_write(fd_con, buf, a);
    } 

    sys_close(fd_con);
    sys_close(fd1);

    sys_exit(0);
}