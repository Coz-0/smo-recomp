/* Included by nnnet.c: nn::account network-service-account functions and AsyncContext, nn::friends AsyncContext and
 * Profile, nn::bcat DeliveryCacheDirectory / DeliveryCacheFile, nn::pl::BeginVrMode / EndVrMode.
 * Registered by online_register() from nnnet_register_final(), i.e. after every other table: these REPLACE the handlers of
 * the same names in nnfs.c, nnmisc.c, nnsdk.c and nnpl.c (whose table entries are now dead).
 *
 * Sources: [sdk] = Super Smash Bros. Ultimate 13.0.4's SDK module, where ssbu-recomp read these functions (addresses quoted;
 * [sdk 7.3.2 ...] = this game's), [suyu] = the suyu emulator's src/core/hle/service (file:line),
 * [game] = main, read with Ghidra. Console state modelled: one local user, no network connection (nn::nifm never accepts
 * an internet request), no Nintendo Account linked to the user.
 *
 * ------------------------------------------------------------------ nn::account
 * UserHandle +0x10 is the IManagerForApplication of the opened user; its commands as the SDK calls them:
 *   +0x20 CheckAvailability, +0x28 GetAccountId, +0x30 EnsureIdTokenCacheAsync, +0x38 LoadIdTokenCache, +0x40 (cmd 130).
 * [sdk 0x4b450] IsNetworkServiceAccountAvailable: CheckAvailability; 0 -> true; a result of module 124 with description
 *     200..269 -> success + false; anything else is returned. So "the user has no usable network service account" is
 *     the range 124-0200..0269. WHICH code of the range the account module gives for a user without a linked Nintendo
 *     Account is NOT in the reference ([suyu] acc/acc.cpp:713 CheckAvailability is a stub that answers success, i.e. it
 *     models a linked account) and no such constant exists in sdk.elf (searched: none).
 * [sdk 0x4b380] EnsureNetworkServiceAccountAvailable: CheckAvailability; 0 -> 0; a code outside that range is returned;
 *     a code INSIDE it is discarded: GetUserId, then detail::StartPselApplet [sdk 0x3afe0 -> 0x3b010] with UiSettings
 *     {mode 2, uid} ([suyu] am/frontend/applet_profile_select.h:29 mode 2 = EnsureNetworkServiceAccountAvailable):
 *     CreateLibraryApplet(0x10); StartLibraryApplet failing -> 0x27c (124-0001); else the first u32 of the applet's 0x18
 *     byte answer is the result. The applet can only link an account over the network; offline the user can only leave
 *     it, and the applet then answers 124-0001 "cancelled by user" ([suyu] applet_profile_select.cpp:106-107). So the
 *     function gives 0x27c whichever code of the range the module used. The applet is not shown here.
 *     [game] 0x71037558a0: (result & 0x3fffff) == 0x27c -> returns 2 without an error message.
 * [sdk 0x4b500] EnsureNetworkServiceAccountIdTokenCacheAsync: FIRST detail::CheckInternetAvailability [sdk 0x3afb0]:
 *     nn::nifm::IsAnyInternetRequestAccepted(GetClientId()) false -> 0x767c (124-0059), returned before the account
 *     module is asked and before the AsyncContext is touched. [game] 0x7100249cd0 only tests it for 0.
 * AsyncContext [sdk 0x494f0 C1, 0x49510 D1]: {vtable, 8 + 8 bytes, IAsyncContext * @0x18}.
 *     C1(): vtable (_ZTVN2nn7account12AsyncContextE + 0x10), the other three words 0.
 *     D1: vtable; if the pointer is set: HasDone must succeed, release, pointer = 0, abort when it had not finished.
 *     HasDone [0x49750] / GetResult [0x49780] / Cancel [0x49720] / GetSystemEvent [0x49670] call the IAsyncContext
 *         (+0x30, +0x38, +0x28, +0x20) WITHOUT a null check. In this model no IAsyncContext ever exists (the only
 *         function of the game that makes one fails first, see above), so the pointer is always null and those four
 *         would fault on a console; here they stop the game with a message instead of answering anything.
 * GetNetworkServiceAccountId [0x4b4d0] (+0x28), LoadNetworkServiceAccountIdTokenCache [0x4b5d0] (+0x38): the account
 *     module's answer for a user without a network service account, returned unchanged. STILL OPEN: 0x1907c = 124-0200,
 *     the first code of the range above, is used; the exact code is not established (see IsNetworkServiceAccountAvailable).
 *
 * ------------------------------------------------------------------ nn::friends
 * AsyncContext [sdk 0x1de230 C1 (lifted)]: {nn::os::MutexType @0, detail::AsyncContextInternal * @0x20 = null}.
 *     GetSystemEvent(ev) [0x1de290]: lock; internal null -> 0xe79 (121-0007); unlock.
 *     GetResult [0x1de390]: lock; internal null -> 0xe79; unlock.
 *     D1 [0x1de260]: internal -> DeleteInstance; FinalizeMutex(this).
 *     The functions that start a request (and set the internal pointer) are not imports of the game, so the pointer stays
 *     null and these answer 0xe79.
 * Profile::GetAccountId() const [sdk 7.3.2 0x1dc190] is the Profile's first field.
 *
 * ------------------------------------------------------------------ nn::bcat DeliveryCacheDirectory / DeliveryCacheFile
 * Both objects are one pointer to the module's directory / file service ([sdk 0xc3830, 0xc40c0] C1: null; lifted).
 * Directory::Open(name) [0xc3be0 -> 0xc3f80]: DirectoryName::IsValid false -> 0x27a (122-0001); already open -> 0xc7a
 *     (122-0006); ShimLibraryGlobal::CreateDirectoryService [0xc5610]: storage not mounted -> 0xa7a (122-0005); the
 *     service's Open: [suyu] bcat/delivery_cache_directory_service.cpp:55-56 a directory that does not exist is
 *     ResultFailedOpenEntity = 122-0002 = 0x47a; detail::HandleError [0xc51b0] passes it through (it aborts only for
 *     fs results 2-4000..4999); the service is released and the object stays closed.
 *     The delivery cache of a console that never synchronised is empty ([sdk] nnsdk.c: 0 directories), so every valid
 *     name gives 0x47a.
 * Directory::Read [0xc3dd0]: not open -> 0xe7a (122-0007), count not written. D1 [0xc3840]: nothing when not open.
 * File::Open(dir, file) [0xc4520 -> 0xc4960]: DirectoryName or FileName [0xc1b60: 1..31 characters of A-Z a-z 0-9 _ . ,
 *     not ending in '.'] invalid -> 0x27a; already open -> 0xc7a; CreateFileService: not mounted -> 0xa7a; the service's
 *     Open: [suyu] bcat/delivery_cache_file_service.cpp:42-43 -> 0x47a. Close [0xc45d0] / D1 [0xc40d0]: nothing when
 *     not open.
 *
 * ------------------------------------------------------------------ nn::pl
 * pl::BeginVrMode [0x341e90]: nn::pctl::CheckStereoVisionPermission(false) false -> 0xd08e (142-0104); else
 *     ICommonStateGetter +0xd0 (BeginVrModeEx; abort on failure) and 0. pl::EndVrMode [0x341f00]: +0xd8 (EndVrModeEx).
 *     [suyu] am/service/common_state_getter.cpp:225-237: the module only keeps a flag. The console also changes the
 *     panel's brightness handling in VR mode; nothing of that exists on a PC. */
void os_signal_event(uint64_t key);                       /* os_threads.c */

/* ---------------------------------------------------------------- nn::account */
#define ACC_CANCELLED 0x27cu                               /* 124-0001 */
#define ACC_NO_INTERNET 0x767cu                            /* 124-0059 */
#define ACC_NSA_UNAVAILABLE_UNVERIFIED 0x1907cu            /* 124-0200: first code of the SDK's range; exact code open */
static uint64_t online_acc_vtable(void) { uint64_t v = sdk_symbol("_ZTVN2nn7account12AsyncContextE"); return v ? v + 0x10 : 0; }
static void online_acc_no_context(Ctx *c, const char *what) {
    printf("\nnn::account::AsyncContext::%s on a context that holds no request ([sdk] null pointer at +0x18: a console faults here).\n", what);
    guest_trap("nn::account::AsyncContext used without a request", c->x[30] - 4);
}
static void online_acc_release_old(Ctx *c, uint64_t old) {
    if (old) online_acc_no_context(c, "destructor (a request pointer exists, which this model never creates)");
}
static void online_acc_Async_ctor(Ctx *c) { uint64_t s = A0; wr64(s, online_acc_vtable()); wr64(s + 8, 0); wr64(s + 0x10, 0); wr64(s + 0x18, 0); }
static void online_acc_Async_dtor(Ctx *c) {
    uint64_t s = A0;
    wr64(s, online_acc_vtable());
    online_acc_release_old(c, rd64(s + 0x18));
    RET(s);
}
static void online_acc_Async_HasDone(Ctx *c) { online_acc_no_context(c, "HasDone"); }
static void online_acc_Async_GetResult(Ctx *c) { online_acc_no_context(c, "GetResult"); }
static void online_acc_Async_Cancel(Ctx *c) { online_acc_no_context(c, "Cancel"); }
static void online_acc_Async_GetSystemEvent(Ctx *c) { online_acc_no_context(c, "GetSystemEvent"); }
static void online_acc_EnsureAvailable(Ctx *c) { RET(ACC_CANCELLED); }
static void online_acc_EnsureIdTokenCacheAsync(Ctx *c) { RET(ACC_NO_INTERNET); }
static void online_acc_nsa_unavailable(Ctx *c) { RET(ACC_NSA_UNAVAILABLE_UNVERIFIED); }

/* ---------------------------------------------------------------- nn::friends */
#define FR_NOT_CALLED 0xe79u                               /* 121-0007 */
static void online_fr_no_request(uint64_t ctx) {
    if (rd64(ctx + 0x20)) guest_trap("nn::friends::AsyncContext holds a request (this model never starts one)", ctx);
}
static void online_fr_Async_GetSystemEvent(Ctx *c) {         /* (this, SystemEvent *) */
    uint64_t self = A0;
    fr_lock(c, self, 1);
    online_fr_no_request(self);
    fr_lock(c, self, 0);
    RET(FR_NOT_CALLED);
}
static void online_fr_Async_GetResult(Ctx *c) {
    uint64_t self = A0;
    fr_lock(c, self, 1);
    online_fr_no_request(self);
    fr_lock(c, self, 0);
    RET(FR_NOT_CALLED);
}
static void online_fr_Async_dtor(Ctx *c) {
    uint64_t self = A0;
    online_fr_no_request(self);
    ImportFn fin = import_lookup("_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE");
    c->x[0] = self;
    if (fin) fin(c);
    RET(self);
}
static void online_fr_Profile_GetAccountId(Ctx *c) { RET(rd64(A0)); }

/* ---------------------------------------------------------------- nn::bcat */
#define BCAT_INVALID_ARG 0x27au
#define BCAT_NOT_FOUND 0x47au
#define BCAT_NOT_MOUNTED 0xa7au
#define BCAT_ALREADY_OPEN 0xc7au
#define BCAT_NOT_OPEN 0xe7au
extern volatile LONG g_bcat_mounted;                      /* nnsdk.c */
int bcat_dirname_valid(const char *s);                    /* nnsdk.c: [sdk 0xc1be0] */
static int online_bcat_dir_ok(uint64_t name) { const char *s = (const char *)(uintptr_t)name; return strnlen(s, 32) < 32 && bcat_dirname_valid(s); }
static int online_bcat_file_ok(uint64_t name) {              /* [sdk 0xc1b60] */
    const unsigned char *s = (const unsigned char *)(uintptr_t)name;
    unsigned n = 0;
    for (; n < 32; n++) {
        unsigned ch = s[n];
        if (!ch) break;
        if (!((ch >= '0' && ch <= '9') || ((ch & 0xdf) >= 'A' && (ch & 0xdf) <= 'Z') || ch == '_' || ch == '.')) return 0;
    }
    if (n == 0 || n == 32) return 0;
    return s[n - 1] != '.';
}
static uint32_t online_bcat_open(uint64_t self, int names_ok) {
    if (!names_ok) return BCAT_INVALID_ARG;
    if (rd64(self)) return BCAT_ALREADY_OPEN;
    if (!g_bcat_mounted) return BCAT_NOT_MOUNTED;
    return BCAT_NOT_FOUND;                                  /* the module: nothing was ever delivered */
}
static void online_bcat_Dir_Open(Ctx *c) { RET(online_bcat_open(A0, online_bcat_dir_ok(A1))); }                              /* (this, const DirectoryName &) */
/* File::Open(const char *path) [sdk 7.3.2 0xc11f0]: no '/' in the path, or more than 31
 * characters before it -> 0x27a; else the characters before the '/' become the DirectoryName and the first 31 after it
 * the FileName (a longer name is cut there), then the two-argument Open (see the top of this file), whose Result is
 * returned. */
static void online_bcat_File_Open_path(Ctx *c) {
    const char *path = (const char *)(uintptr_t)A1, *slash = strchr(path, '/');
    char dir[32] = {0}, file[32] = {0};
    if (!slash || slash - path + 1 > 32) { RET(BCAT_INVALID_ARG); return; }
    memcpy(dir, path, (size_t)(slash - path));
    strncpy(file, slash + 1, 31);
    RET(online_bcat_open(A0, online_bcat_dir_ok((uint64_t)(uintptr_t)dir) && online_bcat_file_ok((uint64_t)(uintptr_t)file)));
}
static void online_bcat_Dir_Read(Ctx *c) {                    /* (this, int *outCount, DeliveryCacheDirectoryEntry *, int) */
    if (rd64(A0)) guest_trap("nn::bcat::DeliveryCacheDirectory::Read on an open directory (this model never opens one)", c->x[30] - 4);
    RET(BCAT_NOT_OPEN);
}
static void online_bcat_close(Ctx *c) {                       /* Close and the destructors: nothing when not open */
    if (rd64(A0)) guest_trap("nn::bcat delivery cache object is open (this model never opens one)", c->x[30] - 4);
}

/* ---------------------------------------------------------------- nn::pl */
static volatile LONG g_vr_mode;                        /* the flag the am module keeps ([suyu] common_state_getter.cpp:225) */
static void online_pl_BeginVrMode(Ctx *c) {
    ImportFn permit = import_lookup("_ZN2nn4pctl27CheckStereoVisionPermissionEb");
    c->x[0] = 0;
    if (permit) permit(c);
    if (!permit || !(c->x[0] & 1)) { RET(0xd08eu); return; }
    InterlockedExchange(&g_vr_mode, 1);
    RET(0);
}
static void online_pl_EndVrMode(Ctx *c) { InterlockedExchange(&g_vr_mode, 0); RET(0); }

static const ImportDef online_defs[] = {
    {"_ZN2nn7account12AsyncContextC1Ev", online_acc_Async_ctor},
    {"_ZN2nn7account12AsyncContextD1Ev", online_acc_Async_dtor},
    {"_ZN2nn7account12AsyncContext7HasDoneEPb", online_acc_Async_HasDone}, {"_ZN2nn7account12AsyncContext9GetResultEv", online_acc_Async_GetResult},
    {"_ZN2nn7account12AsyncContext6CancelEv", online_acc_Async_Cancel},
    {"_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", online_acc_Async_GetSystemEvent},
    {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", online_acc_EnsureAvailable},
    {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", online_acc_EnsureIdTokenCacheAsync},
    {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", online_acc_nsa_unavailable},
    {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", online_acc_nsa_unavailable},
    {"_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", online_fr_Async_GetSystemEvent},
    {"_ZN2nn7friends12AsyncContextD1Ev", online_fr_Async_dtor}, {"_ZNK2nn7friends12AsyncContext9GetResultEv", online_fr_Async_GetResult},
    {"_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", online_bcat_Dir_Open},
    {"_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi", online_bcat_Dir_Read},
    {"_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev", online_bcat_close},
    {"_ZN2nn4bcat17DeliveryCacheFile5CloseEv", online_bcat_close}, {"_ZN2nn4bcat17DeliveryCacheFileD1Ev", online_bcat_close},
    {"_ZN2nn2pl11BeginVrModeEv", online_pl_BeginVrMode}, {"_ZN2nn2pl9EndVrModeEv", online_pl_EndVrMode},
    {"_ZN2nn4bcat17DeliveryCacheFile4OpenEPKc", online_bcat_File_Open_path},
    {"_ZNK2nn7friends7Profile12GetAccountIdEv", online_fr_Profile_GetAccountId},
};
static void online_register(void) {
    int n = (int)(sizeof online_defs / sizeof online_defs[0]);
    for (int i = 0; i < n; i++)
        if (!import_data(online_defs[i].name))
            printf("nnonline: \"%s\" is not an import of the game (a typo?)\n", online_defs[i].name);
    register_imports(online_defs, n);
}

/* ---------------------------------------------------------------- self-test */
#define ONLINE_CHECK(cond, what) do { cases++; if (!(cond)) { printf("    online FAILED: %s\n", what); bad++; } } while (0)
static int online_selftest(Ctx *c) {
    int bad = 0, cases = 0;
    /* every name is answered by the handler of this file */
    for (int i = 0; i < (int)(sizeof online_defs / sizeof online_defs[0]); i++)
        if (import_lookup(online_defs[i].name) != online_defs[i].fn) { printf("    online FAILED: %s is answered by another handler\n", online_defs[i].name); bad++; }
    cases++;

    /* nn::account */
    uint64_t a[4], b[4], handle[3] = {1, 0, 0};
    memset(a, 0xee, sizeof a); memset(b, 0xee, sizeof b);
    c->x[0] = P(a); h("_ZN2nn7account12AsyncContextC1Ev")(c);
    ONLINE_CHECK(a[0] == online_acc_vtable() && a[0] && !a[1] && !a[2] && !a[3], "account AsyncContext(): vtable + three zero words");
    c->x[0] = P(a); h("_ZN2nn7account12AsyncContextD1Ev")(c);
    ONLINE_CHECK(a[0] == online_acc_vtable() && !a[3], "account ~AsyncContext without a request");
    c->x[0] = P(handle); h("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE")(c);
    ONLINE_CHECK((uint32_t)c->x[0] == 0x27c && (c->x[0] & 0x1ff) == 124 && ((c->x[0] >> 9) & 0x1fff) == 1, "EnsureNetworkServiceAccountAvailable = 124-0001");
    c->x[0] = P(b); h("_ZN2nn7account12AsyncContextC1Ev")(c);
    c->x[0] = P(b); c->x[1] = P(handle); h("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE")(c);
    ONLINE_CHECK((uint32_t)c->x[0] == 0x767c && ((c->x[0] >> 9) & 0x1fff) == 59 && !b[3], "EnsureNetworkServiceAccountIdTokenCacheAsync = 124-0059, context untouched");
    {
        uint8_t avail = 0xee;
        uint64_t id = 0xeeeeeeeeeeeeeeeeull;
        c->x[0] = P(&avail); c->x[1] = P(handle); h("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE")(c);
        ONLINE_CHECK(c->x[0] == 0 && avail == 0, "IsNetworkServiceAccountAvailable = success, false");
        c->x[0] = P(&id); c->x[1] = P(handle); h("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE")(c);
        uint32_t r = (uint32_t)c->x[0], desc = (r >> 9) & 0x1fff;
        ONLINE_CHECK((r & 0x1ff) == 124 && desc >= 200 && desc < 270 && id == 0xeeeeeeeeeeeeeeeeull, "GetNetworkServiceAccountId: a result of the SDK's 'unavailable' range, id untouched");
    }
    uint64_t uid[2] = {1, 0};
    for (int i = 0; i < (int)(sizeof online_defs / sizeof online_defs[0]); i++) {
        ImportFn f = online_defs[i].fn;
        if (f != online_acc_EnsureAvailable && f != online_acc_EnsureIdTokenCacheAsync && f != online_acc_nsa_unavailable) continue;
        c->x[0] = P(uid); c->x[1] = P(uid); c->x[2] = P(uid); c->x[3] = 0;
        f(c);
        if ((uint32_t)c->x[0] == 0x3e87a) { printf("    online FAILED: %s still answers 0x3e87a\n", online_defs[i].name); bad++; }
    }
    cases++;

    /* nn::friends */
    uint8_t ctx[0x40], ev[0x40];
    memset(ctx, 0xee, sizeof ctx);
    c->x[0] = P(ctx); h("_ZN2nn7friends12AsyncContextC1Ev")(c);
    ONLINE_CHECK(rd64(P(ctx) + 0x20) == 0, "friends AsyncContext(): no request");
    c->x[0] = P(ctx); h("_ZNK2nn7friends12AsyncContext9GetResultEv")(c);
    ONLINE_CHECK((uint32_t)c->x[0] == 0xe79, "friends AsyncContext::GetResult without a request = 121-0007");
    c->x[0] = P(ctx); c->x[1] = P(ev); h("_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE")(c);
    ONLINE_CHECK((uint32_t)c->x[0] == 0xe79, "friends AsyncContext::GetSystemEvent without a request = 121-0007");
    c->x[0] = P(ctx); h("_ZN2nn7friends12AsyncContextD1Ev")(c);

    /* nn::bcat */
    uint64_t dir = 0xeeeeeeeeeeeeeeeeull, file = 0xeeeeeeeeeeeeeeeeull;
    char dname[32];
    int was_mounted = g_bcat_mounted, count = 77;
    c->x[0] = P(&dir); h("_ZN2nn4bcat22DeliveryCacheDirectoryC1Ev")(c);
    c->x[0] = P(&file); h("_ZN2nn4bcat17DeliveryCacheFileC1Ev")(c);
    ONLINE_CHECK(dir == 0 && file == 0, "bcat constructors: closed objects");
    memset(dname, 0, sizeof dname);
    strcpy(dname, "bad name");
    c->x[0] = P(&dir); c->x[1] = P(dname); h("_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE")(c);
    ONLINE_CHECK((uint32_t)c->x[0] == 0x27a, "Directory::Open with an invalid name = 122-0001");
    strcpy(dname, "spirits");
    if (!was_mounted) {
        c->x[0] = P(&dir); c->x[1] = P(dname); h("_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE")(c);
        ONLINE_CHECK((uint32_t)c->x[0] == 0xa7a, "Directory::Open without a mounted storage = 122-0005");
        h("_ZN2nn4bcat25MountDeliveryCacheStorageEv")(c);
    }
    c->x[0] = P(&dir); c->x[1] = P(dname); h("_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE")(c);
    ONLINE_CHECK((uint32_t)c->x[0] == 0x47a && dir == 0, "Directory::Open in an empty delivery cache = 122-0002, still closed");
    c->x[0] = P(&dir); c->x[1] = P(&count); c->x[2] = 0; c->x[3] = 4; h("_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi")(c);
    ONLINE_CHECK((uint32_t)c->x[0] == 0xe7a && count == 77, "Directory::Read when not open = 122-0007, count untouched");
    static const struct { const char *name; int ok; } fn[] = {{"a.bin", 1}, {"A_9.msbt", 1}, {"", 0}, {"end.", 0}, {"sp ace", 0}, {"dash-x", 0},
                                                              {"0123456789012345678901234567890", 1}, {"01234567890123456789012345678901", 0}};
    int names_ok = 1;
    for (int i = 0; i < (int)(sizeof fn / sizeof fn[0]); i++) {
        char buf[40];
        memset(buf, 0, sizeof buf);
        memcpy(buf, fn[i].name, strlen(fn[i].name));
        if (online_bcat_file_ok(P(buf)) != fn[i].ok) names_ok = 0;
    }
    ONLINE_CHECK(names_ok, "FileName rule of sdk 0xc1b60 (8 names)");
    c->x[0] = P(&file); h("_ZN2nn4bcat17DeliveryCacheFile5CloseEv")(c);
    c->x[0] = P(&dir); h("_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev")(c);
    c->x[0] = P(&file); h("_ZN2nn4bcat17DeliveryCacheFileD1Ev")(c);
    ONLINE_CHECK(dir == 0 && file == 0, "Close / destructors of closed objects do nothing");
    if (!was_mounted) h("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv")(c);

    /* nn::pl */
    c->x[0] = 0; h("_ZN2nn4pctl27CheckStereoVisionPermissionEb")(c);
    int permitted = (int)(c->x[0] & 1);
    h("_ZN2nn2pl11BeginVrModeEv")(c);
    ONLINE_CHECK(permitted ? (c->x[0] == 0 && g_vr_mode == 1) : ((uint32_t)c->x[0] == 0xd08e && g_vr_mode == 0), "BeginVrMode follows CheckStereoVisionPermission (0xd08e when refused)");
    h("_ZN2nn2pl9EndVrModeEv")(c);
    ONLINE_CHECK(g_vr_mode == 0, "EndVrMode clears the flag");

    printf("  online: account AsyncContext + network service account answers, friends AsyncContext, bcat directory / file,\n"
           "       pl VR mode: %d cases: %s\n", cases, bad ? "FAILED" : "ok");
    return bad;
}
