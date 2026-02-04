#include "ac97.h"
#include "pci.h"
#include "hal.h"
#include "../cpu/timer.h"
#include "../drivers/screen.h"
#include "../fs/fscmd.h"
#include "../mm/mem.h"
#include "../libc/string.h"
#include <stddef.h>

#define AC97_PCI_CMD_IO_SPACE     (1u << 0)
#define AC97_PCI_CMD_BUS_MASTER   (1u << 2)

// Native Audio Mixer Base (BAR0)
#define AC97_NAM_RESET            0x00
#define AC97_NAM_MASTER_VOL       0x02
#define AC97_NAM_AUX_VOL          0x04
#define AC97_NAM_PCM_OUT_VOL      0x18
#define AC97_NAM_EXT_CAP          0x28
#define AC97_NAM_EXT_CTRL         0x2A
#define AC97_NAM_PCM_FRONT_RATE   0x2C

// Native Audio Bus Master Base (BAR1)
#define AC97_NABM_PO_BASE         0x10
#define AC97_NABM_GLOB_CNT        0x2C
#define AC97_NABM_GLOB_STA        0x30

// PCM OUT register box (NABM + 0x10)
#define AC97_PO_BDBAR             0x00
#define AC97_PO_CIV               0x04
#define AC97_PO_LVI               0x05
#define AC97_PO_SR                0x06
#define AC97_PO_PICB              0x08
#define AC97_PO_PIV               0x0A
#define AC97_PO_CR                0x0B

// Transfer Status (SR) bits
#define AC97_SR_DCH               (1u << 0) // DMA Controller Halted (RO)
#define AC97_SR_CELV              (1u << 1) // Current equals LVI (RO)
#define AC97_SR_LVBCI             (1u << 2) // Last Valid Buffer Completion Interrupt (R/WC)
#define AC97_SR_IOCI              (1u << 3) // Interrupt On Completion (R/WC)
#define AC97_SR_FIFOE             (1u << 4) // FIFO Error (R/WC)
#define AC97_SR_CLEAR_ALL         (AC97_SR_LVBCI | AC97_SR_IOCI | AC97_SR_FIFOE)

// Transfer Control (CR) bits
#define AC97_CR_RPBM              (1u << 0) // Run/Pause Bus Master
#define AC97_CR_RR                (1u << 1) // Reset Register Box (self-clears)

// Global Control (GLOB_CNT) bits
#define AC97_GC_GIE               (1u << 0)
#define AC97_GC_COLD_RESET        (1u << 1)

#define AC97_BDL_ENTRIES          32u
#define AC97_BUFFER_BYTES         4096u
#define AC97_SAMPLE_RATE          48000u
#define AC97_OUT_CHANNELS         2u

#define AC97_BDL_FLAG_IOC         0x8000u
#define AC97_BDL_FLAG_BUP         0x4000u

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint16_t samples; // number of 16-bit samples (all channels counted)
    uint16_t flags;   // bit15 IOC, bit14 BUP
} ac97_bdl_entry_t;

typedef struct {
    bool present;
    bool vra;

    uint8_t bus;
    uint8_t dev;
    uint8_t func;

    uint16_t namb;
    uint16_t nabmb;
    uint8_t irq_line;

    ac97_bdl_entry_t* bdl;
    uint32_t bdl_phys;

    void* buffers[AC97_BDL_ENTRIES];
    uint32_t buffers_phys[AC97_BDL_ENTRIES];
} ac97_dev_t;

static ac97_dev_t g_ac97;

// 256-entry sine table (signed 16-bit), amplitude ~12000
static const int16_t k_sine_256[256] = {
    0, 294, 589, 883, 1176, 1469, 1761, 2052, 2341, 2629, 2916, 3201,
    3483, 3764, 4043, 4319, 4592, 4863, 5131, 5395, 5657, 5915, 6169, 6420,
    6667, 6910, 7148, 7383, 7613, 7838, 8059, 8274, 8485, 8691, 8891, 9087,
    9276, 9460, 9638, 9811, 9978, 10138, 10293, 10441, 10583, 10719, 10848, 10971,
    11087, 11196, 11299, 11394, 11483, 11565, 11640, 11708, 11769, 11823, 11870, 11910,
    11942, 11967, 11986, 11996, 12000, 11996, 11986, 11967, 11942, 11910, 11870, 11823,
    11769, 11708, 11640, 11565, 11483, 11394, 11299, 11196, 11087, 10971, 10848, 10719,
    10583, 10441, 10293, 10138, 9978, 9811, 9638, 9460, 9276, 9087, 8891, 8691,
    8485, 8274, 8059, 7838, 7613, 7383, 7148, 6910, 6667, 6420, 6169, 5915,
    5657, 5395, 5131, 4863, 4592, 4319, 4043, 3764, 3483, 3201, 2916, 2629,
    2341, 2052, 1761, 1469, 1176, 883, 589, 294, 0, -294, -589, -883,
    -1176, -1469, -1761, -2052, -2341, -2629, -2916, -3201, -3483, -3764, -4043, -4319,
    -4592, -4863, -5131, -5395, -5657, -5915, -6169, -6420, -6667, -6910, -7148, -7383,
    -7613, -7838, -8059, -8274, -8485, -8691, -8891, -9087, -9276, -9460, -9638, -9811,
    -9978, -10138, -10293, -10441, -10583, -10719, -10848, -10971, -11087, -11196, -11299, -11394,
    -11483, -11565, -11640, -11708, -11769, -11823, -11870, -11910, -11942, -11967, -11986, -11996,
    -12000, -11996, -11986, -11967, -11942, -11910, -11870, -11823, -11769, -11708, -11640, -11565,
    -11483, -11394, -11299, -11196, -11087, -10971, -10848, -10719, -10583, -10441, -10293, -10138,
    -9978, -9811, -9638, -9460, -9276, -9087, -8891, -8691, -8485, -8274, -8059, -7838,
    -7613, -7383, -7148, -6910, -6667, -6420, -6169, -5915, -5657, -5395, -5131, -4863,
    -4592, -4319, -4043, -3764, -3483, -3201, -2916, -2629, -2341, -2052, -1761, -1469,
    -1176, -883, -589, -294,
};

static inline uint16_t ac97_nam_read(uint16_t reg) {
    return hal_in16((uint16_t)(g_ac97.namb + reg));
}

static inline void ac97_nam_write(uint16_t reg, uint16_t val) {
    hal_out16((uint16_t)(g_ac97.namb + reg), val);
}

static inline uint32_t ac97_nabm_readl(uint16_t reg) {
    return hal_in32((uint16_t)(g_ac97.nabmb + reg));
}

static inline void ac97_nabm_writel(uint16_t reg, uint32_t val) {
    hal_out32((uint16_t)(g_ac97.nabmb + reg), val);
}

static inline uint8_t ac97_po_readb(uint16_t reg) {
    return hal_in8((uint16_t)(g_ac97.nabmb + AC97_NABM_PO_BASE + reg));
}

static inline void ac97_po_writeb(uint16_t reg, uint8_t val) {
    hal_out8((uint16_t)(g_ac97.nabmb + AC97_NABM_PO_BASE + reg), val);
}

static inline uint16_t ac97_po_readw(uint16_t reg) {
    return hal_in16((uint16_t)(g_ac97.nabmb + AC97_NABM_PO_BASE + reg));
}

static inline void ac97_po_writew(uint16_t reg, uint16_t val) {
    hal_out16((uint16_t)(g_ac97.nabmb + AC97_NABM_PO_BASE + reg), val);
}

static inline void ac97_po_writel(uint16_t reg, uint32_t val) {
    hal_out32((uint16_t)(g_ac97.nabmb + AC97_NABM_PO_BASE + reg), val);
}

static void ac97_busy_wait_ms(uint32_t ms) {
    // tick is 100Hz (10ms). This is coarse but fine for init delays.
    uint32_t start = tick;
    uint32_t wait_ticks = (ms + 9u) / 10u;
    if (wait_ticks == 0)
        wait_ticks = 1;
    while ((tick - start) < wait_ticks) {
        hal_halt();
    }
}

static bool ac97_alloc_dma(void) {
    if (g_ac97.bdl)
        return true;

    uint32_t bdl_phys = 0;
    g_ac97.bdl = (ac97_bdl_entry_t*)kmalloc(sizeof(ac97_bdl_entry_t) * AC97_BDL_ENTRIES, 1, &bdl_phys);
    if (!g_ac97.bdl) {
        kprint("[AC97] kmalloc failed for BDL\n");
        return false;
    }
    g_ac97.bdl_phys = bdl_phys;
    memset(g_ac97.bdl, 0, sizeof(ac97_bdl_entry_t) * AC97_BDL_ENTRIES);

    for (uint32_t i = 0; i < AC97_BDL_ENTRIES; i++) {
        uint32_t phys = 0;
        void* buf = kmalloc(AC97_BUFFER_BYTES, 1, &phys);
        if (!buf) {
            kprintf("[AC97] kmalloc failed for buffer %d\n", (int)i);
            return false;
        }
        g_ac97.buffers[i] = buf;
        g_ac97.buffers_phys[i] = phys;
        memset(buf, 0, AC97_BUFFER_BYTES);

        g_ac97.bdl[i].addr = phys;
        g_ac97.bdl[i].samples = (uint16_t)(AC97_BUFFER_BYTES / 2u);
        g_ac97.bdl[i].flags = 0; // no IOC/BUP for polled streaming
    }

    return true;
}

static bool ac97_reset_codec(void) {
    // Cold reset (no interrupts)
    ac97_nabm_writel(AC97_NABM_GLOB_CNT, AC97_GC_COLD_RESET);
    ac97_busy_wait_ms(20);

    // Reset NAM registers to defaults
    ac97_nam_write(AC97_NAM_RESET, 0x0000);
    ac97_busy_wait_ms(20);

    // Basic unmute + reasonable volume.
    // Master volume (0=0dB loudest, 31=46.5dB attenuation); set ~12dB attenuation on both channels.
    uint16_t master = (uint16_t)((8u << 8) | 8u);
    ac97_nam_write(AC97_NAM_MASTER_VOL, master);

    // PCM out volume: 0x08 means 0dB, and mute is bit15 (keep it 0).
    uint16_t pcm = (uint16_t)((8u << 8) | 8u);
    ac97_nam_write(AC97_NAM_PCM_OUT_VOL, pcm);

    // Enable VRA if supported and set sample rate.
    uint16_t ext_cap = ac97_nam_read(AC97_NAM_EXT_CAP);
    if (ext_cap & 0x0001u) {
        uint16_t ext_ctrl = ac97_nam_read(AC97_NAM_EXT_CTRL);
        ext_ctrl |= 0x0001u;
        ac97_nam_write(AC97_NAM_EXT_CTRL, ext_ctrl);
        ac97_nam_write(AC97_NAM_PCM_FRONT_RATE, (uint16_t)AC97_SAMPLE_RATE);
        g_ac97.vra = true;
    } else {
        g_ac97.vra = false;
    }

    return true;
}

static bool ac97_reset_pcm_out_box(void) {
    // Stop DMA
    uint8_t cr = ac97_po_readb(AC97_PO_CR);
    cr &= (uint8_t)~AC97_CR_RPBM;
    ac97_po_writeb(AC97_PO_CR, cr);

    // Reset box
    ac97_po_writeb(AC97_PO_CR, AC97_CR_RR);
    for (uint32_t i = 0; i < 100000u; i++) {
        if ((ac97_po_readb(AC97_PO_CR) & AC97_CR_RR) == 0)
            return true;
    }
    kprint("[AC97] PCM OUT box reset timed out\n");
    return false;
}

bool ac97_is_present(void) {
    return g_ac97.present;
}

bool ac97_pci_attach(uint8_t bus, uint8_t device, uint8_t function) {
    if (g_ac97.present) {
        return true;
    }

    uint32_t class_reg = pci_read_dword(bus, device, function, 0x08);
    uint8_t class_code = (class_reg >> 24) & 0xFF;
    uint8_t subclass   = (class_reg >> 16) & 0xFF;
    uint8_t prog_if    = (class_reg >> 8) & 0xFF;

    if (class_code != 0x04 || subclass != 0x01 || prog_if != 0x00) {
        return false;
    }

    uint32_t bar0 = pci_read_dword(bus, device, function, 0x10);
    uint32_t bar1 = pci_read_dword(bus, device, function, 0x14);
    if (((bar0 & 0x1u) == 0) || ((bar1 & 0x1u) == 0)) {
        kprintf("[AC97] Unsupported BARs (BAR0=%08X BAR1=%08X)\n", bar0, bar1);
        return false;
    }

    uint16_t namb = (uint16_t)(bar0 & ~0x3u);
    uint16_t nabmb = (uint16_t)(bar1 & ~0x3u);
    if (namb == 0 || nabmb == 0) {
        kprintf("[AC97] Invalid I/O bases (NAMB=%04X NABMB=%04X)\n", namb, nabmb);
        return false;
    }

    // Enable I/O space + bus mastering
    uint32_t cmdsts = pci_read_dword(bus, device, function, 0x04);
    cmdsts |= AC97_PCI_CMD_IO_SPACE | AC97_PCI_CMD_BUS_MASTER;
    pci_write_dword(bus, device, function, 0x04, cmdsts);

    uint32_t irq_reg = pci_read_dword(bus, device, function, 0x3C);
    uint8_t irq_line = irq_reg & 0xFF;

    g_ac97.present = true;
    g_ac97.bus = bus;
    g_ac97.dev = device;
    g_ac97.func = function;
    g_ac97.namb = namb;
    g_ac97.nabmb = nabmb;
    g_ac97.irq_line = irq_line;

    kprintf("[AC97] Found AC'97 controller at %d:%d.%d (NAMB=%04X NABMB=%04X IRQ=%d)\n",
            bus, device, function, namb, nabmb, irq_line);

    if (!ac97_alloc_dma()) {
        kprint("[AC97] DMA allocation failed\n");
        g_ac97.present = false;
        return false;
    }

    if (!ac97_reset_codec()) {
        kprint("[AC97] Codec init failed\n");
        g_ac97.present = false;
        return false;
    }

    if (!ac97_reset_pcm_out_box()) {
        g_ac97.present = false;
        return false;
    }

    // Clear any pending status bits
    ac97_po_writew(AC97_PO_SR, (uint16_t)AC97_SR_CLEAR_ALL);

    // Disable interrupts from the controller (GIE=0, keep cold reset=1)
    ac97_nabm_writel(AC97_NABM_GLOB_CNT, AC97_GC_COLD_RESET);

    // Print basic capabilities
    uint32_t glob_sta = ac97_nabm_readl(AC97_NABM_GLOB_STA);
    uint16_t nam_cap = ac97_nam_read(AC97_NAM_RESET);
    kprintf("[AC97] GLOB_STA=%08X NAM_CAP=%04X\n", glob_sta, nam_cap);

    return true;
}

void ac97_dump(void) {
    if (!g_ac97.present) {
        kprint("[AC97] not present\n");
        return;
    }

    uint16_t master = ac97_nam_read(AC97_NAM_MASTER_VOL);
    uint16_t pcm = ac97_nam_read(AC97_NAM_PCM_OUT_VOL);
    uint32_t glob_cnt = ac97_nabm_readl(AC97_NABM_GLOB_CNT);
    uint32_t glob_sta = ac97_nabm_readl(AC97_NABM_GLOB_STA);

    kprintf("[AC97] NAMB=%04X NABMB=%04X IRQ=%d\n", g_ac97.namb, g_ac97.nabmb, g_ac97.irq_line);
    kprintf("[AC97] GLOB_CNT=%08X GLOB_STA=%08X\n", glob_cnt, glob_sta);
    kprintf("[AC97] MASTER_VOL=%04X PCM_OUT_VOL=%04X\n", master, pcm);
}

static void ac97_fill_tone_buffer(uint32_t buf_index, uint16_t* phase, uint16_t step) {
    // Interleaved stereo, 16-bit signed
    int16_t* out = (int16_t*)g_ac97.buffers[buf_index];
    const uint32_t frames = AC97_BUFFER_BYTES / 4u; // 2ch * 16-bit

    for (uint32_t i = 0; i < frames; i++) {
        uint8_t idx = (uint8_t)(*phase >> 8);
        int16_t s = k_sine_256[idx];
        *phase = (uint16_t)(*phase + step);
        out[i * 2u + 0u] = s;
        out[i * 2u + 1u] = s;
    }
}

void ac97_stop(void) {
    if (!g_ac97.present)
        return;

    // Pause/stop DMA
    uint8_t cr = ac97_po_readb(AC97_PO_CR);
    cr &= (uint8_t)~AC97_CR_RPBM;
    ac97_po_writeb(AC97_PO_CR, cr);

    // Clear pending status
    ac97_po_writew(AC97_PO_SR, (uint16_t)AC97_SR_CLEAR_ALL);
}

int ac97_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    if (!g_ac97.present) {
        kprint("[AC97] not present\n");
        return -1;
    }
    if (freq_hz == 0 || duration_ms == 0) {
        return 0;
    }
    if (freq_hz > (AC97_SAMPLE_RATE / 2u)) {
        kprint("[AC97] freq too high\n");
        return -1;
    }

    if (!ac97_alloc_dma()) {
        return -1;
    }

    if (!ac97_reset_pcm_out_box()) {
        return -1;
    }

    uint16_t step = (uint16_t)(((uint32_t)freq_hz * 65536u) / (uint32_t)AC97_SAMPLE_RATE);
    uint16_t phase = 0;

    for (uint32_t i = 0; i < AC97_BDL_ENTRIES; i++) {
        ac97_fill_tone_buffer(i, &phase, step);
    }

    // Program BDL
    ac97_po_writel(AC97_PO_BDBAR, g_ac97.bdl_phys);
    ac97_po_writeb(AC97_PO_LVI, (uint8_t)(AC97_BDL_ENTRIES - 1u));
    ac97_po_writew(AC97_PO_SR, (uint16_t)AC97_SR_CLEAR_ALL);

    // Start playback
    ac97_po_writeb(AC97_PO_CR, AC97_CR_RPBM);

    // Ensure DMA is actually running; otherwise we'd spin forever waiting for CIV to advance.
    {
        uint32_t spins = 0;
        while (ac97_po_readw(AC97_PO_SR) & AC97_SR_DCH) {
            if (++spins > 1000000u) {
                kprint("[AC97] DMA did not start\n");
                ac97_stop();
                return -1;
            }
            hal_pause();
        }
    }

    // Avoid depending on the PIT IRQ tick (it may be masked); instead, run for the
    // requested duration by counting completed DMA buffers (CIV advances).
    const uint32_t frames_per_buffer = AC97_BUFFER_BYTES / 4u; // 2ch * 16-bit
    uint32_t total_frames = (duration_ms * AC97_SAMPLE_RATE + 999u) / 1000u;
    uint32_t buffers_to_play = (total_frames + frames_per_buffer - 1u) / frames_per_buffer;
    if (buffers_to_play == 0)
        buffers_to_play = 1;

    uint32_t buffers_played = 0;
    uint8_t last_civ = ac97_po_readb(AC97_PO_CIV) & 0x1Fu;

    while (buffers_played < buffers_to_play) {
        uint8_t civ = ac97_po_readb(AC97_PO_CIV) & 0x1Fu;
        if (civ != last_civ) {
            // The previous CIV entry has completed, refill it and advance LVI
            ac97_fill_tone_buffer(last_civ, &phase, step);
            ac97_po_writeb(AC97_PO_LVI, last_civ);
            ac97_po_writew(AC97_PO_SR, (uint16_t)AC97_SR_CLEAR_ALL);
            last_civ = civ;
            buffers_played++;
        } else {
            // Also ensure the DMA doesn't stop if we stall for a while.
            // Keep LVI at the last refilled index.
            uint16_t sr = ac97_po_readw(AC97_PO_SR);
            if (sr & AC97_SR_DCH) {
                kprint("[AC97] DMA halted\n");
                break;
            }
            if (sr & AC97_SR_CELV) {
                // CE == LVI; push LVI one entry behind CIV to keep ring full
                uint8_t new_lvi = (uint8_t)((civ + AC97_BDL_ENTRIES - 1u) & 0x1Fu);
                ac97_po_writeb(AC97_PO_LVI, new_lvi);
                ac97_po_writew(AC97_PO_SR, (uint16_t)AC97_SR_CLEAR_ALL);
            }
            // Don't halt the CPU here (can hang if IRQs are masked); just spin lightly.
            hal_pause();
        }
    }

    ac97_stop();
    return 0;
}

typedef struct {
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_fmt_t;

typedef struct {
    const uint8_t* data;
    uint32_t data_size;
    wav_fmt_t fmt;

    uint32_t src_frames;
    uint32_t dst_rate;

    uint64_t src_pos_fp; // 16.16 fixed point in source frames (widened to avoid wrap on long files)
    uint32_t step_fp;    // 16.16 source frames per output frame
} wav_state_t;

typedef struct {
    uint16_t samples; // number of 16-bit samples (all channels counted)
    bool end;
} ac97_fill_result_t;

static inline uint16_t ac97_le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t ac97_le32(const uint8_t* p) {
    return (uint32_t)((uint32_t)p[0]
                    | ((uint32_t)p[1] << 8)
                    | ((uint32_t)p[2] << 16)
                    | ((uint32_t)p[3] << 24));
}

static bool ac97_wav_parse(const uint8_t* wav, uint32_t wav_size, wav_fmt_t* out_fmt, const uint8_t** out_data,
                           uint32_t* out_data_size) {
    if (!wav || wav_size < 12 || !out_fmt || !out_data || !out_data_size) {
        return false;
    }
    if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool have_fmt = false;
    bool have_data = false;

    uint32_t pos = 12;
    while (pos + 8u <= wav_size) {
        const uint8_t* chunk = wav + pos;
        uint32_t chunk_size = ac97_le32(chunk + 4);
        pos += 8;

        if (pos > wav_size) {
            return false;
        }

        uint32_t chunk_end = pos + chunk_size;
        if (chunk_end > wav_size) {
            return false;
        }

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16u) {
                return false;
            }

            out_fmt->audio_format = ac97_le16(wav + pos + 0);
            out_fmt->channels = ac97_le16(wav + pos + 2);
            out_fmt->sample_rate = ac97_le32(wav + pos + 4);
            out_fmt->block_align = ac97_le16(wav + pos + 12);
            out_fmt->bits_per_sample = ac97_le16(wav + pos + 14);
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            *out_data = wav + pos;
            *out_data_size = chunk_size;
            have_data = true;
        }

        pos = chunk_end + (chunk_size & 1u);
        if (have_fmt && have_data) {
            return true;
        }
    }

    return false;
}

static int32_t ac97_wav_sample_at(const wav_state_t* st, uint32_t frame, uint16_t channel) {
    if (frame >= st->src_frames) {
        return 0;
    }
    if (st->fmt.channels == 0) {
        return 0;
    }

    if (channel >= st->fmt.channels) {
        channel = 0;
    }

    uint32_t bytes_per_sample = (uint32_t)st->fmt.bits_per_sample / 8u;
    const uint8_t* p = st->data + frame * (uint32_t)st->fmt.block_align + (uint32_t)channel * bytes_per_sample;

    if (st->fmt.bits_per_sample == 8u) {
        return ((int32_t)p[0] - 128) << 8;
    }
    if (st->fmt.bits_per_sample == 16u) {
        return (int16_t)ac97_le16(p);
    }

    return 0;
}

static inline int16_t ac97_clamp_s16(int32_t x) {
    if (x > 32767)
        return 32767;
    if (x < -32768)
        return -32768;
    return (int16_t)x;
}

static int16_t ac97_wav_interp(const wav_state_t* st, uint32_t src_index, uint32_t frac, uint16_t channel) {
    int32_t s0 = ac97_wav_sample_at(st, src_index, channel);
    int32_t s1 = s0;
    if ((src_index + 1u) < st->src_frames) {
        s1 = ac97_wav_sample_at(st, src_index + 1u, channel);
    }

    int32_t diff = s1 - s0;
    int32_t frac15 = (int32_t)(frac >> 1); // 0..32767 to keep 32-bit math safe
    int32_t out = s0 + ((diff * frac15) >> 15);
    return ac97_clamp_s16(out);
}

static ac97_fill_result_t ac97_fill_wav_buffer(uint32_t buf_index, wav_state_t* st) {
    ac97_fill_result_t res;
    res.samples = 0;
    res.end = false;

    int16_t* out = (int16_t*)g_ac97.buffers[buf_index];
    const uint32_t out_frames = AC97_BUFFER_BYTES / 4u; // 2ch * 16-bit

    uint32_t frames_written = 0;

    for (uint32_t i = 0; i < out_frames; i++) {
        uint32_t src_index = (uint32_t)(st->src_pos_fp >> 16);
        if (src_index >= st->src_frames) {
            out[i * 2u + 0u] = 0;
            out[i * 2u + 1u] = 0;
            res.end = true;
            continue;
        }

        uint32_t frac = (uint32_t)(st->src_pos_fp & 0xFFFFull);

        int16_t l = ac97_wav_interp(st, src_index, frac, 0);
        int16_t r = l;
        if (st->fmt.channels >= 2u) {
            r = ac97_wav_interp(st, src_index, frac, 1);
        }

        out[i * 2u + 0u] = l;
        out[i * 2u + 1u] = r;

        st->src_pos_fp += st->step_fp;
        frames_written++;
    }

    if ((uint32_t)(st->src_pos_fp >> 16) >= st->src_frames) {
        res.end = true;
    }

    uint32_t samples = frames_written * AC97_OUT_CHANNELS;
    if (samples == 0u) {
        samples = AC97_OUT_CHANNELS; // keep DMA happy with a minimal count
    }
    if (samples > 0xFFFEu) {
        samples = 0xFFFEu;
    }
    res.samples = (uint16_t)samples;
    return res;
}

static int ac97_wait_dma_halt(uint32_t max_spins) {
    for (uint32_t i = 0; i < max_spins; i++) {
        uint16_t sr = ac97_po_readw(AC97_PO_SR);
        if (sr & AC97_SR_DCH) {
            return 0;
        }
        hal_pause();
    }
    return -1;
}

int ac97_play_wav(const uint8_t* wav, uint32_t wav_size) {
    if (!g_ac97.present) {
        kprint("[AC97] not present\n");
        return -1;
    }
    if (!wav || wav_size < 12u) {
        kprint("[AC97] invalid wav\n");
        return -1;
    }

    wav_fmt_t fmt;
    const uint8_t* data = NULL;
    uint32_t data_size = 0;
    if (!ac97_wav_parse(wav, wav_size, &fmt, &data, &data_size)) {
        kprint("[AC97] wav parse failed\n");
        return -1;
    }

    if (fmt.audio_format != 1u) {
        kprintf("[AC97] unsupported wav format: %u\n", (uint32_t)fmt.audio_format);
        return -1;
    }
    if (fmt.channels < 1u || fmt.channels > 2u) {
        kprintf("[AC97] unsupported channels: %u\n", (uint32_t)fmt.channels);
        return -1;
    }
    if (fmt.bits_per_sample != 8u && fmt.bits_per_sample != 16u) {
        kprintf("[AC97] unsupported bits: %u\n", (uint32_t)fmt.bits_per_sample);
        return -1;
    }
    if (fmt.sample_rate < 8000u || fmt.sample_rate > 192000u) {
        kprintf("[AC97] unsupported sample rate: %u\n", fmt.sample_rate);
        return -1;
    }
    if (fmt.block_align == 0u || data_size < fmt.block_align) {
        kprint("[AC97] invalid wav data\n");
        return -1;
    }

    if (!ac97_alloc_dma()) {
        return -1;
    }
    if (!ac97_reset_pcm_out_box()) {
        return -1;
    }

    // Reset descriptor fields (may have been tweaked by a previous run).
    for (uint32_t i = 0; i < AC97_BDL_ENTRIES; i++) {
        g_ac97.bdl[i].samples = (uint16_t)(AC97_BUFFER_BYTES / 2u);
        g_ac97.bdl[i].flags = 0;
    }

    wav_state_t st;
    memset(&st, 0, sizeof(st));
    st.data = data;
    st.data_size = data_size;
    st.fmt = fmt;
    st.src_frames = data_size / (uint32_t)fmt.block_align;

    if (st.src_frames == 0u) {
        kprint("[AC97] empty wav\n");
        return -1;
    }

    st.dst_rate = AC97_SAMPLE_RATE;
    if (g_ac97.vra && fmt.sample_rate >= 8000u && fmt.sample_rate <= 48000u) {
        st.dst_rate = fmt.sample_rate;
    }

    if (g_ac97.vra) {
        ac97_nam_write(AC97_NAM_PCM_FRONT_RATE, (uint16_t)st.dst_rate);
    }

    // step_fp = (src_rate / dst_rate) in 16.16 without 64-bit division
    {
        uint32_t q = fmt.sample_rate / st.dst_rate;
        uint32_t r = fmt.sample_rate % st.dst_rate;
        st.step_fp = (q << 16) + (r * 65536u) / st.dst_rate;
        if (st.step_fp == 0u) {
            st.step_fp = 1u;
        }
    }
    st.src_pos_fp = 0;

    // Prefill ring
    uint8_t stop_index = 0xFFu;
    for (uint32_t i = 0; i < AC97_BDL_ENTRIES; i++) {
        ac97_fill_result_t fr = ac97_fill_wav_buffer(i, &st);
        g_ac97.bdl[i].samples = fr.samples;
        g_ac97.bdl[i].flags = 0;
        if (fr.end) {
            g_ac97.bdl[i].flags |= AC97_BDL_FLAG_BUP;
            stop_index = (uint8_t)i;
            break;
        }
    }

    // Program BDL
    ac97_po_writel(AC97_PO_BDBAR, g_ac97.bdl_phys);
    ac97_po_writeb(AC97_PO_LVI, (uint8_t)((stop_index != 0xFFu) ? stop_index : (AC97_BDL_ENTRIES - 1u)));
    ac97_po_writew(AC97_PO_SR, (uint16_t)AC97_SR_CLEAR_ALL);

    // Start playback
    ac97_po_writeb(AC97_PO_CR, AC97_CR_RPBM);

    // Ensure DMA is actually running
    {
        uint32_t spins = 0;
        while (ac97_po_readw(AC97_PO_SR) & AC97_SR_DCH) {
            if (++spins > 1000000u) {
                kprint("[AC97] DMA did not start\n");
                ac97_stop();
                return -1;
            }
            hal_pause();
        }
    }

    // If it fit in the initial buffers, just wait for the device to stop.
    if (stop_index != 0xFFu) {
        if (ac97_wait_dma_halt(20000000u) != 0) {
            kprint("[AC97] wav: timeout waiting for halt\n");
        }
        ac97_stop();
        return 0;
    }

    uint8_t last_civ = ac97_po_readb(AC97_PO_CIV) & 0x1Fu;

    // Stream: refill completed buffers until EOF, then let the device drain and halt.
    while (1) {
        uint8_t civ = ac97_po_readb(AC97_PO_CIV) & 0x1Fu;
        if (civ != last_civ) {
            ac97_fill_result_t fr = ac97_fill_wav_buffer(last_civ, &st);
            g_ac97.bdl[last_civ].samples = fr.samples;
            g_ac97.bdl[last_civ].flags = fr.end ? AC97_BDL_FLAG_BUP : 0;

            ac97_po_writeb(AC97_PO_LVI, last_civ);
            ac97_po_writew(AC97_PO_SR, (uint16_t)AC97_SR_CLEAR_ALL);

            if (fr.end) {
                break;
            }
            last_civ = civ;
        } else {
            uint16_t sr = ac97_po_readw(AC97_PO_SR);
            if (sr & AC97_SR_DCH) {
                kprint("[AC97] wav: DMA halted\n");
                break;
            }
            if (sr & AC97_SR_CELV) {
                uint8_t new_lvi = (uint8_t)((civ + AC97_BDL_ENTRIES - 1u) & 0x1Fu);
                ac97_po_writeb(AC97_PO_LVI, new_lvi);
                ac97_po_writew(AC97_PO_SR, (uint16_t)AC97_SR_CLEAR_ALL);
            }
            hal_pause();
        }
    }

    if (ac97_wait_dma_halt(40000000u) != 0) {
        kprint("[AC97] wav: timeout waiting for halt\n");
    }

    ac97_stop();
    return 0;
}
