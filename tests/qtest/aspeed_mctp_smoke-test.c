/*
 * Smoke qtest for the openprot mctp_server polling loop.
 *
 * Boots the production mctp firmware image (no qtest-feature instrumentation
 * required), then drives a single SPDM `GetVersion` request as an
 * MCTP-over-SMBus frame onto bus 2 via the `i2c-test-master` device. The
 * test passes if the firmware's I²C slave (configured at OWN_I2C_ADDR=0x10)
 * acknowledges the address phase and receives the data phase without NACK.
 *
 * What it proves:
 *   - mctp_loop reaches steady-state (configure_target_address + enable_receive
 *     completed; slave bound on the AST controller).
 *   - The wire-level MCTP-I2C framing matches what `MctpI2cEncap::decode`
 *     in mctp-estack expects (header layout, byte_count semantics, SMBus PEC).
 *
 * What it does NOT prove (out of scope for this smoke test):
 *   - That `MctpI2cReceiver::decode` actually decoded the frame (visible in
 *     the firmware's tokenized log, but not via the qtest channel here).
 *   - That `Server::inbound` accepted the packet and routed it to the SPDM
 *     listener.
 *   - That the SPDM responder produced a correct `Version` response.
 *
 * Capturing the firmware's outbound response would need either an extension
 * to `i2c-test-master` to also act as a slave at the source address, or a
 * separate loopback-slave device. Both are follow-ups; see
 * plan-mctp-server-qtest.md (M7).
 *
 * Expects:
 *   FIRMWARE_ELF=/path/to/spdm-requester/bazel-bin/target/virt-ast1060-evb/mctp/mctp.elf
 *
 * Run directly:
 *   FIRMWARE_ELF=… QTEST_QEMU_BINARY=… \
 *     ./aspeed_mctp_smoke-test -p /arm/ast1060/mctp/getversion_smoke
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "hw/i2c/i2c-test-master.h"

/* Bus-2 i2c-test-master sysbus base, mounted by hw/arm/aspeed_ast10x0_evb.c. */
#define TEST_MASTER_BUS2_BASE  0x7E7C1000

/*
 * aspeed-qtest-ctrl scratchpad MMIO. The mctp_server, when built with the
 * services/mctp/server `qtest` Cargo feature, snapshots its polling-loop
 * counters into the R_RESULT region every 256 loop iterations.
 *
 * Slot layout (each slot is a u32 written byte-by-byte, since the device
 * truncates multi-byte writes to R_RESULT to a single byte):
 *   slot 0 (0x10): i2c_pkt      -- frames decoded + accepted by router
 *   slot 1 (0x14): idle_polls   -- wait_for_messages timeouts
 *   slot 2 (0x18): decode_err   -- MctpI2cReceiver::decode failures
 *   slot 3 (0x1c): inbound_err  -- Server::inbound() rejections
 *   slot 4 (0x20): spdm_ok      -- responder_process_message Ok
 *   slot 5 (0x24): spdm_err     -- responder_process_message Err (high in idle)
 */
#define QTEST_CTRL_BASE        0x7E7D0000
#define QTEST_CTRL_R_READY     0x04
#define QTEST_CTRL_R_RESULT    0x10
#define COUNTER_I2C_PKT        0
#define COUNTER_IDLE_POLLS     1
#define COUNTER_DECODE_ERR     2
#define COUNTER_INBOUND_ERR    3
#define COUNTER_SPDM_OK        4
#define COUNTER_SPDM_ERR       5

/* Firmware-side constants — must match services/mctp/server/src/main.rs. */
#define FW_OWN_I2C_ADDR        0x10  /* OWN_I2C_ADDR */
#define FW_OWN_EID             0x08  /* OWN_EID */

/* qtest-side identity. Pick any 7-bit address not colliding with bus-2
 * peripherals (pca9552 @ 0x60). */
#define QTEST_SRC_I2C_ADDR     0x21
#define QTEST_SRC_EID          0x21

/* MCTP message types (DSP0239) */
#define MCTP_MSG_TYPE_SPDM     0x05

/* SPDM 1.0 GetVersion (DSP0274 §10.2) */
#define SPDM_VERSION_BYTE      0x10  /* SPDM v1.0 */
#define SPDM_REQ_GET_VERSION   0x84

/* MCTP-I2C transport header (mctp-estack/src/i2c.rs) */
#define MCTP_I2C_CMD           0x0F  /* MCTP_I2C_COMMAND_CODE */

/* Wall-clock budget for firmware boot to reach the polling loop. */
#define BOOT_BUDGET_MS         3000

/*
 * SMBus Packet Error Code: CRC-8-CCITT, polynomial 0x07, init 0x00.
 * Matches mctp-estack's smbus_pec dependency.
 */
static uint8_t smbus_pec(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                               : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

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
 * Read a u32 counter from the scratchpad's R_RESULT slot. The mctp_server
 * (with the `qtest` Cargo feature on) writes each counter byte-by-byte
 * into 4-byte slots starting at R_RESULT.
 */
static uint32_t read_counter(QTestState *s, unsigned slot)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++) {
        v |= (uint32_t)qtest_readb(s,
            QTEST_CTRL_BASE + QTEST_CTRL_R_RESULT + slot * 4 + i) << (i * 8);
    }
    return v;
}

/*
 * Drive an SMBus master write through i2c-test-master.
 * `body` is everything AFTER the device-emitted dest_addr byte:
 *   [cmd, byte_count, src_addr_byte, MCTP_payload..., PEC]
 */
static void drive_write(QTestState *s, uint8_t addr,
                        const uint8_t *body, size_t len)
{
    g_assert_cmpuint(len, <=, I2C_TEST_MASTER_BUF_SIZE);
    for (size_t i = 0; i < len; i++) {
        qtest_writeb(s, TEST_MASTER_BUS2_BASE + I2C_TEST_MASTER_R_BUF + i,
                     body[i]);
    }
    qtest_writel(s, TEST_MASTER_BUS2_BASE + I2C_TEST_MASTER_R_ADDR, addr);
    qtest_writel(s, TEST_MASTER_BUS2_BASE + I2C_TEST_MASTER_R_LEN, len);
    qtest_writel(s, TEST_MASTER_BUS2_BASE + I2C_TEST_MASTER_R_CMD,
                 I2C_TEST_MASTER_CMD_WRITE);
}

static void test_getversion_smoke(void)
{
    QTestState *s = qtest_init_with_firmware();
    if (!s) {
        return;
    }

    /*
     * Give the firmware time to:
     *   1. boot pigweed kernel + start mctp_server / i2c_server processes
     *   2. mctp_loop: configure_target_address(BUS_2, 0x10)
     *   3. mctp_loop: enable_receive(BUS_2)
     *   4. enter the polling loop on wait_for_messages
     * On a host TCG @ ast1060-evb this completes well under 3 s.
     */
    g_usleep((gulong)BOOT_BUDGET_MS * 1000);

    /*
     * Build the on-the-wire MCTP-over-SMBus frame for a GetVersion request.
     *
     * Wire layout (per mctp-estack/src/i2c.rs `MctpI2cEncap::encode`):
     *   [0] dest << 1                      (i2c-test-master generates this)
     *   [1] MCTP_I2C_CMD = 0x0F
     *   [2] byte_count = MCTP_payload_len + 1
     *   [3] src << 1 | 1
     *   [4..] MCTP packet:
     *           hdr_ver=0x01, dest_eid, src_eid,
     *           flags = SOM(7) | EOM(6) | seq(5..4) | TO(3) | tag(2..0)
     *         then message body:
     *           msg_type=0x05 (SPDM)
     *           SPDM GetVersion: [version=0x10, req=0x84, p1=0, p2=0]
     *   [N-1] PEC = CRC-8 over bytes [0..N-1]
     */
    /*
     * Fixed sizes (QEMU build rejects VLAs):
     *   MCTP_LEN  = 4-byte hdr + 1-byte msg_type + 4-byte SPDM GetVersion = 9
     *   PEC input = 1-byte dest + 4-byte i2c hdr + MCTP_LEN              = 14
     *   R_BUF body = 4-byte i2c hdr - dest + MCTP_LEN + 1-byte PEC       = 13
     */
    enum { MCTP_LEN = 9, PEC_INPUT_LEN = 14, BODY_LEN = 13 };

    uint8_t mctp_payload[MCTP_LEN] = {
        /* MCTP transport header (4 bytes) */
        0x01,                      /* hdr ver = 1, reserved = 0 */
        FW_OWN_EID,                /* dest EID = 8 (firmware) */
        QTEST_SRC_EID,             /* src EID  = 0x21 (this test) */
        0xC8,                      /* SOM=1, EOM=1, seq=0, TO=1, tag=0 */
        /* MCTP message body */
        MCTP_MSG_TYPE_SPDM,        /* 0x05 */
        SPDM_VERSION_BYTE,         /* SPDM v1.0 */
        SPDM_REQ_GET_VERSION,      /* GET_VERSION */
        0x00,                      /* param1 */
        0x00,                      /* param2 */
    };

    /* PEC input is the COMPLETE on-bus frame including dest_addr,
     * which the test-master device generates from R_ADDR. */
    uint8_t pec_buf[PEC_INPUT_LEN];
    pec_buf[0] = (uint8_t)(FW_OWN_I2C_ADDR << 1);
    pec_buf[1] = MCTP_I2C_CMD;
    pec_buf[2] = (uint8_t)(MCTP_LEN + 1);
    pec_buf[3] = (uint8_t)((QTEST_SRC_I2C_ADDR << 1) | 1);
    memcpy(&pec_buf[4], mctp_payload, MCTP_LEN);
    uint8_t pec = smbus_pec(pec_buf, PEC_INPUT_LEN);

    /* Body to write into R_BUF (everything after the dest byte). */
    uint8_t body[BODY_LEN];
    body[0] = MCTP_I2C_CMD;
    body[1] = (uint8_t)(MCTP_LEN + 1);
    body[2] = (uint8_t)((QTEST_SRC_I2C_ADDR << 1) | 1);
    memcpy(&body[3], mctp_payload, MCTP_LEN);
    body[3 + MCTP_LEN] = pec;

    /*
     * Diagnostic: confirm the qtest-feature observer actually wired up.
     * mctp_loop sets R_READY=1 immediately before entering the polling
     * loop. If this fires, the qtest Cargo feature is active AND the
     * scratchpad MMIO grant is honoured.
     */
    uint32_t ready = qtest_readl(s, QTEST_CTRL_BASE + QTEST_CTRL_R_READY);
    g_test_message("R_READY before send = %u (1 = qtest_obs active)", ready);

    /*
     * Snapshot pre-send counters so we can attribute deltas to this
     * specific GetVersion frame. spdm_err is expected to be high (the
     * loop polls the responder on every iteration).
     */
    uint32_t pre_i2c_pkt     = read_counter(s, COUNTER_I2C_PKT);
    uint32_t pre_decode_err  = read_counter(s, COUNTER_DECODE_ERR);
    uint32_t pre_inbound_err = read_counter(s, COUNTER_INBOUND_ERR);
    uint32_t pre_spdm_ok     = read_counter(s, COUNTER_SPDM_OK);

    drive_write(s, FW_OWN_I2C_ADDR, body, BODY_LEN);

    /*
     * Status check: the test-master device sets STATUS.NACK if the slave
     * NACKed any byte (including the address phase). NACK clear ⇒ the
     * firmware's slave at 0x10 is bound and accepted the full frame.
     */
    uint32_t status = qtest_readl(s,
        TEST_MASTER_BUS2_BASE + I2C_TEST_MASTER_R_STATUS);
    g_assert_cmphex(status & I2C_TEST_MASTER_STATUS_NACK, ==, 0);

    /*
     * Give the firmware enough wallclock to:
     *   slave_wait_event returns -> wait_for_messages returns Ok ->
     *   MctpI2cReceiver::decode -> Server::inbound -> SPDM listener ->
     *   responder_process_message -> outbound I2cSender -> next
     *   snapshot tick (every 256 loop iterations).
     *
     * Each loop iteration includes a wait_for_messages call with a 10 000-
     * slave-poll budget; under -accel tcg one full iteration can take
     * tens of ms of host time, so 256 iterations need a few seconds.
     */
    g_usleep(5000000);

    uint32_t post_i2c_pkt     = read_counter(s, COUNTER_I2C_PKT);
    uint32_t post_decode_err  = read_counter(s, COUNTER_DECODE_ERR);
    uint32_t post_inbound_err = read_counter(s, COUNTER_INBOUND_ERR);
    uint32_t post_spdm_ok     = read_counter(s, COUNTER_SPDM_OK);

    g_test_message(
        "counters delta:  i2c_pkt %u->%u  decode_err %u->%u  "
        "inbound_err %u->%u  spdm_ok %u->%u",
        pre_i2c_pkt, post_i2c_pkt,
        pre_decode_err, post_decode_err,
        pre_inbound_err, post_inbound_err,
        pre_spdm_ok, post_spdm_ok);

    /* Layered diagnosis -- each assertion isolates one stage. */
    g_assert_cmpuint(post_decode_err - pre_decode_err, ==, 0);
    g_assert_cmpuint(post_i2c_pkt - pre_i2c_pkt, >=, 1);
    g_assert_cmpuint(post_inbound_err - pre_inbound_err, ==, 0);
    g_assert_cmpuint(post_spdm_ok - pre_spdm_ok, >=, 1);

    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ast1060/mctp/getversion_smoke", test_getversion_smoke);
    return g_test_run();
}
