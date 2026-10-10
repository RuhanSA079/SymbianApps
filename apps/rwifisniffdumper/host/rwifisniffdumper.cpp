/*
 * rwifisniffdumper.cpp: rWiFiSniffDumper's driver on Linux, through libusb,
 * to try it against a real RTL8188CUS/RTL8192CU before it goes to the phone.
 *
 *   build/host/rwifisniffdumper [-c channel] [-H channels] [-d ms] [-g gain]
 *                              [-w out.pcap] [-t seconds] [-D vid:pid] [-v]
 *
 *   -c  channel to listen on (default 6)
 *   -H  hop over channels, e.g. 1,6,11 or 1-13, dwelling -d ms (default 250)
 *   -g  OFDM initial gain index (0x1C-0x7F; default: the tables' 0x20)
 *   -w  write every frame to a pcap file (radiotap; Wireshark reads it)
 *   -t  stop after this many seconds
 *   -D  use this USB id instead of the known list
 *   -v  log register-level progress
 *
 * Prints the networks heard (from beacons and probe responses) every two
 * seconds. Needs access to the USB device (root, or a udev rule); the kernel
 * driver, if bound, is detached for the run.
 */
#include "../driver/rtlu.h"
#include <libusb-1.0/libusb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <map>
#include <string>
#include <vector>

// USB ids of the 92C family, from Linux rtl8192cu/sw.c.
static const uint16_t KIds[][2] = {
    {0x0bda, 0x8191}, {0x0bda, 0x018a}, {0x0bda, 0x8170}, {0x0bda, 0x8176},
    {0x0bda, 0x8177}, {0x0bda, 0x817a}, {0x0bda, 0x817b}, {0x0bda, 0x817d},
    {0x0bda, 0x817e}, {0x0bda, 0x817f}, {0x0bda, 0x818a}, {0x0bda, 0x819a},
    {0x0bda, 0x8754}, {0x0bda, 0x8178}, {0x0bda, 0x817c}, {0x050d, 0x1102},
    {0x050d, 0x11f2}, {0x06f8, 0xe033}, {0x07b8, 0x8189}, {0x0846, 0x9041},
    {0x0846, 0x9043}, {0x0b05, 0x17ba}, {0x0bda, 0x5088}, {0x0df6, 0x0052},
    {0x0df6, 0x005c}, {0x0df6, 0x0070}, {0x0df6, 0x0077}, {0x0eb0, 0x9071},
    {0x4856, 0x0091}, {0x103c, 0x1629}, {0x13d3, 0x3357}, {0x2001, 0x3308},
    {0x2019, 0x4902}, {0x2019, 0xab2a}, {0x2019, 0xab2e}, {0x2019, 0xed17},
    {0x20f4, 0x648b}, {0x7392, 0x7811}, {0x13d3, 0x3358}, {0x13d3, 0x3359},
    {0x4855, 0x0090}, {0x4855, 0x0091}, {0x9846, 0x9041}, {0x0bda, 0x317f},
    {0x04f2, 0xaff7}, {0x04f2, 0xaff9}, {0x04f2, 0xaffa}, {0x0846, 0x9042},
    {0x04f2, 0xaff8}, {0x04f2, 0xaffb}, {0x04f2, 0xaffc}, {0x2019, 0x1201},
    {0x050d, 0x1004}, {0x050d, 0x2102}, {0x050d, 0x2103}, {0x0586, 0x341f},
    {0x07aa, 0x0056}, {0x07b8, 0x8178}, {0x0846, 0x9021}, {0x0846, 0xf001},
    {0x0b05, 0x17ab}, {0x0bda, 0x8186}, {0x0df6, 0x0061}, {0x0e66, 0x0019},
    {0x2001, 0x3307}, {0x2001, 0x3309}, {0x2001, 0x330a}, {0x2001, 0x330d},
    {0x2019, 0xab2b}, {0x20f4, 0x624d}, {0x2357, 0x0100}, {0x7392, 0x7822},
};

static volatile sig_atomic_t gStop;
static void OnSignal(int) { gStop = 1; }

static bool gVerbose;

// ---------------------------------------------------------------------------
// rtlu_io over libusb

static int UsbCtrl(libusb_device_handle *h, uint8_t type, uint16_t addr,
                   uint8_t *buf, int len)
{
    int r = 0;
    for (int i = 0; i < 10; i++) {     // as Linux: retry up to 10 times
        r = libusb_control_transfer(h, type, 0x05, addr, 0, buf, len, 500);
        if (r == len) return 0;
        if (r == LIBUSB_ERROR_NO_DEVICE) break;
    }
    fprintf(stderr, "USB %s of 0x%04x failed: %s\n", type & 0x80 ? "read" : "write",
            addr, r < 0 ? libusb_error_name(r) : "short transfer");
    return -1;
}

static int IoRead(void *ctx, uint16_t addr, uint8_t *buf, int len)
{
    return UsbCtrl((libusb_device_handle *)ctx, 0xC0, addr, buf, len);
}

static int IoWrite(void *ctx, uint16_t addr, const uint8_t *buf, int len)
{
    return UsbCtrl((libusb_device_handle *)ctx, 0x40, addr, (uint8_t *)buf, len);
}

static void IoSleep(void *, uint32_t us)
{
    usleep(us);
}

static void IoLog(void *, const char *msg)
{
    fprintf(stderr, "rtlu: %s\n", msg);
}

// ---------------------------------------------------------------------------

struct Net
    {
    rtlu_bss bss;
    int signal;
    int frames;
    time_t last;
    };

static std::vector<int> ParseChannels(const char *s)
{
    std::vector<int> out;
    while (*s) {
        char *end;
        long a = strtol(s, &end, 10), b = a;
        if (end == s) break;
        if (*end == '-') b = strtol(end + 1, &end, 10);
        for (long c = a; c <= b; c++) if (c >= 1 && c <= 14) out.push_back((int)c);
        s = *end == ',' ? end + 1 : end;
    }
    return out;
}

static const char *SecName(int s)
{
    static const char *const KNames[] = { "open", "WEP", "WPA", "WPA2", "WPA3" };
    return KNames[s];
}

static void PrintNets(const std::map<std::string, Net> &nets, long frames, long bad)
{
    printf("\n%-17s %3s %5s %-4s %6s  %s   (%ld frames, %ld bad FCS)\n",
           "BSSID", "ch", "dBm", "sec", "frames", "SSID", frames, bad);
    for (std::map<std::string, Net>::const_iterator i = nets.begin(); i != nets.end(); ++i) {
        const Net &n = i->second;
        printf("%s %3d %5d %-4s %6d  %s\n", i->first.c_str(), n.bss.channel, n.signal,
               SecName(n.bss.security), n.frames,
               n.bss.ssid_len ? n.bss.ssid : "<hidden>");
    }
    fflush(stdout);
}

static double Now()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    int channel = 6, dwell = 250, gain = -1, seconds = 0;
    std::vector<int> hop;
    const char *pcapPath = NULL;
    int vid = -1, pid = -1;
    int opt;
    while ((opt = getopt(argc, argv, "c:H:d:g:w:t:D:v")) != -1) {
        switch (opt) {
        case 'c': channel = atoi(optarg); break;
        case 'H': hop = ParseChannels(optarg); break;
        case 'd': dwell = atoi(optarg); break;
        case 'g': gain = (int)strtol(optarg, NULL, 0); break;
        case 'w': pcapPath = optarg; break;
        case 't': seconds = atoi(optarg); break;
        case 'D': if (sscanf(optarg, "%x:%x", &vid, &pid) != 2) vid = -1; break;
        case 'v': gVerbose = true; break;
        default:
            fprintf(stderr, "usage: %s [-c ch] [-H 1,6,11] [-d ms] [-g gain] "
                    "[-w out.pcap] [-t s] [-D vid:pid] [-v]\n", argv[0]);
            return 2;
        }
    }
    if (!hop.empty()) channel = hop[0];

    libusb_context *usb;
    if (libusb_init(&usb) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
    libusb_device **list;
    ssize_t count = libusb_get_device_list(usb, &list);
    libusb_device *dev = NULL;
    for (ssize_t i = 0; i < count && !dev; i++) {
        libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
        if (vid >= 0) {
            if (dd.idVendor == vid && dd.idProduct == pid) dev = list[i];
            continue;
        }
        for (size_t k = 0; k < sizeof KIds / sizeof KIds[0]; k++)
            if (dd.idVendor == KIds[k][0] && dd.idProduct == KIds[k][1]) dev = list[i];
    }
    if (!dev) {
        fprintf(stderr, "no RTL8188CU/8192CU adapter found (try -D vid:pid)\n");
        return 1;
    }
    libusb_device_descriptor dd;
    libusb_get_device_descriptor(dev, &dd);

    // the bulk-IN endpoint that carries received frames
    int inEp = -1, maxPacket = 512;
    libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(dev, &cfg) == 0) {
        const libusb_interface_descriptor *alt = &cfg->interface[0].altsetting[0];
        for (int e = 0; e < alt->bNumEndpoints && inEp < 0; e++) {
            const libusb_endpoint_descriptor *ep = &alt->endpoint[e];
            if ((ep->bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_BULK
                && (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN)) {
                inEp = ep->bEndpointAddress;
                maxPacket = ep->wMaxPacketSize;
            }
        }
        libusb_free_config_descriptor(cfg);
    }
    if (inEp < 0) { fprintf(stderr, "no bulk-IN endpoint\n"); return 1; }

    libusb_device_handle *h;
    int r = libusb_open(dev, &h);
    libusb_free_device_list(list, 1);
    if (r != 0) {
        fprintf(stderr, "cannot open %04x:%04x: %s (permissions?)\n", dd.idVendor,
                dd.idProduct, libusb_error_name(r));
        return 1;
    }
    libusb_set_auto_detach_kernel_driver(h, 1);
    if ((r = libusb_claim_interface(h, 0)) != 0) {
        fprintf(stderr, "cannot claim the interface: %s\n", libusb_error_name(r));
        return 1;
    }
    fprintf(stderr, "%04x:%04x, bulk IN 0x%02x (%d-byte packets)\n", dd.idVendor,
            dd.idProduct, inEp, maxPacket);

    rtlu_io io = { h, IoRead, IoWrite, IoSleep, IoLog };
    rtlu_dev rd;
    double t0 = Now();
    int err = rtlu_init(&rd, &io, channel);
    if (err) {
        fprintf(stderr, "init failed (%d)\n", err);
        libusb_release_interface(h, 0);
        libusb_close(h);
        return 1;
    }
    fprintf(stderr, "init took %.1f s; MAC %02x:%02x:%02x:%02x:%02x:%02x; listening on %d\n",
            Now() - t0, rd.chip.mac[0], rd.chip.mac[1], rd.chip.mac[2], rd.chip.mac[3],
            rd.chip.mac[4], rd.chip.mac[5], channel);
    if (gain >= 0) rtlu_set_gain(&rd, gain);

    FILE *pcap = NULL;
    if (pcapPath) {
        pcap = fopen(pcapPath, "wb");
        if (!pcap) { perror(pcapPath); return 1; }
        uint8_t hdr[RTLU_PCAP_HEADER_LEN];
        rtlu_pcap_header(hdr);
        fwrite(hdr, 1, sizeof hdr, pcap);
    }

    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
    std::vector<uint8_t> buf(16384), rec(4096 + RTLU_PCAP_RECORD_OVERHEAD);
    std::map<std::string, Net> nets;
    long frames = 0, bad = 0, transfers = 0, malformed = 0;
    double start = Now(), lastPrint = start, lastHop = start;
    size_t hopAt = 0;
    while (!gStop) {
        double now = Now();
        if (seconds && now - start >= seconds) break;
        if (hop.size() > 1 && (now - lastHop) * 1000 >= dwell) {
            hopAt = (hopAt + 1) % hop.size();
            channel = hop[hopAt];
            rtlu_set_channel(&rd, channel);
            lastHop = now;
        }
        if (now - lastPrint >= 2) {
            PrintNets(nets, frames, bad);
            lastPrint = now;
        }

        int got = 0;
        r = libusb_bulk_transfer(h, inEp, &buf[0], buf.size(), &got, 100);
        if (r == LIBUSB_ERROR_TIMEOUT && got == 0) continue;
        if (r != 0 && r != LIBUSB_ERROR_TIMEOUT) {
            fprintf(stderr, "bulk IN: %s\n", libusb_error_name(r));
            break;
        }
        transfers++;
        int off = 0;
        rtlu_frame f;
        int k;
        while ((k = rtlu_rx_next(&buf[0], got, &off, rd.cck_high_power, &f)) == 1) {
            frames++;
            if (f.crc_error) { bad++; }
            if (pcap && f.len <= 4096) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                int n = rtlu_pcap_record(&rec[0], &f, channel, ts.tv_sec, ts.tv_nsec / 1000);
                fwrite(&rec[0], 1, n, pcap);
            }
            if (f.crc_error) continue;
            rtlu_bss b;
            if (rtlu_parse_bss(f.data, f.len, &b)) {
                char key[18];
                snprintf(key, sizeof key, "%02x:%02x:%02x:%02x:%02x:%02x", b.bssid[0],
                         b.bssid[1], b.bssid[2], b.bssid[3], b.bssid[4], b.bssid[5]);
                Net &n = nets[key];
                if (!b.channel) b.channel = channel;
                n.bss = b;
                n.signal = f.has_signal ? f.signal_dbm : 0;
                n.frames++;
                n.last = time(NULL);
            }
            if (gVerbose) {
                int sub, type = rtlu_frame_type(f.data, f.len, &sub);
                fprintf(stderr, "frame %4d B type %d/%2d rate %2d %s%d dBm\n", f.len,
                        type, sub, f.rate, f.crc_error ? "BAD " : "", f.signal_dbm);
            }
        }
        if (k < 0) malformed++;
    }

    PrintNets(nets, frames, bad);
    fprintf(stderr, "%ld transfers, %ld frames, %ld with bad FCS, %ld unparsable transfers\n",
            transfers, frames, bad, malformed);
    if (pcap) fclose(pcap);
    rtlu_stop(&rd);
    libusb_release_interface(h, 0);
    libusb_attach_kernel_driver(h, 0);
    libusb_close(h);
    libusb_exit(usb);
    return 0;
}
