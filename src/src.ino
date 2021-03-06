//******************************************************************************
const int firmware_version = 22;   //MAJOR.MINOR more info on: http://semver.org  Version as integer to simplify sending OSC report
#define SERIAL_SPEED 115200       // 9600 for BLE friend
#define SERIAL_DEBUG true       //coment to turn the serial debuging off
#define SERIAL_PLOTTER false     // for isolating Arduino IDE serial ploter
// #define STOPWATCH               //run stopwatch to measure timing in code
#define HOSTNAME "monitor"      // something like: monitor211, to ping or upload firmware over OTA use monitor211.local
//#define NEOPIXEL
#define ONBOARDLED              //ESP build in blue led
//******************************************************************************

extern "C"{
 #include "user_interface.h"    //NOTE needed for esp_system_info Since the include file from SDK is a plain C not a C++
}
#include <ESP8266WiFi.h>
#include "credentials.h"  //ignored by git to keep the network details private, add lines below into the file
                          // const char* ssid = "your-network-name";
                          // const char* password = "password";
#include <WiFiUdp.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
//   |--------------|-------|---------------|--|--|--|--|--|
//   ^              ^       ^               ^     ^
//   Sketch    OTA update   File system   EEPROM  WiFi config (SDK)

// -------------------- OSC libraries ------------------------------------------
#include <OSCMessage.h>       // https://github.com/CNMAT/OSC.git
#include <OSCBundle.h>
#include <OSCData.h>
// OSC IP and port settings in credentials.h
WiFiUDP Udp;
OSCErrorCode error;

int report_interval = 3000;      //OSC report inerval 3 secs
int measurment_interval = 50;       //AD measurment inerval
unsigned long previousMillisReport = 0;
unsigned long previousMillisMeasurment = 0;
unsigned long currentMillisReport, currentMillisMeasurment, runningTime;

#if SERIAL_PLOTTER == true
  bool serialPlotterEnable = 1;
  #else
  bool serialPlotterEnable = 0;
#endif

#ifdef STOPWATCH
 unsigned long timingMillisReference, timingMillisRuning;
#endif

#ifdef NEOPIXEL
  #include <FastLED.h>
  #define NUM_LEDS 1
  #define DATA_PIN D1 //   pin for neopixel
  CRGB leds[NUM_LEDS];
#endif
#ifdef ONBOARDLED
  #define BUILD_IN_LED 02
#endif

#include <Wire.h>
#include <Adafruit_ADS1015.h>
// Adafruit_ADS1115 ads;  // Use this for the 16-bit version - first prototype made for workshop with GSR and neopixel
Adafruit_ADS1015 ads;     // Use thi for the 12-bit version -  units 203 and 204

// static uint16_t readRegister(uint8_t i2cAddress, uint8_t reg) {
//   Wire.beginTransmission(i2cAddress);
//   Wire.write((uint8_t)ADS1015_REG_POINTER_CONVERT);
//   Wire.endTransmission();
//   Wire.requestFrom(i2cAddress, (uint8_t)2);
//   return ((Wire.read() << 8) | Wire.read());
// }

// normalize function variables
long sliding_min = -1;
long sliding_max = -1;
unsigned int step = 1;

//OSC message headers
char osc_header_report[8];
char osc_header_hr[8];
char osc_header_gsr[10];

bool led_status = 1;   // 1 led OFF, 0 led ON
int destination = 255; //last octet of IP OSC destination machine. 255=broadcast default
IPAddress remoteIP;    //dynamically created based on device IP + destination
int unit_ID;

int i2c_adc_address = 0x48;         //0x48 (1001000) ADR -> GND
                                    // 0x49 (1001001) ADR -> VDD
                                    // 0x4A (1001010) ADR -> SDA
                                    // 0x4B (1001011) ADR -> SCL
bool external_adac_present = 0;

void setup()
{
#ifdef SERIAL_DEBUG
  Serial.begin(SERIAL_SPEED);
#endif

#ifdef STOPWATCH
  #ifdef SERIAL_DEBUG
    Serial.println(); Serial.println();
    Serial.print("\r\n- entering setup, reseting stopwatch");
  #endif
  timingMillisReference = millis(); //reset stopwatch
#endif

#ifdef SERIAL_DEBUG
  Serial.println("\r\n--------------------------------");        // compiling info
  Serial.print("HR&GSR Ver: "); Serial.println(firmware_version);
  Serial.println("by Grzegorz Zajac and Nathan Andrew Fain");
  Serial.println("Compiled: " __DATE__ ", " __TIME__ ", " __VERSION__);
  Serial.println("---------------------------------");
  Serial.println("ESP Info: ");
  Serial.print( F("Heap: ") ); Serial.println(system_get_free_heap_size());
  Serial.print( F("Boot Vers: ") ); Serial.println(system_get_boot_version());
  Serial.print( F("CPU: ") ); Serial.println(system_get_cpu_freq());
  Serial.print( F("SDK: ") ); Serial.println(system_get_sdk_version());
  Serial.print( F("Chip ID: ") ); Serial.println(system_get_chip_id());
  Serial.print( F("Flash ID: ") ); Serial.println(spi_flash_get_id());
  Serial.print( F("Flash Size: ") ); Serial.println(ESP.getFlashChipRealSize());
  Serial.printf("Sketch size: %u\n", ESP.getSketchSize());
  Serial.printf("Free size: %u\n", ESP.getFreeSketchSpace());
  Serial.print( F("Vcc: ") ); Serial.println(ESP.getVcc());
  Serial.println();
#endif

#ifdef NEOPIXEL     // initialize neopixel
  FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.showColor(CHSV(HUE_GREEN, 255, 100));
#endif

#ifdef ONBOARDLED
  pinMode(BUILD_IN_LED, OUTPUT);
  digitalWrite(BUILD_IN_LED,led_status); //off by default
#endif

// external ADC presence test
Wire.begin();
Wire.beginTransmission(i2c_adc_address);
if (Wire.endTransmission() == 0) {external_adac_present = 1;}

#ifdef SERIAL_DEBUG
  Serial.print("External ADAC: "); Serial.println(external_adac_present);
#endif

//---------------------------- WiFi --------------------------------------------
WiFi.mode(WIFI_STA);  // https://www.arduino.cc/en/Reference/WiFiConfig
// WiFi.config(ip, gateway, subnet);  //uncomment for fixed ip address, needs to be defined in credentials.h file

#ifdef SERIAL_DEBUG
  Serial.print("unit MAC address: "); Serial.println(WiFi.macAddress());
  Serial.print("Connecting to ");   Serial.print(ssid); Serial.println(" with DHCP");
#endif

WiFi.begin(ssid, password);                                                     //TODO check reconnecting when lost wifi

while (WiFi.waitForConnectResult() != WL_CONNECTED) {
#ifdef SERIAL_DEBUG
  Serial.println("Connection Failed! Rebooting...");
#endif
  delay(8000);
  ESP.restart();
}

#ifdef SERIAL_DEBUG
  Serial.print("assigned IP address: "); Serial.println(WiFi.localIP());
#endif

// remoteIP = localIP + the destination var as last octet
remoteIP    = WiFi.localIP();
remoteIP[3] = destination;
#ifdef SERIAL_DEBUG
  Serial.print("remote IP address default: "); Serial.println(remoteIP);
#endif

// ------------------------- OSC headers ---------------------------------------
 unit_ID = WiFi.localIP()[3];
osc_header_report[0] = {0};  //reset buffor, start with a null string
snprintf(osc_header_report, 8,"/%d", unit_ID);
osc_header_hr[0] = {0}; //reset buffor, start with a null string
snprintf(osc_header_hr, 8, "/%d/hr", unit_ID);

if (external_adac_present){
  osc_header_gsr[0] = {0}; //reset buffor, start with a null string
  snprintf(osc_header_gsr, 10, "/%d/gsr", unit_ID);
}

#ifdef SERIAL_DEBUG
  Serial.print("osc report header: "); Serial.println(osc_header_report);
  Serial.print("osc hr header: "); Serial.println(osc_header_hr);
  if (external_adac_present){
    Serial.print("osc gsr header: "); Serial.println(osc_header_gsr);
  }
#endif

// --------------------------- OTA ---------------------------------------------
char buf[30]; buf[0] = {0};
snprintf(buf, 30, "%s%i", HOSTNAME, unit_ID);
ArduinoOTA.setHostname(buf);
#ifdef SERIAL_DEBUG
  Serial.print("Hostname: "); Serial.println(buf);
#endif

ArduinoOTA.setPort(8080);

ArduinoOTA.onStart([]() {
  #ifdef SERIAL_DEBUG
    Serial.println("Start updating ");
  #endif
});

ArduinoOTA.onEnd([]() {
  #ifdef SERIAL_DEBUG
    Serial.println("\nEnd");
  #endif
  #ifdef NEOPIXEL
    FastLED.showColor(CHSV(HUE_ORANGE, 255, 200));
  #endif
});
ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
  #ifdef SERIAL_DEBUG
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  #endif
  #ifdef NEOPIXEL   //ota uploading indicator
    FastLED.showColor(CHSV(HUE_BLUE, 255, 100));
    delay(1);
    FastLED.showColor(CHSV(HUE_BLUE, 255, 0));
  #endif
  #ifdef ONBOARDLED
    digitalWrite(BUILD_IN_LED, !led_status); delay(1); digitalWrite(BUILD_IN_LED, led_status);  //flash when uploading
  #endif
});
ArduinoOTA.onError([](ota_error_t error) {
  #ifdef SERIAL_DEBUG
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed. Firewall Issue ?");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  #endif
});
ArduinoOTA.begin();
Udp.begin(localPort);

//-------------------------- External ADc --------------------------------------
if(external_adac_present){
  #ifdef SERIAL_DEBUG
    Serial.println("Getting single-ended readings from AIN0..3");
    Serial.println("ADC Range: +/- 6.144V (1 bit = 3mV/ADS1015, 0.1875mV/ADS1115)");
  #endif
  ads.setGain(GAIN_TWOTHIRDS);  // 2/3x gain +/- 6.144V  1 bit = 3mV      0.1875mV (default)
  ads.begin();
}

// Serial.print("ADS? ");
// uint32_t reg = readRegister(ADS1015_ADDRESS, ADS1015_REG_POINTER_CONFIG);
// #if ARDUINO >= 100
// Serial.println(reg);
// #endif

#ifdef NEOPIXEL
  FastLED.showColor(CHSV(HUE_GREEN, 255, 100));
  delay(500);
  FastLED.showColor(CHSV(HUE_GREEN, 255, 0));
#endif

#ifdef ONBOARDLED    //turn led on if connected
  led_status = 0;
  digitalWrite(BUILD_IN_LED, led_status);
#endif

#ifdef STOPWATCH
  timingMillisRuning = millis() - timingMillisReference;
  #ifdef SERIAL_DEBUG
    Serial.print("- setup time: "); Serial.println(timingMillisRuning);
  #endif
#endif
}

void loop() {
  #ifdef STOPWATCH
    #ifdef SERIAL_DEBUG
      Serial.println("\r\n- starting loop");
    #endif
    timingMillisReference = millis(); //reset stopwatch
  #endif

  ArduinoOTA.handle();

  #ifdef STOPWATCH
    timingMillisRuning = millis() - timingMillisReference;
    #ifdef SERIAL_DEBUG
      Serial.print("  OTA fn time: "); Serial.println(timingMillisRuning);
    #endif
  #endif
  #ifdef STOPWATCH
    timingMillisReference = millis(); //reset stopwatch
  #endif

  OSCMsgReceive();

  #ifdef STOPWATCH
    timingMillisRuning = millis() - timingMillisReference;
    #ifdef SERIAL_DEBUG
      Serial.print("  OSC receive fn time: "); Serial.println(timingMillisRuning);
    #endif
  #endif

  currentMillisMeasurment = millis();
  if (currentMillisMeasurment - previousMillisMeasurment >= (measurment_interval)) {
    previousMillisMeasurment = currentMillisMeasurment;
    AD2OSC();
  }

  if (report_interval > 0){
    currentMillisReport = millis();
    if (currentMillisReport - previousMillisReport >= (report_interval)) {
      previousMillisReport = currentMillisReport;
      sendReport();
    }
  }
}

void AD2OSC(){
  #ifdef STOPWATCH
    #ifdef SERIAL_DEBUG
      Serial.println("\r\n   starting AD2OSC");
    #endif
    timingMillisReference = millis(); //reset stopwatch
  #endif

  int adc_int;       //internal ESP ADC
  adc_int = analogRead(A0);

  #ifdef STOPWATCH
    timingMillisRuning = millis() - timingMillisReference;
    #ifdef SERIAL_DEBUG
      Serial.print("   AD sampling time: "); Serial.println(timingMillisRuning);
    #endif
  #endif

  #ifdef STOPWATCH
    timingMillisReference = millis(); //reset stopwatch
  #endif

  adc_int = normalize(0, 1024, adc_int);

  #ifdef STOPWATCH
    timingMillisRuning = millis() - timingMillisReference;
    #ifdef SERIAL_DEBUG
      Serial.print("   normalizing time: "); Serial.println(timingMillisRuning);
    #endif
  #endif

  #ifdef STOPWATCH
    timingMillisReference = millis(); //reset stopwatch
  #endif

  if (serialPlotterEnable && Serial) {
      Serial.print(adc_int); Serial.print(",");
      Serial.print(sliding_min); Serial.print(",");
      Serial.println(sliding_max);
  }

  #ifdef STOPWATCH
    timingMillisRuning = millis() - timingMillisReference;
    #ifdef SERIAL_DEBUG
      Serial.print("   serial2ploter time: "); Serial.println(timingMillisRuning);
    #endif
  #endif

  #ifdef STOPWATCH
    timingMillisReference = millis(); //reset stopwatch
  #endif
  OSCMessage voltage_hr(osc_header_hr);
  voltage_hr.add(adc_int);
  Udp.beginPacket(remoteIP, destPort);
  voltage_hr.send(Udp);
  Udp.endPacket();
  voltage_hr.empty();

  if(external_adac_present){
    int adc_ext;    //external ADC
    // adc_ext = map(ads.readADC_SingleEnded(0), 0, 4096, 0, 1024);             //map full ADC range (12bit - 4096) to 0-1024
    adc_ext = ads.readADC_SingleEnded(0);                                       //GSR is giving 1200 max without maping
    OSCMessage voltage_gsr(osc_header_gsr);
    voltage_gsr.add(adc_ext);
    Udp.beginPacket(remoteIP, destPort);
    voltage_gsr.send(Udp);
    Udp.endPacket();
    voltage_gsr.empty();
  }

  #ifdef STOPWATCH
    timingMillisRuning = millis() - timingMillisReference;
    #ifdef SERIAL_DEBUG
      Serial.print("   osc send time: "); Serial.println(timingMillisRuning);
    #endif
  #endif
}

unsigned long normalize(unsigned long value_min, unsigned long value_max, unsigned long value) {
  // ghetto callibration
  // think of the sliding_min and sliding_max as two walls that are always closing in creating
  // and more and more narrow hallway for the incoming value to fit in. when the value breaches
  // those walls they are expanded by `step`. when the value is below those walls they are
  // further enclosed by step

  // first run (when sliding_min == -1):
  if (sliding_min < 0) sliding_min = value_min;
  // value is lower than expected, make floor this value:
  else if (value < sliding_min) sliding_min = value;
  // value is above floor, slowly move floor up:
  else if (value > sliding_min && sliding_min+step < sliding_max) sliding_min += step;

  if (sliding_max < 0) sliding_max = value_max;
  else if (value > sliding_max) sliding_max = value;
  else if (value < sliding_max && sliding_max-step > sliding_min) sliding_max -= step;

  unsigned long output;
  // this calculation assumes we want a return value between 0 and 1000. We scale using value so as to avoid any need for float point
  output = (value-sliding_min)*1000/(sliding_max-sliding_min);

  // if you do not want to use the sliding window calibration method then uncomment the following
  //output = (value*1000)/(value_max-value_min);

  return output;
}

void led_fn(OSCMessage &msg) {
  led_status = msg.getInt(0);
  #ifdef ONBOARDLED
    digitalWrite(BUILD_IN_LED, led_status);
  #endif
}

void measurment_interval_fn(OSCMessage &msg){
  measurment_interval = msg.getInt(0);
  #ifdef SERIAL_DEBUG
    Serial.print("updated measurment interval to "); Serial.println(measurment_interval);
  #endif
}

void report_interval_fn(OSCMessage &msg){
  report_interval = msg.getInt(0);
  #ifdef SERIAL_DEBUG
    Serial.print("updated report interval to "); Serial.println(report_interval);
  #endif
}

void osc_destination_fn(OSCMessage &msg){
  destination = msg.getInt(0);
  remoteIP[3] = destination;
  #ifdef SERIAL_DEBUG
    Serial.print("updated destination to "); Serial.println(destination);
  #endif
}

void serial_plot_fn(OSCMessage &msg){
  int onoff = msg.getInt(0);
  if (onoff != 1) {
    // == 0  disable Serial port and exit
    serialPlotterEnable = 0;
    Serial.end();
    return;
  }
  if (!Serial)
    Serial.begin(SERIAL_SPEED);
  serialPlotterEnable = 1;
}

void OSCMsgReceive(){
  OSCMessage msgIN;
  int size;
  if((size = Udp.parsePacket())>0){
    while(size--)
      msgIN.fill(Udp.read());
        if(!msgIN.hasError()){
          msgIN.dispatch("/led", led_fn);
          msgIN.dispatch("/interval", measurment_interval_fn);
          msgIN.dispatch("/report", report_interval_fn);
          msgIN.dispatch("/destination", osc_destination_fn);
          msgIN.dispatch("/serialplot", serial_plot_fn);
        } else {
          error = msgIN.getError();
          #ifdef SERIAL_DEBUG
            Serial.print("error: ");
            Serial.println(error);
          #endif
          }
  }
}

void sendOSCmessage(char* name, int value){
  char message_osc_header[16];
  message_osc_header[0] = {0};
  strcat(message_osc_header, osc_header_report);
  strcat(message_osc_header, name);
  OSCMessage message(message_osc_header);
  message.add(value);
  Udp.beginPacket(remoteIP, destPort);
  message.send(Udp);
  Udp.endPacket();
  message.empty();
}

void sendReport(){
  #ifdef STOPWATCH
    #ifdef SERIAL_DEBUG
      Serial.println("\r\n   starting report fn");
    #endif
    timingMillisReference = millis(); //reset stopwatch
  #endif

  sendOSCmessage("/ver", firmware_version);
  sendOSCmessage("/rssi", WiFi.RSSI());
  sendOSCmessage("/channel", WiFi.channel());
  sendOSCmessage("/time", (millis()/1000));               //running time in secs
  sendOSCmessage("/plotter", serialPlotterEnable);
  sendOSCmessage("/ext_adac", external_adac_present);
  sendOSCmessage("/led", led_status);
  sendOSCmessage("/report", report_interval); delay(1);    //TODO investigate why next couple of messages are droped without the delay
  sendOSCmessage("/interval", measurment_interval);        //TODO seems to does not send, to check!
  sendOSCmessage("/destination", destination);

  #ifdef STOPWATCH
    timingMillisRuning = millis() - timingMillisReference;
    #ifdef SERIAL_DEBUG
      Serial.print("   osc report time: "); Serial.println(timingMillisRuning);
    #endif
  #endif
}
