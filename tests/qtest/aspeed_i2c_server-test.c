/*
 * QTests for the openprot i2c_server IPC service running on ast1060-evb.
 *
 * This file holds the infrastructure-only tests. Full scenario coverage
 * (slave_config validation, init speeds, master ACK/NACK, slave-mode
 * RX/TX, …) lands in subsequent phases alongside the firmware
 * test-client app at //target/ast1060-evb/i2c-qtest.
 *
 * Infra tests covered here:
 *   - aspeed-qtest-ctrl register round-trip
 *   - i2c-test-master LEN=0 probe: ACK against the known slave on bus 3
 *     (pca9554 @ 0x20) vs NACK against an empty address
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "hw/i2c/i2c-test-master.h"
#include "hw/misc/aspeed-qtest-ctrl.h"

/* Sysbus base addresses wired into hw/arm/aspeed_ast10x0_evb.c */
#define TEST_MASTER_BASE 0x7E7C0000
#define QTEST_CTRL_BASE  0x7E7D0000

/* Known slaves on ast1060-evb after the AST10x0 I²C patch series */
#define BUS3_PCA9554_ADDR 0x20
#define BUS3_EMPTY_ADDR   0x55

/*
 * ---------------------------------------------------------------------------
 * aspeed-qtest-ctrl: passive scratchpad round-trip
 * ---------------------------------------------------------------------------
 */
static void test_ctrl_scalar_rw(void)
{
    QTestState *s = qtest_init("-M ast1060-evb -nographic");

    qtest_writel(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_SCENARIO_ID,
                 0xdeadbeef);
    qtest_writel(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_READY, 1);
    qtest_writel(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_STATUS,
                 ASPEED_QTEST_CTRL_STATUS_PASS);
    qtest_writel(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT_LEN, 7);

    g_assert_cmphex(
        qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_SCENARIO_ID),
        ==, 0xdeadbeef);
    g_assert_cmpuint(
        qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_READY),
        ==, 1);
    g_assert_cmpuint(
        qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_STATUS),
        ==, ASPEED_QTEST_CTRL_STATUS_PASS);
    g_assert_cmpuint(
        qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT_LEN),
        ==, 7);

    qtest_quit(s);
}

static void test_ctrl_result_buffer_rw(void)
{
    QTestState *s = qtest_init("-M ast1060-evb -nographic");
    unsigned i;

    for (i = 0; i < ASPEED_QTEST_CTRL_RESULT_SIZE; i++) {
        qtest_writeb(s,
                     QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT + i,
                     (uint8_t)(0xC0 | (i & 0x1f)));
    }
    for (i = 0; i < ASPEED_QTEST_CTRL_RESULT_SIZE; i++) {
        uint8_t b = qtest_readb(s,
                                QTEST_CTRL_BASE +
                                    ASPEED_QTEST_CTRL_R_RESULT + i);
        g_assert_cmpuint(b, ==, (uint8_t)(0xC0 | (i & 0x1f)));
    }
    qtest_quit(s);
}

static void test_ctrl_reset_clears(void)
{
    QTestState *s = qtest_init("-M ast1060-evb -nographic");

    qtest_writel(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_SCENARIO_ID, 42);
    qtest_writel(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_READY, 1);
    qtest_writeb(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT + 0, 0xAA);

    qtest_system_reset(s);

    g_assert_cmpuint(
        qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_SCENARIO_ID),
        ==, 0);
    g_assert_cmpuint(
        qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_READY),
        ==, 0);
    g_assert_cmpuint(
        qtest_readb(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT + 0),
        ==, 0);
    qtest_quit(s);
}

/*
 * ---------------------------------------------------------------------------
 * i2c-test-master LEN=0 probe
 * ---------------------------------------------------------------------------
 */
static void drive_probe(QTestState *s, uint8_t addr)
{
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_ADDR, addr);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_LEN,  0);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_CMD,
                 I2C_TEST_MASTER_CMD_WRITE);
}

static void test_probe_ack(void)
{
    /*
     * pca9554 is instantiated at bus 3 / 0x20 by ast1060_evb_i2c_init,
     * and the i2c-test-master on ast1060-evb is wired to bus 3. An
     * address-phase-only transaction to 0x20 should ACK, leaving STATUS=0.
     */
    QTestState *s = qtest_init("-M ast1060-evb -nographic");

    drive_probe(s, BUS3_PCA9554_ADDR);
    g_assert_cmpuint(
        qtest_readl(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_STATUS),
        ==, 0);
    qtest_quit(s);
}

static void test_probe_nack(void)
{
    /* 0x55 is unoccupied on bus 3. Probe should NACK. */
    QTestState *s = qtest_init("-M ast1060-evb -nographic");

    drive_probe(s, BUS3_EMPTY_ADDR);
    g_assert_cmpuint(
        qtest_readl(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_STATUS),
        ==, I2C_TEST_MASTER_STATUS_NACK);
    qtest_quit(s);
}

/*
 * ---------------------------------------------------------------------------
 * Entrypoint
 * ---------------------------------------------------------------------------
 */
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/ast1060/qtest_ctrl/scalar_rw",     test_ctrl_scalar_rw);
    qtest_add_func("/ast1060/qtest_ctrl/result_buf_rw", test_ctrl_result_buffer_rw);
    qtest_add_func("/ast1060/qtest_ctrl/reset_clears",  test_ctrl_reset_clears);
    qtest_add_func("/ast1060/test_master/probe_ack",    test_probe_ack);
    qtest_add_func("/ast1060/test_master/probe_nack",   test_probe_nack);

    return g_test_run();
}
