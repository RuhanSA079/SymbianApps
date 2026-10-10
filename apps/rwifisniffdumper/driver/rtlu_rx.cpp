/*
 * rtlu_rx.cpp: the receive path. RX descriptors and PHY status (as Linux
 * rtl8192cu trx.c and mac.c read them), radiotap/pcap output, and the
 * 802.11 fields a scanner shows.
 */
#include "rtlu_int.h"
#include <string.h>

static uint32_t Le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void Put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void Put32(uint8_t *p, uint32_t v)
{
    Put16(p, v);
    Put16(p + 2, v >> 16);
}

// ---------------------------------------------------------------------------
// RX descriptors

enum { RX_DESC_SIZE = 24 };

// Received power from the PHY status that follows the descriptor.
static void ReadPhyStatus(const uint8_t *p, int aLen, int aCckHighPower,
                          rtlu_frame *f)
{
    if (f->rate <= 3) {                 // CCK: one AGC report
        if (aLen < 6) return;
        int agc = p[5];
        static const int8_t KBase[4] = { 16, -12, -26, -46 };
        if (!aCckHighPower)
            f->signal_dbm = KBase[(agc & 0xc0) >> 6] - (agc & 0x3e);
        else
            f->signal_dbm = KBase[(p[5] & 0x60) >> 5] - ((agc & 0x1f) << 1);
    } else {                            // OFDM/HT: per path and overall
        if (aLen < 28) return;
        for (int i = 0; i < 2; i++)
            f->path_dbm[i] = ((p[i] & 0x3f) * 2) - 110;
        f->signal_dbm = ((p[4] >> 1) & 0x7f) - 110;
        f->short_gi = (p[27] >> 1) & 1;
    }
    // Linux reports this + 10 dB for the 8192CU.
    f->signal_dbm += 10;
    f->has_signal = 1;
}

int rtlu_rx_next(const uint8_t *aBuf, int aLen, int *aOffset,
                 int aCckHighPower, rtlu_frame *f)
{
    int off = *aOffset;
    if (off >= aLen) return 0;
    if (aLen - off < RX_DESC_SIZE) return -1;
    const uint8_t *desc = aBuf + off;
    uint32_t dw0 = Le32(desc), dw3 = Le32(desc + 12);
    int pkt_len = dw0 & 0x3fff;
    int drvinfo = ((dw0 >> 16) & 0xf) * 8;
    int shift = (dw0 >> 24) & 0x3;
    int start = off + RX_DESC_SIZE + drvinfo + shift;
    if (pkt_len == 0 || start + pkt_len > aLen) return -1;

    memset(f, 0, sizeof *f);
    f->path_dbm[0] = f->path_dbm[1] = -128;
    f->data = aBuf + start;
    f->len = pkt_len;
    f->crc_error = (dw0 >> 14) & 1;
    f->icv_error = (dw0 >> 15) & 1;
    f->rate = dw3 & 0x3f;
    f->is_ht = (dw3 >> 6) & 1;
    f->short_preamble = (dw3 >> 8) & 1;
    f->bw40 = (dw3 >> 9) & 1;
    f->tsf_low = Le32(desc + 20);
    if (((dw0 >> 26) & 1) && drvinfo)
        ReadPhyStatus(desc + RX_DESC_SIZE, drvinfo, aCckHighPower, f);

    // With RX aggregation frames are 128-byte aligned; without it there is
    // one per transfer.
    int size = RX_DESC_SIZE + drvinfo + shift + pkt_len;
    int next = off + size;
    if (next < aLen) next = off + ((size + 127) & ~127);
    *aOffset = next;
    return 1;
}

int rtlu_rate_500k(int aRate)
{
    static const uint8_t KRates[12] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
    return aRate >= 0 && aRate < 12 ? KRates[aRate] : 0;
}

int rtlu_rate_mcs(int aRate)
{
    return aRate >= 12 && aRate <= 0x1b ? aRate - 12 : -1;
}

// ---------------------------------------------------------------------------
// pcap

void rtlu_pcap_header(uint8_t *aOut)
{
    Put32(aOut, 0xa1b2c3d4);    // microsecond timestamps
    Put16(aOut + 4, 2);
    Put16(aOut + 6, 4);
    Put32(aOut + 8, 0);
    Put32(aOut + 12, 0);
    Put32(aOut + 16, 65535);    // snap length
    Put32(aOut + 20, 127);      // LINKTYPE_IEEE802_11_RADIOTAP
}

int rtlu_pcap_record(uint8_t *aOut, const rtlu_frame *f, int aChannel,
                     uint32_t aSec, uint32_t aUsec)
{
    // radiotap: flags, rate (non-HT), channel, antenna signal, MCS (HT)
    uint8_t *rt = aOut + 16;
    int mcs = rtlu_rate_mcs(f->rate);
    int ht = f->is_ht && mcs >= 0;
    uint32_t present = BIT(1) | BIT(3);
    int n = 8;
    uint8_t flags = 0x10;                       // FCS at the end
    if (f->crc_error) flags |= 0x40;
    if (f->short_preamble && f->rate <= 3) flags |= 0x02;
    rt[n++] = flags;
    if (!ht) {
        present |= BIT(2);
        rt[n++] = (uint8_t)rtlu_rate_500k(f->rate);
    }
    if (n & 1) rt[n++] = 0;
    Put16(rt + n, rtlu_channel_mhz(aChannel));
    Put16(rt + n + 2, 0x0080 | (f->rate <= 3 ? 0x0020 : 0x0040));
    n += 4;
    if (f->has_signal) {
        present |= BIT(5);
        int s = f->signal_dbm < -127 ? -127 : f->signal_dbm > 127 ? 127 : f->signal_dbm;
        rt[n++] = (uint8_t)(int8_t)s;
    }
    if (ht) {
        present |= BIT(19);
        rt[n++] = 0x07;                         // bandwidth, MCS, GI known
        rt[n++] = (f->bw40 ? 0x01 : 0) | (f->short_gi ? 0x04 : 0);
        rt[n++] = (uint8_t)mcs;
    }
    rt[0] = 0;
    rt[1] = 0;
    Put16(rt + 2, n);
    Put32(rt + 4, present);
    memcpy(rt + n, f->data, f->len);

    Put32(aOut, aSec);
    Put32(aOut + 4, aUsec);
    Put32(aOut + 8, n + f->len);
    Put32(aOut + 12, n + f->len);
    return 16 + n + f->len;
}

// ---------------------------------------------------------------------------
// 802.11

int rtlu_frame_type(const uint8_t *aFrame, int aLen, int *aSubtype)
{
    if (aLen < 2) return -1;
    if (aSubtype) *aSubtype = (aFrame[0] >> 4) & 0xf;
    return (aFrame[0] >> 2) & 0x3;
}

// The AKM suites of an RSN element: 1 if one is SAE (WPA3-Personal) and
// none is PSK.
static int RsnIsWpa3(const uint8_t *p, int aLen)
{
    int i = 2 + 4;                              // version, group cipher
    if (i + 2 > aLen) return 0;
    int n = p[i] | (p[i + 1] << 8);
    i += 2 + 4 * n;                             // pairwise ciphers
    if (i + 2 > aLen) return 0;
    n = p[i] | (p[i + 1] << 8);
    i += 2;
    int sae = 0, psk = 0;
    for (int k = 0; k < n && i + 4 <= aLen; k++, i += 4) {
        if (p[i] != 0x00 || p[i + 1] != 0x0f || p[i + 2] != 0xac) continue;
        if (p[i + 3] == 8) sae = 1;
        if (p[i + 3] == 2 || p[i + 3] == 6) psk = 1;
    }
    return sae && !psk;
}

int rtlu_parse_bss(const uint8_t *aFrame, int aLen, rtlu_bss *aBss)
{
    int sub;
    if (rtlu_frame_type(aFrame, aLen, &sub) != RTLU_FT_MGMT
        || (sub != RTLU_ST_BEACON && sub != RTLU_ST_PROBE_RESP))
        return 0;
    int end = aLen - 4;                         // FCS
    if (end < 36) return 0;
    memset(aBss, 0, sizeof *aBss);
    memcpy(aBss->bssid, aFrame + 16, 6);
    aBss->beacon_interval = aFrame[32] | (aFrame[33] << 8);
    int privacy = (aFrame[34] >> 4) & 1;
    int rsn = 0, wpa = 0, wpa3 = 0;

    for (int i = 36; i + 2 <= end; ) {
        int id = aFrame[i], len = aFrame[i + 1];
        const uint8_t *v = aFrame + i + 2;
        if (i + 2 + len > end) break;
        switch (id) {
        case 0: {                               // SSID
            int n = len > 32 ? 32 : len;
            int zero = 1;
            for (int k = 0; k < n; k++) if (v[k]) zero = 0;
            if (!zero) {
                memcpy(aBss->ssid, v, n);
                aBss->ssid[n] = 0;
                aBss->ssid_len = n;
            }
            break;
        }
        case 3:                                 // DS Parameter Set
            if (len >= 1) aBss->channel = v[0];
            break;
        case 45:                                // HT Capabilities
            aBss->ht = 1;
            break;
        case 48:                                // RSN
            rsn = 1;
            wpa3 = RsnIsWpa3(v, len);
            break;
        case 221:                               // vendor: Microsoft WPA
            if (len >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xf2 && v[3] == 1)
                wpa = 1;
            break;
        }
        i += 2 + len;
    }
    aBss->security = wpa3 ? RTLU_SEC_WPA3 : rsn ? RTLU_SEC_WPA2 : wpa ? RTLU_SEC_WPA
        : privacy ? RTLU_SEC_WEP : RTLU_SEC_OPEN;
    return 1;
}
