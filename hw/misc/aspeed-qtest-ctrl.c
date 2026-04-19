/*
 * Aspeed qtest control — see include/hw/misc/aspeed-qtest-ctrl.h.
 *
 * Passive MMIO scratchpad shared between a qtest-side driver and guest
 * firmware. The device performs no interpretation of register values; it
 * exists only so both sides have a stable location to exchange scenario
 * IDs, ready/status flags, and a result buffer.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/misc/aspeed-qtest-ctrl.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(AspeedQtestCtrl, ASPEED_QTEST_CTRL)

struct AspeedQtestCtrl {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t scenario_id;
    uint32_t ready;
    uint32_t status;
    uint32_t result_len;
    uint8_t  result[ASPEED_QTEST_CTRL_RESULT_SIZE];
};

static uint64_t aspeed_qtest_ctrl_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    AspeedQtestCtrl *s = opaque;

    if (offset >= ASPEED_QTEST_CTRL_R_RESULT &&
        offset <  ASPEED_QTEST_CTRL_R_RESULT +
                  ASPEED_QTEST_CTRL_RESULT_SIZE) {
        return s->result[offset - ASPEED_QTEST_CTRL_R_RESULT];
    }
    switch (offset) {
    case ASPEED_QTEST_CTRL_R_SCENARIO_ID: return s->scenario_id;
    case ASPEED_QTEST_CTRL_R_READY:       return s->ready;
    case ASPEED_QTEST_CTRL_R_STATUS:      return s->status;
    case ASPEED_QTEST_CTRL_R_RESULT_LEN:  return s->result_len;
    default:                              return 0;
    }
}

static void aspeed_qtest_ctrl_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    AspeedQtestCtrl *s = opaque;

    if (offset >= ASPEED_QTEST_CTRL_R_RESULT &&
        offset <  ASPEED_QTEST_CTRL_R_RESULT +
                  ASPEED_QTEST_CTRL_RESULT_SIZE) {
        s->result[offset - ASPEED_QTEST_CTRL_R_RESULT] = value & 0xff;
        return;
    }
    switch (offset) {
    case ASPEED_QTEST_CTRL_R_SCENARIO_ID: s->scenario_id = value; break;
    case ASPEED_QTEST_CTRL_R_READY:       s->ready = value;       break;
    case ASPEED_QTEST_CTRL_R_STATUS:      s->status = value;      break;
    case ASPEED_QTEST_CTRL_R_RESULT_LEN:  s->result_len = value;  break;
    default: break;
    }
}

static const MemoryRegionOps aspeed_qtest_ctrl_ops = {
    .read = aspeed_qtest_ctrl_read,
    .write = aspeed_qtest_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static const VMStateDescription vmstate_aspeed_qtest_ctrl = {
    .name = TYPE_ASPEED_QTEST_CTRL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(scenario_id, AspeedQtestCtrl),
        VMSTATE_UINT32(ready,       AspeedQtestCtrl),
        VMSTATE_UINT32(status,      AspeedQtestCtrl),
        VMSTATE_UINT32(result_len,  AspeedQtestCtrl),
        VMSTATE_UINT8_ARRAY(result, AspeedQtestCtrl,
                            ASPEED_QTEST_CTRL_RESULT_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static void aspeed_qtest_ctrl_reset(DeviceState *dev)
{
    AspeedQtestCtrl *s = ASPEED_QTEST_CTRL(dev);

    s->scenario_id = 0;
    s->ready = 0;
    s->status = 0;
    s->result_len = 0;
    memset(s->result, 0, sizeof(s->result));
}

static void aspeed_qtest_ctrl_realize(DeviceState *dev, Error **errp)
{
    AspeedQtestCtrl *s = ASPEED_QTEST_CTRL(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &aspeed_qtest_ctrl_ops, s,
                          TYPE_ASPEED_QTEST_CTRL,
                          ASPEED_QTEST_CTRL_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void aspeed_qtest_ctrl_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Aspeed qtest control scratchpad";
    dc->realize = aspeed_qtest_ctrl_realize;
    device_class_set_legacy_reset(dc, aspeed_qtest_ctrl_reset);
    dc->vmsd = &vmstate_aspeed_qtest_ctrl;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo aspeed_qtest_ctrl_info = {
    .name = TYPE_ASPEED_QTEST_CTRL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedQtestCtrl),
    .class_init = aspeed_qtest_ctrl_class_init,
};

static void aspeed_qtest_ctrl_register_types(void)
{
    type_register_static(&aspeed_qtest_ctrl_info);
}

type_init(aspeed_qtest_ctrl_register_types)

DeviceState *aspeed_qtest_ctrl_create(hwaddr base)
{
    DeviceState *dev = qdev_new(TYPE_ASPEED_QTEST_CTRL);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    return dev;
}
