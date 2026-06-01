#include <stdio.h>
int main() {
    void* tpidr;
    __asm__("mrs %0, tpidr_el0" : "=r"(tpidr));
    void** dtv = *(void***)tpidr;
    printf("tpidr=%p, dtv=%p\n", tpidr, dtv);
    return 0;
}
