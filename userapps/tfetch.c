#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

int main() {
    struct utsname system_info;
    uname(&system_info);

    char hostname[256];
    gethostname(hostname, 256);

    printf("   /\\___/\\\n");
    printf("  ( o   o )   OS: %s %s\n", system_info.sysname, system_info.release);
    printf("  (  =^=  )   Host: %s\n", hostname);
    printf("  (_______)\n");
    printf("\n");

    return 0;
}
