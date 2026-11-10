// Home Assistant dashboard for the e-radionica Inkplate 6.
// Shows room temperature/humidity and the HA weather forecast. Wakes every REFRESH_MINUTES,
// fetches the data over Wi-Fi, redraws the screen and goes back to deep sleep.
// If HA can't be reached, the last good data (kept in RTC memory) is shown, marked as old.

#if !defined(ARDUINO_INKPLATE6) && !defined(ARDUINO_INKPLATE6V2)
#error "Select 'e-radionica Inkplate 6' or 'Soldered Inkplate 6' as the board."
#endif

#if !__has_include("secrets.h")
#error "Copy secrets.example.h to secrets.h and fill in your Wi-Fi and Home Assistant details."
#endif

#include "Inkplate.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <math.h>

#include "config.h"
#include "secrets.h"

#include "src/fonts/FreeSans12pt7b.h"
#include "src/fonts/FreeSans18pt7b.h"
#include "src/fonts/FreeSans9pt7b.h"
#include "src/fonts/FreeSansBold12pt7b.h"
#include "src/fonts/FreeSansBold18pt7b.h"
#include "src/fonts/FreeSansBold48pt7b.h"

Inkplate display(INKPLATE_3BIT);

// 3-bit grayscale: 0 = black ... 7 = white
const int C_BLACK = 0, C_DARK = 2, C_GRAY = 4, C_LIGHT = 5, C_PALE = 6, C_WHITE = 7;

// ---------------------------------------------------------------------------
// Data, kept in RTC memory so it survives deep sleep

const int MAX_ROOMS = 8, MAX_HOURS = 24, MAX_DAYS = 6;
static_assert(ROOM_COUNT <= MAX_ROOMS, "Too many rooms in config.h");

struct Hour {
    uint32_t t;
    float temp, rain;
};

struct Day {
    uint32_t t;
    float hi, lo, rain;
    char cond[20];
};

struct Data {
    uint32_t magic;
    uint32_t now; // UTC epoch from HA
    int32_t tz;   // UTC offset in seconds
    bool night;
    float roomT[MAX_ROOMS], roomH[MAX_ROOMS];
    float outT, outH;
    char cond[20];
    float temp, pressure, wind;
    int nHours, nDays;
    Hour hours[MAX_HOURS];
    Day days[MAX_DAYS];
};

const uint32_t MAGIC = 0x48414431; // "HAD1"
RTC_DATA_ATTR Data cache;
Data fresh;
String lastError;

// Declared before the first function: the Arduino builder inserts function prototypes there.
struct LocalTime {
    int year;
    unsigned month, day;
    int weekday; // 0 = Sunday
    int hour, minute;
    int64_t dayNumber;
};

enum Align { LEFT, CENTER, RIGHT };

// ---------------------------------------------------------------------------
// Time helpers (HA sends UTC timestamps; the UTC offset comes from HA too)

int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

void civilFromDays(int64_t z, int &y, unsigned &m, unsigned &d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = (int)(yoe + era * 400) + (m <= 2);
}

// "2026-11-10T17:00:00+00:00" -> UTC epoch
uint32_t parseIso(const char *s) {
    int Y, M, D, h, mi, sec, oh = 0, om = 0;
    char sign = 0;
    int n = sscanf(s, "%d-%d-%dT%d:%d:%d%c%d:%d", &Y, &M, &D, &h, &mi, &sec, &sign, &oh, &om);
    if (n < 6) return 0;
    int64_t t = daysFromCivil(Y, M, D) * 86400 + h * 3600 + mi * 60 + sec;
    if (n >= 8 && (sign == '+' || sign == '-')) t += (sign == '+' ? -1 : 1) * (oh * 3600 + om * 60);
    return (uint32_t)t;
}

LocalTime toLocal(uint32_t utc) {
    LocalTime lt;
    int64_t t = (int64_t)utc + cache.tz;
    lt.dayNumber = t / 86400;
    int secs = t % 86400;
    lt.hour = secs / 3600;
    lt.minute = (secs / 60) % 60;
    lt.weekday = (int)((lt.dayNumber + 4) % 7);
    civilFromDays(lt.dayNumber, lt.year, lt.month, lt.day);
    return lt;
}

const char *WEEKDAYS[] = {"Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"};
const char *MONTHS[] = {"Januar", "Februar", "März",      "April",   "Mai",      "Juni",
                        "Juli",   "August",  "September", "Oktober", "November", "Dezember"};

const char *conditionText(const char *c) {
    static const char *map[][2] = {
        {"sunny", "Sonnig"},           {"clear-night", "Klar"},         {"partlycloudy", "Teilweise bewölkt"},
        {"cloudy", "Bewölkt"},         {"fog", "Nebel"},                {"rainy", "Regen"},
        {"pouring", "Starkregen"},     {"snowy", "Schnee"},             {"snowy-rainy", "Schneeregen"},
        {"hail", "Hagel"},             {"lightning", "Gewitter"},       {"lightning-rainy", "Gewitter, Regen"},
        {"windy", "Windig"},           {"windy-variant", "Windig"},     {"exceptional", "Unwetter"},
    };
    for (auto &m : map)
        if (strcmp(c, m[0]) == 0) return m[1];
    return c;
}

// ---------------------------------------------------------------------------
// Home Assistant

String haPost(const char *path, const String &body, int &code) {
    HTTPClient http;
    http.setTimeout(10000);
    http.begin(String(HA_URL) + path);
    http.addHeader("Authorization", "Bearer " HA_TOKEN);
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
    String response = code == 200 ? http.getString() : String();
    http.end();
    return response;
}

String stateExpr(const String &entity) {
    return "\"{{ states('" + entity + "') }}\"";
}

String attrExpr(const char *attr) {
    return String("{{ state_attr('" WEATHER_ENTITY "','") + attr + "')|float(0) }}";
}

// One template call returns all current values as compact JSON.
String buildTemplate() {
    String t = "{\"now\":{{ now().timestamp()|int }},\"tz\":{{ now().utcoffset().total_seconds()|int }},\"rooms\":[";
    for (int i = 0; i < ROOM_COUNT; i++) {
        String p = ROOMS[i].prefix;
        if (i) t += ",";
        t += "[" + stateExpr(p + "temperature") + "," + stateExpr(p + "humidity") + "]";
    }
    String out = OUTDOOR_PREFIX;
    t += "],\"out\":[" + stateExpr(out + "temperature") + "," + stateExpr(out + "humidity") + "]";
    t += ",\"sun\":" + stateExpr("sun.sun");
    t += ",\"cond\":" + stateExpr(WEATHER_ENTITY);
    t += ",\"t\":" + attrExpr("temperature");
    t += ",\"p\":" + attrExpr("pressure");
    t += ",\"w\":" + attrExpr("wind_speed");
    t += "}";
    return t;
}

float toNumber(JsonVariant v) {
    if (v.is<float>()) return v.as<float>();
    const char *s = v.as<const char *>();
    if (!s) return NAN;
    char *end;
    float f = strtof(s, &end);
    return end == s ? NAN : f;
}

bool fetchCurrent(Data &d) {
    JsonDocument req;
    req["template"] = buildTemplate();
    String body;
    serializeJson(req, body);

    int code;
    String res = haPost("/api/template", body, code);
    if (code != 200) {
        lastError = "HA /api/template: HTTP " + String(code);
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, res)) {
        lastError = "HA template: ungueltiges JSON";
        return false;
    }
    d.now = doc["now"];
    d.tz = doc["tz"];
    for (int i = 0; i < ROOM_COUNT; i++) {
        d.roomT[i] = toNumber(doc["rooms"][i][0]);
        d.roomH[i] = toNumber(doc["rooms"][i][1]);
    }
    d.outT = toNumber(doc["out"][0]);
    d.outH = toNumber(doc["out"][1]);
    d.night = strcmp(doc["sun"] | "", "below_horizon") == 0;
    strlcpy(d.cond, doc["cond"] | "", sizeof(d.cond));
    d.temp = doc["t"];
    d.pressure = doc["p"];
    d.wind = doc["w"];
    return true;
}

bool fetchForecast(const char *type, JsonDocument &doc) {
    JsonDocument filter;
    // to<JsonObject>() creates the element; a plain JsonVariant of a missing element would stay null
    JsonObject f = filter["service_response"][WEATHER_ENTITY]["forecast"][0].to<JsonObject>();
    for (const char *key : {"datetime", "temperature", "templow", "precipitation", "condition"}) f[key] = true;

    String body = String("{\"entity_id\":\"" WEATHER_ENTITY "\",\"type\":\"") + type + "\"}";
    int code;
    String res = haPost("/api/services/weather/get_forecasts?return_response", body, code);
    if (code != 200) {
        lastError = String("HA Prognose (") + type + "): HTTP " + String(code);
        return false;
    }
    if (deserializeJson(doc, res, DeserializationOption::Filter(filter))) {
        lastError = "HA Prognose: ungueltiges JSON";
        return false;
    }
    return true;
}

bool fetchAll(Data &d) {
    if (!fetchCurrent(d)) return false;

    JsonDocument hourly;
    if (!fetchForecast("hourly", hourly)) return false;
    d.nHours = 0;
    for (JsonObject h : hourly["service_response"][WEATHER_ENTITY]["forecast"].as<JsonArray>()) {
        uint32_t t = parseIso(h["datetime"] | "");
        if (t + 3600 <= d.now) continue; // skip past hours
        if (d.nHours == MAX_HOURS) break;
        d.hours[d.nHours++] = {t, h["temperature"] | NAN, h["precipitation"] | 0.0f};
    }

    JsonDocument daily;
    if (!fetchForecast("daily", daily)) return false;
    d.nDays = 0;
    for (JsonObject x : daily["service_response"][WEATHER_ENTITY]["forecast"].as<JsonArray>()) {
        if (d.nDays == MAX_DAYS) break;
        Day &day = d.days[d.nDays++];
        day.t = parseIso(x["datetime"] | "");
        day.hi = x["temperature"] | NAN;
        day.lo = x["templow"] | NAN;
        day.rain = x["precipitation"] | 0.0f;
        strlcpy(day.cond, x["condition"] | "", sizeof(day.cond));
    }
    return true;
}

bool connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 20000) {
            lastError = "WLAN \"" WIFI_SSID "\" nicht erreichbar";
            return false;
        }
        delay(100);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Text. The GFX fonts only have ASCII, so umlauts, the degree sign and the middle dot
// are drawn by hand: a UTF-8 "ä" is printed as "a" with two dots on top.

const GFXfont *curFont;
const char SZ = 1, DEG = 2, MIDDOT = 3; // pseudo characters

void setFont(const GFXfont &f) {
    curFont = &f;
    display.setFont(&f);
}

const GFXglyph *glyph(char c) {
    return &curFont->glyph[(uint8_t)c - curFont->first];
}

int capHeight() {
    return -glyph('H')->yOffset;
}

int degreeRadius() {
    return max(2, capHeight() / 7);
}

// Decodes one character; returns bytes consumed. umlaut is set for ä/ö/ü/Ä/Ö/Ü.
int decode(const char *s, char &c, bool &umlaut) {
    uint8_t b0 = s[0], b1 = s[1];
    umlaut = false;
    if (b0 == 0xC3) {
        const char *bases = "aouAOU";
        const uint8_t codes[] = {0xA4, 0xB6, 0xBC, 0x84, 0x96, 0x9C};
        c = '?';
        for (int i = 0; i < 6; i++)
            if (b1 == codes[i]) c = bases[i], umlaut = true;
        if (b1 == 0x9F) c = SZ;
        return 2;
    }
    if (b0 == 0xC2 && b1 == 0xB0) return c = DEG, 2;
    if (b0 == 0xC2 && b1 == 0xB7) return c = MIDDOT, 2;
    c = (b0 >= 0x20 && b0 <= 0x7E) ? (char)b0 : '?';
    return 1;
}

int advance(char c) {
    if (c == DEG) return 2 * degreeRadius() + 3;
    if (c == SZ) return 2 * glyph('s')->xAdvance;
    if (c == MIDDOT) return 3 * glyph(' ')->xAdvance;
    return glyph(c)->xAdvance;
}

int textWidth(const char *s) {
    int w = 0;
    char c;
    bool umlaut;
    while (*s) {
        s += decode(s, c, umlaut);
        w += advance(c);
    }
    return w;
}

void drawText(int x, int y, const char *s, int color = C_BLACK, Align align = LEFT) {
    if (align == RIGHT) x -= textWidth(s);
    if (align == CENTER) x -= textWidth(s) / 2;
    display.setTextColor(color);
    char c;
    bool umlaut;
    while (*s) {
        s += decode(s, c, umlaut);
        if (c == DEG) {
            int r = degreeRadius();
            int cx = x + r + 1, cy = y - capHeight() + r;
            for (int i = 0; i < max(1, r / 3); i++) display.drawCircle(cx, cy, r - i, color);
        } else if (c == MIDDOT) {
            display.fillCircle(x + advance(c) / 2, y - capHeight() * 2 / 5, max(1, capHeight() / 9), color);
        } else if (c == SZ) {
            display.setCursor(x, y);
            display.print("ss");
        } else {
            display.setCursor(x, y);
            display.write(c);
            if (umlaut) {
                const GFXglyph *g = glyph(c);
                int d = max(2, (g->width + 3) / 5);
                int top = y + g->yOffset - d - max(1, d / 2);
                int left = x + g->xOffset;
                display.fillRect(left + g->width * 28 / 100 - d / 2, top, d, d, color);
                display.fillRect(left + g->width * 72 / 100 - d / 2, top, d, d, color);
            }
        }
        x += advance(c);
    }
}

String fmt(float v, int decimals, const char *unit) {
    if (isnan(v)) return String("--") + unit;
    return String(v, decimals) + unit;
}

// ---------------------------------------------------------------------------
// Weather icons, drawn with primitives so they scale to any size. w = icon width.

void thickLine(float x1, float y1, float x2, float y2, int color, float t) {
    display.drawThickLine(x1, y1, x2, y2, color, t);
}

void sun(int cx, int cy, int r) {
    float t = max(2.0f, r / 6.0f);
    for (int i = 0; i < 8; i++) {
        float a = i * PI / 4;
        thickLine(cx + cos(a) * r * 1.35, cy + sin(a) * r * 1.35, cx + cos(a) * r * 1.8, cy + sin(a) * r * 1.8,
                  C_BLACK, t);
    }
    display.fillCircle(cx, cy, r, C_BLACK);
    display.fillCircle(cx, cy, r - t, C_GRAY);
}

void moon(int cx, int cy, int r) {
    display.fillCircle(cx, cy, r, C_DARK);
    display.fillCircle(cx + r * 0.45, cy - r * 0.35, r * 0.8, C_WHITE);
}

// Cloud with a flat bottom at cy + 0.25 w
void cloud(int cx, int cy, int w, int fill) {
    const float circles[][3] = {{-0.22f, 0.05f, 0.20f}, {0.05f, -0.08f, 0.27f}, {0.30f, 0.08f, 0.17f}};
    int o = max(2, w / 30);
    for (int pass = 0; pass < 2; pass++) {
        int grow = pass == 0 ? o : 0, color = pass == 0 ? C_BLACK : fill;
        for (auto &c : circles) display.fillCircle(cx + c[0] * w, cy + c[1] * w, c[2] * w + grow, color);
        display.fillRect(cx - 0.22f * w, cy + 0.05f * w - grow, 0.52f * w, 0.20f * w + 2 * grow, color);
    }
}

void drops(int cx, int by, int w, int count) {
    float t = max(2.0f, w / 25.0f);
    for (int i = 0; i < count; i++) {
        float x = cx - 0.2f * w + i * 0.4f * w / (count - 1);
        thickLine(x, by + 0.08f * w, x - 0.06f * w, by + 0.25f * w, C_BLACK, t);
    }
}

void flakes(int cx, int by, int w) {
    float r = 0.06f * w;
    for (int i = 0; i < 3; i++) {
        float x = cx - 0.2f * w + i * 0.2f * w, y = by + (i % 2 ? 0.22f : 0.14f) * w;
        for (int k = 0; k < 3; k++) {
            float a = k * PI / 3;
            thickLine(x - cos(a) * r, y - sin(a) * r, x + cos(a) * r, y + sin(a) * r, C_BLACK, max(1.5f, w / 40.0f));
        }
    }
}

void bolt(int cx, int by, int w) {
    float x = cx + 0.02f * w;
    display.fillTriangle(x + 0.08f * w, by - 0.02f * w, x - 0.08f * w, by + 0.16f * w, x + 0.02f * w, by + 0.16f * w,
                         C_BLACK);
    display.fillTriangle(x + 0.06f * w, by + 0.11f * w, x - 0.03f * w, by + 0.11f * w, x - 0.07f * w, by + 0.33f * w,
                         C_BLACK);
}

void drawIcon(const char *c, int cx, int cy, int w, bool night) {
    auto is = [&](const char *s) { return strcmp(c, s) == 0; };
    int ccy = cy - 0.12f * w, by = ccy + 0.25f * w; // cloud center and bottom when something falls from it

    if (is("sunny")) {
        sun(cx, cy, 0.24f * w);
    } else if (is("clear-night")) {
        moon(cx, cy, 0.3f * w);
    } else if (is("partlycloudy")) {
        if (night) moon(cx + 0.12f * w, cy - 0.14f * w, 0.22f * w);
        else sun(cx + 0.14f * w, cy - 0.14f * w, 0.17f * w);
        cloud(cx - 0.06f * w, cy + 0.06f * w, 0.85f * w, C_WHITE);
    } else if (is("cloudy")) {
        cloud(cx + 0.14f * w, cy - 0.12f * w, 0.7f * w, C_LIGHT);
        cloud(cx - 0.06f * w, cy + 0.04f * w, 0.9f * w, C_WHITE);
    } else if (is("fog")) {
        cloud(cx, ccy, w, C_PALE);
        for (int i = 0; i < 3; i++)
            thickLine(cx - 0.3f * w + i * 0.05f * w, by + (0.1f + i * 0.08f) * w, cx + 0.3f * w - i * 0.05f * w,
                      by + (0.1f + i * 0.08f) * w, C_DARK, max(2.0f, w / 25.0f));
    } else if (is("windy") || is("windy-variant")) {
        float t = max(2.0f, w / 22.0f);
        thickLine(cx - 0.35f * w, cy - 0.15f * w, cx + 0.25f * w, cy - 0.15f * w, C_BLACK, t);
        thickLine(cx - 0.25f * w, cy, cx + 0.35f * w, cy, C_BLACK, t);
        thickLine(cx - 0.35f * w, cy + 0.15f * w, cx + 0.15f * w, cy + 0.15f * w, C_BLACK, t);
    } else {
        // Everything that falls out of a cloud
        bool storm = is("lightning") || is("lightning-rainy") || is("exceptional");
        cloud(cx, ccy, w, storm ? C_GRAY : C_PALE);
        if (storm) bolt(cx, by, w);
        if (is("rainy") || is("lightning-rainy")) drops(cx, by, w, 3);
        else if (is("pouring")) drops(cx, by, w, 5);
        else if (is("snowy")) flakes(cx, by, w);
        else if (is("snowy-rainy")) {
            drops(cx - 0.1f * w, by, w * 0.6f, 2);
            flakes(cx + 0.12f * w, by, w * 0.6f);
        } else if (is("hail")) {
            for (int i = 0; i < 4; i++)
                display.fillCircle(cx - 0.2f * w + i * 0.13f * w, by + (i % 2 ? 0.2f : 0.12f) * w, 0.035f * w,
                                   C_BLACK);
        }
    }
}

// ---------------------------------------------------------------------------
// Screen layout (800 x 600)

void drawHeader(bool stale, float battery) {
    LocalTime now = toLocal(cache.now);
    char buf[80];
    setFont(FreeSansBold18pt7b);
    snprintf(buf, sizeof(buf), "%s, %u. %s", WEEKDAYS[now.weekday], now.day, MONTHS[now.month - 1]);
    drawText(20, 38, buf);

    setFont(FreeSans12pt7b);
    if (stale)
        snprintf(buf, sizeof(buf), "Stand %02d:%02d · HA nicht erreichbar", now.hour, now.minute);
    else
        snprintf(buf, sizeof(buf), "Akku %.2f V · aktualisiert %02d:%02d", battery, now.hour, now.minute);
    drawText(780, 36, buf, stale ? C_BLACK : C_DARK, RIGHT);
    display.fillRect(0, 50, 800, 2, C_BLACK);
}

void drawCurrent() {
    drawIcon(cache.cond, 85, 125, 120, cache.night);

    setFont(FreeSansBold48pt7b);
    drawText(165, 155, fmt(cache.temp, 1, "°").c_str());

    setFont(FreeSans18pt7b);
    drawText(20, 212, conditionText(cache.cond));

    setFont(FreeSans12pt7b);
    String outdoor = String(OUTDOOR_NAME " ") + fmt(cache.outT, 1, "°") + " · " + fmt(cache.outH, 0, " %");
    drawText(20, 244, outdoor.c_str());
    String air = fmt(cache.pressure, 0, " hPa") + " · Wind " + fmt(cache.wind, 0, " km/h");
    drawText(20, 272, air.c_str(), C_DARK);

    // Today's high/low from the daily forecast
    if (cache.nDays && toLocal(cache.days[0].t).dayNumber == toLocal(cache.now).dayNumber) {
        const Day &d = cache.days[0];
        String today = "Heute " + fmt(d.hi, 0, "°") + " / " + fmt(d.lo, 0, "°") + " · " + fmt(d.rain, 1, " mm");
        drawText(20, 300, today.c_str(), C_DARK);
    }

    display.fillRect(392, 62, 2, 244, C_LIGHT);
}

void drawRooms() {
    const int top = 56, rowH = 250 / max(4, ROOM_COUNT), xName = 410, xRight = 785, gap = 16;

    // Column widths from the actual values, so long room names never run into the numbers
    int humW = 0, tempW = 0, nameW = 0;
    setFont(FreeSans18pt7b);
    for (int i = 0; i < ROOM_COUNT; i++) humW = max(humW, textWidth(fmt(cache.roomH[i], 0, "%").c_str()));
    setFont(FreeSansBold18pt7b);
    for (int i = 0; i < ROOM_COUNT; i++) tempW = max(tempW, textWidth(fmt(cache.roomT[i], 1, "°").c_str()));
    int xTemp = xRight - humW - gap; // right edge of the temperature column
    setFont(FreeSans18pt7b);
    for (int i = 0; i < ROOM_COUNT; i++) nameW = max(nameW, textWidth(ROOMS[i].name));
    const GFXfont &nameFont = xName + nameW + gap <= xTemp - tempW ? FreeSans18pt7b : FreeSans12pt7b;

    for (int i = 0; i < ROOM_COUNT; i++) {
        int y = top + i * rowH + rowH / 2 + 10;
        setFont(nameFont);
        drawText(xName, y, ROOMS[i].name);
        setFont(FreeSansBold18pt7b);
        drawText(xTemp, y, fmt(cache.roomT[i], 1, "°").c_str(), C_BLACK, RIGHT);
        setFont(FreeSans18pt7b);
        drawText(xRight, y, fmt(cache.roomH[i], 0, "%").c_str(), C_DARK, RIGHT);
        if (i < ROOM_COUNT - 1) display.drawFastHLine(410, top + (i + 1) * rowH, 375, C_PALE);
    }
}

void drawHourly() {
    display.fillRect(0, 312, 800, 2, C_BLACK);
    const int n = cache.nHours;
    if (n < 2) return;

    const int x0 = 62, x1 = 780, yTop = 334, yBot = 424;
    float tMin = 1e9, tMax = -1e9, rMax = 2.0f; // rain scale is at least 2 mm/h
    for (int i = 0; i < n; i++) {
        if (!isnan(cache.hours[i].temp)) {
            tMin = min(tMin, cache.hours[i].temp);
            tMax = max(tMax, cache.hours[i].temp);
        }
        rMax = max(rMax, cache.hours[i].rain);
    }
    if (tMin > tMax) return;
    float lo = floorf(tMin - 0.5f), hi = ceilf(tMax + 0.5f);
    auto xAt = [&](int i) { return x0 + (x1 - x0) * i / (n - 1); };
    auto yAt = [&](float t) { return yBot - (yBot - yTop) * (t - lo) / (hi - lo); };

    // Rain bars
    int barW = max(4, (x1 - x0) / n - 4);
    for (int i = 0; i < n; i++) {
        float r = cache.hours[i].rain;
        if (r < 0.05f) continue;
        int h = max(2, (int)((yBot - yTop) * r / rMax));
        display.fillRect(xAt(i) - barW / 2, yBot - h, barW, h, C_PALE);
    }

    // Hour labels every 3 hours
    setFont(FreeSans9pt7b);
    for (int i = 0; i < n; i++) {
        LocalTime lt = toLocal(cache.hours[i].t);
        if (lt.hour % 3) continue;
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d h", lt.hour);
        display.drawFastVLine(xAt(i), yBot + 2, 5, C_GRAY);
        drawText(xAt(i), yBot + 24, buf, C_DARK, CENTER);
    }
    display.drawFastHLine(x0, yBot + 1, x1 - x0, C_GRAY);

    // Temperature line
    for (int i = 1; i < n; i++) {
        if (isnan(cache.hours[i - 1].temp) || isnan(cache.hours[i].temp)) continue;
        thickLine(xAt(i - 1), yAt(cache.hours[i - 1].temp), xAt(i), yAt(cache.hours[i].temp), C_BLACK, 3);
    }

    // Axis: high and low temperature, and the rain scale if it rains
    drawText(52, yTop + 6, fmt(tMax, 0, "°").c_str(), C_BLACK, RIGHT);
    drawText(52, yBot, fmt(tMin, 0, "°").c_str(), C_BLACK, RIGHT);
    float rainSum = 0;
    for (int i = 0; i < n; i++) rainSum += cache.hours[i].rain;
    String label = "24 h · Regen " + fmt(rainSum, 1, " mm");
    drawText(780, yTop, label.c_str(), C_DARK, RIGHT);
}

void drawDaily() {
    display.fillRect(0, 462, 800, 2, C_BLACK);
    int64_t today = toLocal(cache.now).dayNumber;
    int col = 0;
    for (int i = 0; i < cache.nDays && col < 5; i++) {
        const Day &d = cache.days[i];
        LocalTime lt = toLocal(d.t);
        if (lt.dayNumber <= today) continue;
        int cx = col * 160 + 80;
        if (col) display.drawFastVLine(col * 160, 474, 116, C_PALE);

        setFont(FreeSansBold12pt7b);
        drawText(cx, 490, WEEKDAYS[lt.weekday], C_BLACK, CENTER);
        drawIcon(d.cond, cx, 526, 56, false);
        setFont(FreeSans12pt7b);
        String temps = fmt(d.hi, 0, "°") + " / " + fmt(d.lo, 0, "°");
        drawText(cx, 574, temps.c_str(), C_BLACK, CENTER);
        setFont(FreeSans9pt7b);
        if (d.rain >= 0.1f) drawText(cx, 594, fmt(d.rain, 1, " mm").c_str(), C_DARK, CENTER);
        col++;
    }
}

void drawError() {
    setFont(FreeSansBold18pt7b);
    drawText(400, 270, "Keine Daten", C_BLACK, CENTER);
    setFont(FreeSans12pt7b);
    drawText(400, 310, lastError.c_str(), C_DARK, CENTER);
    char buf[64];
    snprintf(buf, sizeof(buf), "Neuer Versuch in %d Minuten", REFRESH_MINUTES);
    drawText(400, 340, buf, C_DARK, CENTER);
}

// ---------------------------------------------------------------------------

void setup() {
    uint32_t start = millis();
    Serial.begin(115200);
    display.begin();

    bool ok = connectWifi() && fetchAll(fresh);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    if (ok) {
        Serial.printf("Fetched %d hourly and %d daily forecast entries\n", fresh.nHours, fresh.nDays);
        fresh.magic = MAGIC;
        cache = fresh;
    } else {
        Serial.println("Update failed: " + lastError);
    }

    float battery = display.readBattery();
    display.clearDisplay();
    if (cache.magic == MAGIC) {
        drawHeader(!ok, battery);
        drawCurrent();
        drawRooms();
        drawHourly();
        drawDaily();
    } else {
        drawError();
    }
    display.display();
    Serial.printf("Done in %lu ms, sleeping %d min\n", millis() - start, REFRESH_MINUTES);

    uint64_t elapsedUs = (uint64_t)(millis() - start) * 1000ULL;
    uint64_t periodUs = (uint64_t)REFRESH_MINUTES * 60ULL * 1000000ULL;
    esp_sleep_enable_timer_wakeup(periodUs > elapsedUs ? periodUs - elapsedUs : periodUs);
    esp_deep_sleep_start();
}

void loop() {
}
