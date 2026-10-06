#pragma once
#include <cstdint>

namespace guest {

struct LocalTime {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0;
  int ymd() const { return year * 10000 + month * 100 + day; }
};

// Nattlig rotation vid hour:minute. En missad rotation (t.ex. strömavbrott)
// tas igen fram till catchUpUntilHour, annars väntar den till nästa natt så att
// gäster inte kastas ut mitt på dagen.
class Schedule {
 public:
  Schedule(int hour, int minute, int catchUpUntilHour)
      : hour_(hour), minute_(minute), catchUpUntilHour_(catchUpUntilHour) {}

  // lastRotatedYmd == 0 betyder "aldrig" – då adopteras routerns lösenord i stället.
  bool isDue(const LocalTime& now, int lastRotatedYmd) const {
    if (lastRotatedYmd == 0 || now.ymd() <= lastRotatedYmd) return false;
    const int nowMin = now.hour * 60 + now.minute;
    return nowMin >= hour_ * 60 + minute_ && now.hour < catchUpUntilHour_;
  }

  // Väntetid före nytt försök efter `failures` misslyckanden i rad.
  static uint32_t retryDelaySeconds(int failures) {
    static const uint32_t steps[] = {60, 120, 300, 600, 900};
    if (failures < 1) return 0;
    const int i = failures - 1 < 4 ? failures - 1 : 4;
    return steps[i];
  }

 private:
  int hour_, minute_, catchUpUntilHour_;
};

}  // namespace guest
