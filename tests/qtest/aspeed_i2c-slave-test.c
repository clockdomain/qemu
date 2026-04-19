/*
 * QTest for the Aspeed I2C controller slave-mode DMA TX path.
 *
 * Pre-loads a byte pattern into guest SRAM, arms bus 0 of the Aspeed
 * I2C controller as a slave answering to a fixed address, then uses
 * the sideband i2c-test-master on the same bus to issue a read.
 * The received bytes and the TX_LEN status counter are asserted
 * against the pattern.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "hw/i2c/i2c-test-master.h"

/* ast1030 memory map */
#define SRAM_BASE          0x00000000
#define I2C_BASE           0x7E7B0000
#define I2C_BUS0_BASE      (I2C_BASE + 0x80) /* bus N at 0x80*(N+1) */

/* Global controller regs */
#define I2C_CTRL_GLOBAL    (I2C_BASE + 0x0c)
#define CTRL_GLOBAL_SRAM_EN  (1u << 0)
#define CTRL_GLOBAL_REG_MODE (1u << 2)

/* Per-bus regs (new-mode offsets within the bus block) */
#define R_I2CC_FUN_CTRL    0x00
#define R_I2CS_CMD         0x28
#define R_I2CS_DMA_LEN     0x2c
#define R_I2CS_DMA_TX_ADDR 0x38
#define R_I2CS_DEV_ADDR    0x40
#define R_I2CS_DMA_LEN_STS 0x4c

#define FUN_CTRL_SLAVE_EN  (1u << 1)
#define I2CS_CMD_TX_DMA_EN (1u << 8)

/* Test-master MMIO base (set by ast1030_evb_i2c_init) */
#define TEST_MASTER_BASE   0x7E7C0000

/* Test parameters */
#define SLAVE_ADDR         0x42
#define DMA_OFFSET         0x00020000 /* well within the 768 KiB internal SRAM */
#define PATTERN_LEN        16

static uint32_t bus0_read(QTestState *s, unsigned reg)
{
    return qtest_readl(s, I2C_BUS0_BASE + reg);
}

static void bus0_write(QTestState *s, unsigned reg, uint32_t value)
{
    qtest_writel(s, I2C_BUS0_BASE + reg, value);
}

static void test_slave_dma_tx(void)
{
    QTestState *s;
    uint8_t pattern[PATTERN_LEN];
    uint8_t received[PATTERN_LEN];
    uint32_t tx_len_sts;
    unsigned i;

    for (i = 0; i < PATTERN_LEN; i++) {
        pattern[i] = 0xA0 | (i & 0x0f);
    }

    s = qtest_init("-M ast1030-evb -nographic");

    /* 1. Pre-load the pattern into guest SRAM. */
    qtest_memwrite(s, SRAM_BASE + DMA_OFFSET, pattern, PATTERN_LEN);

    /* 2. Switch the I2C controller to new mode with SRAM enabled. */
    qtest_writel(s, I2C_CTRL_GLOBAL,
                 CTRL_GLOBAL_REG_MODE | CTRL_GLOBAL_SRAM_EN);

    /* 3. Arm bus 0 as a slave answering at SLAVE_ADDR. */
    bus0_write(s, R_I2CC_FUN_CTRL, FUN_CTRL_SLAVE_EN);
    bus0_write(s, R_I2CS_DEV_ADDR, SLAVE_ADDR);

    /* 4. Program the DMA TX source and length. */
    bus0_write(s, R_I2CS_DMA_TX_ADDR, DMA_OFFSET);
    /* TX_BUF_LEN encodes (N - 1) per the patched code at aspeed_i2c.c:1441. */
    bus0_write(s, R_I2CS_DMA_LEN, PATTERN_LEN - 1);

    /* 5. Arm TX_DMA_EN. The bit self-clears after I2C_FINISH. */
    bus0_write(s, R_I2CS_CMD, I2CS_CMD_TX_DMA_EN);

    /* 6. Drive a read from the synthetic master on the same bus. */
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_ADDR, SLAVE_ADDR);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_LEN,  PATTERN_LEN);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_CMD,
                 I2C_TEST_MASTER_CMD_READ);

    /* 7. Verify no NACK and the buffer contents match the pattern. */
    g_assert_cmpuint(qtest_readl(s, TEST_MASTER_BASE +
                                    I2C_TEST_MASTER_R_STATUS), ==, 0);
    for (i = 0; i < PATTERN_LEN; i++) {
        received[i] = qtest_readb(s, TEST_MASTER_BASE +
                                     I2C_TEST_MASTER_R_BUF + i);
    }
    g_assert_cmpmem(received, PATTERN_LEN, pattern, PATTERN_LEN);

    /* 8. TX_LEN status counter should equal PATTERN_LEN. */
    tx_len_sts = bus0_read(s, R_I2CS_DMA_LEN_STS) & 0x1fff;
    g_assert_cmpuint(tx_len_sts, ==, PATTERN_LEN);

    /* 9. TX_DMA_EN should have self-cleared via the I2C_FINISH handler. */
    g_assert_cmpuint(bus0_read(s, R_I2CS_CMD) & I2CS_CMD_TX_DMA_EN, ==, 0);

    qtest_quit(s);
}

static void test_slave_dma_tx_back_to_back(void)
{
    QTestState *s;
    uint8_t pattern[PATTERN_LEN];
    uint8_t received[PATTERN_LEN];
    unsigned round;

    s = qtest_init("-M ast1030-evb -nographic");

    qtest_writel(s, I2C_CTRL_GLOBAL,
                 CTRL_GLOBAL_REG_MODE | CTRL_GLOBAL_SRAM_EN);
    bus0_write(s, R_I2CC_FUN_CTRL, FUN_CTRL_SLAVE_EN);
    bus0_write(s, R_I2CS_DEV_ADDR, SLAVE_ADDR);

    for (round = 0; round < 3; round++) {
        unsigned i;

        for (i = 0; i < PATTERN_LEN; i++) {
            pattern[i] = (round << 4) | (i & 0x0f);
        }
        qtest_memwrite(s, SRAM_BASE + DMA_OFFSET, pattern, PATTERN_LEN);

        bus0_write(s, R_I2CS_DMA_TX_ADDR, DMA_OFFSET);
        bus0_write(s, R_I2CS_DMA_LEN, PATTERN_LEN - 1);
        bus0_write(s, R_I2CS_CMD, I2CS_CMD_TX_DMA_EN);

        qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_ADDR, SLAVE_ADDR);
        qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_LEN,  PATTERN_LEN);
        qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_CMD,
                     I2C_TEST_MASTER_CMD_READ);

        for (i = 0; i < PATTERN_LEN; i++) {
            received[i] = qtest_readb(s, TEST_MASTER_BASE +
                                         I2C_TEST_MASTER_R_BUF + i);
        }
        g_assert_cmpmem(received, PATTERN_LEN, pattern, PATTERN_LEN);
    }

    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ast1030/i2c/slave_dma_tx",             test_slave_dma_tx);
    qtest_add_func("/ast1030/i2c/slave_dma_tx_back_to_back",
                   test_slave_dma_tx_back_to_back);
    return g_test_run();
}
