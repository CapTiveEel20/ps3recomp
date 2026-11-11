/*
 * ps3recomp - one fd space for cellFs (ppu_fs.cpp) and the raw sys_fs syscalls.
 *
 * On a PS3, cellFsOpen and syscall 801 hand out fds from the same kernel
 * table, so a title may open with one and read with the other. Civilization
 * Revolution opens its movies with cellFsOpen and gives the fd to Bink, whose
 * reader seeks and reads with syscalls 818/802. The two tables here were
 * separate, so those syscalls answered EBADF and no movie ever played.
 *
 * Checks: syscall lseek/read/fstat work on a cellFsOpen'd fd, and the two
 * open paths never return the same fd number while both files are open.
 *
 *   tools/test_ppu_fs_syscall_bridge.py
 *   test_fs_syscall_bridge [DIR]   (DIR: where the file goes; default temp)
 */
#include "ppu_fs.cpp"

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

/* ---- link stubs (sys_fs.c itself is linked: it is what is under test) ---- */
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
void ppu_guest_caller(char* out, size_t n) { if (n) out[0] = 0; }
void ydkj_host_bt(const char* tag) { (void)tag; }
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }
const char* cellGame_get_hdd_title_id(void) { return nullptr; }

int64_t sys_fs_open(ppu_context* ctx);
int64_t sys_fs_read(ppu_context* ctx);
int64_t sys_fs_lseek(ppu_context* ctx);
int64_t sys_fs_fstat(ppu_context* ctx);
int64_t sys_fs_close(ppu_context* ctx);
extern char g_sys_fs_root[512];
}
#include "../../platform/ps3_dev_flash.c"
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }

static const uint32_t WORDS = 1024;   /* 4 KiB file; word at offset o holds o */

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static uint8_t mem[1 << 20];
    vm_base = mem; ppu_vm_size = sizeof mem;

    const char* dir = argc > 1 ? argv[1] : getenv("TMPDIR");
    if (!dir) dir = "/tmp";
    char host[1100];
    snprintf(host, sizeof host, "%s/ps3recomp_fs_bridge.bin", dir);
    {
        FILE* f = fopen(host, "wb");
        if (!f) { printf("FAIL: could not create %s\n", host); return 1; }
        for (uint32_t i = 0; i < WORDS; i++) { uint32_t be = __builtin_bswap32(i * 4); fwrite(&be, 4, 1, f); }
        fclose(f);
    }
    ppu_vfs_root = dir;
    snprintf(g_sys_fs_root, sizeof g_sys_fs_root, "%s", dir);
    ppu_fs_register();   /* what boot does: also hooks the syscalls up to cellFs fds */

    /* 1. cellFsOpen, then raw syscalls on that fd. */
    strcpy((char*)mem + 0x100, "/dev_hdd1/ps3recomp_fs_bridge.bin");
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = 0 /* O_RDONLY */; ctx.gpr[5] = 0x200;
    cellFsOpen(&ctx);
    const uint32_t fd = rd32(0x200);
    CHECK((uint32_t)ctx.gpr[3] == 0, "cellFsOpen returns CELL_OK (fd %u)", fd);
    if ((uint32_t)ctx.gpr[3] != 0) { remove(host); return 1; }

    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x40; ctx.gpr[5] = 0 /* SET */; ctx.gpr[6] = 0x208;
    int64_t rc = sys_fs_lseek(&ctx);
    CHECK(rc == 0 && rd64(0x208) == 0x40, "syscall lseek on the cellFs fd: rc=0x%llX pos=0x%llX (want 0, 0x40)",
          (unsigned long long)rc, (unsigned long long)rd64(0x208));

    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x1000; ctx.gpr[5] = 16; ctx.gpr[6] = 0x210;
    rc = sys_fs_read(&ctx);
    CHECK(rc == 0 && rd64(0x210) == 16 && rd32(0x1000) == 0x40 && rd32(0x100C) == 0x4C,
          "syscall read on the cellFs fd: rc=0x%llX n=%llu first word 0x%08X (want 0, 16, 0x00000040)",
          (unsigned long long)rc, (unsigned long long)rd64(0x210), rd32(0x1000));

    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x2000;
    rc = sys_fs_fstat(&ctx);
    CHECK(rc == 0, "syscall fstat on the cellFs fd: rc=0x%llX (want 0)", (unsigned long long)rc);

    /* 2. The raw open path must not reuse that number while it is open... */
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = 0; ctx.gpr[5] = 0x220;
    rc = sys_fs_open(&ctx);
    const uint32_t sfd = rd32(0x220);
    CHECK(rc == 0 && sfd != fd, "syscall open returns fd %u, not the cellFs fd %u", sfd, fd);

    /* 3. ...nor cellFsOpen the syscall's. */
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = 0; ctx.gpr[5] = 0x230;
    cellFsOpen(&ctx);
    const uint32_t fd2 = rd32(0x230);
    CHECK((uint32_t)ctx.gpr[3] == 0 && fd2 != sfd && fd2 != fd,
          "second cellFsOpen returns fd %u, distinct from %u and %u", fd2, fd, sfd);

    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd2; cellFsClose(&ctx);
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = sfd; sys_fs_close(&ctx);
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd;  cellFsClose(&ctx);
    remove(host);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
