// ILC-3-5 clock with ESP32 C3 super mini
//
// https://github.com/alexander-krotov/ilc-3-5-clock
//
// Copyright (c) 2025 Alexander Krotov.
//
// Code is mostly derived from
// https://github.com/alexander-krotov/IV28-clock
//
// NTP related code is derived from TimeNTP sample.
// Si4703 related code is derived from Si4703 sample.

// --- Include required libraries ---
#include <Si4703.h>      // FM radio chip control
#include <Wire.h>        // I2C communication
#include <DS3231.h>      // RTC chip library
#include <TimeLib.h>     // Time manipulation utilities
#include <WiFiManager.h> // WiFi and configuration portal
#include <EEPROM.h>      // Persistent storage
#include <GyverPortal.h> // Web UI
#include <WiFiUdp.h>     // UDP networking
#include <SPI.h>         // SPI to interface with MAX6921

// --- Hardware pin assignments ---
// SI4703 (FM radio) pins
const int SDIO = 8;
const int SCLK = 9;
const int SEN = 5;
const int RST = 7;

// MAX6921 (VFD tube driver) pins
const int MAX_BLANK = 1;
const int MAX_CLK = 2;
const int MAX_LOAD = 3;
const int MAX_DIN = 4;

// DS3231 (RTC) I2C pins and speed
const int SDA_PIN = 8;
const int SCL_PIN = 9;
const int WIRE_SPEED = 100000;

// --- RTC Instance ---
DS3231 myRTC;

// --- Application State ---
const int MAX_VOLUME = 16;
Si4703 radio(RST, SDIO, SCLK);  // FM radio object
int channel = 9440;             // Default FM channel (in 10kHz units)
int volume = 15;                // Default radio volume
char rdsBuffer[16];             // Buffer for RDS data

// Display state: digits and decimal points
const int display_size = 9;
int digit_bits[display_size];              // Encoded SPI data for each digit

// --- NTP Related ---
const int NTP_UPDATE_INTERVAL = 3000; // NTP sync interval (seconds)
struct timezone tz = {0, 0};    // Timezone placeholder

// --- Clock Configuration ---
char ntpServerName[80] = "fi.pool.ntp.org"; // NTP server address
signed char clock_tz = 2;           // Timezone offset (hours)
unsigned char clock_12 = true;      // 12h format flag. ILC-3-5 cannot show 24h format.
unsigned char clock_leading_0;      // Display leading zero on hours
unsigned char clock_bar_mode = 0;   // Separator/bar display mode
unsigned char clock_use_ntp = true; // Enable NTP synchronization
unsigned char clock_use_rtc = true; // Enable RTC synchronization
unsigned char clock_use_rds = true; // Enable RDS (FM radio text)unsigned char clock_show_weekday = true;
unsigned char clock_show_weekday;   // Show the weekday on a display
unsigned char clock_show_sec;       // Show the running seconds.

// --- EEPROM Configuration Storage ---
const int eeprom_addr = 12;         // EEPROM base address for config

// --- WiFi Manager Timeout ---
const int WIFI_MANAGER_TIMEOUT = 60; // Portal timeout (seconds)

// --- Web Interface ---
GyverPortal ui;

// Global variable for the display task handle
TaskHandle_t displayTaskHandle;
void show_display_string_task(void *parameter);

// --- EEPROM Read/Write ---
void read_eeprom_data()
{
  // Restore configuration from EEPROM
  clock_tz = (signed char)EEPROM.read(eeprom_addr);
  volume = EEPROM.read(eeprom_addr+1);
  clock_leading_0 = EEPROM.read(eeprom_addr+3);
  clock_bar_mode = EEPROM.read(eeprom_addr+4);
  clock_use_ntp = EEPROM.read(eeprom_addr+5);
  clock_use_rtc = EEPROM.read(eeprom_addr+6);
  clock_use_rds = EEPROM.read(eeprom_addr+7);
  channel = EEPROM.readInt(eeprom_addr+8);
  clock_show_weekday = EEPROM.read(eeprom_addr+9);
  clock_show_sec = EEPROM.read(eeprom_addr+10);
  EEPROM.readString(eeprom_addr+12, ntpServerName, sizeof(ntpServerName)-1);
  EEPROM.commit();
}

// Write the config data to EEPROM.
void write_eeprom_data()
{
  // Save current configuration to EEPROM
  EEPROM.write(eeprom_addr, clock_tz);
  EEPROM.write(eeprom_addr+1, volume);
  EEPROM.write(eeprom_addr+3, clock_leading_0);
  EEPROM.write(eeprom_addr+4, clock_bar_mode);
  EEPROM.write(eeprom_addr+5, clock_use_ntp);
  EEPROM.write(eeprom_addr+6, clock_use_rtc);
  EEPROM.write(eeprom_addr+7, clock_use_rds);
  EEPROM.writeInt(eeprom_addr+8, channel);
  EEPROM.write(eeprom_addr+9, clock_show_weekday);
  EEPROM.write(eeprom_addr+10,clock_show_sec);
  EEPROM.writeString(eeprom_addr+12, ntpServerName);
  EEPROM.commit();
}
// --- Network Initialization ---
bool initialize_network()
{
  WiFiManager wm;
  // Attempt auto-connect with credentials, fallback to AP if needed
  wm.setConfigPortalTimeout(WIFI_MANAGER_TIMEOUT);
  bool res = wm.autoConnect("NixieClock");
  wm.stopWebPortal();

  return res;
}

// --- Time Setting Helpers ---
void set_clock_time(unsigned int h, unsigned int m, unsigned int s)
{
  log_printf("set time: %02u:%02u:%02u\n", h, m, s);

  // Check input values for validity
  if (h < 24 && m < 60 && s < 60) {
    struct timeval tv = {0};
    tv.tv_sec = h*3600 + m*60 + s;
    settimeofday(&tv, &tz); // Set software system time
  } else {
    log_printf("set time: time is invalid\n");
  }
}

// --- RTC Synchronization ---
void get_time_from_rtc()
{
  bool h12Flag, pmFlag;
  unsigned int h = myRTC.getHour(h12Flag, pmFlag);
  unsigned int m = myRTC.getMinute();
  unsigned int s = myRTC.getSecond();

  log_printf("set time from RTC: %02u:%02u:%02u\n", h, m, s);

  set_clock_time(h, m, s);
}

// Updates display state with current time, called by hardware timer
void IRAM_ATTR Timer0_ISR()
{
  struct timeval tv;
  struct timezone tz = {0};
  gettimeofday(&tv, &tz);

  // localtime cannot be used inside timer function.
  int s = tv.tv_sec % 60;
  int m = (tv.tv_sec / 60) % 60;
  int h = (tv.tv_sec / 3600) % 24;

  // log_printf("Chip time %02d:%02d:%02d\n", h, m, s);

  // Format time as display string
  char disp_text[9];

  // Get the system time and set it to to disp_test as text.
  // Set hours. ILC display could only show 1 as the first digit.
  // So it is ether blank or 1, and our clock is always 12h mode.
  h %= 12;
  if (h==0) {
    h = 12;
  }

  // Print hours
  if (h>=10) {
    disp_text[0] = '1';
  } else {
    disp_text[0] = ' ';
  }
  disp_text[1] = h%10+'0';

  // Print minutes
  disp_text[3] = m/10+'0';
  disp_text[4] = m%10+'0';

  // Print seconds
  if (clock_show_sec) {
    disp_text[6] = s/10+'0';
    disp_text[7] = s%10+'0';
    disp_text[8] = 0;
  } else {
    disp_text[6] = 0;
    disp_text[7] = 0;
    disp_text[8] = 0;
  }
  // Show the : bar
  if (clock_bar_mode == 0) {
    disp_text[5] = ' ';
  } else if (clock_bar_mode==3) {
    disp_text[5] = ':';
  } else if (tv.tv_usec >= 500000) {
    disp_text[5] = (clock_bar_mode==2) ? ':': ' ';
  } else {
    disp_text[5] = (clock_bar_mode==2) ? ' ': ':';
  }

#if 0
  // Show weekaday
  if (0 && clock_show_weekday) {
    time_t rawtime = tv.tv_sec;
    tm *timeinfo = localtime(&rawtime);
    disp_text[2] = weekday_table[timeinfo->tm_wday];
  } else {
    disp_text[2] = ' ';
  }
#endif

  for (int i=0; i<display_size; i++) {
    digit_bits[i] = show_bits(i+1, disp_text);
  }
}

// --- Main Setup ---
void setup()
{
  // Initialize hardware peripherals
  // Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN, WIRE_SPEED);
  init_spi();

  EEPROM.begin(100);
  read_eeprom_data();

  log_printf("\ndoing setup: clock_tz=%d\n", clock_tz);

  // Set initial time from RTC if enabled
  if (clock_use_rtc) {
    get_time_from_rtc();
  }

  // Create FreeRTOS task for showing display string
  xTaskCreate(show_display_string_task, "DisplayTask", 2048, NULL, 1, &displayTaskHandle);

  // Start network and UI if WiFi is available
  if (initialize_network()) {
    IPAddress myIP = WiFi.localIP();
    String ip_addr_str = myIP.toString();
    log_printf("AP IP address: %s\n", ip_addr_str.c_str());
    run_string_on_display(ip_addr_str.c_str());

    // Setup web UI
    ui.attachBuild(build);
    ui.attach(action);
    ui.start();
    log_printf("setup ui started\n");

    // Sync time from NTP if enabled
    if (clock_use_ntp) {
      getNtpTime();
    }
  }
  // init_radio(); // Uncomment to enable radio

  // Setup display refresh timer (4 times/sec)
  static hw_timer_t * Timer0_Cfg = timerBegin(1000);
  if (Timer0_Cfg) {
    log_printf("Timer setup\n");
    timerAttachInterrupt(Timer0_Cfg, &Timer0_ISR);
    timerAlarm(Timer0_Cfg, 250, true, 0);
  }

  log_printf("setup done\n");
}

// Run a string on the display.
// We use it to run the clock assigned IP address.
void run_string_on_display(const char *str)
{
  char disp_text[display_size];
  int len = strlen(str);

  log_printf("run_string_on_display: str=%s len=%d\n", str, len);

  for (int s=-display_size; s<=len; s++) {
    for (int i=0; i<display_size; i++) {
      if (i+s>=0 && i+s<len && str[i+s]>='0' && str[i+s]<='9') {
        // character fits to the display and it is betweern 0 and 9.
        disp_text[i] = str[i+s];
      } else {
        disp_text[i] = ' ';
      }
    }

    for (int i=0; i<display_size; i++) {
      digit_bits[i] = show_bits(i+1, disp_text);
    }

    // Scroll the strings on display with 5 char/sec speed.
    delay(200);
  }
}

// ==============================================================================

// ==============================================================================
// --- NTP Time Sync (adapted from TimeNTP sample) ---
WiFiUDP Udp;
unsigned int localPort = 8888;   // UDP listen port
const int NTP_PACKET_SIZE = 48;  // NTP packet size
byte packetBuffer[NTP_PACKET_SIZE]; // NTP data buffer

// Request and update time from NTP server
time_t getNtpTime()
{
  IPAddress ntpServerIP; // NTP server IP

  while (Udp.parsePacket() > 0); // Flush old packets
  WiFi.hostByName(ntpServerName, ntpServerIP);
  log_printf("Transmit NTP Request: %s\n", ntpServerName);
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Udp.read(packetBuffer, NTP_PACKET_SIZE);
      time_t secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      secsSince1900 = secsSince1900 - 2208988800UL + clock_tz * SECS_PER_HOUR;

      log_printf("Receive NTP Response %lu\n", (unsigned long)secsSince1900);

      tm *ttm = localtime(&secsSince1900);
      myRTC.setSecond(ttm->tm_sec);
      myRTC.setMinute(ttm->tm_min);
      myRTC.setHour(ttm->tm_hour);

      set_clock_time(ttm->tm_hour, ttm->tm_min, ttm->tm_sec);

      return secsSince1900;
    }
  }
  log_printf("No NTP Response :-(\n");
  return 0;
}

// Send NTP request packet
void sendNTPpacket(IPAddress &address)
{
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011; // LI, Version, Mode
  packetBuffer[1] = 0;          // Stratum
  packetBuffer[2] = 6;          // Polling Interval
  packetBuffer[3] = 0xEC;       // Peer Clock Precision
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  Udp.beginPacket(address, 123);
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

// --- Main Loop ---
void loop()
{
  // Refresh UI and periodically sync time
  static unsigned int i = 0;
  if (i % 128 == 0) {
    // RTC sync (very infrequent)
    if (clock_use_rtc && i % (1024*1024) == 0) {
        get_time_from_rtc();
    }
  }
  i++;
  ui.tick();
  // loop_radio(); // Uncomment to enable radio loop
}

// ========================================================================================
// --- FM Radio Control ---
void init_radio()
{
  log_printf("init_radio\n");
  radio.powerUp();
  log_printf("init_radio on\n");
  radio.setVolume(volume);
  radio.setChannel(channel);
  //radio.readRDS(rdsBuffer, 4000);
  log_printf("init_radio rds done\n");
}

void loop_radio()
{
  log_printf("RDS listening");
  radio.readRDS(); // rdsBuffer, 4000);
  log_printf("RDS heard: %s", rdsBuffer);
  for (int i = 0; i < 10; i++) {
    log_printf(" %d ", rdsBuffer[i]);
  }
  log_printf("\n", rdsBuffer);
}

// Show the web UI form.
void build()
{
  log_printf("BUILD\n");

  GP.BUILD_BEGIN();
  GP.PAGE_TITLE("Nixie clock");

  GP.THEME(GP_DARK);
  GP.FORM_BEGIN("/update");

  GP_MAKE_BLOCK_TAB(
    "Clock config",
    GP_MAKE_BOX(GP.LABEL("TimeZone shift:"); GP.NUMBER("clock_tz", "", clock_tz););
    GP_MAKE_BOX(GP.LABEL("Volume:"); GP.SLIDER("clock_volume", volume, 0, MAX_VOLUME););
    GP_MAKE_BOX(GP.LABEL("Bar mode"); GP.SELECT("clock_bar_mode", "Always off, Blink, Always on", clock_bar_mode););
    GP_MAKE_BOX(GP.LABEL("Use RDS"); GP.SWITCH("clock_use_rds", clock_use_rds ? true: false, 0););
    GP_MAKE_BOX(GP.LABEL("Use NTP"); GP.SWITCH("clock_use_ntp", clock_use_ntp ? true: false););
    GP_MAKE_BOX(GP.LABEL("Show weekday"); GP.SWITCH("clock_show_weekday", clock_show_weekday ? true: false, 0););
    GP_MAKE_BOX(GP.LABEL("Show seconds"); GP.SWITCH("clock_show_sec", clock_show_sec ? true: false, 0););
    GP_MAKE_BOX(GP.LABEL("Channel"); GP.SLIDER("channel", channel, 9400, 10800););
    GP_MAKE_BOX(GP.LABEL("NTP Server name: "); GP.TEXT("clock_ntp_server", "local NTP server if you have", ntpServerName, "", sizeof(ntpServerName)-1););
  );
  GP.SUBMIT("UPDATE");

  GP.FORM_END();

  GP.FORM_BEGIN("/settime");

  time_t t = time(NULL);
  tm *ttm = localtime(&t);
  myRTC.setSecond(ttm->tm_sec);
  myRTC.setMinute(ttm->tm_min);
  myRTC.setHour(ttm->tm_hour);

  GPtime gptime (ttm->tm_hour, ttm->tm_min, ttm->tm_sec);

  GP_MAKE_BLOCK_TAB(
    "Time",
    GP_MAKE_BOX(GP.LABEL("Time :"); GP.TIME("time", gptime););
  );
  GP.SUBMIT("SET TIME");

  GP.FORM_END();

  GP.BUILD_END();
}

void action(GyverPortal& p)
{
  log_printf("ACTION\n");

  if (p.form("/update")) {
    int n;
    bool update_time = false;

    log_printf("ACTION update\n");

    // Reed the new values, and check them for sanity.
    n = ui.getInt("clock_tz");
    if (n>=-12 && n<=12) {
      if (n!=clock_tz) {
        update_time = true;
      }
      clock_tz = n;
    }

    n = ui.getInt("clock_volume");
    if (n>=0 && n<=MAX_VOLUME) {
      volume = n;
    }

    n = ui.getInt("clock_bar_mode");
    if (n>=0 && n<4) {
      clock_bar_mode = n;
    }

    n = ui.getBool("clock_use_rds");
    if (n>=0 && n<=1) {
      clock_use_rds = n;
    }

    n = ui.getBool("clock_use_ntp");
    if (n>=0 && n<=1) {
      clock_use_ntp = n;
    }

    n = ui.getBool("clock_show_weekday");
    if (n>=0 && n<=1) {
      clock_show_weekday = n;
    }

    n = ui.getBool("clock_show_sec");
    if (n>=0 && n<=1) {
      clock_show_sec = n;
    }

    String s = ui.getString("clock_ntp_server");
    if (s && strcmp(s.c_str(), ntpServerName) != 0) {
      strncpy(ntpServerName, s.c_str(), sizeof(ntpServerName)-1);
    }

    write_eeprom_data();

    if (update_time) {
      // Timezone changed, we shoule change the system time.
      if (clock_use_ntp) {
        getNtpTime();
      } else if (clock_use_rtc) {
        get_time_from_rtc();
      }
    }
  }

  if (p.form("/settime")) {
    GPtime gptime = ui.getTime("time");
    log_printf("Action Settime: %d:%02d:%02d\n", gptime.hour, gptime.minute, gptime.second);

    if (clock_use_rtc) {
      // Set time to RPC
      myRTC.setSecond(gptime.second);
      myRTC.setMinute(gptime.minute);
      myRTC.setHour(gptime.hour);
    }

    set_clock_time(gptime.hour, gptime.minute, gptime.second);
  }
}
// ===================================================================

// Initialize the serial peripheral interface (SPI) that will communicate
// with the MAX9621 VFD driver chip.
void init_spi()
{
  pinMode(MAX_LOAD, OUTPUT);
  pinMode(MAX_CLK, OUTPUT);
  pinMode(MAX_DIN, OUTPUT);
  pinMode(MAX_BLANK, OUTPUT);

  digitalWrite(MAX_LOAD, LOW);
  digitalWrite(MAX_BLANK, LOW);

  // Configure SPI pins
  SPI.begin(MAX_CLK, -1, MAX_DIN);  // CLK, MISO, MOSI, CS
  SPI.setFrequency(5000000);  // 5 MHz frequency
  SPI.setDataMode(SPI_MODE0);  // CPOL=0, CPHA=0
  SPI.setBitOrder(LSBFIRST);  // Least Significant Bit first
}

// Bitmaps for the characters
const int digit_table[] = {
   //TGWXX57AC8:BDF42E                                    T
   0b01000001100101001, // 0                 //   AAAA
   0b00000000100001000, // 1                 //  B    C
   0b01000001100010001, // 2                 //  B    C
   0b01000001100011000, // 3                 //   DDDD
   0b00000000100111000, // 4                 //  E    F
   0b01000001000111000, // 5                 //  E    F
   0b01000001000111001, // 6                 //   GGGG
   0b00000001100001000, // 7
   0b01000001100111001, // 8                 Mon=A Tue=:
   0b01000001100111000, // 9                 Wed=B Thu=C
   0b00000000000000000, //                   Fri=F Sat=D
   0b00000000000010000, // -                 Sun=G BOX=E
};

// Bits to set for weekday display (at position 2)
const int weekday_table[] = {
   //TGWXX57AC8:BDF42E
   0b00000001000000000,
   0b10000000000000000,
   0b00000000000100000,
   0b00000000100000000,
   0b00000000000001000,
   0b00000000000010000,
   0b01000000000000000,
   0b00000000000000001
};

// Character position bits.
const int position_table[] = {
   //TGWXX57AC8:BDF42E
   0b00000000000000000,
   0b00000000000000010,
   0b00000000000000000,
   0b00000000000000100,
   0b00000100000000000,
   0b00000000001000000,
   0b00000010000000000,
   0b00000000010000000,
   0b00000000000000000,
};

const int dot =
  //TGWXX57AC8:BDF42E
  0b10000000000000000;

// Calculate a bitmask for MAX6921 driver to show ith digit of disp_text.
int show_bits(int i, const char *disp_text)
{
  // Compose bits to be sent to MAX6921 to show one digit at one position.
  unsigned int bits = 0;

  switch (i) {
  case 0:
    break;
  case 2: // Weekday
    break;
  case 5:
    // : position.
    if (disp_text[i]==':') {
      bits = dot;
    }
    break;
  case 1:
    // First digit dot is a leading (1 or blank).
    if (disp_text[0]=='1') {
      bits = dot;
    }
    // Fall through. 
  default:
    if (disp_text[i] == '-') {
      bits |= digit_table[11];
    } else if (disp_text[i]>='0' && disp_text[i]<='9') {
      bits |= digit_table[disp_text[i]-'0'];  // Set the digit bits
    }
    break;
  }
  if (bits!=0 ) {
    bits |= position_table[i];  // Set the digit positon bit
  }
  return bits;
}

// --- SPI Data Transmission ---
// Send 32 bits to MAX6921
void send_spi_data(unsigned int spi_data)
{
  digitalWrite(MAX_LOAD, LOW);

  // Send data as 4 bytes (32 bits)
  SPI.write32(spi_data);

  digitalWrite(MAX_LOAD, HIGH);
  digitalWrite(MAX_BLANK, LOW);
}

void show_display_string_task(void *parameter)
{
  // Turn on the display
  digitalWrite(MAX_BLANK, LOW);

  // Loop through the display digits
  for (int i=0; ; i++) {
    // In this infinite loop we get back to the first digit.
    if (i == display_size) {
      i = 0;
    }

    if (digit_bits[i] == 0) {
      // No need to display this digit - it is all black.
      continue;
    }
    send_spi_data(digit_bits[i] << 12); // Lowest 11 bits are not used in MAX6921 driver

    // 2ms is sort of magic value: less - and the digits are dimmed,
    // more - and it starts to flicker.
    vTaskDelay(pdMS_TO_TICKS(2)); // Adjust the delay as necessary
  }
}
