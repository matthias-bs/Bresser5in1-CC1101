/*
Weather logger using bresser-5-in-1 data on 868Mhz using CC1101

(WIP TODO/TEST: BMP280 for pressure measurements as those are measured in the base unit)

Uses information/code cribbed from a variety of sources:

https://github.com/RFD-FHEM/RFFHEM/blob/00b9492bc3e4a88d7457e68bde55a91846247b4c/FHEM/lib/SD_ProtocolData.pm
https://github.com/RFD-FHEM/SIGNALDuino
https://github.com/jgromes/RadioLib/issues/168
https://github.com/merbanan/rtl_433/blob/master/src/devices/bresser_5in1.c

15/01/22 - Sean Siford
*/

#include <Arduino.h>
#include <RadioLib.h>
#include <stdint.h>
#include "wifi_details.h"
#include "WifiHandler.h"
#include "GoogleSheets.h"
#include "WeatherDataBuffer.h"
#include "PressureSensor.h"

CC1101 radio = new Module(PIN_CC1101_CS, PIN_CC1101_GDO0, RADIOLIB_NC, PIN_CC1101_GDO2);

typedef enum DecodeStatus {
  DECODE_OK, DECODE_PAR_ERR, DECODE_CHK_ERR
} DecodeStatus;

typedef enum SamplingState {
  INITIAL_WIFI_CONNECTION,
  REINIT_WIFI_CONNECTION,
  AWAIT_TIME_SLOT,
  CAPTURE_WEATHER_DATA,
  SEND_WEATHER_DATA,
  SLEEP_UNTIL_TIME_SLOT
} SamplingState;

static SamplingState samplingState = INITIAL_WIFI_CONNECTION;
static const int     sleepTimeMinutes = 10;
static int32_t       wifiReinitAttempts = 0;
static uint64_t      targetWakeTime = 0; // YYMMDDMM
static bool          lightSlept = false;

// Cribbed from rtl_433 project - but added extra checksum to verify uu
//
// https://github.com/merbanan/rtl_433/blob/master/src/devices/bresser_5in1.c
//
// Example input data:
//   EA EC 7F EB 5F EE EF FA FE 76 BB FA FF 15 13 80 14 A0 11 10 05 01 89 44 05 00
//   CC CC CC CC CC CC CC CC CC CC CC CC CC uu II SS GG DG WW  W TT  T HH RR  R Bt
// - C = Check, inverted data of 13 byte further
// - uu = checksum (number/count of set bits within bytes 14-25)
// - I = station ID (maybe)
// - G = wind gust in 1/10 m/s, normal binary coded, GGxG = 0x76D1 => 0x0176 = 256 + 118 = 374 => 37.4 m/s.  MSB is out of sequence.
// - D = wind direction 0..F = N..NNE..E..S..W..NNW
// - W = wind speed in 1/10 m/s, BCD coded, WWxW = 0x7512 => 0x0275 = 275 => 27.5 m/s. MSB is out of sequence.
// - T = temperature in 1/10 °C, BCD coded, TTxT = 1203 => 31.2 °C
// - t = temperature sign, minus if unequal 0
// - H = humidity in percent, BCD coded, HH = 23 => 23 %
// - R = rain in mm, BCD coded, RRxR = 1203 => 31.2 mm
// - B = Battery. 0=Ok, 8=Low.
// - S = sensor type, only low nibble used, 0x9 for Bresser Professional Rain Gauge
//
// Parameters:
//
// msg     - Pointer to message
// msgSize - Size of message
// pOut    - Pointer to WeatherData
//
// Returns:
//
// DECODE_OK      - OK - WeatherData will contain the updated information
// DECODE_PAR_ERR - Parity Error
// DECODE_CHK_ERR - Checksum Error
//
static DecodeStatus decodeBresser5In1Payload(uint8_t *msg, uint8_t msgSize, WeatherData *pOut) { 
    // First 13 bytes need to match inverse of last 13 bytes
    for (unsigned col = 0; col < msgSize / 2; ++col) {
        if ((msg[col] ^ msg[col + 13]) != 0xff) {
            Serial.printf("%s: Parity wrong at %u\n", __func__, col);
            return DECODE_PAR_ERR;
        }
    }

    // Verify checksum (number number bits set in bytes 14-25)
    uint8_t bitsSet = 0;
    uint8_t expectedBitsSet = msg[13];

    for(uint8_t p = 14 ; p < msgSize ; p++) {
      uint8_t currentByte = msg[p];
      while(currentByte) {
        bitsSet += (currentByte & 1);
        currentByte >>= 1;
      }
    }

    if (bitsSet != expectedBitsSet) {
       Serial.printf("%s: Checksum wrong actual [%02X] != expected [%02X]\n", __func__, bitsSet, expectedBitsSet);
       return DECODE_CHK_ERR;
    }

    pOut->sensor_id = msg[14];

    int temp_raw = (msg[20] & 0x0f) + ((msg[20] & 0xf0) >> 4) * 10 + (msg[21] &0x0f) * 100;
    if (msg[25] & 0x0f) {
        temp_raw = -temp_raw;
    }
    pOut->temp_c = temp_raw * 0.1f;

    pOut->humidity = (msg[22] & 0x0f) + ((msg[22] & 0xf0) >> 4) * 10;

    pOut->wind_direction_deg = ((msg[17] & 0xf0) >> 4) * 22.5f;

    int gust_raw = ((msg[17] & 0x0f) << 8) + msg[16];
    pOut->wind_gust_meter_sec = gust_raw * 0.1f;

    int wind_raw = (msg[18] & 0x0f) + ((msg[18] & 0xf0) >> 4) * 10 + (msg[19] & 0x0f) * 100;
    pOut->wind_avg_meter_sec = wind_raw * 0.1f;

    int rain_raw = (msg[23] & 0x0f) + ((msg[23] & 0xf0) >> 4) * 10 + (msg[24] & 0x0f) * 100;
    pOut->rain_mm = rain_raw * 0.1f;

    pOut->battery_ok = (msg[25] & 0x80) ? false : true;

    return DECODE_OK;
}

static bool emitBufferedDataEntry() {
  bool sent = false;
  WeatherDataEntry dataEntry;
  uint32_t entryRead = 0;
  // Peek the next data entry...
  if(peekNextWeatherDataEntry(&dataEntry, &entryRead)) {
    // OK - now need to emit a row...
    static char dateItem[12];
    static char timeItem[12];
    static char timestampItem[26];
    static char humidity[12];
    static char rain[12];
    static char temp[12];
    static char wind[12];
    static char gust[12];
    static char direction[12];
    static char pressure[12];
    sprintf(dateItem, "%02d/%02d/%02d", dataEntry.year, dataEntry.month, dataEntry.day);
    sprintf(timeItem, "%02d:%02d:%02d", dataEntry.hour, dataEntry.minute, dataEntry.second);
    sprintf(timestampItem, "%02d-%02d-%02dT%02d:%02d:%02dZ",
            dataEntry.year, dataEntry.month, dataEntry.day,
            dataEntry.hour, dataEntry.minute, dataEntry.second);
    sprintf(humidity, "%.2f", (float) dataEntry.weatherData.humidity);
    sprintf(rain, "%.3f", dataEntry.weatherData.rain_mm);
    sprintf(temp, "%.2f", dataEntry.weatherData.temp_c);
    sprintf(wind, "%.2f", dataEntry.weatherData.wind_avg_meter_sec);
    sprintf(gust, "%.2f", dataEntry.weatherData.wind_gust_meter_sec);
    sprintf(direction, "%.2f", dataEntry.weatherData.wind_direction_deg);
    sprintf(pressure, "%.2f", dataEntry.weatherData.pressure);
    SheetDataItem rowData[] = {
      { dateItem, false },
      { timeItem, false },
      { timestampItem, false },
      { humidity, true },
      { rain, true },
      { temp, true },
      { wind, true },
      { gust, true },
      { direction, true },
      { pressure,  true }
    };
    if(appendRowToSheet(rowData, sizeof(rowData) / sizeof(SheetDataItem))) {
      confirmPeekWeatherDataEntry(entryRead);
      sent = true;
    }
  }
  return sent;
}

static uint64_t assembleTargetTime(struct tm timeinfo, uint32_t minutesToAdd) {
  struct tm outputTime = timeinfo;
  outputTime.tm_sec = 0;
  outputTime.tm_min += minutesToAdd;
  mktime(&outputTime);
  // YYYMMDDHHMM
  uint64_t outputTimeEncoded = outputTime.tm_year - 100;
  outputTimeEncoded *= 100;
  outputTimeEncoded += outputTime.tm_mon + 1;
  outputTimeEncoded *= 100;
  outputTimeEncoded += outputTime.tm_mday;
  outputTimeEncoded *= 100;
  outputTimeEncoded += outputTime.tm_hour;
  outputTimeEncoded *= 100;
  outputTimeEncoded += outputTime.tm_min;
  return outputTimeEncoded;
}

static bool send() {
    Serial.println("Checking WiFi connection...");
    delay(100);
    bool sent = false;
    for(int retry = 0 ; retry < 6 && !handleWifiConnection() ; retry++) {
      Serial.println("WiFi connection pending - wait...");
      for(int i = 0 ; i < 20 && !handleWifiConnection() ; i++) {
        delay(250);
        Serial.print(".");
      }
      Serial.println();
    }

    if(!handleWifiConnection()) {
        Serial.println("Wifi connection not up - cannot upload...");
        return false;
    }

    if(!isGoogleAccessTokenInit()) {
      Serial.println("Checking Token...");
      delay(100);
      // Expire the token during a sleep period so as when we come back in we re-acquire one.
      setupGoogleAccessTokenAcquire((sleepTimeMinutes * 60) + 30);
    }
    Serial.println("Waiting for Token...");
    delay(100);
    // Allow up to 30 seconds for the token to be received...
    for(int i = 0 ; !checkGoogleAccessTokenReady() && i < 60 ; i++) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("Checking for Token Ready...");
    delay(100);
    if(checkGoogleAccessTokenReady()) {
      // Send up to 12 entries at a time, which means that we're catching up
      // 2 hours every time - so even if we have a full buffer, it will only take
      // a few hours to catch up.
      Serial.printf("%d item(s) to upload.\n", getTotalWeatherDataEntries());
      delay(100);
      for(int i = 0 ; i < 20 && getTotalWeatherDataEntries() > 0 ; i++) {
        sent |= emitBufferedDataEntry();
        delay(250);
      }
    }
    else {
      Serial.printf("Can't upload items - token not ready.\n");
    }
    return sent;
}

// Use canned sensor data for testing purposes
//#define _EMULATE_RECV_

#ifndef _EMULATE_RECV_
static bool capture() {
    bool captured = false;
    uint8_t recvData[27];
    int state = radio.receive(recvData, 27);
    if (state == RADIOLIB_ERR_NONE) {
      // Verify last syncword is 1st byte of payload (saa above)
      if (recvData[0] == 0xD4) {
          #ifdef _DEBUG_MODE_
            // print the data of the packet
            Serial.print("[CC1101] Data:\t\t");
            for(int i = 0 ; i < sizeof(recvData) ; i++) {
              Serial.printf(" %02X", recvData[i]);
            }
            Serial.println();

            Serial.printf("[CC1101] R [0x%02X] RSSI: %f LQI: %d\n", recvData[0], radio.getRSSI(), radio.getLQI());
          #endif

          // Decode the information - skip the last sync byte we use to check the data is OK
          WeatherData weatherData = { 0 };
          if(decodeBresser5In1Payload(&recvData[1], sizeof(recvData) - 1, &weatherData) == DECODE_OK) {

            // If pressure is available - read it
            if(pressureSensorAvailable()) {
              readPressureSensorHpa(weatherData.pressure);
            }

            const float METERS_SEC_TO_MPH = 2.237;
            char dateTime[24];
            struct tm timeinfo = {0};
            getLocalTime(&timeinfo);
            strftime(dateTime, 20, "%y-%m-%dT%H:%M:%S", &timeinfo);

            printf("[%s] [Bresser-5in1 (%d)] Batt: [%s] Temp: [%.1fC] Hum: [%d] WGust: [%.1f mph] WSpeed: [%.1f mph] WDir: [%.1f] Rain [%.1f mm] Pressure: [%.1f hPa]\n",
                  dateTime,
                  weatherData.sensor_id,
                  weatherData.battery_ok ? "OK" : "Low",
                  weatherData.temp_c,
                  weatherData.humidity,
                  weatherData.wind_gust_meter_sec * METERS_SEC_TO_MPH,
                  weatherData.wind_avg_meter_sec * METERS_SEC_TO_MPH,
                  weatherData.wind_direction_deg, weatherData.rain_mm,
                  weatherData.pressure);
            appendWeatherDataEntry(weatherData);
            captured = true;
          }
      }
      else {
        #ifdef _DEBUG_MODE_
          Serial.printf("[CC1101] R [0x%02X] RSSI: %f LQI: %d\n", recvData[0], radio.getRSSI(), radio.getLQI());
        #endif
      }
    } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
        #ifdef _DEBUG_MODE_
          Serial.print("T");
        #endif
    }
    else {
        // some other error occurred
        Serial.printf("[CC1101] Receive failed - failed, code %d\n", state);
    }  
    return captured;
}
#else
static bool capture() {
    WeatherData weatherData = { 0 };
    weatherData.humidity = 50;
    weatherData.pressure = 1024.4;
    weatherData.rain_mm = 54.0;
    weatherData.temp_c = 19.0;
    weatherData.wind_direction_deg = 90.0;
    weatherData.wind_avg_meter_sec = 0.3;
    weatherData.wind_gust_meter_sec = 1.0;
    appendWeatherDataEntry(weatherData);
    return true;
}
#endif

static void beginPressureSensor() {
  if (pressureSensorAvailable()) {
    Serial.printf("[BMP280] Unavailable - pressure will read as 0.\n");
  }
  else if(initPressureSensor()) {
    Serial.printf("[BMP280] Initialised OK.\n");
  }
  else {
    Serial.printf("[BMP280] Failed initialising - pressure unavailable.\n");
  }
}

static void initCC1101() {
    int state = radio.begin(868.35, 8.22, 57.136417, 270.0, 10, 32);
    if (state == RADIOLIB_ERR_NONE) {
        Serial.println("success!");
        state = radio.setCrcFiltering(false);
        if (state != RADIOLIB_ERR_NONE) {
            Serial.printf("[CC1101] Error disabling crc filtering: [%d]\n", state);
            while (true)
                ;
        }
        state = radio.fixedPacketLengthMode(27);
        if (state != RADIOLIB_ERR_NONE) {
            Serial.printf("[CC1101] Error setting fixed packet length: [%d]\n", state);
            while (true)
                ;
        }
        // Preamble: AA AA AA AA AA
        // Sync is: 2D D4 
        // Preamble 40 bits but the CC1101 doesn't allow us to set that
        // so we use a preamble of 32 bits and then use the sync as AA 2D
        // which then uses the last byte of the preamble - we recieve the last sync byte
        // as the 1st byte of the payload.
        state = radio.setSyncWord(0xAA, 0x2D, 0, false);
        if (state != RADIOLIB_ERR_NONE) {
            Serial.printf("[CC1101] Error setting sync words: [%d]\n", state);
            while (true)
                ;
        }
    } else {
        Serial.printf("[CC1101] Error initialising: [%d]\n", state);
        while (true)
            ;
    }
    Serial.println("[CC1101] Setup complete - awaiting incoming messages...");
}

void setup() {
    delay(5000);
    Serial.begin(115200);
    Serial.println("Bresser-5-in-1 logger initializing ... ");
    initCC1101();
    beginPressureSensor();
    setHostname("ESP32-WeatherMonitor");
    initWeatherDataBuffer();
    setWifiConnectionDetails(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {

    switch(samplingState) {
      case INITIAL_WIFI_CONNECTION:
        if(handleWifiConnection()) {
          samplingState = SLEEP_UNTIL_TIME_SLOT;
          Serial.printf("Connected to WiFi - OK\n");
        }
        break;
      case REINIT_WIFI_CONNECTION:
        if (handleWifiConnection()) {
          samplingState = CAPTURE_WEATHER_DATA;
          if (lightSlept == true) {
            Serial.printf("Connected to WiFi - OK - waiting a few seconds for time sync\n");
            lightSlept = false;
            for(int i = 0 ; i < 10 ; i++) {
              delay(1000);
              Serial.print(".");
            }
          }
        }
        else {
          // Allow up to 20 seconds to get a WiFi connection
          if (wifiReinitAttempts < 40) {
            delay(500);
            wifiReinitAttempts += 1;
            Serial.printf(".");
          }
          // Have we time synced?
          else if(getTime() >= (24 * 60 * 60 * 21)) {
            samplingState = AWAIT_TIME_SLOT;
            Serial.printf("Cannot connect to WiFi but time synced - so continuing...\n");
          }
          else {
            samplingState = SLEEP_UNTIL_TIME_SLOT;
            Serial.printf("Cannot connect to WiFi but time not synced so waiting for WiFi as times would be wrong...\n");
          }
        }
        break;
      case AWAIT_TIME_SLOT: {
          struct tm timeinfo;
          if(getLocalTime(&timeinfo)) {
            uint64_t currentTime = assembleTargetTime(timeinfo, 0);
            if (currentTime >= targetWakeTime) {
              samplingState = CAPTURE_WEATHER_DATA;
              Serial.printf("Reached time to capture data.\n");
            }
            else {
              Serial.printf("Awaiting time to capture data want: [%lld] got: [%lld].\n", targetWakeTime, currentTime);
              delay(2000);
            }
          }
        }
        break;
      case CAPTURE_WEATHER_DATA: {
          if(capture()) {
            Serial.printf("Captured data - signalling sending.\n");
            samplingState = SEND_WEATHER_DATA;
          }
        }
        break;
      case SEND_WEATHER_DATA: {
          Serial.printf("Attempting to send data...\n");
          send();
          Serial.printf("Attempted to send data - signalling sleep.\n");
          samplingState = SLEEP_UNTIL_TIME_SLOT;
        }
        break;
      case SLEEP_UNTIL_TIME_SLOT: {
          struct tm timeinfo;
          if(getLocalTime(&timeinfo)) {
            uint64_t minutesToWait = sleepTimeMinutes - (timeinfo.tm_min % sleepTimeMinutes);
            uint64_t secondsToWait = minutesToWait * 60;
            secondsToWait -= timeinfo.tm_sec;
            targetWakeTime = assembleTargetTime(timeinfo, minutesToWait);
            if (secondsToWait >= 30) {
              samplingState = REINIT_WIFI_CONNECTION;
              wifiReinitAttempts = 0;
              Serial.printf("Awaiting time to capture: [%lld].\n", targetWakeTime);
              Serial.printf("Sleeping for %llu second(s) until next timeslot - night night!\n", secondsToWait);
              disconnectWifi();
              delay(1000);
              lightSlept = true;
              esp_sleep_enable_timer_wakeup(secondsToWait * 1000000L);
              esp_light_sleep_start();
            }
            else {
              // Await the time slot...
              samplingState = AWAIT_TIME_SLOT;
            }
          }
        }
        break;
    }
}