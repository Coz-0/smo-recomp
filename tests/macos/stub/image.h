/* Stand-in for build/gen/image.h, so runtime/core compiles without a dump (tests/macos/check_core.sh).
 * Every value is made up; nothing here comes from the game. */
#ifndef SMO_IMAGE_H
#define SMO_IMAGE_H
#define GAME_VERSION "stub"
#define SDK_VERSION 0u
#define GAME_ELF_DEFAULT "main.elf"
#define GAME_BUILD_ID "stub"
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
#endif
