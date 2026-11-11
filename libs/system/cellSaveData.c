/*
 * ps3recomp - cellSaveData HLE implementation
 *
 * Game save data management with callback-driven flow.
 * Save data is stored under: {root}/gamedata/dev_hdd0/home/00000001/savedata/{dirName}/
 */

#include "cellSaveData.h"
#include "ps3_psf.h"
#include "ps3emu/guest_call.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../runtime/memory/vm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

/* glibc's <sys/stat.h> exposes st_atime/st_mtime/st_ctime as macros (st_atim.tv_sec
 * etc.) that collide with the identically named members of CellSaveData*Stat; undef
 * them so the struct fields resolve, and read host times via the explicit timespec
 * fields in the POSIX branches below. */
#ifdef st_atime
#  undef st_atime
#endif
#ifdef st_mtime
#  undef st_mtime
#endif
#ifdef st_ctime
#  undef st_ctime
#endif

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  define HOST_MKDIR(p) _mkdir(p)
#  define HOST_STAT     _stat64
#  define HOST_STAT_T   struct __stat64
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <sys/types.h>
#  define HOST_MKDIR(p) mkdir(p, 0755)
#  define HOST_STAT     stat
#  define HOST_STAT_T   struct stat
#endif

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

static char s_save_root[1024] = "./gamedata/dev_hdd0/home/00000001/savedata";

/* $PS3_GAMEDATA_ROOT replaces the cwd-relative "./gamedata", so a port can keep
 * the player's saves in one fixed place (e.g. ~/Library/Application Support/...)
 * no matter which directory the game was launched from. */
static void adopt_gamedata_root(void)
{
    static int s_done = 0;
    if (s_done) return;
    s_done = 1;
    const char* env = getenv("PS3_GAMEDATA_ROOT");
    if (env && *env)
        snprintf(s_save_root, sizeof(s_save_root), "%s/dev_hdd0/home/00000001/savedata", env);
}

/* Ensure directory (and parents) exist */
static void ensure_dirs(const char* path)
{
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            HOST_MKDIR(tmp);
            *p = saved;
        }
    }
    HOST_MKDIR(tmp);
}

static void build_save_path(char* buf, size_t buf_size, const char* dirName)
{
    adopt_gamedata_root();
    snprintf(buf, buf_size, "%s/%s", s_save_root, dirName);
#ifdef _WIN32
    for (char* p = buf; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif
}

/* The generic HLE adapter passes PPC register values raw, so string args
 * arrive as GUEST addresses (< 4GB). Translate to a host pointer once at
 * the entry point; the internal helpers all expect host char*. */
static const char* savedata_host_str(const char* p)
{
    uintptr_t v = (uintptr_t)p;
    if (v && v < 0x100000000ull)
        return (const char*)(vm_base + (uint32_t)v);
    return p;
}

static int dir_exists(const char* path)
{
    HOST_STAT_T st;
    if (HOST_STAT(path, &st) != 0)
        return 0;
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

/* Does this directory hold an actual save, as opposed to merely existing?
 *
 * Firmware writes a PARAM.SFO into every savedata directory, so its presence is
 * what separates a real save from an empty or half-made one. dir_exists() alone
 * is not that test, and the difference is not academic: an empty
 * BLUS30020-SYSTEM directory made Virtua Fighter 5's stat callback come back
 * CELL_SAVEDATA_CBRESULT_ERR_BROKEN (-3), because we had told it isNewData=0 --
 * "your save is there" -- and then handed it a directory with nothing in it.
 * Reporting isNewData=1 for that case is both true and the answer that lets a
 * title create the save it was looking for. */
static int dir_has_save(const char* path)
{
    if (!dir_exists(path)) return 0;
    char sfo[1024];
    snprintf(sfo, sizeof(sfo), "%s/PARAM.SFO", path);
    HOST_STAT_T st;
    return HOST_STAT(sfo, &st) == 0;
}


/* ---------------------------------------------------------------------------
 * Guest-callback marshalling
 *
 * The funcStat / funcFile callbacks the game registers are GUEST function
 * pointers (OPD addresses), not host-callable. We have to:
 *   1. Allocate guest memory for the StatGet/StatSet/CBResult structures
 *   2. Write the structs in big-endian format
 *   3. Dispatch the OPD via g_ps3_guest_caller with the guest pointers
 *   4. Read the structures back (game may have updated CBResult.result)
 *
 * Layout (sizes in PS3 ABI, all big-endian, 32-bit pointers):
 *   CellSaveDataCBResult:    4 + 4 + 4 + 4 + 4                = 20 bytes
 *   CellSaveDataStatGet:     4 + 4 + 56 + 1552 + 4*5 + 4      = 1640 bytes
 *   CellSaveDataStatSet:     4 + 4 + 4                        = 12 bytes (pointers + reCreateMode)
 *
 * We use a fixed scratch region at 0x024E0000 (128 KB, just before the
 * cmdbuf region at 0x02500000). Only one savedata operation is in flight
 * at a time, so a bump allocator is sufficient.
 * -----------------------------------------------------------------------*/

#define SAVEDATA_SCRATCH_BASE  0x024E0000u
#define SAVEDATA_SCRATCH_SIZE  0x00020000u  /* 128 KB */

static uint32_t s_scratch_base = SAVEDATA_SCRATCH_BASE;
static uint32_t s_scratch_size = SAVEDATA_SCRATCH_SIZE;
static uint32_t s_scratch_next = SAVEDATA_SCRATCH_BASE;

/* Configure a committed, runtime-owned guest window before guest execution.
 * Ports with modules at the legacy default must supply nonoverlapping space. */
s32 cellSaveData_set_scratch_region(u32 base, u32 size)
{
    if (!base || (base & 15) || size < 0x20000u || (u64)base + size > UINT32_MAX)
        return CELL_SAVEDATA_ERROR_PARAM;
    s_scratch_base = base; s_scratch_size = size; s_scratch_next = base;
    return CELL_OK;
}

static void scratch_reset(void) { s_scratch_next = s_scratch_base; }

static uint32_t scratch_alloc(uint32_t size)
{
    /* 16-byte align */
    size = (size + 0xF) & ~0xFu;
    if ((u64)s_scratch_next + size > (u64)s_scratch_base + s_scratch_size)
        return 0;
    uint32_t addr = s_scratch_next;
    s_scratch_next += size;
    /* Zero-init */
    memset(vm_base + addr, 0, size);
    return addr;
}

/* Big-endian struct field writers. Offsets are in PS3 ABI layout.
 *
 * CellSaveDataCBResult (sysutil_savedata.h) is 20 bytes, and invalidMsg is a
 * POINTER, not an inline buffer:
 *   int   result;                +0
 *   unsigned int progressBarInc; +4
 *   int   errNeedSizeKB;         +8
 *   char *invalidMsg;            +12   (32-bit guest pointer, NULL when unused)
 *   void *userdata;              +16   (32-bit guest pointer)
 *
 * userdata is the pointer the title handed to cellSaveData*(..., userdata) and
 * it MUST be echoed back here: a callback typically recovers its own object
 * from it. LBP's funcStat is literally
 *     v = cbResult->userdata; r = (*(v+104))(cbResult, get, set); ...
 * so leaving it zero made it dispatch through guest address 104, read the VM's
 * zero-fill, and report CELL_SAVEDATA_CBRESULT_ERR_NODATA (-4) -- it never
 * reached its own handler at all. */
static void marshal_cbresult_init(uint32_t addr, s32 result, uint32_t userdata_ea)
{
    vm_write32(addr + 0,  (uint32_t)result);     /* result */
    vm_write32(addr + 4,  0);                    /* progressBarInc */
    vm_write32(addr + 8,  0);                    /* errNeedSizeKB */
    vm_write32(addr + 12, 0);                    /* invalidMsg = NULL */
    vm_write32(addr + 16, userdata_ea);          /* userdata (echoed back) */
}

static s32 marshal_cbresult_read_result(uint32_t addr)
{
    return (s32)vm_read32(addr + 0);
}

/* PS3 ABI layout of CellSaveDataStatGet (all big-endian on PPC, 32-bit pointers):
 *   s32 hddFreeSizeKB                     +0
 *   u32 isNewData                         +4
 *   CellSaveDataDirStat dir               +8   (s64*3 + char[32] = 56 bytes)
 *   CellSaveDataSystemFileParam getParam  +64  (128+128+1024+4+4+8+256 = 1552 bytes)
 *   u32 bind                              +1616
 *   s32 sizeKB                            +1620
 *   s32 sysSizeKB                         +1624
 *   u32 fileNum                           +1628
 *   u32 fileListNum                       +1632
 *   ptr fileList                          +1636 (32-bit guest ptr)
 *   reserved[64]                          +1640
 *   total                                 1704 bytes
 */
#define SAVEDATA_STATGET_SIZE   1704u
#define SAVEDATA_STATSET_SIZE   16u    /* setParam ptr + reCreateMode + indicator ptr */
#define SAVEDATA_CBRESULT_SIZE  20u    /* s32 + u32 + s32 + char* + void* */

/* CellSaveDataFileGet is 0x44 (excSize + reserved[64]); CellSaveDataFileSet is
 * 0x30 (fileOperation, reserved, fileType, secureFileId[16], fileName ptr,
 * fileOffset, fileSize, fileBufSize, fileBuf ptr). */
#define SAVEDATA_FILEGET_SIZE   0x44u
#define SAVEDATA_FILESET_SIZE   0x30u

/* Preserve callback-updated userdata and read the requested operation back
 * before scratch is reused. FileGet reports the previous operation's byte count. */
static s32 dispatch_func_file(uint32_t func_opd, uint32_t* userdata_ea,
                              u32 exc_size, CellSaveDataFileSet* out)
{
    if (!g_ps3_guest_caller) return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;
    scratch_reset();
    u32 cb_ea = scratch_alloc(SAVEDATA_CBRESULT_SIZE);
    u32 get_ea = scratch_alloc(SAVEDATA_FILEGET_SIZE);
    u32 set_ea = scratch_alloc(SAVEDATA_FILESET_SIZE);
    if (!cb_ea || !get_ea || !set_ea) return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;
    marshal_cbresult_init(cb_ea, CELL_SAVEDATA_CBRESULT_OK_NEXT, *userdata_ea);
    vm_write32(get_ea, exc_size);
    g_ps3_guest_caller(func_opd, cb_ea, get_ea, set_ea, 0, 0, 0, 0, 0);
    *userdata_ea = vm_read32(cb_ea + 16);
    memset(out, 0, sizeof(*out));
    out->fileOperation = vm_read32(set_ea);
    out->fileType = vm_read32(set_ea + 8);
    memcpy(out->secureFileId, vm_base + set_ea + 12, 16);
    u32 name = vm_read32(set_ea + 28), buffer = vm_read32(set_ea + 44);
    out->fileName = name ? (char*)vm_base + name : NULL;
    out->fileOffset = vm_read32(set_ea + 32);
    out->fileSize = vm_read32(set_ea + 36);
    out->fileBufSize = vm_read32(set_ea + 40);
    out->fileBuf = buffer ? vm_base + buffer : NULL;
    /* fread/fwrite on fileBuf go through the kernel, which cannot fault in a
     * demand-committed guest page (see sys_fs_read). Commit it first. */
    if (buffer && out->fileBufSize) vm_commit(buffer, out->fileBufSize);
    return marshal_cbresult_read_result(cb_ea);
}

static void marshal_statget_init(uint32_t addr, int is_new, const char* dirName,
                                  s32 sizeKB, u32 fileNum)
{
    vm_write32(addr + 0,    1024 * 1024);    /* hddFreeSizeKB = 1 GB */
    vm_write32(addr + 4,    is_new ? 1 : 0); /* isNewData */
    vm_write64(addr + 8,    0);              /* dir.st_atime */
    vm_write64(addr + 16,   0);              /* dir.st_mtime */
    vm_write64(addr + 24,   0);              /* dir.st_ctime */
    if (dirName) {
        size_t len = strnlen(dirName, CELL_SAVEDATA_DIRNAME_SIZE - 1);
        memcpy(vm_base + addr + 32, dirName, len);
        ((char*)vm_base)[addr + 32 + len] = 0;
    }
    /* getParam (+64..+1615) left zero — no PARAM.SFO data */
    vm_write32(addr + 1616, 0);              /* bind */
    vm_write32(addr + 1620, (uint32_t)sizeKB); /* sizeKB */
    vm_write32(addr + 1624, 0);              /* sysSizeKB */
    vm_write32(addr + 1628, fileNum);        /* fileNum */
    vm_write32(addr + 1632, 0);              /* fileListNum */
    vm_write32(addr + 1636, 0);              /* fileList ptr (NULL — no files) */
}

/* Dispatch funcStat callback via the guest-caller hook.
 * Returns the cbResult.result value the callback wrote, or
 * CELL_SAVEDATA_CBRESULT_ERR_FAILURE if no dispatcher is installed. */
static s32 dispatch_func_stat_full(uint32_t func_opd, int is_new, const char* dirName,
                                   uint32_t* userdata_ea, const CellSaveDataStatGet* input,
                                   CellSaveDataStatSet* output)
{
    if (!g_ps3_guest_caller) return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;

    scratch_reset();
    uint32_t cb_ea  = scratch_alloc(SAVEDATA_CBRESULT_SIZE);
    uint32_t get_ea = scratch_alloc(SAVEDATA_STATGET_SIZE);
    uint32_t set_ea = scratch_alloc(SAVEDATA_STATSET_SIZE);
    if (!cb_ea || !get_ea || !set_ea) {
        printf("[cellSaveData] scratch alloc failed\n");
        return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;
    }

    marshal_cbresult_init(cb_ea, CELL_SAVEDATA_CBRESULT_OK_NEXT, *userdata_ea);
    marshal_statget_init(get_ea, is_new, dirName, 0, 0);
    if (input) {
        vm_write32(get_ea, (u32)input->hddFreeSizeKB);
        vm_write64(get_ea + 8, (u64)input->dir.st_atime);
        vm_write64(get_ea + 16, (u64)input->dir.st_mtime);
        vm_write64(get_ea + 24, (u64)input->dir.st_ctime);
        memcpy(vm_base + get_ea + 64, &input->getParam, 1552);
        vm_write32(get_ea + 64 + 1280, input->getParam.attribute);
        vm_write32(get_ea + 1616, input->bind);
        vm_write32(get_ea + 1620, (u32)input->sizeKB);
        vm_write32(get_ea + 1624, (u32)input->sysSizeKB);
        vm_write32(get_ea + 1628, input->fileNum);
        vm_write32(get_ea + 1632, input->fileListNum);
        u32 files = input->fileListNum ? scratch_alloc(input->fileListNum * 56) : 0;
        if (input->fileListNum && !files) return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;
        vm_write32(get_ea + 1636, files);
        for (u32 i = 0; i < input->fileListNum; i++) {
            const CellSaveDataFileStat* f = input->fileList + i;
            u32 dst = files + i * 56;
            vm_write32(dst, f->fileType); vm_write64(dst + 8, f->st_size);
            vm_write64(dst + 16, (u64)f->st_atime);
            vm_write64(dst + 24, (u64)f->st_mtime);
            vm_write64(dst + 32, (u64)f->st_ctime);
            memcpy(vm_base + dst + 40, f->fileName, CELL_SAVEDATA_FILENAME_SIZE);
        }
    }
    /* StatSet zero-init by scratch_alloc */

    printf("[cellSaveData] dispatching funcStat OPD=0x%08X (cb=0x%X get=0x%X set=0x%X, isNew=%d, userdata=0x%08X)\n",
           func_opd, cb_ea, get_ea, set_ea, is_new, *userdata_ea);
    g_ps3_guest_caller(func_opd, cb_ea, get_ea, set_ea, 0, 0, 0, 0, 0);

    *userdata_ea = vm_read32(cb_ea + 16);
    if (output) {
        u32 params = vm_read32(set_ea);
        output->setParam = params ? (CellSaveDataSystemFileParam*)(vm_base + params) : NULL;
        output->reCreateMode = vm_read32(set_ea + 4);
        output->indicator = NULL;
    }
    s32 result = marshal_cbresult_read_result(cb_ea);
    printf("[cellSaveData] funcStat returned cbResult.result=%d\n", result);
    return result;
}

/* Dispatch a List/Fixed selection callback.
 *
 * The four List/Fixed entry points used to call the guest OPD as a HOST
 * function pointer with HOST struct pointers -- `funcList(&cbResult, &listGet,
 * &listSet)`. That is an immediate access violation the first time a title
 * actually reaches one, and none did until Virtua Fighter 5 called
 * cellSaveDataFixedLoad: rip == the guest OPD address, 0x00691F30.
 *
 * ponytail: this dispatches through the guest caller with ZEROED guest
 * structures rather than marshalling the directory list. With no save data on
 * the VFS the list is empty anyway, which is the honest answer, and a title
 * sees "nothing to select" and proceeds with defaults. Marshal CellSaveData-
 * ListGet/ListSet properly when a title needs to pick an existing save; the
 * layouts are the only missing piece, and dispatch_func_stat next door is the
 * shape to copy. */
/* List callback. The directory list is marshalled into the title's own
 * CellSaveDataSetBuf (SDK: "dirList uses CellSaveDataSetBuf *buf"), and the
 * directory the title picks is read back out of the GUEST CellSaveDataListSet:
 * the callers used to inspect a host listSet nothing ever filled, so every
 * List{Save,Load}2 with no existing saves returned NODATA -- Tornado
 * Outbreak's "New Game" then showed "Unable to save game data".
 *
 * Guest layouts (sysutil_savedata.h, 32-bit pointers):
 *   ListGet  { dirNum +0; dirListNum +4; dirList* +8; reserved[64] }
 *   ListSet  { focusPosition +0; focusDirName* +4; fixedListNum +8;
 *              fixedList* +12; newData* +16; reserved* +20 }
 *   NewData  { iconPosition +0; dirName* +4; icon* +8; reserved* +12 }
 *   DirList  { dirName[32]; listParam[8]; reserved[8] }  (48 bytes)
 *   SetBuf   { dirListMax +0; fileListMax +4; reserved[6]; bufSize +32; buf* +36 }
 *
 * `selected` receives the chosen directory ("" when none): the first listed
 * entry, else the new-save name, else the focus name. For a save with nothing
 * listed the new-save name wins -- headless, there is no user to pick. */
static void read_param_sfo(const char* save_path, CellSaveDataSystemFileParam* param);

/* The user cancelled the save/load picker: List{Save,Load}2 return
 * CELL_SAVEDATA_RET_CANCEL, as when the PS3 user backs out of the list. */
#define SAVEDATA_PICK_CANCELLED  0x7FFF0001
#ifndef CELL_SAVEDATA_RET_CANCEL
#define CELL_SAVEDATA_RET_CANCEL 1
#endif

#ifdef _WIN32
/* Host stand-in for the PS3 system save/load list. On a PS3, List{Save,Load}2
 * show the title's candidates (its fixed list, plus "new data" for a save)
 * and the USER picks. Headless, the first entry always won, so every save in
 * Civilization Revolution overwrote LISTSAVE0 and every load read it back.
 * Shown only when there is a real choice. Keyboard (arrows, Enter, Esc),
 * mouse (double-click) or an XInput pad (d-pad, A/START = OK, B = cancel).
 * SAVEDATA_PICKER=0 restores the old first-entry behaviour. */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } sd_xi_pad;   /* XINPUT_GAMEPAD */
typedef struct { DWORD packet; sd_xi_pad pad; } sd_xi_state;                     /* XINPUT_STATE */
typedef DWORD (WINAPI *sd_xi_get_fn)(DWORD, sd_xi_state*);

typedef struct { HWND list; int done; int choice; } sd_pick_state;

static LRESULT CALLBACK sd_pick_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    sd_pick_state* st = (sd_pick_state*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case DM_GETDEFID: return MAKELONG(IDOK, DC_HASDEFID);
    case WM_COMMAND:
        if (!st) break;
        if (LOWORD(wp) == IDOK || (LOWORD(wp) == 100 && HIWORD(wp) == LBN_DBLCLK)) {
            st->choice = (int)SendMessageW(st->list, LB_GETCURSEL, 0, 0); st->done = 1; return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { st->choice = -1; st->done = 1; return 0; }
        break;
    case WM_CLOSE:
        if (st) { st->choice = -1; st->done = 1; }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static BOOL CALLBACK sd_find_game_window(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    *(HWND*)lp = h;
    return FALSE;
}

/* Returns the chosen index, or -1 when cancelled. `prompt` (may be NULL) is
 * a message shown above the list. */
static int savedata_host_pick(const wchar_t* caption, const wchar_t* prompt,
                              wchar_t labels[][160], int n, int def)
{
    static sd_xi_get_fn xi_get; static int xi_tried;
    if (!xi_tried) {
        static const wchar_t* const dlls[] = { L"XInput1_4.dll", L"XInput1_3.dll", L"XInput9_1_0.dll" };
        xi_tried = 1;
        for (int k = 0; k < 3 && !xi_get; k++) {
            HMODULE m = LoadLibraryExW(dlls[k], NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (m) xi_get = (sd_xi_get_fn)(void (*)(void))GetProcAddress(m, "XInputGetState");
        }
    }
    HINSTANCE inst = GetModuleHandleW(NULL);
    static int registered;
    if (!registered) {
        WNDCLASSW wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = sd_pick_proc; wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ps3recomp_savedata_picker";
        registered = RegisterClassW(&wc) != 0;
    }
    HWND owner = NULL;
    EnumWindows(sd_find_game_window, (LPARAM)&owner);
    const int W = 560, H = 340;
    RECT orc = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (owner) GetWindowRect(owner, &orc);
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT | WS_EX_TOPMOST,
                             L"ps3recomp_savedata_picker", caption,
                             WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             (orc.left + orc.right - W) / 2, (orc.top + orc.bottom - H) / 2, W, H,
                             owner, NULL, inst, NULL);
    if (!w) return def;
    HFONT font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");
    RECT rc; GetClientRect(w, &rc);
    sd_pick_state st = { NULL, 0, def };
    const int top = prompt ? 124 : 12;
    HWND text = prompt ? CreateWindowExW(0, L"STATIC", prompt, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                         12, 12, rc.right - 24, top - 20, w, NULL, inst, NULL) : NULL;
    st.list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                              12, top, rc.right - 24, rc.bottom - 58 - top, w, (HMENU)100, inst, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              rc.right - 232, rc.bottom - 46, 106, 34, w, (HMENU)IDOK, inst, NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  rc.right - 118, rc.bottom - 46, 106, 34, w, (HMENU)IDCANCEL, inst, NULL);
    if (font) {
        SendMessageW(st.list, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(ok, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);
        if (text) SendMessageW(text, WM_SETFONT, (WPARAM)font, TRUE);
    }
    for (int i = 0; i < n; i++) SendMessageW(st.list, LB_ADDSTRING, 0, (LPARAM)labels[i]);
    SendMessageW(st.list, LB_SETCURSEL, (WPARAM)def, 0);
    SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)&st);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    SetFocus(st.list);

    /* Pad buttons already held when the picker opens (the press that chose
     * "save" in the game) must not count as a choice. */
    WORD prev = 0;
    sd_xi_state xs;
    if (xi_get) for (DWORD p = 0; p < 4; p++)
        if (xi_get(p, &xs) == ERROR_SUCCESS) { prev |= xs.pad.buttons; }
    while (!st.done) {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 30, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (!IsDialogMessageW(w, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
        }
        if (!xi_get || st.done) continue;
        WORD now = 0;
        for (DWORD p = 0; p < 4; p++)
            if (xi_get(p, &xs) == ERROR_SUCCESS) now |= xs.pad.buttons;
        const WORD hit = (WORD)(now & ~prev);
        prev = now;
        int cur = (int)SendMessageW(st.list, LB_GETCURSEL, 0, 0);
        if (hit & 0x0001) SendMessageW(st.list, LB_SETCURSEL, (WPARAM)(cur > 0 ? cur - 1 : 0), 0);      /* up */
        if (hit & 0x0002) SendMessageW(st.list, LB_SETCURSEL, (WPARAM)(cur < n - 1 ? cur + 1 : n - 1), 0); /* down */
        if (hit & (0x1000 | 0x0010)) { st.choice = (int)SendMessageW(st.list, LB_GETCURSEL, 0, 0); st.done = 1; } /* A, START */
        if (hit & 0x2000) { st.choice = -1; st.done = 1; }                                                 /* B */
    }
    SetWindowLongPtrW(w, GWLP_USERDATA, 0);
    DestroyWindow(w);
    if (font) DeleteObject(font);
    if (owner) SetForegroundWindow(owner);
    return st.choice < 0 ? -1 : (st.choice < n ? st.choice : def);
}

/* Yes/No warning in the same window style, so the pad works too. "No" is the
 * default: a held or repeated OK press can never confirm by accident. */
static int savedata_host_confirm(const wchar_t* caption, const wchar_t* prompt, const wchar_t* yes)
{
    wchar_t opts[2][160];
    wcscpy(opts[0], L"No, go back");
    wcsncpy(opts[1], yes, 159); opts[1][159] = 0;
    return savedata_host_pick(caption, prompt, opts, 2, 0) == 1;
}

/* "Francesi, 2800 a.C., Capitano   (11/11/2026 00:31)" from the save's
 * PARAM.SFO and its folder time; the folder name when there is no subtitle. */
static void savedata_pick_label(const char* dir, wchar_t out[160])
{
    char path[1200], text[400];
    snprintf(path, sizeof path, "%s/%s", s_save_root, dir);
    CellSaveDataSystemFileParam* p = (CellSaveDataSystemFileParam*)calloc(1, sizeof *p);
    const char* name = dir;
    if (p) {
        read_param_sfo(path, p);
        size_t k = strlen(p->subTitle);
        while (k && p->subTitle[k - 1] == ' ') p->subTitle[--k] = '\0';
        if (k) name = p->subTitle;
        else if (p->title[0]) name = p->title;
    }
    char when[32] = "";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    for (char* c = path; *c; c++) if (*c == '/') *c = '\\';
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
        FILETIME lt; SYSTEMTIME s;
        if (FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt) && FileTimeToSystemTime(&lt, &s))
            snprintf(when, sizeof when, "   (%02u/%02u/%04u %02u:%02u)",
                     s.wDay, s.wMonth, s.wYear, s.wHour, s.wMinute);
    }
    snprintf(text, sizeof text, "%s%s", name, when);
    if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, out, 160))
        MultiByteToWideChar(CP_ACP, 0, text, -1, out, 160);
    out[159] = 0;
    free(p);
}
#endif

static s32 dispatch_func_select(uint32_t func_opd, const CellSaveDataDirList* dirList,
                                uint32_t dirCount, uint32_t setBuf_ea, int is_save,
                                uint32_t userdata_ea, const char* who, char selected[64])
{
    selected[0] = '\0';
    if (!g_ps3_guest_caller) return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;

    scratch_reset();
    uint32_t cb_ea  = scratch_alloc(SAVEDATA_CBRESULT_SIZE);
    uint32_t get_ea = scratch_alloc(SAVEDATA_STATGET_SIZE);
    uint32_t set_ea = scratch_alloc(SAVEDATA_STATSET_SIZE);
    if (!cb_ea || !get_ea || !set_ea) return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;
    for (uint32_t i = 0; i < 24; i += 4) vm_write32(set_ea + i, 0);

    uint32_t buf_ea = setBuf_ea ? vm_read32(setBuf_ea + 36) : 0;
    uint32_t cap = setBuf_ea ? vm_read32(setBuf_ea + 32) / 48u : 0;
    if (setBuf_ea && vm_read32(setBuf_ea + 0) < cap) cap = vm_read32(setBuf_ea + 0);
    uint32_t listed = buf_ea ? (dirCount < cap ? dirCount : cap) : 0;
    for (uint32_t i = 0; i < listed; i++)
        for (uint32_t k = 0; k < 48; k++)
            vm_write8(buf_ea + i * 48 + k, (uint8_t)((const char*)&dirList[i])[k]);

    marshal_cbresult_init(cb_ea, CELL_SAVEDATA_CBRESULT_OK_NEXT, userdata_ea);
    vm_write32(get_ea + 0, dirCount);        /* dirNum     */
    vm_write32(get_ea + 4, listed);          /* dirListNum */
    vm_write32(get_ea + 8, listed ? buf_ea : 0);

    printf("[cellSaveData] dispatching %s OPD=0x%08X (dirNum=%u listed=%u)%c",
           who, func_opd, dirCount, listed, 10);
    g_ps3_guest_caller(func_opd, cb_ea, get_ea, set_ea, 0, 0, 0, 0, 0);

    s32 result = marshal_cbresult_read_result(cb_ea);
    const uint32_t fixed_n = vm_read32(set_ea + 8), fixed = vm_read32(set_ea + 12);
    const uint32_t newd = vm_read32(set_ea + 16), focus = vm_read32(set_ea + 4);
    const uint32_t new_name = newd ? vm_read32(newd + 4) : 0;
    uint32_t pick = 0;
    if (is_save && new_name && fixed_n == 0) pick = new_name;
    else if (fixed_n && fixed)               pick = fixed;
    else if (new_name)                       pick = new_name;
    else if (focus)                          pick = focus;
    if (pick) {
        for (int i = 0; i < 63; i++) {
            selected[i] = (char)vm_read8(pick + (uint32_t)i);
            if (!selected[i]) break;
        }
        selected[63] = '\0';
    }
#ifdef _WIN32
    /* Let the user choose when the title offers more than one candidate:
     * its fixed list (else the listed directories), plus new data on a save. */
    const char* pk = getenv("SAVEDATA_PICKER");
    if (result >= 0 && !(pk && pk[0] == '0')) {
        enum { MAXC = 64 };
        static char names[MAXC][64];
        static wchar_t labels[MAXC][160];
        int n = 0, def = 0, new_idx = -1;
        if (is_save && new_name) {
            for (int i = 0; i < 63; i++) { names[n][i] = (char)vm_read8(new_name + (uint32_t)i); if (!names[n][i]) break; }
            names[n][63] = '\0';
            if (names[n][0]) {
                /* A "new" save must not land on an existing folder: if the
                 * title's name is taken, use the first free <prefix><number>. */
                char probe[1200];
                snprintf(probe, sizeof probe, "%s/%s", s_save_root, names[n]);
                if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
                    char base[64]; size_t L = strlen(names[n]);
                    while (L && names[n][L - 1] >= '0' && names[n][L - 1] <= '9') L--;
                    memcpy(base, names[n], L); base[L] = '\0';
                    for (int k = 0; k < 1000; k++) {
                        snprintf(names[n], 64, "%s%d", base, k);
                        snprintf(probe, sizeof probe, "%s/%s", s_save_root, names[n]);
                        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) break;
                    }
                }
                printf("[cellSaveData] %s: new-save folder '%s'%c", who, names[n], 10);
                MultiByteToWideChar(CP_UTF8, 0, "New save", -1, labels[n], 160);
                new_idx = n++;
            }
        }
        if (fixed_n && fixed) {
            for (uint32_t i = 0; i < fixed_n && n < MAXC; i++) {
                for (int k = 0; k < 32; k++) { names[n][k] = (char)vm_read8(fixed + i * 48u + (uint32_t)k); if (!names[n][k]) break; }
                names[n][31] = '\0';
                if (!names[n][0] || (new_idx >= 0 && !strcmp(names[n], names[new_idx]))) continue;
                savedata_pick_label(names[n], labels[n]);
                n++;
            }
        } else {
            for (uint32_t i = 0; i < listed && n < MAXC; i++) {
                strncpy(names[n], dirList[i].dirName, 63); names[n][63] = '\0';
                if (!names[n][0] || (new_idx >= 0 && !strcmp(names[n], names[new_idx]))) continue;
                savedata_pick_label(names[n], labels[n]);
                n++;
            }
        }
        if (n >= 2) {
            for (int i = 0; i < n; i++) if (!strcmp(names[i], selected)) def = i;
            if (is_save && new_idx >= 0) def = new_idx;   /* never overwrite by default */
            int c;
            for (;;) {
                c = savedata_host_pick(is_save ? L"Save game" : L"Load game", NULL, labels, n, def);
                if (c < 0) {
                    printf("[cellSaveData] %s: user cancelled the %s picker%c", who, is_save ? "save" : "load", 10);
                    selected[0] = '\0';
                    return SAVEDATA_PICK_CANCELLED;
                }
                if (!is_save || c == new_idx) break;
                /* Saving over an existing game: warn first; "No" goes back to the list. */
                wchar_t msg[400];
                _snwprintf(msg, 400, L"Overwrite this saved game?\n%ls\nThe old game will be lost.", labels[c]);
                msg[399] = 0;
                if (savedata_host_confirm(L"Overwrite save?", msg, L"Yes, overwrite")) break;
                printf("[cellSaveData] %s: overwrite of '%s' declined%c", who, names[c], 10);
                def = c;
            }
            strncpy(selected, names[c], 63); selected[63] = '\0';
        }
    }
#endif
    printf("[cellSaveData] %s returned cbResult.result=%d fixed=%u newData=%s -> '%s'%c",
           who, result, fixed_n, new_name ? "yes" : "no", selected, 10);
    return result;
}

/* Enumerate save directories matching a prefix. Returns count, fills dirList up to max. */
static u32 enumerate_save_dirs(const char* prefix, CellSaveDataDirList* dirList, u32 max)
{
    u32 count = 0;

    adopt_gamedata_root();
    ensure_dirs(s_save_root);

#ifdef _WIN32
    {
        char search[1024];
        snprintf(search, sizeof(search), "%s\\*", s_save_root);
        for (char* p = search; *p; p++) {
            if (*p == '/') *p = '\\';
        }

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search, &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            return 0;

        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            if (fd.cFileName[0] == '.')
                continue;
            if (prefix && prefix[0] && strncmp(fd.cFileName, prefix, strlen(prefix)) != 0)
                continue;
            if (count < max && dirList) {
                memset(&dirList[count], 0, sizeof(CellSaveDataDirList));
                strncpy(dirList[count].dirName, fd.cFileName,
                        CELL_SAVEDATA_DIRNAME_SIZE - 1);
            }
            count++;
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
    }
#else
    {
        DIR* dp = opendir(s_save_root);
        if (!dp)
            return 0;

        struct dirent* de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            /* Check it's a directory */
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", s_save_root, de->d_name);
            if (!dir_exists(full))
                continue;
            if (prefix && prefix[0] && strncmp(de->d_name, prefix, strlen(prefix)) != 0)
                continue;
            if (count < max && dirList) {
                memset(&dirList[count], 0, sizeof(CellSaveDataDirList));
                strncpy(dirList[count].dirName, de->d_name,
                        CELL_SAVEDATA_DIRNAME_SIZE - 1);
            }
            count++;
        }
        closedir(dp);
    }
#endif

    return count;
}

/* System metadata belongs to the service, not the guest file list. */
static int savedata_system_file(const char* name)
{
    return !strcmp(name, "PARAM.SFO") || !strcmp(name, "PARAM.PFD");
}

static u32 savedata_content_type(const char* name)
{
    if (!strcmp(name, "ICON0.PNG")) return CELL_SAVEDATA_FILETYPE_CONTENT_ICON0;
    if (!strcmp(name, "ICON1.PAM")) return CELL_SAVEDATA_FILETYPE_CONTENT_ICON1;
    if (!strcmp(name, "PIC1.PNG")) return CELL_SAVEDATA_FILETYPE_CONTENT_PIC1;
    if (!strcmp(name, "SND0.AT3")) return CELL_SAVEDATA_FILETYPE_CONTENT_SND0;
    /* Secure-file metadata is not persisted by this simplified backend yet. */
    return CELL_SAVEDATA_FILETYPE_NORMALFILE;
}

/* Which files were written as SECUREFILE. A real save records them in
 * PARAM.PFD; ours keeps one name per line there, and the file stays hidden
 * from the guest's list (savedata_system_file). Without it every file came
 * back as NORMALFILE, and a title that looks for its secure payload in the
 * stat list (The Simpsons Arcade Game) called its own fresh save corrupt. */
static void pfd_path(const char* save_path, char* out, size_t n)
{
    snprintf(out, n, "%s/PARAM.PFD", save_path);
#ifdef _WIN32
    for (char* p = out; *p; p++) if (*p == '/') *p = '\\';
#endif
}

static int pfd_has(const char* save_path, const char* name)
{
    char path[1024], line[64];
    pfd_path(save_path, path, sizeof(path));
    FILE* fp = fopen(path, "rb");
    if (!fp) return 0;
    int found = 0;
    while (!found && fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0;
        found = !strcmp(line, name);
    }
    fclose(fp);
    return found;
}

static void pfd_add(const char* save_path, const char* name)
{
    if (pfd_has(save_path, name)) return;
    char path[1024];
    pfd_path(save_path, path, sizeof(path));
    FILE* fp = fopen(path, "ab");
    if (fp) { fprintf(fp, "%s\n", name); fclose(fp); }
}

static u32 savedata_file_type(const char* save_path, const char* name)
{
    u32 t = savedata_content_type(name);
    return t == CELL_SAVEDATA_FILETYPE_NORMALFILE && pfd_has(save_path, name)
         ? CELL_SAVEDATA_FILETYPE_SECUREFILE : t;
}

/* Enumerate files in a save directory. Returns count, fills fileList up to max. */
static u32 enumerate_save_files(const char* save_path,
                                 CellSaveDataFileStat* fileList, u32 max)
{
    u32 count = 0;

#ifdef _WIN32
    {
        char search[1024];
        snprintf(search, sizeof(search), "%s\\*", save_path);
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search, &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            return 0;

        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || savedata_system_file(fd.cFileName))
                continue;
            if (count < max && fileList) {
                memset(&fileList[count], 0, sizeof(CellSaveDataFileStat));
                fileList[count].fileType = savedata_file_type(save_path, fd.cFileName);
                strncpy(fileList[count].fileName, fd.cFileName,
                        CELL_SAVEDATA_FILENAME_SIZE - 1);
                ULARGE_INTEGER sz;
                sz.HighPart = fd.nFileSizeHigh;
                sz.LowPart  = fd.nFileSizeLow;
                fileList[count].st_size = (u64)sz.QuadPart;
            }
            count++;
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
    }
#else
    {
        DIR* dp = opendir(save_path);
        if (!dp)
            return 0;

        struct dirent* de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.' || savedata_system_file(de->d_name))
                continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", save_path, de->d_name);
            HOST_STAT_T st;
            if (HOST_STAT(full, &st) != 0)
                continue;
#ifdef _WIN32
            if (st.st_mode & _S_IFDIR)
                continue;
#else
            if (S_ISDIR(st.st_mode))
                continue;
#endif
            if (count < max && fileList) {
                memset(&fileList[count], 0, sizeof(CellSaveDataFileStat));
                fileList[count].fileType = savedata_file_type(save_path, de->d_name);
                strncpy(fileList[count].fileName, de->d_name,
                        CELL_SAVEDATA_FILENAME_SIZE - 1);
                fileList[count].st_size = (u64)st.st_size;
#ifdef _WIN32
                fileList[count].st_atime = (s64)st.st_atime;
                fileList[count].st_mtime = (s64)st.st_mtime;
                fileList[count].st_ctime = (s64)st.st_ctime;
#elif defined(__APPLE__)
/* Darwin names the sub-second stat fields st_*timespec, not glibc's st_*tim. */
                fileList[count].st_atime = (s64)st.st_atimespec.tv_sec;
                fileList[count].st_mtime = (s64)st.st_mtimespec.tv_sec;
                fileList[count].st_ctime = (s64)st.st_ctimespec.tv_sec;
#else
                fileList[count].st_atime = (s64)st.st_atim.tv_sec;
                fileList[count].st_mtime = (s64)st.st_mtim.tv_sec;
                fileList[count].st_ctime = (s64)st.st_ctim.tv_sec;
#endif
            }
            count++;
        }
        closedir(dp);
    }
#endif

    return count;
}

/* Execute file callback: read/write/delete files in the save directory */
static s32 process_file_op(const char* save_path, CellSaveDataFileSet* set)
{
    if (!set) return CELL_SAVEDATA_ERROR_PARAM;
    const char* name = set->fileName;
    switch (set->fileType) {
    case CELL_SAVEDATA_FILETYPE_CONTENT_ICON0: name = "ICON0.PNG"; break;
    case CELL_SAVEDATA_FILETYPE_CONTENT_ICON1: name = "ICON1.PAM"; break;
    case CELL_SAVEDATA_FILETYPE_CONTENT_PIC1: name = "PIC1.PNG"; break;
    case CELL_SAVEDATA_FILETYPE_CONTENT_SND0: name = "SND0.AT3"; break;
    case CELL_SAVEDATA_FILETYPE_SECUREFILE:
    case CELL_SAVEDATA_FILETYPE_NORMALFILE: break;
    default: return CELL_SAVEDATA_ERROR_PARAM;
    }
    if (!name || !name[0] || strnlen(name, CELL_SAVEDATA_FILENAME_SIZE) == CELL_SAVEDATA_FILENAME_SIZE ||
        strchr(name, '/') || strchr(name, '\\') || !strcmp(name, ".") || !strcmp(name, "..") ||
        (set->fileOperation != CELL_SAVEDATA_FILEOP_DELETE && !set->fileBuf))
        return CELL_SAVEDATA_ERROR_PARAM;
    char file_path[1024];
    snprintf(file_path, sizeof(file_path), "%s/%s", save_path, name);
#ifdef _WIN32
    for (char* p = file_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    switch (set->fileOperation) {
    case CELL_SAVEDATA_FILEOP_READ: {
        FILE* fp = fopen(file_path, "rb");
        if (!fp) {
            printf("[cellSaveData] file read: cannot open '%s'\n", file_path);
            return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
        }
        if (set->fileOffset > 0) {
#ifdef _MSC_VER
            _fseeki64(fp, (long long)set->fileOffset, SEEK_SET);
#else
            fseeko(fp, (off_t)set->fileOffset, SEEK_SET);
#endif
        }
        size_t read_size = (size_t)set->fileSize;
        if (read_size > set->fileBufSize) read_size = set->fileBufSize;
        size_t got = fread(set->fileBuf, 1, read_size, fp);
        int failed = ferror(fp);
        if (fclose(fp) != 0) failed = 1;
        return failed ? CELL_SAVEDATA_ERROR_ACCESS_ERROR : (s32)got;
    }

    case CELL_SAVEDATA_FILEOP_WRITE:
    case CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC: {
        ensure_dirs(save_path);
        const char* mode;
        if (set->fileOperation == CELL_SAVEDATA_FILEOP_WRITE && set->fileOffset == 0) {
            mode = "wb";
        } else {
            mode = "r+b";
        }
        FILE* fp = fopen(file_path, mode);
        if (!fp) {
            fp = fopen(file_path, "wb");
        }
        if (!fp) {
            printf("[cellSaveData] file write: cannot open '%s'\n", file_path);
            return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
        }
        if (set->fileOffset > 0) {
#ifdef _MSC_VER
            _fseeki64(fp, (long long)set->fileOffset, SEEK_SET);
#else
            fseeko(fp, (off_t)set->fileOffset, SEEK_SET);
#endif
        }
        size_t write_size = (size_t)set->fileSize;
        if (write_size > set->fileBufSize) write_size = set->fileBufSize;
        size_t wrote = fwrite(set->fileBuf, 1, write_size, fp);
        int failed = wrote != write_size;
        if (fclose(fp) != 0) failed = 1;
        if (!failed && set->fileType == CELL_SAVEDATA_FILETYPE_SECUREFILE)
            pfd_add(save_path, name);
        return failed ? CELL_SAVEDATA_ERROR_ACCESS_ERROR : (s32)wrote;
    }

    case CELL_SAVEDATA_FILEOP_DELETE:
        remove(file_path);
        return 0;

    default:
        return 0;
    }
}

/* Write PARAM.SFO as a real binary PSF (layout in ps3_psf.h). The old simplified text file was not a
 * PSF, so RPCS3 and a PS3 rejected the save. */
static void write_param_sfo(const char* save_path, const CellSaveDataSystemFileParam* param)
{
    if (!param) return;

    char sfo_path[1024];
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", save_path);
#ifdef _WIN32
    for (char* p = sfo_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    /* SAVEDATA_DIRECTORY is the last path component of the save directory */
    const char* dir = save_path;
    for (const char* q = save_path; *q; q++)
        if ((*q == '/' || *q == '\\') && q[1]) dir = q + 1;

    /* the guest strings are fixed-size arrays that may lack a terminator: copy with one */
    char title[CELL_SAVEDATA_SYSP_TITLE_SIZE], sub[CELL_SAVEDATA_SYSP_SUBTITLE_SIZE];
    char detail[CELL_SAVEDATA_SYSP_DETAIL_SIZE], lp[CELL_SAVEDATA_SYSP_LPARAM_SIZE];
    snprintf(title, sizeof title, "%.*s", (int)sizeof title - 1, param->title);
    snprintf(sub, sizeof sub, "%.*s", (int)sizeof sub - 1, param->subTitle);
    snprintf(detail, sizeof detail, "%.*s", (int)sizeof detail - 1, param->detail);
    snprintf(lp, sizeof lp, "%.*s", (int)sizeof lp - 1, param->listParam);

    uint8_t buf[4096];
    size_t n = ps3_psf_build(buf, sizeof buf, dir, title, sub, detail, param->attribute, lp);
    if (!n) return;

    FILE* fp = fopen(sfo_path, "wb");
    if (!fp) return;
    fwrite(buf, 1, n, fp);
    fclose(fp);
}

/* Read PARAM.SFO: a binary PSF, or the old simplified text file that earlier builds wrote (existing saves keep
 * loading; the next save rewrites them as PSF). */
static void read_param_sfo(const char* save_path, CellSaveDataSystemFileParam* param)
{
    if (!param) return;
    memset(param, 0, sizeof(CellSaveDataSystemFileParam));

    char sfo_path[1024];
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", save_path);
#ifdef _WIN32
    for (char* p = sfo_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    FILE* fp = fopen(sfo_path, "rb");
    if (!fp) return;

    uint8_t buf[8192];
    size_t got = fread(buf, 1, sizeof buf, fp);
    fclose(fp);
    if (got >= 4 && buf[0] == 0 && buf[1] == 'P' && buf[2] == 'S' && buf[3] == 'F') {
        uint32_t attr = 0;
        if (ps3_psf_parse(buf, got, param->title, sizeof param->title, param->subTitle, sizeof param->subTitle,
                          param->detail, sizeof param->detail, &attr, param->listParam, sizeof param->listParam))
            param->attribute = attr;
        return;
    }

    /* old text format */
    char text[sizeof buf + 1];
    memcpy(text, buf, got); text[got] = 0;
    for (char* line = text; *line; ) {
        char* nl = line; while (*nl && *nl != '\n') nl++;
        char saved = *nl; *nl = 0;
        size_t len = strlen(line);
        while (len > 0 && line[len-1] == '\r') line[--len] = '\0';

        if (strncmp(line, "TITLE=", 6) == 0)
            strncpy(param->title, line + 6, CELL_SAVEDATA_SYSP_TITLE_SIZE - 1);
        else if (strncmp(line, "SUB_TITLE=", 10) == 0)
            strncpy(param->subTitle, line + 10, CELL_SAVEDATA_SYSP_SUBTITLE_SIZE - 1);
        else if (strncmp(line, "DETAIL=", 7) == 0)
            strncpy(param->detail, line + 7, CELL_SAVEDATA_SYSP_DETAIL_SIZE - 1);
        else if (strncmp(line, "ATTRIBUTE=", 10) == 0)
            param->attribute = (u32)atoi(line + 10);
        else if (strncmp(line, "LIST_PARAM=", 11) == 0)
            strncpy(param->listParam, line + 11, CELL_SAVEDATA_SYSP_LPARAM_SIZE - 1);
        if (!saved) break;
        line = nl + 1;
    }
}

/* Core save/load implementation shared by List/Fixed/Auto variants */
/* Saves in flight, for a host-side quit (cellSysutil.c ps3_host_quit): it
 * waits for this to reach 0 so it never cuts a save file short. */
static volatile int s_savedata_busy;
int ps3_savedata_busy(void) { return __atomic_load_n(&s_savedata_busy, __ATOMIC_ACQUIRE); }

static s32 savedata_execute_run(const char* dirName, int is_save,
                                CellSaveDataSetBuf* setBuf,
                                CellSaveDataStatCallback funcStat,
                                CellSaveDataFileCallback funcFile,
                                void* userdata);

static s32 savedata_execute(const char* dirName, int is_save,
                             CellSaveDataSetBuf* setBuf,
                             CellSaveDataStatCallback funcStat,
                             CellSaveDataFileCallback funcFile,
                             void* userdata)
{
    __atomic_add_fetch(&s_savedata_busy, 1, __ATOMIC_ACQ_REL);
    const s32 r = savedata_execute_run(dirName, is_save, setBuf, funcStat, funcFile, userdata);
    __atomic_sub_fetch(&s_savedata_busy, 1, __ATOMIC_ACQ_REL);
    return r;
}

static s32 savedata_execute_run(const char* dirName, int is_save,
                                CellSaveDataSetBuf* setBuf,
                                CellSaveDataStatCallback funcStat,
                                CellSaveDataFileCallback funcFile,
                                void* userdata)
{
    if (!dirName || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    char save_path[1024];
    build_save_path(save_path, sizeof(save_path), dirName);

    int is_new = !dir_has_save(save_path);

    printf("[cellSaveData] %s dir='%s' (new=%d)\n",
           is_save ? "SAVE" : "LOAD", dirName, is_new);

    /* If loading and directory doesn't exist, it's not an error -
       the stat callback will see isNewData=1 and can handle it */

    /* Prepare stat get */
    /* setBuf is a GUEST address, not a host pointer. Dereferencing it faulted
     * the moment the title autosaved: cellSaveDataAutoSave2 -> savedata_execute
     * read setBuf->fileListMax off a guest stack address and took an access
     * violation (0xD00DCF84 = r1+0x90). Read it out of guest memory, the way
     * the dirListMax read below already does. fileListMax is at +4. */
    u32 file_list_max = setBuf ? vm_read32((u32)(uintptr_t)setBuf + 4) : 64;
    if (file_list_max > CELL_SAVEDATA_LISTITEM_MAX) return CELL_SAVEDATA_ERROR_PARAM;
    CellSaveDataFileStat* fileList = NULL;
    u32 fileNum = 0;

    if (file_list_max > 0) {
        fileList = (CellSaveDataFileStat*)calloc(file_list_max, sizeof(CellSaveDataFileStat));
        if (!fileList) return CELL_SAVEDATA_ERROR_INTERNAL;
        if (!is_new) {
            fileNum = enumerate_save_files(save_path, fileList, file_list_max);
        }
    }

    CellSaveDataStatGet statGet;
    memset(&statGet, 0, sizeof(statGet));
    statGet.hddFreeSizeKB = 1024 * 1024; /* report 1GB free */
    statGet.isNewData = is_new ? 1 : 0;
    strncpy(statGet.dir.dirName, dirName, CELL_SAVEDATA_DIRNAME_SIZE - 1);

    if (!is_new) {
        HOST_STAT_T hst;
        if (HOST_STAT(save_path, &hst) == 0) {
#ifdef _WIN32
            statGet.dir.st_atime = (s64)hst.st_atime;
            statGet.dir.st_mtime = (s64)hst.st_mtime;
            statGet.dir.st_ctime = (s64)hst.st_ctime;
#elif defined(__APPLE__)
/* Darwin names the sub-second stat fields st_*timespec, not glibc's st_*tim. */
            statGet.dir.st_atime = (s64)hst.st_atimespec.tv_sec;
            statGet.dir.st_mtime = (s64)hst.st_mtimespec.tv_sec;
            statGet.dir.st_ctime = (s64)hst.st_ctimespec.tv_sec;
#else
            statGet.dir.st_atime = (s64)hst.st_atim.tv_sec;
            statGet.dir.st_mtime = (s64)hst.st_mtim.tv_sec;
            statGet.dir.st_ctime = (s64)hst.st_ctim.tv_sec;
#endif
        }
        read_param_sfo(save_path, &statGet.getParam);
    }

    statGet.fileNum = fileNum;
    statGet.fileListNum = fileNum < file_list_max ? fileNum : file_list_max;
    statGet.fileList = fileList;
    statGet.bind = 0;
    statGet.sizeKB = 0;
    statGet.sysSizeKB = 0;

    /* Call stat callback */
    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;

    CellSaveDataStatSet statSet;
    memset(&statSet, 0, sizeof(statSet));

    /* Same guest-OPD-as-host-pointer bug as funcFile below: this jumped into
     * guest memory as host code -- ACCESS VIOLATION: execute at host 0x34C250
     * -- the moment Tokyo Jungle autosaved. Route it through the marshalling
     * dispatcher, which builds StatGet/StatSet in guest memory. */
    u32 callback_userdata = (u32)(uintptr_t)userdata;
    cbResult.result = dispatch_func_stat_full((uint32_t)(uintptr_t)funcStat, is_new,
                         dirName, &callback_userdata, &statGet, &statSet);

    if (cbResult.result < 0) {
        printf("[cellSaveData] stat callback returned error %d\n", cbResult.result);
        free(fileList);
        if (cbResult.result == CELL_SAVEDATA_CBRESULT_ERR_NODATA)
            return CELL_SAVEDATA_ERROR_NODATA;
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    /* Write PARAM.SFO if stat set provided params and we're saving */
    if (is_save && statSet.setParam) {
        ensure_dirs(save_path);
        CellSaveDataSystemFileParam params;
        memcpy(&params, statSet.setParam, sizeof(params));
        params.attribute = ps3_bswap32(params.attribute);
        write_param_sfo(save_path, &params);
    }

    /* Each NEXT callback requests one operation. LAST ends the sequence;
     * its zero-initialized FileSet is not an implicit read request. */
    if (funcFile && cbResult.result == CELL_SAVEDATA_CBRESULT_OK_NEXT) {
        u32 exc_size = 0;
        while (1) {
            CellSaveDataFileSet fileSet;
            cbResult.result = dispatch_func_file((u32)(uintptr_t)funcFile,
                                                 &callback_userdata, exc_size, &fileSet);
            if (cbResult.result != CELL_SAVEDATA_CBRESULT_OK_NEXT) break;
            s32 exc = process_file_op(save_path, &fileSet);
            printf("[cellSaveData] file op=%u type=%u name='%s' off=%u size=%u buf=%u -> %d\n",
                   fileSet.fileOperation, fileSet.fileType,
                   fileSet.fileName ? fileSet.fileName : "", fileSet.fileOffset,
                   fileSet.fileSize, fileSet.fileBufSize, exc);
            if (exc < 0) { free(fileList); return exc; }
            exc_size = (u32)exc;
        }
    }

    free(fileList);

    if (cbResult.result < 0) {
        if (cbResult.result == CELL_SAVEDATA_CBRESULT_ERR_NODATA)
            return CELL_SAVEDATA_ERROR_NODATA;
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    printf("[cellSaveData] %s complete for '%s'\n",
           is_save ? "SAVE" : "LOAD", dirName);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellSaveDataListSave2(u32 version, CellSaveDataSetList* setList,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataListCallback funcList,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    printf("[cellSaveData] ListSave2(version=%u)\n", version);

    if (!setList || !setBuf || !funcList || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    /* Enumerate existing save dirs */
    /* CellSaveDataSetList is { u32 sortType; u32 sortOrder; char* dirNamePrefix; }
     * and CellSaveDataSetBuf starts with u32 dirListMax -- guest layout, so the
     * prefix EA is the word at +8 and needs translating before it is used as a
     * host string. The host structs put dirNamePrefix at +8 as an 8-byte
     * pointer, so a cast would have read half of it plus padding. */
    u32 dir_max = vm_read32((u32)(uintptr_t)setBuf + 0);
    if (dir_max == 0) dir_max = CELL_SAVEDATA_DIRLIST_MAX;
    CellSaveDataDirList* dirList = (CellSaveDataDirList*)calloc(dir_max, sizeof(CellSaveDataDirList));

    u32 dirCount = enumerate_save_dirs(
        savedata_host_str((const char*)(uintptr_t)vm_read32((u32)(uintptr_t)setList + 8)) ?
            savedata_host_str((const char*)(uintptr_t)vm_read32((u32)(uintptr_t)setList + 8)) : "",
        dirList, dir_max);

    /* Call list callback */
    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;

    CellSaveDataListGet listGet;
    memset(&listGet, 0, sizeof(listGet));
    listGet.dirListNum = dirCount < dir_max ? dirCount : dir_max;
    listGet.dirList = dirList;

    CellSaveDataListSet listSet;
    memset(&listSet, 0, sizeof(listSet));

    char selectedDir[64];
    cbResult.result = dispatch_func_select((uint32_t)(uintptr_t)funcList, dirList,
                                          listGet.dirListNum, (uint32_t)(uintptr_t)setBuf, 1,
                                          (uint32_t)(uintptr_t)userdata, "funcList", selectedDir);
    (void)listSet;

    if (cbResult.result == SAVEDATA_PICK_CANCELLED) {
        free(dirList);
        return CELL_SAVEDATA_RET_CANCEL;
    }
    if (cbResult.result < 0) {
        free(dirList);
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }
    if (!selectedDir[0] && dirCount > 0)
        strncpy(selectedDir, dirList[0].dirName, sizeof selectedDir - 1);
    if (!selectedDir[0]) {
        free(dirList);
        return CELL_SAVEDATA_ERROR_NODATA;
    }

    s32 result = savedata_execute(selectedDir, 1, setBuf, funcStat, funcFile, userdata);
    free(dirList);
    return result;
}

s32 cellSaveDataListLoad2(u32 version, CellSaveDataSetList* setList,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataListCallback funcList,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    printf("[cellSaveData] ListLoad2(version=%u)\n", version);

    if (!setList || !setBuf || !funcList || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    /* CellSaveDataSetList is { u32 sortType; u32 sortOrder; char* dirNamePrefix; }
     * and CellSaveDataSetBuf starts with u32 dirListMax -- guest layout, so the
     * prefix EA is the word at +8 and needs translating before it is used as a
     * host string. The host structs put dirNamePrefix at +8 as an 8-byte
     * pointer, so a cast would have read half of it plus padding. */
    u32 dir_max = vm_read32((u32)(uintptr_t)setBuf + 0);
    if (dir_max == 0) dir_max = CELL_SAVEDATA_DIRLIST_MAX;
    CellSaveDataDirList* dirList = (CellSaveDataDirList*)calloc(dir_max, sizeof(CellSaveDataDirList));

    u32 dirCount = enumerate_save_dirs(
        savedata_host_str((const char*)(uintptr_t)vm_read32((u32)(uintptr_t)setList + 8)) ?
            savedata_host_str((const char*)(uintptr_t)vm_read32((u32)(uintptr_t)setList + 8)) : "",
        dirList, dir_max);

    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;

    CellSaveDataListGet listGet;
    memset(&listGet, 0, sizeof(listGet));
    listGet.dirListNum = dirCount < dir_max ? dirCount : dir_max;
    listGet.dirList = dirList;

    CellSaveDataListSet listSet;
    memset(&listSet, 0, sizeof(listSet));

    char selectedDir[64];
    cbResult.result = dispatch_func_select((uint32_t)(uintptr_t)funcList, dirList,
                                          listGet.dirListNum, (uint32_t)(uintptr_t)setBuf, 0,
                                          (uint32_t)(uintptr_t)userdata, "funcList", selectedDir);
    (void)listSet;

    if (cbResult.result == SAVEDATA_PICK_CANCELLED) {
        free(dirList);
        return CELL_SAVEDATA_RET_CANCEL;
    }
    if (cbResult.result < 0) {
        free(dirList);
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }
    if (!selectedDir[0] && dirCount > 0)
        strncpy(selectedDir, dirList[0].dirName, sizeof selectedDir - 1);
    if (!selectedDir[0]) {
        free(dirList);
        return CELL_SAVEDATA_ERROR_NODATA;
    }

    s32 result = savedata_execute(selectedDir, 0, setBuf, funcStat, funcFile, userdata);
    free(dirList);
    return result;
}

/* Fixed callbacks have CellSaveDataFixedSet, not CellSaveDataListSet.
 * Marshal the PS3 layouts (32-bit pointers, big endian) and enter the guest
 * through its OPD bridge. Layout reference: RPCS3 cellSaveData.h. */
/* savedata_fixed's early returns were all silent, so a title whose save-data
 * load never completes produced exactly one log line ("FixedLoad2") and no hint
 * which of a dozen validation branches rejected it. Virtua Fighter 5 sits in
 * cellSaveDataFixedLoad2 and repeatedly offers to cancel the load; naming the
 * branch is the difference between that and a day of bisecting. Diagnostic
 * only -- the value returned is unchanged. */
static void savedata_reject_log(const char* fn, const char* why, int rc)
{
    printf("[cellSaveData] %s: %s (rc=0x%08X)\n", fn, why, (unsigned)rc);
}
#define SAVEDATA_REJECT(rc, why) \
    do { savedata_reject_log(__func__, (why), (rc)); return (rc); } while (0)

static s32 savedata_fixed(int is_save, CellSaveDataSetList* setList,
                         CellSaveDataSetBuf* setBuf, CellSaveDataFixedCallback funcFixed,
                         CellSaveDataStatCallback funcStat, CellSaveDataFileCallback funcFile,
                         void* userdata)
{
    if (!setList || !setBuf || !funcFixed)
        SAVEDATA_REJECT(CELL_SAVEDATA_ERROR_PARAM, "setList/setBuf/funcFixed is NULL");
    if (!g_ps3_guest_caller)
        SAVEDATA_REJECT(CELL_SAVEDATA_ERROR_INTERNAL, "no guest caller installed");
    u32 dir_max = vm_read32((u32)(uintptr_t)setBuf);
    if (dir_max > CELL_SAVEDATA_DIRLIST_MAX)
        SAVEDATA_REJECT(CELL_SAVEDATA_ERROR_PARAM, "dirListMax over CELL_SAVEDATA_DIRLIST_MAX");
    CellSaveDataDirList* dirs = dir_max ? calloc(dir_max, sizeof(*dirs)) : NULL;
    if (dir_max && !dirs) return CELL_SAVEDATA_ERROR_INTERNAL;
    u32 prefix = vm_read32((u32)(uintptr_t)setList + 8);
    u32 count = enumerate_save_dirs(prefix ? (char*)vm_base + prefix : "", dirs, dir_max);
    u32 listed = count < dir_max ? count : dir_max;
    scratch_reset();
    u32 cb = scratch_alloc(SAVEDATA_CBRESULT_SIZE);
    u32 get = scratch_alloc(0x4C); /* dirNum, dirListNum, dirList, reserved[64] */
    u32 set = scratch_alloc(12);   /* dirName, newIcon, option */
    u32 list = listed ? scratch_alloc(listed * 48) : 0;
    if (!cb || !get || !set || (listed && !list)) {
        free(dirs); return CELL_SAVEDATA_ERROR_INTERNAL;
    }
    marshal_cbresult_init(cb, CELL_SAVEDATA_CBRESULT_OK_NEXT, (u32)(uintptr_t)userdata);
    vm_write32(get, count); vm_write32(get + 4, listed); vm_write32(get + 8, list);
    if (listed) memcpy(vm_base + list, dirs, listed * 48);
    free(dirs);
    g_ps3_guest_caller((u32)(uintptr_t)funcFixed, cb, get, set, 0, 0, 0, 0, 0);
    s32 result = marshal_cbresult_read_result(cb);
    if (result == CELL_SAVEDATA_CBRESULT_OK_LAST ||
        result == CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM) return CELL_OK;
    if (result != CELL_SAVEDATA_CBRESULT_OK_NEXT) {
        /* Name the value: the title chose this, so it says WHY it refused --
         * ERR_NODATA on a load means the dirList we handed it was empty or
         * held nothing it recognised, which is a different bug from a broken
         * save or a marshalling mistake. */
        const char* w = "funcFixed returned an unknown cbresult";
        switch (result) {
        case CELL_SAVEDATA_CBRESULT_ERR_NOSPACE: w = "funcFixed: ERR_NOSPACE"; break;
        case CELL_SAVEDATA_CBRESULT_ERR_FAILURE: w = "funcFixed: ERR_FAILURE"; break;
        case CELL_SAVEDATA_CBRESULT_ERR_BROKEN:  w = "funcFixed: ERR_BROKEN";  break;
        case CELL_SAVEDATA_CBRESULT_ERR_NODATA:  w = "funcFixed: ERR_NODATA -- it found nothing it wanted in our dirList"; break;
        case CELL_SAVEDATA_CBRESULT_ERR_INVALID: w = "funcFixed: ERR_INVALID"; break;
        }
        printf("[cellSaveData] savedata_fixed: cbresult=%d listed=%u/%u prefix='%s' root='%s'\n",
               result, listed, count,
               prefix ? (const char*)(vm_base + prefix) : "", s_save_root);
        SAVEDATA_REJECT(CELL_SAVEDATA_ERROR_CBRESULT, w);
    }
    if (!funcStat)
        SAVEDATA_REJECT(CELL_SAVEDATA_ERROR_PARAM, "funcStat is NULL");
    u32 selected = vm_read32(set);
    if (!selected || vm_read32(set + 8) > 1)
        SAVEDATA_REJECT(CELL_SAVEDATA_ERROR_PARAM, "funcFixed set no dirName, or an out-of-range option");
    const char* name = (const char*)vm_base + selected;
    size_t len = strnlen(name, CELL_SAVEDATA_DIRNAME_SIZE);
    if (!len || len == CELL_SAVEDATA_DIRNAME_SIZE || strchr(name, '/') ||
        strchr(name, '\\') || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        SAVEDATA_REJECT(CELL_SAVEDATA_ERROR_PARAM, "funcFixed set an unusable dirName");
    /* The selected string may be inside callback scratch, reused by funcStat. */
    char directory[CELL_SAVEDATA_DIRNAME_SIZE];
    memcpy(directory, name, len + 1);
    printf("[cellSaveData] %s: proceeding with dir='%s' (%u listed)\n",
           __func__, directory, listed);
    return savedata_execute(directory, is_save, setBuf, funcStat, funcFile,
                            (void*)(uintptr_t)vm_read32(cb + 16));
}

s32 cellSaveDataFixedSave2(u32 version, CellSaveDataSetList* setList,
                            CellSaveDataSetBuf* setBuf,
                            CellSaveDataFixedCallback funcFixed,
                            CellSaveDataStatCallback funcStat,
                            CellSaveDataFileCallback funcFile,
                            u32 container, void* userdata)
{
    (void)container;
    printf("[cellSaveData] FixedSave2(version=%u)\n", version);
    return savedata_fixed(1, setList, setBuf, funcFixed, funcStat, funcFile, userdata);
}

s32 cellSaveDataFixedLoad2(u32 version, CellSaveDataSetList* setList,
                            CellSaveDataSetBuf* setBuf,
                            CellSaveDataFixedCallback funcFixed,
                            CellSaveDataStatCallback funcStat,
                            CellSaveDataFileCallback funcFile,
                            u32 container, void* userdata)
{
    (void)container;
    printf("[cellSaveData] FixedLoad2(version=%u)\n", version);
    return savedata_fixed(0, setList, setBuf, funcFixed, funcStat, funcFile, userdata);
}

s32 cellSaveDataListAutoLoad(u32 version, u32 errDialog,
                            CellSaveDataSetList* setList, CellSaveDataSetBuf* setBuf,
                            CellSaveDataFixedCallback funcFixed,
                            CellSaveDataStatCallback funcStat,
                            CellSaveDataFileCallback funcFile,
                            u32 container, void* userdata)
{
    (void)container;
    if (errDialog > 2) return CELL_SAVEDATA_ERROR_PARAM;
    printf("[cellSaveData] ListAutoLoad(version=%u)\n", version);
    return savedata_fixed(0, setList, setBuf, funcFixed, funcStat, funcFile, userdata);
}

s32 cellSaveDataAutoSave2(u32 version, const char* dirName,
                           u32 errDialog,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    dirName = savedata_host_str(dirName);
    printf("[cellSaveData] AutoSave2(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");

    /* Delegate to the non-_2 entry, which marshals. This used to call
     * savedata_execute(), which invokes funcStat as a HOST function pointer
     * with HOST struct addresses -- but funcStat is a guest OPD and the structs
     * must live in guest memory, so the callback wrote nothing the caller could
     * read and the first dereference faulted. The Simpsons Arcade Game crashed
     * exactly here on its first boot, creating its profile right after
     * AutoLoad2 had correctly reported ERR_NODATA. The two entries take
     * identical arguments; _2 is just the newer SDK name.
     *
     * savedata_execute() is still behind the List* and Fixed* entries and
     * carries the same defect for each of them. Fixing those needs list/file
     * marshalling that dispatch_func_stat does not do yet, so they are left
     * alone until a title actually reaches one. */
    return cellSaveDataAutoSave(version, dirName, errDialog, setBuf,
                                funcStat, funcFile, container, userdata);
}

s32 cellSaveDataAutoLoad2(u32 version, const char* dirName,
                           u32 errDialog,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    dirName = savedata_host_str(dirName);
    (void)version; (void)errDialog; (void)setBuf; (void)funcFile;
    (void)container;
    printf("[cellSaveData] AutoLoad2(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");

    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    /* funcStat is the GAME's guest-OPD address. Marshal StatGet/Set/CB
     * into guest BE memory and dispatch through g_ps3_guest_caller.
     * Even on first-run-no-data, the game observes funcStat fire with
     * isNewData=1 so its title state machine can advance. */
    char save_path[1024];
    build_save_path(save_path, sizeof(save_path), dirName);
    int is_new = !dir_has_save(save_path);

    (void)is_new;
    /* Full sequence: funcStat, then the funcFile loop that reads the files.
     * Stopping after funcStat (OK_NEXT) left a title with a save on disk
     * waiting for its data forever -- GH3's second boot sat on "Checking HDD".
     *
     * flОw first-boot: its funcStat returns ERR_NODATA on a new profile
     * (isNewData=1). The callback already ran and told the game "no save",
     * so report AutoLoad as CELL_OK -- an ERROR return leaves the title
     * parked in MODE_AUTO_LOAD (no app loop, no flips).
     * Per the SDK the correct return here is CELL_SAVEDATA_ERROR_NODATA,
     * and a real title handles it; that flОw does not is a bug somewhere in
     * its MODE_AUTO_LOAD state machine we have not tracked down. Keep the
     * compat return until that is understood -- it was dropped once already
     * in the fold merge and cost a boot regression. */
    s32 r = savedata_execute(dirName, 0, setBuf, funcStat, funcFile, userdata);
    return r == CELL_SAVEDATA_ERROR_NODATA ? CELL_OK : r;
}

#ifdef _WIN32
/* Delete a save folder and the files in it (save folders are flat, but
 * recurse anyway). Returns 1 on success. */
static int savedata_remove_tree(const char* path)
{
    char search[1200], child[1200];
    snprintf(search, sizeof search, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(child, sizeof child, "%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) savedata_remove_tree(child);
            else { SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL); DeleteFileA(child); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(path) != 0;
}
#endif

/* cellSaveDataDelete2 = the PS3 system "delete saved data" list for this
 * title: the USER picks a save and the system deletes it; the game passes no
 * name. It used to just return OK, so Civilization Revolution's "delete"
 * menu did nothing. On Windows the save picker lists the title's saves
 * (folders starting with its title id), asks "Delete ...?" (default No),
 * then removes the folder. Cancel -> CELL_SAVEDATA_RET_CANCEL.
 * Elsewhere, or with SAVEDATA_PICKER=0, it still returns OK and deletes nothing. */
s32 cellSaveDataDelete2(u32 container)
{
    printf("[cellSaveData] Delete2(container=%u)\n", container);
#ifdef _WIN32
    extern const char* cellGame_get_title_id(void);
    const char* pk = getenv("SAVEDATA_PICKER");
    if (pk && pk[0] == '0') return CELL_OK;
    const char* tid = cellGame_get_title_id();
    if (!tid || !tid[0]) return CELL_OK;

    enum { MAXD = 64 };
    static CellSaveDataDirList dirs[MAXD];
    static wchar_t labels[MAXD][160];
    u32 total = enumerate_save_dirs(tid, dirs, MAXD);
    int n = (int)(total < MAXD ? total : MAXD);
    if (n == 0) {
        printf("[cellSaveData] Delete2: no saves for '%s'%c", tid, 10);
        return CELL_OK;
    }
    for (int i = 0; i < n; i++) savedata_pick_label(dirs[i].dirName, labels[i]);

    int def = 0;
    for (;;) {
        /* No prompt: the caption says it, and the list keeps the same
         * layout as the Load/Save pickers (a prompt shrinks it). */
        const int c = savedata_host_pick(L"Delete save", NULL, labels, n, def);
        if (c < 0) {
            printf("[cellSaveData] Delete2: user cancelled%c", 10);
            return CELL_SAVEDATA_RET_CANCEL;
        }
        wchar_t msg[400];
        _snwprintf(msg, 400, L"Delete this saved game?\n%ls\nThis cannot be undone.", labels[c]);
        msg[399] = 0;
        if (!savedata_host_confirm(L"Delete save?", msg, L"Yes, delete")) { def = c; continue; }

        const char* name = dirs[c].dirName;
        if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return CELL_OK;
        char path[1024];
        build_save_path(path, sizeof path, name);
        const int ok = savedata_remove_tree(path);
        printf("[cellSaveData] Delete2: deleted '%s' -> %s%c", name, ok ? "ok" : "FAILED", 10);
        return CELL_OK;
    }
#else
    /* No host UI here: without a directory name nothing can be deleted. */
    return CELL_OK;
#endif
}

/* ---------------------------------------------------------------------------
 * Old / non-_2 variants — same wrapper pattern as the _2 versions, with
 * the same guest-callback caveat.
 *
 * Older PS3 SDK builds (e.g. our flOw NPUA80001 dump) link the original
 * cellSaveDataAutoSave / AutoLoad / Delete instead of the _2 variants
 * RPCS3's flOw build uses. Same semantics, different NID.
 * -----------------------------------------------------------------------*/
/* The pre-3.00 List/Fixed entry points. Identical to their "2" forms except
 * that v2 added a trailing `userdata` the callbacks echo back; RPCS3 forwards
 * v1 with userdata = NULL and so do we. Registering only the "2" forms left a
 * title on the older API getting the unresolved-NID default, so its save-data
 * callbacks never fired and whatever it was loading never completed. Virtua
 * Fighter 5 calls FixedLoad during its boot and stops there.
 *
 * Same shape as cellMsgDialogOpen vs Open2 and cellGameDataCheckCreate vs
 * Create2 -- three of the four v1/v2 pairs in this tree had the same gap. */
s32 cellSaveDataListSave(u32 version, CellSaveDataSetList* setList,
                         CellSaveDataSetBuf* setBuf,
                         CellSaveDataListCallback funcList,
                         CellSaveDataStatCallback funcStat,
                         CellSaveDataFileCallback funcFile, u32 container)
{
    return cellSaveDataListSave2(version, setList, setBuf, funcList, funcStat,
                                 funcFile, container, NULL);
}

s32 cellSaveDataListLoad(u32 version, CellSaveDataSetList* setList,
                         CellSaveDataSetBuf* setBuf,
                         CellSaveDataListCallback funcList,
                         CellSaveDataStatCallback funcStat,
                         CellSaveDataFileCallback funcFile, u32 container)
{
    return cellSaveDataListLoad2(version, setList, setBuf, funcList, funcStat,
                                 funcFile, container, NULL);
}

s32 cellSaveDataFixedSave(u32 version, CellSaveDataSetList* setList,
                          CellSaveDataSetBuf* setBuf,
                          CellSaveDataFixedCallback funcFixed,
                          CellSaveDataStatCallback funcStat,
                          CellSaveDataFileCallback funcFile, u32 container)
{
    return cellSaveDataFixedSave2(version, setList, setBuf, funcFixed, funcStat,
                                  funcFile, container, NULL);
}

s32 cellSaveDataFixedLoad(u32 version, CellSaveDataSetList* setList,
                          CellSaveDataSetBuf* setBuf,
                          CellSaveDataFixedCallback funcFixed,
                          CellSaveDataStatCallback funcStat,
                          CellSaveDataFileCallback funcFile, u32 container)
{
    return cellSaveDataFixedLoad2(version, setList, setBuf, funcFixed, funcStat,
                                  funcFile, container, NULL);
}

s32 cellSaveDataAutoSave(u32 version, const char* dirName,
                          u32 errDialog,
                          CellSaveDataSetBuf* setBuf,
                          CellSaveDataStatCallback funcStat,
                          CellSaveDataFileCallback funcFile,
                          u32 container, void* userdata)
{
    dirName = savedata_host_str(dirName);
    (void)errDialog; (void)funcFile; (void)container;
    printf("[cellSaveData] AutoSave(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");
    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    /* This used to return CELL_OK WITHOUT running funcStat -- the game believed
     * its profile saved while nothing happened (silent data loss). Run the
     * guest funcStat callback the same proven way cellSaveDataAutoLoad does
     * (dispatch_func_stat marshals through g_ps3_guest_caller). The callback
     * fires and returns a real result the game can act on. (Persisting the file
     * bytes needs the funcFile marshalling loop; the stat callback running is
     * the correct, honest first step and matches the load path's behaviour.) */
    char save_path[1024];
    build_save_path(save_path, sizeof(save_path), dirName);
    int is_new = !dir_has_save(save_path);

    (void)is_new;
    /* Run the whole sequence: funcStat, then the funcFile loop that writes the
     * files. Stopping after funcStat left the title waiting for file callbacks
     * that never came -- GH3 sat on "Saving..." forever after its first
     * autosave. savedata_execute marshals both callbacks through guest memory
     * (dispatch_func_stat_full / dispatch_func_file). */
    return savedata_execute(dirName, 1, setBuf, funcStat, funcFile, userdata);
}

s32 cellSaveDataAutoLoad(u32 version, const char* dirName,
                          u32 errDialog,
                          CellSaveDataSetBuf* setBuf,
                          CellSaveDataStatCallback funcStat,
                          CellSaveDataFileCallback funcFile,
                          u32 container, void* userdata)
{
    dirName = savedata_host_str(dirName);
    (void)version; (void)errDialog; (void)setBuf; (void)funcFile;
    (void)container;
    printf("[cellSaveData] AutoLoad(version=%u, dir='%s')\n",
           version, dirName ? dirName : "<null>");
    if (!dirName || !setBuf || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    char save_path[1024];
    build_save_path(save_path, sizeof(save_path), dirName);
    int is_new = !dir_has_save(save_path);

    (void)is_new;
    /* Same as cellSaveDataAutoLoad2 above, including flОw's first-boot compat
     * return (it calls this old non-_2 variant). */
    s32 r = savedata_execute(dirName, 0, setBuf, funcStat, funcFile, userdata);
    return r == CELL_SAVEDATA_ERROR_NODATA ? CELL_OK : r;
}

s32 cellSaveDataDelete(u32 version, const char* dirName,
                        u32 container)
{
    (void)version;
    dirName = savedata_host_str(dirName);
    printf("[cellSaveData] Delete(dir='%s', container=%u)\n",
           dirName ? dirName : "<null>", container);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * User variants (cellSaveDataUserAutoSave/Load): same as the non-User forms
 * with a userId inserted after version. The trailing userdata is the guest's
 * NINTH argument (on the guest stack, not in r3-r10) — the generic adapter
 * only passes 8 registers, so declare 8 params and pass NULL through.
 * LittleBigPlanet auto-loads its profile through UserAutoLoad at boot.
 * -----------------------------------------------------------------------*/
s32 cellSaveDataUserAutoSave(u32 version, u32 userId, const char* dirName,
                             u32 errDialog, CellSaveDataSetBuf* setBuf,
                             CellSaveDataStatCallback funcStat,
                             CellSaveDataFileCallback funcFile, u32 container)
{
    (void)userId;
    printf("[cellSaveData] UserAutoSave(user=%u)\n", userId);
    return cellSaveDataAutoSave(version, dirName, errDialog, setBuf,
                                funcStat, funcFile, container, NULL);
}

/* NOTE: the User* entry points take NINE arguments -- cellSaveDataUserAutoLoad
 * inserts `userId` ahead of dirName, which pushes `userdata` into the ninth
 * slot. The generic HLE adapter only forwards r3..r10 (eight), so the ninth
 * argument is unreachable here and MUST come from the caller's parameter save
 * area. That is what hle_cellSaveDataUserAutoLoad (below) is for; this plain-C
 * entry only sees eight and cannot know userdata. It stays for direct/internal
 * callers, and the ctx handler overrides it on the NID path. */
/* ---------------------------------------------------------------------------
 * Nine-argument entry points
 *
 * cellSaveDataUserAutoLoad/UserAutoSave take nine arguments; the generic HLE
 * adapter forwards only r3..r10, so the ninth (userdata) never arrives and used
 * to be hardcoded to NULL. That is fatal: a funcStat callback typically recovers
 * its own object from cbResult->userdata. LBP's is
 *     v = cbResult->userdata;  r = sub(v + 104, cbResult, get, set);
 * so a NULL made it work off guest address 104, read the VM's zero-fill, and
 * return CELL_SAVEDATA_CBRESULT_ERR_NODATA (-4) -- it never reached its handler.
 *
 * Take the full context and read the ninth argument from the caller's parameter
 * save area. SAVEDATA_ARG9_SP_OFF can be overridden at runtime
 * (PS3_SAVEDATA_ARG9_OFF) and the surrounding window is dumped once, so the
 * offset is confirmed from a real call rather than assumed.
 * -----------------------------------------------------------------------*/
#include "../../runtime/ppu/ppu_context.h"

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

/* PPC64 ELFv1: back chain, CR, LR, 2 reserved, TOC = 48 bytes, then the
 * parameter save area shadowing r3.. one doubleword each -> arg9 at SP+112. */
#define SAVEDATA_ARG9_SP_OFF 112u

static uint32_t savedata_arg9(ppu_context* ctx)
{
    uint32_t sp = (uint32_t)ctx->gpr[1];
    static int off = -1;
    if (off < 0) {
        const char* e = getenv("PS3_SAVEDATA_ARG9_OFF");
        off = e ? (int)strtol(e, NULL, 0) : (int)SAVEDATA_ARG9_SP_OFF;
    }
    { static int dumped = 0;
      if (!dumped) { dumped = 1;
        printf("[cellSaveData] caller SP=0x%08X param-save-area window:\n", sp);
        for (uint32_t o = 48; o <= 136; o += 8)
            printf("    SP+%3u = 0x%016llX%s\n", o,
                   (unsigned long long)vm_read64(sp + o),
                   o == (uint32_t)off ? "   <- taking as arg9 (userdata)" : "");
      } }
    return (uint32_t)vm_read64(sp + (uint32_t)off);
}

static void hle_cellSaveDataUserAutoLoad(ppu_context* ctx)
{
    u32   version   = (u32)ctx->gpr[3];
    u32   userId    = (u32)ctx->gpr[4];
    const char* dir = (const char*)(uintptr_t)(u32)ctx->gpr[5];
    u32   errDialog = (u32)ctx->gpr[6];
    void* setBuf    = (void*)(uintptr_t)(u32)ctx->gpr[7];
    void* funcStat  = (void*)(uintptr_t)(u32)ctx->gpr[8];
    void* funcFile  = (void*)(uintptr_t)(u32)ctx->gpr[9];
    u32   container = (u32)ctx->gpr[10];
    uint32_t userdata = savedata_arg9(ctx);

    printf("[cellSaveData] UserAutoLoad(user=%u, userdata=0x%08X)\n", userId, userdata);
    ctx->gpr[3] = (uint64_t)(int64_t)(s32)
        cellSaveDataAutoLoad2(version, dir, errDialog,
                              (CellSaveDataSetBuf*)setBuf,
                              (CellSaveDataStatCallback)funcStat,
                              (CellSaveDataFileCallback)funcFile,
                              container, (void*)(uintptr_t)userdata);
}

/* UserAutoSave takes the same nine arguments as UserAutoLoad. Through the
 * generic eight-argument binding the ninth (userdata) is lost, so the title's
 * stat and file callbacks see cbResult->userdata == 0. */
static void hle_cellSaveDataUserAutoSave(ppu_context* ctx)
{
    u32   version   = (u32)ctx->gpr[3];
    u32   userId    = (u32)ctx->gpr[4];
    const char* dir = (const char*)(uintptr_t)(u32)ctx->gpr[5];
    u32   errDialog = (u32)ctx->gpr[6];
    void* setBuf    = (void*)(uintptr_t)(u32)ctx->gpr[7];
    void* funcStat  = (void*)(uintptr_t)(u32)ctx->gpr[8];
    void* funcFile  = (void*)(uintptr_t)(u32)ctx->gpr[9];
    u32   container = (u32)ctx->gpr[10];
    uint32_t userdata = savedata_arg9(ctx);

    printf("[cellSaveData] UserAutoSave(user=%u, userdata=0x%08X)\n", userId, userdata);
    /* cellSaveDataAutoSave, not AutoSave2: AutoSave2 maps dirName to a host
     * pointer and passes it on to AutoSave, which maps it again; a host
     * address below 4 GiB would then be read as a guest one. */
    ctx->gpr[3] = (uint64_t)(int64_t)(s32)
        cellSaveDataAutoSave(version, dir, errDialog,
                             (CellSaveDataSetBuf*)setBuf,
                             (CellSaveDataStatCallback)funcStat,
                             (CellSaveDataFileCallback)funcFile,
                             container, (void*)(uintptr_t)userdata);
}

/* ListAutoLoad's ninth argument is in the guest parameter save area. */
void ps3_savedata_list_auto_load(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)(int64_t)cellSaveDataListAutoLoad(
        (u32)ctx->gpr[3], (u32)ctx->gpr[4],
        (CellSaveDataSetList*)(uintptr_t)(u32)ctx->gpr[5],
        (CellSaveDataSetBuf*)(uintptr_t)(u32)ctx->gpr[6],
        (CellSaveDataFixedCallback)(uintptr_t)(u32)ctx->gpr[7],
        (CellSaveDataStatCallback)(uintptr_t)(u32)ctx->gpr[8],
        (CellSaveDataFileCallback)(uintptr_t)(u32)ctx->gpr[9],
        (u32)ctx->gpr[10], (void*)(uintptr_t)savedata_arg9(ctx));
}

void cellSaveData_register_ctx_handlers(void)
{
    /* Overrides the generated 8-arg registration: ctx handlers are dispatched
     * before the generic table. */
    ps3_hle_register_ctx(0xCDC6AEFDu, "cellSaveDataUserAutoLoad",
                         hle_cellSaveDataUserAutoLoad);
    ps3_hle_register_ctx(0x52AAC4FAu, "cellSaveDataUserAutoSave",
                         hle_cellSaveDataUserAutoSave);
}

s32 cellSaveDataUserAutoLoad(u32 version, u32 userId, const char* dirName,
                             u32 errDialog, CellSaveDataSetBuf* setBuf,
                             CellSaveDataStatCallback funcStat,
                             CellSaveDataFileCallback funcFile, u32 container)
{
    (void)userId;
    printf("[cellSaveData] UserAutoLoad(user=%u) [8-arg path: userdata unavailable]\n", userId);
    return cellSaveDataAutoLoad(version, dirName, errDialog, setBuf,
                                funcStat, funcFile, container, NULL);
}
