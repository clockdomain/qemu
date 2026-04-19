/*
 * I2C test master — see include/hw/i2c/i2c-test-master.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/i2c-test-master.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(I2CTestMaster, I2C_TEST_MASTER)

struct I2CTestMaster {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    I2CBus *bus;

    uint32_t addr;
    uint32_t len;
    uint32_t status;
    uint8_t  buf[I2C_TEST_MASTER_BUF_SIZE];
};

static uint64_t i2c_test_master_read(void *opaque, hwaddr offset, unsigned size)
{
    I2CTestMaster *s = opaque;

    if (offset >= I2C_TEST_MASTER_R_BUF &&
        offset <  I2C_TEST_MASTER_R_BUF + I2C_TEST_MASTER_BUF_SIZE) {
        return s->buf[offset - I2C_TEST_MASTER_R_BUF];
    }
    switch (offset) {
    case I2C_TEST_MASTER_R_ADDR:
        return s->addr;
    case I2C_TEST_MASTER_R_LEN:
        return s->len;
    case I2C_TEST_MASTER_R_STATUS:
        return s->status;
    default:
        return 0;
    }
}

static void i2c_test_master_do_read(I2CTestMaster *s)
{
    unsigned n = s->len;
    unsigned i;

    /* LEN == 0 is an address-phase-only probe (see do_write). */
    if (!s->bus || n > I2C_TEST_MASTER_BUF_SIZE) {
        s->status = I2C_TEST_MASTER_STATUS_NACK;
        return;
    }

    if (i2c_start_transfer(s->bus, s->addr & 0x7f, /* is_recv */ true)) {
        s->status = I2C_TEST_MASTER_STATUS_NACK;
        return;
    }
    for (i = 0; i < n; i++) {
        s->buf[i] = i2c_recv(s->bus);
    }
    i2c_end_transfer(s->bus);
    s->status = 0;
}

static void i2c_test_master_do_write(I2CTestMaster *s)
{
    unsigned n = s->len;
    unsigned i;

    /*
     * LEN == 0 issues an address-phase-only transaction: START, address
     * byte, STOP. Status reflects whether the addressee ACKed, letting
     * tests probe for the presence of a slave without touching data.
     */
    if (!s->bus || n > I2C_TEST_MASTER_BUF_SIZE) {
        s->status = I2C_TEST_MASTER_STATUS_NACK;
        return;
    }

    if (i2c_start_transfer(s->bus, s->addr & 0x7f, /* is_recv */ false)) {
        s->status = I2C_TEST_MASTER_STATUS_NACK;
        return;
    }
    for (i = 0; i < n; i++) {
        if (i2c_send(s->bus, s->buf[i])) {
            i2c_end_transfer(s->bus);
            s->status = I2C_TEST_MASTER_STATUS_NACK;
            return;
        }
    }
    i2c_end_transfer(s->bus);
    s->status = 0;
}

static void i2c_test_master_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    I2CTestMaster *s = opaque;

    if (offset >= I2C_TEST_MASTER_R_BUF &&
        offset <  I2C_TEST_MASTER_R_BUF + I2C_TEST_MASTER_BUF_SIZE) {
        s->buf[offset - I2C_TEST_MASTER_R_BUF] = value & 0xff;
        return;
    }
    switch (offset) {
    case I2C_TEST_MASTER_R_ADDR:
        s->addr = value & 0xff;
        break;
    case I2C_TEST_MASTER_R_LEN:
        s->len = value & 0xff;
        break;
    case I2C_TEST_MASTER_R_CMD:
        switch (value & 0xff) {
        case I2C_TEST_MASTER_CMD_READ:
            i2c_test_master_do_read(s);
            break;
        case I2C_TEST_MASTER_CMD_WRITE:
            i2c_test_master_do_write(s);
            break;
        default:
            break;
        }
        break;
    case I2C_TEST_MASTER_R_STATUS:
        s->status = 0;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps i2c_test_master_ops = {
    .read = i2c_test_master_read,
    .write = i2c_test_master_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static const VMStateDescription vmstate_i2c_test_master = {
    .name = TYPE_I2C_TEST_MASTER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(addr,   I2CTestMaster),
        VMSTATE_UINT32(len,    I2CTestMaster),
        VMSTATE_UINT32(status, I2CTestMaster),
        VMSTATE_UINT8_ARRAY(buf, I2CTestMaster, I2C_TEST_MASTER_BUF_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static void i2c_test_master_reset(DeviceState *dev)
{
    I2CTestMaster *s = I2C_TEST_MASTER(dev);

    s->addr = 0;
    s->len = 0;
    s->status = 0;
    memset(s->buf, 0, sizeof(s->buf));
}

static void i2c_test_master_realize(DeviceState *dev, Error **errp)
{
    I2CTestMaster *s = I2C_TEST_MASTER(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &i2c_test_master_ops, s,
                          TYPE_I2C_TEST_MASTER, I2C_TEST_MASTER_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void i2c_test_master_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "I2C test master (qtest helper)";
    dc->realize = i2c_test_master_realize;
    device_class_set_legacy_reset(dc, i2c_test_master_reset);
    dc->vmsd = &vmstate_i2c_test_master;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo i2c_test_master_info = {
    .name = TYPE_I2C_TEST_MASTER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(I2CTestMaster),
    .class_init = i2c_test_master_class_init,
};

static void i2c_test_master_register_types(void)
{
    type_register_static(&i2c_test_master_info);
}

type_init(i2c_test_master_register_types)

DeviceState *i2c_test_master_create(hwaddr base, I2CBus *bus)
{
    I2CTestMaster *s = I2C_TEST_MASTER(qdev_new(TYPE_I2C_TEST_MASTER));

    s->bus = bus;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s), 0, base);
    return DEVICE(s);
}
