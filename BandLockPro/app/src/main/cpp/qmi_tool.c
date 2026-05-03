/*
 * qmi_tool — Band Lock & Cell Info via QRTR/QMI
 * Target: Snapdragon 695 (veux), KernelSU root
 * Build: clang -o qmi_tool qmi_tool.c -Wall -O2
 */
#include "qmi_common.h"

static uint16_t g_txn = 1;
static int g_sock = -1;
static struct sockaddr_qrtr g_nas_addr;

/* ---- QRTR Layer ---- */

static int qrtr_open(void) {
    int sock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "ERROR: socket(AF_QIPCRTR) failed: %s\n", strerror(errno));
        if (errno == EAFNOSUPPORT)
            fprintf(stderr, "HINT: Kernel may not support QRTR. Check CONFIG_QRTR.\n");
        if (errno == EACCES)
            fprintf(stderr, "HINT: SELinux blocking. Run with: su -c\n");
        return -1;
    }
    
    struct sockaddr_qrtr sq;
    socklen_t sl = sizeof(sq);
    if (getsockname(sock, (struct sockaddr *)&sq, &sl) < 0) {
        fprintf(stderr, "ERROR: getsockname failed: %s\n", strerror(errno));
        close(sock);
        return -1;
    }
    
    sq.sq_port = 0;
    if (bind(sock, (struct sockaddr *)&sq, sizeof(sq)) < 0) {
        fprintf(stderr, "ERROR: bind failed: %s\n", strerror(errno));
        close(sock);
        return -1;
    }
    return sock;
}

static int qrtr_lookup_nas(int sock, struct sockaddr_qrtr *out) {
    struct qrtr_ctrl_pkt pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
    pkt.service = QMI_SERVICE_NAS;
    pkt.instance = 0;

    struct sockaddr_qrtr ctrl = {
        .sq_family = AF_QIPCRTR,
        .sq_node = 1,
        .sq_port = QRTR_PORT_CTRL
    };

    if (sendto(sock, &pkt, sizeof(pkt), 0,
               (struct sockaddr *)&ctrl, sizeof(ctrl)) < 0) {
        fprintf(stderr, "ERROR: lookup sendto failed: %s\n", strerror(errno));
        return -1;
    }

    struct pollfd pfd = { .fd = sock, .events = POLLIN };
    for (int i = 0; i < 5; i++) {
        if (poll(&pfd, 1, 2000) <= 0) break;

        uint8_t buf[256];
        struct sockaddr_qrtr from;
        socklen_t sl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &sl);
        if (n < 20) continue;

        uint32_t cmd = get_le32(buf);
        uint32_t svc = get_le32(buf + 4);
        if (cmd == QRTR_TYPE_NEW_SERVER && svc == QMI_SERVICE_NAS) {
            out->sq_family = AF_QIPCRTR;
            out->sq_node = get_le32(buf + 12);
            out->sq_port = get_le32(buf + 16);
            return 0;
        }
    }
    fprintf(stderr, "ERROR: NAS service not found via QRTR\n");
    return -1;
}

/* ---- QMI Layer ---- */

static int qmi_send(int sock, struct sockaddr_qrtr *addr,
                    uint16_t msg_id, const uint8_t *tlv_data, int tlv_len) {
    uint8_t buf[MSG_BUF_SIZE];
    int pos = 0;
    put_u8(buf, &pos, QMI_REQUEST);
    put_le16(buf, &pos, g_txn++);
    put_le16(buf, &pos, msg_id);
    put_le16(buf, &pos, tlv_len);
    if (tlv_len > 0) {
        memcpy(buf + pos, tlv_data, tlv_len);
        pos += tlv_len;
    }
    return sendto(sock, buf, pos, 0, (struct sockaddr *)addr, sizeof(*addr));
}

static int qmi_recv(int sock, uint16_t exp_msg_id,
                    uint8_t *tlv_out, int *tlv_len_out, int timeout_ms) {
    long long start_ms = 0;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    start_ms = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;

    while (1) {
        long long now_ms;
        gettimeofday(&tv, NULL);
        now_ms = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        int rem_ms = timeout_ms - (now_ms - start_ms);
        if (rem_ms <= 0) {
            fprintf(stderr, "ERROR: QMI response timeout\n");
            return -1;
        }

        struct pollfd pfd = { .fd = sock, .events = POLLIN };
        if (poll(&pfd, 1, rem_ms) <= 0) {
            fprintf(stderr, "ERROR: QMI poll timeout\n");
            return -1;
        }
        
        uint8_t buf[MSG_BUF_SIZE];
        int n = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
        if (n < 7) continue;

        uint8_t type = buf[0];
        uint16_t msg_id = get_le16(buf + 3);
        uint16_t msg_len = get_le16(buf + 5);

        if (type == QMI_INDICATION) {
            /* Ignore asynchronous indications */
            continue;
        }

        if (type != QMI_RESPONSE || msg_id != exp_msg_id) {
            fprintf(stderr, "WARN: unexpected msg type=0x%02x id=0x%04x (expected 0x%04x)\n", type, msg_id, exp_msg_id);
            continue;
        }

        /* Check result TLV */
        const uint8_t *data = buf + 7;
        uint16_t rlen;
        const uint8_t *res = find_tlv(data, msg_len, 0x02, &rlen);
        if (res && rlen >= 4) {
            uint16_t result = get_le16(res);
            uint16_t error = get_le16(res + 2);
            if (result != 0) {
                fprintf(stderr, "ERROR: QMI error result=%d error=%d\n", result, error);
                return -1;
            }
        }

        if (tlv_out && tlv_len_out) {
            int copy = msg_len < MSG_BUF_SIZE ? msg_len : MSG_BUF_SIZE;
            memcpy(tlv_out, data, copy);
            *tlv_len_out = copy;
        }
        return 0;
    }
}

static int qmi_init(void) {
    g_sock = qrtr_open();
    if (g_sock < 0) return -1;
    if (qrtr_lookup_nas(g_sock, &g_nas_addr) < 0) {
        close(g_sock);
        return -1;
    }
    // Silent init for clean JSON output
    return 0;
}

/* ---- Commands ---- */

static int cmd_test(void) {
    if (qmi_init() < 0) return 1;
    /* Try GET_SIGNAL_INFO */
    qmi_send(g_sock, &g_nas_addr, QMI_NAS_GET_SIGNAL_INFO, NULL, 0);
    uint8_t resp[MSG_BUF_SIZE];
    int rlen = 0;
    if (qmi_recv(g_sock, QMI_NAS_GET_SIGNAL_INFO, resp, &rlen, 5000) == 0) {
        printf("{\"test\":\"PASS\",\"signal_resp_len\":%d}\n", rlen);
    } else {
        printf("{\"test\":\"PARTIAL\",\"note\":\"connected but signal query failed\"}\n");
    }
    close(g_sock);
    return 0;
}

static int cmd_cell_info(void) {
    if (qmi_init() < 0) return 1;
    qmi_send(g_sock, &g_nas_addr, QMI_NAS_GET_CELL_LOCATION_INFO, NULL, 0);
    uint8_t resp[MSG_BUF_SIZE];
    int rlen = 0;
    if (qmi_recv(g_sock, QMI_NAS_GET_CELL_LOCATION_INFO, resp, &rlen, 5000) < 0) {
        close(g_sock);
        return 1;
    }

    printf("{\"cells\":[");
    int first = 1;

    /* TLV 0x13: LTE intra-freq info */
    uint16_t tlen;
    const uint8_t *t = find_tlv(resp, rlen, 0x13, &tlen);
    if (t && tlen >= 14) {
        uint16_t earfcn = get_le16(t + 10);
        uint16_t scid = get_le16(t + 12);
        
        if (!first) printf(",");
        printf("{\"type\":\"serving\",\"pci\":%u,\"earfcn\":%u}", scid, earfcn);
        first = 0;
        
        /* Parse intra-freq neighbors if present */
        if (tlen >= 19) {
            uint8_t n_intra = t[18];
            int pos = 19;
            for (int i = 0; i < n_intra && pos + 6 <= tlen; i++) {
                uint16_t pci = get_le16(t + pos);
                int16_t rsrq = get_le16s(t + pos + 2);
                int16_t rsrp = get_le16s(t + pos + 4);
                pos += 10; // each intra-freq neighbor entry is 10 bytes
                
                printf(",{\"type\":\"neighbor\",\"pci\":%u,\"earfcn\":%u,\"rsrp\":%d,\"rsrq\":%d}", pci, earfcn, rsrp, rsrq);
            }
        }
    }

    /* TLV 0x14: LTE inter-freq info */
    t = find_tlv(resp, rlen, 0x14, &tlen);
    if (t && tlen >= 2) {
        uint8_t n_freqs = t[1];
        int pos = 2;
        for (int f = 0; f < n_freqs && pos + 6 <= tlen; f++) {
            uint32_t freq_earfcn = get_le32(t + pos);
            pos += 4;
            pos++; // skip byte (idle?)
            if (pos < tlen) {
                uint8_t n_cells = t[pos++];
                for (int c = 0; c < n_cells && pos + 6 <= tlen; c++) {
                    uint16_t pci = get_le16(t + pos);
                    int16_t rsrq = get_le16s(t + pos + 2);
                    int16_t rsrp = get_le16s(t + pos + 4);
                    pos += 10; // each inter-freq neighbor entry is 10 bytes
                    
                    printf(",{\"type\":\"neighbor\",\"pci\":%u,\"earfcn\":%u,\"rsrp\":%d,\"rsrq\":%d}", pci, freq_earfcn, rsrp, rsrq);
                }
            }
        }
    }

    /* TLV 0x1E: NR5G serving cell */
    t = find_tlv(resp, rlen, 0x1E, &tlen);
    if (t && tlen >= 10) {
        if (!first) printf(",");
        uint16_t pci = get_le16(t + 4);
        int16_t rsrp = get_le16s(t + 6);
        int16_t rsrq = get_le16s(t + 8);
        printf("{\"type\":\"nr_serving\",\"pci\":%u,\"rsrp\":%d,\"rsrq\":%d}", pci, rsrp, rsrq);
    }

    printf("]}\n");

    /* Also get signal info */
    qmi_send(g_sock, &g_nas_addr, QMI_NAS_GET_SIGNAL_INFO, NULL, 0);
    rlen = 0;
    if (qmi_recv(g_sock, QMI_NAS_GET_SIGNAL_INFO, resp, &rlen, 5000) == 0) {
        /* TLV 0x11: LTE signal */
        t = find_tlv(resp, rlen, 0x11, &tlen);
        if (t && tlen >= 6) {
            int8_t rssi = (int8_t)t[0];
            int8_t rsrq = (int8_t)t[1];
            int16_t rsrp = get_le16s(t + 2);
            int16_t snr = get_le16s(t + 4);
            printf("{\"signal\":{\"lte\":{\"rssi\":%d,\"rsrq\":%d,\"rsrp\":%d,\"snr_x10\":%d}}}\n",
                   rssi, rsrq, rsrp, snr);
        }
        /* TLV 0x14: NR5G signal */
        t = find_tlv(resp, rlen, 0x14, &tlen);
        if (t && tlen >= 4) {
            int16_t rsrp = get_le16s(t);
            int16_t snr = get_le16s(t + 2);
            printf("{\"signal\":{\"nr\":{\"rsrp\":%d,\"snr_x10\":%d}}}\n", rsrp, snr);
        }
    }

    close(g_sock);
    return 0;
}

static int cmd_get_pref(void) {
    if (qmi_init() < 0) return 1;
    qmi_send(g_sock, &g_nas_addr, QMI_NAS_GET_SYS_SEL_PREF, NULL, 0);
    uint8_t resp[MSG_BUF_SIZE];
    int rlen = 0;
    if (qmi_recv(g_sock, QMI_NAS_GET_SYS_SEL_PREF, resp, &rlen, 5000) < 0) {
        close(g_sock);
        return 1;
    }
    printf("{\"preferences\":{");
    uint16_t tlen;
    const uint8_t *t;

    t = find_tlv(resp, rlen, TLV_MODE_PREF, &tlen);
    if (t && tlen >= 2)
        printf("\"mode\":\"0x%04x\"", get_le16(t));

    t = find_tlv(resp, rlen, TLV_LTE_BAND_PREF, &tlen);
    if (t && tlen >= 8)
        printf(",\"lte_bands\":\"0x%016llx\"", (unsigned long long)get_le64(t));

    /* NR5G band preference — read full extended bitmask */
    t = find_tlv(resp, rlen, TLV_NR5G_BAND_PREF, &tlen);
    if (t && tlen > 0) {
        /* Print first 8 bytes as hex for backward compat */
        if (tlen >= 8)
            printf(",\"nr_bands\":\"0x%016llx\"", (unsigned long long)get_le64(t));
        /* Print human-readable NR band list */
        printf(",\"nr_band_list\":[");
        int first = 1;
        for (int b = 1; b <= (int)(tlen * 8) && b <= 512; b++) {
            int byte_idx = (b - 1) / 8;
            int bit_idx = (b - 1) % 8;
            if (byte_idx < tlen && (t[byte_idx] >> bit_idx) & 1) {
                if (!first) printf(",");
                printf("\"n%d\"", b);
                first = 0;
            }
        }
        printf("]");
        /* Check specific important bands */
        if (tlen >= 10) {
            /* n78 = bit 77 = byte 9, bit 5 */
            int has_n78 = (t[9] >> 5) & 1;
            printf(",\"has_n78\":%s", has_n78 ? "true" : "false");
        }
    }

    t = find_tlv(resp, rlen, TLV_NET_SEL_PREF, &tlen);
    if (t && tlen >= 1)
        printf(",\"net_sel\":%d", t[0]);

    printf("}}\n");
    close(g_sock);
    return 0;
}

/*
 * Parse NR band arguments: supports individual band numbers (e.g., "78")
 * or the special keyword "n78". Sets bits in the extended mask.
 */
static void parse_nr_bands(const char *spec, uint8_t *nr_mask) {
    /* Check for keyword "n78" or just "78" */
    if (strcmp(spec, "n78") == 0 || strcmp(spec, "78") == 0) {
        nr5g_mask_set_band(nr_mask, 78);
        return;
    }
    /* Try as hex bitmask (legacy: only covers n1-n64) */
    char *endptr;
    uint64_t val = strtoull(spec, &endptr, 0);
    if (*endptr == '\0' && val != 0) {
        /* Write 64-bit value into first 8 bytes of mask (LE) */
        for (int i = 0; i < 8; i++)
            nr_mask[i] = (val >> (i * 8)) & 0xFF;
        return;
    }
    /* Try as comma-separated band list: "1,78,257" */
    char *tmp = strdup(spec);
    char *tok = strtok(tmp, ",");
    while (tok) {
        int band = atoi(tok);
        if (band > 0 && band <= 512)
            nr5g_mask_set_band(nr_mask, band);
        tok = strtok(NULL, ",");
    }
    free(tmp);
}

static int cmd_band_lock(const char *lte_str, const char *nr_str) {
    if (qmi_init() < 0) return 1;

    uint64_t lte_mask = strtoull(lte_str, NULL, 0);

    /* Build NR5G extended bitmask */
    uint8_t nr_mask[NR5G_MASK_BYTES];
    memset(nr_mask, 0, sizeof(nr_mask));
    int has_nr = 0;
    if (nr_str) {
        parse_nr_bands(nr_str, nr_mask);
        has_nr = !nr5g_mask_is_zero(nr_mask);
    }

    uint8_t tlv[256];
    int pos = 0;

    /* TLV 0x11: Mode preference - LTE + NR5G */
    uint16_t mode = MODE_LTE;
    if (has_nr) mode |= MODE_NR5G;
    put_u8(tlv, &pos, TLV_MODE_PREF);
    put_le16(tlv, &pos, 2);
    put_le16(tlv, &pos, mode);

    /* TLV 0x15: LTE band preference */
    put_u8(tlv, &pos, TLV_LTE_BAND_PREF);
    put_le16(tlv, &pos, 8);
    put_le64(tlv, &pos, lte_mask);

    /* TLV 0x2C: NR5G band preference (extended bitmask) */
    if (has_nr) {
        /* Determine actual length needed (trim trailing zero bytes, min 8) */
        int nr_len = NR5G_MASK_BYTES;
        while (nr_len > 8 && nr_mask[nr_len - 1] == 0) nr_len--;
        /* Round up to next 8-byte boundary for QMI alignment */
        nr_len = ((nr_len + 7) / 8) * 8;
        put_u8(tlv, &pos, TLV_NR5G_BAND_PREF);
        put_le16(tlv, &pos, nr_len);
        memcpy(tlv + pos, nr_mask, nr_len);
        pos += nr_len;
    }

    qmi_send(g_sock, &g_nas_addr, QMI_NAS_SET_SYS_SEL_PREF, tlv, pos);
    uint8_t resp[MSG_BUF_SIZE];
    int rlen = 0;
    int rc = qmi_recv(g_sock, QMI_NAS_SET_SYS_SEL_PREF, resp, &rlen, 5000);

    if (rc == 0) {
        printf("{\"result\":\"OK\",\"lte_bands\":\"0x%llx\"",
               (unsigned long long)lte_mask);

        /* Print human readable band list */
        printf(",\"locked_bands\":[");
        int f = 1;
        for (int b = 1; b <= 64; b++) {
            if (lte_mask & LTE_BAND(b)) {
                if (!f) printf(",");
                printf("\"B%d\"", b);
                f = 0;
            }
        }
        /* NR bands from extended mask */
        for (int b = 1; b <= NR5G_MASK_BYTES * 8; b++) {
            if (nr5g_mask_has_band(nr_mask, b)) {
                if (!f) printf(",");
                printf("\"n%d\"", b);
                f = 0;
            }
        }
        printf("]}\n");
    } else {
        printf("{\"result\":\"FAILED\"}\n");
    }

    close(g_sock);
    return rc;
}

/* Lock to LTE bands + NR n78 in a single command */
static int cmd_band_lock_n78(const char *lte_str) {
    if (qmi_init() < 0) return 1;

    uint64_t lte_mask = lte_str ? strtoull(lte_str, NULL, 0) : MY_LTE_ALL;

    uint8_t nr_mask[NR5G_MASK_BYTES];
    memset(nr_mask, 0, sizeof(nr_mask));
    nr5g_mask_set_band(nr_mask, MY_NR_N78);

    uint8_t tlv[256];
    int pos = 0;

    /* TLV 0x11: Mode - LTE + NR5G */
    uint16_t mode = MODE_LTE | MODE_NR5G;
    put_u8(tlv, &pos, TLV_MODE_PREF);
    put_le16(tlv, &pos, 2);
    put_le16(tlv, &pos, mode);

    /* TLV 0x15: LTE bands */
    put_u8(tlv, &pos, TLV_LTE_BAND_PREF);
    put_le16(tlv, &pos, 8);
    put_le64(tlv, &pos, lte_mask);

    /* TLV 0x2C: NR5G n78 (need 16 bytes for bit 77) */
    int nr_len = 16; /* 128 bits, covers n78 at bit 77 */
    put_u8(tlv, &pos, TLV_NR5G_BAND_PREF);
    put_le16(tlv, &pos, nr_len);
    memcpy(tlv + pos, nr_mask, nr_len);
    pos += nr_len;

    qmi_send(g_sock, &g_nas_addr, QMI_NAS_SET_SYS_SEL_PREF, tlv, pos);
    uint8_t resp[MSG_BUF_SIZE];
    int rlen = 0;
    int rc = qmi_recv(g_sock, QMI_NAS_SET_SYS_SEL_PREF, resp, &rlen, 5000);

    if (rc == 0) {
        printf("{\"result\":\"OK\",\"lte_bands\":\"0x%llx\"",
               (unsigned long long)lte_mask);
        printf(",\"locked_bands\":[");
        int f = 1;
        for (int b = 1; b <= 64; b++) {
            if (lte_mask & LTE_BAND(b)) {
                if (!f) printf(",");
                printf("\"B%d\"", b);
                f = 0;
            }
        }
        printf("%s\"n78\"]}\n", f ? "" : ",");
    } else {
        printf("{\"result\":\"FAILED\"}\n");
    }

    close(g_sock);
    return rc;
}

static int cmd_unlock(void) {
    if (qmi_init() < 0) return 1;

    uint8_t tlv[64];
    int pos = 0;

    /* Mode: all technologies (from get_pref: 0x005c) */
    uint16_t mode = 0x005c;
    put_u8(tlv, &pos, TLV_MODE_PREF);
    put_le16(tlv, &pos, 2);
    put_le16(tlv, &pos, mode);

    /* LTE: all supported bands (from get_pref: 0x0011e7ffffdf3fff) */
    put_u8(tlv, &pos, TLV_LTE_BAND_PREF);
    put_le16(tlv, &pos, 8);
    put_le64(tlv, &pos, 0x0011e7ffffdf3fffULL);

    qmi_send(g_sock, &g_nas_addr, QMI_NAS_SET_SYS_SEL_PREF, tlv, pos);
    uint8_t resp[MSG_BUF_SIZE];
    int rlen = 0;
    int rc = qmi_recv(g_sock, QMI_NAS_SET_SYS_SEL_PREF, resp, &rlen, 5000);
    printf("{\"result\":\"%s\",\"action\":\"unlock_all_bands\"}\n",
           rc == 0 ? "OK" : "FAILED");
    close(g_sock);
    return rc;
}

/* ---- Usage & Main ---- */

static void usage(void) {
    printf(
        "qmi_tool — Band Lock & Cell Info via QRTR/QMI\n"
        "Usage: qmi_tool <command> [args]\n\n"
        "Commands:\n"
        "  test                    Test QRTR connection to modem\n"
        "  cell_info               Get serving & neighbor cell info\n"
        "  get_pref                Get current band preferences\n"
        "  band_lock <lte> [nr]    Lock to specific bands\n"
        "  band_lock_n78 [lte]     Lock NR n78 + optional LTE bands\n"
        "  unlock                  Unlock all bands\n\n"
        "Band lock examples:\n"
        "  band_lock 0x4           Lock to LTE Band 3 only\n"
        "  band_lock 0x44          Lock to LTE B3 + B7\n"
        "  band_lock 0x8000004     Lock to LTE B3 + B28\n"
        "  band_lock 0x4 n78       Lock LTE B3 + NR n78\n"
        "  band_lock 0x4 78        Lock LTE B3 + NR n78 (alt)\n"
        "  band_lock 0x4 1,78      Lock LTE B3 + NR n1 + n78\n"
        "  band_lock_n78           Lock NR n78 + ALL LTE bands\n"
        "  band_lock_n78 0x4       Lock NR n78 + LTE B3 only\n\n"
        "Malaysia LTE band bitmasks:\n"
        "  B1  = 0x1              (2100 MHz)\n"
        "  B3  = 0x4              (1800 MHz)\n"
        "  B7  = 0x40             (2600 MHz)\n"
        "  B8  = 0x80             (900 MHz)\n"
        "  B28 = 0x8000000        (700 MHz)\n"
        "  B40 = 0x8000000000     (2300 MHz)\n"
        "  ALL = 0x80000000C5\n\n"
        "Malaysia NR5G bands:\n"
        "  n78 = 3500 MHz          (Primary 5G band)\n"
    );
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "test") == 0)
        return cmd_test();
    if (strcmp(cmd, "cell_info") == 0)
        return cmd_cell_info();
    if (strcmp(cmd, "get_pref") == 0)
        return cmd_get_pref();
    if (strcmp(cmd, "band_lock") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: qmi_tool band_lock <lte_mask> [nr_bands]\n"); return 1; }
        return cmd_band_lock(argv[2], argc > 3 ? argv[3] : NULL);
    }
    if (strcmp(cmd, "band_lock_n78") == 0) {
        return cmd_band_lock_n78(argc > 2 ? argv[2] : NULL);
    }
    if (strcmp(cmd, "unlock") == 0)
        return cmd_unlock();

    fprintf(stderr, "Unknown command: %s\n", cmd);
    usage();
    return 1;
}
