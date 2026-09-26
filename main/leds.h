#pragma once

// LED signals. Two looks only: a steady colour, or a spinner, and a spinner always means "waiting".
//
//   boot          red, green, blue, then spinning yellow
//   waiting()     spinning yellow  : starting up, not online yet
//   ready()       low green        : ready for touch-to-talk
//   listening()   low blue         : the pad is touched, recording
//   thinking()    spinning blue    : released, waiting for the answer
//   speaking()    low red          : something is being spoken (the intro, or an answer)
//
// A spinner is ONE bright LED circling the ring once per second, followed by a 3-LED tail of diminishing
// intensity. One task owns the strip; callers only say which state they are in, and it shows within ~10 ms.
namespace leds {

void start();          // start the animation task (after board::init())
void boot_sequence();  // red, green, blue (blocking, about 1.2 s), then waiting()

void waiting();
void ready();
void listening();
void thinking();
void speaking();

void solid(unsigned char r, unsigned char g, unsigned char b);  // any steady colour

}  // namespace leds
