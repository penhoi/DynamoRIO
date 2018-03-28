
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>

typedef void (*fp_entry) (int argc, char* argv[], char* envp[]);

void call_libstart(int argc, char**argv, char** envp, void *libstart)
{
#define MAX_ENVP    100
    char *new_stack[MAX_ENVP] = {0};
    char **pstack = new_stack;
    char *oldsp;
    char **t;
    int n;

    //Count the number of env variables
    for (t = envp, n = 0; *t != NULL; t++, n++);

    //create a local stack
    n += argc + 3;
    assert(n < MAX_ENVP);

    //fill the stack
    *(long*)pstack = argc;
    pstack++;

    //copy argv[]
    for (t = argv, n = 0; *t != NULL && n < MAX_ENVP; t++, pstack++)
        *pstack = *t;
    *pstack++ = NULL;
    assert(new_stack[1] == argv[0]);
    //copy envp[]
    for (t = envp, n = 0; *t != NULL && n < MAX_ENVP; t++, pstack++)
        *pstack = *t;
    *pstack++ = NULL;
    assert(new_stack[argc+2] == envp[0]);

    /*asm volatile ("\n\t"*/
    /*"push %0    \n\t"*/
    /*"push %1    \n\t"*/
    /*"push %2    \n\t"*/
    /*"xor %%rdi,%%rdi  \n\t"*/
    /*"jmp *%3   \n\t"*/
    /*::"rm"(envp), "rm"(argv), "rm"((long)argc), "rm"(libstart));*/

    asm volatile ("\n\t"
            "mov %%rsp, %0   \n\t"
            "mov %1, %%rsp   \n\t"
            "xor %%rdi,%%rdi  \n\t"
            "jmp *%2   \n\t"
            ::"rm"(oldsp), "rm"(new_stack), "rm"(libstart));
}


/* Application entry */
int main(int argc, char *argv[], char* envp[])
{
    /*char *plib = "/home/sgx/project/sgx/sgx-dbi/dynamorio/libdynamorio.so";*/
    char *plib = "/home/sgx/project/dynamorio/debug/lib64/debug/libdynamorio.so.org";
    void *hlib = NULL;

    hlib = dlopen(plib, RTLD_LAZY);
    if (!hlib) {
        printf("%s\n", dlerror());
        return -1;
    }

    struct link_map *lm = (struct link_map*)hlib;
    printf("start address: %lx\n", lm->l_addr);

    /*mprotect((void*)0x71000000, 0x163000, PROT_READ|PROT_WRITE|PROT_EXEC);*/
    mprotect((void*)0x7158c000, 0x27000, PROT_READ|PROT_WRITE|PROT_EXEC);

    fp_entry libstart;
    char *pentry = "_start";
    libstart = dlsym(hlib, pentry);

    if (!libstart) {
        printf("%s\n", dlerror());
        dlclose(hlib);
        return -1;
    }

    call_libstart(argc, argv, envp, libstart);

    dlclose(hlib);
    return 0;
}

