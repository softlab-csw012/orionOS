#include "hda.h"
#include "pci.h"
#include "hal.h"
#include "../drivers/screen.h"
#include "../libc/string.h"
#include "../mm/mem.h"
#include "../mm/paging.h"
#include <stddef.h>
#include <stdint.h>

// HDA Global registers (MMIO)
#define HDA_REG_GCAP      0x00u // 32
#define HDA_REG_VMIN      0x02u // 8
#define HDA_REG_VMAJ      0x03u // 8
#define HDA_REG_GCTL      0x08u // 32
#define HDA_REG_STATESTS  0x0Eu // 16
#define HDA_REG_INTCTL    0x20u // 32
#define HDA_REG_INTSTS    0x24u // 32

// Stream descriptors
#define HDA_REG_SD_BASE   0x80u
#define HDA_REG_SD_SIZE   0x20u

#define HDA_SD_CTL0       0x00u
#define HDA_SD_CTL2       0x02u
#define HDA_SD_STS        0x03u
#define HDA_SD_LPIB       0x04u
#define HDA_SD_CBL        0x08u
#define HDA_SD_LVI        0x0Cu
#define HDA_SD_FMT        0x12u
#define HDA_SD_BDPL       0x18u
#define HDA_SD_BDPU       0x1Cu

// Immediate Command Interface
#define HDA_REG_ICOI      0x60u // 32
#define HDA_REG_ICII      0x64u // 32
#define HDA_REG_ICIS      0x68u // 16

#define HDA_GCTL_CRST     (1u << 0)

#define HDA_ICIS_ICB      (1u << 0) // Immediate Command Busy
#define HDA_ICIS_IRV      (1u << 1) // Immediate Response Valid
#define HDA_ICIS_ICES     (1u << 2) // Immediate Command Error

#define HDA_SAMPLE_RATE      48000u
#define HDA_OUT_CHANNELS     2u
#define HDA_BUFFER_BYTES     4096u
#define HDA_BDL_ENTRIES      32u

// 48kHz, 16-bit, 2ch
#define HDA_STREAM_FORMAT_48K_16B_2CH 0x0011u

// Realtek ALC887: 0x14 is commonly the rear green line-out pin.
#define HDA_PREFERRED_PIN_NID 0x14u

// Widget types (Audio Widget Capabilities >> 20)
#define HDA_WTYPE_AUDIO_OUT 0x0u
#define HDA_WTYPE_AUDIO_IN  0x1u
#define HDA_WTYPE_MIXER     0x2u
#define HDA_WTYPE_SELECTOR  0x3u
#define HDA_WTYPE_PIN       0x4u

// Parameters
#define HDA_PARAM_VENDOR_ID     0x00u
#define HDA_PARAM_NODE_COUNT    0x04u
#define HDA_PARAM_FG_TYPE       0x05u
#define HDA_PARAM_AWCAP         0x09u
#define HDA_PARAM_PIN_CAP       0x0Cu
#define HDA_PARAM_CONN_LIST_LEN 0x0Eu

// Verbs (12-bit)
#define HDA_VERB_GET_PARAMETER              0xF00u
#define HDA_VERB_GET_CONN_LIST_ENTRY        0xF02u
#define HDA_VERB_SET_SELECTED_INPUT         0x701u
#define HDA_VERB_SET_POWER_STATE            0x705u
#define HDA_VERB_SET_CONV_STREAM_CHAN       0x706u
#define HDA_VERB_SET_PIN_WIDGET_CONTROL     0x707u
#define HDA_VERB_SET_EAPD_BTL               0x70Cu
#define HDA_VERB_SET_OUTPUT_CONV_CHAN_CNT   0x72Du
#define HDA_VERB_AFG_RESET                  0x7FFu
#define HDA_VERB_GET_PIN_CFG_DEFAULT        0xF1Cu

// Verbs (4-bit, 16-bit payload)
#define HDA_VERB4_SET_CONV_FORMAT           0x2u
#define HDA_VERB4_SET_AMP_GAIN_MUTE         0x3u

// Amplifier payload bits (16-bit payload for verb 0x3)
#define HDA_AMP_SET_OUTPUT  0x8000u
#define HDA_AMP_SET_INPUT   0x4000u
#define HDA_AMP_SET_LEFT    0x2000u
#define HDA_AMP_SET_RIGHT   0x1000u
#define HDA_AMP_SET_INDEX_SHIFT 8u
#define HDA_AMP_SET_MUTE    0x0080u
#define HDA_AMP_SET_GAIN_MASK 0x007Fu

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint32_t ioc;
} hda_bdl_entry_t;

typedef struct {
    bool present;

    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint8_t irq_line;

    uintptr_t mmio_base;
    volatile uint8_t* mmio;

    uint32_t gcap;
    uint8_t vmaj;
    uint8_t vmin;
    uint16_t codecs_mask;

    uint32_t codec_vendor[15];

    bool play_ready;
    uint8_t play_cad;
    uint8_t play_afg;
    uint8_t play_pin;
    uint8_t play_dac;
    uint8_t play_stream_id;

    uint32_t sd_off;

    hda_bdl_entry_t* bdl;
    uint32_t bdl_phys;
    void* buffers[HDA_BDL_ENTRIES];
    uint32_t buffers_phys[HDA_BDL_ENTRIES];
} hda_dev_t;

#define HDA_MAX_DEVS 4

static hda_dev_t g_hda_devs[HDA_MAX_DEVS];
static int g_hda_count = 0;
static int g_hda_active = -1;
static hda_dev_t* g_hda = NULL;
static uint8_t g_hda_forced_pin = 0;

static inline void invlpg(uint32_t addr) {
    hal_invlpg((const void*)(uintptr_t)addr);
}

static void map_mmio(uint32_t base, uint32_t size) {
    uint32_t start = base & ~0xFFFu;
    uint32_t end = (base + size + 0xFFFu) & ~0xFFFu;
    for (uint32_t addr = start; addr < end; addr += 0x1000u) {
        map_page(page_directory, addr, addr, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
        invlpg(addr);
    }
}

static inline uint8_t hda_rd8(uint32_t off) {
    return *(volatile uint8_t*)(g_hda->mmio + off);
}

static inline uint16_t hda_rd16(uint32_t off) {
    return *(volatile uint16_t*)(g_hda->mmio + off);
}

static inline uint32_t hda_rd32(uint32_t off) {
    return *(volatile uint32_t*)(g_hda->mmio + off);
}

static inline void hda_wr8(uint32_t off, uint8_t v) {
    *(volatile uint8_t*)(g_hda->mmio + off) = v;
}

static inline void hda_wr16(uint32_t off, uint16_t v) {
    *(volatile uint16_t*)(g_hda->mmio + off) = v;
}

static inline void hda_wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(g_hda->mmio + off) = v;
}

static void delay_ms(uint32_t ms) {
    extern uint32_t tick;
    uint32_t start = tick;
    uint32_t ticks_needed = (ms + 9u) / 10u; // PIT 100Hz => 10ms/tick
    if (ticks_needed == 0) ticks_needed = 1;
    while ((tick - start) < ticks_needed) {
        hal_wait_for_interrupt();
    }
}

static bool hda_wait_gctl_crst(bool want_set, uint32_t timeout_ms) {
    extern uint32_t tick;
    uint32_t start = tick;
    uint32_t timeout_ticks = (timeout_ms + 9u) / 10u;
    if (timeout_ticks == 0) timeout_ticks = 1;

    while (1) {
        uint32_t gctl = hda_rd32(HDA_REG_GCTL);
        bool set = (gctl & HDA_GCTL_CRST) != 0;
        if (set == want_set)
            return true;
        if ((tick - start) > timeout_ticks)
            return false;
        hal_wait_for_interrupt();
    }
}

static bool hda_wait_state_sts(uint32_t timeout_ms) {
    extern uint32_t tick;
    uint32_t start = tick;
    uint32_t timeout_ticks = (timeout_ms + 9u) / 10u;
    if (timeout_ticks == 0) timeout_ticks = 1;

    while (1) {
        uint16_t st = hda_rd16(HDA_REG_STATESTS);
        if ((st & 0x7FFFu) != 0) {
            return true;
        }
        if ((tick - start) > timeout_ticks) {
            return false;
        }
        hal_wait_for_interrupt();
    }
}

static bool hda_controller_reset(void) {
    uint32_t gctl = hda_rd32(HDA_REG_GCTL);

    // Enter reset (CRST=0)
    hda_wr32(HDA_REG_GCTL, gctl & ~HDA_GCTL_CRST);
    if (!hda_wait_gctl_crst(false, 100)) {
        kprint("[HDA] GCTL reset deassert timeout\n");
        return false;
    }

    delay_ms(20);

    // Exit reset (CRST=1)
    hda_wr32(HDA_REG_GCTL, (gctl | HDA_GCTL_CRST));
    if (!hda_wait_gctl_crst(true, 100)) {
        kprint("[HDA] GCTL reset assert timeout\n");
        return false;
    }

    delay_ms(20);
    return true;
}

static int hda_send_cmd20(uint8_t cad, uint8_t nid, uint32_t cmd20, uint32_t* out_resp) {
    if (!g_hda || !g_hda->present || !g_hda->mmio)
        return -1;
    if (cad >= 15)
        return -1;

    uint32_t cmd = ((uint32_t)cad << 28) | ((uint32_t)nid << 20) | (cmd20 & 0xFFFFFu);

    // Wait for ICB=0
    for (uint32_t spins = 0; spins < 1000000u; spins++) {
        uint16_t icis = hda_rd16(HDA_REG_ICIS);
        if ((icis & HDA_ICIS_ICB) == 0)
            break;
    }
    if (hda_rd16(HDA_REG_ICIS) & HDA_ICIS_ICB) {
        return -1;
    }

    // Clear prior status (RW1C bits)
    hda_wr16(HDA_REG_ICIS, (uint16_t)(HDA_ICIS_IRV | HDA_ICIS_ICES));

    // Write command
    hda_wr32(HDA_REG_ICOI, cmd);

    // Start immediate command by setting ICB
    hda_wr16(HDA_REG_ICIS, HDA_ICIS_ICB);

    // Poll until ICB clears
    uint32_t timeout = 2000000u;
    while (timeout--) {
        uint16_t icis = hda_rd16(HDA_REG_ICIS);
        if ((icis & HDA_ICIS_ICB) == 0) {
            if (icis & HDA_ICIS_ICES) {
                hda_wr16(HDA_REG_ICIS, HDA_ICIS_ICES);
                return -1;
            }
            if ((icis & HDA_ICIS_IRV) == 0) {
                return -1;
            }
            uint32_t resp = hda_rd32(HDA_REG_ICII);
            // Clear IRV
            hda_wr16(HDA_REG_ICIS, HDA_ICIS_IRV);
            if (out_resp)
                *out_resp = resp;
            return 0;
        }
        hal_pause();
    }

    return -1;
}

int hda_send_verb(uint8_t cad, uint8_t nid, uint16_t verb, uint8_t payload, uint32_t* out_resp) {
    uint32_t cmd20 = ((uint32_t)(verb & 0x0FFFu) << 8) | (uint32_t)payload;
    return hda_send_cmd20(cad, nid, cmd20, out_resp);
}

static int hda_send_verb4(uint8_t cad, uint8_t nid, uint8_t verb4, uint16_t payload16, uint32_t* out_resp) {
    uint32_t cmd20 = ((uint32_t)(verb4 & 0x0Fu) << 16) | (uint32_t)payload16;
    return hda_send_cmd20(cad, nid, cmd20, out_resp);
}

static int hda_get_parameter(uint8_t cad, uint8_t nid, uint8_t param_id, uint32_t* out_val) {
    // GetParameter verb: 0xF00, payload = param_id
    return hda_send_verb(cad, nid, HDA_VERB_GET_PARAMETER, param_id, out_val);
}

static uint8_t hda_widget_type_from_awcap(uint32_t awcap) {
    return (uint8_t)((awcap >> 20) & 0x0Fu);
}

static int hda_get_awcap(uint8_t cad, uint8_t nid, uint32_t* out_awcap) {
    return hda_get_parameter(cad, nid, HDA_PARAM_AWCAP, out_awcap);
}

static uint8_t hda_find_afg(uint8_t cad) {
    uint32_t fg = 0;
    if (hda_get_parameter(cad, 0, HDA_PARAM_NODE_COUNT, &fg) != 0)
        return 0;

    uint8_t start = (uint8_t)((fg >> 16) & 0xFFu);
    uint8_t count = (uint8_t)(fg & 0xFFu);

    for (uint8_t i = 0; i < count; i++) {
        uint8_t nid = (uint8_t)(start + i);
        uint32_t t = 0;
        if (hda_get_parameter(cad, nid, HDA_PARAM_FG_TYPE, &t) != 0)
            continue;
        if ((t & 0xFFu) == 0x01u) {
            return nid;
        }
    }

    return 0;
}

static int hda_get_connections(uint8_t cad, uint8_t nid, uint8_t* out_list, int max, int* out_len) {
    if (!out_list || max <= 0 || !out_len)
        return -1;

    uint32_t cl = 0;
    if (hda_get_parameter(cad, nid, HDA_PARAM_CONN_LIST_LEN, &cl) != 0)
        return -1;

    int len = (int)(cl & 0x7Fu);
    bool long_form = (cl & 0x80u) != 0;

    if (len <= 0) {
        *out_len = 0;
        return 0;
    }

    int outc = 0;
    int step = long_form ? 2 : 4;
    for (int idx = 0; idx < len && outc < max; idx += step) {
        uint32_t resp = 0;
        if (hda_send_verb(cad, nid, HDA_VERB_GET_CONN_LIST_ENTRY, (uint8_t)idx, &resp) != 0)
            return -1;

        if (long_form) {
            uint16_t e0 = (uint16_t)(resp & 0xFFFFu);
            uint16_t e1 = (uint16_t)((resp >> 16) & 0xFFFFu);
            if (idx + 0 < len && outc < max) out_list[outc++] = (uint8_t)(e0 & 0xFFu);
            if (idx + 1 < len && outc < max) out_list[outc++] = (uint8_t)(e1 & 0xFFu);
        } else {
            uint8_t e0 = (uint8_t)(resp & 0xFFu);
            uint8_t e1 = (uint8_t)((resp >> 8) & 0xFFu);
            uint8_t e2 = (uint8_t)((resp >> 16) & 0xFFu);
            uint8_t e3 = (uint8_t)((resp >> 24) & 0xFFu);
            if (idx + 0 < len && outc < max) out_list[outc++] = e0;
            if (idx + 1 < len && outc < max) out_list[outc++] = e1;
            if (idx + 2 < len && outc < max) out_list[outc++] = e2;
            if (idx + 3 < len && outc < max) out_list[outc++] = e3;
        }
    }

    *out_len = outc;
    return 0;
}

static int hda_score_output_pin(uint8_t nid, uint32_t pincap, uint32_t cfg) {
    int score = 0;

    bool out_cap = (pincap & (1u << 4)) != 0;
    bool in_cap = (pincap & (1u << 5)) != 0;

    if (out_cap) score += 100;
    if (in_cap && !out_cap) score -= 10;

    if (cfg != 0) score += 5;

    uint8_t port_conn = (uint8_t)((cfg >> 30) & 0x3u);
    uint8_t dev_type = (uint8_t)((cfg >> 20) & 0xFu);

    // Prefer "connected" pins if the BIOS provided meaningful defaults,
    // but don't reject port_conn==0 outright (QEMU sometimes leaves it 0).
    if (port_conn == 0) score -= 5;
    else score += 5;

    // Device type preference (HD Audio config default "device type").
    if (dev_type == 0x0u) score += 50;      // line out
    else if (dev_type == 0x1u) score += 45; // speaker
    else if (dev_type == 0x2u) score += 40; // headphone
    else score += 10;

    if (nid == HDA_PREFERRED_PIN_NID) {
        score += 30;
    }

    return score;
}

static int hda_dfs_to_dac(uint8_t cad, uint8_t nid, uint8_t* path, int depth,
                          uint8_t* out_dac, int* out_path_len) {
    if (!path || !out_dac || !out_path_len)
        return -1;
    if (depth >= 10)
        return -1;

    // loop guard
    for (int i = 0; i < depth; i++) {
        if (path[i] == nid)
            return -1;
    }

    path[depth] = nid;

    uint32_t awcap = 0;
    if (hda_get_awcap(cad, nid, &awcap) != 0)
        return -1;
    uint8_t wtype = hda_widget_type_from_awcap(awcap);

    if (wtype == HDA_WTYPE_AUDIO_OUT) {
        *out_dac = nid;
        *out_path_len = depth + 1;
        return 0;
    }

    uint8_t conns[32];
    int nconn = 0;
    if (hda_get_connections(cad, nid, conns, (int)sizeof(conns), &nconn) != 0 || nconn <= 0)
        return -1;

    for (int idx = 0; idx < nconn; idx++) {
        uint8_t next = conns[idx];
        if (next == 0)
            continue;

        // For selector-like widgets (selectors and some pins), select the desired input.
        if (wtype == HDA_WTYPE_SELECTOR || wtype == HDA_WTYPE_PIN) {
            (void)hda_send_verb(cad, nid, HDA_VERB_SET_SELECTED_INPUT, (uint8_t)idx, NULL);
        }

        if (hda_dfs_to_dac(cad, next, path, depth + 1, out_dac, out_path_len) == 0)
            return 0;
    }

    return -1;
}

static int hda_follow_to_dac(uint8_t cad, uint8_t start_nid, uint8_t* out_dac, uint8_t* path, int* out_path_len) {
    if (!out_dac || !path || !out_path_len)
        return -1;
    *out_path_len = 0;
    return hda_dfs_to_dac(cad, start_nid, path, 0, out_dac, out_path_len);
}

static int hda_select_output_path(uint8_t cad, uint8_t afg,
                                  uint8_t* out_pin, uint8_t* out_dac, uint8_t* out_path, int* out_path_len) {
    if (!out_pin || !out_dac || !out_path || !out_path_len)
        return -1;

    uint32_t nodes = 0;
    if (hda_get_parameter(cad, afg, HDA_PARAM_NODE_COUNT, &nodes) != 0)
        return -1;

    uint8_t start = (uint8_t)((nodes >> 16) & 0xFFu);
    uint8_t count = (uint8_t)(nodes & 0xFFu);

    uint8_t pins[32];
    int scores[32];
    int npins = 0;

    if (g_hda_forced_pin != 0) {
        kprintf("[HDA] forcing pin nid=0x%02X\n", g_hda_forced_pin);
    }

    for (uint8_t i = 0; i < count; i++) {
        uint8_t nid = (uint8_t)(start + i);

        uint32_t awcap = 0;
        if (hda_get_awcap(cad, nid, &awcap) != 0)
            continue;
        if (hda_widget_type_from_awcap(awcap) != HDA_WTYPE_PIN)
            continue;

        if (g_hda_forced_pin != 0 && nid != g_hda_forced_pin) {
            continue;
        }

        uint32_t pincap = 0;
        (void)hda_get_parameter(cad, nid, HDA_PARAM_PIN_CAP, &pincap);

        uint32_t cfg = 0;
        (void)hda_send_verb(cad, nid, HDA_VERB_GET_PIN_CFG_DEFAULT, 0, &cfg);

        if (npins < (int)(sizeof(pins) / sizeof(pins[0]))) {
            int score = hda_score_output_pin(nid, pincap, cfg);
            const char* tag = (nid == HDA_PREFERRED_PIN_NID) ? " preferred" : "";
            kprintf("[HDA] pin nid=%u pincap=%08X cfg=%08X score=%d%s\n",
                    nid, pincap, cfg, score, tag);
            pins[npins] = nid;
            scores[npins] = score;
            npins++;
        }
    }

    if (npins == 0) {
        if (g_hda_forced_pin != 0) {
            kprint("[HDA] forced pin not found\n");
        }
        return -1;
    }

    if (g_hda_forced_pin != 0) {
        uint8_t pin = pins[0];
        uint8_t dac = 0;
        int path_len = 0;
        if (hda_follow_to_dac(cad, pin, &dac, out_path, &path_len) != 0 || path_len <= 0) {
            kprint("[HDA] forced pin has no DAC path\n");
            return -1;
        }
        *out_pin = pin;
        *out_dac = dac;
        *out_path_len = path_len;
        return 0;
    }

    // Prefer the known line-out pin if it yields a valid DAC path.
    for (int i = 0; i < npins; i++) {
        if (pins[i] != HDA_PREFERRED_PIN_NID) {
            continue;
        }
        uint8_t dac = 0;
        int path_len = 0;
        if (hda_follow_to_dac(cad, pins[i], &dac, out_path, &path_len) == 0 && path_len > 0) {
            *out_pin = pins[i];
            *out_dac = dac;
            *out_path_len = path_len;
            kprintf("[HDA] preferred pin 0x%02X selected\n", HDA_PREFERRED_PIN_NID);
            return 0;
        }
        kprintf("[HDA] preferred pin 0x%02X has no DAC path, falling back\n", HDA_PREFERRED_PIN_NID);
        scores[i] = -100000;
        break;
    }

    // Try higher-scored pins first; accept the first one that yields a DAC path.
    for (int attempt = 0; attempt < npins; attempt++) {
        int best_i = -1;
        int best_score = -100000;
        for (int i = 0; i < npins; i++) {
            if (scores[i] > best_score) {
                best_score = scores[i];
                best_i = i;
            }
        }
        if (best_i < 0)
            break;

        uint8_t pin = pins[best_i];
        scores[best_i] = -100000; // mark used

        uint8_t dac = 0;
        int path_len = 0;
        if (hda_follow_to_dac(cad, pin, &dac, out_path, &path_len) == 0 && path_len > 0) {
            *out_pin = pin;
            *out_dac = dac;
            *out_path_len = path_len;
            return 0;
        }
    }

    return -1;
}

static void hda_set_power_d0(uint8_t cad, uint8_t nid) {
    (void)hda_send_verb(cad, nid, HDA_VERB_SET_POWER_STATE, 0, NULL);
}

static void hda_set_pin_out_enable(uint8_t cad, uint8_t nid) {
    // Pin Widget Control: OUT enable is commonly bit 6.
    (void)hda_send_verb(cad, nid, HDA_VERB_SET_PIN_WIDGET_CONTROL, 0xC0u, NULL);
}

static void hda_set_eapd(uint8_t cad, uint8_t nid) {
    // Commonly bit 1 enables EAPD.
    (void)hda_send_verb(cad, nid, HDA_VERB_SET_EAPD_BTL, 0x02u, NULL);
}

static void hda_unmute_amp(uint8_t cad, uint8_t nid, bool output, uint8_t index, uint8_t gain) {
    uint16_t payload = (uint16_t)((output ? HDA_AMP_SET_OUTPUT : HDA_AMP_SET_INPUT) | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
                                  ((uint16_t)(index & 0x0Fu) << HDA_AMP_SET_INDEX_SHIFT) | (gain & HDA_AMP_SET_GAIN_MASK));
    (void)hda_send_verb4(cad, nid, HDA_VERB4_SET_AMP_GAIN_MUTE, payload, NULL);
}

static bool hda_alloc_dma(void) {
    if (g_hda->bdl)
        return true;

    hda_bdl_entry_t* bdl = (hda_bdl_entry_t*)kmalloc_aligned(sizeof(hda_bdl_entry_t) * HDA_BDL_ENTRIES, 1024);
    if (!bdl) {
        kprint("[HDA] kmalloc_aligned failed for BDL\n");
        return false;
    }
    memset(bdl, 0, sizeof(hda_bdl_entry_t) * HDA_BDL_ENTRIES);

    uint32_t bdl_phys = 0;
    if (vmm_virt_to_phys((uint32_t)bdl, &bdl_phys) != 0) {
        bdl_phys = (uint32_t)bdl;
    }
    if (bdl_phys & 0x3FFu) {
        kprintf("[HDA] BDL not 1KB aligned (phys=%08X)\n", bdl_phys);
        return false;
    }

    g_hda->bdl = bdl;
    g_hda->bdl_phys = bdl_phys;

    for (uint32_t i = 0; i < HDA_BDL_ENTRIES; i++) {
        uint32_t phys = 0;
        void* buf = kmalloc(HDA_BUFFER_BYTES, 1, &phys);
        if (!buf) {
            kprintf("[HDA] kmalloc failed for buffer %d\n", (int)i);
            return false;
        }
        g_hda->buffers[i] = buf;
        g_hda->buffers_phys[i] = phys;
        memset(buf, 0, HDA_BUFFER_BYTES);

        g_hda->bdl[i].addr = (uint64_t)phys;
        g_hda->bdl[i].len = HDA_BUFFER_BYTES;
        g_hda->bdl[i].ioc = 0;
    }

    return true;
}

static void hda_sd_stop(uint32_t sd_off) {
    uint8_t ctl0 = hda_rd8(sd_off + HDA_SD_CTL0);
    ctl0 &= (uint8_t)~0x02u;
    hda_wr8(sd_off + HDA_SD_CTL0, ctl0);
}

static bool hda_sd_reset(uint32_t sd_off) {
    uint8_t ctl0 = hda_rd8(sd_off + HDA_SD_CTL0);
    ctl0 &= (uint8_t)~0x02u; // ensure run=0
    hda_wr8(sd_off + HDA_SD_CTL0, ctl0 | 0x01u);

    uint32_t spins = 0;
    while (((hda_rd8(sd_off + HDA_SD_CTL0) & 0x01u) == 0) && spins++ < 1000000u) {
        hal_pause();
    }
    if ((hda_rd8(sd_off + HDA_SD_CTL0) & 0x01u) == 0) {
        kprint("[HDA] stream reset set timeout\n");
        return false;
    }

    hda_wr8(sd_off + HDA_SD_CTL0, ctl0 & (uint8_t)~0x01u);
    spins = 0;
    while (((hda_rd8(sd_off + HDA_SD_CTL0) & 0x01u) != 0) && spins++ < 1000000u) {
        hal_pause();
    }
    if (hda_rd8(sd_off + HDA_SD_CTL0) & 0x01u) {
        kprint("[HDA] stream reset clear timeout\n");
        return false;
    }

    return true;
}

static bool hda_sd_start(uint32_t sd_off, uint8_t stream_id, uint16_t fmt, uint32_t cbl_bytes, uint8_t lvi,
                         uint32_t bdl_phys) {
    hda_sd_stop(sd_off);
    if (!hda_sd_reset(sd_off))
        return false;

    // Clear sticky status bits
    hda_wr8(sd_off + HDA_SD_STS, 0x1Fu);

    hda_wr32(sd_off + HDA_SD_BDPL, bdl_phys & ~0x3FFu);
    hda_wr32(sd_off + HDA_SD_BDPU, 0);
    hda_wr32(sd_off + HDA_SD_CBL, cbl_bytes);
    hda_wr16(sd_off + HDA_SD_LVI, lvi);
    hda_wr16(sd_off + HDA_SD_FMT, fmt);

    {
        uint16_t ctl = hda_rd16(sd_off + HDA_SD_CTL0);
        ctl &= (uint16_t)~(0x0Fu << 8);
        ctl |= (uint16_t)((stream_id & 0x0Fu) << 8);
        hda_wr16(sd_off + HDA_SD_CTL0, ctl);
    }

    uint8_t ctl0 = hda_rd8(sd_off + HDA_SD_CTL0);
    ctl0 |= 0x02u; // run
    hda_wr8(sd_off + HDA_SD_CTL0, ctl0);

    // Ensure LPIB starts moving (DMA engine running)
    uint32_t lp0 = hda_rd32(sd_off + HDA_SD_LPIB);
    uint32_t spins = 0;
    while (spins++ < 2000000u) {
        uint32_t lp1 = hda_rd32(sd_off + HDA_SD_LPIB);
        if (lp1 != lp0)
            return true;
        hal_pause();
    }

    kprint("[HDA] LPIB did not advance\n");
    return false;
}

static void hda_sd_halt(uint32_t sd_off) {
    hda_sd_stop(sd_off);
    hda_wr8(sd_off + HDA_SD_STS, 0x1Fu);
}

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

static void hda_fill_tone_buffer(uint32_t buf_index, uint16_t* phase, uint16_t step) {
    int16_t* out = (int16_t*)g_hda->buffers[buf_index];
    const uint32_t frames = HDA_BUFFER_BYTES / 4u; // 2ch * 16-bit

    for (uint32_t i = 0; i < frames; i++) {
        uint8_t idx = (uint8_t)(*phase >> 8);
        int16_t s = k_sine_256[idx];
        *phase = (uint16_t)(*phase + step);
        out[i * 2u + 0u] = s;
        out[i * 2u + 1u] = s;
    }
}

static bool hda_setup_output_path(void) {
    if (!g_hda || !g_hda->present)
        return false;

    if (g_hda->play_ready)
        return true;

    // Pick first codec present.
    uint8_t cad = 0xFFu;
    for (uint8_t i = 0; i < 15; i++) {
        if (g_hda->codecs_mask & (1u << i)) {
            cad = i;
            break;
        }
    }
    if (cad == 0xFFu) {
        kprint("[HDA] no codecs\n");
        return false;
    }

    uint8_t afg = hda_find_afg(cad);
    if (afg == 0) {
        kprint("[HDA] no AFG found\n");
        return false;
    }

    // Reset the audio function group nodes.
    (void)hda_send_verb(cad, afg, HDA_VERB_AFG_RESET, 0, NULL);

    uint8_t path[16];
    int path_len = 0;
    uint8_t pin = 0;
    uint8_t dac = 0;
    if (hda_select_output_path(cad, afg, &pin, &dac, path, &path_len) != 0) {
        kprint("[HDA] failed to find output pin/DAC path\n");
        return false;
    }

    // Power up + basic unmute along the discovered path.
    hda_set_power_d0(cad, afg);
    for (int i = 0; i < path_len; i++) {
        hda_set_power_d0(cad, path[i]);
        // Best-effort unmute/max-gain: open a wider range of input indices.
        for (uint8_t in_ix = 0; in_ix < 16; in_ix++) {
            hda_unmute_amp(cad, path[i], false, in_ix, 0x7Fu);
        }
        hda_unmute_amp(cad, path[i], true, 0, 0x7Fu);
    }

    hda_set_pin_out_enable(cad, pin);
    hda_set_eapd(cad, pin);

    g_hda->play_ready = true;
    g_hda->play_cad = cad;
    g_hda->play_afg = afg;
    g_hda->play_pin = pin;
    g_hda->play_dac = dac;
    g_hda->play_stream_id = 1; // stream 1 (0 is reserved)

    // Stream descriptors are laid out: input, then output, then bidirectional.
    // Select the first output stream descriptor.
    uint8_t in_streams = (uint8_t)((g_hda->gcap >> 8) & 0xFu);
    uint8_t out_streams = (uint8_t)((g_hda->gcap >> 12) & 0xFu);
    if (out_streams == 0) {
        kprint("[HDA] no output streams reported\n");
        return false;
    }
    g_hda->sd_off = HDA_REG_SD_BASE + (uint32_t)HDA_REG_SD_SIZE * (uint32_t)in_streams;

    kprintf("[HDA] output path: cad=%d afg=%d pin=%d dac=%d sd_off=%X\n", cad, afg, pin, dac, g_hda->sd_off);
    return true;
}

static void hda_probe_codecs(void) {
    uint16_t st = hda_rd16(HDA_REG_STATESTS);
    g_hda->codecs_mask = (uint16_t)(st & 0x7FFFu);

    if (g_hda->codecs_mask == 0) {
        kprint("[HDA] No codecs reported in STATESTS\n");
        return;
    }

    for (uint8_t cad = 0; cad < 15; cad++) {
        if ((g_hda->codecs_mask & (1u << cad)) == 0)
            continue;

        uint32_t vendor = 0;
        if (hda_get_parameter(cad, 0, HDA_PARAM_VENDOR_ID, &vendor) != 0) {
            kprintf("[HDA] Codec %d: failed to read vendor id\n", cad);
            continue;
        }

        g_hda->codec_vendor[cad] = vendor;

        uint32_t sub = 0;
        (void)hda_get_parameter(cad, 0, HDA_PARAM_NODE_COUNT, &sub);
        uint8_t start_nid = (uint8_t)((sub >> 16) & 0xFFu);
        uint8_t count = (uint8_t)(sub & 0xFFu);

        kprintf("[HDA] Codec %d: vendor=%08X (root children start=%d count=%d)\n",
                cad, vendor, start_nid, count);
    }
}

bool hda_is_present(void) {
    return (g_hda && g_hda->present);
}

int hda_get_count(void) {
    return g_hda_count;
}

int hda_get_active_index(void) {
    return g_hda_active;
}

bool hda_select(int index) {
    if (index < 0 || index >= g_hda_count)
        return false;
    g_hda = &g_hda_devs[index];
    g_hda_active = index;
    return g_hda->present;
}

void hda_set_forced_pin(uint8_t nid) {
    g_hda_forced_pin = nid;
    if (g_hda) {
        g_hda->play_ready = false;
    }
}

uint8_t hda_get_forced_pin(void) {
    return g_hda_forced_pin;
}

void hda_list(void) {
    kprintf("[HDA] controllers: %d\n", g_hda_count);
    for (int i = 0; i < g_hda_count; i++) {
        hda_dev_t* dev = &g_hda_devs[i];
        const char* mark = (i == g_hda_active) ? "*" : " ";
        kprintf("[HDA]%s %d: %u:%u.%u MMIO=%08X IRQ=%u\n",
                mark, i, dev->bus, dev->dev, dev->func, dev->mmio_base, dev->irq_line);
    }
}

void hda_stop(void) {
    if (!g_hda || !g_hda->present || !g_hda->mmio)
        return;
    if (!g_hda->play_ready || g_hda->sd_off < HDA_REG_SD_BASE)
        return;

    hda_sd_halt(g_hda->sd_off);
}

int hda_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    if (!g_hda || !g_hda->present) {
        kprint("[HDA] not present\n");
        return -1;
    }
    if (freq_hz == 0 || duration_ms == 0) {
        return 0;
    }
    if (freq_hz > (HDA_SAMPLE_RATE / 2u)) {
        kprint("[HDA] freq too high\n");
        return -1;
    }
    if (duration_ms > 60000u) {
        // Keep arithmetic 32-bit friendly; this is a bring-up tone helper.
        duration_ms = 60000u;
    }

    if (!hda_setup_output_path()) {
        return -1;
    }

    if (!hda_alloc_dma()) {
        return -1;
    }

    // Configure the DAC with format + stream id/channel.
    uint8_t cad = g_hda->play_cad;
    uint8_t dac = g_hda->play_dac;
    uint8_t stream_id = g_hda->play_stream_id;
    uint16_t fmt = (uint16_t)HDA_STREAM_FORMAT_48K_16B_2CH;

    // Set converter format (verb 0x2, 16-bit payload = stream format)
    (void)hda_send_verb4(cad, dac, HDA_VERB4_SET_CONV_FORMAT, fmt, NULL);
    // 2 channels (payload is typically channel count - 1)
    (void)hda_send_verb(cad, dac, HDA_VERB_SET_OUTPUT_CONV_CHAN_CNT, (uint8_t)(HDA_OUT_CHANNELS - 1u), NULL);
    // Set stream id + starting channel (channel 0)
    (void)hda_send_verb(cad, dac, HDA_VERB_SET_CONV_STREAM_CHAN, (uint8_t)((stream_id << 4) | 0u), NULL);

    // Fill buffers with a continuous sine wave.
    uint16_t step = (uint16_t)(((uint32_t)freq_hz * 65536u) / (uint32_t)HDA_SAMPLE_RATE);
    uint16_t phase = 0;
    for (uint32_t i = 0; i < HDA_BDL_ENTRIES; i++) {
        hda_fill_tone_buffer(i, &phase, step);
    }

    // Flush caches so the device sees the updated buffers/BDL.
    hal_wbinvd();

    uint32_t cbl_bytes = HDA_BUFFER_BYTES * HDA_BDL_ENTRIES;
    uint8_t lvi = (uint8_t)(HDA_BDL_ENTRIES - 1u);

    if (!hda_sd_start(g_hda->sd_off, stream_id, fmt, cbl_bytes, lvi, g_hda->bdl_phys)) {
        hda_sd_halt(g_hda->sd_off);
        return -1;
    }

    // Run for the requested duration by tracking LPIB progress (no timer IRQ dependency).
    uint32_t total_frames = (duration_ms * HDA_SAMPLE_RATE + 999u) / 1000u;
    uint32_t target_bytes = total_frames * (HDA_OUT_CHANNELS * 2u);
    if (target_bytes == 0)
        target_bytes = (HDA_OUT_CHANNELS * 2u);

    uint32_t prev_lpib = hda_rd32(g_hda->sd_off + HDA_SD_LPIB) % cbl_bytes;
    uint32_t played = 0;

    while (played < target_bytes) {
        uint32_t cur_lpib = hda_rd32(g_hda->sd_off + HDA_SD_LPIB) % cbl_bytes;
        if (cur_lpib != prev_lpib) {
            uint32_t delta = (cur_lpib >= prev_lpib) ? (cur_lpib - prev_lpib) : ((cbl_bytes - prev_lpib) + cur_lpib);
            played += delta;
            prev_lpib = cur_lpib;
        } else {
            uint8_t ctl0 = hda_rd8(g_hda->sd_off + HDA_SD_CTL0);
            if ((ctl0 & 0x02u) == 0) {
                kprint("[HDA] stream halted\n");
                break;
            }
            hal_pause();
        }
    }

    hda_sd_halt(g_hda->sd_off);
    return 0;
}

typedef struct {
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} hda_wav_fmt_t;

typedef struct {
    const uint8_t* data;
    uint32_t data_size;
    hda_wav_fmt_t fmt;

    uint32_t src_frames;

    uint64_t src_pos_fp; // 16.16 fixed point in source frames
    uint32_t step_fp;    // 16.16 source frames per output frame (src_rate/dst_rate)
} hda_wav_state_t;

typedef struct {
    uint32_t bytes;
    bool end;
} hda_wav_fill_result_t;

static inline uint16_t hda_wav_le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t hda_wav_le32(const uint8_t* p) {
    return (uint32_t)((uint32_t)p[0]
                    | ((uint32_t)p[1] << 8)
                    | ((uint32_t)p[2] << 16)
                    | ((uint32_t)p[3] << 24));
}

static bool hda_wav_parse(const uint8_t* wav, uint32_t wav_size, hda_wav_fmt_t* out_fmt, const uint8_t** out_data,
                          uint32_t* out_data_size) {
    if (!wav || wav_size < 12u || !out_fmt || !out_data || !out_data_size) {
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
        uint32_t chunk_size = hda_wav_le32(chunk + 4);
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

            out_fmt->audio_format = hda_wav_le16(wav + pos + 0);
            out_fmt->channels = hda_wav_le16(wav + pos + 2);
            out_fmt->sample_rate = hda_wav_le32(wav + pos + 4);
            out_fmt->block_align = hda_wav_le16(wav + pos + 12);
            out_fmt->bits_per_sample = hda_wav_le16(wav + pos + 14);
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

static int32_t hda_wav_sample_at(const hda_wav_state_t* st, uint32_t frame, uint16_t channel) {
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
        return (int16_t)hda_wav_le16(p);
    }

    return 0;
}

static inline int16_t hda_clamp_s16(int32_t x) {
    if (x > 32767)
        return 32767;
    if (x < -32768)
        return -32768;
    return (int16_t)x;
}

static int16_t hda_wav_interp(const hda_wav_state_t* st, uint32_t src_index, uint32_t frac, uint16_t channel) {
    int32_t s0 = hda_wav_sample_at(st, src_index, channel);
    int32_t s1 = s0;
    if ((src_index + 1u) < st->src_frames) {
        s1 = hda_wav_sample_at(st, src_index + 1u, channel);
    }

    int32_t diff = s1 - s0;
    int32_t frac15 = (int32_t)(frac >> 1); // 0..32767 to keep 32-bit math safe
    int32_t out = s0 + ((diff * frac15) >> 15);
    return hda_clamp_s16(out);
}

static hda_wav_fill_result_t hda_fill_wav_buffer(uint32_t buf_index, hda_wav_state_t* st) {
    hda_wav_fill_result_t res;
    res.bytes = 0;
    res.end = false;

    int16_t* out = (int16_t*)g_hda->buffers[buf_index];
    const uint32_t out_frames = HDA_BUFFER_BYTES / 4u; // 2ch * 16-bit

    uint32_t frames_written = 0;

    for (uint32_t i = 0; i < out_frames; i++) {
        uint32_t src_index = (uint32_t)(st->src_pos_fp >> 16);
        if (src_index >= st->src_frames) {
            res.end = true;
            break;
        }

        uint32_t frac = (uint32_t)(st->src_pos_fp & 0xFFFFull);

        int16_t l = hda_wav_interp(st, src_index, frac, 0);
        int16_t r = l;
        if (st->fmt.channels >= 2u) {
            r = hda_wav_interp(st, src_index, frac, 1);
        }

        out[i * 2u + 0u] = l;
        out[i * 2u + 1u] = r;

        st->src_pos_fp += st->step_fp;
        frames_written++;
    }

    // Zero any remaining frames in the buffer (silence padding).
    for (uint32_t i = frames_written; i < out_frames; i++) {
        out[i * 2u + 0u] = 0;
        out[i * 2u + 1u] = 0;
    }

    if ((uint32_t)(st->src_pos_fp >> 16) >= st->src_frames) {
        res.end = true;
    }

    res.bytes = frames_written * (HDA_OUT_CHANNELS * 2u);
    return res;
}

int hda_play_wav(const uint8_t* wav, uint32_t wav_size) {
    if (!g_hda || !g_hda->present) {
        kprint("[HDA] not present\n");
        return -1;
    }
    if (!wav || wav_size < 12u) {
        kprint("[HDA] invalid wav\n");
        return -1;
    }

    hda_wav_fmt_t fmt;
    const uint8_t* data = NULL;
    uint32_t data_size = 0;
    if (!hda_wav_parse(wav, wav_size, &fmt, &data, &data_size)) {
        kprint("[HDA] wav parse failed\n");
        return -1;
    }

    if (fmt.audio_format != 1u) {
        kprintf("[HDA] unsupported wav format: %u\n", (uint32_t)fmt.audio_format);
        return -1;
    }
    if (fmt.channels < 1u || fmt.channels > 2u) {
        kprintf("[HDA] unsupported channels: %u\n", (uint32_t)fmt.channels);
        return -1;
    }
    if (fmt.bits_per_sample != 8u && fmt.bits_per_sample != 16u) {
        kprintf("[HDA] unsupported bits: %u\n", (uint32_t)fmt.bits_per_sample);
        return -1;
    }
    if (fmt.sample_rate < 8000u || fmt.sample_rate > 192000u) {
        kprintf("[HDA] unsupported sample rate: %u\n", fmt.sample_rate);
        return -1;
    }
    if (fmt.block_align == 0u || data_size < fmt.block_align) {
        kprint("[HDA] invalid wav data\n");
        return -1;
    }

    if (!hda_setup_output_path()) {
        return -1;
    }
    if (!hda_alloc_dma()) {
        return -1;
    }

    // Reset descriptor fields (may have been tweaked by a previous run).
    for (uint32_t i = 0; i < HDA_BDL_ENTRIES; i++) {
        g_hda->bdl[i].len = HDA_BUFFER_BYTES;
        g_hda->bdl[i].ioc = 0;
    }

    hda_wav_state_t st;
    memset(&st, 0, sizeof(st));
    st.data = data;
    st.data_size = data_size;
    st.fmt = fmt;
    st.src_frames = data_size / (uint32_t)fmt.block_align;
    if (st.src_frames == 0u) {
        kprint("[HDA] empty wav\n");
        return -1;
    }

    // step_fp = (src_rate / dst_rate) in 16.16 without 64-bit division
    {
        uint32_t q = fmt.sample_rate / HDA_SAMPLE_RATE;
        uint32_t r = fmt.sample_rate % HDA_SAMPLE_RATE;
        st.step_fp = (q << 16) + (r * 65536u) / HDA_SAMPLE_RATE;
        if (st.step_fp == 0u) {
            st.step_fp = 1u;
        }
    }
    st.src_pos_fp = 0;

    // Configure the DAC with format + stream id/channel.
    uint8_t cad = g_hda->play_cad;
    uint8_t dac = g_hda->play_dac;
    uint8_t stream_id = g_hda->play_stream_id;
    uint16_t stream_fmt = (uint16_t)HDA_STREAM_FORMAT_48K_16B_2CH;

    (void)hda_send_verb4(cad, dac, HDA_VERB4_SET_CONV_FORMAT, stream_fmt, NULL);
    (void)hda_send_verb(cad, dac, HDA_VERB_SET_OUTPUT_CONV_CHAN_CNT, (uint8_t)(HDA_OUT_CHANNELS - 1u), NULL);
    (void)hda_send_verb(cad, dac, HDA_VERB_SET_CONV_STREAM_CHAN, (uint8_t)((stream_id << 4) | 0u), NULL);

    uint32_t generated = 0;
    bool end = false;
    uint32_t total_audio_bytes = 0;

    // Prefill ring
    for (uint32_t i = 0; i < HDA_BDL_ENTRIES; i++) {
        hda_wav_fill_result_t fr = hda_fill_wav_buffer(i, &st);
        generated += fr.bytes;
        if (fr.end) {
            end = true;
            total_audio_bytes = generated;
            // Avoid stale audio if we overshoot into later buffers.
            for (uint32_t j = i + 1u; j < HDA_BDL_ENTRIES; j++) {
                memset(g_hda->buffers[j], 0, HDA_BUFFER_BYTES);
            }
            break;
        }
    }

    hal_wbinvd();

    uint32_t cbl_bytes = HDA_BUFFER_BYTES * HDA_BDL_ENTRIES;
    uint8_t lvi = (uint8_t)(HDA_BDL_ENTRIES - 1u);

    if (!hda_sd_start(g_hda->sd_off, stream_id, stream_fmt, cbl_bytes, lvi, g_hda->bdl_phys)) {
        hda_sd_halt(g_hda->sd_off);
        return -1;
    }

    uint32_t prev_lpib = hda_rd32(g_hda->sd_off + HDA_SD_LPIB) % cbl_bytes;
    uint32_t played = 0;
    uint8_t last_buf = (uint8_t)(prev_lpib >> 12);

    // Stream: refill completed buffers until EOF, then keep feeding silence and stop once all audio bytes were played.
    while (1) {
        uint32_t cur_lpib = hda_rd32(g_hda->sd_off + HDA_SD_LPIB) % cbl_bytes;

        if (cur_lpib != prev_lpib) {
            uint32_t delta = (cur_lpib >= prev_lpib) ? (cur_lpib - prev_lpib) : ((cbl_bytes - prev_lpib) + cur_lpib);
            played += delta;
            prev_lpib = cur_lpib;
        }

        uint8_t cur_buf = (uint8_t)(cur_lpib >> 12);
        if (cur_buf != last_buf) {
            uint8_t completed = last_buf;

            if (!end) {
                hda_wav_fill_result_t fr = hda_fill_wav_buffer(completed, &st);
                generated += fr.bytes;
                if (fr.end) {
                    end = true;
                    total_audio_bytes = generated;
                }
            } else {
                memset(g_hda->buffers[completed], 0, HDA_BUFFER_BYTES);
            }

            hal_wbinvd();
            last_buf = cur_buf;
        }

        if (end && played >= total_audio_bytes) {
            break;
        }

        uint8_t ctl0 = hda_rd8(g_hda->sd_off + HDA_SD_CTL0);
        if ((ctl0 & 0x02u) == 0) {
            kprint("[HDA] wav: stream halted\n");
            break;
        }

        hal_pause();
    }

    hda_sd_halt(g_hda->sd_off);
    return 0;
}

static bool hda_pci_attach_internal(uint8_t bus, uint8_t device, uint8_t function, bool force_class) {
    /* --- Class check: Multimedia / Audio --- */
    uint32_t class_reg = pci_read_dword(bus, device, function, 0x08);
    uint8_t class_code = (class_reg >> 24) & 0xFF;
    uint8_t subclass   = (class_reg >> 16) & 0xFF;

    if (!force_class && (class_code != 0x04 || subclass != 0x03))
        return false;

    if (g_hda_count >= HDA_MAX_DEVS) {
        kprint("[HDA] Max controllers reached, skipping attach\n");
        return false;
    }

    /* --- BAR0: MMIO only (no I/O BAR) --- */
    uint32_t bar0 = pci_read_dword(bus, device, function, 0x10);
    if (bar0 & 0x1u) {
        kprintf("[HDA] BAR0 is I/O space (%08X), skipping\n", bar0);
        return false;
    }

    uint64_t mmio_base = 0;
    uint8_t mem_type = (uint8_t)((bar0 >> 1) & 0x3u);

    if (mem_type == 0x2u) {
        /* 64-bit MMIO BAR (VMware / real hardware) */
        uint32_t bar1 = pci_read_dword(bus, device, function, 0x14);
        if (bar1 != 0) {
            kprintf("[HDA] 64-bit BAR above 4GiB (BAR1=%08X), skipping\n", bar1);
            return false;
        }
        mmio_base = (uint64_t)(bar0 & ~0xFu);
    } else {
        /* 32-bit MMIO BAR (QEMU) */
        mmio_base = (uint64_t)(bar0 & ~0xFu);
    }

    if (mmio_base == 0) {
        kprint("[HDA] MMIO base is 0, skipping attach\n");
        return false;
    }
    if (mmio_base > 0xFFFFFFFFu) {
        kprintf("[HDA] MMIO base above 4GiB (%016llX), skipping\n", mmio_base);
        return false;
    }

    /* --- Enable MMIO + Bus Mastering --- */
    uint32_t cmdsts = pci_read_dword(bus, device, function, 0x04);
    cmdsts |= (1u << 1); /* Memory Space */
    cmdsts |= (1u << 2); /* Bus Master */
    pci_write_dword(bus, device, function, 0x04, cmdsts);

    /* --- IRQ line (legacy; polling is fine) --- */
    uint32_t irq_reg = pci_read_dword(bus, device, function, 0x3C);
    uint8_t irq_line = (uint8_t)(irq_reg & 0xFFu);

    /* --- Initialize device struct --- */
    hda_dev_t* dev = &g_hda_devs[g_hda_count];
    memset(dev, 0, sizeof(*dev));
    dev->present   = true;
    dev->bus       = bus;
    dev->dev       = device;
    dev->func      = function;
    dev->irq_line  = irq_line;
    dev->mmio_base = mmio_base;
    dev->mmio = (volatile uint8_t*)(uintptr_t)mmio_base;

    int prev_active = g_hda_active;
    hda_dev_t* prev_dev = g_hda;

    g_hda = dev;
    g_hda_active = g_hda_count;

    /* --- Map MMIO (global + streams + CORB/RIRB safe range) --- */
    map_mmio((uint32_t)mmio_base, 0x4000u);

    /* --- Read version / capabilities --- */
    g_hda->gcap = hda_rd32(HDA_REG_GCAP);
    g_hda->vmin = hda_rd8(HDA_REG_VMIN);
    g_hda->vmaj = hda_rd8(HDA_REG_VMAJ);

    kprintf("[HDA] Found controller at %u:%u.%u\n", bus, device, function);
    kprintf("[HDA] MMIO=%016llX IRQ=%u version=%u.%u GCAP=%08X\n",
            g_hda->mmio_base, g_hda->irq_line, g_hda->vmaj, g_hda->vmin, g_hda->gcap);

    /* --- Reset controller --- */
    if (!hda_controller_reset()) {
        kprint("[HDA] Controller reset failed\n");
        g_hda->present = false;
        g_hda = prev_dev;
        g_hda_active = prev_active;
        return false;
    }

    if (!hda_wait_state_sts(200)) {
        kprint("[HDA] STATESTS remained 0 after reset\n");
    }

    /* --- Detect codecs --- */
    hda_probe_codecs();

    g_hda_count++;
    if (prev_active < 0) {
        g_hda = dev;
        g_hda_active = g_hda_count - 1;
    } else {
        g_hda = prev_dev;
        g_hda_active = prev_active;
    }

    return true;
}

bool hda_pci_attach(uint8_t bus, uint8_t device, uint8_t function) {
    return hda_pci_attach_internal(bus, device, function, false);
}

bool hda_pci_attach_force(uint8_t bus, uint8_t device, uint8_t function) {
    return hda_pci_attach_internal(bus, device, function, true);
}

void hda_dump(void) {
    if (!g_hda || !g_hda->present || !g_hda->mmio) {
        kprint("[HDA] not present\n");
        return;
    }

    uint32_t gctl = hda_rd32(HDA_REG_GCTL);
    uint16_t st = hda_rd16(HDA_REG_STATESTS);
    uint32_t intctl = hda_rd32(HDA_REG_INTCTL);
    uint32_t intsts = hda_rd32(HDA_REG_INTSTS);

    kprintf("[HDA] %d:%d.%d MMIO=%08X IRQ=%d\n",
            g_hda->bus, g_hda->dev, g_hda->func, g_hda->mmio_base, g_hda->irq_line);
    kprintf("[HDA] GCAP=%08X GCTL=%08X STATESTS=%04X INTCTL=%08X INTSTS=%08X\n",
            g_hda->gcap, gctl, st, intctl, intsts);

    kprintf("[HDA] codecs mask=%04X\n", g_hda->codecs_mask);
    for (int cad = 0; cad < 15; cad++) {
        if (g_hda->codecs_mask & (1u << cad)) {
            kprintf("[HDA] codec %d vendor=%08X\n", cad, g_hda->codec_vendor[cad]);
        }
    }
    if (g_hda_forced_pin != 0) {
        kprintf("[HDA] forced pin=0x%02X (preferred=0x%02X)\n", g_hda_forced_pin, HDA_PREFERRED_PIN_NID);
    } else {
        kprintf("[HDA] forced pin=off (preferred=0x%02X)\n", HDA_PREFERRED_PIN_NID);
    }
}
