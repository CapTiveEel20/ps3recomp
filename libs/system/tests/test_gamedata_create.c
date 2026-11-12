/* cellGameDataCheckCreate2 must report isNewData from the host directory and
 * create /dev_hdd0/game/<dir>/USRDIR when the title's callback accepts
 * (CELL_GAMEDATA_CBRESULT_OK or OK_CANCEL), as the firmware does.
 *
 * Civilization Revolution's callback always answers OK_CANCEL and then copies its
 * data files from the disc into /dev_hdd0/game/BLES00238/USRDIR; on a fresh install
 * that directory did not exist, every copy failed and the game ran without its data.
 * RPCS3 (same title) ends up with dev_hdd0/game/BLES00238/USRDIR created.
 *
 * Windows: clang-cl /I include libs/system/tests/test_gamedata_create.c
 * POSIX:   clang -std=gnu17 -I include libs/system/tests/test_gamedata_create.c -o /tmp/t && /tmp/t */
#include "../cellGame.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
const char* ppu_vfs_root = ".";
ps3_guest_caller_fn g_ps3_guest_caller;

/* Fake title callback: records isNewData, answers s_cb_answer. */
static int32_t  s_cb_answer;
static uint32_t s_seen_is_new;
static int      s_cb_calls;
static void fake_caller(uint32_t opd, uint64_t cb, uint64_t get, uint64_t set,
                        uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)opd; (void)set; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    s_cb_calls++;
    s_seen_is_new = vm_read32((uint32_t)get + 0x004);
    vm_write32((uint32_t)cb + 0x000, (uint32_t)s_cb_answer);
}

static void make_root(char* out, size_t cap)
{
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    snprintf(out, cap, "%sps3recomp-gamedata-%lu", tmp, (unsigned long)GetCurrentProcessId());
#else
    snprintf(out, cap, "/tmp/ps3recomp-gamedata-%d", (int)getpid());
#endif
    for (char* q = out; *q; q++) if (*q == '\\') *q = '/';
    HOST_MKDIR(out);
}

static int call(const char* dir, int32_t answer)
{
    strcpy((char*)vm_base + 0x100, dir);
    s_cb_answer = answer;
    s_cb_calls = 0;
    return cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x200, 0);
}

static int host_dir(const char* root, const char* rel)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    return dir_exists(p);
}

int main(void)
{
    ppu_vm_size = 0x02600000u;
    vm_base = calloc(1, ppu_vm_size); assert(vm_base);
    g_ps3_guest_caller = fake_caller;
    char root[400];
    make_root(root, sizeof root);
    cellGame_set_content_path(root);

    /* Fresh install, title answers OK_CANCEL (Civ Rev): new data reported, USRDIR created. */
    assert(call("BLES00238", CELL_GAMEDATA_CBRESULT_OK_CANCEL) == CELL_OK);
    assert(s_cb_calls == 1);
    assert(s_seen_is_new == 1);
    assert(host_dir(root, "BLES00238/USRDIR"));
    puts("PASS fresh install + OK_CANCEL: isNewData=1, USRDIR created");

    /* Second boot: data exists now. */
    assert(call("BLES00238", CELL_GAMEDATA_CBRESULT_OK_CANCEL) == CELL_OK);
    assert(s_seen_is_new == 0);
    puts("PASS existing data: isNewData=0");

    /* Title answers OK: also created. */
    assert(call("TEST00001", CELL_GAMEDATA_CBRESULT_OK) == CELL_OK);
    assert(s_seen_is_new == 1);
    assert(host_dir(root, "TEST00001/USRDIR"));
    puts("PASS fresh install + OK: USRDIR created");

    /* Title reports an error (negative result): nothing created. */
    assert(call("TEST00002", -1) != CELL_OK);
    assert(!host_dir(root, "TEST00002"));
    puts("PASS callback error: nothing created");

    /* A directory name that would leave /dev_hdd0/game is refused before the callback. */
    assert(call("../escape", CELL_GAMEDATA_CBRESULT_OK) == CELL_GAME_ERROR_PARAM);
    assert(s_cb_calls == 0);
    assert(!host_dir(root, "../escape"));
    assert(call("A/B", CELL_GAMEDATA_CBRESULT_OK) == CELL_GAME_ERROR_PARAM);
    assert(!host_dir(root, "A"));
    puts("PASS path-escaping directory names refused");

    free(vm_base);
    puts("ALL PASS");
    return 0;
}
