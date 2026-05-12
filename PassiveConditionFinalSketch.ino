/*
Sketch for Passive Building Conditioning Controls.
Tells user when to open/close shading and windows using an SHT85 sensor, PHFS-01 Sensor,
NWS API calls, and a local wifi accessed server. Intended for use on ESP32_DevKitc_V4.

Once sketch is uploaded to ESP32, retrieve IP address from serial monitor and search it
into any web browser and device of your choosing on the same Wi-Fi network to view inputs and outputs.

By: Robert Jordan and Shamik Chatterjee

Library Credits:
AsyncTCP.h: ESP32Async
ESP8266WiFi.h: Ivan Grokhotkov
ESPAsyncTCP.h: ESP32Async
ESPAsyncWebServer.h: ESP32Async
ArduinoJson.h: Benoit Blanchon
SHTSensor.h: Johannes Winkelmann, Andreas Brauchli

Tutorial Credits:
RandomNerdTutorials.com (Sara Santos & Rui Santos)
*/

//Libraries
//For Web server:
#include <Arduino.h>
#ifdef ESP32
  #include <WiFi.h>
  #include <AsyncTCP.h>
#else
  #include <ESP8266WiFi.h>
  #include <ESPAsyncTCP.h>
  #include <Hash.h>
#endif
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
//For SHT85 Sensor:
#include <Wire.h>
#include "SHTSensor.h"
//For Heat Flux sensor PHFS-01
//#include <driver/adc.h>
//#include "esp_adc_cal.h"
//For API Calls
#include <HTTPClient.h>
#include <ArduinoJson.h>
//Other
#include "secrets.h"
#include <utility>

#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define HEAT_FLUX_PIN 34 //GPIO pin of the heat flux sensor on ESP32
#define WINDOW_LED_PIN 4 //GPIO pin of the window LED indicator
#define SHADES_LED_PIN 2 //GPIO pin of the shading LED indicator
#define I2C_SDA_PIN 21 //GPIO pin of SDA on ESP32 for SHT85 sensor use
#define I2C_SCL_PIN 22 //GPIO pin of SCL on ESP32 for SHT85 sensor use
#define DESIRED_TEMP_RANGE_VAR 5 //+- range of desired temperature
#define ADCRAW_TO_VOLTAGE 2450/3000 //2450mV gives a count of 3000. As defined in ESP32 datasheet

AsyncWebServer server(80); //initialize web server

//Variable initialization
bool window_open = false;   // Initialize window status. false = closed, true = open
bool shades_open = true;   // Shading status. False = close, true = open
float desired_temp;         // User desired temperature (°F)
float outsidetemp;          // Outdoor temperature
float insidetemp;           // Indoor temperature
float humidity;             // Humidity, not needed for this project, included because its built in to SHT85 Sensor
float heat_flux;            // Heat flux: positive = heat entering building, negative = heat leaving building
String location;            // User inputted location
const char* PARAM_LOCATION = "location"; // Parameter for use in the HTML web page
const char* PARAM_DESIREDTEMP = "desiredtemp"; // Parameter for use in the HTML web page
//Variables for SHT85 Sensor:
SHTSensor sht;

// HTML web page 
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
  <title>ESP Input Form</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv=\"refresh\" content=\"30\>
  <script>
    function submitMessage() {
      alert("Saved value to ESP LittleFS");
      setTimeout(function(){ document.location.reload(false); }, 500);   
    }
  </script></head><body>
  <h2>Should the window be open?: %windowopen%</h2>
  <h2>Should the shading be open?: %shadesopen%</h2>
  <form action="/get" target="hidden-form">
    <h3>Desired Temperature (current value %desiredtemp% &deg;F):</h3> <input type="number " name="desiredtemp">
    <input type="submit" value="Submit" onclick="submitMessage()">
  </form>
  Desired Temperature Format: Whole Number ... ex. 70<br>
  <form action="/get" target="hidden-form">
    <h3>Location (current value %location%):</h3> <input type="text" name="location">
    <input type="submit" value="Submit" onclick="submitMessage()">
  </form>
  Location Format: City, State Abbrev ... ex. Troy, NY
  <h3>Inside Temperature: %insidetemp% &deg;F</h3> 
  <h3>Outside Temperature: %outsidetemp% &deg;F</h3> 
  <h3>Heat Flux: %heatflux% W/m<sup>2</sup></h3> 
  <h3>Humidity: %humidity% %</h3>
  <iframe style="display:none" name="hidden-form"></iframe>
</body></html>)rawliteral";

//Rules Based Algorithn Functions
bool shading_state(float desired_max, float desired_min, float insidetemp, float heat_flux) {
  if (insidetemp > desired_max && heat_flux > 0){ //Rule 1
    return false;
  }
  else if (insidetemp > desired_max && heat_flux < 0){ //Rule 2
    return true;
  }
  else if (insidetemp < desired_min && heat_flux > 0){ //Rule 3
    return true;
  } 
  else if (insidetemp < desired_min && heat_flux < 0){ //Rule 4
    return false;
  }
  return true; //default
}

bool window_state(float desired_max, float desired_min, float insidetemp, float outsidetemp){
  if (outsidetemp > desired_max && insidetemp > desired_max){ //Rule 5
    return false;
  }
  else if (outsidetemp > desired_max && insidetemp < desired_min){ //Rule 6
    return true;
  }
  else if (outsidetemp < desired_min && insidetemp < desired_min){ //Rule 7
    return false;
  }
  else if (outsidetemp < desired_min && insidetemp > desired_max){ //Rule 8
    return true;
  }
  return false; //default
}

// Web Server Failure message
void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Not found");
}

// Write user input to the LittleFS file system
void writeFile(fs::FS &fs, const char * path, const char * message){
  Serial.printf("Writing file: %s\r\n", path);
  File file = fs.open(path, "w");
  if(!file){
    Serial.println("- failed to open file for writing");
    return;
  }
  if(file.print(message)){
    Serial.println("- file written");
  } else {
    Serial.println("- write failed");
  }
  file.close();
}

// read what value is currently stored for the user inputs
String readFile(fs::FS &fs, const char * path){
  Serial.printf("Reading file: %s\r\n", path);
  File file = fs.open(path, "r");
  if(!file || file.isDirectory()){
    Serial.println("- empty file or failed to open file");
    return String();
  }
  Serial.print("- read from file: ");
  String fileContent;
  while(file.available()){
    fileContent+=String((char)file.read());
  }
  file.close();
  Serial.println(fileContent);
  return fileContent;
}

// Replaces placeholder with stored values in HTML page
String processor(const String& var){
  //Serial.println(var);
  if(var == "location"){
    return readFile(LittleFS, "/location.txt");
  }
  else if(var == "desiredtemp"){
    return readFile(LittleFS, "/desiredtemp.txt");
  }
  else if(var == "insidetemp"){
    return String(insidetemp);
  }
  else if(var == "outsidetemp"){
    return String(outsidetemp);
  }
  else if(var == "heatflux"){
    return String(heat_flux);
  }
  else if(var == "humidity"){
    return String(humidity);
  }
  else if(var == "windowopen"){
    if (window_open){
      return "Yes";
    }
    else{
      return "No";
    }
  }
  else if(var == "shadesopen"){
    if (shades_open){
      return "Yes";
    }
    else{
      return "No";
    }
  }
  return String();
}

// Functions for Google Location API and NWS Weather API calls
std::pair<String, String> getCoords(HTTPClient &http, JsonDocument &doc, const String &geocodingUrl) {
  http.begin(geocodingUrl);
  int httpResponse = http.GET();

  if (httpResponse < 0) {
    Serial.println("Error fetching coordinates...");
    http.end();
    return std::pair<String, String>();
  }

  String geocodingPayload = http.getString();
  DeserializationError error = deserializeJson(doc, geocodingPayload);

  if (error) {
    Serial.println("Error in getting coordinates from HTTP response...");
    http.end();
    return std::pair<String, String>();
  }

  String latitude = doc["results"][0]["geometry"]["location"]["lat"];
  latitude.remove(latitude.length() - 3);
  String longitude = doc["results"][0]["geometry"]["location"]["lng"];
  longitude.remove(longitude.length() - 3);
  http.end();

  return std::make_pair(latitude, longitude);

}

String getStation(HTTPClient &http, JsonDocument &doc, const char* nwsLocatorUrl) {
  http.begin(nwsLocatorUrl);
  int httpResponse = http.GET();

  if (httpResponse < 0) {
    Serial.println("Error fetching station...");
    http.end();
    return String();
  }

  String locationPayload = http.getString();
  DeserializationError error = deserializeJson(doc, locationPayload);

  if (error) {
    Serial.println("Error in getting coordinates from HTTP response...");
    http.end();
    return String();
  }

  String location = doc["properties"]["forecast"];
  http.end();
  return location;
}

String getOutdoorTemp(HTTPClient &http, JsonDocument &doc, const char* forecastEndpoint) {
  http.begin(forecastEndpoint);
  int httpResponse = http.GET();

  if (httpResponse < 0) {
    Serial.println("Error fetching forecast...");
    http.end();
    return String();
  }

  String forecastPayload = http.getString();
  JsonDocument filter;
  filter["properties"]["periods"][0]["temperature"] = true;
  DeserializationError error = deserializeJson(doc, forecastPayload, DeserializationOption::Filter(filter));

  if (error) {
    Serial.println("Error in fetching outdoor temperature from forecast...");
    Serial.println(error.c_str());
    http.end();
    return String();
  }

  String temp = doc["properties"]["periods"][0]["temperature"];
  return temp;
}

void setup() {
  Serial.begin(115200); //Initialize Serial Monitor 
  
  WiFi.mode(WIFI_MODE_STA);
  
  delay(4000);
  WiFi.begin(ssid, password); // Begin Wifi using ssid and password configured in secrets.h

    while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print("Connecting to WiFi... ");
    Serial.print(ssid);
  }  

  Serial.println("... Connected to network!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Initialize SHT85 pins
  Wire.setPins(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.begin();
  if (sht.init()) {
    Serial.println("SHT85 sensor initialized successfully.");
  } else {
    Serial.println("Failed to initialize SHT85 sensor.");
  }

  //Initialize LED pins
  uint8_t windowled = WINDOW_LED_PIN;  //GPIO pin of window state led
  uint8_t shadeled = SHADES_LED_PIN;   //GPIO pin of shade state led
  pinMode(windowled , OUTPUT); 
  pinMode(shadeled , OUTPUT);

  //Configure ADC for Heat Flux Sensor Readings
  /*
  adc1_config_width(ADC_WIDTH_BIT_12); // Configure Width of ADC1
  adc1_config_channel_atten(ADC1_CHANNEL_6,ADC_ATTEN_DB_11); // Configure Attenuation on ADC1 Channel 6 (GPIO 34)
  */

  // Initialize LittleFS
  #ifdef ESP32
    if(!LittleFS.begin(true)){
      Serial.println("An Error has occurred while mounting LittleFS");
      return;
    }
    //Else littlfs worked *********************
  #else
    if(!LittleFS.begin()){
      Serial.println("An Error has occurred while mounting LittleFS");
      return;
    }
  #endif

  // Send web page with input fields to client
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", index_html, processor);
  });

  // Send a GET request to <ESP_IP>/get?location=<inputMessage>
  server.on("/get", HTTP_GET, [] (AsyncWebServerRequest *request) {
    String inputMessage;
    // GET location value on <ESP_IP>/get?location=<inputMessage>
    if (request->hasParam(PARAM_LOCATION)) {
      inputMessage = request->getParam(PARAM_LOCATION)->value();
      writeFile(LittleFS, "/location.txt", inputMessage.c_str());
    }
    // GET desiredtemp value on <ESP_IP>/get?idesiredtemp=<inputMessage>
    else if (request->hasParam(PARAM_DESIREDTEMP)) {
      inputMessage = request->getParam(PARAM_DESIREDTEMP)->value();
      writeFile(LittleFS, "/desiredtemp.txt", inputMessage.c_str());
    }
    else {
      inputMessage = "No message sent";
    }
    Serial.println(inputMessage);
    request->send(200, "text/text", inputMessage);
  });
  server.onNotFound(notFound);
  server.begin();
}

void loop() {
  // put your main code here, to run repeatedly:
  long api_call = 
  if (WiFi.status() == WL_CONNECTED) {

    HTTPClient http;
    JsonDocument doc;

    // Accessing values to be used in algorithm
    location = readFile(LittleFS, "/location.txt");
    desired_temp = readFile(LittleFS, "/desiredtemp.txt").toFloat();
    float desired_max = desired_temp + 5;
    float desired_min = desired_temp - 5;

    location.replace(" ", "+");
    String geocodingUrl = String("https://maps.googleapis.com/maps/api/geocode/json?address=") + location + String("&key=") + String(geocodingKey);

    auto coords = getCoords(http, doc, geocodingUrl);
    String latitude = coords.first;
    String longitude = coords.second;

    String nwsLocatorUrl = "https://api.weather.gov/points/" + latitude + "," + longitude;
    Serial.println(nwsLocatorUrl.c_str());
    String forecastEndpoint = getStation(http, doc, nwsLocatorUrl.c_str());
    
    if (sht.readSample()) { //Get Data from SHT85 Sensor, display error if failed
      insidetemp = sht.getTemperature();
      insidetemp = (insidetemp*(9/5)) + 32; // Converting from Celsius to Fahrenheit 
      humidity = sht.getHumidity();
    } else {
      Serial.println("Failed to read data from SHT85 sensor.");
    }
    outsidetemp = getOutdoorTemp(http, doc, forecastEndpoint.c_str()).toFloat();
    

    // Using ADC on ESP32 to heat raw Heat flux sensor circuit output voltage
    //uint32_t val = adc1_get_raw(ADC1_CHANNEL_6); //Get Raw ADC value 
    //uint32_t voltage = esp_adc_cal_raw_to_voltage(val, &adc_chars); //Convert the ADC value back into voltage
    //voltage = voltage - 50; // Remedy voltage reading offset by subtracting 50mV. (If needed)
    uint32_t val = analogRead(HEAT_FLUX_PIN);
    float voltage = val*ADCRAW_TO_VOLTAGE; // Converting raw val to voltage: voltage
    Serial.print("ADC value: " + String(val) + " Voltage: " + String(voltage)); //Printing ADC value and Voltage
    Serial.println();
    heat_flux = voltage; //Formula for converting the voltage into heat flux (if needed);

    // Run Rules Based algorithm to get ouput states
    shades_open = shading_state(desired_max, desired_min, insidetemp, heat_flux);
    window_open = window_state(desired_max, desired_min, insidetemp, outsidetemp);
    // Add compatibility for shades_open
    
    if (window_open){
      digitalWrite(WINDOW_LED_PIN, HIGH); // Activate Window State LED to show window should be OPEN
    }
    if (!window_open) {
      digitalWrite(WINDOW_LED_PIN, LOW);// Deactivate Window State LED
    }
    if (shades_open){
      digitalWrite(SHADES_LED_PIN, HIGH); // Activate Shades state LED to show window should be OPEN
    }
    if (!shades_open) {
      digitalWrite(SHADES_LED_PIN, LOW);// Deactivate Shades state LED
    }
    delay(10000); // delay to not loop too fast, 10000 = ~10 seconds

  } else {
    Serial.println("Reconnecting to WiFi...");
    WiFi.disconnect();
    WiFi.reconnect(); // Or WiFi.begin(ssid, password)
  }
  
}

