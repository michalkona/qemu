/*
 * ARM mach-virt emulation
 *
 * Copyright (c) 2013 Linaro Limited
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Emulate a virtual board which works by passing Linux all the information
 * it needs about what devices are present via the device tree.
 * There are some restrictions about what we can do here:
 *  + we can only present devices whose Linux drivers will work based
 *    purely on the device tree with no platform data at all
 *  + we want to present a very stripped-down minimalist platform,
 *    both because this reduces the security attack surface from the guest
 *    and also because it reduces our exposure to being broken when
 *    the kernel updates its device tree bindings and requires further
 *    information in a device binding that we aren't providing.
 * This is essentially the same approach kvmtool uses.
 */

#include "hw/sysbus.h"
#include "hw/arm/arm.h"
#include "hw/arm/primecell.h"
#include "hw/devices.h"
#include "net/net.h"
#include "sysemu/block-backend.h"
#include "sysemu/device_tree.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "exec/address-spaces.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"

#define NUM_VIRTIO_TRANSPORTS 32

/* Number of external interrupt lines to configure the GIC with */
#define NUM_IRQS 128

#define GIC_FDT_IRQ_TYPE_SPI 0
#define GIC_FDT_IRQ_TYPE_PPI 1

#define GIC_FDT_IRQ_FLAGS_EDGE_LO_HI 1
#define GIC_FDT_IRQ_FLAGS_EDGE_HI_LO 2
#define GIC_FDT_IRQ_FLAGS_LEVEL_HI 4
#define GIC_FDT_IRQ_FLAGS_LEVEL_LO 8

#define GIC_FDT_IRQ_PPI_CPU_START 8
#define GIC_FDT_IRQ_PPI_CPU_WIDTH 8

enum {
    VIRT_FLASH,
    VIRT_MEM,
    VIRT_CPUPERIPHS,
    VIRT_GIC_DIST,
    VIRT_GIC_CPU,
    VIRT_GIC_VIC,
    VIRT_GIC_VIC_ALIAS,
    VIRT_GIC_VCPU,
    VIRT_UART,
    VIRT_MMIO,
    VIRT_RTC,
    VIRT_FW_CFG,
};

typedef struct MemMapEntry {
    hwaddr base;
    hwaddr size;
} MemMapEntry;

typedef struct VirtBoardInfo {
    struct arm_boot_info bootinfo;
    const char *cpu_model;
    const MemMapEntry *memmap;
    const int *irqmap;
    int smp_cpus;
    void *fdt;
    int fdt_size;
    uint32_t clock_phandle;
} VirtBoardInfo;

typedef struct {
    MachineClass parent;
    VirtBoardInfo *daughterboard;
} VirtMachineClass;

typedef struct {
    MachineState parent;
    bool secure;
} VirtMachineState;

#define TYPE_VIRT_MACHINE   "virt"
#define VIRT_MACHINE(obj) \
    OBJECT_CHECK(VirtMachineState, (obj), TYPE_VIRT_MACHINE)
#define VIRT_MACHINE_GET_CLASS(obj) \
    OBJECT_GET_CLASS(VirtMachineClass, obj, TYPE_VIRT_MACHINE)
#define VIRT_MACHINE_CLASS(klass) \
    OBJECT_CLASS_CHECK(VirtMachineClass, klass, TYPE_VIRT_MACHINE)

/* Addresses and sizes of our components.
 * 0..128MB is space for a flash device so we can run bootrom code such as UEFI.
 * 128MB..256MB is used for miscellaneous device I/O.
 * 256MB..1GB is reserved for possible future PCI support (ie where the
 * PCI memory window will go if we add a PCI host controller).
 * 1GB and up is RAM (which may happily spill over into the
 * high memory region beyond 4GB).
 * This represents a compromise between how much RAM can be given to
 * a 32 bit VM and leaving space for expansion and in particular for PCI.
 * Note that devices should generally be placed at multiples of 0x10000,
 * to accommodate guests using 64K pages.
 */
static const MemMapEntry a15memmap[] = {
    /* Space up to 0x8000000 is reserved for a boot ROM */
    [VIRT_FLASH] =      {          0, 0x08000000 },
    [VIRT_CPUPERIPHS] = { 0x08000000, 0x00020000 },
    /* GIC distributor and CPU interfaces sit inside the CPU peripheral space */
    [VIRT_GIC_DIST] =   { 0x08000000, 0x00010000 },
    [VIRT_GIC_CPU] =    { 0x08010000, 0x00010000 },
    [VIRT_GIC_VIC] = { 0x8020000, 0x10000 },
    [VIRT_GIC_VIC_ALIAS] = { 0x8020000, 0x80000 },
    [VIRT_GIC_VCPU] = { 0x80a0000, 0x10000 },
    [VIRT_UART] =       { 0x09000000, 0x00001000 },
    [VIRT_RTC] =        { 0x09010000, 0x00001000 },
    [VIRT_FW_CFG] =     { 0x09020000, 0x0000000a },
    [VIRT_MMIO] =       { 0x0a000000, 0x00000200 },
    /* ...repeating for a total of NUM_VIRTIO_TRANSPORTS, each of that size */
    /* 0x10000000 .. 0x40000000 reserved for PCI */
    [VIRT_MEM] =        { 0x40000000, 30ULL * 1024 * 1024 * 1024 },
};

static const int a15irqmap[] = {
    [VIRT_UART] = 1,
    [VIRT_RTC] = 2,
    [VIRT_MMIO] = 16, /* ...to 16 + NUM_VIRTIO_TRANSPORTS - 1 */
};

static VirtBoardInfo machines[] = {
    {
        .cpu_model = "cortex-a15",
        .memmap = a15memmap,
        .irqmap = a15irqmap,
    },
    {
        .cpu_model = "cortex-a57",
        .memmap = a15memmap,
        .irqmap = a15irqmap,
    },
    {
        .cpu_model = "cortex-a57",
        /* Use the A15 private peripheral model for now: probably wrong! */
        .qdevname = "a15mpcore_priv",
        .gic_compatible = "arm,cortex-a15-gic",
        .memmap = a15memmap,
        .irqmap = a15irqmap,
    },
    {
        .cpu_model = "host",
        .memmap = a15memmap,
        .irqmap = a15irqmap,
    },
};

static VirtBoardInfo *find_machine_info(const char *cpu)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(machines); i++) {
        if (strcmp(cpu, machines[i].cpu_model) == 0) {
            return &machines[i];
        }
    }
    return NULL;
}

static void create_fdt(VirtBoardInfo *vbi)
{
    void *fdt = create_device_tree(&vbi->fdt_size);

    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(1);
    }

    vbi->fdt = fdt;

    /* Header */
    qemu_fdt_setprop_string(fdt, "/", "compatible", "linux,dummy-virt");
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 0x2);

    /*
     * /chosen and /memory nodes must exist for load_dtb
     * to fill in necessary properties later
     */
    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_add_subnode(fdt, "/memory");
    qemu_fdt_setprop_string(fdt, "/memory", "device_type", "memory");

    /* Clock node, for the benefit of the UART. The kernel device tree
     * binding documentation claims the PL011 node clock properties are
     * optional but in practice if you omit them the kernel refuses to
     * probe for the device.
     */
    vbi->clock_phandle = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_add_subnode(fdt, "/apb-pclk");
    qemu_fdt_setprop_string(fdt, "/apb-pclk", "compatible", "fixed-clock");
    qemu_fdt_setprop_cell(fdt, "/apb-pclk", "#clock-cells", 0x0);
    qemu_fdt_setprop_cell(fdt, "/apb-pclk", "clock-frequency", 24000000);
    qemu_fdt_setprop_string(fdt, "/apb-pclk", "clock-output-names",
                                "clk24mhz");
    qemu_fdt_setprop_cell(fdt, "/apb-pclk", "phandle", vbi->clock_phandle);

}

static void fdt_add_psci_node(const VirtBoardInfo *vbi)
{
    uint32_t cpu_suspend_fn;
    uint32_t cpu_off_fn;
    uint32_t cpu_on_fn;
    uint32_t migrate_fn;
    void *fdt = vbi->fdt;
    ARMCPU *armcpu = ARM_CPU(qemu_get_cpu(0));

    qemu_fdt_add_subnode(fdt, "/psci");
    if (armcpu->psci_version == 2) {
        const char comp[] = "arm,psci-0.2\0arm,psci";
        qemu_fdt_setprop(fdt, "/psci", "compatible", comp, sizeof(comp));

        cpu_off_fn = QEMU_PSCI_0_2_FN_CPU_OFF;
        if (arm_feature(&armcpu->env, ARM_FEATURE_AARCH64)) {
            cpu_suspend_fn = QEMU_PSCI_0_2_FN64_CPU_SUSPEND;
            cpu_on_fn = QEMU_PSCI_0_2_FN64_CPU_ON;
            migrate_fn = QEMU_PSCI_0_2_FN64_MIGRATE;
        } else {
            cpu_suspend_fn = QEMU_PSCI_0_2_FN_CPU_SUSPEND;
            cpu_on_fn = QEMU_PSCI_0_2_FN_CPU_ON;
            migrate_fn = QEMU_PSCI_0_2_FN_MIGRATE;
        }
    } else {
        qemu_fdt_setprop_string(fdt, "/psci", "compatible", "arm,psci");

        cpu_suspend_fn = QEMU_PSCI_0_1_FN_CPU_SUSPEND;
        cpu_off_fn = QEMU_PSCI_0_1_FN_CPU_OFF;
        cpu_on_fn = QEMU_PSCI_0_1_FN_CPU_ON;
        migrate_fn = QEMU_PSCI_0_1_FN_MIGRATE;
    }

    /* We adopt the PSCI spec's nomenclature, and use 'conduit' to refer
     * to the instruction that should be used to invoke PSCI functions.
     * However, the device tree binding uses 'method' instead, so that is
     * what we should use here.
     */
    qemu_fdt_setprop_string(fdt, "/psci", "method", "hvc");

    qemu_fdt_setprop_cell(fdt, "/psci", "cpu_suspend", cpu_suspend_fn);
    qemu_fdt_setprop_cell(fdt, "/psci", "cpu_off", cpu_off_fn);
    qemu_fdt_setprop_cell(fdt, "/psci", "cpu_on", cpu_on_fn);
    qemu_fdt_setprop_cell(fdt, "/psci", "migrate", migrate_fn);
}

static void fdt_add_timer_nodes(const VirtBoardInfo *vbi)
{
    /* Note that on A15 h/w these interrupts are level-triggered,
     * but for the GIC implementation provided by both QEMU and KVM
     * they are edge-triggered.
     */
    ARMCPU *armcpu;
    uint32_t irqflags = GIC_FDT_IRQ_FLAGS_EDGE_LO_HI;

    irqflags = deposit32(irqflags, GIC_FDT_IRQ_PPI_CPU_START,
                         GIC_FDT_IRQ_PPI_CPU_WIDTH, (1 << vbi->smp_cpus) - 1);

    qemu_fdt_add_subnode(vbi->fdt, "/timer");

    armcpu = ARM_CPU(qemu_get_cpu(0));
    if (arm_feature(&armcpu->env, ARM_FEATURE_V8)) {
        const char compat[] = "arm,armv8-timer\0arm,armv7-timer";
        qemu_fdt_setprop(vbi->fdt, "/timer", "compatible",
                         compat, sizeof(compat));
    } else {
        qemu_fdt_setprop_string(vbi->fdt, "/timer", "compatible",
                                "arm,armv7-timer");
    }
    qemu_fdt_setprop_cells(vbi->fdt, "/timer", "interrupts",
                               GIC_FDT_IRQ_TYPE_PPI, 13, irqflags,
                               GIC_FDT_IRQ_TYPE_PPI, 14, irqflags,
                               GIC_FDT_IRQ_TYPE_PPI, 11, irqflags,
                               GIC_FDT_IRQ_TYPE_PPI, 10, irqflags);
}

static void fdt_add_cpu_nodes(const VirtBoardInfo *vbi)
{
    int cpu;

    qemu_fdt_add_subnode(vbi->fdt, "/cpus");
    qemu_fdt_setprop_cell(vbi->fdt, "/cpus", "#address-cells", 0x1);
    qemu_fdt_setprop_cell(vbi->fdt, "/cpus", "#size-cells", 0x0);

    for (cpu = vbi->smp_cpus - 1; cpu >= 0; cpu--) {
        char *nodename = g_strdup_printf("/cpus/cpu@%d", cpu);
        ARMCPU *armcpu = ARM_CPU(qemu_get_cpu(cpu));

        qemu_fdt_add_subnode(vbi->fdt, nodename);
        qemu_fdt_setprop_string(vbi->fdt, nodename, "device_type", "cpu");
        qemu_fdt_setprop_string(vbi->fdt, nodename, "compatible",
                                    armcpu->dtb_compatible);

        if (vbi->smp_cpus > 1) {
            qemu_fdt_setprop_string(vbi->fdt, nodename,
                                        "enable-method", "psci");
        }

        qemu_fdt_setprop_cell(vbi->fdt, nodename, "reg", armcpu->mp_affinity);
        g_free(nodename);
    }
}

static void fdt_add_gic_node(const VirtBoardInfo *vbi)
{
    uint32_t gic_phandle;
    uint32_t irqflags = GIC_FDT_IRQ_FLAGS_LEVEL_HI;

    irqflags = deposit32(irqflags, GIC_FDT_IRQ_PPI_CPU_START,
                         GIC_FDT_IRQ_PPI_CPU_WIDTH, (1 << vbi->smp_cpus) - 1);

    gic_phandle = qemu_fdt_alloc_phandle(vbi->fdt);
    qemu_fdt_setprop_cell(vbi->fdt, "/", "interrupt-parent", gic_phandle);

    qemu_fdt_add_subnode(vbi->fdt, "/intc");
    /* 'cortex-a15-gic' means 'GIC v2' */
    qemu_fdt_setprop_string(vbi->fdt, "/intc", "compatible",
                            "arm,cortex-a15-gic");
    qemu_fdt_setprop_cell(vbi->fdt, "/intc", "#interrupt-cells", 3);
    qemu_fdt_setprop(vbi->fdt, "/intc", "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_sized_cells(vbi->fdt, "/intc", "reg",
                                     2, vbi->memmap[VIRT_GIC_DIST].base,
                                     2, vbi->memmap[VIRT_GIC_DIST].size,
                                     2, vbi->memmap[VIRT_GIC_CPU].base,
                                     2, vbi->memmap[VIRT_GIC_CPU].size,
                                     2, vbi->memmap[VIRT_GIC_VIC].base,
                                     2, vbi->memmap[VIRT_GIC_VIC].size,
                                     2, vbi->memmap[VIRT_GIC_VCPU].base,
                                     2, vbi->memmap[VIRT_GIC_VCPU].size);
    qemu_fdt_setprop_cell(vbi->fdt, "/intc", "phandle", gic_phandle);
    qemu_fdt_setprop_cells(vbi->fdt, "/intc", "interrupts",
                               GIC_FDT_IRQ_TYPE_PPI, 9, irqflags);
}

static void create_gic(const VirtBoardInfo *vbi, qemu_irq *pic)
{
    /* We create a standalone GIC v2 */
    DeviceState *gicdev;
    SysBusDevice *gicbusdev;
    const char *gictype = "arm_gic";
    int hregion = (smp_cpus + 1) + 1;
    int vregion = (smp_cpus + 1) * 2 + 1;
    int i;

    if (kvm_irqchip_in_kernel()) {
        gictype = "kvm-arm-gic";
    }

    gicdev = qdev_create(NULL, gictype);
    qdev_prop_set_uint32(gicdev, "revision", 2);
    qdev_prop_set_uint32(gicdev, "num-cpu", smp_cpus);
    /* Note that the num-irq property counts both internal and external
     * interrupts; there are always 32 of the former (mandated by GIC spec).
     */
    qdev_prop_set_uint32(gicdev, "num-irq", NUM_IRQS + 32);
    qdev_init_nofail(gicdev);
    gicbusdev = SYS_BUS_DEVICE(gicdev);
    sysbus_mmio_map(gicbusdev, 0, vbi->memmap[VIRT_GIC_DIST].base);
    sysbus_mmio_map(gicbusdev, 1, vbi->memmap[VIRT_GIC_CPU].base);
    if (!kvm_enabled()) {
        sysbus_mmio_map(gicbusdev, hregion,
                        vbi->memmap[VIRT_GIC_VIC].base);
        sysbus_mmio_map(gicbusdev, vregion,
                        vbi->memmap[VIRT_GIC_VCPU].base);
    }

    /* Wire the outputs from each CPU's generic timer to the
     * appropriate GIC PPI inputs, and the GIC's IRQ output to
     * the CPU's IRQ input.
     */
    for (i = 0; i < smp_cpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        int ppibase = NUM_IRQS + i * 32;
        /* physical timer; we wire it up to the non-secure timer's ID,
         * since a real A15 always has TrustZone but QEMU doesn't.
         */
        qdev_connect_gpio_out(cpudev, 0,
                              qdev_get_gpio_in(gicdev, ppibase + 30));
        /* virtual timer */
        qdev_connect_gpio_out(cpudev, 1,
                              qdev_get_gpio_in(gicdev, ppibase + 27));
        sysbus_connect_irq(gicbusdev, i, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, smp_cpus + i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        /* Hypervisor timer.  */
        qdev_connect_gpio_out(cpudev, 2,
                              qdev_get_gpio_in(gicdev, ppibase + 26));
        /* Maintenance interrupt.  */
        sysbus_connect_irq(gicbusdev, smp_cpus * 2 + i,
                           qdev_get_gpio_in(gicdev, ppibase + 25));
        if (!kvm_enabled()) {
            /* Per CPU Virtual Interface Control.  */
            sysbus_mmio_map(gicbusdev, hregion + 1 + i,
                            vbi->memmap[VIRT_GIC_VIC_ALIAS].base + i * 0x10000);
        }
    }

    for (i = 0; i < NUM_IRQS; i++) {
        pic[i] = qdev_get_gpio_in(gicdev, i);
    }

    fdt_add_gic_node(vbi);
}

static void create_uart(const VirtBoardInfo *vbi, qemu_irq *pic)
{
    char *nodename;
    hwaddr base = vbi->memmap[VIRT_UART].base;
    hwaddr size = vbi->memmap[VIRT_UART].size;
    int irq = vbi->irqmap[VIRT_UART];
    const char compat[] = "arm,pl011\0arm,primecell";
    const char clocknames[] = "uartclk\0apb_pclk";

    sysbus_create_simple("pl011", base, pic[irq]);

    nodename = g_strdup_printf("/pl011@%" PRIx64, base);
    qemu_fdt_add_subnode(vbi->fdt, nodename);
    /* Note that we can't use setprop_string because of the embedded NUL */
    qemu_fdt_setprop(vbi->fdt, nodename, "compatible",
                         compat, sizeof(compat));
    qemu_fdt_setprop_sized_cells(vbi->fdt, nodename, "reg",
                                     2, base, 2, size);
    qemu_fdt_setprop_cells(vbi->fdt, nodename, "interrupts",
                               GIC_FDT_IRQ_TYPE_SPI, irq,
                               GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cells(vbi->fdt, nodename, "clocks",
                               vbi->clock_phandle, vbi->clock_phandle);
    qemu_fdt_setprop(vbi->fdt, nodename, "clock-names",
                         clocknames, sizeof(clocknames));

    qemu_fdt_setprop_string(vbi->fdt, "/chosen", "stdout-path", nodename);
    g_free(nodename);
}

static void create_rtc(const VirtBoardInfo *vbi, qemu_irq *pic)
{
    char *nodename;
    hwaddr base = vbi->memmap[VIRT_RTC].base;
    hwaddr size = vbi->memmap[VIRT_RTC].size;
    int irq = vbi->irqmap[VIRT_RTC];
    const char compat[] = "arm,pl031\0arm,primecell";

    sysbus_create_simple("pl031", base, pic[irq]);

    nodename = g_strdup_printf("/pl031@%" PRIx64, base);
    qemu_fdt_add_subnode(vbi->fdt, nodename);
    qemu_fdt_setprop(vbi->fdt, nodename, "compatible", compat, sizeof(compat));
    qemu_fdt_setprop_sized_cells(vbi->fdt, nodename, "reg",
                                 2, base, 2, size);
    qemu_fdt_setprop_cells(vbi->fdt, nodename, "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, irq,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cell(vbi->fdt, nodename, "clocks", vbi->clock_phandle);
    qemu_fdt_setprop_string(vbi->fdt, nodename, "clock-names", "apb_pclk");
    g_free(nodename);
}

static void create_virtio_devices(const VirtBoardInfo *vbi, qemu_irq *pic)
{
    int i;
    hwaddr size = vbi->memmap[VIRT_MMIO].size;

    /* Note that we have to create the transports in forwards order
     * so that command line devices are inserted lowest address first,
     * and then add dtb nodes in reverse order so that they appear in
     * the finished device tree lowest address first.
     */
    for (i = 0; i < NUM_VIRTIO_TRANSPORTS; i++) {
        int irq = vbi->irqmap[VIRT_MMIO] + i;
        hwaddr base = vbi->memmap[VIRT_MMIO].base + i * size;

        sysbus_create_simple("virtio-mmio", base, pic[irq]);
    }

    for (i = NUM_VIRTIO_TRANSPORTS - 1; i >= 0; i--) {
        char *nodename;
        int irq = vbi->irqmap[VIRT_MMIO] + i;
        hwaddr base = vbi->memmap[VIRT_MMIO].base + i * size;

        nodename = g_strdup_printf("/virtio_mmio@%" PRIx64, base);
        qemu_fdt_add_subnode(vbi->fdt, nodename);
        qemu_fdt_setprop_string(vbi->fdt, nodename,
                                "compatible", "virtio,mmio");
        qemu_fdt_setprop_sized_cells(vbi->fdt, nodename, "reg",
                                     2, base, 2, size);
        qemu_fdt_setprop_cells(vbi->fdt, nodename, "interrupts",
                               GIC_FDT_IRQ_TYPE_SPI, irq,
                               GIC_FDT_IRQ_FLAGS_EDGE_LO_HI);
        g_free(nodename);
    }
}

static void create_one_flash(const char *name, hwaddr flashbase,
                             hwaddr flashsize)
{
    /* Create and map a single flash device. We use the same
     * parameters as the flash devices on the Versatile Express board.
     */
    DriveInfo *dinfo = drive_get_next(IF_PFLASH);
    DeviceState *dev = qdev_create(NULL, "cfi.pflash01");
    const uint64_t sectorlength = 256 * 1024;

    if (dinfo && qdev_prop_set_drive(dev, "drive",
                                     blk_by_legacy_dinfo(dinfo))) {
        abort();
    }

    qdev_prop_set_uint32(dev, "num-blocks", flashsize / sectorlength);
    qdev_prop_set_uint64(dev, "sector-length", sectorlength);
    qdev_prop_set_uint8(dev, "width", 4);
    qdev_prop_set_uint8(dev, "device-width", 2);
    qdev_prop_set_uint8(dev, "big-endian", 0);
    qdev_prop_set_uint16(dev, "id0", 0x89);
    qdev_prop_set_uint16(dev, "id1", 0x18);
    qdev_prop_set_uint16(dev, "id2", 0x00);
    qdev_prop_set_uint16(dev, "id3", 0x00);
    qdev_prop_set_string(dev, "name", name);
    qdev_init_nofail(dev);

    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, flashbase);
}

static void create_flash(const VirtBoardInfo *vbi)
{
    /* Create two flash devices to fill the VIRT_FLASH space in the memmap.
     * Any file passed via -bios goes in the first of these.
     */
    hwaddr flashsize = vbi->memmap[VIRT_FLASH].size / 2;
    hwaddr flashbase = vbi->memmap[VIRT_FLASH].base;
    char *nodename;

    if (bios_name) {
        const char *fn;

        if (drive_get(IF_PFLASH, 0, 0)) {
            error_report("The contents of the first flash device may be "
                         "specified with -bios or with -drive if=pflash... "
                         "but you cannot use both options at once");
            exit(1);
        }
        fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
        if (!fn || load_image_targphys(fn, flashbase, flashsize) < 0) {
            error_report("Could not load ROM image '%s'", bios_name);
            exit(1);
        }
    }

    create_one_flash("virt.flash0", flashbase, flashsize);
    create_one_flash("virt.flash1", flashbase + flashsize, flashsize);

    nodename = g_strdup_printf("/flash@%" PRIx64, flashbase);
    qemu_fdt_add_subnode(vbi->fdt, nodename);
    qemu_fdt_setprop_string(vbi->fdt, nodename, "compatible", "cfi-flash");
    qemu_fdt_setprop_sized_cells(vbi->fdt, nodename, "reg",
                                 2, flashbase, 2, flashsize,
                                 2, flashbase + flashsize, 2, flashsize);
    qemu_fdt_setprop_cell(vbi->fdt, nodename, "bank-width", 4);
    g_free(nodename);
}

static void create_fw_cfg(const VirtBoardInfo *vbi)
{
    hwaddr base = vbi->memmap[VIRT_FW_CFG].base;
    hwaddr size = vbi->memmap[VIRT_FW_CFG].size;
    char *nodename;

    fw_cfg_init_mem_wide(base + 8, base, 8);

    nodename = g_strdup_printf("/fw-cfg@%" PRIx64, base);
    qemu_fdt_add_subnode(vbi->fdt, nodename);
    qemu_fdt_setprop_string(vbi->fdt, nodename,
                            "compatible", "qemu,fw-cfg-mmio");
    qemu_fdt_setprop_sized_cells(vbi->fdt, nodename, "reg",
                                 2, base, 2, size);
    g_free(nodename);
}

static void *machvirt_dtb(const struct arm_boot_info *binfo, int *fdt_size)
{
    const VirtBoardInfo *board = (const VirtBoardInfo *)binfo;

    *fdt_size = board->fdt_size;
    return board->fdt;
}

static void machvirt_init(MachineState *machine)
{
    VirtMachineState *vms = VIRT_MACHINE(machine);
    qemu_irq pic[NUM_IRQS];
    MemoryRegion *sysmem = get_system_memory();
    int n;
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    const char *cpu_model = machine->cpu_model;
    VirtBoardInfo *vbi;

    if (!cpu_model) {
        cpu_model = "cortex-a15";
    }

    vbi = find_machine_info(cpu_model);

    if (!vbi) {
        error_report("mach-virt: CPU %s not supported", cpu_model);
        exit(1);
    }

    vbi->smp_cpus = smp_cpus;

    if (machine->ram_size > vbi->memmap[VIRT_MEM].size) {
        error_report("mach-virt: cannot model more than 30GB RAM");
        exit(1);
    }

    create_fdt(vbi);

    for (n = 0; n < smp_cpus; n++) {
        ObjectClass *oc = cpu_class_by_name(TYPE_ARM_CPU, cpu_model);
        Object *cpuobj;

        if (!oc) {
            fprintf(stderr, "Unable to find CPU definition\n");
            exit(1);
        }
        cpuobj = object_new(object_class_get_name(oc));

        if (!vms->secure) {
            object_property_set_bool(cpuobj, false, "has_el3", NULL);
        }

        object_property_set_int(cpuobj, QEMU_PSCI_CONDUIT_HVC, "psci-conduit",
                                NULL);

        /* Secondary CPUs start in PSCI powered-down state */
        if (n > 0) {
            object_property_set_bool(cpuobj, true, "start-powered-off", NULL);
        }

        if (object_property_find(cpuobj, "reset-cbar", NULL)) {
            object_property_set_int(cpuobj, vbi->memmap[VIRT_CPUPERIPHS].base,
                                    "reset-cbar", &error_abort);
        }

        object_property_set_bool(cpuobj, true, "realized", NULL);
    }
    fdt_add_timer_nodes(vbi);
    fdt_add_cpu_nodes(vbi);
    fdt_add_psci_node(vbi);

    memory_region_init_ram(ram, NULL, "mach-virt.ram", machine->ram_size,
                           &error_abort);
    vmstate_register_ram_global(ram);
    memory_region_add_subregion(sysmem, vbi->memmap[VIRT_MEM].base, ram);

    create_flash(vbi);

    create_gic(vbi, pic);

    create_uart(vbi, pic);

    create_rtc(vbi, pic);

    /* Create mmio transports, so the user can create virtio backends
     * (which will be automatically plugged in to the transports). If
     * no backend is created the transport will just sit harmlessly idle.
     */
    create_virtio_devices(vbi, pic);

    create_fw_cfg(vbi);

    vbi->bootinfo.ram_size = machine->ram_size;
    vbi->bootinfo.kernel_filename = machine->kernel_filename;
    vbi->bootinfo.kernel_cmdline = machine->kernel_cmdline;
    vbi->bootinfo.initrd_filename = machine->initrd_filename;
    vbi->bootinfo.nb_cpus = smp_cpus;
    vbi->bootinfo.board_id = -1;
    vbi->bootinfo.loader_start = vbi->memmap[VIRT_MEM].base;
    vbi->bootinfo.get_dtb = machvirt_dtb;
    vbi->bootinfo.firmware_loaded = bios_name || drive_get(IF_PFLASH, 0, 0);
    arm_load_kernel(ARM_CPU(first_cpu), &vbi->bootinfo);
}

static bool virt_get_secure(Object *obj, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    return vms->secure;
}

static void virt_set_secure(Object *obj, bool value, Error **errp)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    vms->secure = value;
}

static void virt_instance_init(Object *obj)
{
    VirtMachineState *vms = VIRT_MACHINE(obj);

    /* EL3 is enabled by default on virt */
    vms->secure = true;
    object_property_add_bool(obj, "secure", virt_get_secure,
                             virt_set_secure, NULL);
    object_property_set_description(obj, "secure",
                                    "Set on/off to enable/disable the ARM "
                                    "Security Extensions (TrustZone)",
                                    NULL);
}

static void virt_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->name = TYPE_VIRT_MACHINE;
    mc->desc = "ARM Virtual Machine",
    mc->init = machvirt_init;
    mc->max_cpus = 8;
}

static const TypeInfo machvirt_info = {
    .name = TYPE_VIRT_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(VirtMachineState),
    .instance_init = virt_instance_init,
    .class_size = sizeof(VirtMachineClass),
    .class_init = virt_class_init,
};

static void machvirt_machine_init(void)
{
    type_register_static(&machvirt_info);
}

machine_init(machvirt_machine_init);
