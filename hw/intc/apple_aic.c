#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "target/arm/cpu.h"
#include "hw/intc/apple_aic.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"

/*
 * minimal aic implementation:
 *  - knows how many interrupts there are
 *  - knows which are enabled/ disabled
 *  - can receive interrupts from devices, and send irqs to core 0
 *  - can respond to eoi conditions
 *  - supports v1-3
 *  - ignores everything else
 */

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t  u8;

// #define aic_log printf
#define aic_log(...)

#define TYPE_APPLE_AIC "APPLE_AIC"
OBJECT_DECLARE_SIMPLE_TYPE(AppleAICState, APPLE_AIC)

// Use this for AICs where we don't know how many interrupts there should be
#define A_GOOD_NUMBER_OF_IRQS           0x1000

#define IRQS_PER_WORD                   (( 8 * sizeof(u32) ))
#define AIC_EOI_FLAG_VALID_INT          0x10000

/* ref: AppleInterruptControllerVX::enableVector & ::initVector */
#define AIC1_REG_NUM_INTS               0x4
#define AIC1_REG_IACK                   0x2004
#define AIC1_ACK_BASE                   0x4080
#define AIC1_ACK_SIZE                   0x80
#define AIC1_DISABLE_BASE               0x4100
#define AIC1_DISABLE_SIZE               0x80
#define AIC1_ENABLE_BASE                0x4180
#define AIC1_ENABLE_SIZE                0x80

#define AIC2_REG_NUM_INTS               0x0C
#define AIC2_DISABLE_BASE               0x6400
#define AIC2_DISABLE_SIZE               0x200
#define AIC2_ENABLE_BASE                0x6600
#define AIC2_ENABLE_SIZE                0x200

#define AIC3_REG_NUM_INTS               0x0C
#define AIC3_DISABLE_BASE               0x14400
#define AIC3_DISABLE_SIZE               0x200
#define AIC3_ENABLE_BASE                0x14600
#define AIC3_ENABLE_SIZE                0x200

/*
 * FOR_BITVEC_IN
 * o: offset of a register access
 * b: base on the MMIO region to test for
 * sz: size of the MMIO region to test for
 *
 * If the register access at offset o is within the MMIO region starting at b
 * and sz bytes large, this will loop over every bit in the register, and
 * define the irqnum variable to correspond to which interrupt number the
 * current iteration refers to.
 *
 * It won't do anything if offset isn't within the bounds of this MMIO region.
 *
 * Yes, this macro is *extremely* cursed. No I'm not sorry.
 */
#define FOR_BITVEC_IN(o, b, sz) \
    for (size_t i = 0, irqnum = 0; \
        addr_between(o, b, sz) && i < IRQS_PER_WORD; \
        i++, irqnum = i + IRQS_PER_WORD * ((o - b) >> 2))

struct AppleAICState {
    SysBusDevice   parent_obj;
    MemoryRegion   mmio;
    size_t         physlen;
    uint32_t       num_irqs;
    qemu_irq       irq_out;
    bool          *enable;
    bool          *pending;
    aic_version_t  version;
    bool           has_ack_region;
    size_t         r_iack;
    size_t         r_num_ints;
    size_t         r_enable_base, r_enable_size;
    size_t         r_disable_base, r_disable_size;
    size_t         r_ack_base, r_ack_size; // only valid if has_ack_region
};

static bool addr_between(hwaddr addr, hwaddr base, size_t len) {
    return addr >= base && addr < base + len;
}

static void apple_aic_update_irqs(AppleAICState *aic) {
    for (size_t i = 0; i < aic->num_irqs; i++) {
        if (aic->pending[i] && aic->enable[i]) {
            aic_log("aic: raising IRQ\n");
            qemu_irq_raise(aic->irq_out);
            return;
        }
    }
}

static u64 aic_handle_eoi(AppleAICState *aic) {
    // AppleInterruptController::handleInterrupt will keep looping looking for
    // more interrupts until this register returns 0.
    for (size_t i = 0; i < aic->num_irqs; i++) {
        if (aic->pending[i] && aic->enable[i]) {
            if (!aic->has_ack_region) aic->pending[i] = false;
            aic_log("aic: EOI for int num %zu\n", i);
            return i | AIC_EOI_FLAG_VALID_INT;
        }
    }

    // All interrupts handled, we can shut down now
    aic_log("aic: EOI with no remaining interrupts\n");
    qemu_irq_lower(aic->irq_out);
    return 0;
}

static uint64_t apple_aic_read(void *opaque, hwaddr offset, unsigned size) {
    AppleAICState *aic = APPLE_AIC(opaque);

    assert(sizeof(uint32_t) == size);

    aic_log("aic: read(offset 0x%llX, pc 0x%llX, lr 0x%llX)\n", offset, cpu_env(qemu_get_cpu(0))->pc, cpu_env(qemu_get_cpu(0))->xregs[30]);

    if (aic->r_num_ints == offset) return aic->num_irqs;
    if (aic->r_iack == offset) return aic_handle_eoi(aic);
    return 0;
}

static void apple_aic_write(void *opaque, hwaddr offset, uint64_t value, unsigned size) {
    AppleAICState *aic = APPLE_AIC(opaque);

    assert(sizeof(uint32_t) == size);

    aic_log("aic: write(offset 0x%llX, value 0x%llX, pc 0x%llX, lr 0x%llX)\n", offset, value, cpu_env(qemu_get_cpu(0))->pc, cpu_env(qemu_get_cpu(0))->xregs[30]);

    FOR_BITVEC_IN(offset, aic->r_disable_base, aic->r_disable_size) {
        if (1 & (value >> i)) {
            aic_log("aic: disabling IRQ %ld\n", irqnum);
            aic->enable[irqnum] = false;
        }
    }

    FOR_BITVEC_IN(offset, aic->r_enable_base, aic->r_enable_size) {
        if (1 & (value >> i)) {
            aic_log("aic: enabling IRQ %ld\n", irqnum);
            aic->enable[irqnum] = true;
        }
    }

    if (aic->has_ack_region) {
        FOR_BITVEC_IN(offset, aic->r_ack_base, aic->r_ack_size) {
            if (1 & (value >> i)) {
                aic_log("aic: acknowledging IRQ %ld\n", irqnum);
                aic->pending[irqnum] = false;
            }
        }
    }

    apple_aic_update_irqs(aic);
}

static void apple_aic_irq_request(void *opaque, int n, int level) {
    AppleAICState *aic = opaque;
    aic_log("aic: interrupt %d taken (level %d)\n", n, level);
    if (level) aic->pending[n] = true;
    apple_aic_update_irqs(aic);
}

static const MemoryRegionOps apple_aic_ops = {
    .read  = apple_aic_read,
    .write = apple_aic_write,
};

static void apple_aic_realize(DeviceState *dev, Error **errp) {
    AppleAICState *aic = APPLE_AIC(dev);

    memory_region_init_io(&aic->mmio, OBJECT(dev), &apple_aic_ops, aic, TYPE_APPLE_AIC, aic->physlen);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &aic->mmio);

    aic->enable  = g_new0(bool, aic->num_irqs);
    aic->pending = g_new0(bool, aic->num_irqs);

    qdev_init_gpio_in(DEVICE(aic), apple_aic_irq_request, aic->num_irqs);
    qdev_init_gpio_out(DEVICE(aic), &aic->irq_out, 1);
}

static void apple_aic_class_init(ObjectClass *klass, const void *data) {
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = apple_aic_realize;
}

static const TypeInfo apple_aic1_info = {
    .name           =  TYPE_APPLE_AIC,
    .parent         =  TYPE_SYS_BUS_DEVICE,
    .instance_size  =  sizeof(AppleAICState),
    .class_init     =  apple_aic_class_init,
};

static void apple_aic_register_types(void) {
    type_register_static(&apple_aic1_info);
}

type_init(apple_aic_register_types)

DeviceState *apple_aic_create(struct dtree_node *adt_node, uint64_t iobase) {
    DeviceState *dev = qdev_new(TYPE_APPLE_AIC);
    AppleAICState *aic = APPLE_AIC(dev);
    struct adt_io_reg *aic_reg;

    assert(32 == IRQS_PER_WORD);
    assert(adt_node);
    aic_reg = adt_get_prop_val(adt_node, "reg");
    assert(aic_reg);

    aic->version = AIC_UNKNOWN;
    aic->physlen = aic_reg[0].len;
    sscanf(adt_get_prop_val(adt_node, "compatible"), "aic,%d", &aic->version);

    switch (aic->version) {
        case AIC_V1:
            // aic,1 must have *exactly* 8x the length of ipid-mask interrupt lines
            aic->num_irqs        =  8 * adt_get_prop_len(adt_node, "ipid-mask");
            aic->has_ack_region  =  true;
            aic->r_iack          =  AIC1_REG_IACK;
            aic->r_num_ints      =  AIC1_REG_NUM_INTS;
            aic->r_enable_base   =  AIC1_ENABLE_BASE;
            aic->r_enable_size   =  AIC1_ENABLE_SIZE;
            aic->r_disable_base  =  AIC1_DISABLE_BASE;
            aic->r_disable_size  =  AIC1_DISABLE_SIZE;
            aic->r_ack_base      =  AIC1_ACK_BASE;
            aic->r_ack_size      =  AIC1_ACK_SIZE;
            break;

        case AIC_V2:
            aic->num_irqs        =  A_GOOD_NUMBER_OF_IRQS;
            aic->has_ack_region  =  false;
            aic->r_iack          = *(u64*)adt_get_prop_val(adt_node, "aic-iack-offset");
            aic->r_num_ints      =  AIC2_REG_NUM_INTS;
            aic->r_enable_base   =  AIC2_ENABLE_BASE;
            aic->r_enable_size   =  AIC2_ENABLE_SIZE;
            aic->r_disable_base  =  AIC2_DISABLE_BASE;
            aic->r_disable_size  =  AIC2_DISABLE_SIZE;
            break;

        case AIC_V3:
            aic->num_irqs        =  A_GOOD_NUMBER_OF_IRQS;
            aic->has_ack_region  =  false;
            aic->r_iack          = *(u64*)adt_get_prop_val(adt_node, "aic-iack-offset");
            aic->r_num_ints      =  AIC3_REG_NUM_INTS;
            aic->r_enable_base   =  AIC3_ENABLE_BASE;
            aic->r_enable_size   =  AIC3_ENABLE_SIZE;
            aic->r_disable_base  =  AIC3_DISABLE_BASE;
            aic->r_disable_size  =  AIC3_DISABLE_SIZE;
            break;

        default:
            // Note: you don't actually need a working aic to boot to a root
            // shell; all you need usually is for the aic to report that the
            // number of interrupts is nonzero. Proper aic support is only
            // needed if you want the qemuport UARTs (non-primary UARTs) to
            // work. uart0 doesn't use interrupts so it always works even if
            // the aic doesn't.
            fprintf(stderr, "unknown aic version %d\n", aic->version);
            exit(1);
            break;
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, aic_reg[0].base + iobase);
    return dev;
}
