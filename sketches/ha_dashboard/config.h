// Dashboard settings: which Home Assistant entities to show and how often to refresh.

#pragma once

// Update interval while on battery (deep sleep in between)
#define REFRESH_MINUTES 15

#define WEATHER_ENTITY "weather.forecast_gap_home"

// Outdoor sensor, shown in the weather panel
#define OUTDOOR_NAME   "Balkon"
#define OUTDOOR_PREFIX "sensor.shellyplusht_balkon_"

// Indoor rooms, shown top to bottom. Each prefix gets "temperature" / "humidity" appended.
struct Room {
    const char *name;
    const char *prefix;
};

static const Room ROOMS[] = {
    {"Wohnzimmer", "sensor.shellyplusht_wohnzimmer_"},
    {"Büro", "sensor.shellyplusht_buro_"},
    {"Kinderzimmer", "sensor.shellyplusht_kinderzimmer_"},
    {"Schlafzimmer", "sensor.shellyplusht_schlafzimmer_"},
};
static const int ROOM_COUNT = sizeof(ROOMS) / sizeof(ROOMS[0]);
