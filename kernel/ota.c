/*
 * Over-the-air OS updates (docs/os/OTA.md, glaze/ota.h): SYS_OTA_STATUS for
 * anyone, and SYS_OTA_STAGE for the file server, which streams a staged
 * update file in to be verified (ota_stage.c), then arms the journal. The
 * SPL applies it after the reset (boot/ota_apply.c); this kernel never
 * writes a boot block itself.
 */

#include "kernel.h"
#include "hw.h"
#include "drivers.h"
#include "proc.h"
#include "ota_stage.h"
#include "drivers/nand.h"
#include <glaze/bootinfo.h>
#include "ota_key.h"            /* generated from config/ota_pubkey.bin */

static struct gz_spl_info spl;  /* what this boot's SPL left */
static uint8_t chunk[2048] __attribute__((aligned(4)));

static const char *const reasons[] = {
    "", "journal", "mount", "file", "read", "crc", "format", "signature", "digest",
    "version", "spl", "space", "flash",
};

void ota_boot(void)
{
    struct gz_spl_info *info = (struct gz_spl_info *)KSEG1(GZ_SPL_INFO_PA);

    if (ram_boot || info->magic != GZ_SPL_INFO_MAGIC)
        return;
    spl = *info;
    spl.ota_version[sizeof(spl.ota_version) - 1] = 0;
    info->magic = 0;
    if (spl.ota_result == GZ_OTA_APPLIED)
        kprintf("ota: the SPL installed %s (%u blocks written)\n", spl.ota_version, spl.ota_detail);
    else if (spl.ota_result == GZ_OTA_REJECTED)
        kprintf("ota: the SPL rejected %s (%s)\n", spl.ota_version,
                spl.ota_detail < sizeof(reasons) / sizeof(reasons[0]) ? reasons[spl.ota_detail] : "?");
}

static bool supported(void)
{
    return !ram_boot && boot_nand_manifest() && spl.version >= GZ_SPL_VERSION &&
           (spl.features & GZ_SPL_F_OTA);
}

int sys_ota_status(struct addrspace *as, uint32_t uout, uint32_t size)
{
    struct gz_ota_status st;

    if (size != sizeof(st))
        return GZ_EINVAL;
    memset(&st, 0, sizeof(st));
    if (supported())
        st.flags |= GZ_OTA_S_SUPPORTED;
    if (!ram_boot && boot_nand_manifest())
        st.flags |= GZ_OTA_S_PHYSICAL;
    if (ota_stage_journal())
        st.flags |= GZ_OTA_S_STAGED;
    st.spl_version = (spl.features & GZ_SPL_F_OTA) ? spl.version : 0;
    st.last_result = spl.ota_result;
    st.last_detail = spl.ota_detail;
    strncpy(st.version, GLAZE_VERSION, sizeof(st.version) - 1);
    strncpy(st.build, GLAZE_BUILD, sizeof(st.build) - 1);
    memcpy(st.last_version, spl.ota_version, sizeof(st.last_version));
    return copy_to_as(as, uout, &st, sizeof(st));
}

static int flash_read(uint32_t page, uint8_t raw[528])
{
    nand_read_raw(page, raw);
    return 0;
}

static const struct ota_flash flash = {
    .read_raw = flash_read,
    .program_raw = nand_program_raw,
    .erase = nand_erase_block,
};

static int err(int e)
{
    return e ? GZ_OTA_ERR(e) : 0;
}

static int arm(void)
{
    const struct gz_ota_journal *j = ota_stage_journal();

    if (!j)
        return GZ_EPERM;
    int phys = ota_journal_write(&flash, j, boot_nand_map(), boot_nand_manifest());
    if (phys < 0)
        return err(-phys);
    kprintf("ota: journal for %s in block %d; resetting for the SPL to install it\n", j->version, phys);
    machine_reboot();
}

int sys_ota_stage(struct proc *p, uint32_t op, uint32_t a1, uint32_t a2, uint32_t a3)
{
    if (!(p->flags & PROC_FLASH))
        return GZ_EPERM;
    if (!supported())
        return err(GZ_OTA_E_SPL);
    switch (op) {
    case GZ_OTA_OP_BEGIN: {
        struct gz_ota_header h;
        char path[GZ_OTA_PATH_MAX];
        if (copy_from_as(&p->as, &h, a1, sizeof(h)) || copy_str_from_as(&p->as, path, a2, sizeof(path)) < 0)
            return GZ_EINVAL;
        return err(ota_stage_begin(&h, path, a3, GLAZE_VERSION, spl.version, ota_release_key));
    }
    case GZ_OTA_OP_DATA:
        if (a2 > GZ_OTA_DATA_MAX)
            return GZ_EINVAL;
        while (a2) {
            uint32_t n = a2 < sizeof(chunk) ? a2 : (uint32_t)sizeof(chunk);
            if (copy_from_as(&p->as, chunk, a1, n)) {
                ota_stage_abort();
                return GZ_EINVAL;
            }
            int rv = ota_stage_data(chunk, n);
            if (rv)
                return err(rv);
            a1 += n;
            a2 -= n;
        }
        return 0;
    case GZ_OTA_OP_COMMIT:
        return err(ota_stage_commit());
    case GZ_OTA_OP_ABORT:
        ota_stage_abort();
        return 0;
    case GZ_OTA_OP_ARM:
        return arm();
    default:
        return GZ_EINVAL;
    }
}
