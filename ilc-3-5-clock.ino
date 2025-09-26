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

#include <Si4703.h>
#include <Wire.h>
#include <DS3231.h>
#include <TimeLib.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <GyverPortal.h>
#include <WiFiUdp.h>

// SI4703 (radio chip) pins
const int SDIO = 8;
const int SCLK = 9;
const int SEN = 5;
const int RST = 7;

// MAX6921 (driver chip) pins
const int MAX_BLANK = 1;
const int MAX_CLK = 2;
const int MAX_LOAD = 3;
const int MAX_DIN = 4;

// DS3231 pins and speeed
#define SDA_PIN 8
#define SCL_PIN 9
#define WIRE_SPEED 100000

// DS3231 clock
DS3231 myRTC;

// Maximum volume
const int MAX_VOLUME=16;
Si4703 radio(RST, SDIO, SCLK);
int channel = 9440;  // Radio chanel
int volume = 15; // Radio volume
char rdsBuffer[16];

// NTP update interval in seconds
const int NTP_UPDATE_INTERVAL=3000;

// Display data: digits and dots.
int disp_bits[8];
// Our fake TZ
struct timezone tz = {0, 0};

// Clock global configuration.
char ntpServerName[80] = "fi.pool.ntp.org";
signed char clock_tz = 2; // Timezone shift (could be negative)
unsigned char clock_bar_mode = 0;   // bar mode
unsigned char clock_use_ntp = true;  // Use NTP switch
unsigned char clock_use_rtc = false;  // Use RTC switch
unsigned char clock_use_rds = true;  // Use RDS switch
unsigned char clock_show_weekday = true;

// Clock eeprom data address.
const int eeprom_addr=12;

// If we do not have WiFi we wait 60 seconds in the configuration portal.
const int WIFI_MANAGER_TIMEOUT=60;

// Web interface
GyverPortal ui;

// Read the config data from EEPROM.
void read_eeprom_data()
{
  // Read the EEPROM settings.
  clock_tz = (signed char)EEPROM.read(eeprom_addr);
  volume = EEPROM.read(eeprom_addr+1);
  clock_bar_mode = EEPROM.read(eeprom_addr+4);
  clock_use_ntp = EEPROM.read(eeprom_addr+5);
  clock_show_weekday = EEPROM.read(eeprom_addr+6);
  clock_use_rds = EEPROM.read(eeprom_addr+7);
  channel = EEPROM.readInt(eeprom_addr+8);
  EEPROM.readString(eeprom_addr+12, ntpServerName, sizeof(ntpServerName)-1);
  EEPROM.commit();
}

// Write the config data to EEPROM.
void write_eeprom_data()
{
  EEPROM.write(eeprom_addr, clock_tz);
  EEPROM.write(eeprom_addr+1, volume);
  EEPROM.write(eeprom_addr+4, clock_bar_mode);
  EEPROM.write(eeprom_addr+5, clock_use_ntp);
  EEPROM.write(eeprom_addr+6, clock_show_weekday);
  EEPROM.write(eeprom_addr+7, clock_use_rds);
  EEPROM.writeInt(eeprom_addr+8, channel);
  EEPROM.writeString(eeprom_addr+12, ntpServerName);
  EEPROM.commit();
}

// Initialize the network.
bool initialize_network()
{
  WiFiManager wm;
  // wm.resetSettings();

  // Automatically connect using saved credentials,
  // if connection fails, it starts an access point with the name "NixieClock".
  wm.setConfigPortalTimeout(WIFI_MANAGER_TIMEOUT);
  bool res = wm.autoConnect("NixieClock");
  wm.stopWebPortal();

  return res;
}

// Set clock time to H:M:S
void set_clock_time(unsigned int h, unsigned int m, unsigned int s)
{
  log_printf("set time: %02u:%02u:%02u\n", h, m, s);

  // Check time sanity. Uninitialized RTC might give strange values.
  if (h<24 && m<60 && s<60) {
    // Get current time.
    struct timeval tv = {0};
    gettimeofday(&tv, &tz);
    // Clean hours minutes and seconds, preserving day month and year.
    unsigned long sec_in_day = tv.tv_sec%(60*60*24);
    tv.tv_sec -= sec_in_day;
    // Set hours minutes and seconds from the pareameters.
    tv.tv_sec += h*60*60+m*60+s;
    // Set current time
    settimeofday(&tv, &tz);
  }
}

// Get time from RTC and set it as clock time.
void get_time_from_rtc()
{
  bool h12Flag;
  bool pmFlag;
  unsigned int h = myRTC.getHour(h12Flag, pmFlag);
  unsigned int m = myRTC.getMinute();
  unsigned int s = myRTC.getSecond();

  log_printf("set time from RTC: %02u:%02u:%02u\n", h, m, s);

  // Check time for sanity.
  if (h<24 && m<60 && s<60) {
    set_clock_time(h, m, s);
  }
}

// Interrupt function to updae the display with current time.
void IRAM_ATTR Timer0_ISR()
{
  char disp_text[8];
  set_time_to_display(disp_text);
  update_display(disp_text);
}

void setup()
{
  // Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN, WIRE_SPEED);
  init_spi();

  EEPROM.begin(100);

  read_eeprom_data();

  log_printf("\ndoing setup: clock_tz=%d\n", clock_tz);

  if (clock_use_rtc) {
    get_time_from_rtc();
  }

  if (initialize_network()) {
    // Show the clock IP address
    IPAddress myIP = WiFi.localIP();
    String ip_addr_str = myIP.toString();
    log_printf("AP IP address: %s\n", ip_addr_str.c_str());
    run_string_on_display(ip_addr_str.c_str());

    // start server portal
    ui.attachBuild(build);
    ui.attach(action);
    ui.start();
    log_printf("setup ui started\n");

    if (clock_use_ntp) {
      getNtpTime();
    }
  }
  // init_radio();

  // Set the timer to update the display 4 times/sec.
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
    char disp_text[8];
    int len = strlen(str);

    log_printf("run_string_on_display: str=%s len=%d\n", str, len);

    for (int s=-8; s<=len; s++) {
      for (int i=0; i<8; i++) {
        if (i+s>=0 && i+s<len && str[i+s]>='0' && str[i+s]<='9') {
          // character fits to the display and it is betweern 0 and 9.
          disp_text[i] = str[i+s];
        } else {
          disp_text[i] = ' ';
        }
      }

      update_display(disp_text);

      unsigned long end_time = millis()+200;
      for (int i=0; millis()<end_time; i++) {
        show_display(i%8);
      }
    }
}

// ==============================================================================

// Following code is derived from TimeNTP sample
WiFiUDP Udp;
unsigned int localPort = 8888;  // local port to listen for UDP packets
const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; // buffer to hold incoming & outgoing packets

time_t getNtpTime()
{
  IPAddress ntpServerIP; // NTP server's ip address

  while (Udp.parsePacket() > 0) ; // discard any previously received packets
  WiFi.hostByName(ntpServerName, ntpServerIP);
  log_printf("Transmit NTP Request: %s\n", ntpServerName);
  // get a random server from the pool
  sendNTPpacket(ntpServerIP);
  uint32_t beginWait = millis();
  while (millis() - beginWait < 1500) {
    int size = Udp.parsePacket();
    if (size >= NTP_PACKET_SIZE) {
      Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
      time_t secsSince1900;
      // convert four bytes starting at location 40 to a long integer
      secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
      secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
      secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
      secsSince1900 |= (unsigned long)packetBuffer[43];
      secsSince1900 = secsSince1900 - 2208988800UL + clock_tz * SECS_PER_HOUR;

      log_printf("Receive NTP Response %lu\n", (unsigned long)secsSince1900);

      if (clock_use_rtc) {
        tm *ttm = localtime(&secsSince1900);
        myRTC.setSecond(ttm->tm_sec);
        myRTC.setMinute(ttm->tm_min);
        myRTC.setHour(ttm->tm_hour);
      }
      
      struct timeval tv = { .tv_sec = secsSince1900, .tv_usec = 0 };
      // Set current time
      settimeofday(&tv, &tz);

      return secsSince1900;
    }
  }
  log_printf("No NTP Response :-(\n");
  return 0;
}

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address)
{
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

void loop()
{
  // Keep the digit number static to give all the digits aproximately the
  // same time share.
  static unsigned int i=0;

  if (clock_use_rtc && i%(1024*1024) == 0) {
    get_time_from_rtc();
  }

  // Show the next digit.
  show_display(i%8);
  i++;
  // loop_radio();

  // Web UI tick.
  if (i%155==0) {
     ui.tick();
  }
}

// ========================================================================================
void init_radio()
{
  log_printf("init_radio\n");
  radio.powerUp();
  log_printf("init_radio on\n");
  radio.setVolume(volume);
  radio.setChannel(channel);
  //log_printf("init_radio set done\n");
  //radio.readRDS(rdsBuffer, 4000);
  log_printf("init_radio rds done\n");
}

void loop_radio()
{
  log_printf("RDS listening");
  radio.readRDS(); // rdsBuffer, 4000);
  log_printf("RDS heard: %s", rdsBuffer);
  for (int i=0; i<10; i++) {
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
    GP_MAKE_BOX(GP.LABEL("Bar mode"); GP.SELECT("clock_bar_mode", "Always off, Async, Sync, Always on", clock_bar_mode););
    GP_MAKE_BOX(GP.LABEL("Use RDS"); GP.SWITCH("clock_use_rds", clock_use_rds ? true: false, 0););
    GP_MAKE_BOX(GP.LABEL("Use NTP"); GP.SWITCH("clock_use_ntp", clock_use_ntp ? true: false););
    GP_MAKE_BOX(GP.LABEL("Show weekday"); GP.SWITCH("clock_show_weekday", clock_show_weekday ? true: false, 0););
    GP_MAKE_BOX(GP.LABEL("Channel"); GP.SLIDER("channel", channel, 9400, 10800););
    GP_MAKE_BOX(GP.LABEL("NTP Server name: "); GP.TEXT("clock_ntp_server", "local NTP server if you have", ntpServerName, "", sizeof(ntpServerName)-1););
  );
  GP.SUBMIT("UPDATE");

  GP.FORM_END();

  GP.FORM_BEGIN("/settime");

  time_t t = time(NULL);
  tm *ttm = localtime(&t);
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
      myRTC.setSecond(gptime.hour);
      myRTC.setMinute(gptime.minute);
      myRTC.setHour(gptime.second);
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

  digitalWrite(MAX_LOAD, LOW);
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
    if (disp_text[0]=='1') {
      bits = dot;
    }
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

// Set the display data for ith digit, send it to MAX driver and delay
// for some time to let the digit show.
void show_display(int i)
{
  int bits = disp_bits[i];
  // If all the bits are blank - there is nothing to display and we
  // do not need to delay here.
  if (bits) {
    send_spi_data(bits<<12); // Lowest 11 bits are not used in MAX6921 driver
    delayMicroseconds(50);
  }
}

// Set the display data for 8 display digits.
// Every digit needs a bitmask derived from the disp_text character.
void update_display(const char *disp_text)
{
  for (int i=0; i<8; i++) {
    disp_bits[i] = show_bits(i+1, disp_text);
  }
}

// Get the system time and set it to to disp_test as text.
void set_time_to_display(char *disp_text)
{
    struct timeval tv;
    // Read the current time
    gettimeofday(&tv, &tz);

    int s = tv.tv_sec;
    int m = tv.tv_sec/60;
    int h = tv.tv_sec/(60*60);

    // Set hours. ILC display could only show 1 as the first digit.
    // So it is ether blank or 1, and our clock is always 12h mode.
    h %= 12;
    if (h==0) {
      h = 12;
    }

    // Calculate minutes and seconds.
    m %= 60;
    s %= 60;
    // log_printf("Chip time %02d:%02d:%02d\n", h, m, s);

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
    disp_text[6] = s/10+'0';
    disp_text[7] = s%10+'0';

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

    // Show weekaday
    if (clock_show_weekday) {
      time_t rawtime = tv.tv_sec;
      tm *timeinfo = localtime(&rawtime);
      disp_text[2] = weekday_table[timeinfo->tm_wday];
    } else {
      disp_text[2] = ' ';
    }
}

// Send data to MAX6921 via SPI.
void send_spi_data(unsigned int spi_data)
{
    digitalWrite(MAX_LOAD, LOW);
    for (int i=0; i<32; i++) {
        digitalWrite(MAX_CLK, LOW);
        int bit = (spi_data>>i)&1;
        digitalWrite(MAX_DIN, bit);
        digitalWrite(MAX_CLK, HIGH);
    }
    digitalWrite(MAX_LOAD, HIGH);
}