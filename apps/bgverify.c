/*
 * bgverify: checks the event log's hash chain and, optionally, writes a
 * one-page HTML shift report from it.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bg/eventlog.h"
#include "bg/service.h"

/* Copy the value of key=... (quoted or bare) from a payload. */
static int kv(const char *payload, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    const char *p = payload;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == payload || p[-1] == ' ') && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t n = 0;
            if (*v == '"') {
                v++;
                while (v[n] && v[n] != '"') n++;
            } else {
                while (v[n] && v[n] != ' ') n++;
            }
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return 0;
        }
        p += klen;
    }
    out[0] = '\0';
    return -1;
}

static void html_text(FILE *o, const char *s) {
    for (; *s; s++) {
        if (*s == '<') fputs("&lt;", o);
        else if (*s == '>') fputs("&gt;", o);
        else if (*s == '&') fputs("&amp;", o);
        else if (*s == '"') fputs("&quot;", o);
        else fputc(*s, o);
    }
}

typedef struct {
    char id[12], bed[24], code[32], prio[12], device[32];
    uint64_t raised, acked, resolved;
    int escalations, refusals, repeats, incident;
    char top_tier[24], acked_by[64], note[200];
} summary;

static int write_report(const char *log_path, const char *out_path, const bg_verify_result *vr,
                        int ok) {
    FILE *in = fopen(log_path, "r"), *o;
    char line[BG_LOG_LINE_MAX + 2];
    static summary sums[512];
    int ns = 0;
    if (in == NULL) return -1;
    o = fopen(out_path, "w");
    if (o == NULL) {
        fclose(in);
        return -1;
    }
    /* Pass 1: per-alarm summaries. */
    while (fgets(line, sizeof line, in)) {
        bg_log_record r;
        char id[12];
        summary *s = NULL;
        if (bg_log_parse(line, &r) != 0) break;
        if (kv(r.payload, "alarm", id, sizeof id) != 0) continue;
        for (int i = 0; i < ns; i++)
            if (!strcmp(sums[i].id, id)) s = &sums[i];
        if (s == NULL) {
            if (ns >= 512) continue;
            s = &sums[ns++];
            memset(s, 0, sizeof *s);
            snprintf(s->id, sizeof s->id, "%s", id);
        }
        if (!strcmp(r.type, "RAISE")) {
            s->raised = r.t_ms;
            kv(r.payload, "bed", s->bed, sizeof s->bed);
            kv(r.payload, "code", s->code, sizeof s->code);
            kv(r.payload, "prio", s->prio, sizeof s->prio);
            kv(r.payload, "device", s->device, sizeof s->device);
        } else if (!strcmp(r.type, "UPGRADE")) {
            kv(r.payload, "prio", s->prio, sizeof s->prio);
        } else if (!strcmp(r.type, "ESCALATE")) {
            s->escalations++;
            kv(r.payload, "tier", s->top_tier, sizeof s->top_tier);
        } else if (!strcmp(r.type, "REPEAT")) {
            s->repeats++;
        } else if (!strcmp(r.type, "ACK")) {
            if (!s->acked) s->acked = r.t_ms;
            kv(r.payload, "by", s->acked_by, sizeof s->acked_by);
            kv(r.payload, "note", s->note, sizeof s->note);
        } else if (!strcmp(r.type, "ACK_REFUSED")) {
            s->refusals++;
        } else if (!strcmp(r.type, "RESOLVE")) {
            s->resolved = r.t_ms;
        } else if (!strcmp(r.type, "INCIDENT")) {
            s->incident = 1;
        }
    }

    fprintf(o, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
               "<title>Bedside Guardian shift report</title><style>"
               "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:32px;color:#1d2433;background:#f7f8fa}"
               "h1{font-size:22px;margin:0 0 4px}p.sub{color:#5b6475;margin:0 0 20px}"
               ".badge{display:inline-block;padding:6px 12px;border-radius:6px;font-weight:600;margin-bottom:18px}"
               ".ok{background:#e3f5e9;color:#17663a}.bad{background:#fde8e8;color:#9b1c1c}"
               "table{border-collapse:collapse;width:100%%;background:#fff;margin-bottom:28px;font-size:13px}"
               "th,td{border-bottom:1px solid #e4e7ec;padding:7px 9px;text-align:left;vertical-align:top}"
               "th{background:#eef1f5;font-weight:600}td.mono{font-family:ui-monospace,Menlo,monospace;font-size:12px}"
               ".t-ESCALATE,.t-REPAGE{color:#b45309;font-weight:600}.t-INCIDENT{color:#9b1c1c;font-weight:700}"
               ".t-ACK{color:#17663a;font-weight:600}.t-ACK_REFUSED{color:#9b1c1c}.t-RESOLVE{color:#1d4ed8}"
               "h2{font-size:16px;margin:8px 0 10px}.foot{color:#5b6475;font-size:12px}"
               "</style></head><body>\n");
    fprintf(o, "<h1>Bedside Guardian: shift report</h1>"
               "<p class=\"sub\">Synthetic data only. Generated from the event log by bgverify.</p>\n");
    if (ok)
        fprintf(o, "<div class=\"badge ok\">Event log intact: %" PRIu64
                   " events, chain head %.16s&hellip;</div>\n",
                vr->events, vr->head);
    else {
        fprintf(o, "<div class=\"badge bad\">Event log BROKEN at line %" PRIu64 ": ",
                vr->first_bad_line);
        html_text(o, vr->reason);
        fprintf(o, "</div>\n");
    }

    fprintf(o, "<h2>Alarms</h2><table><tr><th>#</th><th>Bed</th><th>Condition</th><th>Priority</th>"
               "<th>Repeats folded</th><th>Escalations</th><th>Time to acknowledge</th>"
               "<th>Acknowledged by</th><th>Their note (verbatim)</th><th>Refused acks</th>"
               "<th>Outcome</th></tr>\n");
    for (int i = 0; i < ns; i++) {
        summary *s = &sums[i];
        fprintf(o, "<tr><td>%s</td><td>", s->id);
        html_text(o, s->bed);
        fprintf(o, "</td><td>");
        html_text(o, s->code);
        fprintf(o, "<br><span class=\"foot\">");
        html_text(o, s->device);
        fprintf(o, "</span></td><td>%s</td><td>%d</td><td>%d%s%s</td><td>", s->prio, s->repeats,
                s->escalations, s->escalations ? " &rarr; " : "", s->top_tier);
        if (s->acked) fprintf(o, "%.1f s", (double)(s->acked - s->raised) / 1000.0);
        else fprintf(o, "not acknowledged");
        fprintf(o, "</td><td>");
        html_text(o, s->acked_by);
        fprintf(o, "</td><td>");
        html_text(o, s->note);
        fprintf(o, "</td><td>%d</td><td>%s%s</td></tr>\n", s->refusals,
                s->resolved ? "cleared by device" : "still open",
                s->incident ? "; <b>incident filed</b>" : "");
    }
    fprintf(o, "</table>\n<h2>Event log</h2><table><tr><th>Seq</th><th>t (s)</th><th>Event</th>"
               "<th>Details</th><th>Hash</th></tr>\n");
    rewind(in);
    while (fgets(line, sizeof line, in)) {
        bg_log_record r;
        if (bg_log_parse(line, &r) != 0) break;
        fprintf(o, "<tr><td>%" PRIu64 "</td><td>%.1f</td><td class=\"t-%s\">%s</td><td class=\"mono\">",
                r.seq, (double)r.t_ms / 1000.0, r.type, r.type);
        html_text(o, r.payload);
        fprintf(o, "</td><td class=\"mono\">%.12s</td></tr>\n", r.hash);
    }
    fprintf(o, "</table><p class=\"foot\">Each row's hash covers its sequence number, time, event, "
               "details and the previous row's hash. Not a medical device; demo escalation "
               "timings are not clinical guidance.</p></body></html>\n");
    fclose(o);
    fclose(in);
    return 0;
}

int main(int argc, char **argv) {
    const char *path = NULL, *html = NULL;
    bg_verify_result vr;
    FILE *fp;
    int broken;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--html") && i + 1 < argc) html = argv[++i];
        else if (argv[i][0] != '-') path = argv[i];
        else {
            fprintf(stderr, "usage: bgverify LOG [--html REPORT.html]\n");
            return 2;
        }
    }
    if (path == NULL) {
        fprintf(stderr, "usage: bgverify LOG [--html REPORT.html]\n");
        return 2;
    }
    fp = fopen(path, "r");
    if (fp == NULL) {
        perror(path);
        return 2;
    }
    broken = bg_log_verify(fp, &vr);
    fclose(fp);
    if (broken) printf("BROKEN: %s\n", vr.reason);
    else printf("OK: %" PRIu64 " events, chain head %s\n", vr.events, vr.head);
    if (html && write_report(path, html, &vr, !broken) != 0) {
        fprintf(stderr, "bgverify: could not write %s\n", html);
        return 2;
    }
    return broken ? 1 : 0;
}
