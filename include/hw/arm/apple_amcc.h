#ifndef APPLE_AMCC_H
#define APPLE_AMCC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "xnu/apple_dtree.h"

#define TYPE_AMCC "amcc"
OBJECT_DECLARE_SIMPLE_TYPE(AMCCState, AMCC)

enum {
    AMCC_BANK_A           =   0,
    AMCC_BANK_B           =   1,
    AMCC_BANK_C           =   2,
    AMCC_BANK_D           =   3,

    AMCC_NUM_BANKS
};

struct AMCCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    hwaddr ctrr_lwr[AMCC_NUM_BANKS];
    hwaddr ctrr_upr[AMCC_NUM_BANKS];
    hwaddr lwr_reg[AMCC_NUM_BANKS];
    hwaddr upr_reg[AMCC_NUM_BANKS];
};

DeviceState *amcc_create(struct dtree_node *amcc_node);

#endif // APPLE_AMCC_H
