#include <autoconf.h>
#include <elfloader/gen_config.h>
#include <elfloader_common.h>

#include <devices_gen.h>
#include <drivers/common.h>
#include <drivers/smp.h>

#include <printf.h>
#include <armv/machine.h>
#include <abort.h>

#define REG(base, offs) ((volatile word_t *)(((uintptr_t)base) + (offs)))

static int smp_bcm2711_cpu_on(UNUSED struct elfloader_device *dev,
                           UNUSED struct elfloader_cpu *cpu, UNUSED void *entry, UNUSED void *stack)
{
#if CONFIG_MAX_NUM_NODES > 1
    volatile void *mmio = dev->region_bases[0];
    secondary_data.entry = entry;
    secondary_data.stack = stack;
    *REG(mmio, 0xd8 + (cpu->cpu_id * 8)) = (word_t)secondary_startup;
    dsb();
    return 0;
#else
    return -1;
#endif
}

static int smp_bcm2711_init(UNUSED struct elfloader_device *dev,
                         UNUSED void *match_data)
{
#if CONFIG_MAX_NUM_NODES > 1
    smp_register_handler(dev);
#endif
    return 0;
}

static const struct dtb_match_table smp_bcm2711_matches[] = {
    { .compatible = "arm,cortex-a72" },
    { .compatible = NULL /* sentinel */ },
};

static const struct elfloader_smp_ops smp_bcm2711_ops = {
    .enable_method = "spin-table",
    .cpu_on = &smp_bcm2711_cpu_on,
};

static const struct elfloader_driver smp_bcm2711 = {
    .match_table = smp_bcm2711_matches,
    .type = DRIVER_SMP,
    .init = &smp_bcm2711_init,
    .ops = &smp_bcm2711_ops,
};

ELFLOADER_DRIVER(smp_bcm2711);