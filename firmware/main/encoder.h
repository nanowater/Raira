#pragma once

/* The rotary encoder (EC11 module on header JP1: A = GPIO31, B = GPIO30, KEY = GPIO33). Turning it changes the volume
 * (one detent = one `media_poller_volume_step`), a short press is play/pause. The first input also tells the app that an
 * encoder is fitted (`music_app_encoder_seen`), which hides the on-screen volume button in its "Auto" mode.
 * Call once after the app has started. Returns false when a driver could not be set up (the app runs without it). */
bool encoder_start();
