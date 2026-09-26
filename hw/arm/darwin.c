#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "cpu.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "exec/memattrs.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "cpregs.h"
#include "hw/arm/exynos4210.h"
#include "system/system.h"
#include "hw/core/sysbus.h"
#include "hw/core/platform-bus.h"
#include "xnu/boot/xnuboot.h"
#include "xnu/apple_dtree.h"
#include "xnu/apple_regs.h"
#include "hw/arm/apple_amcc.h"
#include "xnu/patch.h"
#include "xnu/macho.h"
#include "hw/intc/apple_aic.h"

// See device tree specification section 2.3.8: ranges
#define IO_RANGE_BASE_OFFSET     1

// Apple hardcodes this in the Samsung serial driver kext
#define EXPECTED_UART_FIFO_DEPTH 0x10

#define EXPECTED_FIRMWARE_NAME   "qemu-sptm"

static char *g_default_args = (char*)"debug=0x8 kextlog=0xffff cpus=1 rd=md0 serial=7 -v -noprogress keepsyms=1 wdt=-1 -enable_kprintf_spam wlan-olyhal-abort";

#define CPACR_ENABLE_FPU         (( BIT(20) | BIT(21) ))
#define CPTR_ENABLE_FPU          (( BIT(20) | BIT(21) ))

#define TYPE_DARWIN_MACHINE MACHINE_TYPE_NAME("darwin")
OBJECT_DECLARE_SIMPLE_TYPE(DarwinState, DARWIN_MACHINE)

#define IRQ_QEMUPORT_(d)            ((d))
#define TUNNEL_UART_NAME            "qemuport"
#define NUM_TUNNEL_DEVICES          4

#define DARWIN_MACHINE_NUM_IRQS     NUM_TUNNEL_DEVICES

struct DarwinState {
    MachineState           parent_obj;
    ARMCPU                *cpu;
    AMCCState             *amcc;
    DeviceState           *aic;
    struct xnu_boot_info   bootinfo;
    qemu_irq               irqs[DARWIN_MACHINE_NUM_IRQS];
};

MACHINE_CLASS_ARG(bootkc);
MACHINE_CLASS_ARG(args);
MACHINE_CLASS_ARG(dtree);
MACHINE_CLASS_ARG(sptm);
MACHINE_CLASS_ARG(txm);
MACHINE_CLASS_ARG(tc);
MACHINE_CLASS_ARG(ramdisk);

static void alloc_zeroed(const char *name, hwaddr pa, size_t len) {
    if (0 == len) {
        fprintf(stderr, "error: alloc_ram called with len=0 for region %s\n", name);
        exit(1);
    }
    MemoryRegion *ram_main = get_system_memory();
    MemoryRegion *ram_subreg = g_new(MemoryRegion, 1);
    memory_region_init_ram(ram_subreg, NULL, name, len, &error_fatal);
    memory_region_add_subregion(ram_main, pa, ram_subreg);
    address_space_set(&address_space_memory, pa, 0, len, MEMTXATTRS_UNSPECIFIED);
}

static void do_darwin_reset(void *state) {
    DarwinState *s = DARWIN_MACHINE((MachineState *)state);
    ARMCPU *cpu = s->cpu;
    CPUState *cs = CPU(cpu);
    CPUARMState *env = &cpu->env;

    cpu_reset(cs);

    assert(2 == arm_highest_el(env));
    env->xregs[0] = s->bootinfo.init_x0;
    env->pc = s->bootinfo.init_pc;

    // SPTM needs fpu enabled:
    env->cp15.cpacr_el1 |= CPACR_ENABLE_FPU;
    env->cp15.cptr_el[2] |= CPTR_ENABLE_FPU;
    arm_rebuild_hflags(env);
}

static mmap_file_t check_and_open(const char *path, const char *errmsg) {
    int fd;
    struct stat stats;
    void *mapping;

    if (!path) goto fail;

    fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) goto fail;

    fstat(fd, &stats);
    mapping = mmap(NULL, stats.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (MAP_FAILED == mapping) goto fail;

    if (is_im4p(mapping)) {
        fprintf(stderr, "error: %s is an im4p, you need to unwrap it (eg. ipsw img4 im4p extract)\n", path);
        goto fail;
    }

    return (mmap_file_t){
        .buf = mapping,
        .len = stats.st_size,
    };

fail:
    fprintf(stderr, "%s\n", errmsg);
    exit(1);
}

static void init_cpu_impl(struct dtree_node *dt_root) {
    struct dtree_node *cpu0 = adt_find_node(dt_root, "cpus/cpu0");
    struct adt_io_reg *cpu_impl = adt_get_prop_val(cpu0, "cpu-impl-reg");
    struct adt_io_reg *cpm_impl = adt_get_prop_val(cpu0, "cpm-impl-reg");
    alloc_zeroed("cpu_reg_impl", cpu_impl[0].base, cpu_impl[0].len);
    alloc_zeroed("cpm_reg_impl", cpm_impl[0].base, cpm_impl[0].len);
}

static u32 armio_get_irqnum(struct dtree_node *node) {
    u32 *irqnum = adt_get_prop_val(node, "interrupts");
    if (!irqnum) {
        fprintf(stderr, "missing interrupts property for device\n");
        exit(1);
    }
    return irqnum[0];
}

static void init_uart(struct dtree_node *uart, uint64_t iobase, Chardev *cdev, qemu_irq irq) {
    struct adt_io_reg *uart_reg = adt_get_prop_val(uart, "reg");
    exynos4210_uart_create(uart_reg[0].base + iobase, EXPECTED_UART_FIFO_DEPTH, 0, cdev, irq);
}

static void init_sep(struct dtree_node *dt_root) {
    // This is a bare-bones sep InvalidateHmac config=1 implementation that ignores all register reads/ writes.
    // Since we fixup the device tree to have sio-hmac1-disable-mask = -1, InvalidateHmac is effectively disabled,
    // so long as we can read/ write the "registers" in the reg-block.
    struct dtree_node *sep = adt_find_node(dt_root, "arm-io/sep/iop-sep-nub/InvalidateHmac");
    if (!sep) return;
    struct adt_io_reg *sep_reg = adt_get_prop_val(sep, "reg-block");

    alloc_zeroed("sep", sep_reg[0].base, sep_reg[0].len);
}

static void setup_mte(Object *cpuobj, MachineState *machine, struct xnu_boot_info *info) {
    MemoryRegion *tag_sysmem = NULL;

    if (!object_property_find(cpuobj, "tag-memory")) {
        fprintf(stderr, "cpu missing tag-memory property; MTE can't be enabled\n");
        exit(1);
    }

    tag_sysmem = g_new(MemoryRegion, 1);
    memory_region_init(tag_sysmem, OBJECT(machine), "tag-memory", UINT64_MAX / 32);
    object_property_set_link(cpuobj, "tag-memory", OBJECT(tag_sysmem), &error_abort);

    MemoryRegion *tagram = g_new(MemoryRegion, 1);
    memory_region_init_ram(tagram, NULL, "mte_tags", info->dram_size / 32, &error_fatal);
    memory_region_add_subregion(tag_sysmem, info->dram_base / 32, tagram);
}

__attribute__((unused))
static int get_soc_gen(struct dtree_node *dt_root) {
    struct dtree_node *arm_io = adt_find_node(dt_root, "arm-io");
    assert(arm_io);
    const char *soc_gen_str = adt_get_prop_val(arm_io, "soc-generation");
    if (!soc_gen_str || 'H' != soc_gen_str[0]) return 0;
    int soc_gen = 0;
    sscanf(soc_gen_str, "H%d", &soc_gen);
    return soc_gen;
}

static void check_dtree(struct dtree_node *dt_root) {
    struct dtree_node *chosen = adt_find_node(dt_root, "chosen");
    assert(chosen);

    const char *fw_vers = adt_get_prop_val(chosen, "firmware-version");
    assert(fw_vers);

    if (0 != strcmp(fw_vers, EXPECTED_FIRMWARE_NAME)) {
        fprintf(stderr, "error: device tree firmware-version doesn't match %s, did you run dt_fixup on this device tree?\n", EXPECTED_FIRMWARE_NAME);
        exit(1);
    }
}

static void darwin_init(MachineState *ms) {
    DarwinState *s = DARWIN_MACHINE(ms);
    struct xnu_boot_info *info = &s->bootinfo;
    struct dtree_node *dt_root;

    if (!info->args) info->args = g_default_args;
    info->bootkc_f = check_and_open(info->bootkc, "error opening XNU kernel");
    info->dtree_f = check_and_open(info->dtree, "error opening device tree");
    info->tc_f = check_and_open(info->tc, "error opening trust cache");
    info->ramdisk_f = check_and_open(info->ramdisk, "error opening ramdisk");
    if (info->sptm) {
        info->sptm_f = check_and_open(info->sptm, "error opening SPTM");
        info->txm_f = check_and_open(info->txm, "error opening TXM");
    }
    dt_root = info->dtree_f.buf;

    check_dtree(dt_root);

    info->has_mte = kc_uses_mte(info->bootkc_f.buf);

    Object *cpuobj = object_new(ms->cpu_type);
    DeviceState *cpudev = DEVICE(cpuobj);
    ARMCPU *cpu = ARM_CPU(cpuobj);

    cpu->has_el2 = true;
    cpu->has_el3 = false;

    // Disable CBAR because some MSRs conflict with Apple Si specific ones (see aarch64_a57_initfn)
    unset_feature(&cpu->env, ARM_FEATURE_CBAR);
    unset_feature(&cpu->env, ARM_FEATURE_CBAR_RO);
    unset_feature(&cpu->env, ARM_FEATURE_EL3);
    unset_feature(&cpu->env, ARM_FEATURE_PMU);

    // SPTM requires EL2, and non-SPTM XNU can run in either EL1 or EL2, so just always use EL2
    set_feature(&cpu->env, ARM_FEATURE_EL2);

    struct dtree_node *cpu_node = adt_find_node(dt_root, "cpus/cpu0");
    u32 dtree_frq = *(u32*)adt_get_prop_val(cpu_node, "clock-frequency");
    cpu->gt_cntfrq_hz = dtree_frq;

    struct dtree_node *chosen_node = adt_find_node(dt_root, "chosen");
    info->dram_base = *(u64*)adt_get_prop_val(chosen_node, "dram-base");
    info->dram_size = *(u64*)adt_get_prop_val(chosen_node, "dram-size");

    s->cpu = ARM_CPU(cpuobj);
    arm_load_xnu(s->cpu, ms, info);
    if (info->has_mte) setup_mte(cpuobj, MACHINE(s), info);

    struct dtree_node *amcc_node = adt_find_node(dt_root, "chosen/lock-regs/amcc");
    s->amcc = amcc_node ? (AMCCState*)amcc_create(amcc_node) : NULL;
    apple_regs_init(cpu, s->amcc, dt_root, info);

    struct dtree_node *arm_io = adt_find_node(dt_root, "arm-io");
    uint64_t *arm_io_ranges = adt_get_prop_val(arm_io, "ranges");
    uint64_t iobase = arm_io_ranges[IO_RANGE_BASE_OFFSET];
    assert(0 == arm_io_ranges[0]); // child bus phys addr must be zero

    s->aic = apple_aic_create(adt_find_node(dt_root, "arm-io/aic"), iobase);

    struct dtree_node *uart0 = adt_find_node(dt_root, "arm-io/uart0");
    if (!uart0) {
        fprintf(stderr, "error: no uart0 in device tree\n");
        exit(1);
    }
    init_uart(uart0, iobase, serial_hd(0), NULL);

    for (int i = 0; i < NUM_TUNNEL_DEVICES; i++) {
        char tunnel_devname[32];
        snprintf(tunnel_devname, sizeof(tunnel_devname), "arm-io/%s%d", TUNNEL_UART_NAME, i);
        struct dtree_node *port = adt_find_node(dt_root, tunnel_devname);
        if (port) {
            u32 irqnum = armio_get_irqnum(port);
            s->irqs[IRQ_QEMUPORT_(i)] = qdev_get_gpio_in(s->aic, irqnum);
            init_uart(port, iobase, serial_hd(1+i), s->irqs[IRQ_QEMUPORT_(i)]);
        }
    }

    init_sep(dt_root);
    init_cpu_impl(dt_root);

    // M4 (T8132) asserts SME's max VQ len is this many.
    // Specifically, rdsvl   x8, #0x1 must return 0x40.
    s->cpu->sme_vq.supported = 0x0b;

    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);

    // On Apple Si, FIQ is hardwired to platform timer
    // Hardwire IRQ to the AIC
    qdev_connect_gpio_out(cpudev, GTIMER_HYPVIRT, qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
    qdev_connect_gpio_out(s->aic, 0, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));

    qemu_register_reset(do_darwin_reset, s);
    munmap(info->bootkc_f.buf, info->bootkc_f.len);
    munmap(info->dtree_f.buf, info->dtree_f.len);
    munmap(info->tc_f.buf, info->tc_f.len);
    munmap(info->ramdisk_f.buf, info->ramdisk_f.len);
    if (info->sptm) {
        munmap(info->sptm_f.buf, info->sptm_f.len);
        munmap(info->txm_f.buf, info->txm_f.len);
    }
}

static void darwin_machine_init(MachineClass *mc) {
    mc->desc = "Generic Apple Silicon Device";
    mc->init = darwin_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("max");
}

static void darwin_machine_class_init(ObjectClass *oc, const void *data) {
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "bootkc", bootkc_darwin_class_get, bootkc_darwin_class_set);
    object_class_property_add_str(oc, "args", args_darwin_class_get, args_darwin_class_set);
    object_class_property_add_str(oc, "dtree", dtree_darwin_class_get, dtree_darwin_class_set);
    object_class_property_add_str(oc, "sptm", sptm_darwin_class_get, sptm_darwin_class_set);
    object_class_property_add_str(oc, "txm", txm_darwin_class_get, txm_darwin_class_set);
    object_class_property_add_str(oc, "tc", tc_darwin_class_get, tc_darwin_class_set);
    object_class_property_add_str(oc, "ramdisk", ramdisk_darwin_class_get, ramdisk_darwin_class_set);

    darwin_machine_init(mc);
}

static const TypeInfo darwin_machine_typeinfo = {
    .name           =  TYPE_DARWIN_MACHINE,
    .parent         =  TYPE_MACHINE,
    .class_init     =  darwin_machine_class_init,
    .instance_size  =  sizeof(DarwinState),
    .abstract       =  false,
    .interfaces     =  arm_machine_interfaces,
};

static void darwin_register_types(void) {
    type_register_static(&darwin_machine_typeinfo);
}

type_init(darwin_register_types)
