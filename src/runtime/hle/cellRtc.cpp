// HLE: cellRtc. Ticks are microseconds since 0001-01-01 00:00:00 (proleptic Gregorian).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "runtime/hle.h"

namespace rt {
namespace {

struct DateTime {
  be16 year, month, day, hour, minute, second;
  be32 microsecond;
};

constexpr u64 kUsPerDay = 86400000000ull;

bool leap(u32 y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
u32 days_in_month(u32 y, u32 m) {
  static const u8 d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && leap(y) ? 29 : d[(m - 1) % 12];
}
// Days from 0001-01-01 to y-m-d.
u64 days_from_civil(u32 y, u32 m, u32 d) {
  u64 yy = y - 1;
  u64 days = yy * 365 + yy / 4 - yy / 100 + yy / 400;
  for (u32 i = 1; i < m; ++i) days += days_in_month(y, i);
  return days + d - 1;
}
void civil_from_days(u64 days, u32* y, u32* m, u32* d) {
  u32 year = 1;
  for (;;) {  // coarse by 400-year cycles then years
    if (days >= 146097) {
      days -= 146097;
      year += 400;
      continue;
    }
    u32 len = leap(year) ? 366 : 365;
    if (days < len) break;
    days -= len;
    ++year;
  }
  u32 month = 1;
  while (days >= days_in_month(year, month)) {
    days -= days_in_month(year, month);
    ++month;
  }
  *y = year;
  *m = month;
  *d = u32(days) + 1;
}
u64 tick_of(const DateTime& t) {
  return days_from_civil(t.year, t.month, t.day) * kUsPerDay +
         ((u64(u16(t.hour)) * 60 + u16(t.minute)) * 60 + u16(t.second)) * 1000000ull + u32(t.microsecond);
}
void set_from_tick(DateTime& t, u64 tick) {
  u32 y, m, d;
  civil_from_days(tick / kUsPerDay, &y, &m, &d);
  u64 rem = tick % kUsPerDay;
  t.year = u16(y);
  t.month = u16(m);
  t.day = u16(d);
  t.hour = u16(rem / 3600000000ull);
  t.minute = u16((rem / 60000000ull) % 60);
  t.second = u16((rem / 1000000ull) % 60);
  t.microsecond = u32(rem % 1000000ull);
}
s64 tz_offset_us() {
  TIME_ZONE_INFORMATION tzi{};
  DWORD r = GetTimeZoneInformation(&tzi);
  s64 bias = tzi.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tzi.DaylightBias : 0);
  return -bias * 60ll * 1000000ll;
}
u64 utc_now_tick() {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  u64 t = (u64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // 100 ns since 1601-01-01
  return t / 10 + days_from_civil(1601, 1, 1) * kUsPerDay;
}

s32 cellRtcGetCurrentClockLocalTime(Ptr<DateTime> dt) {
  if (!dt) return s32(0x80010602);  // CELL_RTC_ERROR_INVALID_POINTER
  set_from_tick(*dt, u64(s64(utc_now_tick()) + tz_offset_us()));
  return CELL_OK;
}
s32 cellRtcGetTick(Ptr<DateTime> dt, Ptr<be64> tick) {
  if (!dt || !tick) return s32(0x80010602);
  *tick = tick_of(*dt);
  return CELL_OK;
}
s32 cellRtcSetTick(Ptr<DateTime> dt, Ptr<be64> tick) {
  if (!dt || !tick) return s32(0x80010602);
  set_from_tick(*dt, *tick);
  return CELL_OK;
}
s32 cellRtcConvertLocalTimeToUtc(Ptr<be64> local, Ptr<be64> utc) {
  if (!local || !utc) return s32(0x80010602);
  *utc = u64(s64(u64(*local)) - tz_offset_us());
  return CELL_OK;
}
s32 cellRtcConvertUtcToLocalTime(Ptr<be64> utc, Ptr<be64> local) {
  if (!local || !utc) return s32(0x80010602);
  *local = u64(s64(u64(*utc)) + tz_offset_us());
  return CELL_OK;
}
s32 cellRtcGetDayOfWeek(s32 year, s32 month, s32 day) {
  if (year < 1 || month < 1 || month > 12 || day < 1) return s32(0x80010603);  // CELL_RTC_ERROR_INVALID_ARG
  return s32((days_from_civil(u32(year), u32(month), u32(day)) + 1) % 7);  // 0001-01-01 was a Monday; 0 = Sunday
}

}  // namespace

static hle::Module s_module("cellRtc", {
    HLE_FN(cellRtcConvertLocalTimeToUtc),
    HLE_FN(cellRtcConvertUtcToLocalTime),
    HLE_FN(cellRtcGetCurrentClockLocalTime),
    HLE_FN(cellRtcGetDayOfWeek),
    HLE_FN(cellRtcGetTick),
    HLE_FN(cellRtcSetTick),
});

}  // namespace rt
