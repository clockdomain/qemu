/*
 * ASPEED AST10x0 EVB
 *
 * Copyright 2016 IBM Corp.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/aspeed.h"
#include "hw/arm/aspeed_soc.h"
#include "hw/core/qdev-clock.h"
#include "system/system.h"
#include "hw/i2c/smbus_eeprom.h"
#include "hw/i2c/i2c-test-master.h"
#include "hw/misc/aspeed-qtest-ctrl.h"
#include "hw/sensor/tmp105.h"
#include "hw/sensor/isl_pmbus_vr.h"

#define AST1030_INTERNAL_FLASH_SIZE (1024 * 1024)
/* Main SYSCLK frequency in Hz (200MHz) */
#define SYSCLK_FRQ 200000000ULL

static void aspeed_minibmc_machine_init(MachineState *machine)
{
    AspeedMachineState *bmc = ASPEED_MACHINE(machine);
    AspeedMachineClass *amc = ASPEED_MACHINE_GET_CLASS(machine);
    Clock *sysclk;

    sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(sysclk, SYSCLK_FRQ);

    bmc->soc = ASPEED_SOC(object_new(amc->soc_name));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(bmc->soc));
    object_unref(OBJECT(bmc->soc));
    qdev_connect_clock_in(DEVICE(bmc->soc), "sysclk", sysclk);

    object_property_set_link(OBJECT(bmc->soc), "memory",
                             OBJECT(get_system_memory()), &error_abort);
    aspeed_connect_serial_hds_to_uarts(bmc);
    qdev_realize(DEVICE(bmc->soc), NULL, &error_abort);

    if (defaults_enabled()) {
        aspeed_board_init_flashes(&bmc->soc->fmc,
                            bmc->fmc_model ? bmc->fmc_model : amc->fmc_model,
                            amc->num_cs,
                            0);

        aspeed_board_init_flashes(&bmc->soc->spi[0],
                            bmc->spi_model ? bmc->spi_model : amc->spi_model,
                            amc->num_cs, amc->num_cs);

        aspeed_board_init_flashes(&bmc->soc->spi[1],
                            bmc->spi_model ? bmc->spi_model : amc->spi_model,
                            amc->num_cs, (amc->num_cs * 2));
    }

    if (amc->i2c_init) {
        amc->i2c_init(bmc);
    }

    armv7m_load_kernel(ARM_CPU(first_cpu),
                       machine->kernel_filename,
                       0,
                       AST1030_INTERNAL_FLASH_SIZE);
}

static void ast1030_evb_i2c_init(AspeedMachineState *bmc)
{
    AspeedSoCState *soc = bmc->soc;

    /*
     * The AST1030 MiniBMC EVB exposes 14 I2C buses. The layout below
     * follows the reference schematic and matches the slave layout that
     * Aspeed's Zephyr SDK probes at boot:
     *
     *   Bus 0 : U10 24C08 EEPROM at 0x50
     *   Bus 1 : U11 LM75 (TMP105-compatible) at 0x4d
     *   Bus 2 : PCA9552 16-bit LED/GPIO expander at 0x60
     *   Bus 3 : second 24C08 EEPROM at 0x51 (expansion header)
     *   Bus 4 : PCA9554 8-bit IO expander at 0x20
     *   Bus 5 : spare TMP105 at 0x48 (optional daughter-card sensor)
     */

    /* Bus 0: primary configuration EEPROM */
    uint8_t *eeprom0_buf = g_malloc0(32 * 1024);
    smbus_eeprom_init_one(aspeed_i2c_get_bus(&soc->i2c, 0), 0x50, eeprom0_buf);

    /* Bus 1: on-board temperature sensor */
    i2c_slave_create_simple(aspeed_i2c_get_bus(&soc->i2c, 1), TYPE_TMP105,
                            0x4d);

    /* Bus 2: LED/GPIO expander */
    aspeed_create_pca9552(soc, 2, 0x60);

    /* Bus 3: expansion EEPROM */
    uint8_t *eeprom1_buf = g_malloc0(32 * 1024);
    smbus_eeprom_init_one(aspeed_i2c_get_bus(&soc->i2c, 3), 0x51, eeprom1_buf);

    /* Bus 4: general-purpose IO expander */
    aspeed_create_pca9554(soc, 4, 0x20);

    /* Bus 5: optional secondary temperature sensor */
    i2c_slave_create_simple(aspeed_i2c_get_bus(&soc->i2c, 5), TYPE_TMP105,
                            0x48);

    /*
     * qtest helpers. `i2c-test-master` at 0x7E7C_0000 is pinned to bus 0,
     * used by aspeed_i2c-slave-test.c for slave-mode DMA TX coverage.
     * `aspeed-qtest-ctrl` at 0x7E7D_0000 is a passive scratchpad used by
     * the i2c_server qtest (currently only targets ast1060-evb, but we
     * instantiate the control region here too for parity).
     */
    i2c_test_master_create(0x7E7C0000,
                           aspeed_i2c_get_bus(&soc->i2c, 0));
    aspeed_qtest_ctrl_create(0x7E7D0000);
}

static void ast1060_evb_i2c_init(AspeedMachineState *bmc)
{
    AspeedSoCState *soc = bmc->soc;

    /*
     * The AST1060 Platform Root of Trust EVB also exposes 14 I2C buses
     * via the shared AST10x0 SoC model. The layout here reflects the
     * reference schematic - a small mix of sensors, EEPROMs, IO
     * expanders, and a PMBus voltage regulator that firmware probes
     * during attestation/telemetry bring-up.
     *
     *   Bus 0 : 24C08 EEPROM              @ 0x50   (config EEPROM)
     *   Bus 1 : TMP105                    @ 0x4c   (core temperature)
     *   Bus 2 : PCA9552                   @ 0x60   (status LEDs)
     *   Bus 3 : PCA9554                   @ 0x20   (miscellaneous GPIO)
     *   Bus 4 : ISL69259 PMBus VR         @ 0x60   (core voltage rail)
     *   Bus 5 : 24C08 EEPROM              @ 0x51   (PRoT secure store)
     */

    uint8_t *eeprom0_buf = g_malloc0(32 * 1024);
    smbus_eeprom_init_one(aspeed_i2c_get_bus(&soc->i2c, 0), 0x50, eeprom0_buf);

    i2c_slave_create_simple(aspeed_i2c_get_bus(&soc->i2c, 1), TYPE_TMP105,
                            0x4c);

    aspeed_create_pca9552(soc, 2, 0x60);
    aspeed_create_pca9554(soc, 3, 0x20);

    i2c_slave_create_simple(aspeed_i2c_get_bus(&soc->i2c, 4), TYPE_ISL69259,
                            0x60);

    uint8_t *eeprom1_buf = g_malloc0(32 * 1024);
    smbus_eeprom_init_one(aspeed_i2c_get_bus(&soc->i2c, 5), 0x51, eeprom1_buf);

    /*
     * qtest helpers for the i2c_server qtest suite. `i2c-test-master` is
     * pinned to bus 3 so tests can drive transactions into the Aspeed
     * controller configured as a slave there. `aspeed-qtest-ctrl` is a
     * passive scratchpad used by firmware + qtest to coordinate scenario
     * dispatch.
     */
    i2c_test_master_create(0x7E7C0000,
                           aspeed_i2c_get_bus(&soc->i2c, 3));
    aspeed_qtest_ctrl_create(0x7E7D0000);
}

static void aspeed_minibmc_machine_ast1030_evb_class_init(ObjectClass *oc,
                                                          const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AspeedMachineClass *amc = ASPEED_MACHINE_CLASS(oc);

    mc->desc = "Aspeed AST1030 MiniBMC (Cortex-M4)";
    amc->soc_name = "ast1030-a1";
    amc->hw_strap1 = 0;
    amc->hw_strap2 = 0;
    mc->init = aspeed_minibmc_machine_init;
    amc->i2c_init = ast1030_evb_i2c_init;
    mc->default_ram_size = 0;
    amc->fmc_model = "w25q80bl";
    amc->spi_model = "w25q256";
    amc->num_cs = 2;
    amc->macs_mask = 0;
    aspeed_machine_class_init_cpus_defaults(mc);
}

static void aspeed_minibmc_machine_ast1060_evb_class_init(ObjectClass *oc,
                                                          const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AspeedMachineClass *amc = ASPEED_MACHINE_CLASS(oc);

    mc->desc = "Aspeed AST1060 Platform Root of Trust (Cortex-M4)";
    amc->soc_name = "ast1060-a2";
    amc->hw_strap1 = 0;
    amc->hw_strap2 = 0;
    mc->init = aspeed_minibmc_machine_init;
    amc->i2c_init = ast1060_evb_i2c_init;
    amc->fmc_model = "w25q80bl";
    amc->spi_model = "w25q02jvm";
    amc->num_cs = 2;
    amc->macs_mask = 0;
    aspeed_machine_class_init_cpus_defaults(mc);
}

static const TypeInfo aspeed_ast10x0_evb_types[] = {
    {
        .name           = MACHINE_TYPE_NAME("ast1030-evb"),
        .parent         = TYPE_ASPEED_MACHINE,
        .class_init     = aspeed_minibmc_machine_ast1030_evb_class_init,
        .interfaces     = arm_machine_interfaces,
    }, {
        .name           = MACHINE_TYPE_NAME("ast1060-evb"),
        .parent         = TYPE_ASPEED_MACHINE,
        .class_init     = aspeed_minibmc_machine_ast1060_evb_class_init,
        .interfaces     = arm_machine_interfaces,
    }
};

DEFINE_TYPES(aspeed_ast10x0_evb_types)
