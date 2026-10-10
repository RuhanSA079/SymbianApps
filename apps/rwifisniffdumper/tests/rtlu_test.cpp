/*
 * rtlu_test.cpp: host tests for the receive-only RTL8192CU driver
 * (apps/rwifisniffdumper/driver).
 *
 *   tests/run-tests.sh      build (with ASan/UBSan) and run
 *
 * The bring-up runs against a simulated register file that mimics just
 * enough of the chip (power-on, LLT, EFUSE and RF read-back handshakes) to
 * check the sequence completes, and that the TX side is never enabled.
 */
#include "../driver/rtlu_int.h"
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

static int gFailed, gChecks;

#define CHECK(c) do { gChecks++; if (!(c)) { gFailed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---------------------------------------------------------------------------
// simulated chip

struct FakeChip
    {
    std::map<uint16_t, uint8_t> regs;
    std::vector<uint8_t> efuse;
    uint32_t rf[2][64];
    int rfReadAddr[2];
    std::vector<std::pair<uint16_t, uint32_t> > writes;  // (addr, value)
    uint32_t crEverOr;              // every bit ever written to REG_CR
    int failReads;

    FakeChip() : crEverOr(0), failReads(0)
        {
        memset(rf, 0, sizeof rf);
        rfReadAddr[0] = rfReadAddr[1] = 0;
        efuse.assign(EFUSE_REAL_CONTENT_LEN, 0xFF);
        }

    uint32_t Get(uint16_t a, int len)
        {
        uint32_t v = 0;
        for (int i = 0; i < len; i++) v |= (uint32_t)regs[a + i] << (8 * i);
        return v;
        }

    void Set(uint16_t a, uint32_t v, int len)
        {
        for (int i = 0; i < len; i++) regs[a + i] = (uint8_t)(v >> (8 * i));
        }

    void OnWrite(uint16_t a, uint32_t v, int len)
        {
        writes.push_back(std::make_pair(a, v));
        // byte-level effects first, for writes that cover these registers
        Set(a, v, len);
        if (a <= REG_CR && REG_CR < a + len) crEverOr |= Get(REG_CR, 2);
        if (a <= REG_APS_FSMCO + 1 && REG_APS_FSMCO + 1 < a + len)
            regs[REG_APS_FSMCO + 1] &= ~(APFM_ONMAC >> 8);   // power-on done
        if (a <= REG_LLT_INIT + 3 && REG_LLT_INIT + 3 < a + len)
            regs[REG_LLT_INIT + 3] &= 0x3F;                   // LLT write done
        if (a == REG_EFUSE_CTRL + 3 && !(v & 0x80)) {         // EFUSE read
            int ea = regs[REG_EFUSE_CTRL + 1] | ((regs[REG_EFUSE_CTRL + 2] & 3) << 8);
            regs[REG_EFUSE_CTRL] = efuse[ea];
            regs[REG_EFUSE_CTRL + 3] |= 0x80;
        }
        for (int p = 0; p < 2; p++) {
            uint16_t lssi = p ? RFPGA0_XB_LSSIPARAMETER : RFPGA0_XA_LSSIPARAMETER;
            uint16_t hssi2 = p ? RFPGA0_XB_HSSIPARAMETER2 : RFPGA0_XA_HSSIPARAMETER2;
            uint16_t rb = p ? RFPGA0_XB_LSSIREADBACK : RFPGA0_XA_LSSIREADBACK;
            if (a == lssi && len == 4)
                rf[p][(v >> 20) & 0x3f] = v & 0xfffff;
            if (a == hssi2 && len == 4) {
                rfReadAddr[p] = (v >> 23) & 0x3f;
                Set(rb, rf[p][rfReadAddr[p]], 4);
            }
        }
        }
    };

static int FakeRead(void *ctx, uint16_t addr, uint8_t *buf, int len)
{
    FakeChip *c = (FakeChip *)ctx;
    if (c->failReads) return -1;
    for (int i = 0; i < len; i++) buf[i] = c->regs[addr + i];
    return 0;
}

static int FakeWrite(void *ctx, uint16_t addr, const uint8_t *buf, int len)
{
    FakeChip *c = (FakeChip *)ctx;
    uint32_t v = 0;
    for (int i = 0; i < len; i++) v |= (uint32_t)buf[i] << (8 * i);
    c->OnWrite(addr, v, len);
    return 0;
}

static void FakeSleep(void *, uint32_t) {}

static std::vector<std::string> gLog;
static void FakeLog(void *, const char *msg) { gLog.push_back(msg); }

// EFUSE contents: logical map -> packed physical form (one block per
// section that has non-0xFF words).
static void PackEfuse(const uint8_t *map, std::vector<uint8_t> &phys)
{
    size_t at = 0;
    for (int s = 0; s < EFUSE_MAX_SECTION; s++) {
        uint8_t wren = 0x0F;
        for (int w = 0; w < 4; w++)
            if (map[s * 8 + w * 2] != 0xFF || map[s * 8 + w * 2 + 1] != 0xFF)
                wren &= ~(1 << w);
        if (wren == 0x0F) continue;
        phys[at++] = (uint8_t)((s << 4) | wren);
        for (int w = 0; w < 4; w++) {
            if (wren & (1 << w)) continue;
            phys[at++] = map[s * 8 + w * 2];
            phys[at++] = map[s * 8 + w * 2 + 1];
        }
    }
}

static void SetupChip(FakeChip &c, bool is92c)
{
    c.Set(REG_SYS_CFG, is92c ? TYPE_ID : 0, 4);     // TSMC, normal chip
    c.Set(REG_APS_FSMCO, PFM_ALDN, 2);
    c.Set(REG_9346CR, EEPROM_EN, 1);
    c.Set(REG_NORMAL_SIE_EP + 1, 0x11, 1);          // HQ + NQ endpoints
    c.Set(REG_NORMAL_SIE_EP + 2, 0x01, 1);          // LQ endpoint
    c.Set(REG_HPON_FSM, 0, 4);
    uint8_t map[EFUSE_MAP_LEN];
    memset(map, 0xFF, sizeof map);
    map[0] = 0x29; map[1] = 0x81;
    map[EEPROM_VID] = 0xda; map[EEPROM_VID + 1] = 0x0b;
    map[EEPROM_PID] = 0x76; map[EEPROM_PID + 1] = 0x81;
    const uint8_t mac[6] = { 0x00, 0xe0, 0x4c, 0x81, 0x88, 0x01 };
    memcpy(map + EEPROM_MAC_ADDR, mac, 6);
    map[EEPROM_RF_OPT1] = 0x00;                     // board type 0
    PackEfuse(map, c.efuse);
}

// ---------------------------------------------------------------------------
// tests

static uint8_t ArrayRead(void *ctx, int addr)
{
    return (*(std::vector<uint8_t> *)ctx)[addr];
}

static void TestEfuse()
{
    std::vector<uint8_t> phys(EFUSE_REAL_CONTENT_LEN, 0xFF);
    // section 0: all words; section 2: word 1 only; extended header for
    // section 17 (out of range: skipped, its words consumed); section 3 last
    uint8_t raw[] = {
        0x00, 0x29, 0x81, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x2D, 0xAB, 0xCD,
        0x2F, 0x2E, 0xEE, 0xEE,             // ext: low=1, high byte 0x2E: section (0x20>>1)|1 = 17
        0x30, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    };
    memcpy(&phys[0], raw, sizeof raw);
    uint8_t map[EFUSE_MAP_LEN];
    int used = rtlu_efuse_decode(ArrayRead, &phys, map);
    CHECK(used == (int)sizeof raw);
    CHECK(map[0] == 0x29 && map[1] == 0x81 && map[7] == 0x66);
    CHECK(map[16] == 0xFF && map[18] == 0xAB && map[19] == 0xCD && map[20] == 0xFF);
    CHECK(map[24] == 0x01 && map[31] == 0x08);
    CHECK(map[8] == 0xFF && map[127] == 0xFF);

    std::vector<uint8_t> empty(EFUSE_REAL_CONTENT_LEN, 0xFF);
    CHECK(rtlu_efuse_decode(ArrayRead, &empty, map) == 0);
    CHECK(map[0] == 0xFF);
}

// A bulk-IN frame: descriptor, PHY status (32 bytes), frame.
static std::vector<uint8_t> MakeRx(const std::vector<uint8_t> &frame, int rate,
                                   bool ht, bool crc, const uint8_t *phy)
{
    std::vector<uint8_t> b(24 + 32 + frame.size(), 0);
    uint32_t dw0 = frame.size() | (4u << 16) | (1u << 26) | (crc ? 1u << 14 : 0);
    uint32_t dw3 = rate | (ht ? 1u << 6 : 0);
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)(dw0 >> (8 * i));
        b[12 + i] = (uint8_t)(dw3 >> (8 * i));
        b[20 + i] = (uint8_t)(0x12345678u >> (8 * i));
    }
    memcpy(&b[24], phy, 28);
    memcpy(&b[56], &frame[0], frame.size());
    return b;
}

static std::vector<uint8_t> Beacon(const char *ssid, int channel, bool rsn, bool sae)
{
    std::vector<uint8_t> f(36, 0);
    f[0] = 0x80;                                    // beacon
    for (int i = 0; i < 6; i++) { f[4 + i] = 0xff; f[10 + i] = f[16 + i] = 0x10 + i; }
    f[32] = 100;                                    // beacon interval
    f[34] = rsn ? 0x11 : 0x01;                      // ESS (+ privacy)
    f.push_back(0);
    f.push_back((uint8_t)strlen(ssid));
    f.insert(f.end(), ssid, ssid + strlen(ssid));
    uint8_t ds[] = { 3, 1, (uint8_t)channel };
    f.insert(f.end(), ds, ds + 3);
    if (rsn) {
        uint8_t ie[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4,
                         1, 0, 0x00, 0x0f, 0xac, (uint8_t)(sae ? 8 : 2), 0, 0 };
        f.insert(f.end(), ie, ie + sizeof ie);
    }
    for (int i = 0; i < 4; i++) f.push_back(0xAA); // FCS
    return f;
}

static void TestRx()
{
    std::vector<uint8_t> frame = Beacon("home", 6, true, false);
    uint8_t phy[28] = { 0 };
    phy[0] = 30; phy[1] = 20;                      // path gains
    phy[4] = 100;                                   // pwdb_all: (100>>1)-110 = -60
    phy[27] = 0x02;                                 // short GI
    std::vector<uint8_t> a = MakeRx(frame, 12 + 7, true, false, phy);
    rtlu_frame f;
    int off = 0;
    CHECK(rtlu_rx_next(&a[0], a.size(), &off, 0, &f) == 1);
    CHECK(f.len == (int)frame.size() && memcmp(f.data, &frame[0], frame.size()) == 0);
    CHECK(f.is_ht && rtlu_rate_mcs(f.rate) == 7 && f.short_gi && !f.crc_error);
    CHECK(f.has_signal && f.signal_dbm == -50);    // -60 + Linux's 10
    CHECK(f.path_dbm[0] == -50 && f.path_dbm[1] == -70);
    CHECK(f.tsf_low == 0x12345678u);
    CHECK(off == (int)a.size());
    CHECK(rtlu_rx_next(&a[0], a.size(), &off, 0, &f) == 0);

    // CCK 1 Mb/s with a bad FCS, then aggregated behind the first at 128 bytes
    uint8_t cck[28] = { 0 };
    cck[5] = 0x80 | 0x10;                           // report 2: -26 - 16
    std::vector<uint8_t> b = MakeRx(frame, 0, false, true, cck);
    std::vector<uint8_t> agg = a;
    agg.resize((a.size() + 127) & ~127, 0);
    agg.insert(agg.end(), b.begin(), b.end());
    off = 0;
    CHECK(rtlu_rx_next(&agg[0], agg.size(), &off, 0, &f) == 1);
    CHECK(off == 128);
    CHECK(rtlu_rx_next(&agg[0], agg.size(), &off, 0, &f) == 1);
    CHECK(f.crc_error && f.rate == 0 && f.signal_dbm == -42 + 10);
    CHECK(rtlu_rate_500k(f.rate) == 2);
    CHECK(rtlu_rx_next(&agg[0], agg.size(), &off, 0, &f) == 0);

    // truncated
    off = 0;
    CHECK(rtlu_rx_next(&a[0], a.size() - 1, &off, 0, &f) == -1);
    CHECK(rtlu_rx_next(&a[0], 10, &off, 0, &f) == -1);
}

static void TestPcap()
{
    std::vector<uint8_t> frame = Beacon("x", 1, false, false);
    uint8_t phy[28] = { 0 };
    phy[4] = 100;
    std::vector<uint8_t> a = MakeRx(frame, 4, false, false, phy);   // 6 Mb/s
    rtlu_frame f;
    int off = 0;
    CHECK(rtlu_rx_next(&a[0], a.size(), &off, 0, &f) == 1);
    std::vector<uint8_t> out(frame.size() + RTLU_PCAP_RECORD_OVERHEAD);
    int n = rtlu_pcap_record(&out[0], &f, 1, 7, 9);
    const uint8_t *rt = &out[16];
    int rtlen = rt[2] | (rt[3] << 8);
    uint32_t present = rt[4] | (rt[5] << 8) | (rt[6] << 16) | ((uint32_t)rt[7] << 24);
    CHECK(present == (BIT(1) | BIT(2) | BIT(3) | BIT(5)));
    CHECK(rtlen == 15);                             // 8 + flags, rate, ch(4), signal
    CHECK(rt[8] == 0x10 && rt[9] == 12);
    CHECK((rt[10] | (rt[11] << 8)) == 2412);
    CHECK((int8_t)rt[14] == -50);
    CHECK(n == 16 + rtlen + (int)frame.size());
    CHECK(out[8] == (uint8_t)(rtlen + frame.size()));

    a = MakeRx(frame, 12 + 3, true, false, phy);    // MCS3: no rate field
    off = 0;
    CHECK(rtlu_rx_next(&a[0], a.size(), &off, 0, &f) == 1);
    n = rtlu_pcap_record(&out[0], &f, 11, 0, 0);
    rtlen = rt[2] | (rt[3] << 8);
    present = rt[4] | (rt[5] << 8) | (rt[6] << 16) | ((uint32_t)rt[7] << 24);
    CHECK(present == (BIT(1) | BIT(3) | BIT(5) | BIT(19)));
    CHECK(rtlen == 18 && rt[9] == 0 && rt[17] == 3);
    CHECK(n <= (int)out.size());

    uint8_t h[RTLU_PCAP_HEADER_LEN];
    rtlu_pcap_header(h);
    CHECK(h[0] == 0xd4 && h[3] == 0xa1 && h[20] == 127);
}

static void TestBss()
{
    rtlu_bss b;
    std::vector<uint8_t> f = Beacon("café", 11, true, false);
    CHECK(rtlu_parse_bss(&f[0], f.size(), &b) == 1);
    CHECK(strcmp(b.ssid, "café") == 0 && b.ssid_len == 5);
    CHECK(b.channel == 11 && b.security == RTLU_SEC_WPA2 && b.beacon_interval == 100);
    CHECK(b.bssid[0] == 0x10 && b.bssid[5] == 0x15);

    f = Beacon("", 1, true, true);
    CHECK(rtlu_parse_bss(&f[0], f.size(), &b) == 1);
    CHECK(b.ssid_len == 0 && b.security == RTLU_SEC_WPA3);

    f = Beacon("open", 3, false, false);
    CHECK(rtlu_parse_bss(&f[0], f.size(), &b) == 1 && b.security == RTLU_SEC_OPEN);

    f[0] = 0x08;                                    // data frame
    CHECK(rtlu_parse_bss(&f[0], f.size(), &b) == 0);
    CHECK(rtlu_frame_type(&f[0], f.size(), NULL) == RTLU_FT_DATA);
    f.resize(20);
    f[0] = 0x80;
    CHECK(rtlu_parse_bss(&f[0], f.size(), &b) == 0);
}

static void TestInit(bool is92c)
{
    FakeChip c;
    SetupChip(c, is92c);
    rtlu_io io = { &c, FakeRead, FakeWrite, FakeSleep, FakeLog };
    rtlu_dev d;
    gLog.clear();
    int err = rtlu_init(&d, &io, 6);
    CHECK(err == RTLU_OK);
    if (err) for (size_t i = 0; i < gLog.size(); i++) printf("  log: %s\n", gLog[i].c_str());
    CHECK(d.chip.is_92c == (int)is92c && d.chip.rf_paths == (is92c ? 2 : 1));
    CHECK(d.chip.autoload_ok && d.chip.eeprom_pid == 0x8176 && d.chip.mac[5] == 0x01);

    // listen only: the TX engine and TX DMA were never switched on
    CHECK(!(c.crEverOr & (MACTXEN | TXDMA_EN | HCI_TXDMA_EN | ENSEC)));
    CHECK(c.Get(REG_CR, 2) & MACRXEN);
    CHECK(c.Get(REG_TXPAUSE, 1) == 0xFF);
    uint32_t rcr = c.Get(REG_RCR, 4);
    CHECK((rcr & RCR_AAP) && (rcr & RCR_ACF) && !(rcr & (RCR_CBSSID_DATA | RCR_CBSSID_BCN)));
    CHECK(c.Get(REG_RXFLTMAP1, 2) == 0xFFFF);
    CHECK(!(c.Get(REG_TRXDMA_CTRL, 1) & RXDMA_AGG_EN));

    // tuned to channel 6, 20 MHz
    for (int p = 0; p < d.chip.rf_paths; p++)
        CHECK((c.rf[p][RF_CHNLBW] & 0x3ff) == 6);
    CHECK((c.rf[0][RF_CHNLBW] & 0xc00) == 0x400);
    CHECK(rtlu_set_channel(&d, 11) == RTLU_OK && (c.rf[0][RF_CHNLBW] & 0x3ff) == 11);
    CHECK(rtlu_set_channel(&d, 15) == RTLU_ERR_ARG);

    rtlu_set_gain(&d, 0x30);
    CHECK((c.Get(ROFDM0_XAAGCCORE1, 4) & 0x7f) == 0x30);

    rtlu_stop(&d);
    CHECK(!(c.Get(REG_CR, 2) & MACRXEN) && c.Get(REG_RCR, 4) == 0);

    // a dead device fails cleanly
    FakeChip dead;
    dead.failReads = 1;
    rtlu_io io2 = { &dead, FakeRead, FakeWrite, FakeSleep, NULL };
    CHECK(rtlu_init(&d, &io2, 1) == RTLU_ERR_IO);
}

int main()
{
    TestEfuse();
    TestRx();
    TestPcap();
    TestBss();
    TestInit(false);
    TestInit(true);
    printf("%d checks, %d failed\n", gChecks, gFailed);
    return gFailed ? 1 : 0;
}
