#pragma once
#include <Arduino.h>
void relay_init(const String& url, const String& token);
void relay_loop();
void relay_begin_turn();   // start streaming mic to the relay (pauses wake word)
void relay_end_turn();     // stop streaming
void relay_cancel();        // tap while Charlie talks: stop now, drop late audio
void relay_send_alarms();   // tell the relay which alarms are set
void relay_greet(const String& label);   // after an alarm is dismissed: Charlie says good morning
