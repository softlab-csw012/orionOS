#include "ahci.h"
#include "hal.h"
#include "screen.h"
#include "../mm/paging.h"
#include "../mm/mem.h"
#include "../libc/string.h"

#define AHCI_MAX_CTRLS 4
#define AHCI_MMIO_SIZE 0x2000u
#define AHCI_PORT_BASE 0x100u
#define AHCI_PORT_SIZE 0x80u
#define AHCI_MAX_PORTS 32
#define AHCI_MAX_CMD_SLOTS 32
#define AHCI_MAX_PRDT 32
#define AHCI_MAX_SATA_PORTS (AHCI_MAX_CTRLS * AHCI_MAX_PORTS)

// HBA memory registers (offsets)
#define AHCI_REG_CAP 0x00
#define AHCI_REG_GHC 0x04
#define AHCI_REG_IS  0x08
#define AHCI_REG_PI  0x0C
#define AHCI_REG_VS  0x10

#define AHCI_GHC_AE (1u << 31)

#define AHCI_SIG_ATA   0x00000101u
#define AHCI_SIG_ATAPI 0xEB140101u
#define AHCI_SIG_SEMB  0xC33C0101u
#define AHCI_SIG_PM    0x96690101u

#define HBA_PxIS_TFES (1u << 30)

#define HBA_PxCMD_ST  (1u << 0)
#define HBA_PxCMD_FRE (1u << 4)
#define HBA_PxCMD_FR  (1u << 14)
#define HBA_PxCMD_CR  (1u << 15)

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

#define ATA_CMD_IDENTIFY       0xEC
#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35

#define FIS_TYPE_REG_H2D 0x27

typedef volatile struct {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} hba_port_t;

typedef struct {
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv[4];
} hba_cmd_header_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc;
} hba_prdt_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    hba_prdt_t prdt[AHCI_MAX_PRDT];
} hba_cmd_tbl_t;

typedef struct {
    uint8_t fis_type;
    uint8_t pmport:4;
    uint8_t rsv0:3;
    uint8_t c:1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t rsv1[4];
} fis_reg_h2d_t;

typedef struct ahci_ctrl ahci_ctrl_t;

typedef struct {
    bool present;
    uint8_t port_no;
    hba_port_t* port;
    ahci_ctrl_t* ctrl;
    bool ata_device;
    void* clb;
    uint32_t clb_phys;
    void* fb;
    uint32_t fb_phys;
    void* cmd_tables[AHCI_MAX_CMD_SLOTS];
    uint32_t cmd_tables_phys[AHCI_MAX_CMD_SLOTS];
} ahci_port_state_t;

struct ahci_ctrl {
    uint32_t base;
    volatile uint32_t* regs;
    uint8_t irq_line;
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint32_t cap;
    uint32_t pi;
    uint32_t vs;
    uint32_t cmd_slots;
    ahci_port_state_t ports[AHCI_MAX_PORTS];
    uint32_t port_count;
};

static ahci_ctrl_t g_ahci[AHCI_MAX_CTRLS];
static int g_ahci_count = 0;
static ahci_port_state_t* g_sata_ports[AHCI_MAX_SATA_PORTS];
static uint32_t g_sata_port_count = 0;

static inline void invlpg(uint32_t addr) {
    hal_invlpg((const void*)(uintptr_t)addr);
}

static void map_mmio(uint32_t base, uint32_t size) {
    uint32_t start = base & ~0xFFFu;
    uint32_t end = (base + size + 0xFFFu) & ~0xFFFu;
    for (uint32_t addr = start; addr < end; addr += 0x1000u) {
        vmm_map_page(addr, addr, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
        invlpg(addr);
    }
}

static inline uint32_t ahci_rd32(const ahci_ctrl_t* c, uint32_t off) {
    return c->regs[off / 4];
}

static inline void ahci_wr32(const ahci_ctrl_t* c, uint32_t off, uint32_t v) {
    c->regs[off / 4] = v;
}

static void* ahci_dma_alloc(size_t size, uint32_t* out_phys) {
    size_t alloc = (size + 0xFFFu) & ~0xFFFu;
    void* p = kmalloc(alloc, 1, out_phys);
    if (!p) return NULL;
    memset(p, 0, alloc);
    return p;
}

static inline hba_port_t* ahci_port_ptr(ahci_ctrl_t* c, uint8_t port_no);

static void ahci_register_sata_port(ahci_port_state_t* st) {
    if (!st || !st->ata_device)
        return;
    if (g_sata_port_count >= AHCI_MAX_SATA_PORTS)
        return;
    g_sata_ports[g_sata_port_count++] = st;
}

static ahci_port_state_t* ahci_get_sata_port(uint32_t index) {
    if (index >= g_sata_port_count)
        return NULL;
    return g_sata_ports[index];
}

static const char* ahci_sig_name(uint32_t sig) {
    switch (sig) {
    case AHCI_SIG_ATA:
        return "SATA";
    case AHCI_SIG_ATAPI:
        return "ATAPI";
    case AHCI_SIG_SEMB:
        return "SEMB";
    case AHCI_SIG_PM:
        return "PM";
    default:
        return "UNKNOWN";
    }
}

static const char* ahci_det_name(uint32_t det) {
    switch (det) {
    case 0x0: return "NO_DEVICE";
    case 0x1: return "PRESENT";
    case 0x3: return "PRESENT_COMM";
    default: return "RESERVED";
    }
}

static const char* ahci_ipm_name(uint32_t ipm) {
    switch (ipm) {
    case 0x0: return "NOT_PRESENT";
    case 0x1: return "ACTIVE";
    case 0x2: return "PARTIAL";
    case 0x6: return "SLUMBER";
    default: return "RESERVED";
    }
}

static void ahci_log_port_state(ahci_ctrl_t* c, uint8_t port_no) {
    hba_port_t* p = ahci_port_ptr(c, port_no);
    uint32_t ssts = p->ssts;
    uint32_t det = ssts & 0xFu;
    uint32_t ipm = (ssts >> 8) & 0xFu;
    uint32_t spd = (ssts >> 4) & 0xFu;
    kprintf("[AHCI] port %u det=%s ipm=%s spd=%u sig=%08X (%s)\n",
            port_no,
            ahci_det_name(det),
            ahci_ipm_name(ipm),
            spd,
            p->sig,
            ahci_sig_name(p->sig));
}

static int ahci_find_free_slot(ahci_ctrl_t* c, hba_port_t* p) {
    uint32_t slots = p->sact | p->ci;
    for (uint32_t i = 0; i < c->cmd_slots; i++) {
        if ((slots & (1u << i)) == 0)
            return (int)i;
    }
    return -1;
}

static bool ahci_wait_port_idle(hba_port_t* p) {
    for (uint32_t i = 0; i < 1000000u; i++) {
        uint32_t tfd = p->tfd;
        if ((tfd & (ATA_SR_BSY | ATA_SR_DRQ)) == 0)
            return true;
    }
    return false;
}

static bool ahci_build_prdt(void* buf, uint32_t bytes, hba_prdt_t* prdt, uint16_t* out_prdtl) {
    uint32_t remaining = bytes;
    uintptr_t virt = (uintptr_t)buf;
    uint16_t idx = 0;

    if (bytes == 0)
        return false;

    while (remaining > 0) {
        if (idx >= AHCI_MAX_PRDT) {
            kprintf("[AHCI] PRDT overflow (bytes=%u)\n", bytes);
            return false;
        }

        uint32_t phys;
        if (vmm_virt_to_phys((uint32_t)virt, &phys) != 0)
            phys = (uint32_t)virt;

        uint32_t page_off = phys & 0xFFFu;
        uint32_t chunk = 0x1000u - page_off;
        if (chunk > remaining)
            chunk = remaining;

        prdt[idx].dba = phys;
        prdt[idx].dbau = 0;
        prdt[idx].rsv0 = 0;
        prdt[idx].dbc = (chunk - 1u);

        remaining -= chunk;
        virt += chunk;
        idx++;
    }

    prdt[idx - 1].dbc |= (1u << 31);
    *out_prdtl = idx;
    return true;
}

static bool ahci_exec_cmd(ahci_ctrl_t* c, ahci_port_state_t* st, uint8_t cmd,
                          uint64_t lba, uint16_t count, void* buf,
                          uint32_t bytes, bool write) {
    hba_port_t* p = st->port;
    int slot = ahci_find_free_slot(c, p);
    if (slot < 0) {
        kprintf("[AHCI] port %u no free slot\n", st->port_no);
        return false;
    }

    if (!ahci_wait_port_idle(p)) {
        kprintf("[AHCI] port %u busy (TFD=%08X)\n", st->port_no, p->tfd);
        return false;
    }

    hba_cmd_header_t* headers = (hba_cmd_header_t*)st->clb;
    hba_cmd_header_t* hdr = &headers[slot];
    memset(hdr, 0, sizeof(*hdr));
    hdr->flags = (uint16_t)((5u) | (write ? (1u << 6) : 0));

    hba_cmd_tbl_t* tbl = (hba_cmd_tbl_t*)st->cmd_tables[slot];
    memset(tbl, 0, sizeof(*tbl));

    if (!ahci_build_prdt(buf, bytes, tbl->prdt, &hdr->prdtl)) {
        kprintf("[AHCI] port %u PRDT build failed\n", st->port_no);
        return false;
    }

    hdr->ctba = st->cmd_tables_phys[slot];
    hdr->ctbau = 0;
    hdr->prdbc = 0;

    fis_reg_h2d_t* fis = (fis_reg_h2d_t*)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = cmd;

    bool lba_cmd = (cmd == ATA_CMD_READ_DMA_EXT) || (cmd == ATA_CMD_WRITE_DMA_EXT);
    fis->device = lba_cmd ? (1u << 6) : 0;

    if (lba_cmd) {
        fis->lba0 = (uint8_t)(lba & 0xFFu);
        fis->lba1 = (uint8_t)((lba >> 8) & 0xFFu);
        fis->lba2 = (uint8_t)((lba >> 16) & 0xFFu);
        fis->lba3 = (uint8_t)((lba >> 24) & 0xFFu);
        fis->lba4 = (uint8_t)((lba >> 32) & 0xFFu);
        fis->lba5 = (uint8_t)((lba >> 40) & 0xFFu);
        fis->countl = (uint8_t)(count & 0xFFu);
        fis->counth = (uint8_t)((count >> 8) & 0xFFu);
    }

    p->serr = 0xFFFFFFFFu;
    p->is = 0xFFFFFFFFu;
    p->ci = (1u << slot);

    for (uint32_t t = 0; t < 1000000u; t++) {
        if ((p->ci & (1u << slot)) == 0)
            break;
        if (p->is & HBA_PxIS_TFES) {
            kprintf("[AHCI] port %u TFES (TFD=%08X SERR=%08X)\n",
                    st->port_no, p->tfd, p->serr);
            return false;
        }
    }

    if (p->ci & (1u << slot)) {
        kprintf("[AHCI] port %u cmd timeout (TFD=%08X)\n", st->port_no, p->tfd);
        return false;
    }
    if (p->tfd & ATA_SR_ERR) {
        kprintf("[AHCI] port %u cmd error (TFD=%08X SERR=%08X)\n",
                st->port_no, p->tfd, p->serr);
        return false;
    }
    return true;
}

static inline hba_port_t* ahci_port_ptr(ahci_ctrl_t* c, uint8_t port_no) {
    return (hba_port_t*)(c->base + AHCI_PORT_BASE + (uint32_t)port_no * AHCI_PORT_SIZE);
}

static bool ahci_port_present(hba_port_t* p) {
    uint32_t ssts = p->ssts;
    uint32_t det = ssts & 0xFu;
    uint32_t ipm = (ssts >> 8) & 0xFu;
    return (det == 0x3u) && (ipm == 0x1u);
}

static void ahci_port_stop(hba_port_t* p) {
    uint32_t cmd = p->cmd;
    cmd &= ~(HBA_PxCMD_ST | HBA_PxCMD_FRE);
    p->cmd = cmd;

    for (int i = 0; i < 100000; i++) {
        uint32_t c = p->cmd;
        if ((c & (HBA_PxCMD_CR | HBA_PxCMD_FR)) == 0)
            break;
    }
}

static void ahci_port_start(hba_port_t* p) {
    uint32_t cmd = p->cmd;
    cmd |= HBA_PxCMD_FRE;
    p->cmd = cmd;
    cmd |= HBA_PxCMD_ST;
    p->cmd = cmd;
}

static void ata_id_string(char* out, size_t out_len, const uint16_t* id,
                           int start, int words) {
    int pos = 0;
    for (int i = 0; i < words && pos + 1 < (int)out_len; i++) {
        uint16_t w = id[start + i];
        char c1 = (char)(w >> 8);
        char c2 = (char)(w & 0xFF);
        if (pos < (int)out_len - 1) out[pos++] = c1;
        if (pos < (int)out_len - 1) out[pos++] = c2;
    }
    while (pos > 0 && (out[pos - 1] == ' ' || out[pos - 1] == '\0'))
        pos--;
    out[pos] = '\0';
}

static void ahci_log_identify(ahci_port_state_t* st, const uint16_t* id) {
    char model[41];
    ata_id_string(model, sizeof(model), id, 27, 20);
    bool lba48 = (id[83] & (1u << 10)) != 0;
    uint64_t sectors = 0;
    if (lba48) {
        sectors = ((uint64_t)id[100]) |
                  ((uint64_t)id[101] << 16) |
                  ((uint64_t)id[102] << 32) |
                  ((uint64_t)id[103] << 48);
    } else {
        sectors = ((uint32_t)id[61] << 16) | id[60];
    }

    kprintf("[AHCI] port %u model='%s' lba48=%u\n",
            st->port_no, model, lba48 ? 1u : 0u);
    kprintf("[AHCI] port %u sectors=%u (0x%08X%08X)\n",
            st->port_no,
            (uint32_t)sectors,
            (uint32_t)(sectors >> 32),
            (uint32_t)sectors);
}

static bool ahci_port_identify(ahci_ctrl_t* c, ahci_port_state_t* st, uint16_t* out_id) {
    return ahci_exec_cmd(c, st, ATA_CMD_IDENTIFY, 0, 0,
                         out_id, 512u, false);
}

static bool ahci_port_read(ahci_ctrl_t* c, ahci_port_state_t* st,
                           uint64_t lba, uint16_t count, void* buf) {
    if (count == 0) return false;
    uint32_t bytes = (uint32_t)count * 512u;
    if (bytes > AHCI_MAX_PRDT * 0x1000u) {
        kprintf("[AHCI] port %u read too large (%u bytes)\n", st->port_no, bytes);
        return false;
    }
    return ahci_exec_cmd(c, st, ATA_CMD_READ_DMA_EXT, lba, count, buf, bytes, false);
}

static bool ahci_port_write(ahci_ctrl_t* c, ahci_port_state_t* st,
                            uint64_t lba, uint16_t count, const void* buf) {
    if (count == 0) return false;
    uint32_t bytes = (uint32_t)count * 512u;
    if (bytes > AHCI_MAX_PRDT * 0x1000u) {
        kprintf("[AHCI] port %u write too large (%u bytes)\n", st->port_no, bytes);
        return false;
    }
    return ahci_exec_cmd(c, st, ATA_CMD_WRITE_DMA_EXT, lba, count,
                         (void*)buf, bytes, true);
}

static void ahci_port_init(ahci_ctrl_t* c, uint8_t port_no) {
    hba_port_t* p = ahci_port_ptr(c, port_no);
    if (!ahci_port_present(p)) {
        return;
    }

    if (c->port_count >= AHCI_MAX_PORTS) {
        kprintf("[AHCI] port table full, skipping port %u\n", port_no);
        return;
    }

    ahci_port_state_t* st = &c->ports[c->port_count];
    memset(st, 0, sizeof(*st));
    st->present = true;
    st->port_no = port_no;
    st->port = p;
    st->ctrl = c;

    ahci_port_stop(p);
    p->serr = 0xFFFFFFFFu;
    p->is = 0xFFFFFFFFu;
    p->ie = 0;

    st->clb = ahci_dma_alloc(1024u, &st->clb_phys);
    st->fb = ahci_dma_alloc(256u, &st->fb_phys);
    if (!st->clb || !st->fb) {
        kprintf("[AHCI] port %u DMA alloc failed\n", port_no);
        return;
    }

    p->clb = st->clb_phys;
    p->clbu = 0;
    p->fb = st->fb_phys;
    p->fbu = 0;
    p->is = 0xFFFFFFFFu;

    hba_cmd_header_t* headers = (hba_cmd_header_t*)st->clb;
    for (uint32_t i = 0; i < AHCI_MAX_CMD_SLOTS; i++) {
        st->cmd_tables[i] = ahci_dma_alloc(sizeof(hba_cmd_tbl_t), &st->cmd_tables_phys[i]);
        if (!st->cmd_tables[i]) {
            kprintf("[AHCI] port %u cmdtbl alloc failed (slot %u)\n", port_no, i);
            continue;
        }
        headers[i].flags = 0;
        headers[i].prdtl = 0;
        headers[i].ctba = st->cmd_tables_phys[i];
        headers[i].ctbau = 0;
    }

    ahci_port_start(p);

    kprintf("[AHCI] port %u ready sig=%08X ssts=%08X\n",
            port_no, p->sig, p->ssts);

    if (p->sig == AHCI_SIG_ATA || p->sig == 0) {
        uint32_t id_phys = 0;
        uint16_t* id = (uint16_t*)ahci_dma_alloc(512u, &id_phys);
        if (id) {
            if (ahci_port_identify(c, st, id)) {
                st->ata_device = true;
                ahci_register_sata_port(st);
                ahci_log_identify(st, id);
            } else {
                kprintf("[AHCI] port %u IDENTIFY failed\n", port_no);
            }
            kfree(id);
        }
    } else {
        kprintf("[AHCI] port %u non-ATA device (%s)\n",
                port_no, ahci_sig_name(p->sig));
    }

    c->port_count++;
}

static ahci_port_state_t* ahci_first_sata_port(void) {
    return ahci_get_sata_port(0);
}

bool ahci_is_present(void) {
    return g_ahci_count > 0;
}

uint32_t ahci_sata_port_count(void) {
    return g_sata_port_count;
}

bool ahci_identify(uint16_t* out_id) {
    if (!out_id)
        return false;
    ahci_port_state_t* st = ahci_first_sata_port();
    if (!st) {
        kprint("[AHCI] no SATA port available for IDENTIFY\n");
        return false;
    }
    return ahci_port_identify(st->ctrl, st, out_id);
}

bool ahci_read(uint64_t lba, uint16_t count, void* buf) {
    if (!buf)
        return false;
    ahci_port_state_t* st = ahci_first_sata_port();
    if (!st) {
        kprint("[AHCI] no SATA port available for READ\n");
        return false;
    }
    return ahci_port_read(st->ctrl, st, lba, count, buf);
}

bool ahci_write(uint64_t lba, uint16_t count, const void* buf) {
    if (!buf)
        return false;
    ahci_port_state_t* st = ahci_first_sata_port();
    if (!st) {
        kprint("[AHCI] no SATA port available for WRITE\n");
        return false;
    }
    return ahci_port_write(st->ctrl, st, lba, count, buf);
}

bool ahci_identify_port(uint32_t port_index, uint16_t* out_id) {
    if (!out_id)
        return false;
    ahci_port_state_t* st = ahci_get_sata_port(port_index);
    if (!st) {
        kprintf("[AHCI] invalid SATA port index %u for IDENTIFY\n", port_index);
        return false;
    }
    return ahci_port_identify(st->ctrl, st, out_id);
}

bool ahci_read_port(uint32_t port_index, uint64_t lba, uint16_t count, void* buf) {
    if (!buf)
        return false;
    ahci_port_state_t* st = ahci_get_sata_port(port_index);
    if (!st) {
        kprintf("[AHCI] invalid SATA port index %u for READ\n", port_index);
        return false;
    }
    return ahci_port_read(st->ctrl, st, lba, count, buf);
}

bool ahci_write_port(uint32_t port_index, uint64_t lba, uint16_t count, const void* buf) {
    if (!buf)
        return false;
    ahci_port_state_t* st = ahci_get_sata_port(port_index);
    if (!st) {
        kprintf("[AHCI] invalid SATA port index %u for WRITE\n", port_index);
        return false;
    }
    return ahci_port_write(st->ctrl, st, lba, count, (void*)buf);
}

void ahci_pci_attach(uint8_t bus, uint8_t dev, uint8_t func,
                     uint32_t mmio_base, uint8_t irq_line) {
    if (mmio_base == 0) {
        kprint("[AHCI] MMIO base is 0, skipping attach\n");
        return;
    }
    if (g_ahci_count >= AHCI_MAX_CTRLS) {
        kprint("[AHCI] controller limit reached, skipping attach\n");
        return;
    }

    map_mmio(mmio_base, AHCI_MMIO_SIZE);

    ahci_ctrl_t* c = &g_ahci[g_ahci_count++];
    memset(c, 0, sizeof(*c));
    c->base = mmio_base;
    c->regs = (volatile uint32_t*)mmio_base;
    c->irq_line = irq_line;
    c->bus = bus;
    c->dev = dev;
    c->func = func;

    c->cap = ahci_rd32(c, AHCI_REG_CAP);
    c->pi = ahci_rd32(c, AHCI_REG_PI);
    c->vs = ahci_rd32(c, AHCI_REG_VS);
    c->cmd_slots = ((c->cap >> 8) & 0x1Fu) + 1u;
    if (c->cmd_slots > AHCI_MAX_CMD_SLOTS)
        c->cmd_slots = AHCI_MAX_CMD_SLOTS;

    uint32_t ghc = ahci_rd32(c, AHCI_REG_GHC);
    if ((ghc & AHCI_GHC_AE) == 0) {
        ahci_wr32(c, AHCI_REG_GHC, ghc | AHCI_GHC_AE);
        ghc = ahci_rd32(c, AHCI_REG_GHC);
    }

    uint32_t ports = (c->cap & 0x1Fu) + 1u;
    kprintf("[AHCI] bus=%u dev=%u func=%u mmio=%08X irq=%u\n",
            bus, dev, func, mmio_base, irq_line);
    kprintf("[AHCI] CAP=%08X PI=%08X VS=%08X ports=%u slots=%u GHC=%08X\n",
            c->cap, c->pi, c->vs, ports, c->cmd_slots, ghc);

    c->port_count = 0;
    for (uint8_t p = 0; p < AHCI_MAX_PORTS; p++) {
        if ((c->pi & (1u << p)) == 0)
            continue;
        ahci_log_port_state(c, p);
        ahci_port_init(c, p);
    }
}
