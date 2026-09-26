#pragma once

// "Waiting" sounds, modelled on Bookworm's ThinkingSoundPlayer: a short confirmation beep, then a
// soft repeating chime (single ping / rising pair / falling pair, rotating every ~10 s) so there is
// always audible proof the device is working. Plays through audio:: at the user's volume setting.
namespace thinking {

void prepare();  // compute the tones once, at start-up, so no press has to wait for it
void start();  // beep, then chimes until stop(); returns immediately
void stop();   // stops within ~20 ms and releases the speaker; safe to call when not running
void beep();   // one short blocking beep ("speak now"); does nothing while start() is running

}  // namespace thinking
