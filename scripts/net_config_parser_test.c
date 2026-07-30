/* Standalone host-side test for the /microk/net.cfg parser in kernel/net.c
 * (net_load_config_text and its helpers). Re-implements the exact same
 * parsing logic against mock config setters, so the parser's behavior on
 * valid/invalid/edge-case input can be verified without booting QEMU.
 *
 * If the parsing logic in kernel/net.c changes, keep this copy in sync. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ---- verbatim copies of the static helpers from kernel/net.c ---- */

static int parse_octet(const char **cursor, uint8_t *out) {
    uint32_t value = 0;
    int digits = 0;
    const char *p = *cursor;

    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (uint32_t)(*p - '0');
        if (value > 255) {
            return 0;
        }
        digits++;
        p++;
    }

    if (digits == 0) {
        return 0;
    }

    *out = (uint8_t)value;
    *cursor = p;
    return 1;
}

static int parse_ipv4(const char *text, uint8_t out[4]) {
    const char *p = text;

    if (!text || !text[0]) {
        return 0;
    }

    for (int i = 0; i < 4; i++) {
        if (!parse_octet(&p, &out[i])) {
            return 0;
        }
        if (i < 3) {
            if (*p != '.') {
                return 0;
            }
            p++;
        }
    }

    return *p == '\0';
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void trim(char *text) {
    char *start = text;
    char *end;

    while (is_space(*start)) {
        start++;
    }
    if (start != text) {
        char *dst = text;
        while (*start) {
            *dst++ = *start++;
        }
        *dst = '\0';
    }

    end = text + strlen(text);
    while (end > text && is_space(end[-1])) {
        end--;
    }
    *end = '\0';
}

static int copy_token(char *out, uint32_t out_size, const char *start, const char *end) {
    uint32_t pos = 0;

    if (!out || out_size == 0 || !start || !end || end < start) {
        return 0;
    }

    while (start < end && pos + 1 < out_size) {
        out[pos++] = *start++;
    }
    out[pos] = '\0';
    trim(out);
    return pos > 0;
}

static int key_equals(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static int parse_u16(const char *text, uint16_t *out) {
    uint32_t value = 0;
    const char *p = text;

    if (!text || !text[0] || !out) {
        return 0;
    }
    while (*p) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        value = value * 10 + (uint32_t)(*p - '0');
        if (value > 0xFFFF) {
            return 0;
        }
        p++;
    }
    *out = (uint16_t)value;
    return 1;
}

/* ---- mock config state + setters, standing in for the real net_config_t
 * and its hardware-dependent setters (net_config_dhcp() touches e1000/DHCP,
 * so it can't run outside QEMU) ---- */

typedef struct {
    int dhcp_calls;
    int static_calls;
    uint8_t ip[4], netmask[4], gateway[4];
    int dns_calls;
    uint8_t dns[4];
    int hostname_calls;
    char hostname[64];
    int llm_net_calls;
    int llm_net_enabled;
    int llm_port_calls;
    uint16_t llm_port;
} mock_state_t;

static mock_state_t g_mock;

static int mock_config_dhcp(void) {
    g_mock.dhcp_calls++;
    return 1; /* pretend NIC present, unlike real net_config_dhcp() w/o hw */
}

static int mock_config_static(const char *ip, const char *netmask, const char *gateway) {
    uint8_t pip[4], pmask[4], pgw[4];
    if (!parse_ipv4(ip, pip) || !parse_ipv4(netmask, pmask) || !parse_ipv4(gateway, pgw)) {
        return 0;
    }
    g_mock.static_calls++;
    memcpy(g_mock.ip, pip, 4);
    memcpy(g_mock.netmask, pmask, 4);
    memcpy(g_mock.gateway, pgw, 4);
    return 1;
}

static int mock_config_dns(const char *dns) {
    uint8_t pdns[4];
    if (!parse_ipv4(dns, pdns)) return 0;
    g_mock.dns_calls++;
    memcpy(g_mock.dns, pdns, 4);
    return 1;
}

static int mock_config_hostname(const char *hostname) {
    uint32_t len;
    if (!hostname || !hostname[0]) return 0;
    len = strlen(hostname);
    if (len >= sizeof(g_mock.hostname)) len = sizeof(g_mock.hostname) - 1;
    memcpy(g_mock.hostname, hostname, len);
    g_mock.hostname[len] = '\0';
    g_mock.hostname_calls++;
    return 1;
}

static int mock_llm_service_set_enabled(int enabled) {
    g_mock.llm_net_calls++;
    g_mock.llm_net_enabled = enabled;
    return 1;
}

static int mock_llm_service_set_port(uint16_t port) {
    g_mock.llm_port_calls++;
    g_mock.llm_port = port;
    return 1;
}

/* ---- net_load_config_text(), mirroring kernel/net.c's version key for key,
 * but calling the mocks above instead of the real net_config_t setters ---- */

static int mock_load_config_text(const char *text, uint32_t size) {
    uint32_t pos = 0;
    int applied = 0;
    int static_seen = 0;
    char ip[24];
    char netmask[24];
    char gateway[24];
    char dns[24];

    memset(ip, 0, sizeof(ip));
    memset(netmask, 0, sizeof(netmask));
    memset(gateway, 0, sizeof(gateway));
    memset(dns, 0, sizeof(dns));

    if (!text || size == 0) {
        return 0;
    }

    while (pos < size && text[pos]) {
        const char *line_start = text + pos;
        const char *line_end;
        const char *equals;
        char key[16];
        char value[40];

        while (pos < size && text[pos] && text[pos] != '\n') {
            pos++;
        }
        line_end = text + pos;
        if (pos < size && text[pos] == '\n') {
            pos++;
        }

        while (line_start < line_end && is_space(*line_start)) {
            line_start++;
        }
        if (line_start >= line_end || *line_start == '#') {
            continue;
        }

        equals = line_start;
        while (equals < line_end && *equals != '=') {
            equals++;
        }
        if (equals >= line_end) {
            continue;
        }

        if (!copy_token(key, sizeof(key), line_start, equals) ||
            !copy_token(value, sizeof(value), equals + 1, line_end)) {
            continue;
        }

        if (key_equals(key, "mode")) {
            if (strcmp(value, "dhcp") == 0) {
                applied |= mock_config_dhcp();
            } else if (strcmp(value, "static") == 0) {
                static_seen = 1;
            }
        } else if (key_equals(key, "ip")) {
            strncpy(ip, value, sizeof(ip) - 1);
        } else if (key_equals(key, "netmask")) {
            strncpy(netmask, value, sizeof(netmask) - 1);
        } else if (key_equals(key, "gateway")) {
            strncpy(gateway, value, sizeof(gateway) - 1);
        } else if (key_equals(key, "dns")) {
            strncpy(dns, value, sizeof(dns) - 1);
        } else if (key_equals(key, "hostname")) {
            applied |= mock_config_hostname(value);
        } else if (key_equals(key, "llm_net")) {
            if (strcmp(value, "on") == 0 || strcmp(value, "enabled") == 0 || strcmp(value, "1") == 0) {
                applied |= mock_llm_service_set_enabled(1);
            } else if (strcmp(value, "off") == 0 || strcmp(value, "disabled") == 0 || strcmp(value, "0") == 0) {
                applied |= mock_llm_service_set_enabled(0);
            }
        } else if (key_equals(key, "llm_port")) {
            uint16_t port;
            if (parse_u16(value, &port)) {
                applied |= mock_llm_service_set_port(port);
            }
        }
    }

    if (static_seen || ip[0] || netmask[0] || gateway[0]) {
        if (!ip[0] || !netmask[0] || !gateway[0]) {
            return applied;
        }
        applied |= mock_config_static(ip, netmask, gateway);
    }

    if (dns[0]) {
        applied |= mock_config_dns(dns);
    }

    return applied;
}

/* ---- test harness ---- */

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

static void reset_mock(void) {
    memset(&g_mock, 0, sizeof(g_mock));
}

static void test_full_static_config(void) {
    const char *text =
        "# example static config\n"
        "mode=static\n"
        "ip=192.168.1.50\n"
        "netmask=255.255.255.0\n"
        "gateway=192.168.1.1\n"
        "dns=8.8.8.8\n"
        "hostname=microk-box\n"
        "llm_net=on\n"
        "llm_port=1234\n";
    reset_mock();
    int applied = mock_load_config_text(text, (uint32_t)strlen(text));

    CHECK(applied != 0, "full static config should report applied changes");
    CHECK(g_mock.static_calls == 1, "static config should be applied exactly once");
    CHECK(g_mock.ip[0] == 192 && g_mock.ip[1] == 168 && g_mock.ip[2] == 1 && g_mock.ip[3] == 50, "ip parsed correctly");
    CHECK(g_mock.gateway[3] == 1, "gateway parsed correctly");
    CHECK(g_mock.dns_calls == 1 && g_mock.dns[0] == 8, "dns applied and parsed");
    CHECK(g_mock.hostname_calls == 1 && strcmp(g_mock.hostname, "microk-box") == 0, "hostname applied");
    CHECK(g_mock.llm_net_calls == 1 && g_mock.llm_net_enabled == 1, "llm_net=on applied");
    CHECK(g_mock.llm_port_calls == 1 && g_mock.llm_port == 1234, "llm_port applied");
}

static void test_dhcp_config(void) {
    const char *text = "mode=dhcp\n";
    reset_mock();
    mock_load_config_text(text, (uint32_t)strlen(text));
    CHECK(g_mock.dhcp_calls == 1, "mode=dhcp triggers dhcp setter");
    CHECK(g_mock.static_calls == 0, "mode=dhcp must not also apply static config");
}

static void test_incomplete_static_config_not_applied(void) {
    /* Missing gateway: static config should not be applied at all. */
    const char *text =
        "mode=static\n"
        "ip=10.0.0.5\n"
        "netmask=255.255.255.0\n";
    reset_mock();
    mock_load_config_text(text, (uint32_t)strlen(text));
    CHECK(g_mock.static_calls == 0, "incomplete static config (missing gateway) is not applied");
}

static void test_invalid_ip_rejected(void) {
    const char *text =
        "mode=static\n"
        "ip=999.1.1.1\n"
        "netmask=255.255.255.0\n"
        "gateway=10.0.0.1\n";
    reset_mock();
    mock_load_config_text(text, (uint32_t)strlen(text));
    CHECK(g_mock.static_calls == 0, "octet > 255 makes ip invalid, static config rejected");

    reset_mock();
    const char *text2 =
        "mode=static\n"
        "ip=10.0.0\n" /* only 3 octets */
        "netmask=255.255.255.0\n"
        "gateway=10.0.0.1\n";
    mock_load_config_text(text2, (uint32_t)strlen(text2));
    CHECK(g_mock.static_calls == 0, "ip with only 3 octets is invalid, static config rejected");
}

static void test_unknown_key_and_missing_equals_ignored(void) {
    const char *text =
        "banana\n"           /* no '=' at all */
        "unknown_key=value\n" /* unrecognized key */
        "mode=dhcp\n";
    reset_mock();
    int applied = mock_load_config_text(text, (uint32_t)strlen(text));
    CHECK(g_mock.dhcp_calls == 1, "valid line still parsed after malformed/unknown lines");
    CHECK(applied != 0, "dhcp line still counted as applied");
}

static void test_llm_port_invalid_value_ignored(void) {
    const char *text = "llm_port=notanumber\n";
    reset_mock();
    mock_load_config_text(text, (uint32_t)strlen(text));
    CHECK(g_mock.llm_port_calls == 0, "non-numeric llm_port is rejected");

    reset_mock();
    const char *text2 = "llm_port=99999\n"; /* exceeds uint16_t range */
    mock_load_config_text(text2, (uint32_t)strlen(text2));
    CHECK(g_mock.llm_port_calls == 0, "llm_port exceeding 65535 is rejected");
}

static void test_whitespace_and_crlf_tolerant(void) {
    const char *text = "  mode = static \r\nip=10.0.0.2\r\nnetmask=255.0.0.0\r\ngateway=10.0.0.1\r\n";
    reset_mock();
    mock_load_config_text(text, (uint32_t)strlen(text));
    CHECK(g_mock.static_calls == 1, "leading/trailing whitespace and CRLF line endings are tolerated");
    CHECK(g_mock.ip[0] == 10 && g_mock.ip[3] == 2, "ip parsed correctly despite surrounding whitespace");
}

static void test_blank_lines_and_comments_skipped(void) {
    const char *text = "\n\n# just a comment\n   \nmode=dhcp\n# trailing comment\n";
    reset_mock();
    mock_load_config_text(text, (uint32_t)strlen(text));
    CHECK(g_mock.dhcp_calls == 1, "blank lines and comment-only lines do not break parsing");
}

static void test_empty_input(void) {
    reset_mock();
    int applied = mock_load_config_text("", 0);
    CHECK(applied == 0, "empty input applies nothing");

    reset_mock();
    applied = mock_load_config_text(NULL, 0);
    CHECK(applied == 0, "NULL input applies nothing");
}

static void test_llm_net_off_variants(void) {
    reset_mock();
    mock_load_config_text("llm_net=off\n", 12);
    CHECK(g_mock.llm_net_calls == 1 && g_mock.llm_net_enabled == 0, "llm_net=off disables service");

    reset_mock();
    const char *text = "llm_net=disabled\n";
    mock_load_config_text(text, (uint32_t)strlen(text));
    CHECK(g_mock.llm_net_calls == 1 && g_mock.llm_net_enabled == 0, "llm_net=disabled disables service");

    reset_mock();
    const char *text2 = "llm_net=0\n";
    mock_load_config_text(text2, (uint32_t)strlen(text2));
    CHECK(g_mock.llm_net_calls == 1 && g_mock.llm_net_enabled == 0, "llm_net=0 disables service");
}

int main(void) {
    test_full_static_config();
    test_dhcp_config();
    test_incomplete_static_config_not_applied();
    test_invalid_ip_rejected();
    test_unknown_key_and_missing_equals_ignored();
    test_llm_port_invalid_value_ignored();
    test_whitespace_and_crlf_tolerant();
    test_blank_lines_and_comments_skipped();
    test_empty_input();
    test_llm_net_off_variants();

    printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    if (g_failures) {
        printf("RESULT: FAIL (%d failing check(s))\n", g_failures);
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
