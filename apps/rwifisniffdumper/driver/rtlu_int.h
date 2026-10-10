/*
 * rtlu_int.h: RTL8192CU registers and internals of the receive-only driver.
 * Names and values follow Linux rtlwifi (rtl8192ce/reg.h, which the 8192CU
 * driver shares).
 */
#ifndef RTLU_INT_H
#define RTLU_INT_H

#include "rtlu.h"

#define BIT(n) (1u << (n))

// ---------------------------------------------------------------------------
// MAC / system registers

#define REG_SYS_ISO_CTRL        0x0000
#define REG_SYS_FUNC_EN         0x0002
#define REG_APS_FSMCO           0x0004
#define REG_SYS_CLKR            0x0008
#define REG_9346CR              0x000A
#define REG_SPS0_CTRL           0x0011
#define REG_RSV_CTRL            0x001C
#define REG_RF_CTRL             0x001F
#define REG_LDOV12D_CTRL        0x0021
#define REG_LDOHCI12_CTRL       0x0022
#define REG_AFE_XTAL_CTRL       0x0024
#define REG_AFE_PLL_CTRL        0x0028
#define REG_EFUSE_CTRL          0x0030
#define REG_HPON_FSM            0x00EC
#define REG_SYS_CFG             0x00F0
#define REG_CR                  0x0100
#define REG_MSR                 0x0102  /* REG_CR + 2: network type */
#define REG_PBP                 0x0104
#define REG_TRXDMA_CTRL         0x010C
#define REG_TRXFF_BNDY          0x0114
#define REG_HIMR                0x0120
#define REG_HIMRE               0x0128
#define REG_LLT_INIT            0x01E0
#define REG_RQPN                0x0200
#define REG_TDECTRL             0x0208
#define REG_RQPN_NPQ            0x0214
#define REG_FWHW_TXQ_CTRL       0x0420
#define REG_HWSEQ_CTRL          0x0423
#define REG_TXPKTBUF_BCNQ_BDNY  0x0424
#define REG_TXPKTBUF_MGQ_BDNY   0x0425
#define REG_SPEC_SIFS           0x0428
#define REG_RL                  0x042A
#define REG_DARFRC              0x0430
#define REG_RARFRC              0x0438
#define REG_RRSR                0x0440
#define REG_AGGLEN_LMT          0x0458
#define REG_AMPDU_MIN_SPACE     0x045C
#define REG_TXPKTBUF_WMAC_LBK_BF_HD 0x045D
#define REG_PROT_MODE_CTRL      0x04C8
#define REG_BAR_MODE_CTRL       0x04CC
#define REG_EDCA_VO_PARAM       0x0500
#define REG_EDCA_VI_PARAM       0x0504
#define REG_EDCA_BE_PARAM       0x0508
#define REG_EDCA_BK_PARAM       0x050C
#define REG_BCNTCFG             0x0510
#define REG_PIFS                0x0512
#define REG_SIFS_CCK            0x0514
#define REG_SIFS_OFDM           0x0516
#define REG_AGGR_BREAK_TIME     0x051A
#define REG_TXPAUSE             0x0522
#define REG_RD_CTRL             0x0524
#define REG_TBTT_PROHIBIT       0x0540
#define REG_NAV_PROT_LEN        0x0546
#define REG_BCN_CTRL            0x0550
#define REG_DRVERLYINT          0x0558
#define REG_BCNDMATIM           0x0559
#define REG_ATIMWND             0x055A
#define REG_BCN_MAX_ERR         0x055D
#define REG_APSD_CTRL           0x0600
#define REG_BWOPMODE            0x0603
#define REG_RCR                 0x0608
#define REG_RX_DRVINFO_SZ       0x060F
#define REG_MAR                 0x0620
#define REG_ACKTO               0x0640
#define REG_RXFLTMAP0           0x06A0  /* management subtypes */
#define REG_RXFLTMAP1           0x06A2  /* control subtypes */
#define REG_RXFLTMAP2           0x06A4  /* data subtypes */
#define REG_NORMAL_SIE_EP       0xFE65

// REG_SYS_ISO_CTRL
#define ISO_MD2PP               BIT(0)
#define ISO_DIOR                BIT(9)
#define PWC_EV12V               BIT(15)
// REG_SYS_FUNC_EN
#define FEN_BBRSTB              BIT(0)
#define FEN_BB_GLB_RSTN         BIT(1)
#define FEN_USBA                BIT(2)
#define FEN_USBD                BIT(4)
#define FEN_ELDR                BIT(12)
// REG_APS_FSMCO
#define PFM_ALDN                BIT(1)
#define APFM_ONMAC              BIT(8)
// REG_SYS_CLKR
#define LOADER_CLK_EN           BIT(5)
#define ANA8M                   BIT(1)
// REG_9346CR
#define EEPROM_EN               BIT(5)
// REG_LDOV12D_CTRL
#define LDV12_EN                BIT(0)
// REG_RF_CTRL
#define RF_EN                   BIT(0)
#define RF_RSTB                 BIT(1)
#define RF_SDMRSTB              BIT(2)
// REG_SYS_CFG
#define VENDOR_ID               BIT(19)
#define TRP_VAUX_EN             BIT(23)
#define TYPE_ID                 BIT(27)
#define CHIP_VER_RTL_MASK       0xF000
// REG_CR
#define HCI_TXDMA_EN            BIT(0)
#define HCI_RXDMA_EN            BIT(1)
#define TXDMA_EN                BIT(2)
#define RXDMA_EN                BIT(3)
#define PROTOCOL_EN             BIT(4)
#define SCHEDULE_EN             BIT(5)
#define MACTXEN                 BIT(6)
#define MACRXEN                 BIT(7)
#define ENSEC                   BIT(9)
// REG_TRXDMA_CTRL
#define RXDMA_AGG_EN            BIT(2)
#define QUEUE_LOW               1
#define QUEUE_NORMAL            2
#define QUEUE_HIGH              3
// REG_RCR
#define RCR_AAP                 BIT(0)
#define RCR_APM                 BIT(1)
#define RCR_AM                  BIT(2)
#define RCR_AB                  BIT(3)
#define RCR_CBSSID_DATA         BIT(6)
#define RCR_CBSSID_BCN          BIT(7)
#define RCR_ACRC32              BIT(8)
#define RCR_AICV                BIT(9)
#define RCR_ADF                 BIT(11)
#define RCR_ACF                 BIT(12)
#define RCR_AMF                 BIT(13)
#define RCR_HTC_LOC_CTRL        BIT(14)
#define RCR_APP_PHYSTS          BIT(28)
#define RCR_APP_ICV             BIT(29)
#define RCR_APP_MIC             BIT(30)
#define RCR_APPFCS              BIT(31)

// TX packet buffer layout (the RX FIFO lies above it)
#define TX_TOTAL_PAGE_NUMBER    0xF8
#define TX_PAGE_BOUNDARY        (TX_TOTAL_PAGE_NUMBER + 1)
#define LLT_LAST_ENTRY          255
#define CHIP_A_PAGE_NUM_PUBQ    0x7E
#define CHIP_B_PAGE_NUM_PUBQ    0xE7

#define RTLU_DRIVER_INFO_SIZE   4       /* PHY status: 4 x 8 bytes */

// ---------------------------------------------------------------------------
// baseband registers

#define RFPGA0_RFMOD            0x800
#define RFPGA0_TXINFO           0x804
#define RFPGA0_XA_HSSIPARAMETER1 0x820
#define RFPGA0_XA_HSSIPARAMETER2 0x824
#define RFPGA0_XB_HSSIPARAMETER1 0x828
#define RFPGA0_XB_HSSIPARAMETER2 0x82c
#define RFPGA0_XA_LSSIPARAMETER 0x840
#define RFPGA0_XB_LSSIPARAMETER 0x844
#define RFPGA0_XA_RFINTERFACEOE 0x860
#define RFPGA0_XB_RFINTERFACEOE 0x864
#define RFPGA0_XAB_RFINTERFACESW 0x870
#define RFPGA0_ANALOGPARAMETER2 0x884
#define RFPGA0_XA_LSSIREADBACK  0x8a0
#define RFPGA0_XB_LSSIREADBACK  0x8a4
#define TRANSCEIVEA_HSPI_READBACK 0x8b8
#define TRANSCEIVEB_HSPI_READBACK 0x8bc
#define RFPGA1_RFMOD            0x900
#define RFPGA1_TXINFO           0x90c
#define RCCK0_SYSTEM            0xa00
#define RCCK0_AFESETTING        0xa04
#define ROFDM0_TRXPATHENABLE    0xc04
#define ROFDM0_XAAGCCORE1       0xc50
#define ROFDM0_XBAGCCORE1       0xc58
#define ROFDM0_AGCPARAMETER1    0xc70
#define ROFDM1_LSTF             0xd00

#define MASKDWORD               0xffffffffu
#define MASKBYTE0               0xffu
#define MASKBYTE3               0xff000000u
#define MASK12BITS              0xfffu
#define RFREG_OFFSET_MASK       0xfffffu
#define BRFMOD                  0x1u
#define BCCKEN                  0x1000000u
#define BOFDMEN                 0x2000000u
#define BRFSI_RFENV             0x10u
#define B3WIREADDREAALENGTH     0x400u
#define B3WIREDATALENGTH        0x800u
#define BLSSIREADADDRESS        0x7f800000u
#define BLSSIREADEDGE           0x80000000u
#define BLSSIREADBACKDATA       0xfffffu

// RF (6052) registers
#define RF_AC                   0x00
#define RF_CHNLBW               0x18
#define RF_RX_G1                0x1A
#define RF_RX_G2                0x1B

// ---------------------------------------------------------------------------
// EFUSE

#define EFUSE_REAL_CONTENT_LEN  512
#define EFUSE_MAX_SECTION       16
#define EFUSE_MAP_LEN           128     /* HWSET_MAX_SIZE */
#define EEPROM_ID               0x8129
#define EEPROM_VID              0x0A
#define EEPROM_PID              0x0C
#define EEPROM_MAC_ADDR         0x16
#define EEPROM_RF_OPT1          0x79
#define BOARD_USB_HIGH_PA       1

/* Decodes the EFUSE's packed physical contents, read a byte at a time
 * through aRead, into the 128-byte logical map (0xFF where nothing is
 * written). Reads stop at the end marker, so only the used part is read
 * (each byte costs several USB transfers). Returns the bytes read. */
int rtlu_efuse_decode(uint8_t (*aRead)(void *aCtx, int aAddr), void *aCtx,
                      uint8_t *aMap);

// ---------------------------------------------------------------------------
// tables (rtlu_tables.cpp): address/value pairs

extern const uint32_t rtlu_mac[];
extern const int rtlu_mac_len;
extern const uint32_t rtlu_phy_2t[];
extern const int rtlu_phy_2t_len;
extern const uint32_t rtlu_phy_1t[];
extern const int rtlu_phy_1t_len;
extern const uint32_t rtlu_phy_1t_hp[];
extern const int rtlu_phy_1t_hp_len;
extern const uint32_t rtlu_agc_2t[];
extern const int rtlu_agc_2t_len;
extern const uint32_t rtlu_agc_1t[];
extern const int rtlu_agc_1t_len;
extern const uint32_t rtlu_agc_1t_hp[];
extern const int rtlu_agc_1t_hp_len;
extern const uint32_t rtlu_radioa_2t[];
extern const int rtlu_radioa_2t_len;
extern const uint32_t rtlu_radiob_2t[];
extern const int rtlu_radiob_2t_len;
extern const uint32_t rtlu_radioa_1t[];
extern const int rtlu_radioa_1t_len;
extern const uint32_t rtlu_radioa_1t_hp[];
extern const int rtlu_radioa_1t_hp_len;

#endif
