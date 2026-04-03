/*
 * WHPX Backend — Lightweight VMM using Windows Hypervisor Platform
 *
 * Boots Linux directly using the hardware hypervisor (Hyper-V).
 * No QEMU or WSL required — just needs a Hyper-V capable CPU and
 * Windows 10 1803+ with "Windows Hypervisor Platform" feature enabled.
 *
 * Architecture:
 *   - Creates a single-vCPU VM partition via WinHvPlatform.dll
 *   - Loads Linux bzImage kernel using the 64-bit boot protocol
 *   - Loads initramfs for zero-disk-dependency boot
 *   - Emulates 16550A UART (serial) for host ↔ guest I/O
 *   - Minimal PIC/PIT emulation to satisfy kernel boot requirements
 *   - Commands execute via the serial console shell
 *
 * Requirements:
 *   - Windows 10 SDK 10.0.17134.0+ (for WinHvPlatform.h)
 *   - CPU with VT-x/AMD-V
 *   - "Windows Hypervisor Platform" Windows feature enabled
 *   - Linux bzImage kernel + initramfs/cpio rootfs
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <WinHvPlatform.h>
#include "backend.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ============================================================
 * Memory layout constants
 * ============================================================ */
#define GUEST_MEM_DEFAULT   (512ULL * 1024 * 1024)
#define GDT_ADDR            0x1000ULL
#define PML4_ADDR           0x2000ULL
#define PDPT_ADDR           0x3000ULL
#define PD_BASE_ADDR        0x4000ULL   /* PD0 at 0x4000, PD1 at 0x5000, ... */
#define BOOT_PARAMS_ADDR    0x10000ULL
#define CMDLINE_ADDR        0x20000ULL
#define CMDLINE_MAX         2048
#define KERNEL_LOAD_ADDR    0x100000ULL /* 1 MB — standard for LOADED_HIGH */
#define KERNEL_64BIT_ENTRY  0x200       /* offset from PM code start */

/* Number of 1GB regions to identity-map (with 2MB pages) */
#define IDENTITY_MAP_GB     4

/* bzImage header offsets (byte offsets from start of boot sector) */
#define HDR_SETUP_SECTS     0x1F1
#define HDR_SYSSIZE         0x1F4
#define HDR_BOOT_FLAG       0x1FE
#define HDR_HEADER_MAGIC    0x202
#define HDR_VERSION         0x206
#define HDR_TYPE_OF_LOADER  0x210
#define HDR_LOADFLAGS       0x211
#define HDR_RAMDISK_IMAGE   0x218
#define HDR_RAMDISK_SIZE    0x21C
#define HDR_HEAP_END_PTR    0x224
#define HDR_CMD_LINE_PTR    0x228
#define HDR_INIT_SIZE       0x260
#define LOADFLAG_LOADED_HIGH 0x01
#define LOADFLAG_CAN_USE_HEAP 0x80

/* Serial port */
#define COM1_BASE           0x3F8
#define COM1_IRQ            4
#define SERIAL_BUF_SIZE     (256 * 1024)

/* x86 bits */
#define CR0_PE  (1ULL << 0)
#define CR0_ET  (1ULL << 4)
#define CR0_NE  (1ULL << 5)
#define CR0_WP  (1ULL << 16)
#define CR0_PG  (1ULL << 31)
#define CR4_PAE (1ULL << 5)
#define CR4_OSFXSR   (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define EFER_SCE (1ULL << 0)
#define EFER_LME (1ULL << 8)
#define EFER_LMA (1ULL << 10)
#define EFER_NXE (1ULL << 11)

/* PTE flags */
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITE    (1ULL << 1)
#define PTE_PS       (1ULL << 7)   /* 2MB page */

/* GDT segment attributes (VMX encoding) */
#define SEG_CODE64  0xA09B  /* L=1 D=0 P=1 S=1 Type=0xB (exec/read/accessed) */
#define SEG_DATA64  0xC093  /* G=1 D/B=1 P=1 S=1 Type=0x3 (read/write/accessed) */

/* ============================================================
 * 16550A UART emulation
 * ============================================================ */
typedef struct {
    /* Ring buffers */
    char    tx_buf[SERIAL_BUF_SIZE];  /* Guest → Host output  */
    int     tx_head, tx_tail;
    char    rx_buf[SERIAL_BUF_SIZE];  /* Host → Guest input   */
    int     rx_head, rx_tail;

    /* UART registers */
    uint8_t ier, iir, lcr, mcr, lsr, msr, scr;
    uint8_t dll, dlm, fcr;
} whpx_serial_t;

static void serial_init(whpx_serial_t *s) {
    memset(s, 0, sizeof(*s));
    s->lsr = 0x60;  /* THR empty + transmitter empty */
    s->msr = 0x30;  /* CTS + DSR asserted */
    s->iir = 0x01;  /* No interrupt pending */
}

static int serial_rx_available(whpx_serial_t *s) {
    return s->rx_head != s->rx_tail;
}

static void serial_push_rx(whpx_serial_t *s, const char *data, int len) {
    for (int i = 0; i < len; i++) {
        int next = (s->rx_head + 1) % SERIAL_BUF_SIZE;
        if (next == s->rx_tail) break;  /* full */
        s->rx_buf[s->rx_head] = data[i];
        s->rx_head = next;
    }
}

static int serial_pop_tx(whpx_serial_t *s, char *buf, int max) {
    int count = 0;
    while (s->tx_tail != s->tx_head && count < max) {
        buf[count++] = s->tx_buf[s->tx_tail];
        s->tx_tail = (s->tx_tail + 1) % SERIAL_BUF_SIZE;
    }
    return count;
}

static uint8_t serial_read(whpx_serial_t *s, uint16_t offset) {
    int dlab = (s->lcr >> 7) & 1;
    switch (offset) {
    case 0: /* RBR / DLL */
        if (dlab) return s->dll;
        if (serial_rx_available(s)) {
            uint8_t c = (uint8_t)s->rx_buf[s->rx_tail];
            s->rx_tail = (s->rx_tail + 1) % SERIAL_BUF_SIZE;
            if (!serial_rx_available(s))
                s->lsr &= ~0x01;  /* Clear data-ready bit */
            return c;
        }
        return 0;
    case 1: return dlab ? s->dlm : s->ier;
    case 2: return s->iir;
    case 3: return s->lcr;
    case 4: return s->mcr;
    case 5: /* LSR */
        s->lsr |= 0x60;  /* THR empty + transmitter idle */
        if (serial_rx_available(s))
            s->lsr |= 0x01;  /* Data ready */
        else
            s->lsr &= ~0x01;
        return s->lsr;
    case 6: return s->msr;
    case 7: return s->scr;
    default: return 0;
    }
}

static void serial_write(whpx_serial_t *s, uint16_t offset, uint8_t val) {
    int dlab = (s->lcr >> 7) & 1;
    switch (offset) {
    case 0: /* THR / DLL */
        if (dlab) { s->dll = val; return; }
        /* Guest wrote a character — store in TX buffer */
        {
            int next = (s->tx_head + 1) % SERIAL_BUF_SIZE;
            if (next != s->tx_tail) {
                s->tx_buf[s->tx_head] = (char)val;
                s->tx_head = next;
            }
        }
        break;
    case 1: if (dlab) s->dlm = val; else s->ier = val; break;
    case 2: s->fcr = val; break;
    case 3: s->lcr = val; break;
    case 4: s->mcr = val; break;
    case 7: s->scr = val; break;
    }
}

/* ============================================================
 * LAPIC (Local APIC) emulation
 *
 * The LAPIC is memory-mapped at 0xFEE00000. The kernel accesses it
 * for interrupt control, timer setup, and EOI signaling.
 * We emulate the critical registers to allow kernel boot.
 * ============================================================ */
#define LAPIC_BASE          0xFEE00000ULL
#define LAPIC_SIZE          0x1000
#define LAPIC_ID            0x020
#define LAPIC_VERSION       0x030
#define LAPIC_TPR           0x080
#define LAPIC_EOI           0x0B0
#define LAPIC_SVR           0x0F0
#define LAPIC_ESR           0x280
#define LAPIC_ICR_LO        0x300
#define LAPIC_ICR_HI        0x310
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_LVT_THERMAL   0x330
#define LAPIC_LVT_PERFCNT   0x340
#define LAPIC_LVT_LINT0     0x350
#define LAPIC_LVT_LINT1     0x360
#define LAPIC_LVT_ERROR     0x370
#define LAPIC_TIMER_ICR      0x380   /* Initial Count Register */
#define LAPIC_TIMER_CCR      0x390   /* Current Count Register */
#define LAPIC_TIMER_DCR      0x3E0   /* Divide Configuration */

/* LVT bits */
#define LVT_MASKED          (1 << 16)
#define LVT_TIMER_PERIODIC  (1 << 17)

/* Simulated bus frequency: 100 MHz (arbitrary — kernel calibrates) */
#define LAPIC_BUS_FREQ_HZ   100000000ULL

typedef struct {
    uint32_t regs[LAPIC_SIZE / 4];  /* All registers as 32-bit words */
    /* Timer tracking */
    LARGE_INTEGER timer_start;       /* QPC when timer was armed */
    LARGE_INTEGER qpc_freq;          /* QPC frequency */
    uint64_t      timer_period_ticks;/* QPC ticks per timer period */
    int           timer_armed;
    int           timer_irq_pending;
} whpx_lapic_t;

static void lapic_init(whpx_lapic_t *a) {
    memset(a, 0, sizeof(*a));
    a->regs[LAPIC_ID / 4]      = 0;          /* APIC ID 0 */
    a->regs[LAPIC_VERSION / 4]  = 0x00050014; /* version 0x14, 5 LVT entries */
    a->regs[LAPIC_SVR / 4]     = 0x000000FF; /* SVR: vector 0xFF, APIC disabled */
    a->regs[LAPIC_LVT_TIMER / 4]   = LVT_MASKED;
    a->regs[LAPIC_LVT_THERMAL / 4] = LVT_MASKED;
    a->regs[LAPIC_LVT_PERFCNT / 4] = LVT_MASKED;
    a->regs[LAPIC_LVT_LINT0 / 4]   = LVT_MASKED;
    a->regs[LAPIC_LVT_LINT1 / 4]   = LVT_MASKED;
    a->regs[LAPIC_LVT_ERROR / 4]   = LVT_MASKED;
    a->regs[LAPIC_TIMER_DCR / 4]    = 0;      /* divide by 2 */
    QueryPerformanceFrequency(&a->qpc_freq);
}

static uint32_t lapic_get_divisor(whpx_lapic_t *a) {
    uint32_t dcr = a->regs[LAPIC_TIMER_DCR / 4] & 0x0B;
    /* Bits 3,1,0 encode: 0=2,1=4,2=8,3=16,8=32,9=64,A=128,B=1 */
    static const uint32_t div_table[] = {2,4,8,16,32,64,128,1};
    int idx = (dcr & 3) | ((dcr >> 1) & 4);
    return div_table[idx & 7];
}

static void lapic_arm_timer(whpx_lapic_t *a) {
    uint32_t icr = a->regs[LAPIC_TIMER_ICR / 4];
    if (icr == 0) { a->timer_armed = 0; return; }
    uint32_t divisor = lapic_get_divisor(a);
    /* period_ns = icr * divisor / bus_freq * 1e9 */
    double period_s = (double)icr * divisor / LAPIC_BUS_FREQ_HZ;
    a->timer_period_ticks = (uint64_t)(period_s * a->qpc_freq.QuadPart);
    if (a->timer_period_ticks == 0) a->timer_period_ticks = 1;
    QueryPerformanceCounter(&a->timer_start);
    a->timer_armed = 1;
    a->timer_irq_pending = 0;
}

static int lapic_check_timer(whpx_lapic_t *a) {
    if (!a->timer_armed) return 0;
    uint32_t lvt = a->regs[LAPIC_LVT_TIMER / 4];
    if (lvt & LVT_MASKED) return 0;
    if (!(a->regs[LAPIC_SVR / 4] & 0x100)) return 0; /* APIC disabled */

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t elapsed = (uint64_t)(now.QuadPart - a->timer_start.QuadPart);

    if (elapsed >= a->timer_period_ticks) {
        a->timer_irq_pending = 1;
        if (lvt & LVT_TIMER_PERIODIC) {
            /* Reload for periodic mode */
            a->timer_start.QuadPart += (LONGLONG)a->timer_period_ticks;
        } else {
            a->timer_armed = 0;
        }
        return lvt & 0xFF; /* return vector */
    }
    return 0;
}

static uint32_t lapic_read(whpx_lapic_t *a, uint32_t offset) {
    offset &= 0xFFF;
    if (offset >= LAPIC_SIZE) return 0;

    switch (offset) {
    case LAPIC_TIMER_CCR: {
        /* Return remaining count */
        if (!a->timer_armed) return 0;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        uint64_t elapsed = (uint64_t)(now.QuadPart - a->timer_start.QuadPart);
        if (elapsed >= a->timer_period_ticks) return 0;
        uint64_t remaining = a->timer_period_ticks - elapsed;
        uint32_t divisor = lapic_get_divisor(a);
        return (uint32_t)(remaining * LAPIC_BUS_FREQ_HZ /
                          (a->qpc_freq.QuadPart * divisor));
    }
    default:
        return a->regs[offset / 4];
    }
}

static void lapic_write(whpx_lapic_t *a, uint32_t offset, uint32_t value) {
    offset &= 0xFFF;
    if (offset >= LAPIC_SIZE) return;

    switch (offset) {
    case LAPIC_EOI:
        /* End of interrupt — clear pending */
        a->timer_irq_pending = 0;
        break;
    case LAPIC_SVR:
        a->regs[offset / 4] = value;
        break;
    case LAPIC_LVT_TIMER:
    case LAPIC_LVT_THERMAL:
    case LAPIC_LVT_PERFCNT:
    case LAPIC_LVT_LINT0:
    case LAPIC_LVT_LINT1:
    case LAPIC_LVT_ERROR:
        a->regs[offset / 4] = value;
        break;
    case LAPIC_TIMER_ICR:
        a->regs[offset / 4] = value;
        lapic_arm_timer(a);
        break;
    case LAPIC_TIMER_DCR:
        a->regs[offset / 4] = value & 0x0B;
        break;
    case LAPIC_ICR_LO:
        a->regs[offset / 4] = value;
        /* IPI delivery — ignore for single-CPU */
        break;
    case LAPIC_ICR_HI:
    case LAPIC_TPR:
    case LAPIC_ESR:
        a->regs[offset / 4] = value;
        break;
    case LAPIC_ID:
        a->regs[offset / 4] = value & 0xFF000000;
        break;
    default:
        a->regs[offset / 4] = value;
        break;
    }
}

/* ============================================================
 * Minimal x86 instruction decoder for MMIO emulation
 *
 * Decodes MOV-class instructions to extract the register operand.
 * Returns the GP register index (0=RAX..15=R15), or -1 on failure.
 * Sets *is_imm=1 and *imm_val if the source is an immediate.
 * ============================================================ */
static const WHV_REGISTER_NAME gp_reg_names[16] = {
    WHvX64RegisterRax, WHvX64RegisterRcx, WHvX64RegisterRdx, WHvX64RegisterRbx,
    WHvX64RegisterRsp, WHvX64RegisterRbp, WHvX64RegisterRsi, WHvX64RegisterRdi,
    WHvX64RegisterR8,  WHvX64RegisterR9,  WHvX64RegisterR10, WHvX64RegisterR11,
    WHvX64RegisterR12, WHvX64RegisterR13, WHvX64RegisterR14, WHvX64RegisterR15,
};

static int decode_mmio_reg(const uint8_t *insn, int len,
                           int *is_imm, uint32_t *imm_val) {
    int i = 0;
    int rex = 0, rex_r = 0;
    *is_imm = 0;

    if (i >= len) return -1;

    /* Skip legacy prefixes (66, 67, F2, F3, 2E, 3E, 26, 36, etc.) */
    while (i < len) {
        uint8_t b = insn[i];
        if (b == 0x66 || b == 0x67 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 ||
            b == 0x64 || b == 0x65) {
            i++;
        } else {
            break;
        }
    }

    /* REX prefix? */
    if (i < len && (insn[i] & 0xF0) == 0x40) {
        rex = insn[i];
        rex_r = (rex >> 2) & 1;
        i++;
    }

    if (i >= len) return -1;
    uint8_t opcode = insn[i++];

    /* Two-byte opcode escape */
    if (opcode == 0x0F && i < len) {
        opcode = insn[i++];
        /* 0F B6 = movzx r, r/m8; 0F B7 = movzx r, r/m16 */
        if ((opcode == 0xB6 || opcode == 0xB7) && i < len) {
            int modrm = insn[i];
            int reg = ((modrm >> 3) & 7) | (rex_r << 3);
            return reg;
        }
        return -1;
    }

    switch (opcode) {
    case 0x89: /* MOV r/m, r32/64 (write: reg is source) */
    case 0x8B: /* MOV r32/64, r/m (read: reg is destination) */
        if (i >= len) return -1;
        {
            int modrm = insn[i];
            int reg = ((modrm >> 3) & 7) | (rex_r << 3);
            return reg;
        }
    case 0xC7: /* MOV r/m, imm32 (write immediate) */
        if (i >= len) return -1;
        {
            int modrm = insn[i]; i++;
            int mod = (modrm >> 6) & 3;
            int rm = modrm & 7;
            /* Skip displacement */
            if (mod == 1) i += 1;
            else if (mod == 2 || (mod == 0 && rm == 5)) i += 4;
            if (rm == 4 && mod != 3) i += 1; /* SIB byte */
            /* Read immediate */
            if (i + 4 <= len) {
                *imm_val = *(uint32_t *)(insn + i);
                *is_imm = 1;
            }
            return 0; /* target is implied by modrm, value is immediate */
        }
    case 0xA1: /* MOV rAX, moffs — read to RAX */
        return 0;
    case 0xA3: /* MOV moffs, rAX — write from RAX */
        return 0;
    case 0x31: /* XOR r/m, r */
    case 0x09: /* OR r/m, r */
    case 0x21: /* AND r/m, r */
    case 0x01: /* ADD r/m, r */
        if (i >= len) return -1;
        {
            int modrm = insn[i];
            int reg = ((modrm >> 3) & 7) | (rex_r << 3);
            return reg;
        }
    default:
        return -1;
    }
}

/* ============================================================
 * Dynamic DLL loading
 * ============================================================ */
typedef HRESULT (WINAPI *PFN_WHvGetCapability)(
    WHV_CAPABILITY_CODE, void*, UINT32, UINT32*);
typedef HRESULT (WINAPI *PFN_WHvCreatePartition)(WHV_PARTITION_HANDLE*);
typedef HRESULT (WINAPI *PFN_WHvSetupPartition)(WHV_PARTITION_HANDLE);
typedef HRESULT (WINAPI *PFN_WHvDeletePartition)(WHV_PARTITION_HANDLE);
typedef HRESULT (WINAPI *PFN_WHvSetPartitionProperty)(
    WHV_PARTITION_HANDLE, WHV_PARTITION_PROPERTY_CODE,
    const void*, UINT32);
typedef HRESULT (WINAPI *PFN_WHvMapGpaRange)(
    WHV_PARTITION_HANDLE, void*, WHV_GUEST_PHYSICAL_ADDRESS,
    UINT64, WHV_MAP_GPA_RANGE_FLAGS);
typedef HRESULT (WINAPI *PFN_WHvUnmapGpaRange)(
    WHV_PARTITION_HANDLE, WHV_GUEST_PHYSICAL_ADDRESS, UINT64);
typedef HRESULT (WINAPI *PFN_WHvCreateVirtualProcessor)(
    WHV_PARTITION_HANDLE, UINT32, UINT32);
typedef HRESULT (WINAPI *PFN_WHvDeleteVirtualProcessor)(
    WHV_PARTITION_HANDLE, UINT32);
typedef HRESULT (WINAPI *PFN_WHvRunVirtualProcessor)(
    WHV_PARTITION_HANDLE, UINT32, void*, UINT32);
typedef HRESULT (WINAPI *PFN_WHvGetVirtualProcessorRegisters)(
    WHV_PARTITION_HANDLE, UINT32, const WHV_REGISTER_NAME*,
    UINT32, WHV_REGISTER_VALUE*);
typedef HRESULT (WINAPI *PFN_WHvSetVirtualProcessorRegisters)(
    WHV_PARTITION_HANDLE, UINT32, const WHV_REGISTER_NAME*,
    UINT32, const WHV_REGISTER_VALUE*);
typedef HRESULT (WINAPI *PFN_WHvCancelRunVirtualProcessor)(
    WHV_PARTITION_HANDLE, UINT32, UINT32);

typedef struct {
    PFN_WHvGetCapability                 GetCapability;
    PFN_WHvCreatePartition               CreatePartition;
    PFN_WHvSetupPartition                SetupPartition;
    PFN_WHvDeletePartition               DeletePartition;
    PFN_WHvSetPartitionProperty          SetPartitionProperty;
    PFN_WHvMapGpaRange                   MapGpaRange;
    PFN_WHvUnmapGpaRange                 UnmapGpaRange;
    PFN_WHvCreateVirtualProcessor        CreateVP;
    PFN_WHvDeleteVirtualProcessor        DeleteVP;
    PFN_WHvRunVirtualProcessor           RunVP;
    PFN_WHvGetVirtualProcessorRegisters  GetVPRegs;
    PFN_WHvSetVirtualProcessorRegisters  SetVPRegs;
    PFN_WHvCancelRunVirtualProcessor     CancelRunVP;
} whpx_api_t;

/* ============================================================
 * Backend state
 * ============================================================ */
typedef struct {
    int     running;
    char    last_error_buf[1024];
    const linux_config_t *config;

    /* WHPX */
    HMODULE         hmod;
    whpx_api_t      api;
    WHV_PARTITION_HANDLE partition;
    int             partition_created;
    int             vp_created;

    /* Guest memory */
    uint8_t *guest_mem;
    size_t   guest_mem_size;

    /* Devices */
    whpx_serial_t serial;
    whpx_lapic_t  lapic;
    uint8_t pic_master_imr, pic_slave_imr;
    uint8_t pic_master_icw_step, pic_slave_icw_step;
    uint8_t pic_master_vector_base, pic_slave_vector_base;
    uint8_t cmos_index;
    uint32_t pci_addr;  /* PCI config address register (0xCF8) */

    /* VM thread */
    HANDLE           vm_thread;
    volatile LONG    vm_stop;
    volatile LONG    booted;
    CRITICAL_SECTION serial_lock;
    HANDLE           output_event;
} whpx_state_t;

static void whpx_set_error(whpx_state_t *st, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->last_error_buf, sizeof(st->last_error_buf), fmt, ap);
    va_end(ap);
}

/* ============================================================
 * Load WinHvPlatform.dll
 * ============================================================ */
static int whpx_load_api(whpx_state_t *st) {
    st->hmod = LoadLibraryExW(L"WinHvPlatform.dll", NULL,
                               LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!st->hmod) {
        whpx_set_error(st, "WinHvPlatform.dll not found — enable "
                       "'Windows Hypervisor Platform' in Windows Features");
        return 0;
    }
#define LOAD(field, fname) st->api.field = (PFN_WHv##fname) \
    GetProcAddress(st->hmod, "WHv" #fname); \
    if (!st->api.field) { \
        whpx_set_error(st, "Missing export: WHv" #fname); return 0; }
    LOAD(GetCapability, GetCapability)
    LOAD(CreatePartition, CreatePartition)
    LOAD(SetupPartition, SetupPartition)
    LOAD(DeletePartition, DeletePartition)
    LOAD(SetPartitionProperty, SetPartitionProperty)
    LOAD(MapGpaRange, MapGpaRange)
    LOAD(UnmapGpaRange, UnmapGpaRange)
    LOAD(CreateVP, CreateVirtualProcessor)
    LOAD(DeleteVP, DeleteVirtualProcessor)
    LOAD(RunVP, RunVirtualProcessor)
    LOAD(GetVPRegs, GetVirtualProcessorRegisters)
    LOAD(SetVPRegs, SetVirtualProcessorRegisters)
    LOAD(CancelRunVP, CancelRunVirtualProcessor)
#undef LOAD
    return 1;
}

/* ============================================================
 * Check hypervisor presence
 * ============================================================ */
static int whpx_check_hypervisor(whpx_state_t *st) {
    WHV_CAPABILITY cap;
    UINT32 written = 0;
    HRESULT hr = st->api.GetCapability(
        WHvCapabilityCodeHypervisorPresent, &cap, sizeof(cap), &written);
    if (FAILED(hr) || !cap.HypervisorPresent) {
        whpx_set_error(st, "Hyper-V hypervisor not present (HRESULT 0x%08lX). "
                       "Enable Hyper-V or Windows Hypervisor Platform.",
                       (unsigned long)hr);
        return 0;
    }
    return 1;
}

/* ============================================================
 * Set up identity-mapped page tables + GDT in guest memory
 * ============================================================ */
static void whpx_setup_tables(whpx_state_t *st) {
    uint8_t *mem = st->guest_mem;

    /* --- GDT at GDT_ADDR --- */
    uint64_t *gdt = (uint64_t *)(mem + GDT_ADDR);
    gdt[0] = 0;                         /* Null descriptor */
    gdt[1] = 0x00AF9B000000FFFFULL;     /* 64-bit code: L=1 D=0 */
    gdt[2] = 0x00CF93000000FFFFULL;     /* 64-bit data: G=1 D/B=1 */

    /* --- Page tables: identity map first IDENTITY_MAP_GB GB with 2MB pages --- */
    uint64_t *pml4 = (uint64_t *)(mem + PML4_ADDR);
    uint64_t *pdpt = (uint64_t *)(mem + PDPT_ADDR);
    memset(pml4, 0, 4096);
    memset(pdpt, 0, 4096);
    pml4[0] = PDPT_ADDR | PTE_PRESENT | PTE_WRITE;

    for (int gb = 0; gb < IDENTITY_MAP_GB; gb++) {
        uint64_t pd_addr = PD_BASE_ADDR + (uint64_t)gb * 4096;
        pdpt[gb] = pd_addr | PTE_PRESENT | PTE_WRITE;

        uint64_t *pd = (uint64_t *)(mem + pd_addr);
        memset(pd, 0, 4096);
        for (int i = 0; i < 512; i++) {
            uint64_t phys = (uint64_t)gb * (1ULL << 30) + (uint64_t)i * (2ULL << 20);
            pd[i] = phys | PTE_PRESENT | PTE_WRITE | PTE_PS;
        }
    }
}

/* ============================================================
 * Load bzImage kernel + initramfs into guest memory
 * ============================================================ */
static int whpx_load_kernel(whpx_state_t *st, const char *kernel_path,
                            const char *rootfs_path) {
    uint8_t *mem = st->guest_mem;

    /* --- Read kernel file --- */
    HANDLE hk = CreateFileA(kernel_path, GENERIC_READ, FILE_SHARE_READ,
                            NULL, OPEN_EXISTING, 0, NULL);
    if (hk == INVALID_HANDLE_VALUE) {
        whpx_set_error(st, "Cannot open kernel: %s (error %lu)",
                       kernel_path, GetLastError());
        return 0;
    }
    DWORD kernel_size = GetFileSize(hk, NULL);
    uint8_t *kernel_data = (uint8_t *)malloc(kernel_size);
    if (!kernel_data) { CloseHandle(hk); return 0; }
    DWORD bytes_read;
    ReadFile(hk, kernel_data, kernel_size, &bytes_read, NULL);
    CloseHandle(hk);

    if (bytes_read < 0x260) {
        whpx_set_error(st, "Kernel too small (%lu bytes)", bytes_read);
        free(kernel_data);
        return 0;
    }

    /* Verify bzImage magic */
    if (memcmp(kernel_data + HDR_HEADER_MAGIC, "HdrS", 4) != 0) {
        whpx_set_error(st, "Not a valid bzImage (missing HdrS signature)");
        free(kernel_data);
        return 0;
    }

    uint16_t boot_proto = *(uint16_t *)(kernel_data + HDR_VERSION);
    LINUX_LOG(st->config, "WHPX: kernel boot protocol version %d.%d",
              boot_proto >> 8, boot_proto & 0xFF);

    if (boot_proto < 0x0206) {
        whpx_set_error(st, "Kernel boot protocol %d.%d too old (need >= 2.06)",
                       boot_proto >> 8, boot_proto & 0xFF);
        free(kernel_data);
        return 0;
    }

    /* Parse setup header */
    uint8_t setup_sects = kernel_data[HDR_SETUP_SECTS];
    if (setup_sects == 0) setup_sects = 4;
    uint32_t setup_size = (setup_sects + 1) * 512;
    uint32_t pm_code_offset = setup_size;
    uint32_t pm_code_size = bytes_read - pm_code_offset;

    LINUX_LOG(st->config, "WHPX: setup_sects=%d, PM code at offset %u (%u bytes)",
              setup_sects, pm_code_offset, pm_code_size);

    /* Copy protected-mode kernel code to KERNEL_LOAD_ADDR */
    if (KERNEL_LOAD_ADDR + pm_code_size > st->guest_mem_size) {
        whpx_set_error(st, "Kernel too large for guest memory");
        free(kernel_data);
        return 0;
    }
    memcpy(mem + KERNEL_LOAD_ADDR, kernel_data + pm_code_offset, pm_code_size);

    /* --- Build boot_params (zero page) at BOOT_PARAMS_ADDR --- */
    uint8_t *bp = mem + BOOT_PARAMS_ADDR;
    memset(bp, 0, 4096);

    /* Copy the setup header from the kernel into boot_params */
    uint32_t hdr_size = setup_size - 0x1F1;
    if (hdr_size > 4096 - 0x1F1) hdr_size = 4096 - 0x1F1;
    memcpy(bp + 0x1F1, kernel_data + 0x1F1, hdr_size);

    free(kernel_data);

    /* Override required fields */
    bp[HDR_TYPE_OF_LOADER] = 0xFF;  /* Unknown bootloader */
    bp[HDR_LOADFLAGS] |= LOADFLAG_LOADED_HIGH | LOADFLAG_CAN_USE_HEAP;
    *(uint16_t *)(bp + HDR_HEAP_END_PTR) = 0xDE00;
    *(uint32_t *)(bp + HDR_CMD_LINE_PTR) = (uint32_t)CMDLINE_ADDR;

    /* Set e820 memory map in boot_params (offset 0x2D0 = e820_table) */
    /* Entry format: 20 bytes each (addr:8, size:8, type:4) */
    {
        uint8_t *e820 = bp + 0x2D0;
        /* Entry 0: Low memory 0 - 0x9FC00 (usable, type=1) */
        *(uint64_t *)(e820 + 0)  = 0;         /* addr */
        *(uint64_t *)(e820 + 8)  = 0x9FC00;   /* size */
        *(uint32_t *)(e820 + 16) = 1;         /* type */
        /* Entry 1: 1MB to end of guest memory (usable) */
        *(uint64_t *)(e820 + 20) = 0x100000;
        *(uint64_t *)(e820 + 28) = st->guest_mem_size - 0x100000;
        *(uint32_t *)(e820 + 36) = 1;
        bp[0x1E8] = 2;  /* e820_entries count */
    }

    /* --- Command line --- */
    const char *cmdline = "console=ttyS0 earlyprintk=ttyS0 rdinit=/init root=/dev/ram0 rw "
                          "noacpi pci=off nosmp "
                          "clocksource=jiffies lpj=1000000 notsc "
                          "no_timer_check nmi_watchdog=0 panic=10";
    size_t cmdline_len = strlen(cmdline);
    if (cmdline_len >= CMDLINE_MAX) cmdline_len = CMDLINE_MAX - 1;
    memcpy(mem + CMDLINE_ADDR, cmdline, cmdline_len);
    mem[CMDLINE_ADDR + cmdline_len] = '\0';

    /* --- Load initramfs/rootfs if provided --- */
    if (rootfs_path) {
        HANDLE hr_f = CreateFileA(rootfs_path, GENERIC_READ, FILE_SHARE_READ,
                                  NULL, OPEN_EXISTING, 0, NULL);
        if (hr_f != INVALID_HANDLE_VALUE) {
            DWORD rd_size = GetFileSize(hr_f, NULL);
            /* Place initramfs right after the kernel */
            uint64_t rd_addr = KERNEL_LOAD_ADDR + pm_code_size;
            /* Align to page boundary */
            rd_addr = (rd_addr + 4095) & ~4095ULL;

            if (rd_addr + rd_size <= st->guest_mem_size) {
                DWORD rd_read;
                ReadFile(hr_f, mem + rd_addr, rd_size, &rd_read, NULL);
                *(uint32_t *)(bp + HDR_RAMDISK_IMAGE) = (uint32_t)rd_addr;
                *(uint32_t *)(bp + HDR_RAMDISK_SIZE) = rd_read;
                LINUX_LOG(st->config, "WHPX: initramfs at 0x%llX (%lu bytes)",
                          rd_addr, rd_read);
                LINUX_LOG(st->config, "WHPX: boot_params ramdisk_image=0x%X ramdisk_size=0x%X",
                          *(uint32_t *)(bp + HDR_RAMDISK_IMAGE),
                          *(uint32_t *)(bp + HDR_RAMDISK_SIZE));
            } else {
                LINUX_LOG(st->config, "WHPX: initramfs too large, skipping");
            }
            CloseHandle(hr_f);
        } else {
            LINUX_LOG(st->config, "WHPX: cannot open rootfs: %s", rootfs_path);
        }
    }

    return 1;
}

/* ============================================================
 * Set initial vCPU registers for 64-bit boot
 * ============================================================ */
static int whpx_setup_vcpu(whpx_state_t *st) {
    WHV_REGISTER_NAME names[] = {
        WHvX64RegisterRip, WHvX64RegisterRflags,
        WHvX64RegisterRsi,
        WHvX64RegisterCs, WHvX64RegisterDs, WHvX64RegisterEs,
        WHvX64RegisterFs, WHvX64RegisterGs, WHvX64RegisterSs,
        WHvX64RegisterGdtr, WHvX64RegisterIdtr,
        WHvX64RegisterCr0, WHvX64RegisterCr3, WHvX64RegisterCr4,
        WHvX64RegisterEfer,
    };
    WHV_REGISTER_VALUE vals[15];
    memset(vals, 0, sizeof(vals));

    int i = 0;
    /* RIP = 64-bit kernel entry */
    vals[i++].Reg64 = KERNEL_LOAD_ADDR + KERNEL_64BIT_ENTRY;
    /* RFLAGS = reserved bit 1 set */
    vals[i++].Reg64 = 0x2;
    /* RSI = pointer to boot_params */
    vals[i++].Reg64 = BOOT_PARAMS_ADDR;

    /* CS: 64-bit code segment, selector 0x08 */
    vals[i].Segment.Selector = 0x08;
    vals[i].Segment.Base = 0;
    vals[i].Segment.Limit = 0xFFFFFFFF;
    vals[i].Segment.Attributes = SEG_CODE64;
    i++;

    /* DS, ES, FS, GS, SS: data segment, selector 0x10 */
    for (int s = 0; s < 5; s++) {
        vals[i].Segment.Selector = 0x10;
        vals[i].Segment.Base = 0;
        vals[i].Segment.Limit = 0xFFFFFFFF;
        vals[i].Segment.Attributes = SEG_DATA64;
        i++;
    }

    /* GDTR */
    vals[i].Table.Base = GDT_ADDR;
    vals[i].Table.Limit = 3 * 8 - 1;  /* 3 GDT entries */
    i++;

    /* IDTR: null — kernel will set up its own IDT */
    vals[i].Table.Base = 0;
    vals[i].Table.Limit = 0xFFFF;
    i++;

    /* CR0: Protected mode + paging */
    vals[i++].Reg64 = CR0_PE | CR0_ET | CR0_NE | CR0_WP | CR0_PG;
    /* CR3: PML4 page table */
    vals[i++].Reg64 = PML4_ADDR;
    /* CR4: PAE + SSE support */
    vals[i++].Reg64 = CR4_PAE | CR4_OSFXSR | CR4_OSXMMEXCPT;
    /* EFER: Long mode enabled + active */
    vals[i++].Reg64 = EFER_SCE | EFER_LME | EFER_LMA | EFER_NXE;

    HRESULT hr = st->api.SetVPRegs(st->partition, 0, names, (UINT32)i, vals);
    if (FAILED(hr)) {
        whpx_set_error(st, "Failed to set vCPU registers: 0x%08lX",
                       (unsigned long)hr);
        return 0;
    }
    return 1;
}

/* ============================================================
 * I/O port handling
 * ============================================================ */
static void whpx_handle_io(whpx_state_t *st,
                           const WHV_RUN_VP_EXIT_CONTEXT *ctx) {
    uint16_t port = ctx->IoPortAccess.PortNumber;
    uint8_t  is_write = (uint8_t)ctx->IoPortAccess.AccessInfo.IsWrite;
    uint8_t  size = (uint8_t)ctx->IoPortAccess.AccessInfo.AccessSize;
    uint64_t rax = ctx->IoPortAccess.Rax;
    uint64_t new_rax = rax;

    if (port >= COM1_BASE && port < COM1_BASE + 8) {
        /* Serial port */
        EnterCriticalSection(&st->serial_lock);
        if (is_write) {
            serial_write(&st->serial, port - COM1_BASE, (uint8_t)(rax & 0xFF));
            /* Signal that output is available */
            if (port == COM1_BASE && !(st->serial.lcr & 0x80))
                SetEvent(st->output_event);
        } else {
            new_rax = serial_read(&st->serial, port - COM1_BASE);
        }
        LeaveCriticalSection(&st->serial_lock);
    } else if (port == 0x20 || port == 0x21) {
        /* PIC master */
        if (is_write) {
            if (port == 0x20) {
                if (rax & 0x10) st->pic_master_icw_step = 1; /* ICW1 */
                /* OCW2 (EOI) or OCW3 — just acknowledge */
            } else {
                switch (st->pic_master_icw_step) {
                case 1: st->pic_master_vector_base = (uint8_t)rax;
                        st->pic_master_icw_step = 2; break;
                case 2: st->pic_master_icw_step = 3; break;
                case 3: st->pic_master_icw_step = 0; break;
                default: st->pic_master_imr = (uint8_t)rax; break;
                }
            }
        } else {
            new_rax = (port == 0x21) ? st->pic_master_imr : 0;
        }
    } else if (port == 0xA0 || port == 0xA1) {
        /* PIC slave */
        if (is_write) {
            if (port == 0xA0) {
                if (rax & 0x10) st->pic_slave_icw_step = 1;
            } else {
                switch (st->pic_slave_icw_step) {
                case 1: st->pic_slave_vector_base = (uint8_t)rax;
                        st->pic_slave_icw_step = 2; break;
                case 2: st->pic_slave_icw_step = 3; break;
                case 3: st->pic_slave_icw_step = 0; break;
                default: st->pic_slave_imr = (uint8_t)rax; break;
                }
            }
        } else {
            new_rax = (port == 0xA1) ? st->pic_slave_imr : 0;
        }
    } else if (port >= 0x40 && port <= 0x43) {
        /* PIT 8254 — acknowledge writes, return 0 for reads */
        if (!is_write) new_rax = 0;
    } else if (port == 0x70 || port == 0x71) {
        /* CMOS/RTC */
        if (is_write && port == 0x70) {
            st->cmos_index = (uint8_t)(rax & 0x7F);
        } else if (!is_write && port == 0x71) {
            /* Return basic time values */
            switch (st->cmos_index) {
            case 0x00: new_rax = 0;    break; /* seconds */
            case 0x02: new_rax = 0;    break; /* minutes */
            case 0x04: new_rax = 0x12; break; /* hours */
            case 0x06: new_rax = 0x04; break; /* day of week */
            case 0x07: new_rax = 0x01; break; /* day of month */
            case 0x08: new_rax = 0x01; break; /* month */
            case 0x09: new_rax = 0x24; break; /* year */
            case 0x0A: new_rax = 0x26; break; /* status A */
            case 0x0B: new_rax = 0x02; break; /* status B: 24hr */
            case 0x0C: new_rax = 0;    break; /* status C */
            case 0x0D: new_rax = 0x80; break; /* status D: valid */
            case 0x0F: new_rax = 0;    break; /* shutdown status */
            default:   new_rax = 0;    break;
            }
        }
    } else if (port == 0x61) {
        /* System control port B — toggle bits 4+5 on reads.
         * Bit 4 = refresh request, Bit 5 = timer 2 output.
         * The kernel uses these for delay calibration loops. */
        if (!is_write) {
            static uint8_t port61_val = 0;
            port61_val ^= 0x30;  /* Toggle both timer bits */
            new_rax = port61_val;
        }
    } else if (port == 0x64) {
        /* Keyboard controller status — return empty buffer */
        if (!is_write) new_rax = 0;
    } else if (port == 0x80) {
        /* Debug port — ignore */
    } else if (port == 0x92) {
        /* Fast A20 gate — ignore */
    } else if (port == 0xCF8) {
        /* PCI config address register — return 0 on reads to make
         * the kernel think PCI type 1 is not available. This skips
         * the entire PCI bus scan (we have no PCI devices). */
        if (is_write) st->pci_addr = (uint32_t)(rax & 0xFFFFFFFF);
        else new_rax = 0;
    } else if (port >= 0xCFC && port <= 0xCFF) {
        /* PCI config data — no devices */
        if (!is_write) new_rax = 0xFFFFFFFF;
    } else if (port == 0x3D4 || port == 0x3D5 ||
               port == 0x3C0 || port == 0x3C4 || port == 0x3C5 ||
               port == 0x3CE || port == 0x3CF) {
        /* VGA registers — ignore */
        if (!is_write) new_rax = 0;
    }

    /* Update RAX for reads and advance RIP past the I/O instruction */
    {
        WHV_REGISTER_NAME names[2] = { WHvX64RegisterRip, WHvX64RegisterRax };
        WHV_REGISTER_VALUE vals[2];
        memset(vals, 0, sizeof(vals));
        int nregs = 0;

        /* Always advance RIP */
        vals[0].Reg64 = ctx->VpContext.Rip + ctx->VpContext.InstructionLength;
        nregs = 1;

        /* Update RAX for reads */
        if (!is_write) {
            switch (size) {
            case 1: vals[1].Reg64 = (rax & ~0xFFULL) | (new_rax & 0xFF); break;
            case 2: vals[1].Reg64 = (rax & ~0xFFFFULL) | (new_rax & 0xFFFF); break;
            case 4: vals[1].Reg64 = new_rax & 0xFFFFFFFF; break;
            default: vals[1].Reg64 = new_rax; break;
            }
            nregs = 2;
        }
        st->api.SetVPRegs(st->partition, 0, names, (UINT32)nregs, vals);
    }
}

/* ============================================================
 * CPUID handler — return reasonable defaults
 * ============================================================ */
static void whpx_handle_cpuid(whpx_state_t *st,
                              const WHV_RUN_VP_EXIT_CONTEXT *ctx) {
    uint64_t rax = ctx->CpuidAccess.DefaultResultRax;
    uint64_t rbx = ctx->CpuidAccess.DefaultResultRbx;
    uint64_t rcx = ctx->CpuidAccess.DefaultResultRcx;
    uint64_t rdx = ctx->CpuidAccess.DefaultResultRdx;

    uint64_t leaf = ctx->CpuidAccess.Rax;
    if (leaf == 1) {
        rcx &= ~(1ULL << 21);  /* Clear x2APIC (ECX.21) */
        rcx &= ~(1ULL << 24);  /* Clear TSC-Deadline (ECX.24) */
    } else if (leaf == 0xB || leaf == 0x1F) {
        rax = 0; rbx = 1; rcx = 0; rdx = 0;
    }

    WHV_REGISTER_NAME names[] = {
        WHvX64RegisterRip, WHvX64RegisterRax, WHvX64RegisterRbx,
        WHvX64RegisterRcx, WHvX64RegisterRdx
    };
    WHV_REGISTER_VALUE vals[5];
    memset(vals, 0, sizeof(vals));
    vals[0].Reg64 = ctx->VpContext.Rip + ctx->VpContext.InstructionLength;
    vals[1].Reg64 = rax;
    vals[2].Reg64 = rbx;
    vals[3].Reg64 = rcx;
    vals[4].Reg64 = rdx;
    st->api.SetVPRegs(st->partition, 0, names, 5, vals);
}

/* ============================================================
 * MSR handler
 * ============================================================ */
static void whpx_handle_msr(whpx_state_t *st,
                            const WHV_RUN_VP_EXIT_CONTEXT *ctx) {
    (void)st;

    WHV_REGISTER_NAME names[3] = { WHvX64RegisterRip,
                                    WHvX64RegisterRax, WHvX64RegisterRdx };
    WHV_REGISTER_VALUE vals[3];
    memset(vals, 0, sizeof(vals));
    vals[0].Reg64 = ctx->VpContext.Rip + ctx->VpContext.InstructionLength;

    if (ctx->MsrAccess.AccessInfo.IsWrite) {
        st->api.SetVPRegs(st->partition, 0, names, 1, vals);
    } else {
        /* Handle known MSRs */
        uint32_t msr_num = ctx->MsrAccess.MsrNumber;
        switch (msr_num) {
        case 0x1B: /* IA32_APIC_BASE */
            /* Return standard APIC base: enabled, BSP, at 0xFEE00000 */
            vals[1].Reg64 = 0xFEE00900;  /* base | BSP | Enable */
            break;
        case 0xFE: /* IA32_MTRRCAP */
            vals[1].Reg64 = 0;  /* No MTRRs */
            break;
        default:
            /* Return 0 for unknown MSRs */
            break;
        }
        st->api.SetVPRegs(st->partition, 0, names, 3, vals);
    }
}

/* ============================================================
 * VM thread — runs the vCPU in a loop
 * ============================================================ */
static DWORD WINAPI whpx_vm_thread(LPVOID param) {
    whpx_state_t *st = (whpx_state_t *)param;
    WHV_RUN_VP_EXIT_CONTEXT exit_ctx;
    int hlt_count = 0;
    /* Diagnostic counters */
    uint64_t exit_io = 0, exit_mmio = 0, exit_cpuid = 0;
    uint64_t exit_msr = 0, exit_hlt = 0, exit_other = 0;
    uint64_t total_exits = 0;
    DWORD last_diag = GetTickCount();

    while (!InterlockedCompareExchange(&st->vm_stop, 0, 0)) {
        HRESULT hr = st->api.RunVP(st->partition, 0,
                                    &exit_ctx, sizeof(exit_ctx));
        if (FAILED(hr)) {
            whpx_set_error(st, "WHvRunVirtualProcessor failed: 0x%08lX",
                           (unsigned long)hr);
            break;
        }

        total_exits++;
        /* Periodic diagnostic dump */
        if (st->config->verbose && (total_exits & 0xFFFFF) == 0) {
            DWORD now = GetTickCount();
            if (now - last_diag > 2000) {
                /* Sample current RIP and last I/O port */
                uint16_t last_port = 0;
                if (exit_ctx.ExitReason == WHvRunVpExitReasonX64IoPortAccess)
                    last_port = exit_ctx.IoPortAccess.PortNumber;
                EnterCriticalSection(&st->serial_lock);
                int tx_len = (st->serial.tx_head - st->serial.tx_tail
                              + SERIAL_BUF_SIZE) % SERIAL_BUF_SIZE;
                LeaveCriticalSection(&st->serial_lock);
                LINUX_LOG(st->config,
                    "WHPX diag: exits=%llu io=%llu mmio=%llu msr=%llu "
                    "hlt=%llu serial_tx=%d RIP=0x%llX port=0x%X",
                    total_exits, exit_io, exit_mmio,
                    exit_msr, exit_hlt, tx_len,
                    (unsigned long long)exit_ctx.VpContext.Rip,
                    (unsigned)last_port);
                last_diag = now;
            }
        }

        switch (exit_ctx.ExitReason) {
        case WHvRunVpExitReasonX64IoPortAccess:
            exit_io++;
            whpx_handle_io(st, &exit_ctx);
            hlt_count = 0;
            lapic_check_timer(&st->lapic);
            break;

        case WHvRunVpExitReasonX64Cpuid:
            exit_cpuid++;
            whpx_handle_cpuid(st, &exit_ctx);
            break;

        case WHvRunVpExitReasonX64MsrAccess:
            exit_msr++;
            whpx_handle_msr(st, &exit_ctx);
            break;

        case WHvRunVpExitReasonX64Halt:
            exit_hlt++;
            hlt_count++;
            {
                /* Check LAPIC timer */
                int lapic_vec = lapic_check_timer(&st->lapic);
                /* Also check PIC timer */
                uint8_t pic_vec = st->pic_master_vector_base;
                if (pic_vec == 0) pic_vec = 0x20;

                int vec = lapic_vec ? lapic_vec : pic_vec;

                /* Inject interrupt to wake CPU */
                WHV_REGISTER_NAME name = WHvRegisterPendingInterruption;
                WHV_REGISTER_VALUE val;
                memset(&val, 0, sizeof(val));
                val.PendingInterruption.InterruptionPending = 1;
                val.PendingInterruption.InterruptionType =
                    WHvX64PendingInterrupt;
                val.PendingInterruption.InterruptionVector = (UINT8)vec;
                st->api.SetVPRegs(st->partition, 0, &name, 1, &val);

                /* Yield when idle */
                if (hlt_count > 50) {
                    EnterCriticalSection(&st->serial_lock);
                    int has_input = serial_rx_available(&st->serial);
                    LeaveCriticalSection(&st->serial_lock);
                    if (!has_input) Sleep(1);
                    hlt_count = 20;
                }
            }
            break;

        case WHvRunVpExitReasonMemoryAccess:
            exit_mmio++;
            /* MMIO — dispatch to LAPIC or return 0 for unknown regions.
             * CRITICAL: must advance RIP or the vCPU loops forever. */
            {
                uint64_t gpa = exit_ctx.MemoryAccess.Gpa;
                int is_write = (exit_ctx.MemoryAccess.AccessInfo.AccessType
                                == WHvMemoryAccessWrite);
                int insn_len = exit_ctx.VpContext.InstructionLength;
                const uint8_t *insn = exit_ctx.MemoryAccess.InstructionBytes;
                int insn_byte_count = exit_ctx.MemoryAccess.InstructionByteCount;

                if (gpa >= LAPIC_BASE && gpa < LAPIC_BASE + LAPIC_SIZE) {
                    /* LAPIC access */
                    uint32_t offset = (uint32_t)(gpa - LAPIC_BASE);
                    if (is_write) {
                        /* Decode instruction to find source value */
                        int is_imm = 0;
                        uint32_t imm_val = 0;
                        int reg = decode_mmio_reg(insn, insn_byte_count,
                                                  &is_imm, &imm_val);
                        uint32_t value = 0;
                        if (is_imm) {
                            value = imm_val;
                        } else if (reg >= 0 && reg < 16) {
                            WHV_REGISTER_VALUE rv;
                            st->api.GetVPRegs(st->partition, 0,
                                &gp_reg_names[reg], 1, &rv);
                            value = (uint32_t)rv.Reg64;
                        }
                        lapic_write(&st->lapic, offset, value);
                    } else {
                        /* Read LAPIC register, write to destination reg */
                        uint32_t value = lapic_read(&st->lapic, offset);
                        int is_imm = 0;
                        uint32_t dummy;
                        int reg = decode_mmio_reg(insn, insn_byte_count,
                                                  &is_imm, &dummy);
                        if (reg >= 0 && reg < 16) {
                            WHV_REGISTER_VALUE rv;
                            memset(&rv, 0, sizeof(rv));
                            rv.Reg64 = value;
                            st->api.SetVPRegs(st->partition, 0,
                                &gp_reg_names[reg], 1, &rv);
                        }
                    }
                } else if (!is_write) {
                    /* Unknown MMIO read — return 0 or 0xFFFFFFFF */
                    int is_imm = 0;
                    uint32_t dummy;
                    int reg = decode_mmio_reg(insn, insn_byte_count,
                                              &is_imm, &dummy);
                    if (reg >= 0 && reg < 16) {
                        WHV_REGISTER_VALUE rv;
                        memset(&rv, 0, sizeof(rv));
                        /* Return 0 for most, 0xFFFFFFFF for PCI-like probes */
                        rv.Reg64 = 0;
                        st->api.SetVPRegs(st->partition, 0,
                            &gp_reg_names[reg], 1, &rv);
                    }
                }

                /* Advance RIP past the instruction.
                 * InstructionLength may be 0 for MMIO exits on some
                 * WHPX versions — fall back to InstructionByteCount. */
                int advance = insn_len;
                if (advance <= 0) advance = insn_byte_count;
                if (advance > 0) {
                    WHV_REGISTER_NAME rip_name = WHvX64RegisterRip;
                    WHV_REGISTER_VALUE rip_val;
                    rip_val.Reg64 = exit_ctx.VpContext.Rip + advance;
                    st->api.SetVPRegs(st->partition, 0,
                                       &rip_name, 1, &rip_val);
                }
                hlt_count = 0;
            }
            break;

        case WHvRunVpExitReasonCanceled:
            /* Cancelled from another thread (stop requested) */
            goto done;

        case WHvRunVpExitReasonNone:
            break;

        default:
            exit_other++;
            LINUX_LOG(st->config, "WHPX: unhandled exit reason %d RIP=0x%llX",
                      (int)exit_ctx.ExitReason,
                      (unsigned long long)exit_ctx.VpContext.Rip);
            Sleep(1);
            break;
        }

        /* Check boot status — flag as booted when we have serial output */
        if (!InterlockedCompareExchange(&st->booted, 0, 0) &&
            (total_exits & 0xFFF) == 0) {
            EnterCriticalSection(&st->serial_lock);
            int out_len = (st->serial.tx_head - st->serial.tx_tail
                           + SERIAL_BUF_SIZE) % SERIAL_BUF_SIZE;
            LeaveCriticalSection(&st->serial_lock);
            if (out_len > 100)
                InterlockedExchange(&st->booted, 1);
        }
    }

done:
    return 0;
}

/* ============================================================
 * Backend interface: start
 * ============================================================ */
static linux_error_t whpx_start(linux_backend_t *self,
                                const linux_config_t *config) {
    whpx_state_t *st = (whpx_state_t *)self->opaque;
    if (st->running) return LINUX_ERR_ALREADY_RUNNING;
    st->config = config;

    LINUX_LOG(config, "WHPX: starting backend");

    /* Check for required files */
    const char *kernel_path = config->kernel_path;
    const char *rootfs_path = config->rootfs_path;

    if (!kernel_path) {
        /* Look for bundled kernel */
        static char kpath[MAX_PATH];
        char exe_dir[MAX_PATH];
        GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
        char *s = strrchr(exe_dir, '\\');
        if (s) *s = '\0';
        snprintf(kpath, sizeof(kpath), "%s\\linux\\bzImage", exe_dir);
        if (GetFileAttributesA(kpath) != INVALID_FILE_ATTRIBUTES)
            kernel_path = kpath;
    }
    if (!kernel_path) {
        whpx_set_error(st, "No Linux kernel found. Provide --kernel path/to/bzImage "
                       "or place bzImage in the linux/ subdirectory.");
        return LINUX_ERR_START_FAILED;
    }

    if (!rootfs_path) {
        static char rpath[MAX_PATH];
        char exe_dir[MAX_PATH];
        GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
        char *s = strrchr(exe_dir, '\\');
        if (s) *s = '\0';
        snprintf(rpath, sizeof(rpath), "%s\\linux\\initramfs.cpio.gz", exe_dir);
        if (GetFileAttributesA(rpath) != INVALID_FILE_ATTRIBUTES)
            rootfs_path = rpath;
    }

    LINUX_LOG(config, "WHPX: kernel=%s rootfs=%s",
              kernel_path, rootfs_path ? rootfs_path : "(none)");

    /* Load WHPX API */
    if (!whpx_load_api(st)) return LINUX_ERR_NOT_AVAILABLE;
    if (!whpx_check_hypervisor(st)) return LINUX_ERR_NOT_AVAILABLE;

    /* Create partition */
    HRESULT hr = st->api.CreatePartition(&st->partition);
    if (FAILED(hr)) {
        whpx_set_error(st, "WHvCreatePartition failed: 0x%08lX",
                       (unsigned long)hr);
        return LINUX_ERR_START_FAILED;
    }
    st->partition_created = 1;

    /* Set 1 vCPU */
    WHV_PARTITION_PROPERTY prop;
    memset(&prop, 0, sizeof(prop));
    prop.ProcessorCount = 1;
    hr = st->api.SetPartitionProperty(st->partition,
        WHvPartitionPropertyCodeProcessorCount, &prop, sizeof(prop));
    if (FAILED(hr)) {
        whpx_set_error(st, "Set processor count failed: 0x%08lX",
                       (unsigned long)hr);
        return LINUX_ERR_START_FAILED;
    }

    hr = st->api.SetupPartition(st->partition);
    if (FAILED(hr)) {
        whpx_set_error(st, "WHvSetupPartition failed: 0x%08lX",
                       (unsigned long)hr);
        return LINUX_ERR_START_FAILED;
    }

    /* Allocate and map guest memory */
    st->guest_mem_size = GUEST_MEM_DEFAULT;
    st->guest_mem = (uint8_t *)VirtualAlloc(NULL, st->guest_mem_size,
                                             MEM_COMMIT | MEM_RESERVE,
                                             PAGE_READWRITE);
    if (!st->guest_mem) {
        whpx_set_error(st, "Failed to allocate %llu MB guest memory",
                       (unsigned long long)(st->guest_mem_size / (1024*1024)));
        return LINUX_ERR_OUT_OF_MEMORY;
    }
    memset(st->guest_mem, 0, st->guest_mem_size);

    hr = st->api.MapGpaRange(st->partition, st->guest_mem, 0,
                              st->guest_mem_size,
                              WHvMapGpaRangeFlagRead |
                              WHvMapGpaRangeFlagWrite |
                              WHvMapGpaRangeFlagExecute);
    if (FAILED(hr)) {
        whpx_set_error(st, "WHvMapGpaRange failed: 0x%08lX",
                       (unsigned long)hr);
        return LINUX_ERR_START_FAILED;
    }

    /* Set up page tables and GDT */
    whpx_setup_tables(st);

    /* Load kernel + initramfs */
    if (!whpx_load_kernel(st, kernel_path, rootfs_path))
        return LINUX_ERR_START_FAILED;

    /* Create vCPU */
    hr = st->api.CreateVP(st->partition, 0, 0);
    if (FAILED(hr)) {
        whpx_set_error(st, "WHvCreateVirtualProcessor failed: 0x%08lX",
                       (unsigned long)hr);
        return LINUX_ERR_START_FAILED;
    }
    st->vp_created = 1;

    /* Set initial register state */
    if (!whpx_setup_vcpu(st))
        return LINUX_ERR_START_FAILED;

    /* Initialize devices */
    serial_init(&st->serial);
    lapic_init(&st->lapic);
    InitializeCriticalSection(&st->serial_lock);
    st->output_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    /* Launch VM thread */
    InterlockedExchange(&st->vm_stop, 0);
    InterlockedExchange(&st->booted, 0);
    st->vm_thread = CreateThread(NULL, 0, whpx_vm_thread, st, 0, NULL);
    if (!st->vm_thread) {
        whpx_set_error(st, "Failed to create VM thread");
        return LINUX_ERR_START_FAILED;
    }

    /* Wait for boot (serial output + HLT pattern indicates shell ready) */
    LINUX_LOG(config, "WHPX: waiting for Linux to boot...");
    DWORD timeout = config->timeout_ms ? config->timeout_ms : 30000;
    DWORD start_time = GetTickCount();
    while ((GetTickCount() - start_time) < timeout) {
        if (InterlockedCompareExchange(&st->booted, 0, 0))
            break;
        Sleep(100);
    }

    if (!InterlockedCompareExchange(&st->booted, 0, 0))
        LINUX_LOG(config, "WHPX: boot timeout — VM may still be starting");

    /* Dump boot output for debugging */
    if (config->verbose) {
        /* Wait a bit more for boot to complete after first output */
        Sleep(3000);
        EnterCriticalSection(&st->serial_lock);
        int total_tx = (st->serial.tx_head - st->serial.tx_tail
                        + SERIAL_BUF_SIZE) % SERIAL_BUF_SIZE;
        char *boot_buf = (char *)malloc(total_tx + 1);
        int n = 0;
        if (boot_buf)
            n = serial_pop_tx(&st->serial, boot_buf, total_tx);
        LeaveCriticalSection(&st->serial_lock);
        if (n > 0 && boot_buf) {
            boot_buf[n] = '\0';
            /* Print last 4000 chars to see end of boot */
            const char *start = boot_buf;
            if (n > 4000) start = boot_buf + n - 4000;
            LINUX_LOG(config, "WHPX: boot output (%d bytes, showing tail):\n%s",
                      n, start);
        } else {
            LINUX_LOG(config, "WHPX: no serial output captured");
        }
        free(boot_buf);
    }

    st->running = 1;
    LINUX_LOG(config, "WHPX: backend started");
    return LINUX_OK;
}

/* ============================================================
 * Backend interface: exec
 * ============================================================ */
static linux_error_t whpx_exec(linux_backend_t *self, const char *command,
                               char **stdout_buf, char **stderr_buf,
                               int *exit_code) {
    whpx_state_t *st = (whpx_state_t *)self->opaque;
    if (!st->running) return LINUX_ERR_NOT_RUNNING;

    if (stdout_buf) *stdout_buf = NULL;
    if (stderr_buf) *stderr_buf = NULL;
    if (exit_code)  *exit_code = -1;

    LINUX_LOG(st->config, "WHPX exec: %s", command);

    /* Build command with exit-code marker:
     * <cmd> 2>&1; echo __EXIT:$?__\n */
    size_t cmd_len = strlen(command) + 64;
    char *full_cmd = (char *)malloc(cmd_len);
    if (!full_cmd) return LINUX_ERR_OUT_OF_MEMORY;
    snprintf(full_cmd, cmd_len, "%s 2>&1; echo __EXIT:$?__\n", command);

    /* Drain any pending output */
    EnterCriticalSection(&st->serial_lock);
    st->serial.tx_head = st->serial.tx_tail = 0;

    /* Inject command into serial input */
    serial_push_rx(&st->serial, full_cmd, (int)strlen(full_cmd));
    /* Signal serial interrupt if data ready */
    st->serial.lsr |= 0x01;
    LeaveCriticalSection(&st->serial_lock);
    free(full_cmd);

    /* Read output until we see __EXIT:N__ marker */
    growbuf_t output;
    growbuf_init(&output, 4096);

    DWORD timeout = st->config->timeout_ms ? st->config->timeout_ms : 30000;
    DWORD start_time = GetTickCount();
    int found_marker = 0;

    while ((GetTickCount() - start_time) < timeout && !found_marker) {
        WaitForSingleObject(st->output_event, 200);

        char buf[4096];
        EnterCriticalSection(&st->serial_lock);
        int n = serial_pop_tx(&st->serial, buf, sizeof(buf));
        LeaveCriticalSection(&st->serial_lock);

        if (n > 0) {
            growbuf_append(&output, buf, (size_t)n);

            /* Check for exit marker */
            if (output.len > 12) {
                char *marker = strstr(output.data, "__EXIT:");
                if (marker) {
                    int code = atoi(marker + 7);
                    if (exit_code) *exit_code = code;
                    *marker = '\0';
                    output.len = (size_t)(marker - output.data);
                    found_marker = 1;
                }
            }
        }
    }

    if (!found_marker && exit_code)
        *exit_code = -1;

    /* Strip the echoed command from the beginning of output */
    char *result = growbuf_finish(&output);
    if (result) {
        /* Find first newline (end of echoed command) */
        char *nl = strchr(result, '\n');
        if (nl && stdout_buf) {
            *stdout_buf = strdup(nl + 1);
            free(result);
        } else if (stdout_buf) {
            *stdout_buf = result;
        } else {
            free(result);
        }
    }

    return found_marker ? LINUX_OK : LINUX_ERR_TIMEOUT;
}

/* ============================================================
 * Backend interface: stop, destroy, etc.
 * ============================================================ */
static linux_error_t whpx_stop(linux_backend_t *self) {
    whpx_state_t *st = (whpx_state_t *)self->opaque;

    InterlockedExchange(&st->vm_stop, 1);
    if (st->vm_thread) {
        st->api.CancelRunVP(st->partition, 0, 0);
        WaitForSingleObject(st->vm_thread, 5000);
        CloseHandle(st->vm_thread);
        st->vm_thread = NULL;
    }
    st->running = 0;
    return LINUX_OK;
}

static int whpx_is_running(linux_backend_t *self) {
    whpx_state_t *st = (whpx_state_t *)self->opaque;
    return st->running;
}

static const char *whpx_last_error(linux_backend_t *self) {
    whpx_state_t *st = (whpx_state_t *)self->opaque;
    return st->last_error_buf;
}

static void whpx_destroy(linux_backend_t *self) {
    if (!self) return;
    whpx_state_t *st = (whpx_state_t *)self->opaque;
    if (st) {
        if (st->running) whpx_stop(self);

        if (st->vp_created)
            st->api.DeleteVP(st->partition, 0);
        if (st->guest_mem) {
            if (st->partition_created)
                st->api.UnmapGpaRange(st->partition, 0, st->guest_mem_size);
            VirtualFree(st->guest_mem, 0, MEM_RELEASE);
        }
        if (st->partition_created)
            st->api.DeletePartition(st->partition);
        if (st->output_event)
            CloseHandle(st->output_event);
        DeleteCriticalSection(&st->serial_lock);
        if (st->hmod)
            FreeLibrary(st->hmod);
        free(st);
    }
    free(self);
}

/* ============================================================
 * Constructor
 * ============================================================ */
linux_backend_t *linux_backend_create_whpx(void) {
    /* Quick probe: can we load the DLL and is hypervisor present? */
    HMODULE h = LoadLibraryExW(L"WinHvPlatform.dll", NULL,
                                LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) return NULL;

    PFN_WHvGetCapability pGetCap = (PFN_WHvGetCapability)
        GetProcAddress(h, "WHvGetCapability");
    if (!pGetCap) { FreeLibrary(h); return NULL; }

    WHV_CAPABILITY cap;
    UINT32 written;
    HRESULT hr = pGetCap(WHvCapabilityCodeHypervisorPresent,
                          &cap, sizeof(cap), &written);
    if (FAILED(hr) || !cap.HypervisorPresent) { FreeLibrary(h); return NULL; }
    FreeLibrary(h);

    /* Allocate backend */
    linux_backend_t *b = (linux_backend_t *)calloc(1, sizeof(linux_backend_t));
    if (!b) return NULL;

    whpx_state_t *st = (whpx_state_t *)calloc(1, sizeof(whpx_state_t));
    if (!st) { free(b); return NULL; }

    b->type       = LINUX_BACKEND_WHPX;
    b->name       = "WHPX";
    b->opaque     = st;
    b->start      = whpx_start;
    b->stop       = whpx_stop;
    b->is_running = whpx_is_running;
    b->destroy    = whpx_destroy;
    b->exec       = whpx_exec;
    b->last_error = whpx_last_error;

    return b;
}

#endif /* _WIN32 */
