#include "qemu/compiler.h"
#include "xnu/patch.h"
#include "xnu/mach-o/loader.h"
#include "xnu/mach-o/macho_arm64.h"
#include "xnu/boot/xnuboot.h"
#include "xnu/macho.h"

#define PACIBSP                (( 0xD503237F ))
#define RETAB                  (( 0xD65F0FFF ))
#define BTI_C                  (( 0xD503245F ))
#define MOVX(rd,v)             (( 0xD2800000 | ( ((v)) << 5) | ((rd)) ))

#define IS_LDG(v)              (( (0b11011001011 == (((v)) >> 21)) && (0 == ((((v)) >> 10) & 0b011)) ))

// Assuming all pointers are DYLD_CHAINED_PTR_64_KERNEL_CACHE, strip everything
// but target (treating this pointer as a dyld_chained_ptr_64_kernel_cache_rebase).
#define REMOVE_DYLD_FIXUP_DATA(p)    (( ((p)) & (( BIT(30) - 1 )) ))

struct sysctl_oid {
    void   *oid_parent;
    void   *oid_link;
    int     oid_number;
    int     oid_kind;
    void    *oid_arg1;
    int     oid_arg2;
    char   *oid_name;
    u64     oid_handler;
    char   *oid_fmt;
    char   *oid_descr;
    int     oid_version;
    int     oid_refcnt;
};

static void patch_img4_deadlock(u8 *bkc_macho) {
    // Find the sysctl__security_mac_img4_ignition_blob struct in AppleImage4.kext,
    // then patch its oid handler (_darwin_trap_ignition_get_blob) to return 0.

    fse_t *img4_fse = macho_find_fileset_entry(bkc_macho, "com.apple.security.AppleImage4");
    if (!img4_fse) {
        fprintf(stderr, "warning: couldn't find img4 kext\n");
        return;
    }

    u8 *img4_macho = &bkc_macho[img4_fse->fileoff];

    seg_t *img4_exc_seg = macho_find_seg(img4_macho, "__TEXT_EXEC");
    sect_t *img4_dat_sect = macho_find_sect(img4_macho, "__DATA", "__data");
    sect_t *img4_str_sect = macho_find_sect(img4_macho, "__TEXT", "__cstring");

    i64 *img4_dat = (i64*)&bkc_macho[img4_dat_sect->offset];
    u8  *img4_str = (u8*)&bkc_macho[img4_str_sect->offset];

    u64 ignition_blob_str = 0;
    for (size_t i = 0; i < img4_str_sect->size; i++) {
        if (0 == strcmp("ignition_blob", (char*)&img4_str[i])) {
            ignition_blob_str = img4_str_sect->offset + i;
        }
    }

    if (0 == ignition_blob_str) {
        fprintf(stderr, "couldn't find ignition_blob sysctl struct\n");
        return;
    }

    u64 oid_handler = 0;
    for (size_t i = 0; i < img4_dat_sect->size / sizeof(u64); i++) {
        // This is sysctl__security_mac_img4_ignition_blob.oid_name
        u64 stripped_ptr = REMOVE_DYLD_FIXUP_DATA(img4_dat[i]);
        if (stripped_ptr == ignition_blob_str) {
            struct sysctl_oid *blob_oid = container_of((char**)&img4_dat[i], struct sysctl_oid, oid_name);
            oid_handler = REMOVE_DYLD_FIXUP_DATA(blob_oid->oid_handler);
        }
    }

    if (0 == oid_handler) {
        fprintf(stderr, "couldn't find _darwin_trap_ignition_get_blob\n");
        return;
    }

    if (oid_handler < img4_exc_seg->fileoff || oid_handler > img4_exc_seg->fileoff + img4_exc_seg->filesize) {
        fprintf(stderr, "Warning: _darwin_trap_ignition_get_blob isn't in __TEXT_EXEC\n");
    }

    u32 *sysctl_handler_fn = (u32*)&bkc_macho[oid_handler];

    if (PACIBSP != sysctl_handler_fn[0] && BTI_C != sysctl_handler_fn[0]) {
        fprintf(stderr, "Warning: _darwin_trap_ignition_get_blob doesn't start with PACIBSP or BTI C\n");
        fprintf(stderr, "opcode: 0x%X\n", sysctl_handler_fn[0]);
    }

    sysctl_handler_fn[0] = PACIBSP;
    sysctl_handler_fn[1] = MOVX(0,0);
    sysctl_handler_fn[2] = RETAB;
}

void patch_kc(u8 *bkc_macho) {
    patch_img4_deadlock(bkc_macho);
}

bool kc_uses_mte(u8 *bkc_macho) {
    seg_t *kc_exec_seg = macho_find_seg(bkc_macho, "__TEXT_EXEC");
    assert(kc_exec_seg);

    u32 *kc_text = (u32*)&bkc_macho[kc_exec_seg->fileoff];
    for (size_t i = 0; i < kc_exec_seg->vmsize / sizeof(u32); i++) {
        if (IS_LDG(kc_text[i])) return true;
    }

    return false;
}
