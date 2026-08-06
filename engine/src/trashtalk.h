#pragma once

#include <string>
#include <vector>

#include "fanorona.h"

namespace fanorona {

/**
 * Picks a line of commentary for a completed search.
 *
 * @param prevScore evaluation after the AI's previous move, so a sudden jump
 *                  can be read as the human having blundered
 */
const char* trash_talk(int score, int prevScore, bool isMate, const std::string& feedback,
                       bool english);

/** Short label for the UI's mode badge. */
const char* strategy_name(int score);

}  // namespace fanorona
