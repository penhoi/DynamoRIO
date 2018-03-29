
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <elf.h>

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

    //copy auxv[]
    Elf64_auxv_t *auxv_n = (Elf64_auxv_t*)pstack;
    Elf64_auxv_t *auxv_o = (Elf64_auxv_t*)(t+1);
    do {
        *auxv_n = *auxv_o;
        auxv_n++;
        auxv_o++;
    } while (auxv_o->a_type != AT_NULL);

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
            /*"xor %%rdi,%%rdi  \n\t"*/
            "jmp *%2   \n\t"
            ::"rm"(oldsp), "rm"(new_stack), "rm"(libstart));

    //fix-me: restore the original stack to exit current process elegently.
}


/* Application entry */
int main(int argc, char *argv[], char* envp[])
{
    /*Get configure file*/
    char szCfg[PATH_MAX];
    getcwd(szCfg, PATH_MAX);
    assert(szCfg[0] == '/');

    char *ps = szCfg;
    ps = strchr(ps+1, '/');
    assert(ps != NULL);
    ps = strchr(ps+1, '/');
    assert(ps != NULL);
    strcpy(ps, "/.dynamorio");
    
    pid_t pid = getpid();
    sprintf(ps+11, "/%s.%d.1config64", argv[0], pid);


    /*Get the path of libdynamorio.so*/
    FILE *fCfg = fopen(szCfg, "r");
    assert(fCfg != NULL);

    char *line = (char*)malloc(PATH_MAX);
    size_t len = PATH_MAX;
    ssize_t read;
    bool bfind = false;
    
    while((read = getline(&line, &len, fCfg)) != -1) {
        if (read < 20 || strncmp(line, "DYNAMORIO_AUTOINJECT", 20))
            continue;
        line[read-1] = 0;
        bfind = true;
        break;
    }
    fclose(fCfg);

    if (!bfind) {
        free(line);
        return -1;
    }


    char szLib[PATH_MAX];
    char szTmp[PATH_MAX];

    strcpy(szTmp, line+21);
    realpath(szTmp, szLib);
    free(line);

    printf("Test: %s\n", szLib);

    /*open libdynamorio & jump to its entry-point*/
    void *hlib = NULL;
    hlib = dlopen(szLib, RTLD_LAZY);
    if (!hlib) {
        printf("%s\n", dlerror());
        return -1;
    }

    /*Fixme: The third segment*/
    /*char szMap[PATH_MAX];*/
    /*FILE*  fMap;*/
    /*sprintf(szMap, "/proc/%d/maps", pid);*/
    
    /*printf("Test: %s\n", szMap);*/
    
    /*fMap = fopen(szMap, "r");*/
    /*assert(fMap != NULL);*/

    /*fclose(fMap);*/

    /*mprotect((void*)0x71588000, 0x388000, PROT_READ|PROT_WRITE|PROT_EXEC);*/

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

