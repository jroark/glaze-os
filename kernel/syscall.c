/* System call table. */

#include "kernel.h"
#include "hw.h"
#include "mips.h"
#include "proc.h"
#include "drivers.h"

#define PRIV()  (cur->proc->flags & PROC_PRIVILEGED)
#define AS      (&cur->proc->as)

static int sys_exit(uint32_t code)
{
    proc_exit(cur->proc, (int)code);
    return SYSRET_BLOCKED;
}

static int sys_yield(void)
{
    need_resched = true;
    return 0;
}

static int sys_sleep_ms(uint32_t ms)
{
    if (!ms)
        return sys_yield();
    thread_sleep_until(time_us() + (uint64_t)ms * 1000u);
    return SYSRET_BLOCKED;
}

static int sys_log(uint32_t ustr, uint32_t len)
{
    char buf[128];

    while (len) {
        uint32_t n = MIN(len, sizeof(buf) - 1);
        if (copy_from_as(AS, buf, ustr, n))
            return GZ_EINVAL;
        buf[n] = 0;
        kputs(buf);
        ustr += n;
        len -= n;
    }
    return 0;
}

static int sys_spawn(uint32_t upath, uint32_t flags)
{
    char path[48];

    if (copy_str_from_as(AS, path, upath, sizeof(path)) < 0)
        return GZ_EINVAL;
    flags &= PROC_PRIVILEGED | PROC_APP;     /* other flags are kernel-granted */
    if (flags && !PRIV())
        return GZ_EPERM;
    struct proc *p = proc_spawn(path, cur->proc->pid, flags);
    return p ? p->pid : GZ_ENOENT;
}

/* Installed apps never get privileges: only romfs programs are trusted. */
static int sys_spawn_image(uint32_t ubuf, uint32_t size, uint32_t uname)
{
    /* Package ids allow 24 characters. The process display name is shortened
     * by spawn(), after the complete package id has passed this boundary. */
    char name[32];

    if (copy_str_from_as(AS, name, uname, sizeof(name)) <= 0 || !gz_elf_range(ubuf, size, USER_TOP))
        return GZ_EINVAL;
    struct proc *p = proc_spawn_image(AS, ubuf, size, name, cur->proc->pid, PROC_APP);
    return p ? p->pid : GZ_EINVAL;
}

static int sys_kill(uint32_t pid)
{
    struct proc *p = proc_by_pid((int)pid);

    if (!p)
        return GZ_ENOENT;
    if (!PRIV() && p->parent != cur->proc->pid && p != cur->proc)
        return GZ_EPERM;
    proc_exit(p, -9);
    return p == cur->proc ? SYSRET_BLOCKED : 0;
}

static int sys_port_register(uint32_t port, uint32_t uname)
{
    char name[16];

    if (copy_str_from_as(AS, name, uname, sizeof(name)) < 0)
        return GZ_EINVAL;
    return ipc_port_register((int)port, name);
}

static int sys_port_lookup(uint32_t uname)
{
    char name[16];

    if (copy_str_from_as(AS, name, uname, sizeof(name)) < 0)
        return GZ_EINVAL;
    return ipc_port_lookup(name);
}

static int sys_shm_map(uint32_t id)
{
    uint32_t va = shm_map(AS, (int)id);
    return va ? (int)va : GZ_EINVAL;
}

static int sys_mmap_anon(uint32_t len)
{
    uint32_t va = as_mmap_anon(AS, len);
    return va ? (int)va : GZ_ENOMEM;
}

static int sys_time_us(uint32_t uout)
{
    uint64_t now = time_us();

    if (uout && copy_to_as(AS, uout, &now, sizeof(now)))
        return GZ_EINVAL;
    return (int)(uint32_t)(now / 1000u);
}

static int sys_fb_map(void)
{
    if (!PRIV())
        return GZ_EPERM;
    uint32_t va = as_map_phys(AS, FB_PA, FB_SIZE, PTE_UNCACHED);
    return va ? (int)va : GZ_ENOMEM;
}

static int sys_romfs_map(uint32_t uname, uint32_t usize)
{
    char name[48];

    if (copy_str_from_as(AS, name, uname, sizeof(name)) < 0)
        return GZ_EINVAL;
    const struct romfs_entry *e = romfs_lookup(name);
    if (!e)
        return GZ_ENOENT;
    /* Read-only, its pages read from NAND as the process touches them. */
    uint32_t va = as_map_romfs(AS, romfs_index(e), e->size, e->offset);
    if (!va)
        return GZ_ENOMEM;
    if (usize && copy_to_as(AS, usize, &e->size, sizeof(e->size)))
        return GZ_EINVAL;
    return (int)va;
}

static int sys_sysinfo(uint32_t uout)
{
    struct gz_sysinfo si;

    memset(&si, 0, sizeof(si));
    si.total_pages = SDRAM_SIZE / PAGE_SIZE;
    /* Cache pages nothing maps are taken back as soon as memory is needed. */
    si.free_pages = mm_free_pages() + romfs_reclaimable_pages();
    si.nprocs = (uint32_t)proc_count();
    si.uptime_ms = (uint32_t)(time_us() / 1000u);
    si.cpu_prid = read_c0_prid();
    strncpy(si.build, GLAZE_BUILD, sizeof(si.build) - 1);
    si.cache_pages = romfs_cache_pages();
    si.reclaimable_pages = romfs_reclaimable_pages();
    return copy_to_as(AS, uout, &si, sizeof(si));
}

static int sys_proc_alive(uint32_t pid)
{
    return proc_by_pid((int)pid) != NULL;
}

void syscall_dispatch(struct trapframe *tf)
{
    uint32_t a0 = tf->regs[R_A0], a1 = tf->regs[R_A1];
    uint32_t a2 = tf->regs[R_A2], a3 = tf->regs[R_A3];
    int rv;

    (void)a3;
    tf->epc += 4;
    switch (tf->regs[R_V0]) {
    case SYS_VM_MAP:        rv = as_loader_map(AS, a0, a1); break;
    case SYS_VM_PUBLISH:    rv = as_loader_publish(AS, a0, a1, a2); break;
    case SYS_VM_RELEASE:    rv = as_loader_release(AS, a0); break;
    case SYS_IMAGE_QUERY:   rv = image_query(AS, a0, a1); break;
    case SYS_IMAGE_MAP:     rv = image_map_user(AS, a0, a1, a2, a3); break;
    case SYS_IMAGE_RELEASE: rv = image_release(AS, a0); break;
    case SYS_IMAGE_SEAL:    rv = image_seal(AS, a0); break;
    case SYS_IMAGE_STATS:   rv = image_stats(AS, a0); break;
    case SYS_EXIT:          rv = sys_exit(a0); break;
    case SYS_YIELD:         rv = sys_yield(); break;
    case SYS_SLEEP_MS:      rv = sys_sleep_ms(a0); break;
    case SYS_LOG:           rv = sys_log(a0, a1); break;
    case SYS_SPAWN:         rv = sys_spawn(a0, a1); break;
    case SYS_SPAWN_IMAGE:   rv = sys_spawn_image(a0, a1, a2); break;
    case SYS_KILL:          rv = sys_kill(a0); break;
    case SYS_PORT_CREATE:   rv = ipc_port_create(cur->proc); break;
    case SYS_PORT_REGISTER: rv = sys_port_register(a0, a1); break;
    case SYS_PORT_LOOKUP:   rv = sys_port_lookup(a0); break;
    case SYS_SEND:          rv = ipc_send((int)a0, a1, a2); break;
    case SYS_RECV:          rv = ipc_recv((int)a0, a1, (int)a2); break;
    case SYS_REPLY:         rv = ipc_reply((int)a0, a1); break;
    case SYS_NOTIFY:        rv = ipc_notify((int)a0, a1); break;
    case SYS_MMAP_ANON:     rv = sys_mmap_anon(a0); break;
    case SYS_SHM_CREATE:    rv = shm_create(a0); break;
    case SYS_SHM_MAP:       rv = sys_shm_map(a0); break;
    case SYS_TIME_US:       rv = sys_time_us(a0); break;
    case SYS_RTC_GET:       rv = sys_rtc_get(AS, a0); break;
    case SYS_RTC_SET:       rv = sys_rtc_set(AS, a0); break;
    case SYS_FB_MAP:        rv = sys_fb_map(); break;
    case SYS_INPUT_STATUS:  rv = stowaway_status(); break;
    case SYS_INPUT_BIND:    rv = PRIV() ? input_bind((int)a0) : GZ_EPERM; break;
    case SYS_INPUT_READ:    rv = PRIV() ? input_read(AS, a0, a1) : GZ_EPERM; break;
    case SYS_SYSINFO:       rv = sys_sysinfo(a0); break;
    case SYS_TASK_SNAPSHOT: rv = proc_snapshot(AS, a0, a1); break;
    case SYS_TASK_STOP:     rv = proc_task_stop((int)a0); break;
    case SYS_PROC_BG:
        /* An app may only suspend itself, once SpringBoard has put it in the background. */
        rv = PRIV() || ((int)a0 == cur->proc->pid && a1 == GZ_PROC_SUSPENDED &&
                        (cur->proc->flags & PROC_BACKGROUND))
                 ? proc_set_bg((int)a0, (int)a1) : GZ_EPERM;
        break;
    case SYS_SHM_UNMAP:     rv = as_unmap_shm(AS, a0); break;
    case SYS_CHILD_NOTIFY:  rv = proc_child_notify((int)a0); break;
    case SYS_GETPID:        rv = cur->proc->pid; break;
    case SYS_ROMFS_MAP:     rv = sys_romfs_map(a0, a1); break;
    case SYS_PROC_ALIVE:    rv = sys_proc_alive(a0); break;
    case SYS_FLASH_READ:    rv = sys_flash_read(cur->proc, a0, a1); break;
    case SYS_FLASH_PROG:    rv = sys_flash_prog(cur->proc, a0, a1); break;
    case SYS_FLASH_ERASE:   rv = sys_flash_erase(cur->proc, a0); break;
    case SYS_FLASH_INFO:    rv = sys_flash_info(cur->proc, a0); break;
    case SYS_NAND_READ:     rv = sys_nand_read(cur->proc, a0, a1); break;
    case SYS_NAND_PROG:     rv = sys_nand_prog(cur->proc, a0, a1); break;
    case SYS_NAND_ERASE:    rv = sys_nand_erase(cur->proc, a0); break;
    case SYS_NET_INFO:      rv = sys_net_info(cur->proc, a0); break;
    case SYS_NET_BIND:      rv = sys_net_bind(cur->proc, (int)a0); break;
    case SYS_NET_SEND:      rv = sys_net_send(cur->proc, a0, a1); break;
    case SYS_NET_RECV:      rv = sys_net_recv(cur->proc, a0, a1); break;
    case SYS_AUDIO_BIND:    rv = sys_audio_bind(cur->proc, (int)a0); break;
    case SYS_AUDIO_START:   rv = sys_audio_start(cur->proc, a0, a1); break;
    case SYS_AUDIO_WRITE:   rv = sys_audio_write(cur->proc, a0, a1); break;
    case SYS_AUDIO_STOP:    rv = sys_audio_stop(cur->proc, a0); break;
    case SYS_AUDIO_STATUS:  rv = sys_audio_status(cur->proc, a0); break;
    case SYS_AUDIO_MIX:     rv = sys_audio_mix(cur->proc, a0, a1); break;
    case SYS_CF_INFO:       rv = sys_cf_info(cur->proc, a0); break;
    case SYS_CF_READ:       rv = sys_cf_read(cur->proc, a0, a1, a2); break;
    case SYS_CF_WRITE:      rv = sys_cf_write(cur->proc, a0, a1, a2); break;
    case SYS_THREAD_CREATE: rv = thread_spawn(a0, a1); break;
    case SYS_THREAD_EXIT:   rv = thread_exit(); break;
    case SYS_GETTID:        rv = cur->tid; break;
    case SYS_SEM_CREATE:    rv = sem_create(cur->proc, (int)a0); break;
    case SYS_SEM_WAIT:      rv = sem_wait((int)a0, (int)a1); break;
    case SYS_SEM_POST:      rv = sem_post((int)a0); break;
    case SYS_SEM_DESTROY:   rv = sem_destroy((int)a0); break;
    default:                rv = GZ_EINVAL; break;
    }
    if (rv != SYSRET_BLOCKED)
        tf->regs[R_V0] = (uint32_t)rv;
}
