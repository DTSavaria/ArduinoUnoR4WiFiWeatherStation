/*

    Arduino UNO R4 Wifi Weather Station

    This sketch for the Arduino Uno R4 Wifi downloads weather data and displays
    it on the LED Matrix.

    If a Modulino Thermo is connected it will also show the measured temperature.

    Copyright (c) 2025 Daniel Savaria
    https://github.com/DTSavaria
*/

#include <WiFiS3.h>
#include <ArduinoGraphics.h>
#include <Arduino_LED_Matrix.h>
#include <TextAnimation.h>
#include <NwsWeatherData.hpp>

// define MAX_CHARS, this was taken from Arudino examples
#define MAX_CHARS 100
TEXT_ANIMATION_DEFINE(anim, MAX_CHARS)

/*
  This library makes it easier to scroll messages that are longer than is
  allowed by MAX_CHARS
*/
#include <AsyncScrollingMessage.hpp>

////// vv CUSTOMIZE OPTIONS vv //////

// 1.  Open this file and set your wifi network name and password
#include "arduino_secrets.h"
const String WIFI_SSID = SECRET_SSID;
const String WIFI_PASS = SECRET_PASS;

/*
    2. Set the your location as latitude and longitude. You can look it up on
    a map. Use decimal degrees. Don't use minutes and seconds. Don't use
    N/S/E/W. Instead N and W should be positive and S and E should be negative.
*/
const String LAT = "41.8292";
const String LON = "-71.4132";

// 3. set to true for celsius, or false for fahrenheit
const bool SHOW_CELSIUS = false;
const TemperatureUnit OUT = (SHOW_CELSIUS)
                              ? TemperatureUnit::CELSIUS
                              : TemperatureUnit::FAHRENHEIT;
const String UNITS = (SHOW_CELSIUS) ? "°C" : "°F";

// 4. set to true for shorter messges
const bool SHORT_TEXT = false;
const String INDOOR_MESSAGE = (SHORT_TEXT)
                                ? "  Indoor:"
                                : "  Indoor Temp " + UNITS + ":";
const String NOW_MESSAGE = (SHORT_TEXT)
                             ? "  Outdoor:"
                             : "  Outdoor Temp " + UNITS + ":";
const String HIGH_MESSAGE = (SHORT_TEXT)
                              ? "  High:"
                              : "  Day's High " + UNITS + ":";
const String LOW_MESSAGE = (SHORT_TEXT)
                             ? "  Low:"
                             : "  Night's Low " + UNITS + ":";

/*
    5. adjust up or down to show values longer or shorter on the led matrix length
    in milliseconds that values are shown
    2 * 1000 is 2 seconds
*/
const unsigned long DISPLAY_DELAY = 2 * 1000;

/*
    6. adjust up or down to refresh the data less or more often length in
    milliseconds between data download
    5 * 60 * 1000 is 5 minutes
*/
const unsigned long DOWNLOAD_INTERVAL = 5 * 60 * 1000;

/*
    7. Modulino
    If you have a Thermo Modulino and plug it in, its reading will be displayed
    as the indoor temperature. Everything will still work even if it's not
    connected, but if you're not using it and don't want to download the
    Modulino library, just comment ou the following line
*/
#define COMPILIE_MODULINO
#ifdef COMPILIE_MODULINO
#include <Modulino.h>
ModulinoThermo thermo;
#endif

////// ^^ CUSTOMIZE OPTIONS ^^ //////

// used to display data on the UNO R4 LED Matrix
ArduinoLEDMatrix matrix;

// used to connect and download data with WiFi
WiFiSSLClient wifi;

// used to download weather data from the National Weather Service
NwsWeatherData* weatherData;

// a small struct used to hold a temperature value with a pointer to the next
struct ValueNode {
  int value;        // the temperature value to display if valid
  bool hasValue;    // true if there is a valid value to display
  ValueNode* next;  // pointer to the next value
};

ValueNode* firstValue;          // first value temperature value to show
ValueNode* lastValue;           // the last temperature value to show
ValueNode* currentValue;        // the temperature value currently showing
unsigned long endValueDisplay;  //the time to stop showing the current value

// hold on to a instance of the indoor temp value so it can be updated often
ValueNode indoorValue{ .value = 0, .hasValue = true, .next = nullptr };
bool addThermoMessage = false;  // true if the indoor temp is available to show

AsyncScrollingMessage* firstMessage;    // the first scrolling message
AsyncScrollingMessage* lastMessage;     // the last scrollling message
AsyncScrollingMessage* currentMessage;  //the current message to show


// An enum to track the state of the program
enum class State {
  START_SCROLL,  // the next action is to start scrolling a message
  WAIT_SCROLL,   // waiting for a message to stop scrolling
  START_VALUE,   // the next action is to show a temperature value
  WAIT_VALUE     // waiting for the value to stop showing
};
State state;

/*
    The setup section to prepare the application.
*/
void setup() {
  Serial.begin(9600);  //serial connection for debug messages

  // initialize the led matrix
  int okay = matrix.begin();
  matrix.beginDraw();
  matrix.textScrollSpeed(40);
  matrix.setCallback(matrixCallback);

  delay(100);
  Serial.println();
  Serial.println("Starting");

  // print the settings to the serial connection
  Serial.println("Settings:");
  Serial.println("  Location: (" + LAT + ", " + LON + ")");
  Serial.print("  Wifi Network: ");
  Serial.println(SECRET_SSID);
  Serial.println("  Units: " + UNITS);
  Serial.print("  Temperature display length (ms): ");
  Serial.println(DISPLAY_DELAY);
  Serial.print("  Time between data download refresh (ms): ");
  Serial.println(DOWNLOAD_INTERVAL);

  if (SHORT_TEXT) {
    Serial.println("  Short messages");
  } else {
    Serial.println("  Normal messages");
  }

  if (!okay) {
    Serial.println("Could not initialize display");
    while (true)
      ;
  }

//initialize the Modulino Thermo
#ifdef COMPILIE_MODULINO
  Modulino.begin();
  if (!thermo.begin()) {
    Serial.println("Modulino Thermo not found");
  }
#endif

  // measure indoor temp, connect to wifi and download weather data
  checkIndoorTemp();
  connectToWifi();
  weatherData = new NwsWeatherData(wifi, LAT, LON);
  downloadData();
}

/*
    The loop. Get the indoor temp. Download forecast if it is time to. Display
    the next message or temperature value.
*/
void loop() {
  unsigned long currentTime = millis();

  if (state == State::START_SCROLL) {
    // this state means it's time to start scrolling the next message

    /*
        if it's the first message, then grab the indoor temp, download forecast
        if it's past the download interval
    */
    if (currentMessage == firstMessage || currentMessage == nullptr) {
      checkIndoorTemp();  // get indoor temp from Modulino

      if (currentTime - weatherData->getLastDownloadTime()
          > DOWNLOAD_INTERVAL) {
        downloadData();  // download forecast
      } else {
        Serial.println("using cached data");
      }
    }

    // if the current message is a nullptr, then go back to teh first message
    if (currentMessage == nullptr) {
      // if the first message is also nullptr, then there's nothing to show
      if (firstMessage == nullptr) {
        displayMarquee("  No data to display.");
        return;
      }
      currentMessage = firstMessage;
      currentValue = firstValue;
    }

    // start scrolling the current message and update state to wait
    state = State::WAIT_SCROLL;
    currentMessage->showMessageAndContinuations(matrixCallback);
  } else if (state == State::START_VALUE) {
    // this state means it's time to show the temperature value

    /*
        if the node has no value (some messages don't have a tempature),
        then move on to the next message and set state to scroll that message
    */
    if (!currentValue->hasValue) {
      state = State::START_SCROLL;
      currentMessage = currentMessage->getNextAfterContinuations();
      currentValue = currentValue->next;
      return;
    }

    // if the node does have a value, then show it and update state to wait
    state = State::WAIT_VALUE;
    endValueDisplay = currentTime + DISPLAY_DELAY;
    displayValue(currentValue->value);
  } else if (state == State::WAIT_VALUE) {
    // this state means it's a tempature value is displaying
    if (currentTime > endValueDisplay) {
      /*
          if it's displayed long enough, then move to the next message and set
          state to scroll it
      */
      state = State::START_SCROLL;
      currentMessage = currentMessage->getNextAfterContinuations();
      currentValue = currentValue->next;
    }
  }
}

/*
    If it is connected, get the temperature from the Modulino and store it in
    the indoor value node.
*/
void checkIndoorTemp() {
#ifdef COMPILIE_MODULINO
  if (thermo) {
    addThermoMessage = true;
    indoorValue.value = NwsWeatherData::convertTemperature(
      thermo.getTemperature(), TemperatureUnit::CELSIUS, OUT);
    return;
  }
#endif
  addThermoMessage = false;
}

/*
    Download the forecast from the Internet and create all the messages to
    display it on the LED Matrix
*/
void downloadData() {
  Serial.println("Downloading new data");
  weatherData->downloadNewData();

  // check if it downloaded okay
  if (!weatherData->hasValidData()) {
    Serial.println("Could not download weather data");
    while (true) {
      displayMarquee("   ERROR Could not download weather data. Press reset");
    }
  }

  // free up memory of the value nodes
  while (firstValue != nullptr) {
    ValueNode* n = firstValue->next;

    // but don't delete the indoor value, that one always exists
    if (firstValue != &indoorValue) {
      delete firstValue;
    }

    firstValue = n;
  }

  lastValue = nullptr;
  currentValue = nullptr;

  // free up the memory of the old messages
  if (firstMessage != nullptr) {
    AsyncScrollingMessage::deleteMessageAndContinuationsAndFollowingMessages(
      firstMessage);
  }

  firstMessage = nullptr;
  lastMessage = nullptr;
  currentMessage = nullptr;

  // get the units for the downloaded temperatures
  const TemperatureUnit IN = weatherData->getTemperatureUnit();

  // if there are any hazards, make messages for them
  size_t hazardCount = weatherData->getHazardCount();
  for (size_t index = 0; index < hazardCount; index++) {
    addMessage(
      generateMessage("  " + weatherData->getHazard(index)), false, 0);
  }

  // create a message with the forecast of the current time period
  addMessage(
    generateMessage(
      "  " + weatherData->getCurrentPeriodName() + ": "
      + weatherData->getCurrentPeriodWeather() + "  "),
    false, 0);

  // if using the Modulino, create a message and value of the indoor temperature
  if (addThermoMessage) {
    addMessage(generateMessage(INDOOR_MESSAGE), &indoorValue);
  }

  // create a message and value of the current outdoor temperature
  addMessage(
    generateMessage(NOW_MESSAGE),
    true, weatherData->getStationTemperature(OUT));

  // if it daytime, create a message and value of today's high temperature
  double high = weatherData->getTodaysHighTemperature(OUT);
  if (!isnan(high)) {
    addMessage(generateMessage(HIGH_MESSAGE), true, high);
  }

  // create a message and value of tonight's low temperature
  double low = weatherData->getTonightsLowTemperature(OUT);
  addMessage(
    generateMessage(LOW_MESSAGE), true, low);

  // set the state to start scrolling the first message
  state = State::START_SCROLL;
  currentValue = firstValue;
  currentMessage = firstMessage;
}
/*
    Create a scrolling message with the given string
*/
AsyncScrollingMessage* generateMessage(const String& message) {
  return AsyncScrollingMessage::generateMessages(
    message, matrix, MAX_CHARS, Font_4x6);
}

/*
    Add a message to the list of messages to scroll. If the message has an
    associated temperature value, then hasValue should be true. In this case
    the temperature value will be shown after the message scrolls by.
*/
void addMessage(
  AsyncScrollingMessage* message,
  bool hasValue,
  int value) {

  //create a value node and call addMessage with the message and node
  ValueNode* valueNode = new ValueNode();
  valueNode->hasValue = hasValue;
  valueNode->value = value;
  addMessage(message, valueNode);
}

/*
    Add a message to the list of messages to scroll. If the message has an
    associated temperature value, then hasValue of valueNode should be true.
    In this case the temperature value will be shown after the message scrolls
    by.
*/
void addMessage(AsyncScrollingMessage* message, ValueNode* valueNode) {

  /*
      if the first value is nullptr, this becomes the first. otherwise add it 
      to the end of the list
  */
  if (firstValue == nullptr) {
    firstValue = valueNode;
    lastValue = firstValue;
  } else {
    lastValue->next = valueNode;
    lastValue = valueNode;
  }

  /*
      if the first message is nullptr, this becomes the first. otherwise add
      it to the end of the list
  */
  if (firstMessage == nullptr) {
    firstMessage = message;
    lastMessage = firstMessage;
  } else {
    lastMessage->insertNext(message);
    lastMessage = message;
  }
}

/*
    Connect to the wifi and display a message if it failed.
*/
void connectToWifi() {
  // check for the WiFi module:
  if (WiFi.status() == WL_NO_MODULE) {
    // don't continue
    while (true) {
      displayMarquee("  Communication with WiFi module failed!");
    }
  }

  String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION) {
    displayMarquee("   Please upgrade the WiFi firmware");
  }

  int status = WL_IDLE_STATUS;
  while (status != WL_CONNECTED) {
    Serial.println("Attempting to connect to: " + WIFI_SSID);

    status = WiFi.begin(WIFI_SSID.c_str(), WIFI_PASS.c_str());
    Serial.print("Status code: ");
    Serial.println(status);

    if (status == WL_CONNECT_FAILED) {
      while (true) {
        displayMarquee("   WiFi connection failed. Check SSID/password");
      }
    }

    if (WiFi.localIP() == "0.0.0.0") {
      status = WL_IDLE_STATUS;
    }
  }

  // everything worked, print the status to Serial

  Serial.print("Connected to: ");
  Serial.println(WiFi.SSID());

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void matrixCallback() {
  state = State::START_VALUE;
}

/*
  This displays a temperature value on the LED matrix. 
*/
void displayValue(int value) {
  // when showing a non-async-scrolling message, the callback must be cleared
  matrix.setCallback(nullptr);

  // basic code to draw some characters to the LED Matrix
  matrix.beginDraw();
  matrix.clear();
  matrix.stroke(0xFFFFFFFF);
  // if it's only 2 digits, use the bigger font
  if (value < 100 && value > -10) {
    matrix.textFont(Font_5x7);
  } else {
    matrix.textFont(Font_4x6);
  }
  matrix.beginText(1, 1, 0xFFFFFF);
  matrix.print(value);
  matrix.endText();
  matrix.endDraw();
}

/*
    Disply a simple scrolling message. This is mainly used for error or other
    status messages. For the weather station messages, the AsyncScrollingMessage
    class is used because it has more features.
*/
void displayMarquee(const String& message) {
  // when showing a non-async-scrolling message, the callback must be cleared
  matrix.setCallback(nullptr);

  matrix.beginDraw();
  matrix.clear();
  matrix.stroke(0xFFFFFFFF);
  matrix.textFont(Font_4x6);
  matrix.beginText(0, 1, 0xFFFFFF);
  matrix.println(message);
  matrix.endText(SCROLL_LEFT);
  matrix.endDraw();
}
