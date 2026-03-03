#include "stdio.h"
#include "syscall.h"

int main(){
    char buf[128];
    int n = sys_read(0, buf, sizeof(buf));
    if (n <= 0) {
        printf("input: <none>\n");
        return 0;
    }
    if (n >= (int)sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    buf[n] = 0;

    printf("input: %s\n", buf);
    return 0;
}
