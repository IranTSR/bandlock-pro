/*
 * qmi_tool — Band Lock & Cell Info via QRTR/QMI
 * Target: Snapdragon 695 (veux), KernelSU root
 * Build: clang -o qmi_tool qmi_tool.c -Wall -O2
 */
#include "qmi_common.h"

static uint16_t g_txn = 1;
static int g_sock = -1;
static struct sockaddr_qrtr g_nas_addr;
static struct sockaddr_qrtr g_dms_addr;

static int qrtr_lookup_dms(int sock, struct sockaddr_qrtr *out) {
    struct qrtr_ctrl_pkt pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
    pkt.service = QMI_SERVICE_DMS;
    pkt.instance = 0;

    struct sockaddr_qrtr ctrl = {
        .sq_family = AF_QIPCRTR,
        .sq_node = 1,
        .sq_port = QRTR_PORT_CTRL
    };

    if (sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&ctrl, sizeof(ctrl)) < 0) return -1;

    struct pollfd pfd = { .fd = sock, .events = POLLIN };
    if (poll(&pfd, 1, 1000) <= 0) return -1;

    uint8_t buf[256];
    int n = recv(sock, buf, sizeof(buf), 0);
    if (n < 20) return -1;

    uint32_t cmd = get_le32(buf);
    uint32_t svc = get_le32(buf + 4);
    if (cmd == QRTR_TYPE_NEW_SERVER && svc == QMI_SERVICE_DMS) {
        out->sq_family = AF_QIPCRTR;
        out->sq_node = get_le32(buf + 12);
        out->sq_port = get_le32(buf + 16);
        return 0;
    }
    return -1;
}

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

        const uint8_t *data = buf + 7;
        if (tlv_out && tlv_len_out) {
            int copy = (msg_len < MSG_BUF_SIZE) ? msg_len : MSG_BUF_SIZE;
            memcpy(tlv_out, data, copy);
            *tlv_len_out = copy;
        }

        /* Check result TLV */
        uint16_t rlen;
        const uint8_t *res = find_tlv(data, msg_len, 0x02, &rlen);
        if (res && rlen >= 4) {
            uint16_t result = get_le16(res);
            if (result != 0) return -1;
        }
        return 0;
    }
}

static long long current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int qmi_recv_ind(int sock, uint16_t exp_msg_id, uint8_t *tlv_out, int *tlv_len_out, int timeout_ms) {
    long long end_ms = current_time_ms() + timeout_ms;
    while (1) {
        long long rem_ms = end_ms - current_time_ms();
        if (rem_ms <= 0) return -1;
        struct pollfd pfd = { .fd = sock, .events = POLLIN };
        if (poll(&pfd, 1, rem_ms) <= 0) return -1;
        
        uint8_t buf[MSG_BUF_SIZE];
        int n = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
        if (n < 7) continue;

        uint8_t type = buf[0];
        uint16_t msg_id = get_le16(buf + 3);
        uint16_t msg_len = get_le16(buf + 5);

        if (type != QMI_INDICATION || msg_id != exp_msg_id) continue;

        const uint8_t *data = buf + 7;
        if (tlv_out && tlv_len_out) {
            int copy = (msg_len < MSG_BUF_SIZE) ? msg_len : MSG_BUF_SIZE;
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
        g_sock = -1;
        return -1;
    }
    qrtr_lookup_dms(g_sock, &g_dms_addr); // DMS is optional for most things, but needed for force-lock
    return 0;
}

static int dms_set_operating_mode(uint8_t mode) {
    if (g_dms_addr.sq_port == 0) return -1;
    uint8_t tlv[16];
    int pos = 0;
    put_u8(tlv, &pos, 0x01); // Mode TLV
    put_le16(tlv, &pos, 1);
    put_u8(tlv, &pos, mode);

    qmi_send(g_sock, &g_dms_addr, QMI_DMS_SET_OPERATING_MODE, tlv, pos);
    uint8_t resp[MSG_BUF_SIZE];
    int rlen = 0;
    return qmi_recv(g_sock, QMI_DMS_SET_OPERATING_MODE, resp, &rlen, 2000);
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
    int rc = qmi_recv(g_sock, QMI_NAS_GET_SYS_SEL_PREF, resp, &rlen, 5000);

    if (rc == 0) {
        printf("{\"get_pref_tlvs\":[\n");
        int pos = 7; // Skip QMI header
        int first = 1;
        while (pos + 3 <= rlen) {
            uint8_t type = resp[pos];
            uint16_t len = get_le16(resp + pos + 1);
            pos += 3;
            if (pos + len > rlen) break;

            if (!first) printf(",\n");
            printf("  {\"id\":\"0x%02X\", \"len\":%d, \"hex\":\"", type, len);
            for (int i = 0; i < len; i++) printf("%02X", resp[pos + i]);
            printf("\"}");
            
            pos += len;
            first = 0;
        }
        printf("\n]}\n");
    } else {
        printf("{\"result\":\"FAILED\", \"error\":%d}\n", rc);
    }

    close(g_sock);
    return rc;
}

static int cmd_modem_info(void) {
    int qsock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    struct sockaddr_qrtr addr;
    if (qrtr_lookup_dms(qsock, &addr) < 0) {
        printf("{\"result\":\"FAILED\",\"error\":\"DMS not found\"}\n");
        close(qsock);
        return 1;
    }

    /* Get MCFG Config ID (Active MBN) */
    qmi_send(qsock, &addr, QMI_DMS_GET_MCFG_CONFIG_ID, NULL, 0);
    uint8_t resp[MSG_BUF_SIZE];
    int rlen = 0;
    char mbn_name[256] = "Unknown";
    if (qmi_recv(qsock, QMI_DMS_GET_MCFG_CONFIG_ID, resp, &rlen, 2000) == 0) {
        uint16_t tlen;
        const uint8_t *t = find_tlv(resp, rlen, 0x01, &tlen); // Active Config ID
        if (t && tlen > 2) {
            int name_len = tlen - 2;
            if (name_len > 255) name_len = 255;
            memcpy(mbn_name, t + 2, name_len);
            mbn_name[name_len] = 0;
        }
    }

    /* Get Capabilities */
    qmi_send(qsock, &addr, QMI_DMS_GET_DEVICE_CAPABILITIES, NULL, 0);
    rlen = 0;
    int has_5g = 0;
    if (qmi_recv(qsock, QMI_DMS_GET_DEVICE_CAPABILITIES, resp, &rlen, 1000) == 0) {
        printf("{\"raw_capabilities\":\"");
        for (int i = 0; i < rlen; i++) printf("%02x", resp[i]);
        printf("\"}\n");

        uint16_t tlen;
        const uint8_t *t = find_tlv(resp, rlen, 0x11, &tlen); // RAT capabilities
        if (t && tlen > 0) {
            for (int i = 0; i < tlen; i++) if (t[i] == 0x05) has_5g = 1; // 0x05 = NR5G
        }
    }

    /* Get Active MBN ID (0x0042) */
    uint8_t mbn_id[256] = "Unknown";
    uint8_t payload_mbn[1] = { 0x01 }; // SW Config
    qmi_send(qsock, &addr, QMI_DMS_GET_MCFG_CONFIG_ID, payload_mbn, 1);
    if (qmi_recv(qsock, QMI_DMS_GET_MCFG_CONFIG_ID, resp, &rlen, 1000) == 0) {
        uint16_t tlen;
        const uint8_t *t = find_tlv(resp, rlen, 0x01, &tlen); // Config ID
        if (t && tlen > 2) {
            int name_len = t[1];
            if (name_len > 255) name_len = 255;
            memcpy(mbn_id, t + 2, name_len);
            mbn_id[name_len] = 0;
        }
    }

    printf("{\"active_mbn\":\"%s\", \"nr5g_supported\":%s}\n", 
           mbn_id, has_5g ? "true" : "false");
    close(qsock);
    return 0;
}

static int cmd_list_mbns(void) {
    int qsock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    struct sockaddr_qrtr addr;
    if (qrtr_lookup_dms(qsock, &addr) < 0) {
        printf("{\"result\":\"FAILED\",\"error\":\"DMS not found\"}\n");
        close(qsock); return 1;
    }

    /* List Configs (0x0041) */
    uint8_t payload[1] = { 0x01 }; // Config type: SW
    qmi_send(qsock, &addr, QMI_DMS_LIST_CONFIGS, payload, 1);
    uint8_t resp[MSG_BUF_SIZE];
    int rlen = 0;
    int rc = qmi_recv(qsock, QMI_DMS_LIST_CONFIGS, resp, &rlen, 3000);
    
    if (rc != 0) {
        printf("{\"result\":\"FAILED\",\"error\":\"TIMEOUT\"}\n");
        close(qsock); return 1;
    }

    printf("{\"raw_mbn_data\":\"");
    for (int i = 0; i < rlen; i++) printf("%02x", resp[i]);
    printf("\"}\n");

    uint16_t tlen;
    const uint8_t *t = find_tlv(resp, rlen, 0x01, &tlen); // Config List
    if (t && tlen > 0) {
        int num_configs = t[0];
        printf("{\"mbn_list\":[");
        const uint8_t *p = t + 1;
        for (int i = 0; i < num_configs; i++) {
            if (p >= t + tlen) break;
            int name_len = *p++;
            char name[256];
            if (name_len > 255) name_len = 255;
            memcpy(name, p, name_len);
            name[name_len] = 0;
            p += name_len;
            if (p >= t + tlen) break;
            uint8_t active = *p++;
            printf("%s{\"name\":\"%s\",\"active\":%s}", i > 0 ? "," : "", name, active ? "true" : "false");
        }
        printf("]}\n");
    }
    close(qsock);
    return 0;
}

static int cmd_scan_fs_mbns(void) {
    const char *paths[] = {
        "/vendor/firmware_mnt/image/modem_pr/mcfg/configs/mcfg_sw/",
        "/vendor/modem_pr/mcfg/configs/mcfg_sw/",
        "/data/vendor/modem_config/",
        NULL
    };
    const char *filters[] = {"SEA", "Malaysia", "UMobile", "Maxis", "Digi", "Celcom", "YTL", "MY", NULL};
    
    printf("{\"scan_results\":[\n");
    int first = 1;
    for (int p = 0; paths[p]; p++) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "find %s -name '*.mbn' 2>/dev/null", paths[p]);
        FILE *fp = popen(cmd, "r");
        if (!fp) continue;
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\n")] = 0;
            int match = 0;
            for (int f = 0; filters[f]; f++) {
                if (strstr(line, filters[f])) { match = 1; break; }
            }
            if (match) {
                if (!first) printf(",\n");
                printf("  \"%s\"", line);
                first = 0;
            }
        }
        pclose(fp);
    }
    printf("\n]}\n");
    return 0;
}

/* PDC service lookup */
static int qrtr_lookup_pdc(int sock, struct sockaddr_qrtr *addr) {
    struct qrtr_ctrl_pkt req = { .cmd = QRTR_TYPE_NEW_LOOKUP, .service = QMI_SERVICE_PDC, .instance = 0, .node = 0, .port = 0 };
    struct sockaddr_qrtr sq = { .sq_family = AF_QIPCRTR, .sq_node = 1, .sq_port = QRTR_PORT_CTRL };
    sendto(sock, &req, sizeof(req), 0, (void*)&sq, sizeof(sq));
    struct pollfd pfd = { .fd = sock, .events = POLLIN };
    while (poll(&pfd, 1, 1000) > 0) {
        struct qrtr_ctrl_pkt resp;
        if (recv(sock, &resp, sizeof(resp), 0) != sizeof(resp)) break;
        if (resp.cmd == QRTR_TYPE_NEW_SERVER && resp.service == QMI_SERVICE_PDC) {
            addr->sq_family = AF_QIPCRTR;
            addr->sq_node = resp.node;
            addr->sq_port = resp.port;
            return 0;
        }
    }
    return -1;
}

static int cmd_pdc_list(void) {
    int qsock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    struct sockaddr_qrtr addr;
    if (qrtr_lookup_pdc(qsock, &addr) < 0) {
        printf("{\"result\":\"FAILED\",\"error\":\"PDC service not found\"}\n");
        close(qsock); return 1;
    }
    printf("{\"pdc_service\":\"FOUND\",\"node\":%d,\"port\":%d}\n", addr.sq_node, addr.sq_port);

    /* Register for indications first */
    uint8_t reg_payload[4];
    int rpos = 0;
    put_u8(reg_payload, &rpos, 0x10); put_le16(reg_payload, &rpos, 1); put_u8(reg_payload, &rpos, 0x01);
    qmi_send(qsock, &addr, QMI_PDC_REGISTER, reg_payload, rpos);
    uint8_t resp[MSG_BUF_SIZE];
    int rlen = 0;
    qmi_recv(qsock, QMI_PDC_REGISTER, resp, &rlen, 1000);

    /* Get Selected Config for slot 0 */
    uint8_t sel_payload[16];
    int spos = 0;
    put_u8(sel_payload, &spos, 0x01); put_le16(sel_payload, &spos, 4);
    put_le32(sel_payload, &spos, 0x00); // Config type: SW
    put_u8(sel_payload, &spos, 0x10); put_le16(sel_payload, &spos, 4);
    put_le32(sel_payload, &spos, 0x00); // Subscription ID: 0
    qmi_send(qsock, &addr, QMI_PDC_GET_SELECTED_CONFIG, sel_payload, spos);
    rlen = 0;
    if (qmi_recv(qsock, QMI_PDC_GET_SELECTED_CONFIG, resp, &rlen, 2000) == 0) {
        printf("{\"raw_pdc_selected\":\"");
        for (int i = 0; i < rlen; i++) printf("%02x", resp[i]);
        printf("\"}\n");
        /* Try to parse config ID from TLV 0x11 */
        uint16_t tlen;
        const uint8_t *t = find_tlv(resp, rlen, 0x11, &tlen);
        if (t && tlen > 0) {
            char config_id[256];
            int len = tlen > 255 ? 255 : tlen;
            memcpy(config_id, t, len);
            config_id[len] = 0;
            printf("{\"active_pdc_config\":\"%s\"}\n", config_id);
        }
    } else {
        printf("{\"pdc_selected\":\"TIMEOUT\"}\n");
    }

    /* List configs */
    uint8_t list_payload[8];
    int lpos = 0;
    put_u8(list_payload, &lpos, 0x01); put_le16(list_payload, &lpos, 4);
    put_le32(list_payload, &lpos, 0x00); // Config type: SW
    qmi_send(qsock, &addr, QMI_PDC_LIST_CONFIGS, list_payload, lpos);
    
    /* Wait for response first */
    qmi_recv(qsock, QMI_PDC_LIST_CONFIGS, resp, &rlen, 1000);
    
    /* Now wait for INDICATION */
    rlen = 0;
    if (qmi_recv_ind(qsock, QMI_PDC_LIST_CONFIGS, resp, &rlen, 3000) == 0) {
        printf("{\"raw_pdc_list_ind\":\"");
        for (int i = 0; i < rlen; i++) printf("%02x", resp[i]);
        printf("\"}\n");
    } else {
        printf("{\"pdc_list_ind\":\"TIMEOUT\"}\n");
    }

    close(qsock);
    return 0;
}

static int cmd_force_mbn(const char *config_id) {
    if (!config_id) return 1;
    int qsock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    struct sockaddr_qrtr addr;
    if (qrtr_lookup_pdc(qsock, &addr) < 0) {
        printf("{\"result\":\"FAILED\",\"error\":\"PDC service not found\"}\n");
        close(qsock); return 1;
    }

    uint8_t resp[MSG_BUF_SIZE];
    int rlen = 0;
    
    printf("{\"action\":\"force_mbn\", \"target_id\":\"%s\", \"steps\":[\n", config_id);
    
    /* 1. Set Selected Config */
    uint8_t set_payload[256];
    int spos = 0;
    int id_len = strlen(config_id);
    put_u8(set_payload, &spos, 0x01); put_le16(set_payload, &spos, id_len);
    memcpy(set_payload + spos, config_id, id_len); spos += id_len;
    put_u8(set_payload, &spos, 0x10); put_le16(set_payload, &spos, 4);
    put_le32(set_payload, &spos, 0x00); // Config type: SW
    put_u8(set_payload, &spos, 0x11); put_le16(set_payload, &spos, 4);
    put_le32(set_payload, &spos, 0x00); // Subscription ID: 0
    
    qmi_send(qsock, &addr, QMI_PDC_SET_SELECTED_CONFIG, set_payload, spos);
    int set_ok = (qmi_recv(qsock, QMI_PDC_SET_SELECTED_CONFIG, resp, &rlen, 2000) == 0);
    printf("  {\"step\":\"SET_SELECTED_CONFIG\", \"status\":\"%s\"},\n", set_ok ? "SUCCESS" : "FAILED");
    
    /* 2. Activate Config */
    uint8_t act_payload[16];
    int apos = 0;
    put_u8(act_payload, &apos, 0x01); put_le16(act_payload, &apos, 4);
    put_le32(act_payload, &apos, 0x00); // Config type: SW
    put_u8(act_payload, &apos, 0x10); put_le16(act_payload, &apos, 4);
    put_le32(act_payload, &apos, 0x00); // Subscription ID: 0
    
    qmi_send(qsock, &addr, QMI_PDC_ACTIVATE_CONFIG, act_payload, apos);
    int act_ok = (qmi_recv(qsock, QMI_PDC_ACTIVATE_CONFIG, resp, &rlen, 3000) == 0);
    printf("  {\"step\":\"ACTIVATE_CONFIG\", \"status\":\"%s\"}\n", act_ok ? "SUCCESS" : "FAILED");
    
    printf("]}\n");
    close(qsock);
    return (set_ok && act_ok) ? 0 : 1;
}

static int parse_nr_band_token(const char *tok) {
    const char *numstr = tok;
    /* Strip leading 'n' or 'N' prefix if present */
    if ((tok[0] == 'n' || tok[0] == 'N') && tok[1] >= '0' && tok[1] <= '9')
        numstr = tok + 1;
    int band = atoi(numstr);
    return (band > 0 && band <= 512) ? band : 0;
}

/*
 * Parse NR band arguments into extended bitmask.
 * Supports:
 *   "n78"         - single band with prefix
 *   "78"          - single band number
 *   "n28,n78"     - comma-separated with prefix
 *   "28,78"       - comma-separated numbers
 *   "n28+n78"     - plus-separated (NR-CA notation)
 *   "0x1234"      - hex bitmask (legacy, covers n1-n64 only)
 */
static void parse_nr_bands(const char *spec, uint8_t *nr_mask) {
    /* Check for comma or plus separators first */
    if (strchr(spec, ',') || strchr(spec, '+')) {
        char *tmp = strdup(spec);
        /* Replace '+' with ',' for uniform tokenizing */
        for (char *p = tmp; *p; p++) {
            if (*p == '+') *p = ',';
        }
        char *tok = strtok(tmp, ",");
        while (tok) {
            /* Skip whitespace */
            while (*tok == ' ') tok++;
            int band = parse_nr_band_token(tok);
            if (band > 0)
                nr5g_mask_set_band(nr_mask, band);
            tok = strtok(NULL, ",");
        }
        free(tmp);
        return;
    }

    /* Try as single band token: "n78", "78", "n28", etc. */
    int band = parse_nr_band_token(spec);
    if (band > 0) {
        nr5g_mask_set_band(nr_mask, band);
        return;
    }

    /* Try as hex bitmask (legacy: only covers n1-n64) */
    char *endptr;
    uint64_t val = strtoull(spec, &endptr, 0);
    if (*endptr == '\0' && val != 0) {
        for (int i = 0; i < 8; i++)
            nr_mask[i] = (val >> (i * 8)) & 0xFF;
    }
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
        uint16_t qmi_err = 0;
        uint16_t tlen;
        const uint8_t *t = find_tlv(resp, rlen, 0x02, &tlen);
        if (t && tlen >= 4) qmi_err = get_le16(t + 2);
        printf("{\"result\":\"FAILED\",\"error\":%d}\n", qmi_err);
    }

    close(g_sock);
    return rc;
}

/*
 * General NR5G band lock command.
 * nr_spec: NR band specification (e.g., "n78", "n28,n78", "n28+n78")
 * lte_str: Optional LTE band hex mask. If NULL, defaults to MY_LTE_ALL.
 */
typedef struct {
    uint32_t node;
    uint32_t port;
} qmi_endpoint_t;

static int qrtr_lookup_all_nas(qmi_endpoint_t *endpoints, int max_eps) {
    int sock = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_qrtr sq = { .sq_family = AF_QIPCRTR, .sq_node = 1, .sq_port = QRTR_PORT_CTRL };
    struct qrtr_ctrl_pkt pkt = { .cmd = QRTR_TYPE_NEW_LOOKUP, .service = QMI_SERVICE_NAS, .instance = 0, .node = 0, .port = 0 };

    if (sendto(sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&sq, sizeof(sq)) < 0) {
        close(sock);
        return -1;
    }

    int count = 0;
    struct pollfd pfd = { .fd = sock, .events = POLLIN };
    while (count < max_eps && poll(&pfd, 1, 500) > 0) {
        struct qrtr_ctrl_pkt resp;
        if (recv(sock, &resp, sizeof(resp), 0) != sizeof(resp)) break;
        if (resp.cmd == QRTR_TYPE_NEW_SERVER) {
            endpoints[count].node = resp.node;
            endpoints[count].port = resp.port;
            count++;
        }
    }
    close(sock);
    return count;
}

static int cmd_nr_lock(const char *nr_spec, const char *lte_str) {
    qmi_endpoint_t endpoints[8];
    int num_eps = qrtr_lookup_all_nas(endpoints, 8);
    if (num_eps <= 0) {
        printf("{\"result\":\"FAILED\",\"error\":\"No NAS service found\"}\n");
        return 1;
    }

    uint64_t lte_mask = lte_str ? strtoull(lte_str, NULL, 0) : 0x0011e7ffffdf3fffULL;
    uint8_t nr_mask[NR5G_MASK_BYTES];
    memset(nr_mask, 0, sizeof(nr_mask));
    parse_nr_bands(nr_spec, nr_mask);

    uint8_t tlv[512];
    int pos = 0;
    put_u8(tlv, &pos, 0x11); put_le16(tlv, &pos, 2); put_le16(tlv, &pos, 0x50); // Mode
    put_u8(tlv, &pos, 0x15); put_le16(tlv, &pos, 8); put_le64(tlv, &pos, lte_mask); // LTE
    put_u8(tlv, &pos, 0x6C); put_le16(tlv, &pos, 16); memcpy(tlv+pos, nr_mask, 16); pos+=16; // NR
    put_u8(tlv, &pos, 0x74); put_le16(tlv, &pos, 1); put_u8(tlv, &pos, 0x01); // Xiaomi Enabler 1
    put_u8(tlv, &pos, 0x76); put_le16(tlv, &pos, 1); put_u8(tlv, &pos, 0x01); // Xiaomi Enabler 2
    put_u8(tlv, &pos, 0x1A); put_le16(tlv, &pos, 1); put_u8(tlv, &pos, 0x01); // Permanent

    printf("{\"nodes\":[\n");
    int success = 0;
    for (int i = 0; i < num_eps; i++) {
        int s = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
        struct sockaddr_qrtr addr = { .sq_family = AF_QIPCRTR, .sq_node = endpoints[i].node, .sq_port = endpoints[i].port };
        qmi_send(s, &addr, QMI_NAS_SET_SYS_SEL_PREF, tlv, pos);
        
        uint8_t resp[MSG_BUF_SIZE];
        int rlen = 0;
        int rc = qmi_recv(s, QMI_NAS_SET_SYS_SEL_PREF, resp, &rlen, 1000); // Shortened to 1s
        
        uint16_t qmi_err = 0;
        if (rc == 0) { // Success receiving response
            uint16_t tlen;
            const uint8_t *t = find_tlv(resp, rlen, 0x02, &tlen);
            if (t && tlen >= 4) {
                uint16_t res = get_le16(t);
                qmi_err = get_le16(t + 2);
                if (res == 0) success = 1;
            }
        }

        if (i > 0) printf(",\n");
        printf("  {\"node\":%d, \"port\":%d, \"rc\":%d, \"error\":%d}", 
               endpoints[i].node, endpoints[i].port, rc, qmi_err);
        close(s);
    }
    printf("\n], \"result\":\"%s\"}\n", success ? "OK" : "FAILED");
    return success ? 0 : 1;
}

static int cmd_unlock(void) {
    if (qmi_init() < 0) return 1;

    uint8_t tlv[128];
    int pos = 0;

    /* Mode: LTE + NR5G + others to prevent rejection */
    uint16_t mode = 0x5c;
    put_u8(tlv, &pos, TLV_MODE_PREF);
    put_le16(tlv, &pos, 2);
    put_le16(tlv, &pos, mode);

    /* LTE: all supported bands */
    put_u8(tlv, &pos, TLV_LTE_BAND_PREF);
    put_le16(tlv, &pos, 8);
    put_le64(tlv, &pos, 0x0011e7ffffdf3fffULL);

    /* Note: We omit TLV_NR5G_BAND_PREF to let the modem revert to default (All Bands) */

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

static int cmd_get_nr_pref(void) {
    if (qmi_init() < 0) return 1;
    qmi_send(g_sock, &g_nas_addr, QMI_NAS_GET_NR5G_BAND_PREF, NULL, 0);
    uint8_t resp[MSG_BUF_SIZE];
    int rlen = 0;
    int rc = qmi_recv(g_sock, QMI_NAS_GET_NR5G_BAND_PREF, resp, &rlen, 5000);
    if (rc == 0) {
        uint16_t tlen;
        const uint8_t *t = find_tlv(resp, rlen, 0x01, &tlen);
        printf("{\"nr_pref_status\":\"OK\",\"tlv_0x01_len\":%d", tlen);
        if (t && tlen >= 8) printf(",\"mask_0\":\"0x%016llx\"", (unsigned long long)get_le64(t));
        printf("}\n");
    } else {
        printf("{\"nr_pref_status\":\"FAILED\"}\n");
    }
    close(g_sock);
    return rc;
}

static void usage(void) {
    printf(
        "qmi_tool — Band Lock & Cell Info via QRTR/QMI\n"
        "Usage: qmi_tool <command> [args]\n\n"
        "Commands:\n"
        "  test                    Test QRTR connection to modem\n"
        "  cell_info               Get serving & neighbor cell info\n"
        "  get_pref                Get current band preferences\n"
        "  band_lock <lte> [nr]    Lock LTE bands (+ optional NR bands)\n"
        "  nr_lock <nr> [lte]      Lock NR bands (+ optional LTE mask)\n"
        "  unlock                  Unlock all bands (LTE + NR)\n\n"
        "NR band lock examples (NR Carrier Aggregation):\n"
        "  nr_lock n78             Lock NR n78 only (+ all LTE)\n"
        "  nr_lock n28+n78         Lock NR n28+n78 (NR-CA) + all LTE\n"
        "  nr_lock n28,n78         Lock NR n28+n78 (alt syntax)\n"
        "  nr_lock n28+n78 0x4     Lock NR n28+n78 + LTE B3 only\n"
        "  nr_lock n41+n78         Lock NR n41+n78 (NR-CA)\n"
        "  nr_lock n28+n41+n78     Lock 3x NR-CA\n\n"
        "LTE band lock examples:\n"
        "  band_lock 0x4           Lock to LTE Band 3 only\n"
        "  band_lock 0x44          Lock to LTE B3 + B7\n"
        "  band_lock 0x8000004     Lock to LTE B3 + B28\n"
        "  band_lock 0x4 n78       Lock LTE B3 + NR n78\n"
        "  band_lock 0x4 n28+n78   Lock LTE B3 + NR n28+n78\n\n"
        "Malaysia LTE band bitmasks:\n"
        "  B1  = 0x1              (2100 MHz)\n"
        "  B3  = 0x4              (1800 MHz)\n"
        "  B7  = 0x40             (2600 MHz)\n"
        "  B8  = 0x80             (900 MHz)\n"
        "  B28 = 0x8000000        (700 MHz)\n"
        "  B40 = 0x8000000000     (2300 MHz)\n"
        "  ALL = 0x80000000C5\n\n"
        "Malaysia NR5G bands:\n"
        "  n28 = 700 MHz           (Low-band 5G, coverage)\n"
        "  n41 = 2500 MHz          (Mid-band 5G, TDD)\n"
        "  n78 = 3500 MHz          (Primary 5G, speed)\n"
    );
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "test") == 0)
        return cmd_test();
    if (strcmp(cmd, "cell_info") == 0)
        return cmd_cell_info();
    if (strcmp(cmd, "modem_info") == 0)
        return cmd_modem_info();
    if (strcmp(cmd, "list_mbns") == 0)
        return cmd_pdc_list();
    if (strcmp(cmd, "pdc_list") == 0)
        return cmd_pdc_list();
    if (strcmp(cmd, "force_mbn") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: qmi_tool force_mbn <config_id>\n"); return 1; }
        return cmd_force_mbn(argv[2]);
    }
    if (strcmp(cmd, "scan_fs_mbns") == 0)
        return cmd_scan_fs_mbns();
    if (strcmp(cmd, "get_pref") == 0)
        return cmd_get_pref();
    if (strcmp(cmd, "get_nr_pref") == 0)
        return cmd_get_nr_pref();
    if (strcmp(cmd, "band_lock") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: qmi_tool band_lock <lte_mask> [nr_bands]\n"); return 1; }
        return cmd_band_lock(argv[2], argc > 3 ? argv[3] : NULL);
    }
    if (strcmp(cmd, "nr_lock") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: qmi_tool nr_lock <nr_bands> [lte_mask]\n"); return 1; }
        return cmd_nr_lock(argv[2], argc > 3 ? argv[3] : NULL);
    }
    /* Backward compat: band_lock_n78 → nr_lock n78 */
    if (strcmp(cmd, "band_lock_n78") == 0) {
        return cmd_nr_lock("n78", argc > 2 ? argv[2] : NULL);
    }
    if (strcmp(cmd, "unlock") == 0)
        return cmd_unlock();

    fprintf(stderr, "Unknown command: %s\n", cmd);
    usage();
    return 1;
}
