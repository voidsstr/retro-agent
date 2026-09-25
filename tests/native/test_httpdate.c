/* agent/shared/httpdate.h - the Date: parser behind clockfix.c (agent 1.85.0).
 *
 * .243 has a dead CMOS battery and booted into 1980 at every power-on; the
 * agent now sets the clock from the NAS's HTTP Date header when the year is
 * implausible. A misparsed date would set a wrong clock with nothing to say
 * so, so the parser must REFUSE anything that is not exact RFC 1123. */
#include <stdio.h>
#include <string.h>
#include "../../agent/shared/httpdate.h"

static int fails = 0, runs = 0;
#define CHECK(c, msg) do { runs++; if (!(c)) { fails++; printf("  [FAIL] %s\n", msg); } else printf("  [ ok ] %s\n", msg); } while (0)

int main(void)
{
    hd_time_t t;
    const char *nas = "HTTP/1.1 200 OK\r\nServer: nginx\r\nDate: Fri, 25 Sep 2026 03:54:00 GMT\r\nContent-Type: text/html\r\n\r\n";
    memset(&t, 0, sizeof(t));
    CHECK(hd_find_date(nas, &t) && t.year == 2026 && t.month == 9 && t.day == 25
          && t.hour == 3 && t.minute == 54 && t.second == 0, "the NAS's real header parses");
    CHECK(hd_find_date("HTTP/1.0 200 OK\r\ndate: Thu, 01 Jan 2026 23:59:59 gmt\r\n\r\n", &t)
          && t.month == 1 && t.hour == 23, "header name and GMT are case-insensitive");
    CHECK(hd_find_date("HTTP/1.0 200 OK\nDate: Mon, 07 Dec 2026 10:00:00 GMT\n\n", &t)
          && t.month == 12, "bare-LF responses parse");
    CHECK(!hd_find_date("HTTP/1.1 200 OK\r\nServer: x\r\n\r\nDate: Fri, 25 Sep 2026 03:54:00 GMT\r\n", &t),
          "a Date after the end of the headers is body text, not a header");
    CHECK(!hd_find_date("HTTP/1.1 200 OK\r\nX-Date: Fri, 25 Sep 2026 03:54:00 GMT\r\n\r\n", &t),
          "X-Date is not Date");
    CHECK(!hd_parse_rfc1123("Friday, 25-Sep-26 03:54:00 GMT", &t), "RFC 850 form is refused, not guessed");
    CHECK(!hd_parse_rfc1123("Fri Sep 25 03:54:00 2026", &t), "asctime form is refused");
    CHECK(!hd_parse_rfc1123("Fri, 25 Sep 2026 03:54:00 +0000", &t), "a non-GMT zone is refused");
    CHECK(!hd_parse_rfc1123("Fri, 25 Sop 2026 03:54:00 GMT", &t), "an unknown month is refused");
    CHECK(!hd_parse_rfc1123("Fri, 25 Sep 2026 24:00:00 GMT", &t), "hour 24 is refused");
    CHECK(!hd_parse_rfc1123("Fri, 2 Sep 2026 03:54:00 GMT", &t), "a one-digit day is refused (RFC 1123 is fixed width)");
    CHECK(!hd_find_date("", &t), "an empty response has no date");
    printf("-- httpdate (clockfix's Date parser, agent 1.85.0): %d/%d tests passed --\n", runs - fails, runs);
    return fails ? 1 : 0;
}
