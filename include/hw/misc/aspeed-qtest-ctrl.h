/*
 * Aspeed qtest control — scratchpad MMIO used by firmware and qtest to
 * coordinate scenario dispatch. Not intended for production boards.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ASPEED_QTEST_CTRL_H
#define HW_MISC_ASPEED_QTEST_CTRL_H

#include "exec/hwaddr.h"

typedef struct DeviceState DeviceState;

#define TYPE_ASPEED_QTEST_CTRL "aspeed-qtest-ctrl"

#define ASPEED_QTEST_CTRL_MMIO_SIZE 0x1000
#define ASPEED_QTEST_CTRL_RESULT_SIZE 32

#define ASPEED_QTEST_CTRL_R_SCENARIO_ID 0x00
#define ASPEED_QTEST_CTRL_R_READY       0x04
#define ASPEED_QTEST_CTRL_R_STATUS      0x08
#define ASPEED_QTEST_CTRL_R_RESULT_LEN  0x0c
#define ASPEED_QTEST_CTRL_R_RESULT      0x10

#define ASPEED_QTEST_CTRL_STATUS_RUNNING 0
#define ASPEED_QTEST_CTRL_STATUS_PASS    1
#define ASPEED_QTEST_CTRL_STATUS_FAIL    2

DeviceState *aspeed_qtest_ctrl_create(hwaddr base);

#endif /* HW_MISC_ASPEED_QTEST_CTRL_H */
