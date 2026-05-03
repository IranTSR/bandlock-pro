#ifndef QMI_COMMON_H
#define QMI_COMMON_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <poll.h>

/* QRTR definitions (may not be in NDK headers) */
#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

struct sockaddr_qrtr {
    unsigned short sq_family;
    uint32_t sq_node;
    uint32_t sq_port;
};

#define QRTR_PORT_CTRL 0xFFFFFFFEu

/* QRTR control packet types */
#define QRTR_TYPE_NEW_SERVER  4
#define QRTR_TYPE_DEL_SERVER  5
#define QRTR_TYPE_NEW_LOOKUP  10

struct qrtr_ctrl_pkt {
    uint32_t cmd;
    uint32_t service;
    uint32_t instance;
    uint32_t node;
    uint32_t port;
} __attribute__((packed));

/* QMI service IDs */
#define QMI_SERVICE_NAS 0x03

/* QMI message types */
#define QMI_REQUEST    0x00
#define QMI_RESPONSE   0x02
#define QMI_INDICATION 0x04

/* QMI header (over QRTR - no QMUX) */
struct qmi_header {
    uint8_t  type;
    uint16_t txn_id;
    uint16_t msg_id;
    uint16_t msg_len;
} __attribute__((packed));

/* QMI TLV */
struct qmi_tlv {
    uint8_t  type;
    uint16_t length;
} __attribute__((packed));

/* QMI Result TLV (type 0x02) */
struct qmi_result {
    uint16_t result;  /* 0=success, 1=failure */
    uint16_t error;
} __attribute__((packed));

/* QMI NAS Message IDs */
#define QMI_NAS_GET_SIGNAL_STRENGTH      0x0020
#define QMI_NAS_GET_SERVING_SYSTEM       0x0024
#define QMI_NAS_SET_SYS_SEL_PREF        0x0033
#define QMI_NAS_GET_SYS_SEL_PREF        0x0034
#define QMI_NAS_GET_CELL_LOCATION_INFO   0x0043
#define QMI_NAS_GET_SIGNAL_INFO          0x004F

/* SET_SYS_SEL_PREF TLV types */
#define TLV_MODE_PREF       0x11
#define TLV_BAND_PREF       0x12
#define TLV_LTE_BAND_PREF   0x15
#define TLV_NET_SEL_PREF    0x16
#define TLV_CHANGE_DURATION  0x1A
#define TLV_LTE_BAND_EXT    0x26
#define TLV_NR5G_BAND_PREF  0x2C

/* Mode preference bits */
#define MODE_CDMA    (1 << 0)
#define MODE_HRPD    (1 << 1)
#define MODE_GSM     (1 << 2)
#define MODE_UMTS    (1 << 3)
#define MODE_LTE     (1 << 4)
#define MODE_TDSCDMA (1 << 5)
#define MODE_NR5G    (1 << 6)

/* LTE band bits: bit N = Band (N+1) */
#define LTE_BAND(n) (1ULL << ((n) - 1))

/* NR5G band bits: bit N = Band n(N+1)
 * Note: For bands n1-n64, fits in a uint64_t.
 * For bands n65+ (like n78), we need extended bitmask. */
#define NR_BAND(n) (1ULL << ((n) - 1))

/* NR5G extended bitmask size (64 bytes = 512 bits, covers all NR bands) */
#define NR5G_MASK_BYTES 64

/* Set a bit in an extended NR5G bitmask (byte array, little-endian) */
static inline void nr5g_mask_set_band(uint8_t *mask, int band) {
    int bit = band - 1; /* 0-indexed */
    if (bit >= 0 && bit < NR5G_MASK_BYTES * 8) {
        mask[bit / 8] |= (1 << (bit % 8));
    }
}

/* Check if a bit is set in an extended NR5G bitmask */
static inline int nr5g_mask_has_band(const uint8_t *mask, int band) {
    int bit = band - 1;
    if (bit >= 0 && bit < NR5G_MASK_BYTES * 8) {
        return (mask[bit / 8] >> (bit % 8)) & 1;
    }
    return 0;
}

/* Check if an NR5G mask is all zeros */
static inline int nr5g_mask_is_zero(const uint8_t *mask) {
    for (int i = 0; i < NR5G_MASK_BYTES; i++) {
        if (mask[i] != 0) return 0;
    }
    return 1;
}

/* Change duration */
#define DURATION_POWER_CYCLE  0x00
#define DURATION_PERMANENT    0x01

/* Malaysia LTE bands */
#define MY_LTE_B1   LTE_BAND(1)
#define MY_LTE_B3   LTE_BAND(3)
#define MY_LTE_B7   LTE_BAND(7)
#define MY_LTE_B8   LTE_BAND(8)
#define MY_LTE_B28  LTE_BAND(28)
#define MY_LTE_B40  LTE_BAND(40)
#define MY_LTE_ALL  (MY_LTE_B1|MY_LTE_B3|MY_LTE_B7|MY_LTE_B8|MY_LTE_B28|MY_LTE_B40)

/* Malaysia NR5G bands */
#define MY_NR_N28   28  /* 700 MHz  — low-band 5G (coverage/anchor) */
#define MY_NR_N41   41  /* 2500 MHz — mid-band 5G (TDD) */
#define MY_NR_N78   78  /* 3500 MHz — primary 5G band in Malaysia */

/* Buffer helpers */
#define MSG_BUF_SIZE 4096

static inline void put_u8(uint8_t *buf, int *pos, uint8_t val) {
    buf[(*pos)++] = val;
}
static inline void put_le16(uint8_t *buf, int *pos, uint16_t val) {
    buf[(*pos)++] = val & 0xFF;
    buf[(*pos)++] = (val >> 8) & 0xFF;
}
static inline void put_le32(uint8_t *buf, int *pos, uint32_t val) {
    buf[(*pos)++] = val & 0xFF;
    buf[(*pos)++] = (val >> 8) & 0xFF;
    buf[(*pos)++] = (val >> 16) & 0xFF;
    buf[(*pos)++] = (val >> 24) & 0xFF;
}
static inline void put_le64(uint8_t *buf, int *pos, uint64_t val) {
    for (int i = 0; i < 8; i++)
        buf[(*pos)++] = (val >> (i * 8)) & 0xFF;
}

static inline uint16_t get_le16(const uint8_t *buf) {
    return buf[0] | (buf[1] << 8);
}
static inline uint32_t get_le32(const uint8_t *buf) {
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}
static inline uint64_t get_le64(const uint8_t *buf) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | buf[i];
    return v;
}
static inline int16_t get_le16s(const uint8_t *buf) {
    return (int16_t)get_le16(buf);
}

/* Add TLV to message buffer */
static inline void add_tlv(uint8_t *buf, int *pos, uint8_t type,
                           uint16_t len, const void *val) {
    put_u8(buf, pos, type);
    put_le16(buf, pos, len);
    memcpy(buf + *pos, val, len);
    *pos += len;
}

/* Find TLV in response data */
static const uint8_t *find_tlv(const uint8_t *data, int data_len,
                               uint8_t type, uint16_t *out_len) {
    int pos = 0;
    while (pos + 3 <= data_len) {
        uint8_t t = data[pos];
        uint16_t l = get_le16(data + pos + 1);
        if (t == type) {
            if (out_len) *out_len = l;
            return data + pos + 3;
        }
        pos += 3 + l;
    }
    return NULL;
}

#endif /* QMI_COMMON_H */
