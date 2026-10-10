/*
 * rtlu.h: a receive-only driver for Realtek RTL8188CUS / RTL8192CU USB WiFi
 * adapters (the "92C" family), ported from Linux rtlwifi/rtl8192cu.
 *
 * It only listens. No firmware is loaded (the chip's 8051 firmware does rate
 * adaptation and power saving, which a monitor does not need), and the MAC's
 * TX engine and TX DMA are never enabled, so the adapter cannot send frames,
 * ACKs included. Calibrations that loop the transmitter back (IQK) and TX
 * power setup are skipped.
 *
 * Portable C++ (gnu++98, no STL, no exceptions, no static constructors): the
 * platform supplies USB register access through rtlu_io, and feeds bulk-IN
 * transfers to rtlu_rx_next(). Linux/libusb for the host tool, Symbian USBDI
 * on the phone.
 */
#ifndef RTLU_H
#define RTLU_H

#include <stddef.h>
#include <stdint.h>

/* Register access over USB control transfers (vendor request 0x05, wValue =
 * register address, wIndex = 0; IN for reads, OUT for writes). len is 1, 2
 * or 4; values are little-endian on the wire. Return 0 on success. */
struct rtlu_io
    {
    void *ctx;
    int (*read)(void *ctx, uint16_t addr, uint8_t *buf, int len);
    int (*write)(void *ctx, uint16_t addr, const uint8_t *buf, int len);
    void (*sleep_us)(void *ctx, uint32_t us);
    void (*log)(void *ctx, const char *msg);    /* may be NULL */
    };

struct rtlu_chip
    {
    int is_92c;             /* 2 RF paths (8192CU); else 8188CU (1 path) */
    int is_1t2r;            /* 92C bonded as 1T2R */
    int is_test_chip;
    int is_umc;             /* fab: UMC (else TSMC) */
    int is_umc_b_cut;
    int rf_paths;           /* receive paths, 1 or 2 */
    int board_type;         /* EEPROM board type; 1 = high-power PA */
    int autoload_ok;        /* EFUSE contents valid */
    uint8_t mac[6];         /* from EFUSE (zero if autoload failed) */
    uint16_t eeprom_vid, eeprom_pid;
    };

struct rtlu_dev
    {
    const rtlu_io *io;
    int io_errors;          /* failed register accesses so far */
    rtlu_chip chip;
    int cck_high_power;     /* BB 0x824 bit 9: CCK AGC report format */
    uint32_t rf_chnlbw[2];  /* RF reg 0x18 per path (channel/bandwidth) */
    uint32_t rf_rx_g1;      /* RF reg 0x1A after init (UMC B-cut channel 6 fix) */
    int channel;
    };

enum
    {
    RTLU_OK = 0,
    RTLU_ERR_IO = -1,           /* register access failed */
    RTLU_ERR_NO_AUTOLOAD = -2,  /* chip did not finish loading its EFUSE */
    RTLU_ERR_POWER = -3,        /* MAC power-on timed out */
    RTLU_ERR_LLT = -4,          /* packet buffer link table init timed out */
    RTLU_ERR_ARG = -5
    };

/* Powers the chip on and sets it up as a 20 MHz monitor on aChannel (1-14):
 * all frame types, any address, FCS appended, CRC-failed frames kept (and
 * marked). */
int rtlu_init(rtlu_dev *aDev, const rtlu_io *aIo, int aChannel);

/* Tunes to channel 1-14 (2.4 GHz). */
int rtlu_set_channel(rtlu_dev *aDev, int aChannel);

/* Initial gain index for the OFDM AGC (0x1C-0x7F; bigger is more sensitive,
 * the tables start at 0x20). Linux adjusts it from false-alarm counts. */
void rtlu_set_gain(rtlu_dev *aDev, int aIgi);

/* Stops reception and switches the RF off. */
void rtlu_stop(rtlu_dev *aDev);

/* Channel number to centre frequency in MHz (2.4 GHz band), 0 if invalid. */
int rtlu_channel_mhz(int aChannel);

/* ---------------------------------------------------------------------------
 * Receive path: bulk-IN transfers hold one or more frames, each behind a 24
 * byte RX descriptor and an optional PHY status block. */

struct rtlu_frame
    {
    const uint8_t *data;    /* 802.11 frame, FCS included */
    int len;
    int crc_error;
    int icv_error;
    int rate;               /* descriptor rate: 0-3 CCK, 4-11 OFDM, 12+ MCS0.. */
    int is_ht;
    int bw40;
    int short_gi;
    int short_preamble;
    uint32_t tsf_low;       /* MAC time, microseconds */
    int has_signal;
    int signal_dbm;         /* overall received power */
    int path_dbm[2];        /* per path (OFDM only; -128 if not reported) */
    };

/* Takes the next frame from a bulk-IN buffer, starting at *aOffset and
 * advancing it. Returns 1 for a frame, 0 at the end of the buffer, -1 if
 * the rest of the buffer does not parse. aCckHighPower is
 * rtlu_dev::cck_high_power. */
int rtlu_rx_next(const uint8_t *aBuf, int aLen, int *aOffset,
                 int aCckHighPower, rtlu_frame *aFrame);

/* Rate in 500 kb/s units for CCK/OFDM rates (0 for HT); HT MCS index or -1. */
int rtlu_rate_500k(int aRate);
int rtlu_rate_mcs(int aRate);

/* ---------------------------------------------------------------------------
 * pcap output (LINKTYPE_IEEE802_11_RADIOTAP), written to caller buffers. */

enum { RTLU_PCAP_HEADER_LEN = 24, RTLU_PCAP_RECORD_OVERHEAD = 16 + 32 };

void rtlu_pcap_header(uint8_t *aOut);

/* Writes a record header, a radiotap header and the frame into aOut (room
 * for aLen + RTLU_PCAP_RECORD_OVERHEAD bytes). Returns the bytes written. */
int rtlu_pcap_record(uint8_t *aOut, const rtlu_frame *aFrame, int aChannel,
                     uint32_t aSec, uint32_t aUsec);

/* ---------------------------------------------------------------------------
 * 802.11 frame fields a scanner needs. */

enum
    {
    RTLU_FT_MGMT = 0, RTLU_FT_CTRL = 1, RTLU_FT_DATA = 2,
    RTLU_ST_PROBE_REQ = 4, RTLU_ST_PROBE_RESP = 5, RTLU_ST_BEACON = 8
    };

enum { RTLU_SEC_OPEN = 0, RTLU_SEC_WEP = 1, RTLU_SEC_WPA = 2, RTLU_SEC_WPA2 = 3,
       RTLU_SEC_WPA3 = 4 };

struct rtlu_bss
    {
    uint8_t bssid[6];
    char ssid[33];          /* NUL-terminated; may hold any bytes */
    int ssid_len;           /* 0 for a hidden network */
    int channel;            /* from the DS Parameter Set, 0 if absent */
    int beacon_interval;    /* TUs */
    int security;           /* RTLU_SEC_* */
    int ht;                 /* HT Capabilities present */
    };

/* Frame type/subtype from the frame control field; -1 if too short. */
int rtlu_frame_type(const uint8_t *aFrame, int aLen, int *aSubtype);

/* Parses a beacon or probe response (aLen includes the FCS). Returns 1 on
 * success. */
int rtlu_parse_bss(const uint8_t *aFrame, int aLen, rtlu_bss *aBss);

#endif
