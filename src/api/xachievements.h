#pragma once

#include <stdint.h>

namespace xls {

// Unlock state of one achievement for the local user: Steam first, then the local record kept
// for apps whose Steamworks setup has no achievement of that name.
bool AchievementUnlocked(uint32_t achievementId, uint64_t* unixTime);

}
