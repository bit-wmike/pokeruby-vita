/*
 * The cartridge's real-time clock (Seiko S-3511, src/libs/siirtc.c) for the
 * host / PS Vita port: it reads the system clock. Setting the clock is
 * ignored - the game keeps its own offset in the save (the same as with a
 * working cartridge battery).
 */
#include <time.h>

#include "global.h"
#include "siirtc.h"

static u8 Bcd(int v)
{
    return (u8)(((v / 10) << 4) | (v % 10));
}

static void ReadClock(struct SiiRtcInfo *rtc, int date)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (date) {
        rtc->year = Bcd(tm.tm_year % 100);
        rtc->month = Bcd(tm.tm_mon + 1);
        rtc->day = Bcd(tm.tm_mday);
        rtc->dayOfWeek = Bcd(tm.tm_wday);
    }
    rtc->hour = Bcd(tm.tm_hour);
    rtc->minute = Bcd(tm.tm_min);
    rtc->second = Bcd(tm.tm_sec);
}

void SiiRtcUnprotect(void) {}
void SiiRtcProtect(void) {}
u8 SiiRtcProbe(void) { return 1; }
bool8 SiiRtcReset(void) { return TRUE; }

bool8 SiiRtcGetStatus(struct SiiRtcInfo *rtc)
{
    rtc->status = SIIRTCINFO_24HOUR;
    return TRUE;
}

bool8 SiiRtcSetStatus(struct SiiRtcInfo *rtc) { (void)rtc; return TRUE; }

bool8 SiiRtcGetDateTime(struct SiiRtcInfo *rtc)
{
    ReadClock(rtc, 1);
    return TRUE;
}

bool8 SiiRtcSetDateTime(struct SiiRtcInfo *rtc) { (void)rtc; return TRUE; }

bool8 SiiRtcGetTime(struct SiiRtcInfo *rtc)
{
    ReadClock(rtc, 0);
    return TRUE;
}

bool8 SiiRtcSetTime(struct SiiRtcInfo *rtc) { (void)rtc; return TRUE; }
bool8 SiiRtcSetAlarm(struct SiiRtcInfo *rtc) { (void)rtc; return TRUE; }
