/* Unit and scenario tests for QNX Bedside Guardian. Synthetic data only. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bg/alarm.h"
#include "bg/eventlog.h"
#include "bg/proto.h"
#include "bg/scenario.h"
#include "bg/service.h"
#include "bg/sha256.h"
#include "harness.h"

#ifndef BG_SOURCE_DIR
#define BG_SOURCE_DIR "."
#endif

/* Where scenarios/ lives. Overridable at run time (e.g. on a QNX target). */
static const char *src_dir(void) {
    const char *e = getenv("BG_SOURCE_DIR");
    return (e && *e) ? e : BG_SOURCE_DIR;
}

/* ---------- helpers ---------- */

static bg_log g_log;
static bg_engine g_eng;

static void fresh(void) {
    bg_log_close(&g_log);
    if (bg_log_open(&g_log, NULL, 0) != 0) {
        fprintf(stderr, "cannot open temp log\n");
        exit(2);
    }
    bg_engine_init(&g_eng, &g_log);
}

static int log_count(const char *type) {
    char line[BG_LOG_LINE_MAX + 2];
    int n = 0;
    long pos = ftell(g_log.fp);
    rewind(g_log.fp);
    while (fgets(line, sizeof line, g_log.fp)) {
        bg_log_record r;
        if (bg_log_parse(line, &r) == 0 && strcmp(r.type, type) == 0) n++;
    }
    fseek(g_log.fp, pos, SEEK_SET);
    return n;
}

static bg_status ack_current(uint32_t id, uint64_t now, const char *who, bg_role role,
                             const char *note, uint64_t req) {
    const bg_alarm *a = bg_find(&g_eng, id);
    char h[BG_HASH16_LEN + 1];
    snprintf(h, sizeof h, "%.16s", a ? a->vhash : "");
    return bg_ack(&g_eng, now, id, a ? a->version : 0, h, who, role, note, req);
}

static void sha_hex(const char *msg, size_t len, char *out) {
    uint8_t d[32];
    bg_sha256(msg, len, d);
    bg_hex(d, 32, out);
}

/* ---------- SHA-256 ---------- */

static void test_sha256_vectors(void) {
    char h[65];
    sha_hex("", 0, h);
    CHECK_STR(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    sha_hex("abc", 3, h);
    CHECK_STR(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    {
        const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        sha_hex(m, strlen(m), h);
        CHECK_STR(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    }
}

static void test_sha256_million_a_incremental(void) {
    bg_sha256_ctx c;
    char chunk[1000];
    uint8_t d[32];
    char h[65];
    memset(chunk, 'a', sizeof chunk);
    bg_sha256_init(&c);
    for (int i = 0; i < 1000; i++) bg_sha256_update(&c, chunk, sizeof chunk);
    bg_sha256_final(&c, d);
    bg_hex(d, 32, h);
    CHECK_STR(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void test_sha256_boundary_lengths(void) {
    /* 55, 56 and 64 bytes exercise the padding edge cases. */
    char buf[64];
    char h[65];
    memset(buf, 'x', sizeof buf);
    (void)h;
    /* Cross-check incremental vs one-shot at every length up to 64. */
    for (size_t n = 0; n <= 64; n++) {
        bg_sha256_ctx c;
        uint8_t d1[32], d2[32];
        bg_sha256(buf, n, d1);
        bg_sha256_init(&c);
        for (size_t i = 0; i < n; i++) bg_sha256_update(&c, buf + i, 1);
        bg_sha256_final(&c, d2);
        CHECK(memcmp(d1, d2, 32) == 0);
    }
}

/* ---------- event log ---------- */

static void test_log_chain_verifies(void) {
    bg_log l;
    bg_verify_result vr;
    CHECK_EQ_INT(bg_log_open(&l, NULL, 0), 0);
    CHECK_EQ_INT(bg_log_append(&l, 1, "A", "x=1"), 0);
    CHECK_EQ_INT(bg_log_append(&l, 2, "B", "x=2"), 0);
    CHECK_EQ_INT(bg_log_append(&l, 3, "C", "x=3"), 0);
    rewind(l.fp);
    CHECK_EQ_INT(bg_log_verify(l.fp, &vr), 0);
    CHECK_EQ_INT(vr.events, 3);
    CHECK_STR(vr.head, l.head);
    bg_log_close(&l);
}

static FILE *log_with_lines(char lines[][BG_LOG_LINE_MAX], int *n) {
    bg_log l;
    FILE *out = tmpfile();
    bg_log_open(&l, NULL, 0);
    bg_log_append(&l, 10, "RAISE", "alarm=1 bed=12");
    bg_log_append(&l, 20, "ESCALATE", "alarm=1 tier=CHARGE_RN");
    bg_log_append(&l, 30, "ACK", "alarm=1 by=\"R. Okafor\"");
    bg_log_append(&l, 40, "RESOLVE", "alarm=1");
    rewind(l.fp);
    *n = 0;
    while (*n < 8 && fgets(lines[*n], BG_LOG_LINE_MAX, l.fp)) (*n)++;
    bg_log_close(&l);
    return out;
}

static int verify_lines(char lines[][BG_LOG_LINE_MAX], const int *order, int n,
                        bg_verify_result *vr) {
    FILE *fp = tmpfile();
    int rc;
    for (int i = 0; i < n; i++) fputs(lines[order[i]], fp);
    rewind(fp);
    rc = bg_log_verify(fp, vr);
    fclose(fp);
    return rc;
}

static void test_log_detects_edit_delete_reorder(void) {
    char lines[8][BG_LOG_LINE_MAX];
    int n;
    bg_verify_result vr;
    FILE *unused = log_with_lines(lines, &n);
    int all[] = {0, 1, 2, 3}, del[] = {0, 2, 3}, swap[] = {0, 2, 1, 3}, trunc[] = {0, 1, 2};
    fclose(unused);
    CHECK_EQ_INT(n, 4);
    CHECK_EQ_INT(verify_lines(lines, all, 4, &vr), 0);

    /* Edit: change who acknowledged. */
    {
        char *p = strstr(lines[2], "Okafor");
        CHECK(p != NULL);
        if (p) memcpy(p, "Okafoz", 6);
        CHECK_EQ_INT(verify_lines(lines, all, 4, &vr), 1);
        CHECK_EQ_INT(vr.first_bad_line, 3);
        CHECK(strstr(vr.reason, "changed") != NULL);
        if (p) memcpy(p, "Okafor", 6);
    }
    CHECK_EQ_INT(verify_lines(lines, del, 3, &vr), 1);
    CHECK_EQ_INT(vr.first_bad_line, 2);
    CHECK_EQ_INT(verify_lines(lines, swap, 4, &vr), 1);
    CHECK_EQ_INT(vr.first_bad_line, 2);
    /* Truncating the tail still verifies; the head hash is what proves it. */
    CHECK_EQ_INT(verify_lines(lines, trunc, 3, &vr), 0);
    CHECK_EQ_INT(vr.events, 3);
}

static void test_log_reopen_continues_chain(void) {
    char path[] = "/tmp/bg-test-log-XXXXXX";
    int fd = mkstemp(path);
    bg_log l;
    bg_verify_result vr;
    FILE *fp;
    CHECK(fd >= 0);
    close(fd);
    CHECK_EQ_INT(bg_log_open(&l, path, 1), 0);
    bg_log_append(&l, 1, "A", "first run");
    bg_log_close(&l);
    CHECK_EQ_INT(bg_log_open(&l, path, 0), 0);
    CHECK_EQ_INT(l.seq, 1);
    bg_log_append(&l, 2, "B", "second run");
    bg_log_close(&l);
    fp = fopen(path, "r");
    CHECK_EQ_INT(bg_log_verify(fp, &vr), 0);
    CHECK_EQ_INT(vr.events, 2);
    fclose(fp);
    /* Damage it: reopening must refuse rather than extend a broken chain. */
    fp = fopen(path, "a");
    fputs("3|3|X|forged|0000000000000000000000000000000000000000000000000000000000000000|"
          "0000000000000000000000000000000000000000000000000000000000000000\n", fp);
    fclose(fp);
    CHECK_EQ_INT(bg_log_open(&l, path, 0), -1);
    remove(path);
}

static void test_log_sanitizes_separators(void) {
    bg_log l;
    bg_log_record r;
    char line[BG_LOG_LINE_MAX + 2];
    bg_verify_result vr;
    bg_log_open(&l, NULL, 0);
    bg_log_append(&l, 5, "NO|TE", "said \"a|b\"\nnext line");
    rewind(l.fp);
    CHECK(fgets(line, sizeof line, l.fp) != NULL);
    CHECK_EQ_INT(bg_log_parse(line, &r), 0);
    CHECK_STR(r.type, "NO/TE");
    CHECK_STR(r.payload, "said \"a/b\" next line");
    rewind(l.fp);
    CHECK_EQ_INT(bg_log_verify(l.fp, &vr), 0);
    bg_log_close(&l);
}

static void test_log_parse_rejects_malformed(void) {
    bg_log_record r;
    CHECK(bg_log_parse("", &r) != 0);
    CHECK(bg_log_parse("1|2|A|p|short|short", &r) != 0);
    CHECK(bg_log_parse("x|2|A|p|0000000000000000000000000000000000000000000000000000000000000000|"
                       "0000000000000000000000000000000000000000000000000000000000000000",
                       &r) != 0);
    CHECK(bg_log_parse("1|2|A|p|0000000000000000000000000000000000000000000000000000000000000000|"
                       "0000000000000000000000000000000000000000000000000000000000000000|extra",
                       &r) != 0);
}

/* ---------- engine: raise, repeat, upgrade ---------- */

static void test_raise_creates_active_bedside_alarm(void) {
    uint32_t id = 0;
    const bg_alarm *a;
    fresh();
    CHECK_EQ_INT(bg_raise(&g_eng, 0, "12", "pulse-ox-12", "SPO2_LOW", BG_PRIO_HIGH, 86, &id), BG_OK);
    a = bg_find(&g_eng, id);
    CHECK(a != NULL);
    CHECK_EQ_INT(a->state, BG_ST_ACTIVE);
    CHECK_EQ_INT(a->tier, BG_ROLE_BEDSIDE_RN);
    CHECK_EQ_INT(a->version, 1);
    CHECK_EQ_INT(a->deadline_ms, 15000);
    CHECK_EQ_INT(strlen(a->vhash), 64);
    CHECK_EQ_INT(bg_open_count(&g_eng), 1);
    CHECK_EQ_INT(log_count("RAISE"), 1);
}

static void test_raise_rejects_bad_input(void) {
    fresh();
    CHECK_EQ_INT(bg_raise(&g_eng, 0, "", "d", "C", BG_PRIO_HIGH, 0, NULL), BG_ERR_INVALID);
    CHECK_EQ_INT(bg_raise(&g_eng, 0, "1", "d", "C", (bg_prio)9, 0, NULL), BG_ERR_INVALID);
    CHECK_EQ_INT(bg_raise(&g_eng, 0, "1", NULL, "C", BG_PRIO_HIGH, 0, NULL), BG_ERR_INVALID);
    CHECK_EQ_INT(bg_open_count(&g_eng), 0);
}

static void test_repeats_fold_without_new_version(void) {
    uint32_t a1, a2;
    fresh();
    bg_raise(&g_eng, 0, "12", "pulse-ox-12", "SPO2_LOW", BG_PRIO_HIGH, 86, &a1);
    bg_raise(&g_eng, 1000, "12", "pulse-ox-12", "SPO2_LOW", BG_PRIO_HIGH, 85, &a2);
    bg_raise(&g_eng, 2000, "12", "pulse-ox-12", "SPO2_LOW", BG_PRIO_HIGH, 84, &a2);
    CHECK_EQ_INT(a1, a2);
    CHECK_EQ_INT(bg_open_count(&g_eng), 1);
    CHECK_EQ_INT(bg_find(&g_eng, a1)->repeats, 2);
    CHECK_EQ_INT(bg_find(&g_eng, a1)->value, 84);
    CHECK_EQ_INT(bg_find(&g_eng, a1)->version, 1); /* nurse's view is still current */
    CHECK_EQ_INT(bg_find(&g_eng, a1)->deadline_ms, 15000); /* repeats don't reset the clock */
    CHECK_EQ_INT(log_count("REPEAT"), 2);
}

static void test_different_device_is_a_different_alarm(void) {
    uint32_t a1, a2;
    fresh();
    bg_raise(&g_eng, 0, "12", "pulse-ox-12", "SPO2_LOW", BG_PRIO_HIGH, 86, &a1);
    bg_raise(&g_eng, 0, "12", "ecg-12", "SPO2_LOW", BG_PRIO_HIGH, 86, &a2);
    CHECK(a1 != a2);
    CHECK_EQ_INT(bg_open_count(&g_eng), 2);
}

static void test_upgrade_bumps_version_and_restarts_cycle(void) {
    uint32_t id;
    fresh();
    bg_raise(&g_eng, 0, "7", "pump-7a", "OCCLUSION", BG_PRIO_MEDIUM, 1, &id);
    CHECK_EQ_INT(bg_find(&g_eng, id)->deadline_ms, 60000);
    bg_raise(&g_eng, 10000, "7", "pump-7a", "OCCLUSION", BG_PRIO_HIGH, 2, &id);
    CHECK_EQ_INT(bg_find(&g_eng, id)->prio, BG_PRIO_HIGH);
    CHECK_EQ_INT(bg_find(&g_eng, id)->version, 2);
    CHECK_EQ_INT(bg_find(&g_eng, id)->deadline_ms, 25000);
    CHECK_EQ_INT(log_count("UPGRADE"), 1);
}

static void test_lower_priority_repeat_does_not_downgrade(void) {
    uint32_t id;
    fresh();
    bg_raise(&g_eng, 0, "7", "pump-7a", "OCCLUSION", BG_PRIO_HIGH, 1, &id);
    bg_raise(&g_eng, 100, "7", "pump-7a", "OCCLUSION", BG_PRIO_LOW, 1, &id);
    CHECK_EQ_INT(bg_find(&g_eng, id)->prio, BG_PRIO_HIGH);
    CHECK_EQ_INT(bg_find(&g_eng, id)->version, 1);
}

/* ---------- engine: escalation timing ---------- */

static void test_high_escalates_on_exact_deadlines(void) {
    uint32_t id;
    const bg_alarm *a;
    fresh();
    bg_raise(&g_eng, 0, "4", "vent-4", "VENT_DISCONNECT", BG_PRIO_HIGH, 0, &id);
    a = bg_find(&g_eng, id);
    CHECK_EQ_INT(bg_tick(&g_eng, 14999), 0);
    CHECK_EQ_INT(a->tier, BG_ROLE_BEDSIDE_RN);
    CHECK_EQ_INT(bg_tick(&g_eng, 15000), 1);
    CHECK_EQ_INT(a->tier, BG_ROLE_CHARGE_RN);
    CHECK_EQ_INT(a->state, BG_ST_ESCALATED);
    CHECK_EQ_INT(a->deadline_ms, 30000);
    CHECK_EQ_INT(bg_tick(&g_eng, 29999), 0);
    CHECK_EQ_INT(bg_tick(&g_eng, 30000), 1);
    CHECK_EQ_INT(a->tier, BG_ROLE_RAPID_RESPONSE);
    CHECK_EQ_INT(a->incident_filed, 1);
    CHECK_EQ_INT(g_eng.incidents, 1);
    CHECK_EQ_INT(log_count("INCIDENT"), 1);
    /* Rapid response is re-paged every 30 s; no second incident. */
    CHECK_EQ_INT(bg_tick(&g_eng, 60000), 1);
    CHECK_EQ_INT(log_count("REPAGE"), 1);
    CHECK_EQ_INT(g_eng.incidents, 1);
}

static void test_medium_stops_at_charge_nurse(void) {
    uint32_t id;
    fresh();
    bg_raise(&g_eng, 0, "7", "pump-7a", "OCCLUSION", BG_PRIO_MEDIUM, 1, &id);
    bg_tick(&g_eng, 60000);
    CHECK_EQ_INT(bg_find(&g_eng, id)->tier, BG_ROLE_CHARGE_RN);
    CHECK_EQ_INT(bg_next_deadline(&g_eng), UINT64_MAX);
    bg_tick(&g_eng, 10000000);
    CHECK_EQ_INT(bg_find(&g_eng, id)->tier, BG_ROLE_CHARGE_RN);
    CHECK_EQ_INT(g_eng.incidents, 0);
}

static void test_low_never_escalates(void) {
    uint32_t id;
    fresh();
    bg_raise(&g_eng, 0, "7", "ecg-7", "LEAD_OFF", BG_PRIO_LOW, 0, &id);
    CHECK_EQ_INT(bg_next_deadline(&g_eng), UINT64_MAX);
    CHECK_EQ_INT(bg_tick(&g_eng, 100000000), 0);
    CHECK_EQ_INT(bg_find(&g_eng, id)->tier, BG_ROLE_BEDSIDE_RN);
}

static void test_scale_shrinks_windows(void) {
    uint32_t id;
    fresh();
    bg_engine_set_scale(&g_eng, 10);
    bg_raise(&g_eng, 0, "4", "vent-4", "VENT_DISCONNECT", BG_PRIO_HIGH, 0, &id);
    CHECK_EQ_INT(bg_next_deadline(&g_eng), 1500);
    bg_engine_set_scale(&g_eng, 0); /* clamps to 1%, never zero windows */
    CHECK_EQ_INT(g_eng.window_scale_pct, 1);
}

static void test_next_deadline_is_earliest(void) {
    fresh();
    bg_raise(&g_eng, 0, "1", "a", "X", BG_PRIO_MEDIUM, 0, NULL);
    bg_raise(&g_eng, 5000, "2", "b", "Y", BG_PRIO_HIGH, 0, NULL);
    CHECK_EQ_INT(bg_next_deadline(&g_eng), 20000);
}

/* ---------- engine: acknowledgement gate ---------- */

static void test_ack_requires_name(void) {
    uint32_t id;
    fresh();
    bg_raise(&g_eng, 0, "2", "pump-2", "AIR_IN_LINE", BG_PRIO_HIGH, 1, &id);
    CHECK_EQ_INT(ack_current(id, 100, "", BG_ROLE_BEDSIDE_RN, "", 1), BG_ERR_NO_NAME);
    CHECK_EQ_INT(ack_current(id, 100, NULL, BG_ROLE_BEDSIDE_RN, "", 2), BG_ERR_NO_NAME);
    CHECK_EQ_INT(bg_find(&g_eng, id)->state, BG_ST_ACTIVE);
    CHECK_EQ_INT(log_count("ACK_REFUSED"), 2);
}

static void test_ack_refuses_role_below_tier(void) {
    uint32_t id;
    fresh();
    bg_raise(&g_eng, 0, "12", "pulse-ox-12", "SPO2_LOW", BG_PRIO_HIGH, 86, &id);
    bg_tick(&g_eng, 15000); /* now the charge nurse's */
    CHECK_EQ_INT(ack_current(id, 16000, "J. Mendez", BG_ROLE_BEDSIDE_RN, "", 1), BG_ERR_ROLE);
    CHECK_EQ_INT(ack_current(id, 16000, "R. Okafor", BG_ROLE_CHARGE_RN, "here", 2), BG_OK);
}

static void test_higher_role_may_ack_lower_tier(void) {
    uint32_t id;
    fresh();
    bg_raise(&g_eng, 0, "12", "pulse-ox-12", "SPO2_LOW", BG_PRIO_HIGH, 86, &id);
    CHECK_EQ_INT(ack_current(id, 10, "T. Hale", BG_ROLE_RAPID_RESPONSE, "passing by", 1), BG_OK);
}

static void test_ack_refuses_stale_version_and_wrong_hash(void) {
    uint32_t id;
    const bg_alarm *a;
    char h[17];
    fresh();
    bg_raise(&g_eng, 0, "12", "pulse-ox-12", "SPO2_LOW", BG_PRIO_HIGH, 86, &id);
    a = bg_find(&g_eng, id);
    snprintf(h, sizeof h, "%.16s", a->vhash);
    bg_tick(&g_eng, 15000); /* escalation changes what is on screen */
    CHECK_EQ_INT(bg_ack(&g_eng, 15100, id, 1, h, "R. Okafor", BG_ROLE_CHARGE_RN, "", 1), BG_ERR_STALE);
    /* Right version number, wrong hash (someone typed it from another screen). */
    CHECK_EQ_INT(bg_ack(&g_eng, 15100, id, a->version, "0000000000000000", "R. Okafor",
                        BG_ROLE_CHARGE_RN, "", 2),
                 BG_ERR_STALE);
    CHECK_EQ_INT(bg_ack(&g_eng, 15100, id, a->version, "abc", "R. Okafor", BG_ROLE_CHARGE_RN, "", 3),
                 BG_ERR_STALE);
    CHECK_EQ_INT(bg_ack(&g_eng, 15100, id, a->version, NULL, "R. Okafor", BG_ROLE_CHARGE_RN, "", 4),
                 BG_ERR_STALE);
    CHECK_EQ_INT(a->state, BG_ST_ESCALATED);
}

static void test_ack_records_words_verbatim(void) {
    uint32_t id;
    const char *note = "Probe slipped off a cold finger; moved it to the earlobe, reading 95.";
    fresh();
    bg_raise(&g_eng, 0, "12", "pulse-ox-12", "SPO2_LOW", BG_PRIO_HIGH, 86, &id);
    CHECK_EQ_INT(ack_current(id, 9000, "J. Mendez", BG_ROLE_BEDSIDE_RN, note, 77), BG_OK);
    CHECK_STR(bg_find(&g_eng, id)->ack_note, note);
    CHECK_STR(bg_find(&g_eng, id)->acked_by, "J. Mendez");
    CHECK_EQ_INT(bg_find(&g_eng, id)->acked_ms, 9000);
    CHECK_EQ_INT(bg_find(&g_eng, id)->state, BG_ST_ACKED);
    CHECK_EQ_INT(bg_find(&g_eng, id)->version, 2);
}

static void test_ack_is_idempotent_per_request(void) {
    uint32_t id;
    fresh();
    bg_raise(&g_eng, 0, "12", "pulse-ox-12", "SPO2_LOW", BG_PRIO_HIGH, 86, &id);
    CHECK_EQ_INT(ack_current(id, 100, "J. Mendez", BG_ROLE_BEDSIDE_RN, "a", 42), BG_OK);
    /* Same press again, even quoting the old version: counted once. */
    CHECK_EQ_INT(bg_ack(&g_eng, 150, id, 1, "whatever", "J. Mendez", BG_ROLE_BEDSIDE_RN, "a", 42),
                 BG_OK_DUPLICATE);
    CHECK_EQ_INT(bg_find(&g_eng, id)->version, 2);
    CHECK_EQ_INT(log_count("ACK"), 1);
    CHECK_EQ_INT(log_count("ACK_DUPLICATE"), 1);
    /* A different person pressing too is refused, not silently merged. */
    CHECK_EQ_INT(ack_current(id, 200, "R. Okafor", BG_ROLE_CHARGE_RN, "b", 43), BG_ERR_STATE);
    CHECK_STR(bg_find(&g_eng, id)->acked_by, "J. Mendez");
}

static void test_ack_unknown_alarm_and_bad_role(void) {
    uint32_t id;
    fresh();
    CHECK_EQ_INT(bg_ack(&g_eng, 0, 99, 1, "0000000000000000", "x", BG_ROLE_CHARGE_RN, "", 1),
                 BG_ERR_NOT_FOUND);
    CHECK_EQ_INT(bg_ack(&g_eng, 0, 0, 1, "0000000000000000", "x", BG_ROLE_CHARGE_RN, "", 1),
                 BG_ERR_NOT_FOUND);
    bg_raise(&g_eng, 0, "1", "d", "C", BG_PRIO_HIGH, 0, &id);
    CHECK_EQ_INT(ack_current(id, 1, "x", BG_ROLE_NONE, "", 2), BG_ERR_INVALID);
    CHECK_EQ_INT(ack_current(id, 1, "x", (bg_role)7, "", 3), BG_ERR_INVALID);
}

static void test_acked_alarm_rearms_if_never_cleared(void) {
    uint32_t id;
    const bg_alarm *a;
    fresh();
    bg_raise(&g_eng, 0, "9", "ecg-9", "HR_HIGH", BG_PRIO_HIGH, 152, &id);
    ack_current(id, 5000, "J. Mendez", BG_ROLE_BEDSIDE_RN, "aware", 1);
    a = bg_find(&g_eng, id);
    CHECK_EQ_INT(a->deadline_ms, 65000);
    CHECK_EQ_INT(bg_tick(&g_eng, 64999), 0);
    CHECK_EQ_INT(bg_tick(&g_eng, 65000), 1);
    CHECK_EQ_INT(a->state, BG_ST_ACTIVE);
    CHECK_EQ_INT(a->deadline_ms, 80000);
    CHECK_EQ_INT(log_count("REARM"), 1);
    /* The old acknowledgement's request id doesn't silence the new cycle. */
    CHECK_EQ_INT(ack_current(id, 66000, "J. Mendez", BG_ROLE_BEDSIDE_RN, "again", 2), BG_OK);
}

static void test_upgrade_reopens_acked_alarm(void) {
    uint32_t id;
    fresh();
    bg_raise(&g_eng, 0, "7", "pump-7a", "OCCLUSION", BG_PRIO_MEDIUM, 1, &id);
    ack_current(id, 1000, "J. Mendez", BG_ROLE_BEDSIDE_RN, "checking", 1);
    bg_raise(&g_eng, 2000, "7", "pump-7a", "OCCLUSION", BG_PRIO_HIGH, 2, &id);
    CHECK_EQ_INT(bg_find(&g_eng, id)->state, BG_ST_ACTIVE);
    CHECK_EQ_INT(bg_find(&g_eng, id)->deadline_ms, 17000);
}

static void test_clear_resolves_and_blocks_late_ack(void) {
    uint32_t id, cid = 0;
    fresh();
    bg_raise(&g_eng, 0, "2", "pump-2", "AIR_IN_LINE", BG_PRIO_HIGH, 1, &id);
    CHECK_EQ_INT(bg_clear(&g_eng, 9000, "2", "pump-2", "AIR_IN_LINE", &cid), BG_OK);
    CHECK_EQ_INT(cid, id);
    CHECK_EQ_INT(bg_find(&g_eng, id)->state, BG_ST_RESOLVED);
    CHECK_EQ_INT(bg_open_count(&g_eng), 0);
    CHECK_EQ_INT(bg_next_deadline(&g_eng), UINT64_MAX);
    CHECK_EQ_INT(ack_current(id, 9500, "J. Mendez", BG_ROLE_BEDSIDE_RN, "", 1), BG_ERR_STATE);
    CHECK_EQ_INT(bg_clear(&g_eng, 9600, "2", "pump-2", "AIR_IN_LINE", NULL), BG_ERR_NOT_FOUND);
    /* The same condition coming back is a new alarm. */
    {
        uint32_t id2;
        bg_raise(&g_eng, 10000, "2", "pump-2", "AIR_IN_LINE", BG_PRIO_HIGH, 1, &id2);
        CHECK(id2 != id);
    }
}

static void test_capacity_full_is_logged_and_resolved_slots_reused(void) {
    char dev[16];
    fresh();
    for (int i = 0; i < BG_MAX_ALARMS; i++) {
        snprintf(dev, sizeof dev, "d%d", i);
        CHECK_EQ_INT(bg_raise(&g_eng, 0, "1", dev, "C", BG_PRIO_LOW, 0, NULL), BG_OK);
    }
    CHECK_EQ_INT(bg_raise(&g_eng, 1, "1", "extra", "C", BG_PRIO_HIGH, 0, NULL), BG_ERR_FULL);
    CHECK_EQ_INT(log_count("CAPACITY"), 1);
    CHECK_EQ_INT(bg_clear(&g_eng, 2, "1", "d5", "C", NULL), BG_OK);
    CHECK_EQ_INT(bg_raise(&g_eng, 3, "1", "extra", "C", BG_PRIO_HIGH, 0, NULL), BG_OK);
    CHECK_EQ_INT(bg_open_count(&g_eng), BG_MAX_ALARMS);
}

static void test_version_hash_changes_with_state(void) {
    uint32_t id;
    char before[65];
    fresh();
    bg_raise(&g_eng, 0, "1", "d", "C", BG_PRIO_HIGH, 0, &id);
    strcpy(before, bg_find(&g_eng, id)->vhash);
    bg_tick(&g_eng, 15000);
    CHECK(strcmp(before, bg_find(&g_eng, id)->vhash) != 0);
}

static void test_engine_log_chain_intact_after_busy_run(void) {
    bg_verify_result vr;
    fresh();
    for (int i = 0; i < 40; i++) {
        char dev[16];
        snprintf(dev, sizeof dev, "d%d", i % 7);
        bg_raise(&g_eng, (uint64_t)i * 700, "1", dev, "C", (bg_prio)(1 + i % 3), i, NULL);
        bg_tick(&g_eng, (uint64_t)i * 700);
    }
    bg_tick(&g_eng, 1000000);
    rewind(g_log.fp);
    CHECK_EQ_INT(bg_log_verify(g_log.fp, &vr), 0);
    CHECK_EQ_INT(vr.events, g_log.seq);
    fseek(g_log.fp, 0, SEEK_END);
}

static void test_name_parsers(void) {
    bg_prio p;
    bg_role r;
    CHECK_EQ_INT(bg_prio_parse("high", &p), 0);
    CHECK_EQ_INT(p, BG_PRIO_HIGH);
    CHECK(bg_prio_parse("urgent", &p) != 0);
    CHECK_EQ_INT(bg_role_parse("charge_rn", &r), 0);
    CHECK_EQ_INT(r, BG_ROLE_CHARGE_RN);
    CHECK(bg_role_parse("doctor", &r) != 0);
    CHECK_STR(bg_status_name(BG_ERR_STALE), "ERR_STALE");
    CHECK_STR(bg_state_name(BG_ST_ACKED), "ACKED");
}

/* ---------- scenarios: parser and locked eval set ---------- */

static int parse_str(const char *text, bg_scenario *sc) {
    FILE *fp = tmpfile();
    int rc;
    fputs(text, fp);
    rewind(fp);
    rc = bg_scenario_parse(fp, sc);
    fclose(fp);
    return rc;
}

static void test_scenario_parser(void) {
    static bg_scenario sc;
    CHECK_EQ_INT(parse_str("# c\n\n0 RAISE bed=1 device=d code=C prio=LOW value=-3\n"
                           "10 ACK alarm=1 who=\"A B\" role=CHARGE_RN note=\"x y\" request=9\n"
                           "20 END\n",
                           &sc),
                 0);
    CHECK_EQ_INT(sc.n, 3);
    CHECK_EQ_INT(sc.steps[0].prio, BG_PRIO_LOW);
    CHECK_EQ_INT(sc.steps[0].value, -3);
    CHECK_STR(sc.steps[1].who, "A B");
    CHECK_STR(sc.steps[1].note, "x y");
    CHECK_EQ_INT(sc.steps[1].request_id, 9);
    CHECK(parse_str("x RAISE bed=1 device=d code=C\n", &sc) != 0);
    CHECK(parse_str("5 RAISE bed=1 device=d code=C\n1 END\n", &sc) != 0);
    CHECK(strstr(sc.error, "backwards") != NULL);
    CHECK(parse_str("0 BEEP\n", &sc) != 0);
    CHECK(parse_str("0 RAISE bed=1 code=C\n", &sc) != 0);
    CHECK(parse_str("0 ACK who=x\n", &sc) != 0);
    CHECK(parse_str("0 RAISE bed=1 device=d code=C prio=URGENT\n", &sc) != 0);
    CHECK(parse_str("0 RAISE bed=1 device=d code=C colour=red\n", &sc) != 0);
    CHECK(parse_str("0 RAISE bed=12345678901234567890 device=d code=C\n", &sc) != 0);
    CHECK(strstr(sc.error, "too long") != NULL);
}

static int read_file(const char *path, char *buf, size_t cap, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) return -1;
    *len = fread(buf, 1, cap, fp);
    fclose(fp);
    return 0;
}

static void test_scenario_manifest_locked(void) {
    char path[512], line[512];
    static char buf[1 << 16];
    FILE *mf;
    int checked = 0;
    snprintf(path, sizeof path, "%s/scenarios/MANIFEST.sha256", src_dir());
    mf = fopen(path, "r");
    CHECK(mf != NULL);
    if (mf == NULL) return;
    while (fgets(line, sizeof line, mf)) {
        char want[65], name[256], got[65], fpath[600];
        size_t len = 0;
        uint8_t d[32];
        if (sscanf(line, "%64s %255s", want, name) != 2) continue;
        snprintf(fpath, sizeof fpath, "%s/scenarios/%s", src_dir(), name);
        CHECK_EQ_INT(read_file(fpath, buf, sizeof buf, &len), 0);
        bg_sha256(buf, len, d);
        bg_hex(d, 32, got);
        if (strcmp(got, want) != 0) fprintf(stderr, "    changed without updating manifest: %s\n", name);
        CHECK_STR(got, want);
        checked++;
    }
    fclose(mf);
    CHECK_EQ_INT(checked, 10);
}

static void run_golden(const char *name) {
    static bg_scenario sc;
    char spath[512], epath[512];
    static char want[1 << 16], got[1 << 16];
    size_t wlen = 0, glen;
    FILE *out = tmpfile();
    snprintf(spath, sizeof spath, "%s/scenarios/%s.scn", src_dir(), name);
    snprintf(epath, sizeof epath, "%s/scenarios/expected/%s.out", src_dir(), name);
    CHECK_EQ_INT(bg_scenario_load(spath, &sc), 0);
    CHECK_EQ_INT(bg_scenario_run_offline(&sc, out), 0);
    rewind(out);
    glen = fread(got, 1, sizeof got - 1, out);
    got[glen] = '\0';
    fclose(out);
    CHECK_EQ_INT(read_file(epath, want, sizeof want - 1, &wlen), 0);
    want[wlen] = '\0';
    if (strcmp(got, want) != 0) fprintf(stderr, "    golden mismatch for %s\n%s", name, got);
    CHECK_STR(got, want);
}

static void test_golden_01_night_desat(void) { run_golden("01-night-desat"); }
static void test_golden_02_unanswered_vent(void) { run_golden("02-unanswered-vent"); }
static void test_golden_03_alarm_fatigue(void) { run_golden("03-alarm-fatigue"); }
static void test_golden_04_rearm(void) { run_golden("04-rearm"); }
static void test_golden_05_gate_refusals(void) { run_golden("05-gate-refusals"); }

/* ---------- service layer (no IPC) ---------- */

static bg_service g_svc;

static void svc_fresh(const char *inc_path) {
    if (bg_service_init(&g_svc, NULL, inc_path, NULL, 0) != 0) {
        fprintf(stderr, "service init failed\n");
        exit(2);
    }
}

static bg_msg mk(uint16_t type) {
    bg_msg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.magic = BG_MAGIC;
    return m;
}

static void test_service_raise_get_ack_status(void) {
    bg_msg m = mk(BG_MSG_RAISE);
    bg_reply r, g;
    svc_fresh(NULL);
    m.prio = BG_PRIO_HIGH;
    strcpy(m.bed, "12");
    strcpy(m.device, "pulse-ox-12");
    strcpy(m.code, "SPO2_LOW");
    m.value = 86;
    bg_service_handle(&g_svc, 0, 12000, &m, &r);
    CHECK_EQ_INT(r.status, BG_OK);
    CHECK_EQ_INT(r.alarm_id, 1);
    CHECK_EQ_INT(r.version, 1);
    CHECK_EQ_INT(strlen(r.hash16), 16);
    CHECK(strstr(r.text, "SPO2_LOW") != NULL);

    m = mk(BG_MSG_GET);
    m.alarm_id = 1;
    bg_service_handle(&g_svc, 10, 0, &m, &g);
    CHECK_EQ_INT(g.status, BG_OK);
    CHECK_STR(g.hash16, r.hash16);

    m = mk(BG_MSG_ACK);
    m.alarm_id = 1;
    m.version = g.version;
    strcpy(m.hash16, g.hash16);
    strcpy(m.who, "J. Mendez");
    m.role = BG_ROLE_BEDSIDE_RN;
    strcpy(m.note, "on it");
    m.request_id = 5;
    bg_service_handle(&g_svc, 20, 0, &m, &r);
    CHECK_EQ_INT(r.status, BG_OK);
    CHECK_EQ_INT(r.state, BG_ST_ACKED);

    m = mk(BG_MSG_STATUS);
    bg_service_handle(&g_svc, 30, 0, &m, &r);
    CHECK(strstr(r.text, "open=1") != NULL);
    CHECK(strstr(r.text, "acked_by=\"J. Mendez\"") != NULL);
    CHECK(strstr(r.text, "p50=12.0") != NULL);

    m = mk(BG_MSG_GET);
    m.alarm_id = 99;
    bg_service_handle(&g_svc, 40, 0, &m, &r);
    CHECK_EQ_INT(r.status, BG_ERR_NOT_FOUND);
    bg_service_close(&g_svc, 50);
}

static void test_service_rejects_bad_magic_and_type(void) {
    bg_msg m = mk(BG_MSG_RAISE);
    bg_reply r;
    svc_fresh(NULL);
    m.magic = 0xdeadbeef;
    bg_service_handle(&g_svc, 0, 0, &m, &r);
    CHECK_EQ_INT(r.status, BG_ERR_INVALID);
    m = mk(0x77);
    bg_service_handle(&g_svc, 0, 0, &m, &r);
    CHECK_EQ_INT(r.status, BG_ERR_INVALID);
    CHECK_EQ_INT(bg_open_count(&g_svc.eng), 0);
    bg_service_close(&g_svc, 1);
}

static void test_service_survives_unterminated_strings(void) {
    bg_msg m = mk(BG_MSG_RAISE);
    bg_reply r;
    svc_fresh(NULL);
    memset(m.bed, 'B', sizeof m.bed);
    memset(m.device, 'D', sizeof m.device);
    memset(m.code, 'C', sizeof m.code);
    memset(m.who, 'W', sizeof m.who);
    memset(m.note, 'N', sizeof m.note);
    memset(m.hash16, 'H', sizeof m.hash16);
    m.prio = BG_PRIO_LOW;
    bg_service_handle(&g_svc, 0, 0, &m, &r);
    CHECK_EQ_INT(r.status, BG_OK);
    CHECK_EQ_INT(strlen(bg_find(&g_svc.eng, r.alarm_id)->bed), BG_BED_LEN - 1);
    bg_service_close(&g_svc, 1);
}

static void test_service_writes_incident_with_verbatim_note(void) {
    char path[] = "/tmp/bg-test-inc-XXXXXX";
    int fd = mkstemp(path);
    bg_msg m = mk(BG_MSG_RAISE);
    bg_reply r, g;
    static char buf[8192];
    size_t len = 0;
    close(fd);
    svc_fresh(path);
    m.prio = BG_PRIO_HIGH;
    strcpy(m.bed, "4");
    strcpy(m.device, "vent-4");
    strcpy(m.code, "VENT_DISCONNECT");
    bg_service_handle(&g_svc, 0, 0, &m, &r);
    bg_tick(&g_svc.eng, 15000);
    bg_tick(&g_svc.eng, 30000);
    m = mk(BG_MSG_GET);
    m.alarm_id = 1;
    bg_service_handle(&g_svc, 31000, 0, &m, &g);
    m = mk(BG_MSG_ACK);
    m.alarm_id = 1;
    m.version = g.version;
    strcpy(m.hash16, g.hash16);
    strcpy(m.who, "T. Hale");
    m.role = BG_ROLE_RAPID_RESPONSE;
    strcpy(m.note, "Reconnected. Nurse was with a \"fall\" in bed 6.");
    m.request_id = 1;
    bg_service_handle(&g_svc, 32000, 0, &m, &r);
    CHECK_EQ_INT(r.status, BG_OK);
    bg_service_close(&g_svc, 33000);
    CHECK_EQ_INT(read_file(path, buf, sizeof buf - 1, &len), 0);
    buf[len] = '\0';
    CHECK(strstr(buf, "\"event\":\"opened\"") != NULL);
    CHECK(strstr(buf, "\"incident_type\":\"unacknowledged_alarm\"") != NULL);
    CHECK(strstr(buf, "\"synthetic\":true") != NULL);
    CHECK(strstr(buf, "not acknowledged for 30.0 s") != NULL);
    CHECK(strstr(buf, "\"event\":\"acknowledged\"") != NULL);
    CHECK(strstr(buf, "Nurse was with a \\\"fall\\\" in bed 6.") != NULL);
    remove(path);
}

static void test_service_latency_percentiles(void) {
    bg_latency lat;
    svc_fresh(NULL);
    for (int i = 1; i <= 100; i++) {
        bg_msg m = mk(BG_MSG_RAISE);
        bg_reply r;
        m.prio = BG_PRIO_LOW;
        strcpy(m.bed, "1");
        snprintf(m.device, sizeof m.device, "d%d", i % 10);
        strcpy(m.code, "C");
        bg_service_handle(&g_svc, (uint64_t)i, (uint64_t)i * 1000, &m, &r);
    }
    bg_service_latency(&g_svc, &lat);
    CHECK_EQ_INT(lat.count, 100);
    CHECK_EQ_INT(lat.p50_ns, 50000);
    CHECK_EQ_INT(lat.p99_ns, 99000);
    CHECK_EQ_INT(lat.max_ns, 100000);
    bg_service_close(&g_svc, 200);
}

static void test_service_shutdown_message(void) {
    bg_msg m = mk(BG_MSG_SHUTDOWN);
    bg_reply r;
    svc_fresh(NULL);
    bg_service_handle(&g_svc, 0, 0, &m, &r);
    CHECK_EQ_INT(g_svc.shutdown_requested, 1);
    bg_service_close(&g_svc, 1);
}

static void test_json_escape(void) {
    char out[64];
    bg_json_escape("a\"b\\c\nd\te", out, sizeof out);
    CHECK_STR(out, "a\\\"b\\\\c\\nd\\u0009e");
    bg_json_escape("abcdefghijklmnopqrstuvwxyz", out, 10);
    CHECK(strlen(out) < 10);
}

static void test_proto_type_below_qnx_io_base(void) {
    /* QNX _IO_BASE is 0x100; our message types must sort below it and the
     * type must be the first field so the server can tell them apart. */
    bg_msg m;
    CHECK(BG_MSG_SHUTDOWN < 0x100);
    CHECK((char *)&m.type == (char *)&m);
    CHECK(sizeof(bg_msg) < 1024);
    CHECK(sizeof(bg_reply) < 2048);
}

int main(void) {
    RUN(test_sha256_vectors);
    RUN(test_sha256_million_a_incremental);
    RUN(test_sha256_boundary_lengths);
    RUN(test_log_chain_verifies);
    RUN(test_log_detects_edit_delete_reorder);
    RUN(test_log_reopen_continues_chain);
    RUN(test_log_sanitizes_separators);
    RUN(test_log_parse_rejects_malformed);
    RUN(test_raise_creates_active_bedside_alarm);
    RUN(test_raise_rejects_bad_input);
    RUN(test_repeats_fold_without_new_version);
    RUN(test_different_device_is_a_different_alarm);
    RUN(test_upgrade_bumps_version_and_restarts_cycle);
    RUN(test_lower_priority_repeat_does_not_downgrade);
    RUN(test_high_escalates_on_exact_deadlines);
    RUN(test_medium_stops_at_charge_nurse);
    RUN(test_low_never_escalates);
    RUN(test_scale_shrinks_windows);
    RUN(test_next_deadline_is_earliest);
    RUN(test_ack_requires_name);
    RUN(test_ack_refuses_role_below_tier);
    RUN(test_higher_role_may_ack_lower_tier);
    RUN(test_ack_refuses_stale_version_and_wrong_hash);
    RUN(test_ack_records_words_verbatim);
    RUN(test_ack_is_idempotent_per_request);
    RUN(test_ack_unknown_alarm_and_bad_role);
    RUN(test_acked_alarm_rearms_if_never_cleared);
    RUN(test_upgrade_reopens_acked_alarm);
    RUN(test_clear_resolves_and_blocks_late_ack);
    RUN(test_capacity_full_is_logged_and_resolved_slots_reused);
    RUN(test_version_hash_changes_with_state);
    RUN(test_engine_log_chain_intact_after_busy_run);
    RUN(test_name_parsers);
    RUN(test_scenario_parser);
    RUN(test_scenario_manifest_locked);
    RUN(test_golden_01_night_desat);
    RUN(test_golden_02_unanswered_vent);
    RUN(test_golden_03_alarm_fatigue);
    RUN(test_golden_04_rearm);
    RUN(test_golden_05_gate_refusals);
    RUN(test_service_raise_get_ack_status);
    RUN(test_service_rejects_bad_magic_and_type);
    RUN(test_service_survives_unterminated_strings);
    RUN(test_service_writes_incident_with_verbatim_note);
    RUN(test_service_latency_percentiles);
    RUN(test_service_shutdown_message);
    RUN(test_json_escape);
    RUN(test_proto_type_below_qnx_io_base);
    bg_log_close(&g_log);
    printf("\n%d tests, %d failed\n", g_tests, g_failed);
    return g_failed ? 1 : 0;
}
