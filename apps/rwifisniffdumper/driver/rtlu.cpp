/*
 * rtlu.cpp: bring-up of the RTL8192CU family as a receive-only monitor.
 * Follows Linux rtlwifi rtl8192cu (hw.c, mac.c, phy.c, rf.c), rtl8192c
 * (phy_common.c) and efuse.c, minus everything that transmits or only
 * matters for transmitting: no firmware, no TX DMA or MAC TX engine, no IQ
 * calibration, no TX power, PA bias, EDCA or rate fallback setup.
 */
#include "rtlu_int.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// register access

static uint32_t Read(rtlu_dev *d, uint16_t aAddr, int aLen)
{
    uint8_t b[4] = { 0, 0, 0, 0 };
    if (d->io->read(d->io->ctx, aAddr, b, aLen) != 0) {
        d->io_errors++;
        return 0;
    }
    return b[0] | (b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void Write(rtlu_dev *d, uint16_t aAddr, uint32_t aVal, int aLen)
{
    uint8_t b[4];
    b[0] = (uint8_t)aVal;
    b[1] = (uint8_t)(aVal >> 8);
    b[2] = (uint8_t)(aVal >> 16);
    b[3] = (uint8_t)(aVal >> 24);
    if (d->io->write(d->io->ctx, aAddr, b, aLen) != 0)
        d->io_errors++;
}

static uint8_t R8(rtlu_dev *d, uint16_t a) { return (uint8_t)Read(d, a, 1); }
static uint16_t R16(rtlu_dev *d, uint16_t a) { return (uint16_t)Read(d, a, 2); }
static uint32_t R32(rtlu_dev *d, uint16_t a) { return Read(d, a, 4); }
static void W8(rtlu_dev *d, uint16_t a, uint32_t v) { Write(d, a, v, 1); }
static void W16(rtlu_dev *d, uint16_t a, uint32_t v) { Write(d, a, v, 2); }
static void W32(rtlu_dev *d, uint16_t a, uint32_t v) { Write(d, a, v, 4); }

static void Sleep(rtlu_dev *d, uint32_t aUs)
{
    d->io->sleep_us(d->io->ctx, aUs);
}

static void Log(rtlu_dev *d, const char *aFmt, ...)
{
    if (!d->io->log) return;
    char buf[160];
    va_list ap;
    va_start(ap, aFmt);
    vsnprintf(buf, sizeof buf, aFmt, ap);
    va_end(ap);
    d->io->log(d->io->ctx, buf);
}

// ---------------------------------------------------------------------------
// baseband and RF registers

static int BitShift(uint32_t aMask)
{
    int s = 0;
    while (s < 31 && !(aMask & (1u << s))) s++;
    return s;
}

static uint32_t GetBB(rtlu_dev *d, uint16_t aAddr, uint32_t aMask)
{
    return (R32(d, aAddr) & aMask) >> BitShift(aMask);
}

static void SetBB(rtlu_dev *d, uint16_t aAddr, uint32_t aMask, uint32_t aData)
{
    if (aMask != MASKDWORD)
        aData = (R32(d, aAddr) & ~aMask) | (aData << BitShift(aMask));
    W32(d, aAddr, aData);
}

// Per-path baseband registers that reach the RF chip (Linux phyreg_def).
struct PathRegs
    {
    uint16_t hssi1, hssi2, lssi, rb, rbpi, intfs, intfo;
    uint32_t env;       // the path's BRFSI_RFENV bit in intfs
    };

static const PathRegs KPath[2] = {
    { RFPGA0_XA_HSSIPARAMETER1, RFPGA0_XA_HSSIPARAMETER2, RFPGA0_XA_LSSIPARAMETER,
      RFPGA0_XA_LSSIREADBACK, TRANSCEIVEA_HSPI_READBACK, RFPGA0_XAB_RFINTERFACESW,
      RFPGA0_XA_RFINTERFACEOE, BRFSI_RFENV },
    { RFPGA0_XB_HSSIPARAMETER1, RFPGA0_XB_HSSIPARAMETER2, RFPGA0_XB_LSSIPARAMETER,
      RFPGA0_XB_LSSIREADBACK, TRANSCEIVEB_HSPI_READBACK, RFPGA0_XAB_RFINTERFACESW,
      RFPGA0_XB_RFINTERFACEOE, BRFSI_RFENV << 16 }
};

// RF reads go through the baseband's 3-wire interface (LSSI).
static uint32_t RfRead(rtlu_dev *d, int aPath, uint32_t aOffset)
{
    const PathRegs *p = &KPath[aPath];
    aOffset &= 0x3f;
    uint32_t a = GetBB(d, RFPGA0_XA_HSSIPARAMETER2, MASKDWORD);
    uint32_t b = aPath == 0 ? a : GetBB(d, p->hssi2, MASKDWORD);
    b = (b & ~BLSSIREADADDRESS) | (aOffset << 23) | BLSSIREADEDGE;
    SetBB(d, RFPGA0_XA_HSSIPARAMETER2, MASKDWORD, a & ~BLSSIREADEDGE);
    Sleep(d, 1000);
    SetBB(d, p->hssi2, MASKDWORD, b);
    Sleep(d, 1000);
    SetBB(d, RFPGA0_XA_HSSIPARAMETER2, MASKDWORD, a | BLSSIREADEDGE);
    Sleep(d, 1000);
    int pi = GetBB(d, p->hssi1, BIT(8));
    return GetBB(d, pi ? p->rbpi : p->rb, BLSSIREADBACKDATA);
}

static void RfWrite(rtlu_dev *d, int aPath, uint32_t aOffset, uint32_t aData)
{
    uint32_t v = (((aOffset & 0x3f) << 20) | (aData & 0xfffff)) & 0x0fffffff;
    SetBB(d, KPath[aPath].lssi, MASKDWORD, v);
}

static uint32_t GetRF(rtlu_dev *d, int aPath, uint32_t aAddr, uint32_t aMask)
{
    return (RfRead(d, aPath, aAddr) & aMask) >> BitShift(aMask);
}

static void SetRF(rtlu_dev *d, int aPath, uint32_t aAddr, uint32_t aMask,
                  uint32_t aData)
{
    if (aMask != RFREG_OFFSET_MASK)
        aData = (RfRead(d, aPath, aAddr) & ~aMask) | (aData << BitShift(aMask));
    RfWrite(d, aPath, aAddr, aData);
}

// Table addresses 0xf9-0xfe are delays, not registers (Linux rtl_addr_delay).
static int DelayEntry(rtlu_dev *d, uint32_t aAddr)
{
    switch (aAddr) {
    case 0xfe: Sleep(d, 50000); return 1;
    case 0xfd: Sleep(d, 5000); return 1;
    case 0xfc: Sleep(d, 1000); return 1;
    case 0xfb: Sleep(d, 100); return 1;
    case 0xfa: Sleep(d, 10); return 1;
    case 0xf9: Sleep(d, 2); return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// chip identity and EFUSE

static void ReadChipVersion(rtlu_dev *d)
{
    rtlu_chip *c = &d->chip;
    uint32_t v = R32(d, REG_SYS_CFG);
    c->is_92c = (v & TYPE_ID) != 0;
    if (v & TRP_VAUX_EN) {
        c->is_test_chip = 1;
    } else {
        c->is_umc = (v & VENDOR_ID) != 0;
        c->is_umc_b_cut = c->is_umc && (v & CHIP_VER_RTL_MASK) != 0;
        if (c->is_92c)
            c->is_1t2r = ((R32(d, REG_HPON_FSM) >> 22) & 0x3) == 1;
    }
    c->rf_paths = c->is_92c ? 2 : 1;
}

static uint8_t EfuseByte(void *aCtx, int aAddr)
{
    rtlu_dev *d = (rtlu_dev *)aCtx;
    W8(d, REG_EFUSE_CTRL + 1, aAddr & 0xff);
    uint8_t b = R8(d, REG_EFUSE_CTRL + 2);
    W8(d, REG_EFUSE_CTRL + 2, ((aAddr >> 8) & 0x03) | (b & 0xfc));
    b = R8(d, REG_EFUSE_CTRL + 3);
    W8(d, REG_EFUSE_CTRL + 3, b & 0x7f);       // start the read
    uint32_t v = R32(d, REG_EFUSE_CTRL);
    for (int i = 0; i < 10 && !(v & 0x80000000u); i++)
        v = R32(d, REG_EFUSE_CTRL);
    Sleep(d, 50);
    return (uint8_t)R32(d, REG_EFUSE_CTRL);
}

int rtlu_efuse_decode(uint8_t (*aRead)(void *aCtx, int aAddr), void *aCtx,
                      uint8_t *aMap)
{
    // 16 sections of 4 little-endian words; each written block is a header
    // (section and a mask of the words that follow, a 0 bit per word)
    // followed by those words.
    uint16_t word[EFUSE_MAX_SECTION][4];
    for (int s = 0; s < EFUSE_MAX_SECTION; s++)
        for (int w = 0; w < 4; w++) word[s][w] = 0xFFFF;

    const int len = EFUSE_REAL_CONTENT_LEN;
    int addr = 0;
    uint8_t h = aRead(aCtx, addr);
    if (h != 0xFF) addr++;
    while (h != 0xFF && addr < len) {
        int section, wren;
        if ((h & 0x1F) == 0x0F) {           // extended header: 2 bytes
            int low = (h & 0xE0) >> 5;
            h = aRead(aCtx, addr);
            if ((h & 0x0F) == 0x0F) {       // no words: skip it
                addr++;
                h = aRead(aCtx, addr);
                if (h != 0xFF && addr < len) addr++;
                continue;
            }
            section = ((h & 0xF0) >> 1) | low;
            wren = h & 0x0F;
            addr++;
        } else {
            section = (h >> 4) & 0x0F;
            wren = h & 0x0F;
        }
        for (int w = 0; w < 4 && addr < len; w++, wren >>= 1) {
            if (wren & 1) continue;
            uint16_t lo = aRead(aCtx, addr++);
            uint16_t hi = addr < len ? aRead(aCtx, addr++) : 0xFF;
            if (section < EFUSE_MAX_SECTION) word[section][w] = (uint16_t)(lo | (hi << 8));
        }
        if (addr >= len) break;
        h = aRead(aCtx, addr);
        if (h != 0xFF) addr++;
    }

    for (int s = 0; s < EFUSE_MAX_SECTION; s++) {
        for (int w = 0; w < 4; w++) {
            aMap[s * 8 + w * 2] = (uint8_t)word[s][w];
            aMap[s * 8 + w * 2 + 1] = (uint8_t)(word[s][w] >> 8);
        }
    }
    return addr;
}

static void ReadEfuse(rtlu_dev *d)
{
    rtlu_chip *c = &d->chip;
    if (!(R8(d, REG_9346CR) & EEPROM_EN)) {
        Log(d, "EFUSE autoload failed; using defaults");
        return;
    }
    // power and clock the EFUSE loader (as the vendor driver does for 92C)
    uint16_t v = R16(d, REG_SYS_ISO_CTRL);
    if (!(v & PWC_EV12V)) W16(d, REG_SYS_ISO_CTRL, v | PWC_EV12V);
    v = R16(d, REG_SYS_FUNC_EN);
    if (!(v & FEN_ELDR)) W16(d, REG_SYS_FUNC_EN, v | FEN_ELDR);
    v = R16(d, REG_SYS_CLKR);
    if ((v & (LOADER_CLK_EN | ANA8M)) != (LOADER_CLK_EN | ANA8M))
        W16(d, REG_SYS_CLKR, v | LOADER_CLK_EN | ANA8M);

    uint8_t map[EFUSE_MAP_LEN];
    int used = rtlu_efuse_decode(EfuseByte, d, map);
    if ((map[0] | (map[1] << 8)) != EEPROM_ID) {
        Log(d, "EFUSE id %02x%02x invalid (%d bytes read)", map[1], map[0], used);
        return;
    }
    c->autoload_ok = 1;
    c->eeprom_vid = (uint16_t)(map[EEPROM_VID] | (map[EEPROM_VID + 1] << 8));
    c->eeprom_pid = (uint16_t)(map[EEPROM_PID] | (map[EEPROM_PID + 1] << 8));
    memcpy(c->mac, map + EEPROM_MAC_ADDR, 6);
    if (!c->is_test_chip)
        c->board_type = (map[EEPROM_RF_OPT1] & 0xE0) >> 5;
}

// ---------------------------------------------------------------------------
// MAC

static int PowerOn(rtlu_dev *d)
{
    int n;
    for (n = 0; !(R8(d, REG_APS_FSMCO) & PFM_ALDN); n++) {
        if (n > 100) return RTLU_ERR_NO_AUTOLOAD;
    }
    W8(d, REG_RSV_CTRL, 0x0);          // unlock ISO/CLK/power registers
    W8(d, REG_SPS0_CTRL, 0x2b);        // switching regulator to PWM mode
    Sleep(d, 100);
    uint8_t v8 = R8(d, REG_LDOV12D_CTRL);
    if (!(v8 & LDV12_EN)) {
        W8(d, REG_LDOV12D_CTRL, v8 | LDV12_EN);
        Sleep(d, 100);
        W8(d, REG_SYS_ISO_CTRL, R8(d, REG_SYS_ISO_CTRL) & ~ISO_MD2PP);
    }
    W16(d, REG_APS_FSMCO, R16(d, REG_APS_FSMCO) | APFM_ONMAC);
    for (n = 0; R16(d, REG_APS_FSMCO) & APFM_ONMAC; n++) {
        if (n > 1000) return RTLU_ERR_POWER;
    }
    W16(d, REG_APS_FSMCO, 0x0812);     // radio, GPIO and LED on
    W16(d, REG_SYS_ISO_CTRL, R16(d, REG_SYS_ISO_CTRL) & ~ISO_DIOR);
    W8(d, REG_APSD_CTRL, R8(d, REG_APSD_CTRL) & ~BIT(6));
    for (n = 0; n < 200 && (R8(d, REG_APSD_CTRL) & BIT(7)); n++) {}
    // Receive side only. Linux also sets HCI_TXDMA_EN, TXDMA_EN, MACTXEN and
    // ENSEC; without MACTXEN the MAC cannot send anything, ACKs included.
    W16(d, REG_CR, R16(d, REG_CR) | HCI_RXDMA_EN | RXDMA_EN | PROTOCOL_EN
        | SCHEDULE_EN | MACRXEN);
    return RTLU_OK;
}

static int LltWrite(rtlu_dev *d, uint32_t aAddr, uint32_t aData)
{
    W32(d, REG_LLT_INIT, ((aAddr & 0xFF) << 8) | (aData & 0xFF) | (1u << 30));
    for (int n = 0; n <= 20; n++) {
        if (((R32(d, REG_LLT_INIT) >> 30) & 0x3) == 0) return 1;
    }
    return 0;
}

// The packet buffer's page list. TX pages are unused here, but the buffer
// layout (and where the RX FIFO starts) follows from it.
static int InitLlt(rtlu_dev *d)
{
    const uint32_t bndy = TX_PAGE_BOUNDARY;
    uint32_t i;
    for (i = 0; i < bndy - 1; i++)
        if (!LltWrite(d, i, i + 1)) return 0;
    if (!LltWrite(d, bndy - 1, 0xFF)) return 0;
    for (i = bndy; i < LLT_LAST_ENTRY; i++)
        if (!LltWrite(d, i, i + 1)) return 0;
    return LltWrite(d, LLT_LAST_ENTRY, bndy);
}

enum { SEL_HQ = 1, SEL_LQ = 2, SEL_NQ = 4 };

static void InitQueues(rtlu_dev *d)
{
    int normal = !d->chip.is_test_chip;
    int sel = 0, eps = 0;
    if (normal) {
        uint8_t cfg = R8(d, REG_NORMAL_SIE_EP + 1);
        if (cfg & 0xF) { sel |= SEL_HQ; eps++; }
        if ((cfg >> 4) & 0xF) { sel |= SEL_NQ; eps++; }
        cfg = R8(d, REG_NORMAL_SIE_EP + 2);
        if (cfg & 0xF) { sel |= SEL_LQ; eps++; }
    }
    if (eps == 0) { sel = SEL_HQ | SEL_LQ; eps = 2; }

    // reserved pages per queue
    uint32_t pubq = normal ? CHIP_B_PAGE_NUM_PUBQ : CHIP_A_PAGE_NUM_PUBQ;
    uint32_t pages = TX_TOTAL_PAGE_NUMBER - pubq;
    uint32_t unit = pages / eps, rest = pages % eps;
    uint32_t hq = (sel & SEL_HQ) ? unit : 0;
    uint32_t lq = (sel & SEL_LQ) ? unit : 0;
    if (eps > 1 && rest) hq += rest;
    if (normal) W8(d, REG_RQPN_NPQ, (sel & SEL_NQ) ? unit : 0);
    W32(d, REG_RQPN, (hq & 0xFF) | ((lq & 0xFF) << 8) | ((pubq & 0xFF) << 16)
        | BIT(31));

    // buffer boundaries; 128-byte pages
    uint8_t b = TX_PAGE_BOUNDARY;
    W8(d, REG_TXPKTBUF_BCNQ_BDNY, b);
    W8(d, REG_TXPKTBUF_MGQ_BDNY, b);
    W8(d, REG_TXPKTBUF_WMAC_LBK_BF_HD, b);
    W8(d, REG_TRXFF_BNDY, b);
    W8(d, REG_TDECTRL + 1, b);
    W16(d, REG_TRXFF_BNDY + 2, 0x27FF);
    W8(d, REG_PBP, 0x11);

    // queue to endpoint priority
    if (normal) {
        uint32_t be, bk, vi, vo, mg, hi;
        if (eps == 1) {
            uint32_t q = sel == SEL_HQ ? QUEUE_HIGH : sel == SEL_LQ ? QUEUE_LOW : QUEUE_NORMAL;
            be = bk = vi = vo = mg = hi = q;
        } else if (eps == 2) {
            uint32_t qhi = (sel & SEL_HQ) ? QUEUE_HIGH : QUEUE_NORMAL;
            uint32_t qlo = (sel & SEL_LQ) ? QUEUE_LOW : QUEUE_NORMAL;
            be = bk = qlo;
            vi = vo = mg = hi = qhi;
        } else {
            be = bk = QUEUE_LOW;
            vi = QUEUE_NORMAL;
            vo = mg = hi = QUEUE_HIGH;
        }
        uint32_t v = R16(d, REG_TRXDMA_CTRL) & 0x7;
        v |= (be << 8) | (bk << 10) | (vi << 6) | (vo << 4) | (mg << 12) | (hi << 14);
        W16(d, REG_TRXDMA_CTRL, v);
    } else {
        W8(d, REG_TRXDMA_CTRL + 1, 0x33);   // VO, VI, MGT, HI on the high queue
    }
}

static void InitMonitorFilters(rtlu_dev *d)
{
    // Everything, from anyone: no BSSID checks, all frame types, frames
    // with a bad FCS kept (and flagged in their descriptor).
    W32(d, REG_RCR, RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_ACRC32 | RCR_AICV
        | RCR_ADF | RCR_ACF | RCR_AMF | RCR_HTC_LOC_CTRL | RCR_APP_PHYSTS
        | RCR_APP_ICV | RCR_APP_MIC | RCR_APPFCS);
    W32(d, REG_MAR, 0xFFFFFFFF);
    W32(d, REG_MAR + 4, 0xFFFFFFFF);
    W16(d, REG_RXFLTMAP0, 0xFFFF);
    W16(d, REG_RXFLTMAP1, 0xFFFF);
    W16(d, REG_RXFLTMAP2, 0xFFFF);
}

static int InitMac(rtlu_dev *d)
{
    int err = PowerOn(d);
    if (err) return err;
    if (!InitLlt(d)) return RTLU_ERR_LLT;
    InitQueues(d);
    W8(d, REG_RX_DRVINFO_SZ, RTLU_DRIVER_INFO_SIZE);   // PHY status with each frame
    W32(d, REG_HIMR, 0);               // no interrupt endpoint traffic
    W32(d, REG_HIMRE, 0);
    W8(d, REG_MSR, 0);                 // network type: no link
    W16(d, REG_BCN_CTRL, 0x1010);
    W8(d, REG_TXPAUSE, 0xFF);          // all TX queues paused, for good
    // one frame per bulk-IN transfer
    W8(d, REG_TRXDMA_CTRL, R8(d, REG_TRXDMA_CTRL) & ~RXDMA_AGG_EN);
    return RTLU_OK;
}

// ---------------------------------------------------------------------------
// PHY

static void WriteBBTable(rtlu_dev *d, const uint32_t *aTab, int aLen)
{
    for (int i = 0; i + 1 < aLen; i += 2) {
        if (DelayEntry(d, aTab[i])) continue;
        W32(d, (uint16_t)aTab[i], aTab[i + 1]);
        Sleep(d, 1);
    }
}

static void ConfigBB(rtlu_dev *d)
{
    const rtlu_chip *c = &d->chip;
    int hp = c->board_type == BOARD_USB_HIGH_PA;

    W16(d, REG_SYS_FUNC_EN, R16(d, REG_SYS_FUNC_EN) | BIT(13) | BIT(0) | BIT(1));
    W8(d, REG_AFE_PLL_CTRL, 0x83);
    W8(d, REG_AFE_PLL_CTRL + 1, 0xdb);
    W8(d, REG_RF_CTRL, RF_EN | RF_RSTB | RF_SDMRSTB);
    W8(d, REG_SYS_FUNC_EN, FEN_USBA | FEN_USBD | FEN_BB_GLB_RSTN | FEN_BBRSTB);
    W32(d, 0x87c, R32(d, 0x87c) & ~BIT(31));
    W8(d, REG_LDOHCI12_CTRL, 0x0f);
    W8(d, REG_AFE_XTAL_CTRL + 1, 0x80);

    if (c->is_92c) WriteBBTable(d, rtlu_phy_2t, rtlu_phy_2t_len);
    else if (hp) WriteBBTable(d, rtlu_phy_1t_hp, rtlu_phy_1t_hp_len);
    else WriteBBTable(d, rtlu_phy_1t, rtlu_phy_1t_len);

    if (c->is_1t2r) {
        SetBB(d, RFPGA0_TXINFO, 0x3, 0x2);
        SetBB(d, RFPGA1_TXINFO, 0x300033, 0x200022);
        SetBB(d, RCCK0_AFESETTING, MASKBYTE3, 0x45);
        SetBB(d, ROFDM0_TRXPATHENABLE, MASKBYTE0, 0x23);
        SetBB(d, ROFDM0_AGCPARAMETER1, 0x30, 0x1);
        SetBB(d, 0xe74, 0x0c000000, 0x2);
        SetBB(d, 0xe78, 0x0c000000, 0x2);
        SetBB(d, 0xe7c, 0x0c000000, 0x2);
        SetBB(d, 0xe80, 0x0c000000, 0x2);
        SetBB(d, 0xe88, 0x0c000000, 0x2);
    }

    if (c->is_92c) WriteBBTable(d, rtlu_agc_2t, rtlu_agc_2t_len);
    else if (hp) WriteBBTable(d, rtlu_agc_1t_hp, rtlu_agc_1t_hp_len);
    else WriteBBTable(d, rtlu_agc_1t, rtlu_agc_1t_len);

    d->cck_high_power = GetBB(d, RFPGA0_XA_HSSIPARAMETER2, 0x200) != 0;
}

static void ConfigRF(rtlu_dev *d)
{
    const rtlu_chip *c = &d->chip;
    for (int path = 0; path < c->rf_paths; path++) {
        const PathRegs *p = &KPath[path];
        const uint32_t *tab;
        int len;
        if (c->is_92c) {
            tab = path == 0 ? rtlu_radioa_2t : rtlu_radiob_2t;
            len = path == 0 ? rtlu_radioa_2t_len : rtlu_radiob_2t_len;
        } else if (c->board_type == BOARD_USB_HIGH_PA) {
            tab = rtlu_radioa_1t_hp;
            len = rtlu_radioa_1t_hp_len;
        } else {
            tab = rtlu_radioa_1t;
            len = rtlu_radioa_1t_len;
        }
        uint32_t env = GetBB(d, p->intfs, p->env);
        SetBB(d, p->intfo, BRFSI_RFENV << 16, 0x1);    // RF interface enable
        Sleep(d, 1);
        SetBB(d, p->intfo, BRFSI_RFENV, 0x1);          // ... and output
        Sleep(d, 1);
        SetBB(d, p->hssi2, B3WIREADDREAALENGTH, 0x0);
        Sleep(d, 1);
        SetBB(d, p->hssi2, B3WIREDATALENGTH, 0x0);
        Sleep(d, 1);
        for (int i = 0; i + 1 < len; i += 2) {
            if (DelayEntry(d, tab[i])) continue;
            SetRF(d, path, tab[i], RFREG_OFFSET_MASK, tab[i + 1]);
            Sleep(d, 1);
        }
        SetBB(d, p->intfs, p->env, env);
    }
}

// LC tank calibration of the RF synthesizer (no transmission involved).
static void CalibrateLC(rtlu_dev *d)
{
    int is2t = d->chip.is_92c;
    uint32_t a_mode = 0, b_mode = 0;
    uint8_t tmp = R8(d, 0xd03);
    int cont_tx = (tmp & 0x70) != 0;
    if (cont_tx) {
        W8(d, 0xd03, tmp & 0x8F);
        a_mode = GetRF(d, 0, RF_AC, MASK12BITS);
        if (is2t) b_mode = GetRF(d, 1, RF_AC, MASK12BITS);
        SetRF(d, 0, RF_AC, MASK12BITS, (a_mode & 0x8FFFF) | 0x10000);
        if (is2t) SetRF(d, 1, RF_AC, MASK12BITS, (b_mode & 0x8FFFF) | 0x10000);
    }
    uint32_t lc = GetRF(d, 0, RF_CHNLBW, MASK12BITS);
    SetRF(d, 0, RF_CHNLBW, MASK12BITS, lc | 0x08000);
    Sleep(d, 100000);
    if (cont_tx) {
        W8(d, 0xd03, tmp);
        SetRF(d, 0, RF_AC, MASK12BITS, a_mode);
        if (is2t) SetRF(d, 1, RF_AC, MASK12BITS, b_mode);
    }
    // Linux unpauses TX here; it stays paused.
}

static void SetBandwidth20(rtlu_dev *d)
{
    W8(d, REG_BWOPMODE, R8(d, REG_BWOPMODE) | BIT(2));
    SetBB(d, RFPGA0_RFMOD, BRFMOD, 0x0);
    SetBB(d, RFPGA1_RFMOD, BRFMOD, 0x0);
    SetBB(d, RFPGA0_ANALOGPARAMETER2, BIT(10), 1);
    d->rf_chnlbw[0] = (d->rf_chnlbw[0] & 0xfffff3ff) | 0x0400;
    SetRF(d, 0, RF_CHNLBW, RFREG_OFFSET_MASK, d->rf_chnlbw[0]);
}

// ---------------------------------------------------------------------------
// API

int rtlu_channel_mhz(int aChannel)
{
    if (aChannel >= 1 && aChannel <= 13) return 2407 + 5 * aChannel;
    if (aChannel == 14) return 2484;
    return 0;
}

int rtlu_set_channel(rtlu_dev *d, int aChannel)
{
    if (aChannel < 1 || aChannel > 14) return RTLU_ERR_ARG;
    int errors = d->io_errors;
    for (int path = 0; path < d->chip.rf_paths; path++) {
        d->rf_chnlbw[path] = (d->rf_chnlbw[path] & 0xfffffc00) | (uint32_t)aChannel;
        SetRF(d, path, RF_CHNLBW, RFREG_OFFSET_MASK, d->rf_chnlbw[path]);
    }
    if (d->chip.is_umc_b_cut)
        SetRF(d, 0, RF_RX_G1, MASKDWORD, aChannel == 6 ? 0x00255 : d->rf_rx_g1);
    Sleep(d, 10000);
    d->channel = aChannel;
    return d->io_errors == errors ? RTLU_OK : RTLU_ERR_IO;
}

void rtlu_set_gain(rtlu_dev *d, int aIgi)
{
    if (aIgi < 0x1C) aIgi = 0x1C;
    if (aIgi > 0x7F) aIgi = 0x7F;
    SetBB(d, ROFDM0_XAAGCCORE1, 0x7f, aIgi);
    if (d->chip.rf_paths > 1) SetBB(d, ROFDM0_XBAGCCORE1, 0x7f, aIgi);
}

int rtlu_init(rtlu_dev *d, const rtlu_io *aIo, int aChannel)
{
    memset(d, 0, sizeof *d);
    d->io = aIo;
    if (aChannel < 1 || aChannel > 14) return RTLU_ERR_ARG;

    R8(d, REG_SYS_CFG);
    if (d->io_errors) return RTLU_ERR_IO;
    ReadChipVersion(d);
    ReadEfuse(d);
    const rtlu_chip *c = &d->chip;
    Log(d, "chip: %s%s%s, %s, board type %d, EFUSE %s",
        c->is_92c ? "RTL8192CU" : "RTL8188CU", c->is_1t2r ? " (1T2R)" : "",
        c->is_test_chip ? " test chip" : "",
        c->is_umc ? (c->is_umc_b_cut ? "UMC B-cut" : "UMC A-cut") : "TSMC",
        c->board_type, c->autoload_ok ? "ok" : "unused");

    int err = InitMac(d);
    if (err) return err;

    // Linux downloads the firmware here; a monitor does without it.
    for (int i = 0; i + 1 < rtlu_mac_len; i += 2)
        W8(d, (uint16_t)rtlu_mac[i], rtlu_mac[i + 1]);
    InitMonitorFilters(d);             // after the table: it sets RCR too
    ConfigBB(d);
    ConfigRF(d);
    if (c->is_umc && !c->is_umc_b_cut && !c->is_92c) {
        SetRF(d, 0, RF_RX_G1, MASKDWORD, 0x30255);
        SetRF(d, 0, RF_RX_G2, MASKDWORD, 0x50a00);
    }
    for (int path = 0; path < c->rf_paths; path++)
        d->rf_chnlbw[path] = GetRF(d, path, RF_CHNLBW, RFREG_OFFSET_MASK);
    d->rf_rx_g1 = GetRF(d, 0, RF_RX_G1, RFREG_OFFSET_MASK);
    SetBB(d, RFPGA0_RFMOD, BCCKEN, 0x1);       // CCK and OFDM blocks on
    SetBB(d, RFPGA0_RFMOD, BOFDMEN, 0x1);
    // main antenna
    if (c->is_92c) SetBB(d, RFPGA0_XB_RFINTERFACEOE, BIT(5) | BIT(6), 0x1);
    else SetBB(d, RFPGA0_XA_RFINTERFACEOE, 0x300, 0x2);
    CalibrateLC(d);

    W8(d, REG_LDOHCI12_CTRL, 0x0f);    // as Linux _rtl92cu_hw_configure
    W8(d, 0x15, 0xe9);
    W8(d, 0xfe40, 0xe0);               // USB interface interference fix
    W8(d, 0xfe41, 0x8d);
    W8(d, 0xfe42, 0x80);
    W8(d, REG_BCN_CTRL, 0x18);

    SetBandwidth20(d);
    err = rtlu_set_channel(d, aChannel);
    if (err) return err;
    if (d->io_errors) {
        Log(d, "%d register accesses failed", d->io_errors);
        return RTLU_ERR_IO;
    }
    return RTLU_OK;
}

void rtlu_stop(rtlu_dev *d)
{
    W32(d, REG_RCR, 0);
    W16(d, REG_CR, R16(d, REG_CR) & ~(MACRXEN | RXDMA_EN | HCI_RXDMA_EN));
    // as Linux disable_rfafeandresetbb: RF off, BB in reset
    SetRF(d, 0, RF_AC, MASKBYTE0, 0x0);
    W8(d, REG_APSD_CTRL, BIT(6));
    W8(d, REG_SYS_FUNC_EN, FEN_USBD | FEN_USBA | FEN_BB_GLB_RSTN);
    W8(d, REG_SYS_FUNC_EN, FEN_USBD | FEN_USBA);
}
