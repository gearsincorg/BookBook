#pragma once

// LED signals. Three looks: a steady colour, a spinner (always "waiting"), or the whole ring flashing (an update is downloading).
//
//   boot          red, green, blue, then spinning yellow
//   waiting()     spinning yellow  : starting up, not online yet
//   ready()       low green        : ready for touch-to-talk (one low-yellow LED among them if an update is available)
//   listening()   low blue         : the pad is touched, recording
//   thinking()    spinning blue    : released, waiting for the answer
//   speaking()    low red          : something is being spoken (the intro, or an answer)
//   updating()    flashing yellow  : a firmware update is downloading (whole ring, 2 Hz)
//
// A spinner is ONE bright LED circling the ring once every two seconds, followed by a 3-LED tail of diminishing
// intensity. One task owns the strip; callers only say which state they are in, and it shows within ~10 ms.
namespace leds {

void start();          // start the animation task (after board::init())
void boot_sequence();  // red, green, blue (blocking, about 1.2 s), then waiting()

void waiting();
void ready(bool update_available = false);
void listening();
void thinking();
void speaking();
void updating();

void solid(unsigned char r, unsigned char g, unsigned char b);  // any steady colour

}  // namespace leds
