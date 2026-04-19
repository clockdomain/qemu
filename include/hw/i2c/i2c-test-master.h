/*
 * I2C test master — a sysbus helper device used by qtest to drive
 * master transactions onto an existing I2C bus.
 *
 * Not intended for production machine use. Mapped into the AST1030 EVB
 * board alongside the SoC peripherals so qtest cases can exercise the
 * Aspeed I2C controller's slave-mode paths.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_TEST_MASTER_H
#define HW_I2C_TEST_MASTER_H

#include "exec/hwaddr.h"

typedef struct I2CBus I2CBus;
typedef struct DeviceState DeviceState;

#define TYPE_I2C_TEST_MASTER "i2c-test-master"

/* MMIO register layout (little-endian 32-bit) */
#define I2C_TEST_MASTER_R_ADDR   0x00 /* 7-bit I2C target address */
#define I2C_TEST_MASTER_R_LEN    0x04 /* transfer length, 1..64 */
#define I2C_TEST_MASTER_R_CMD    0x08 /* 1 = read, 2 = write */
#define I2C_TEST_MASTER_R_STATUS 0x0c /* bit 0 NACK, write any to clear */
#define I2C_TEST_MASTER_R_BUF    0x10 /* 64-byte scratch buffer */

#define I2C_TEST_MASTER_BUF_SIZE 64
#define I2C_TEST_MASTER_MMIO_SIZE 0x80

#define I2C_TEST_MASTER_CMD_READ  1
#define I2C_TEST_MASTER_CMD_WRITE 2

#define I2C_TEST_MASTER_STATUS_NACK 0x1

/*
 * Create and realize an i2c-test-master at the given sysbus address,
 * linked to the supplied I2C bus. Returns the device on success.
 */
DeviceState *i2c_test_master_create(hwaddr base, I2CBus *bus);

#endif /* HW_I2C_TEST_MASTER_H */
