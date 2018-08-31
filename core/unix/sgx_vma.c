#include <sys/types.h>
#include <sys/mman.h>

/* for open */
#include <sys/stat.h>
#include <fcntl.h>
#include "../globals.h"
#include "../hashtable.h"
#include "../native_exec.h"
#include <string.h>
#include <unistd.h> /* for write and usleep and _exit */
#include <limits.h>


#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <syslog.h>             /* vsyslog */
#include "../vmareas.h"
#ifdef RCT_IND_BRANCH
# include "../rct.h"
#endif
#ifdef LINUX
# include "include/syscall.h"            /* our own local copy */
#else
# include <sys/syscall.h>
#endif
#include "../module_shared.h"
#include "os_private.h"
#include "../synch.h"
#include "memquery.h"
#include "ksynch.h"

#include "sgx_vma.h"

/* add new item between llp and lln */
void list_add(list_t *llp, list_t *lln, list_t *ll)
{
    //YPHASSERT(llp != lln);
    /* update the link */
    ll->prev = llp;
    ll->next = lln;

    llp->next = ll;
    lln->prev = ll;
}

/* delete item from its list */
void list_del(list_t *ll)
{
    list_t *llp;
    list_t *lln;

    llp = ll->prev;
    lln = ll->next;
    YPHASSERT(llp != lln);

    /* update the link */
    lln->prev = llp;
    llp->next = lln;
}

#define SGX_PAGE_SIZE 4096

/* simulate the task_struct::fs[] */
#define SGX_PROCMAPS_MAX_FILE 20
char procmaps_cmt[SGX_PROCMAPS_MAX_FILE][64];

void sgx_vma_set_cmt(ulong fd, const char *fname)
{
    if (fd >= SGX_PROCMAPS_MAX_FILE)
        return;

    if (strlen(fname) > SGX_VMA_CMT_LEN)
        return;

    strncpy(procmaps_cmt[fd], fname, SGX_VMA_CMT_LEN);
}

void sgx_vma_get_cmt(ulong fd, char *buffer)
{
    if (fd >= SGX_PROCMAPS_MAX_FILE)
        return;

    strncpy(buffer, procmaps_cmt[fd], SGX_VMA_CMT_LEN);
}



byte* sgx_vm_base   = NULL;
byte* ext_vm_base   = NULL;
byte* ext_vvar_start    = NULL;
sgx_mm_t sgxmm;



/* The memroy layout when Dynamorio is executed */
#define dr_code_size    0x3b0000
#define dr_seggap_s1    0x1fa000
#define vvar_size       0x3000
#define vdso_size       0x2000
#define dr_data_size    0x46000
#define heap_size       0x40000
#define stack_size      0x21000
#define vsyscall_size   0x1000

#define dr_code_start_s1    (byte*)0x7ffff79ca000
#define dr_code_end_s1      (byte*)(dr_code_start_s1 + dr_code_size)
#define vvar_start          (byte*)(dr_code_end_s1 + dr_seggap_s1)
#define vvar_end            (byte*)(vvar_start + vvar_size)
#define vdso_start          (byte*)(vvar_end)
#define vdso_end            (byte*)(vdso_start + vdso_size)
#define dr_data_start_s1    (byte*)(vdso_end)
#define dr_data_end_s1      (byte*)(dr_data_start_s1 + dr_data_size)
#define heap_start          (byte*)(dr_data_end_s1)
#define heap_end            (byte*)(heap_start + heap_size)
#define stack_start         (byte*)0x7ffffffde000
#define stack_end           (byte*)(stack_start + stack_size)
#define vsyscall_start      (byte*)0xffffffffff600000
#define vsyscall_end        (byte*)(vsyscall_start + vsyscall_size)

/* The memroy layout if Dynamorio is reloaed */
#define dr_hole1_size       0x1ff000
#define dr_hole2_size       0x40000
#define dr_hole3_size       0x1000

#define dr_code_start_s2    (byte*)0x71000000
#define dr_code_end_s2      (byte*)(dr_code_start_s2 + dr_code_size)
#define dr_hole1_start_s2   (byte*)(dr_code_end_s2)
#define dr_hole1_end_s2     (byte*)(dr_hole1_start_s2 + dr_hole1_size)
#define dr_data_start_s2    (byte*)(dr_hole1_end_s2)
#define dr_data_end_s2      (byte*)(dr_data_start_s2 + dr_data_size)
#define dr_hole2_start_s2   (byte*)(dr_data_end_s2)
#define dr_hole2_end_s2     (byte*)(dr_hole2_start_s2 + dr_hole2_size)
#define dr_hole3_start_s2   (byte*)(dr_hole2_end_s2)
#define dr_hole3_end_s2     (byte*)(dr_hole3_start_s2 + dr_hole3_size)

#define sgx_mm_size         SGX_BUFFER_SIZE
#define sgx_mm_start        (byte*)SGX_BUFFER_BASE
#define sgx_mm_end          (byte*)(sgx_mm_start + sgx_mm_size)


#define DR_PATH "/home/yph/project/dynamorio.org/debug/lib64/debug/libdynamorio.so"
sgx_vm_area_t vma_list_exec[] = {
    {dr_code_start_s1, dr_code_end_s1, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, 0, {NULL, NULL}, DR_PATH},
    {vvar_start, vvar_end, NULL, PROT_READ, 0, 0, 0, 0, {NULL, NULL}, "[vvar]"},
    {vdso_start, vdso_end, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, 0, {NULL, NULL}, "[vdso]"},
    {dr_data_start_s1, dr_data_end_s1, NULL, PROT_READ|PROT_WRITE, 0, 0, 0, dr_code_size, {NULL, NULL}, DR_PATH},
    {heap_start, heap_end, NULL, PROT_READ|PROT_WRITE, 0, 0, 0, 0, {NULL, NULL}, "[heap]"},
    {stack_start, stack_end, NULL, PROT_READ|PROT_WRITE, 0, 0, 0, 0, {NULL, NULL}, "[stack]"},
    {vsyscall_start, vsyscall_end, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, 0, {NULL, NULL}, "[vsyscall]"},
    {(byte*)NULL, (byte*)NULL, NULL, 0, 0, 0, 0, 0, {NULL, NULL}, ""}
};

sgx_vm_area_t vma_list_reload[] = {
    {dr_code_start_s2, dr_code_end_s2, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, 0, {NULL, NULL}, DR_PATH},
    {dr_hole1_start_s2, dr_hole1_end_s2, NULL, PROT_NONE, 0, 0, 0, 0, {NULL, NULL}, ""},
    {dr_data_start_s2, dr_data_end_s2, NULL, PROT_READ|PROT_WRITE, 0, 0, 0, dr_code_size, {NULL, NULL}, DR_PATH},
    {dr_hole2_start_s2, dr_hole2_end_s2, NULL, PROT_READ|PROT_WRITE, 0, 0, 0, 0, {NULL, NULL}, ""},
    {dr_hole3_start_s2, dr_hole3_end_s2, NULL, PROT_NONE, 0, 0, 0, 0, {NULL, NULL}, ""},
    {sgx_mm_start, sgx_mm_end, NULL, PROT_READ|PROT_WRITE|PROT_EXEC, 0, 0, 0, 0, {NULL, NULL}, "[sgxmm]"},
    {dr_code_start_s1, dr_code_end_s1, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, 0, {NULL, NULL}, DR_PATH},
    {vvar_start, vvar_end, NULL, PROT_READ, 0, 0, 0, 0, {NULL, NULL}, "[vvar]"},
    {vdso_start, vdso_end, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, 0, {NULL, NULL}, "[vdso]"},
    {dr_data_start_s1, dr_data_end_s1, NULL, PROT_READ|PROT_WRITE, 0, 0, 0, dr_code_size, {NULL, NULL}, DR_PATH},
    {heap_start, heap_end, NULL, PROT_READ|PROT_WRITE, 0, 0, 0, 0, {NULL, NULL}, "[heap]"},
    {stack_start, stack_end, NULL, PROT_READ|PROT_WRITE, 0, 0, 0, 0, {NULL, NULL}, "[stack]"},
    {vsyscall_start, vsyscall_end, NULL, PROT_READ|PROT_EXEC, 0, 0, 0, 0, {NULL, NULL}, "[vsyscall]"},
    {(byte*)NULL, (byte*)NULL, NULL, 0, 0, 0, 0, 0, {NULL, NULL}, ""}
};

// exec_phase = 0 means the whole system is just running;
// exec_phase = 1 means the layout-phase1 is just loaded
// exec_phase = 2 means the layout-phase2 is just loaded
// exec_phase = 3 means the dynamoRIO is just reloaded for running
int exec_phase = 0;

bool sgx_mm_within(byte* addr, size_t len)
{
    YPHASSERT(len > 0);

    byte *itn_upper;

    /* the uppper bound for translating */
    if (exec_phase == 0)        // not mapped to sgx-mm-buffer at all
        itn_upper = sgx_vm_base;
    else if (exec_phase == 1)   // address beyond dr_code is not mapped
        itn_upper = dr_code_start_s1 - ext_vm_base + sgx_vm_base;
    else if (exec_phase == 2)   // address beyond vvar is not mapped
        itn_upper = ext_vvar_start - ext_vm_base + sgx_vm_base;
    else if (exec_phase == 3)   // all 0xffffxxxxxxx is mappedph
        //itn_upper = sgx_vm_base + SGX_BUFFER_SIZE;
        itn_upper = ext_vvar_start - ext_vm_base + sgx_vm_base;
    else
        YPHASSERT(false);

    // itn_upper = sgx_vm_base;
    /* return value in terms of execution phase */
    if (itn_upper > sgx_vm_base + SGX_BUFFER_SIZE)
        itn_upper = sgx_vm_base + SGX_BUFFER_SIZE;

    return (addr >= sgx_vm_base && addr + len <= itn_upper);
}

byte* _sgx_mm_itn2ext(byte* itn_addr, bool start_vma)
{
    byte *addr = itn_addr;

    start_vma ? (addr++) : (addr--);
    if (sgx_mm_within(addr, 1))
        return (itn_addr - sgx_vm_base) + ext_vm_base;
    else
        return itn_addr;    // passed in an external address
}

byte* _sgx_mm_ext2itn(byte* ext_addr, bool start_vma)
{
    /* passed in an internal address */
    if (ext_addr < ext_vm_base || ext_addr > ext_vm_base+SGX_BUFFER_SIZE)
        return ext_addr;

    byte* itn_addr = (ext_addr - ext_vm_base) + sgx_vm_base;
    byte* addr = itn_addr;

    start_vma ? (addr ++) : (addr --);
    if (sgx_mm_within(addr, 1))
        return itn_addr;
    else
        return ext_addr;    // cannot mapped into SGX-mm-buffer
}

byte* sgx_mm_itn2ext(byte* itn_addr)
{
    return _sgx_mm_itn2ext(itn_addr, true);
}

byte* sgx_mm_ext2itn(byte* ext_addr)
{
    return _sgx_mm_ext2itn(ext_addr, true);
}


//forward declaration

sgx_vm_area_t* _sgx_vma_alloc(list_t* llp, list_t* lln);
void _sgx_vma_free(sgx_vm_area_t* vma);
static void _sgx_vma_fill(sgx_vm_area_t* vma, byte* ext_addr, size_t len, ulong prot, int fd, ulong offs);

/* init sgx_mm: first should be 1 for true, 0 for reload and -1 for unit debugging */
void sgx_mm_init_static(int first)
{
    sgx_vm_area_t *vma = NULL;
    sgx_vm_area_t *add = NULL;
    list_t *ll = NULL;
    int idx;

    YPHASSERT(SGX_VMA_MAX_CNT > 2);
    for (idx = 0, vma = sgxmm.slots; idx < SGX_VMA_MAX_CNT; idx++, vma++) {
        ll = &vma->ll;
        ll->prev = NULL;    /* set prev to NULL if not used */
        if(idx == SGX_VMA_MAX_CNT - 1) {
            ll->next = NULL;
        }
        else {
            ll->next = &(sgxmm.slots[idx+1].ll);
        }
    }
    sgxmm.un = &(sgxmm.slots[0].ll);
    sgxmm.in.prev = &sgxmm.in;
    sgxmm.in.next = &sgxmm.in;

    sgxmm.nin = 0;
    sgxmm.nun = SGX_VMA_MAX_CNT;

    /* Allocate a big buffer for loading target program into SGX*/
    if (first == true) /* please don't change it */ {
        sgx_vm_base = (byte*)dynamorio_syscall(IF_MACOS_ELSE(SYS_mmap, IF_X64_ELSE(SYS_mmap, SYS_mmap2)), 6,
                SGX_BUFFER_BASE,
                SGX_BUFFER_SIZE,
                PROT_READ|PROT_WRITE|PROT_EXEC,
                MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,
                -1, 0);
        vma = vma_list_exec;
        exec_phase = 1;
    }
    else {
        sgx_vm_base = (byte*)SGX_BUFFER_BASE;
        vma = vma_list_reload;
        exec_phase = 1;
    }


    YPHASSERT(sgx_vm_base == (byte*)SGX_BUFFER_BASE);
    sgxmm.vm_base = sgx_vm_base;
    sgxmm.vm_size = SGX_BUFFER_SIZE;
    ext_vm_base = (app_pc)EXT_VMA_REGION;


    /* Our unit test not wants to load vma_list_exec/reload*/
    if (first == -1) {
        exec_phase = 0;
        return;
    }

    struct stat s;
    for (; vma->vm_start != NULL; vma++) {
        add = _sgx_vma_alloc(sgxmm.in.prev, &sgxmm.in);
        _sgx_vma_fill(add, vma->vm_start, vma->vm_end-vma->vm_start, vma->perm, -1, vma->offset);
        if (vma->comment[0] != '\0') {
            if (vma->comment[0] != '[') {
                int fd;

                fd = dynamorio_syscall(SYS_stat, 2, vma->comment, &s);
                YPHASSERT(fd == 0);

                add->dev = s.st_dev;
                add->inode = s.st_ino;
                add->size = s.st_size;
            }
            strncpy(add->comment, vma->comment, 80);
        }
    }
}


int _sgx_mm_init_byreffing_procmaps(void)
{
    const char *fn = "/proc/self/maps";
    int fd = dynamorio_syscall(SYS_open, 2, fn, O_RDONLY);
    if (fd == -1)
        return -1;

#define BUF_SZ 4096*2
#define HEPA_SZ 0x3ffff000
#define MAPS_LINE_FORMAT4 "%08x-%08x %s %08x %*s %llu %4096s"
#define MAPS_LINE_FORMAT8 "%016llx-%016llx %s %016llx %*s %llu %4096s"
    char buf[BUF_SZ];
    ssize_t nread;
    nread = dynamorio_syscall(SYS_read, 3, fd, buf, BUF_SZ);
    if (nread == -1)
        return -1;

    char *line = buf;
    char *r;

    buf[BUF_SZ-1] = '\0';
    do {
        unsigned long nStart, nEnd, nProt, nOfft, nNode;
        char szProt[8];
        char szCmt[80];
        byte* vm_start;
        sgx_vm_area_t *add;

        r = strchr(line, '\n');
        if (r == NULL)
            break;

        *r = '\0';
        szCmt[0] = '\0';
        sscanf(line,
                sizeof(void*) == 4 ? MAPS_LINE_FORMAT4 : MAPS_LINE_FORMAT8,
                (unsigned long*)&nStart, (unsigned long*)&nEnd,
                szProt, (unsigned long*)&nOfft, &nNode, szCmt);
        line = r+1;

        nProt = 0;
        if (szProt[0] == 'r') nProt += 1;
        if (szProt[1] == 'w') nProt += 2;
        if (szProt[2] == 'x') nProt += 4;

        vm_start = (byte*)nStart;

        // YPHPRINT("%lx-%lx %x %lx %d %d %s\n", nStart, nEnd, nProt, nOfft, nDev, nNode, szCmt);
        add = _sgx_vma_alloc(sgxmm.in.prev, &sgxmm.in);
        _sgx_vma_fill(add, vm_start, nEnd - nStart, nProt, -1, nOfft);
        add->vm_sgx = NULL;
        add->dev = 8;
        add->inode = nNode;
        add->size = nEnd - nStart;

        if (strncmp(szCmt, "[vvar]", 80) == 0)
            ext_vvar_start = vm_start;
        if (vm_start == sgx_vm_base)
            strncpy(add->comment, "[sgxmm]", 80);
        else
            strncpy(add->comment, szCmt, 80);
    }while(r != NULL);

    dynamorio_syscall(SYS_close, 1, fd);

    return 0;
}


void sgx_mm_init(int first)
{
    sgx_vm_area_t *vma = NULL;
    list_t *ll = NULL;
    int idx;

    YPHASSERT(SGX_VMA_MAX_CNT > 2);
    /* Initialize sgxmm */
    for (idx = 0, vma = sgxmm.slots; idx < SGX_VMA_MAX_CNT; idx++, vma++) {
        ll = &vma->ll;
        ll->prev = NULL;    /* set prev to NULL if not used */
        if(idx == SGX_VMA_MAX_CNT - 1) {
            ll->next = NULL;
        }
        else {
            ll->next = &(sgxmm.slots[idx+1].ll);
        }
    }
    sgxmm.un = &(sgxmm.slots[0].ll);
    sgxmm.in.prev = &sgxmm.in;
    sgxmm.in.next = &sgxmm.in;

    sgxmm.nin = 0;
    sgxmm.nun = SGX_VMA_MAX_CNT;

    /* Allocate a big buffer for loading target program into SGX*/
    if (first == true) /* please don't change it */ {
        sgx_vm_base = (byte*)dynamorio_syscall(IF_MACOS_ELSE(SYS_mmap, IF_X64_ELSE(SYS_mmap, SYS_mmap2)), 6,
                SGX_BUFFER_BASE,
                SGX_BUFFER_SIZE,
                PROT_READ|PROT_WRITE|PROT_EXEC,
                MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,
                -1, 0);
        exec_phase = 1;
    }
    else {
        sgx_vm_base = (byte*)SGX_BUFFER_BASE;
        exec_phase = 1;
    }


    YPHASSERT(sgx_vm_base == (byte*)SGX_BUFFER_BASE);
    sgxmm.vm_base = sgx_vm_base;
    sgxmm.vm_size = SGX_BUFFER_SIZE;
    ext_vm_base = (app_pc)EXT_VMA_REGION;

    _sgx_mm_init_byreffing_procmaps();
}


/* exported for debugging */
sgx_mm_t* sgx_mm_getmm(void)
{
    return &sgxmm;
}

/* Allocate a sgx-mm-buffer for mapping external memory region */
byte* _sgx_mm_buffer_alloc(byte* ext_addr, size_t len)
{
    byte* itn_addr;

    itn_addr = sgx_mm_ext2itn(ext_addr);
    if (!sgx_mm_within(itn_addr, len))
        itn_addr = NULL;

    return itn_addr;
}

void _sgx_mm_buffer_free(byte* itn_addr, size_t len)
{
}

sgx_vm_area_t* _sgx_vma_alloc(list_t* llp, list_t* lln)
{
    YPHASSERT(llp != NULL && lln != NULL);

    sgx_vm_area_t* vma = NULL;
    list_t* ll = NULL;

    ll = sgxmm.un;
    sgxmm.un = sgxmm.un->next;
    sgxmm.nin ++, sgxmm.nun --;

    vma = list_entry(ll, sgx_vm_area_t, ll);
    YPHASSERT(vma != NULL);

    list_add(llp, lln, ll);

    return vma;
}

void _sgx_vma_free(sgx_vm_area_t* vma)
{
    YPHASSERT(vma != NULL);

    list_t* ll = &vma->ll;
    list_del(ll);

    ll->prev = NULL; /* set prev to NULL if not used */
    ll->next = sgxmm.un;
    sgxmm.un = ll;
    sgxmm.nin --, sgxmm.nun ++;
}


/* fill a SGX-vma to track the mmap event */
static void _sgx_vma_fill(sgx_vm_area_t* vma, byte* vm_start, size_t len, ulong prot, int fd, ulong offs)
{
    YPHASSERT(vma != NULL);

    vma->vm_start = vm_start;
    vma->vm_end = vm_start + len;
    vma->vm_sgx = NULL;
    vma->offset = offs;
    vma->perm = prot;


    struct stat s;
    int res;

    if (fd == -1) {
        vma->dev = 0;
        vma->inode = 0;
        vma->size = 0;
        *(long*)vma->comment = 0;
    }
    else {
        res = dynamorio_syscall(SYS_fstat, 2, fd, &s);
        YPHASSERT(res == 0);

        vma->dev = s.st_dev;
        vma->inode = s.st_ino;
        vma->size = s.st_size;
        sgx_vma_get_cmt(fd, vma->comment);
    }
}

/* deep copy except the ll field */
static void _sgx_vma_deep_copy(sgx_vm_area_t* dst, sgx_vm_area_t* src)
{
    // don't use :*dst = *src;
    dst->vm_start =  src->vm_start;
    dst->vm_end =  src->vm_end;
    dst->vm_sgx =  src->vm_sgx;
    dst->perm =  src->perm;
    dst->dev =  src->dev;
    dst->inode =  src->inode;
    dst->offset =  src->offset;

    strncpy(dst->comment, src->comment, SGX_VMA_CMT_LEN);
}


/* Test if current VMA is coverred by a given region */
static vma_overlap_t _sgx_vma_overlap(byte* vma_start, byte* vma_end, byte* ref_start, byte* ref_end)
{
    if (ref_start < vma_start) {
        if (ref_end <= vma_start)
            return OVERLAP_NONE;
        else if (ref_end < vma_end)
            return OVERLAP_HEAD;
        else
            return OVERLAP_SUP;
    }
    else if (ref_start == vma_start) {
        if (ref_end < vma_end)
            return OVERLAP_HEAD;
        else
            return OVERLAP_SUP;
    }
    else if (ref_start < vma_end) {
        if (ref_end < vma_end)
            return OVERLAP_SUB;
        else
            return OVERLAP_TAIL;
    }
    else {
        return OVERLAP_NONE;
    }
}


/* Merge adjacent vmas */
static sgx_vm_area_t* _sgx_vma_merge(sgx_vm_area_t* vma)
{
    sgx_vm_area_t* prev;
    sgx_vm_area_t* next;
    list_t* llp;
    list_t* lln;
    list_t* ll;


    YPHASSERT(vma != NULL && &vma->ll != &sgxmm.in);
    ll = &vma->ll;
    llp = ll->prev;
    lln = ll->next;

    if (llp != &sgxmm.in) {
        /* adjacent? */
        prev = list_entry(llp, sgx_vm_area_t, ll);

        if (prev->vm_end == vma->vm_start &&    /* free-prev vma */
                prev->perm == vma->perm &&
                prev->dev == vma->dev &&
                prev->inode == vma->inode &&
                *(long*)prev->comment == *(long*)(vma->comment) &&
                (prev->offset == vma->offset || prev->offset + (prev->vm_end - prev->vm_start) == vma->offset )) {

            vma->vm_start = prev->vm_start;
            if (vma->inode != 0)
                vma->offset = prev->offset;

            _sgx_vma_free(prev);
        }
    }

    if (lln != &sgxmm.in) {
        /* adjacent? */
        next = list_entry(lln, sgx_vm_area_t, ll);

        if (vma->vm_end == next->vm_start &&    /* vma free-next */
                vma->perm == next->perm &&
                vma->dev == next->dev &&
                vma->inode == next->inode &&
                *(long*)vma->comment == *(long*)(next->comment) &&
                (vma->offset == next->offset || vma->offset + (vma->vm_end - vma->vm_start) == next->offset )) {

            vma->vm_end = next->vm_end;

            _sgx_vma_free(next);
        }
    }

    return vma;
}


/* return the number of bytes overlapped */
static size_t _sgx_vma_split(vma_overlap_t ot, sgx_vm_area_t** head, sgx_vm_area_t *vma, sgx_vm_area_t** tail, byte* ref_start, byte *ref_end)
{
    sgx_vm_area_t *add = NULL;
    list_t *ll = &vma->ll;
    list_t *llp = ll->prev;
    list_t *lln = ll->next;
    size_t len = 0;

    YPHASSERT(vma != NULL);
    *head = NULL;
    *tail = NULL;
    switch (ot) {
        case OVERLAP_NONE:

            break;
        case OVERLAP_HEAD:  /* head vma  NULL */
            add = _sgx_vma_alloc(llp, ll);

            _sgx_vma_deep_copy(add, vma);
            add->vm_end = ref_end;
            vma->vm_start = ref_end;
            if (vma->vm_sgx != NULL)
                vma->vm_sgx = sgx_mm_ext2itn(vma->vm_start);
            len = add->vm_end - add->vm_start;
            if (vma->inode != 0)
                vma->offset += len;

            *head = add;

            break;
        case OVERLAP_TAIL:  /* NULL vma tail */
            add = _sgx_vma_alloc(ll, lln);

            _sgx_vma_deep_copy(add, vma);
            vma->vm_end = ref_start;
            add->vm_start = ref_start;
            if (add->vm_sgx != NULL)
                add->vm_sgx = sgx_mm_ext2itn(add->vm_start);
            len = add->vm_end - add->vm_start;
            if (add->inode != 0)
                add->offset += len;

            *tail = add;

            break;
        case OVERLAP_SUP:   /* NULL vma NULL */
            len = vma->vm_end - vma->vm_start;

            break;
        case OVERLAP_SUB:   /* head vma tail */
            if (vma->vm_start != ref_start) {
                add = _sgx_vma_alloc(llp, ll);

                _sgx_vma_deep_copy(add, vma);
                add->vm_end = ref_start;
                vma->vm_start = ref_start;
                if (vma->vm_sgx != NULL)
                    vma->vm_sgx = sgx_mm_ext2itn(vma->vm_start);
                if (vma->inode != 0)
                    vma->offset += add->vm_end - add->vm_start;

                *head = add;
            }

            if (vma->vm_end != ref_end) {
                add = _sgx_vma_alloc(ll, lln);

                _sgx_vma_deep_copy(add, vma);
                vma->vm_end = ref_end;
                add->vm_start = ref_end;
                if (add->vm_sgx != NULL)
                    add->vm_sgx = sgx_mm_ext2itn(add->vm_start);
                if (add->inode != 0)
                    add->offset += vma->vm_end - vma->vm_start;

                *tail = add;
            }
            len = ref_end - ref_start;

            break;
        default:
            YPHASSERT(false);

            break;
    } /* end switch */

    return len;
}



/* Allocate a SGX-vma to track the mmap event */
/* Currently, we always assign the richest privilege: PROTE_READ|PROTE_WRITE|PROTE_EXEC */
int _sgx_mm_mprotect(byte* ext_addr, size_t len, uint prot)
{
    sgx_vm_area_t *vma = NULL;
    sgx_vm_area_t *head = NULL;
    sgx_vm_area_t *tail = NULL;
    list_t *ll = sgxmm.in.next;
    vma_overlap_t ot = OVERLAP_NONE;
    byte *ref_start = ext_addr;
    byte *ref_end = ext_addr + len;
    bool ctuw = true;

    YPHASSERT(ll != &sgxmm.in);

    while (ctuw && ll != &sgxmm.in) {
        vma = list_entry(ll, sgx_vm_area_t, ll);
        ot = _sgx_vma_overlap(vma->vm_start, vma->vm_end, ref_start, ref_end);

        switch (ot) {
            case OVERLAP_NONE:
                if (vma->vm_end <= ref_start)    /* no need to check anymore */
                    ll = ll->next;
                else
                    ctuw = false;

                break;

            case OVERLAP_HEAD:
                if (vma->perm == prot)  {/* already have the same property */
                    len -= vma->vm_end - vma->vm_start;
                }
                else {
                    len -= _sgx_vma_split(ot, &head, vma, &tail, ref_start, ref_end);
                    YPHASSERT(head != NULL);

                    head->perm = prot;
                    if (head->vm_sgx != NULL) {
                        // call mprotect on sgx-mm-buffer
                    }

                    _sgx_vma_merge(head);
                }

                ctuw = false;
                break;

            case OVERLAP_TAIL:
                if (vma->perm == prot) { /* already have the same property */
                    len -= vma->vm_start - vma->vm_end;
                    ll = ll->next;
                }
                else {
                    len -= _sgx_vma_split(ot, &head, vma, &tail, ref_start, ref_end);
                    YPHASSERT(tail != NULL);

                    tail->perm = prot;
                    if (head->vm_sgx != NULL) {
                        // call mprotect on sgx-mm-buffer
                    }
                    /* please don't call _sgx_vma_merge */;

                    ll = tail->ll.next;
                }

                break;

            case OVERLAP_SUP:
                if (vma->perm == prot) { /* already have the same property */
                }
                else {
                    vma->perm = prot;
                    if (vma->vm_sgx != NULL) {
                        // call mprotect on sgx-mm-buffer
                    }
                }
                len -= vma->vm_end - vma->vm_start;
                _sgx_vma_merge(vma);

                ll = ll->next;

                break;

            case OVERLAP_SUB:
                if (vma->perm == prot) { /* already have the same property */
                }
                else {
                    len -= _sgx_vma_split(ot, &head, vma, &tail, ref_start, ref_end);
                    vma->perm = prot;
                    if (vma->vm_sgx != NULL) {
                        // call mprotect on sgx-mm-buffer
                    }
                }

                /* Please don't invoke _sgx_vma_merge */
                len = 0;
                ctuw = false;
                break;

            default:
                YPHASSERT(false);
                break;
        } /* end switch */
    }/* end while */


    if (len != 0)
        return -1;
    else
        return 0;
}


/* track munmap event */
void _sgx_mm_munmap(byte* ext_addr, size_t len)
{
    sgx_vm_area_t *vma = NULL;
    sgx_vm_area_t *head = NULL;
    sgx_vm_area_t *tail = NULL;
    byte *ref_start = ext_addr;
    byte *ref_end = ext_addr + len;

    list_t *ll = sgxmm.in.next;
    vma_overlap_t ot;
    bool ctuw = true;

    while (ctuw && ll != &sgxmm.in) {
        vma = list_entry(ll, sgx_vm_area_t, ll);
        ot = _sgx_vma_overlap(vma->vm_start, vma->vm_end, ext_addr, ext_addr + len);

        switch (ot) {
            case OVERLAP_NONE:
                if (vma->vm_start < ref_start) {
                    ll = ll->next;
                }
                else {
                    ctuw = false;
                }

                break;
            case OVERLAP_HEAD:
                _sgx_vma_split(ot, &head, vma, &tail, ref_start, ref_end);

                YPHASSERT(head != NULL);
                _sgx_vma_free(head);
                ctuw = false;

                break;
            case OVERLAP_TAIL:
                _sgx_vma_split(ot, &head, vma, &tail, ref_start, ref_end);

                YPHASSERT(tail != NULL);
                _sgx_vma_free(tail);

                ll = vma->ll.next;

                break;
            case OVERLAP_SUP:
                ll = vma->ll.next;

                _sgx_vma_free(vma);

                break;
            case OVERLAP_SUB:
                _sgx_vma_split(ot, &head, vma, &tail, ref_start, ref_end);

                YPHASSERT(vma != NULL);
                _sgx_vma_free(vma);
                ctuw = false;

                break;
            default:
                YPHASSERT(false);
                break;
        } /* end switch */
    }/* end while */
}



/* Allocating sgx_vm_area_t to track mmap event */
/* And also allocate sgx-mm-buffer */
sgx_vm_area_t* _sgx_mm_mmap(byte* ext_addr, size_t len, ulong prot, ulong flags, int fd, ulong offs)
{
    sgx_vm_area_t* vma = NULL;

    /* unmap the overllapped area first */
    _sgx_mm_munmap(ext_addr, len);

    /* find the location to insert vma */
    list_t *ll = sgxmm.in.next;
    while (ll != &sgxmm.in) {
        vma = list_entry(ll, sgx_vm_area_t, ll);
        if (vma->vm_start < ext_addr)
            ll = ll->next;
        else
            break;
    }


    /* create a new vma */
    vma = _sgx_vma_alloc(ll->prev, ll);
    _sgx_vma_fill(vma, ext_addr, len, prot, fd, offs);

    /* Allocate sgx-mm-buffer if needs */
    vma->vm_sgx = _sgx_mm_buffer_alloc(ext_addr, len);

    /* do merge if needs for anonymous-mappings */
    if (fd == -1)
        vma = _sgx_vma_merge(vma);

    return vma;
}

// Allocate a sgx-vma whether the given external memory region can be casted into a sgx-mm-buffer.
// If yes, allocates a sgx-mm-buffer. Test the vma->vm_sgx field for checking.
// ext_addr: external address, maybe NULL; should not be internal address
byte* sgx_mm_mmap(byte* ext_addr, size_t len, ulong prot, ulong flags, int fd, ulong offs)
{
    YPHASSERT(!sgx_mm_within(ext_addr, len));

    sgx_vm_area_t* vma = NULL;

    len = (len + SGX_PAGE_SIZE - 1) & ~(SGX_PAGE_SIZE - 1);
    // Allocate vma for tracking mmap event
    vma = _sgx_mm_mmap(ext_addr, len, prot, flags, fd, offs);
    if (vma == NULL) {
        // munmap_syscall();
        return ext_addr;
    }

    /* copy the content from external to internal if needs */
    if (vma->vm_sgx != NULL) {
        if (vma->inode != 0) {
            int ncpy = (vma->size-offs < len) ? ((vma->size-offs + SGX_PAGE_SIZE - 1) & ~(SGX_PAGE_SIZE - 1)) : len;
            memcpy(vma->vm_sgx, ext_addr, ncpy);
            memset(vma->vm_sgx + ncpy, 0, len - ncpy);
        }
        else {
            //memset(vma->vm_sgx, 0, len);
        }
        // mprotect_syscall()
        return vma->vm_sgx;
    }
    else
        return ext_addr;
}

// Free the sgx-vma tracking the given external memory region.
// Free the sgx-mm-buffer if allocated.
// ext_addr: external address; should not be internal address
void sgx_mm_munmap(byte* ext_addr, size_t len)
{
    YPHASSERT(!sgx_mm_within(ext_addr, len));

    sgx_vm_area_t *vma = NULL;
    byte *ref_start = ext_addr;

    list_t *ll = sgxmm.in.next;
    vma_overlap_t ot;
    bool ctuw = true;

    len = (len + SGX_PAGE_SIZE - 1) & ~(SGX_PAGE_SIZE - 1);

    /* flush content to external file if needs */
    while (ctuw && ll != &sgxmm.in) {
        vma = list_entry(ll, sgx_vm_area_t, ll);
        ot = _sgx_vma_overlap(vma->vm_start, vma->vm_end, ext_addr, ext_addr + len);

        switch (ot) {
            case OVERLAP_NONE:
                if (vma->vm_start < ref_start) {
                    ll = ll->next;
                }
                else {
                    ctuw = false;
                }

                break;
            case OVERLAP_HEAD:
                if (vma->inode != 0 && vma->vm_sgx != NULL) {
                    /* flush content to external file */
                }

                ctuw = false;

                break;
            case OVERLAP_TAIL:
                if (vma->inode != 0 && vma->vm_sgx != NULL) {
                    /* flush content to external file */
                }

                ll = vma->ll.next;

                break;
            case OVERLAP_SUP:
                ll = vma->ll.next;

                if (vma->inode != 0 && vma->vm_sgx != NULL) {
                    /* flush content to external file */
                }

                break;
            case OVERLAP_SUB:
                if (vma->inode != 0 && vma->vm_sgx != NULL) {
                    /* flush content to external file */
                }

                ctuw = false;

                break;
            default:
                YPHASSERT(false);
        } /* end case */
    } /* end while */


    /* free vmas */
    _sgx_mm_munmap(ext_addr, len);

    /* invoke mprotect() instead of freeing */
}

// Adjust the sgx-vmas tracking the given external memory region.
// mprotect the sgx-mm-buffer if allocated.
// ext_addr: external address; should not be internal address
int sgx_mm_mprotect(byte* ext_addr, size_t len, uint prot)
{
    YPHASSERT(!sgx_mm_within(ext_addr, len));

    len = (len + SGX_PAGE_SIZE - 1) & ~(SGX_PAGE_SIZE - 1);
    _sgx_mm_mprotect(ext_addr, len, prot);

    return 0;
}


/* allocate vma for the first segment, it's usually .text */
sgx_vm_area_t* ___sgx_vma_alloc_text(uint npgs)
{
    sgx_vm_area_t *vma;
    sgx_vm_area_t *add;
    list_t *llp;

    add = list_entry(sgxmm.un, sgx_vm_area_t, ll);
    sgxmm.un = sgxmm.un->next;

    llp = sgxmm.in.prev;
    if (llp == &sgxmm.in) {
        YPHASSERT(sgxmm.vm_size/SGX_PAGE_SIZE >= npgs);
        add->vm_sgx = 0;
        /*//add->npgs = npgs;*/

        sgxmm.in.next = &add->ll;
        sgxmm.in.prev = &add->ll;
        add->ll.prev = &sgxmm.in;
        add->ll.next = &sgxmm.in;

        return add;
    }
    else {
        vma = list_entry(llp, sgx_vm_area_t, ll);

        /*add->vm_sgx = vma->vm_sgx + vma->npgs;*/
        //add->npgs = npgs;

        list_add(llp, &sgxmm.in, &add->ll);
        return add;
    }
}

sgx_vm_area_t* ___sgx_vma_alloc_data(uint npgs)
{
    YPHASSERT(sgxmm.in.next != &sgxmm.in);
    return ___sgx_vma_alloc_text(npgs);
}

sgx_vm_area_t* ___sgx_vma_alloc_bss(uint npgs)
{
    return ___sgx_vma_alloc_data(npgs);
}



sgx_vm_area_t* ___sgx_vma_alloc_anon(uint npgs)
{
    sgx_vm_area_t *prev;
    sgx_vm_area_t *next;
    sgx_vm_area_t *add;
    list_t *llp;
    list_t *lln;

    llp = sgxmm.in.next;
    lln = llp->next;

    add = list_entry(sgxmm.un, sgx_vm_area_t, ll);
    sgxmm.un = sgxmm.un->next;

    if (llp == &sgxmm.in) {
        YPHASSERT(sgxmm.vm_size/SGX_PAGE_SIZE >= npgs);
        add->vm_sgx = 0;
        //add->npgs = npgs;

        sgxmm.in.next = &add->ll;
        sgxmm.in.prev = &add->ll;
        add->ll.prev = &sgxmm.in;
        add->ll.next = &sgxmm.in;

        return add;
    }
    else if (lln == &sgxmm.in) {
        prev = list_entry(llp, sgx_vm_area_t, ll);
        if (false) {//prev->vm_sgx >= npgs) {
            add->vm_sgx = 0;
            //add->npgs = npgs;

            list_add(&sgxmm.in, llp, &add->ll);
        }
        else {
            /*YPHASSERT(sgxmm.vm_size/SGX_PAGE_SIZE > prev->vm_sgx + prev->npgs);*/
            //add->vm_sgx = prev->vm_sgx + prev->npgs;
            //add->npgs = npgs;

            list_add(llp, &sgxmm.in, &add->ll);
        }
        return add;
    }
    else {
        while (lln != &sgxmm.in) {
            uint gap = 0;

            prev = list_entry(llp, sgx_vm_area_t, ll);
            next = list_entry(lln, sgx_vm_area_t, ll);

            /* adjcent? */
            /*gap = next->vm_sgx - (prev->vm_sgx + prev->npgs);*/
            if (gap >= npgs) {
                //add->vm_sgx = prev->vm_sgx + prev->npgs;
                //add->npgs = npgs;

                list_add(llp, lln, &add->ll);
                return add;
            }
            else {
                llp = lln;
                lln = lln->next;
            }
        }
        if (lln == &sgxmm.in) {
            prev = list_entry(llp, sgx_vm_area_t, ll);
            /*YPHASSERT(sgxmm.vm_size/SGX_PAGE_SIZE > prev->vm_sgx + prev->npgs);*/
            //add->vm_sgx = prev->vm_sgx + prev->npgs;
            //add->npgs = npgs;

            list_add(llp, &sgxmm.in, &add->ll);
            return add;
        }
    }

    /* Failed: free add and return NULL */
    add->ll.next = sgxmm.un;
    sgxmm.un = &add->ll;

    return NULL;
}


sgx_vm_area_t* __sgx_vma_alloc(sgx_vma_type t, uint npgs)
{
    switch(t) {
        case SGX_VMA_TEXT:
            return ___sgx_vma_alloc_text(npgs);
        case SGX_VMA_DATA:
            return ___sgx_vma_alloc_data(npgs);
        case SGX_VMA_BSS:
            return ___sgx_vma_alloc_bss(npgs);
        case SGX_VMA_ANON:
            return ___sgx_vma_alloc_anon(npgs);
            //return ___sgx_vma_alloc_bss(npgs);
        default:
            return NULL;
    }
}


void* __sgx_vma_free(uint first_page, uint npgs)
{
    sgx_vm_area_t *vma = NULL;
    list_t *llp = NULL;
    list_t *ll = NULL;
    void * ext_addr = NULL;

    llp = sgxmm.in.next;
    if (llp == &sgxmm.in)
        return NULL;

    if (llp->next == &sgxmm.in) {
        vma = list_entry(llp, sgx_vm_area_t, ll);

        // if (vma->vm_sgx == first_page && vma->npgs == npgs) {
        //     ext_addr = vma->vm_start;

        //     sgxmm.in.next = &sgxmm.in;
        //     sgxmm.in.prev = &sgxmm.in;

        //     llp->next = sgxmm.un;
        //     sgxmm.un = llp;
        // }

        return ext_addr;
    }

    for (ll = llp->next; ll != &sgxmm.in; ll = ll->next) {
        vma = list_entry(ll, sgx_vm_area_t, ll);

        /*if (vma->vm_sgx == first_page && vma->npgs == npgs) {*/
            /*ext_addr = vma->vm_start;*/
            /*list_del(ll);*/

            /*ll->next = sgxmm.un;*/
            /*sgxmm.un = ll;*/
            /*break;*/
        /*}*/
    }
    return ext_addr;
}


// byte* _sgx_mm_mmap(byte *exp_addr, byte *ext_addr, size_t len, ulong prot, ulong flags, ulong fd, ulong offs)
// {
//     sgx_vm_area_t* vma;
//     sgx_vma_type t;
//     uint npgs;

//     npgs = (len + SGX_PAGE_SIZE - 1) / SGX_PAGE_SIZE;
//     if (exp_addr == NULL) {
//         if (fd == (ulong)-1)
//             t = SGX_VMA_ANON;
//         else
//             t = SGX_VMA_TEXT;
//     }
//     else {
//         YPHASSERT(ext_addr != NULL);
//         if (fd == (ulong)-1)
//             t = SGX_VMA_BSS;
//         else
//             t = SGX_VMA_DATA;
//     }
//     vma = __sgx_vma_alloc(t, npgs);
//     if (vma == NULL)
//         return NULL;

//     switch (t) {
//         case SGX_VMA_ANON:
//             vma->vm_start = NULL;
//             break;

//         case SGX_VMA_TEXT:
//             vma->vm_start = ext_addr;
//             break;

//         case SGX_VMA_DATA:
//             vma->vm_start = ext_addr;
//             break;

//         case SGX_VMA_BSS:
//             vma->vm_start = NULL;
//             break;

//         default:
//             return NULL;
//     }

//     /* fill in other content */
//     vma->perm = prot;
//     vma->offset = offs;
//     /* anonymous mmap ? */
//     if (fd == (ulong)-1)
//         vma->inode = 0;
//     else
//         vma->inode = 0; /* fourcely set it to 0 */

//     sgx_vma_get_cmt(fd, vma->comment);

//     return (sgxmm.vm_base + vma->vm_sgx * SGX_PAGE_SIZE);
// }

// void* _sgx_mm_munmap(byte* itn_addr, size_t len)
// {
//     void *ext_addr;
//     uint pgidx;
//     uint npgs;

//     YPHASSERT(itn_addr > sgxmm.vm_base);
//     pgidx = (itn_addr - sgxmm.vm_base) / SGX_PAGE_SIZE;
//     npgs = (len + SGX_PAGE_SIZE - 1) / SGX_PAGE_SIZE;
//     ext_addr = __sgx_vma_free(pgidx, npgs);

//     YPHASSERT(ext_addr != NULL);

//     return ext_addr;
// }


// void _sgx_mm_mprotect(byte* start, byte* end)
// {

// }

byte* sgx_mm_databss_itn2ext(byte* itn_addr, ulong fd)
{
    //uint pgidx = (itn_addr - sgxmm.vm_base) / SGX_PAGE_SIZE;
    sgx_vm_area_t *vma = NULL;
    list_t *ll = NULL;

    for (ll = sgxmm.in.next; ll != &sgxmm.in; ll = ll->next) {
        YPHASSERT(ll != &sgxmm.in);
        vma = list_entry(ll, sgx_vm_area_t, ll);

        //if (vma->vm_sgx + vma->npgs == pgidx) {
            //YPHASSERT(vma->fd == fd);
            /*return vma->vm_start + vma->npgs * SGX_PAGE_SIZE;*/
        {}
    }
    return NULL;
}


sgx_vm_area_t* sgx_procmaps_mmap(byte* addr)
{
    // sgx_vm_area_t *vma;
    // sgx_vm_area_t *add;
    // list_t *llp;

    // int npgs;
    // add = list_entry(sgxmm.un, sgx_vm_area_t, ll);
    // sgxmm.un = sgxmm.un->next;

    // llp = sgxmm.in.prev;
    // if (llp == &sgxmm.in) {
    //     YPHASSERT(sgxmm.vm_size/SGX_PAGE_SIZE >= npgs);
    //     add->vm_sgx = 0;
    //     //add->npgs = npgs;

    //     sgxmm.in.next = &add->ll;
    //     sgxmm.in.prev = &add->ll;
    //     add->ll.prev = &sgxmm.in;
    //     add->ll.next = &sgxmm.in;

    //     return add;
    // }
    // else {
    //     vma = list_entry(llp, sgx_vm_area_t, ll);
    //     YPHASSERT(sgxmm.vm_size/SGX_PAGE_SIZE > vma->vm_sgx + vma->npgs + npgs);

    //     add->vm_sgx = vma->vm_sgx + vma->npgs;
    //     //add->npgs = npgs;

    //     list_add(llp, &sgxmm.in, &add->ll);
    //     return add;
    return NULL;

}
