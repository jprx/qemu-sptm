#pragma once
#include "qemu/osdep.h"
#include "qom/object.h"
#include "hw/core/sysbus.h"
#include "xnu/apple_dtree.h"

typedef enum {
    AIC_UNKNOWN = 0,
    AIC_V1 = 1,
    AIC_V2 = 2,
    AIC_V3 = 3,
} aic_version_t;

DeviceState *apple_aic_create(struct dtree_node *aic_node, uint64_t iobase);
