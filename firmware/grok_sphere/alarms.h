#pragma once
#include <Arduino.h>
// One-shot alarms ("Charlie, wake me at 6:30"), kept in NVS so they survive reboots. Times are local HH:MM.
void   alarms_init();
void   alarms_apply(const char* op, const char* time, const char* label);   // op "set" | "cancel" (time or "all")
String alarms_json();                     // [{"time":"06:30","label":".."},...] for the relay
bool   alarms_due(String& label);         // true once when an alarm's minute arrives (it is then removed)
