#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>


#define SGX_VMA_MAX_CNT 180
#define SGX_VMA_CMT_LEN 80


typedef unsigned int uint;
typedef unsigned char byte;

typedef struct _list_t {
    struct _list_t *prev;
    struct _list_t *next;
}list_t;

typedef struct _sgx_vm_area_t {
    byte* vm_start;    /* address of corresponding outside memory region */
    byte* vm_end;      /* address of corresponding outside memory region */
    byte* vm_sgx;      /* start address of sgx-buffer */
    uint perm;
    ulong dev;         /*  typedef unsigned long int __dev_t */
    ulong inode;       /*  typedef unsigned long int __ino_t */
    ulong size;
    size_t offset;
    list_t ll;
    char comment[SGX_VMA_CMT_LEN];
}sgx_vm_area_t;


typedef struct _sgx_mm_t {
    byte* vm_base;
    size_t vm_size;  // Total size of sgx_buffer

    /* vm areas */
    sgx_vm_area_t slots[SGX_VMA_MAX_CNT];

    /* managing vm areas */
    list_t in;
    list_t *un;
    int nin, nun;

    /* simulate the reading on procmaps file */
    list_t *read;
} sgx_mm_t;



#define LIBDR_SO "/home/yph/project/dynamorio.org/debug/lib64/debug/libdynamorio.so"

typedef void (*init_ft) (int);
typedef byte* (*mmap_ft)(byte* ext_addr, size_t len, ulong prot, ulong flags, ulong fd, ulong offs);
typedef void (*munmap_ft)(byte* itn_addr, size_t len);
typedef int (*mprotect_ft)(byte* ext_addr, size_t len, uint prot);
typedef int (*start_ft)(void);
typedef void (*stop_ft)(void);
typedef int (*next_ft)(char *buf, int count);
typedef sgx_mm_t* (*getmm_ft)(void);
typedef void (*setcmt_ft)(ulong fd, const char *fname);

/*void sgx_vma_set_cmt(ulong fd, const char *fname);*/
/*void sgx_vma_get_cmt(ulong fd, char *buffer);*/

/*int sgx_procmaps_read_start(void);*/
/*void sgx_procmaps_read_stop(void);*/
/*int sgx_procmaps_read_next(char *buf, int count);*/

init_ft sgx_mm_init = NULL;
mmap_ft sgx_mm_mmap = NULL;
munmap_ft sgx_mm_munmap = NULL;
mprotect_ft sgx_mm_mprotect = NULL;
start_ft sgx_procmaps_read_start = NULL;
stop_ft sgx_procmaps_read_stop = NULL;
next_ft sgx_procmaps_read_next = NULL;
getmm_ft sgx_mm_getmm = NULL;
setcmt_ft sgx_vma_set_cmt = NULL;

#define BUFFER_LEN (4096 + 8)
char buf[BUFFER_LEN];


void test_init_func(void)
{
    printf("%s\n", __FUNCTION__);
    sgx_mm_init(true);

    sgx_procmaps_read_start();
    while (sgx_procmaps_read_next(buf, BUFFER_LEN) != 0) {
        printf("%s", buf);
    }
    sgx_procmaps_read_stop();

    sgx_mm_init(false);

    sgx_procmaps_read_start();
    while (sgx_procmaps_read_next(buf, BUFFER_LEN) != 0) {
        printf("%s", buf);
    }
    sgx_procmaps_read_stop();

}


#define PAGE_SIZE 4096
#define BASE_ADDR 0x7ffff7100000

void test_mprotect(void)
{
    printf("%s starting...\n", __FUNCTION__);

    sgx_mm_init(-1);

    sgx_mm_t* sgxmm = sgx_mm_getmm();
    sgx_vm_area_t *vma0 = &sgxmm->slots[0];
    sgx_vm_area_t *vma1 = &sgxmm->slots[1];
    sgx_vm_area_t *vma2 = &sgxmm->slots[2];
    sgx_vm_area_t *vma3 = &sgxmm->slots[3];
    sgx_vm_area_t *vma4 = &sgxmm->slots[4];


    //first mprotect
    sgx_mm_mmap((byte*)(BASE_ADDR), PAGE_SIZE * 6, PROT_READ, MAP_PRIVATE, -1, 0) ;
    assert(sgxmm->nin == 1);

    //overlap_tail
    sgx_mm_mprotect((byte*)(BASE_ADDR+PAGE_SIZE * 5), PAGE_SIZE, PROT_WRITE);

    //overlap_head
    sgx_mm_mprotect((byte*)(BASE_ADDR), PAGE_SIZE, PROT_WRITE);

    //overlap in
    sgx_mm_mprotect((byte*)(BASE_ADDR+PAGE_SIZE*2), PAGE_SIZE, PROT_WRITE);

    assert(sgxmm->nin == 5);
    assert(vma0->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE*2) && vma0->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*3));
    assert(vma1->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE*5) && vma1->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*6));
    assert(vma2->vm_start == (byte*)BASE_ADDR && vma2->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*1));
    assert(vma3->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE*1) && vma3->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*2));
    assert(vma4->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE*3) && vma4->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*5));


    //change two ajacent
    sgx_mm_mprotect((byte*)(BASE_ADDR+PAGE_SIZE), PAGE_SIZE*2, PROT_EXEC);
    assert(sgxmm->nin == 4);
    assert(vma0->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE*1) && vma0->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*3));

    //change three ajacent
    sgx_mm_mprotect((byte*)(BASE_ADDR), PAGE_SIZE*5, PROT_READ|PROT_EXEC);
    assert(sgxmm->nin == 2);
    assert(vma4->vm_start == (byte*)BASE_ADDR && vma4->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*5));

    printf("%s end successfully\n", __FUNCTION__);
}




void test_munmap(void)
{
    printf("%s starting...\n", __FUNCTION__);

    sgx_mm_init(-1);

    sgx_mm_t* sgxmm = sgx_mm_getmm();
    sgx_vm_area_t *vma0 = &sgxmm->slots[0];
    sgx_vm_area_t *vma1 = &sgxmm->slots[1];
    sgx_vm_area_t *vma2 = &sgxmm->slots[2];
    sgx_vm_area_t *vma3 = &sgxmm->slots[3];
    sgx_vm_area_t *vma4 = &sgxmm->slots[4];
    sgx_vm_area_t *vma5 = &sgxmm->slots[5];
    sgx_vm_area_t *vma6 = &sgxmm->slots[6];

    //first mmap
    sgx_mm_mmap((byte*)(BASE_ADDR), PAGE_SIZE * 16, PROT_READ, MAP_PRIVATE, -1, 0);	/* vma0 */

    //before
    sgx_mm_munmap((byte*)(BASE_ADDR-PAGE_SIZE * 1), PAGE_SIZE * 2);

    //after
    sgx_mm_munmap((byte*)(BASE_ADDR+PAGE_SIZE * 15), PAGE_SIZE * 2);

    assert(sgxmm->nin == 1);
    assert(vma0->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE*1) && vma0->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*15));

    // overlap-in
    sgx_mm_munmap((byte*)(BASE_ADDR+PAGE_SIZE * 4), PAGE_SIZE * 4);
    assert(sgxmm->nin == 2);


    assert(vma1->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE*1) && vma1->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*4));
    assert(vma2->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE*8) && vma2->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*15));

    printf("%s end successfully\n", __FUNCTION__);
}


void test_mmap(void)
{
    printf("%s starting...\n", __FUNCTION__);

    sgx_mm_init(-1);

    sgx_mm_t* sgxmm = sgx_mm_getmm();
    sgx_vm_area_t *vma0 = &sgxmm->slots[0];
    sgx_vm_area_t *vma1 = &sgxmm->slots[1];
    sgx_vm_area_t *vma2 = &sgxmm->slots[2];
    sgx_vm_area_t *vma3 = &sgxmm->slots[3];
    sgx_vm_area_t *vma4 = &sgxmm->slots[4];
    sgx_vm_area_t *vma5 = &sgxmm->slots[5];
    sgx_vm_area_t *vma6 = &sgxmm->slots[6];

    //first mmap
    sgx_mm_mmap((byte*)(BASE_ADDR), PAGE_SIZE * 4, PROT_READ, MAP_PRIVATE, -1, 0) ;	/* vma0 */

    //before
    sgx_mm_mmap((byte*)(BASE_ADDR-PAGE_SIZE * 16), PAGE_SIZE * 4, PROT_READ, MAP_PRIVATE, -1, 0) ;	/* vma1 */

    //after
    sgx_mm_mmap((byte*)(BASE_ADDR+PAGE_SIZE * 20), PAGE_SIZE * 4, PROT_READ, MAP_PRIVATE, -1, 0) ;	/* vma2 */

    assert(sgxmm->nin == 3);
    assert(vma0->vm_start == (byte*)(BASE_ADDR) && vma0->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*4));
    assert(vma1->vm_start == (byte*)(BASE_ADDR-PAGE_SIZE*16) && vma1->vm_end == (byte*)(BASE_ADDR-PAGE_SIZE*12));
    assert(vma2->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE*20) && vma2->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*24));


    //overlap_tail
    sgx_mm_mmap((byte*)(BASE_ADDR+PAGE_SIZE * 3), PAGE_SIZE*2, PROT_EXEC, MAP_PRIVATE, -1, 0) ;

    //overlap_head
    sgx_mm_mmap((byte*)(BASE_ADDR-PAGE_SIZE), PAGE_SIZE*2, PROT_EXEC, MAP_PRIVATE, -1, 0) ;

    assert(sgxmm->nin == 5);
    assert(vma3->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE * 3) && vma3->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*5));
    assert(vma4->vm_start == (byte*)(BASE_ADDR-PAGE_SIZE * 1) && vma4->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE));
    assert(vma0->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE * 1) && vma0->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*3));


    //overlap in
    sgx_mm_mmap((byte*)(BASE_ADDR+PAGE_SIZE * 21), PAGE_SIZE*2, PROT_EXEC, MAP_PRIVATE, -1, 0) ;	/* vma2	*/

    assert(sgxmm->nin == 7);
    assert(vma2->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE*21) && vma2->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*23));
    assert(vma5->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE*20) && vma5->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*21));
    assert(vma6->vm_start == (byte*)(BASE_ADDR+PAGE_SIZE*23) && vma6->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*24));

    //overlap contain
    sgx_mm_mmap((byte*)(BASE_ADDR-PAGE_SIZE*18), PAGE_SIZE*8, PROT_EXEC, MAP_PRIVATE, -1, 0) ; /* vma1 */
    assert(sgxmm->nin == 7);
    assert(vma1->vm_start == (byte*)(BASE_ADDR-PAGE_SIZE*18) && vma1->vm_end == (byte*)(BASE_ADDR-PAGE_SIZE*10));

    printf("%s end successfully\n", __FUNCTION__);
}

void test_mmap2(void)
{

    printf("%s starting...\n", __FUNCTION__);

    sgx_mm_init(-1);

    sgx_mm_t* sgxmm = sgx_mm_getmm();
    sgx_vm_area_t *vma0 = &sgxmm->slots[0];
    sgx_vm_area_t *vma1 = &sgxmm->slots[1];

    sgx_mm_mmap((byte*)(BASE_ADDR+PAGE_SIZE *4), PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    sgx_mm_mmap((byte*)(BASE_ADDR+PAGE_SIZE *3), PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

    assert(sgxmm->nin == 1);
    assert(vma1->vm_start == (byte*)(BASE_ADDR + PAGE_SIZE*3) && vma1->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*5));

    sgx_mm_mmap((byte*)(BASE_ADDR+PAGE_SIZE *2), PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    assert(sgxmm->nin == 1);
    assert(vma0->vm_start == (byte*)(BASE_ADDR + PAGE_SIZE*2) && vma0->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*5));

    sgx_mm_mmap((byte*)BASE_ADDR, PAGE_SIZE *2, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

    assert(sgxmm->nin == 1);
    assert(vma1->vm_start == (byte*)(BASE_ADDR) && vma1->vm_end == (byte*)(BASE_ADDR+PAGE_SIZE*5));

    printf("%s end successfully\n", __FUNCTION__);

}


void sgx_mm_test_ls(void)
{
    printf("%s starting...\n", __FUNCTION__);

    sgx_mm_init(-1);
    char *fn;
    int fd;

    //open("/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
    fn = "/etc/ld.so.cache";
    fd = open(fn, O_RDONLY|O_CLOEXEC);
    if (fd == -1) {
        printf("Faied to open %s\n", fn);
        return;
    }
    sgx_vma_set_cmt(fd, fn);
    sgx_mm_mmap((byte*)0x7ffff7fdc000, 107738, PROT_READ, MAP_PRIVATE, fd, 0) ;
    sgx_mm_mmap((byte*)0x7ffff7fdb000, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) ;
    close(fd);


    //open("/lib/x86_64-linux-gnu/libselinux.so.1", O_RDONLY|O_CLOEXEC) = 3
    fn = "/lib/x86_64-linux-gnu/libselinux.so.1";
    fd = open(fn, O_RDONLY|O_CLOEXEC);
    if (fd == -1) {
        printf("Faied to open %s\n", fn);
        return;
    }
    sgx_vma_set_cmt(fd, fn);
    sgx_mm_mmap((byte*)0x7ffff7bb5000, 2234080, PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_DENYWRITE, fd, 0) ;
    sgx_mm_mprotect((byte*)0x7ffff7bd4000, 2093056, PROT_NONE) ;
    sgx_mm_mmap((byte*)0x7ffff7dd3000, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, fd, 0x1e000) ;
    sgx_mm_mmap((byte*)0x7ffff7dd5000, 5856, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0) ;
    close(fd);


    /*open("/lib/x86_64-linux-gnu/libc.so.6", O_RDONLY|O_CLOEXEC) = 3*/
    fn = "/lib/x86_64-linux-gnu/libc.so.6";
    fd = open(fn, O_RDONLY|O_CLOEXEC);
    if (fd == -1) {
        printf("Faied to open %s\n", fn);
        return;
    }
    sgx_vma_set_cmt(fd, fn);
    sgx_mm_mmap((byte*)0x7ffff77eb000, 3971488, PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_DENYWRITE, fd, 0) ;
    sgx_mm_mprotect((byte*)0x7ffff79ab000, 2097152, PROT_NONE) ;
    sgx_mm_mmap((byte*)0x7ffff7bab000, 24576, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, fd, 0x1c0000) ;
    sgx_mm_mmap((byte*)0x7ffff7bb1000, 14752, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0) ;
    close(fd);

    /*open("/lib/x86_64-linux-gnu/libpcre.so.3", O_RDONLY|O_CLOEXEC) = 6*/
    fn = "/lib/x86_64-linux-gnu/libpcre.so.3";
    fd = open(fn, O_RDONLY|O_CLOEXEC);
    if (fd == -1) {
        printf("Faied to open %s\n", fn);
        return;
    }
    sgx_vma_set_cmt(fd, fn);
    sgx_mm_mmap((byte*)0x7ffff757b000, 2552072, PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_DENYWRITE, fd, 0) ;
    sgx_mm_mprotect((byte*)0x7ffff75e9000, 2097152, PROT_NONE) ;
    sgx_mm_mmap((byte*)0x7ffff77e9000, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, fd, 0x6e000) ;
    close(fd);

    /*open("/lib/x86_64-linux-gnu/libdl.so.2", O_RDONLY|O_CLOEXEC) = 3*/
    fn = "/lib/x86_64-linux-gnu/libdl.so.2";
    fd = open(fn, O_RDONLY|O_CLOEXEC);
    if (fd == -1) {
        printf("Faied to open %s\n", fn);
        return;
    }
    sgx_vma_set_cmt(fd, fn);
    sgx_mm_mmap((byte*)0x7ffff7fda000, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) ;
    sgx_mm_mmap((byte*)0x7ffff7377000, 2109680, PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_DENYWRITE, fd, 0) ;
    sgx_mm_mprotect((byte*)0x7ffff737a000, 2093056, PROT_NONE) ;
    sgx_mm_mmap((byte*)0x7ffff7579000, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, fd, 0x2000) ;
    close(fd);

    /*open("/lib/x86_64-linux-gnu/libpthread.so.0", O_RDONLY|O_CLOEXEC) = 3*/
    fn = "/lib/x86_64-linux-gnu/libpthread.so.0";
    fd = open(fn, O_RDONLY|O_CLOEXEC);
    if (fd == -1) {
        printf("Faied to open %s\n", fn);
        return;
    }
    sgx_vma_set_cmt(fd, fn);
    sgx_mm_mmap((byte*)0x7ffff715a000, 2212904, PROT_READ|PROT_EXEC, MAP_PRIVATE|MAP_DENYWRITE, fd, 0) ;
    sgx_mm_mprotect((byte*)0x7ffff7172000, 2093056, PROT_NONE) ;
    sgx_mm_mmap((byte*)0x7ffff7371000, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, fd, 0x17000) ;
    sgx_mm_mmap((byte*)0x7ffff7373000, 13352, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0) ;
    close(fd);

    sgx_mm_mmap((byte*)0x7ffff7fd9000, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) ;
    sgx_mm_mmap((byte*)0x7ffff7fd7000, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) ;
    sgx_mm_mprotect((byte*)0x7ffff7bab000, 16384, PROT_READ) ;
    sgx_mm_mprotect((byte*)0x7ffff7371000, 4096, PROT_READ) ;
    sgx_mm_mprotect((byte*)0x7ffff7579000, 4096, PROT_READ) ;
    sgx_mm_mprotect((byte*)0x7ffff77e9000, 4096, PROT_READ) ;
    sgx_mm_mprotect((byte*)0x7ffff7dd3000, 4096, PROT_READ) ;
    sgx_mm_mprotect((byte*)0x61d000, 4096, PROT_READ)     ;
    sgx_mm_mprotect((byte*)0x7ffff7ffc000, 4096, PROT_READ);


    sgx_procmaps_read_start();
    while (sgx_procmaps_read_next(buf, BUFFER_LEN) != 0) {
        printf("%s", buf);
    }

    sgx_procmaps_read_stop();

    //sgx_mm_munmap((byte*)0x7ffff7fdc000, 107738)
    /*close(3)                                = 0*/

    /*open("/usr/lib/locale/locale-archive", O_RDONLY|O_CLOEXEC) = 3        ;*/
    //sgx_mm_mmap((byte*)0x7ffff6e82000, 2981280, PROT_READ, MAP_PRIVATE, 3, 0);

    printf("%s end successfully\n", __FUNCTION__);
    return;

}

#define EXT_VMA_REGION  0x71fff0000000
#define SGX_BUFFER_BASE 0x700000000000
#define SGX_BUFFER_SIZE 0x000010000000
void sgx_mm_test_ls_2(void)
{
    printf("%s starting...\n", __FUNCTION__);

    sgx_mm_init(false);

    char *fn;
    int fd;
    byte* itn_addr;
    long nTemp;

    //open("/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3
    fn = "/etc/ld.so.cache";
    fd = open(fn, O_RDONLY|O_CLOEXEC);
    if (fd == -1) {
        printf("Faied to open %s\n", fn);
        return;
    }
    sgx_vma_set_cmt(fd, fn);
    mmap((byte*)0x7ffff0fdc000, 107738, PROT_READ, MAP_PRIVATE, fd, 0) ;
    mmap((byte*)0x7ffff0fdb000, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) ;
    itn_addr = (byte*)sgx_mm_mmap((byte*)0x7ffff0fdc000, 107738, PROT_READ, MAP_PRIVATE, fd, 0) ;

    //Should allocate sgx-mm-buffer successfully
    nTemp = (0x7ffff0fdc000 & (0x10000000-1)) | SGX_BUFFER_BASE;
    assert(itn_addr == (byte*)nTemp && *itn_addr == *(byte*)nTemp);

    //Should allocate sgx-mm-buffer successfully
    itn_addr = sgx_mm_mmap((byte*)0x7ffff0fdb000, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) ;
    nTemp = (0x7ffff0fdb000 & (0x10000000-1)) | SGX_BUFFER_BASE;
    assert(itn_addr == (byte*)nTemp);
    close(fd);

    //0x7ffff7809000     0x7ffff79c9000   0x1c0000        0x0 /lib/x86_64-linux-gnu/libc-2.23.so
    itn_addr = sgx_mm_mmap((byte*)0x71fff79c9000, 0x1c0000, PROT_READ, MAP_PRIVATE, -1, 0) ;
    //should be failed
    assert(itn_addr == (byte*)0x71fff79c9000);


    //open("/lib/x86_64-linux-gnu/libselinux.so.1", O_RDONLY|O_CLOEXEC) = 3
    fn = "/lib/x86_64-linux-gnu/libselinux.so.1";
    fd = open(fn, O_RDONLY|O_CLOEXEC);
    if (fd == -1) {
        printf("Faied to open %s\n", fn);
        return;
    }
    sgx_vma_set_cmt(fd, fn);
    mmap((byte*)0x7ffff0bb5000, 2234080, PROT_READ|PROT_EXEC, MAP_FIXED|MAP_PRIVATE|MAP_DENYWRITE, fd, 0) ;
    itn_addr = sgx_mm_mmap((byte*)0x7ffff0bb5000, 2234080, PROT_READ|PROT_EXEC, MAP_FIXED|MAP_PRIVATE|MAP_DENYWRITE, fd, 0) ;
    //should be success
    nTemp = (0x7ffff0bb5000 & (0x10000000-1)) | SGX_BUFFER_BASE;
    assert(itn_addr == (byte*)nTemp && *itn_addr == *(byte*)nTemp);

    mprotect((byte*)0x7ffff0bd4000, 2093056, PROT_NONE) ;
    sgx_mm_mprotect((byte*)0x7ffff0bd4000, 2093056, PROT_NONE);


    mmap((byte*)0x7ffff0dd3000, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, fd, 0x1e000) ;
    mmap((byte*)0x7ffff0dd5000, 5856, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0) ;

    sgx_mm_mmap((byte*)0x7ffff0dd3000, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_DENYWRITE, fd, 0x1e000) ;
    sgx_mm_mmap((byte*)0x7ffff0dd5000, 5856, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0) ;
    close(fd);

    mmap((byte*)0x7ffff0fd9000, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) ;
    mmap((byte*)0x7ffff0fd7000, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) ;
    mprotect((byte*)0x7ffff0bab000, 16384, PROT_READ) ;
    mprotect((byte*)0x7ffff0371000, 4096, PROT_READ) ;
    mprotect((byte*)0x7ffff0579000, 4096, PROT_READ) ;
    mprotect((byte*)0x7ffff07e9000, 4096, PROT_READ) ;
    mprotect((byte*)0x7ffff0dd3000, 4096, PROT_READ) ;
    mprotect((byte*)0x61d000, 4096, PROT_READ)     ;
    mprotect((byte*)0x7ffff0ffc000, 4096, PROT_READ);

    sgx_mm_mmap((byte*)0x7ffff0fd9000, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) ;
    sgx_mm_mmap((byte*)0x7ffff0fd7000, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) ;
    sgx_mm_mprotect((byte*)0x7ffff0bab000, 16384, PROT_READ) ;
    sgx_mm_mprotect((byte*)0x7ffff0371000, 4096, PROT_READ) ;
    sgx_mm_mprotect((byte*)0x7ffff0579000, 4096, PROT_READ) ;
    sgx_mm_mprotect((byte*)0x7ffff07e9000, 4096, PROT_READ) ;
    sgx_mm_mprotect((byte*)0x7ffff0dd3000, 4096, PROT_READ) ;
    sgx_mm_mprotect((byte*)0x61d000, 4096, PROT_READ)     ;
    sgx_mm_mprotect((byte*)0x7ffff0ffc000, 4096, PROT_READ);


    sgx_procmaps_read_start();
    while (sgx_procmaps_read_next(buf, BUFFER_LEN) != 0) {
        printf("%s", buf);
    }

    sgx_procmaps_read_stop();

    printf("%s end successfully\n", __FUNCTION__);
}

int main(int argc, char *argv[])
{
    void *handle;
    char *error;

    handle = dlopen(LIBDR_SO, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "%s\n", dlerror());
        exit(EXIT_FAILURE);
    }

    dlerror();    /* Clear any existing error */

    sgx_mm_init = (init_ft) dlsym(handle, "sgx_mm_init");
    error = dlerror();
    if (error != NULL) {
        fprintf(stderr, "%s\n", error);
        exit(EXIT_FAILURE);
    }

    sgx_mm_mmap = (mmap_ft) dlsym(handle, "sgx_mm_mmap");
    error = dlerror();
    if (error != NULL) {
        fprintf(stderr, "%s\n", error);
        exit(EXIT_FAILURE);
    }

    sgx_mm_munmap = (munmap_ft) dlsym(handle, "sgx_mm_munmap");
    error = dlerror();
    if (error != NULL) {
        fprintf(stderr, "%s\n", error);
        exit(EXIT_FAILURE);
    }

    sgx_mm_mprotect = (mprotect_ft) dlsym(handle, "sgx_mm_mprotect");
    error = dlerror();
    if (error != NULL) {
        fprintf(stderr, "%s\n", error);
        exit(EXIT_FAILURE);
    }


    sgx_procmaps_read_start = (start_ft) dlsym(handle, "sgx_procmaps_read_start");
    error = dlerror();
    if (error != NULL) {
        fprintf(stderr, "%s\n", error);
        exit(EXIT_FAILURE);
    }


    sgx_procmaps_read_stop = (stop_ft) dlsym(handle, "sgx_procmaps_read_stop");
    error = dlerror();
    if (error != NULL) {
        fprintf(stderr, "%s\n", error);
        exit(EXIT_FAILURE);
    }

    sgx_procmaps_read_next = (next_ft) dlsym(handle, "sgx_procmaps_read_next");
    error = dlerror();
    if (error != NULL) {
        fprintf(stderr, "%s\n", error);
        exit(EXIT_FAILURE);
    }

    sgx_mm_getmm = (getmm_ft) dlsym(handle, "sgx_mm_getmm");

    sgx_vma_set_cmt =  (setcmt_ft) dlsym(handle, "sgx_vma_set_cmt");

    test_init_func();

    test_mprotect();

    test_munmap();
    test_mmap();
    test_mmap2();

    //sgx_mm_test_ls();
    sgx_mm_test_ls_2();

    dlclose(handle);
    exit(EXIT_SUCCESS);
}
