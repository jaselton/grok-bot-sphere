#include "alarms.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>

static const int MAX_ALARMS = 8;
struct Alarm { char time[6]; char label[41]; };
static Alarm list[MAX_ALARMS];
static int count = 0;
static Preferences prefs;

static void save() {   // "HH:MM|label\n" per alarm
  String s;
  for (int i = 0; i < count; i++) { s += list[i].time; s += '|'; s += list[i].label; s += '\n'; }
  prefs.putString("list", s);
}
void alarms_init() {
  prefs.begin("alarms", false);
  String s = prefs.getString("list", "");
  count = 0;
  int start = 0;
  while (start < (int)s.length() && count < MAX_ALARMS) {
    int nl = s.indexOf('\n', start); if (nl < 0) nl = s.length();
    String line = s.substring(start, nl); start = nl + 1;
    int bar = line.indexOf('|'); if (bar != 5) continue;
    strlcpy(list[count].time, line.substring(0, 5).c_str(), sizeof list[count].time);
    strlcpy(list[count].label, line.substring(6).c_str(), sizeof list[count].label);
    count++;
  }
  Serial.printf("[alarms] %d loaded\n", count);
}
void alarms_apply(const char* op, const char* time, const char* label) {
  if (!strcmp(op, "set") && strlen(time) == 5) {
    for (int i = 0; i < count; i++) if (!strcmp(list[i].time, time)) { strlcpy(list[i].label, label ? label : "", sizeof list[i].label); save(); return; }
    if (count >= MAX_ALARMS) return;
    strlcpy(list[count].time, time, sizeof list[count].time);
    strlcpy(list[count].label, label ? label : "", sizeof list[count].label);
    count++;
  } else if (!strcmp(op, "cancel")) {
    if (!strcmp(time, "all")) count = 0;
    else for (int i = 0; i < count; i++) if (!strcmp(list[i].time, time)) { list[i] = list[--count]; break; }
  }
  save();
  Serial.printf("[alarms] %s %s -> %d set\n", op, time, count);
}
String alarms_json() {
  JsonDocument d; JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < count; i++) { JsonObject o = a.add<JsonObject>(); o["time"] = list[i].time; o["label"] = list[i].label; }
  String out; serializeJson(d, out); return out;
}
bool alarms_due(String& label) {
  static int lastMinute = -1;
  struct tm t; if (!getLocalTime(&t, 0)) return false;                 // no clock yet (NTP not synced)
  int minuteOfDay = t.tm_hour * 60 + t.tm_min;
  if (minuteOfDay == lastMinute) return false;
  lastMinute = minuteOfDay;
  char now[6]; snprintf(now, sizeof now, "%02d:%02d", t.tm_hour, t.tm_min);
  for (int i = 0; i < count; i++) if (!strcmp(list[i].time, now)) {
    label = list[i].label;
    list[i] = list[--count]; save();                                    // one-shot
    Serial.printf("[alarms] ringing %s %s\n", now, label.c_str());
    return true;
  }
  return false;
}
