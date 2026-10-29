/* Stand-in for build/gen/image.h, so runtime/core compiles without a dump (tests/macos/check_core.sh).
 * Every value is made up; nothing here comes from the game. */
#ifndef SMO_IMAGE_H
#define SMO_IMAGE_H
#define GAME_VERSION "stub"
#define SDK_VERSION 0u
#define GAME_ELF_DEFAULT "main.elf"
#define GAME_BUILD_ID "stub"
#ifdef __APPLE__                                  /* hostrt.h's layout: 0x10_0000_0000 higher on macOS */
#define GEN_GUEST_BASE 0x8100000000ULL
#define GEN_SDK_BASE 0x8500000000ULL
#else
#define GEN_GUEST_BASE 0x7100000000ULL
#define GEN_SDK_BASE 0x7500000000ULL
#endif
#define GUEST_FILE_IMAGE 0x10000ULL
#define GUEST_IMAGE_SIZE 0x20000ULL
#define GUEST_RO_END 0x9000ULL
#define GUEST_RELA 0x1000ULL
#define GUEST_RELASZ 0x0ULL
#define GUEST_JMPREL 0x1000ULL
#define GUEST_PLTRELSZ 0x0ULL
#define GUEST_DYNSYM 0x2000ULL
#define GUEST_DYNSTR 0x3000ULL
#define GUEST_INIT_ARRAY (GUEST_BASE + 0x4000ULL)
#define GUEST_INIT_ARRAY_SIZE 0x0ULL
#define GUEST_TEXT_SIZE 0x8000ULL
#define GAME_SYM_nninitStartup (GUEST_BASE + 0x100ULL)
#define GAME_SYM_nnMain (GUEST_BASE + 0x200ULL)
#define SDK_IMAGE_SIZE_GEN 0x10000ULL
#define SDK_TEXT_SIZE_GEN 0x8000ULL
/* the other game addresses the runtime asks for: 0, as gen_tables.py writes an entry it cannot find */
#define GAME_FN_ChangeStageInfo_ctor 0ULL
#define GAME_FN_setVelocityZero 0ULL
#define GAME_FN_tryChangeNextStage 0ULL
#define GAME_FN_updatePoseTrans 0ULL
#define GAME_SYM__ZdlPv 0ULL
#define GAME_SYM__ZN3agl6driver6NVNMgr16registerSampler_EiPK10NVNsamplerj 0ULL
#define GAME_SYM__ZN3agl6driver6NVNMgr18compareSamplerKey_EPKNS1_10SamplerKeyES4_ 0ULL
#define GAME_SYM__ZN4sead12PtrArrayImpl4sortEPFiPKvS2_E 0ULL
#define GAME_SYM_aligned_alloc 0ULL
#define GAME_SYM_free 0ULL
#define GAME_VSYM_StageScene_init 0ULL
#endif
