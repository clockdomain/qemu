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
 * i2c_server end-to-end scenarios
 * ---------------------------------------------------------------------------
 *
 * Each scenario boots the openprot i2c-qtest firmware in QEMU, asks it to
 * run a numbered routine via the aspeed-qtest-ctrl scratchpad, and asserts
 * on the returned status/result bytes.
 *
 * Expects the firmware ELF path via the `FIRMWARE_ELF` env var (set by the
 * Bazel sh_test wrapper at //target/virt-ast1060-evb/i2c-qtest).
 */

/* Firmware status value for "configured slave, awaiting bus stimulus". The
 * header only defines RUNNING/PASS/FAIL; ARMED is a protocol extension used
 * by slave-mode scenarios, not something the device itself interprets. */
#define STATUS_ARMED 3

/* Slave address the firmware configures on bus 3 for scenarios 9/10/11. */
#define BUS3_SLAVE_ADDR 0x55

/* Generous wall-clock timeout for any per-scenario poll.
 * QEMU boot + firmware init ≈ 1s; slave-mode scenarios wait for bus
 * stimulus. 30s leaves slack for CI. */
#define SCENARIO_TIMEOUT_NS (30LL * G_USEC_PER_SEC * 1000LL)

static QTestState *qtest_init_with_firmware(void)
{
    const char *firmware = g_getenv("FIRMWARE_ELF");
    if (!firmware || firmware[0] == '\0') {
        g_test_skip("FIRMWARE_ELF not set");
        return NULL;
    }
    if (!g_file_test(firmware, G_FILE_TEST_EXISTS)) {
        g_test_skip("FIRMWARE_ELF points at a non-existent file");
        return NULL;
    }
    /*
     * `-accel tcg` is critical: qtest_init() otherwise appends `-accel qtest`
     * which freezes the vCPU. With TCG enabled the guest runs instructions
     * normally; qtest still mediates MMIO reads/writes against the
     * scratchpad and the i2c-test-master.
     */
    char *cmdline = g_strdup_printf(
        "-accel tcg "
        "-M ast1060-evb -cpu cortex-m4 -nographic "
        "-semihosting-config enable=on,target=native "
        "-kernel %s",
        firmware);
    QTestState *s = qtest_init(cmdline);
    g_free(cmdline);
    return s;
}

/*
 * In `-accel qtest` mode, the vCPU is paused between qtest RPCs. To let the
 * firmware make progress, we advance the virtual clock. Each
 * `qtest_clock_step_next` runs guest code up to the next timer deadline.
 *
 * SCENARIO_TIMEOUT_NS bounds the total amount of guest time we're willing
 * to let elapse; each tick advances by one timer event which for
 * `SYS_TICK_HZ=12_000_000` is ~83µs of guest time, so the loop runs a few
 * thousand iterations before the budget is exhausted.
 */
/*
 * With -accel tcg the vCPU runs during wall-clock sleeps, so simple
 * g_usleep() polling is sufficient.
 */
static bool poll_with_timeout(QTestState *s, uint32_t reg_off, uint32_t expected)
{
    int64_t deadline = g_get_monotonic_time() + SCENARIO_TIMEOUT_NS / 1000;
    do {
        if (qtest_readl(s, QTEST_CTRL_BASE + reg_off) == expected) {
            return true;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    return false;
}

static bool poll_status_oneof(QTestState *s, uint32_t a, uint32_t b)
{
    int64_t deadline = g_get_monotonic_time() + SCENARIO_TIMEOUT_NS / 1000;
    do {
        uint32_t v = qtest_readl(s,
            QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_STATUS);
        if (v == a || v == b) {
            return true;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    return false;
}

static void dispatch_scenario(QTestState *s, uint32_t id)
{
    g_assert_true(poll_with_timeout(s, ASPEED_QTEST_CTRL_R_READY, 1));
    qtest_writel(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_SCENARIO_ID, id);
}

static void wait_scenario_done(QTestState *s)
{
    g_assert_true(poll_with_timeout(s, ASPEED_QTEST_CTRL_R_SCENARIO_ID, 0));
}

static void check_pass(QTestState *s)
{
    uint32_t status = qtest_readl(s,
        QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_STATUS);
    if (status != ASPEED_QTEST_CTRL_STATUS_PASS) {
        /* Include the first few result bytes — firmware uses them as a
         * short failure tag (e.g. "cfg1", "read"). */
        char tag[9] = {0};
        uint32_t len = qtest_readl(s,
            QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT_LEN);
        for (unsigned i = 0; i < MIN(len, sizeof(tag) - 1); i++) {
            tag[i] = (char)qtest_readb(s,
                QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT + i);
        }
        g_error("firmware reported status=%u (expected %u), tag=\"%s\"",
                status, ASPEED_QTEST_CTRL_STATUS_PASS, tag);
    }
}

static uint32_t read_result(QTestState *s, uint8_t *buf, size_t cap)
{
    uint32_t len = qtest_readl(s,
        QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT_LEN);
    uint32_t n = MIN(len, cap);
    for (unsigned i = 0; i < n; i++) {
        buf[i] = qtest_readb(s,
            QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_RESULT + i);
    }
    return len;
}

/* i2c-test-master helpers */
static void drive_master_write(QTestState *s, uint8_t addr,
                               const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        qtest_writeb(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_BUF + i, data[i]);
    }
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_ADDR, addr);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_LEN,  len);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_CMD,
                 I2C_TEST_MASTER_CMD_WRITE);
}

static void drive_master_read(QTestState *s, uint8_t addr, uint32_t len)
{
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_ADDR, addr);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_LEN,  len);
    qtest_writel(s, TEST_MASTER_BASE + I2C_TEST_MASTER_R_CMD,
                 I2C_TEST_MASTER_CMD_READ);
}

/* ---------- scenario 1: configure_slave address validation ---------- */
static void test_scenario_01(void)
{
    QTestState *s = qtest_init_with_firmware();
    if (!s) return;

    dispatch_scenario(s, 1);
    wait_scenario_done(s);
    check_pass(s);

    uint8_t got[1] = {0};
    uint32_t len = read_result(s, got, sizeof(got));
    g_assert_cmpuint(len, ==, 1);
    g_assert_cmphex(got[0], ==, 0x0F);  /* all four probes matched */

    qtest_quit(s);
}

/* ---------- scenario 5: master write→read on tmp105, bus 1 ---------- */
static void test_scenario_05(void)
{
    QTestState *s = qtest_init_with_firmware();
    if (!s) return;

    dispatch_scenario(s, 5);
    wait_scenario_done(s);
    check_pass(s);

    uint8_t got[2] = {0};
    uint32_t len = read_result(s, got, sizeof(got));
    g_assert_cmpuint(len, ==, 2);
    /* tmp105 defaults to 0x0000 at reset — the exact value isn't
     * relevant, the test validates the round-trip completed and two
     * bytes came back. */

    qtest_quit(s);
}

/* ---------- scenario 6: NACK path on vacant address ---------- */
static void test_scenario_06(void)
{
    QTestState *s = qtest_init_with_firmware();
    if (!s) return;

    dispatch_scenario(s, 6);
    wait_scenario_done(s);
    check_pass(s);

    qtest_quit(s);
}

/* ---------- scenario 7: register-read loop ---------- */
static void test_scenario_07(void)
{
    QTestState *s = qtest_init_with_firmware();
    if (!s) return;

    dispatch_scenario(s, 7);
    wait_scenario_done(s);
    check_pass(s);

    uint8_t got[5] = {0};
    uint32_t len = read_result(s, got, sizeof(got));
    g_assert_cmpuint(len, ==, 5);

    qtest_quit(s);
}

/* ---------- scenario 8: pca9552 write+write-read on bus 2 ---------- */
static void test_scenario_08(void)
{
    QTestState *s = qtest_init_with_firmware();
    if (!s) return;

    dispatch_scenario(s, 8);
    wait_scenario_done(s);
    check_pass(s);

    uint8_t got[1] = {0};
    uint32_t len = read_result(s, got, sizeof(got));
    g_assert_cmpuint(len, ==, 1);
    g_assert_cmphex(got[0], ==, 0xAA);

    qtest_quit(s);
}

/* ---------- scenario 9: slave-mode RX ---------- */
static void test_scenario_09(void)
{
    static const uint8_t pattern[] = { 0x42, 0x13, 0x37, 0xAB, 0xCD };
    QTestState *s = qtest_init_with_firmware();
    if (!s) return;

    dispatch_scenario(s, 9);
    /* Wait for firmware to configure the slave and arm (or fail early). */
    g_assert_true(poll_status_oneof(s, STATUS_ARMED,
                                    ASPEED_QTEST_CTRL_STATUS_FAIL));
    /* If firmware set FAIL before reaching ARMED, surface the tag now. */
    uint32_t st = qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_STATUS);
    if (st == ASPEED_QTEST_CTRL_STATUS_FAIL) {
        check_pass(s);  /* always aborts with the firmware's tag */
    }

    drive_master_write(s, BUS3_SLAVE_ADDR, pattern, sizeof(pattern));

    wait_scenario_done(s);
    check_pass(s);

    uint8_t got[sizeof(pattern)] = {0};
    uint32_t len = read_result(s, got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(pattern));
    g_assert_cmpmem(got, len, pattern, sizeof(pattern));

    qtest_quit(s);
}

/* ---------- scenario 10: slave-mode TX ---------- */
static void test_scenario_10(void)
{
    QTestState *s = qtest_init_with_firmware();
    if (!s) return;

    dispatch_scenario(s, 10);
    g_assert_true(poll_status_oneof(s, STATUS_ARMED,
                                    ASPEED_QTEST_CTRL_STATUS_FAIL));
    uint32_t st = qtest_readl(s, QTEST_CTRL_BASE + ASPEED_QTEST_CTRL_R_STATUS);
    if (st == ASPEED_QTEST_CTRL_STATUS_FAIL) {
        check_pass(s);
    }

    drive_master_read(s, BUS3_SLAVE_ADDR, 8);

    wait_scenario_done(s);
    check_pass(s);

    uint8_t got[1] = {0};
    uint32_t len = read_result(s, got, sizeof(got));
    g_assert_cmpuint(len, ==, 1);
    g_assert_cmpuint(got[0], ==, 8);  /* firmware reports TX length */

    qtest_quit(s);
}

/* ---------- scenario 11: slave rebind 0x55 → 0x56 ---------- */
static void test_scenario_11(void)
{
    QTestState *s = qtest_init_with_firmware();
    if (!s) return;

    dispatch_scenario(s, 11);
    wait_scenario_done(s);
    check_pass(s);

    uint8_t got[2] = {0};
    uint32_t len = read_result(s, got, sizeof(got));
    g_assert_cmpuint(len, ==, 2);
    g_assert_cmphex(got[0], ==, 0x55);
    g_assert_cmphex(got[1], ==, 0x56);

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

    qtest_add_func("/ast1060/i2c_server/scenario_01", test_scenario_01);
    qtest_add_func("/ast1060/i2c_server/scenario_05", test_scenario_05);
    qtest_add_func("/ast1060/i2c_server/scenario_06", test_scenario_06);
    qtest_add_func("/ast1060/i2c_server/scenario_07", test_scenario_07);
    qtest_add_func("/ast1060/i2c_server/scenario_08", test_scenario_08);
    qtest_add_func("/ast1060/i2c_server/scenario_09", test_scenario_09);
    qtest_add_func("/ast1060/i2c_server/scenario_10", test_scenario_10);
    qtest_add_func("/ast1060/i2c_server/scenario_11", test_scenario_11);

    return g_test_run();
}
