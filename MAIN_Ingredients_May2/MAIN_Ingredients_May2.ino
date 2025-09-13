#include <esp_now.h>
#include <WiFi.h>
#include <MFRC522.h>
#include <SPI.h>
#include <DYPlayerArduino.h> // Include the DY-HV20T library
#include <ArduinoOTA.h>      // Added OTA support

extern "C" {
  #include "esp_wifi.h"
}

/*****************************************************************
   ─── OPTIONAL DEBUG SWITCH ──────────────────────────────────
   Comment‑out   #define DEBUG
   or redefine it in “Build flags” to silence every Serial.print
*****************************************************************/
//#define DEBUG          // ← uncomment when you DO want Serial output

#ifdef DEBUG
  #define DPRINT(x)    Serial.print(x)
  #define DPRINTLN(x)  Serial.println(x)
#else
  // Replace the global ‘Serial’ object with a do‑nothing stub
  struct NullSerial_t {
      template<typename... T> void begin(T...)   {}
      template<typename... T> void print(T...)   {}
      template<typename... T> void println(T...) {}
      template<typename... T> void printf(T...)  {}
      template<typename... T> void write(T...)   {}
      template<typename... T> void flush(T...)   {}
  } __nullSerial;

  #define Serial   __nullSerial
  #define DPRINT(x)
  #define DPRINTLN(x)
#endif


// Constants for string lengths
#define RFID_LENGTH 20
#define INGREDIENT_LENGTH 30       // Increased from 20 to 30
#define REQUEST_TYPE_LENGTH 25     // Increased from 15 to 25
#define MAX_RECIPE_NAME_LENGTH 30  // Added to match server's struct

// Pin definitions
#define SS_PIN 21
#define RST_PIN 22

// Button pins
#define BUTTON_TOMATO 25
#define BUTTON_LETTUCE 34
#define BUTTON_CHEESE 27
#define BUTTON_MEAT 14
#define BUTTON_APPLE 26
#define BUTTON_DOUGH 35

// LED pin definitions
#define LED_LETTUCE 12  
#define LED_DOUGH 13    
#define LED_MEAT 32     
#define LED_CHEESE 33   
#define LED_APPLE 2
#define LED_TOMATO 4

#define DEBOUNCE_DELAY 300  // Debounce delay for buttons in milliseconds
#define RFID_TIMEOUT 5000   // RFID processing timeout in milliseconds

MFRC522 rfid(SS_PIN, RST_PIN);  // Initialize RFID reader

// Server MAC address (Update this with your server's MAC address)
uint8_t serverAddress[] = {0xC4, 0xDE, 0xE2, 0x5B, 0x81, 0x58};

// Define the struct message used for communication
typedef struct struct_message {
    char rfid[RFID_LENGTH];
    char ingredient[INGREDIENT_LENGTH];
    int chopCount;
    int cookCount;
    int playerScoreDelta;
    bool reset;
    char requestType[REQUEST_TYPE_LENGTH];
    int role;  // Added to match the server's struct
    char recipeName[MAX_RECIPE_NAME_LENGTH]; // New field to match server
} struct_message;

struct_message myData;     // Data to send to the server
struct_message serverData; // Data received from the server

volatile bool dataReceived = false;
char lastIngredientPressed[INGREDIENT_LENGTH] = "";  // Store the last button's ingredient

// New flag to track if a DataRequest has been sent
bool requestSent = false;

// Previous state variables for edge detection
bool previousButtonTomato = false;
bool previousButtonLettuce = false;
bool previousButtonCheese = false;
bool previousButtonMeat = false;
bool previousButtonApple = false;
bool previousButtonDough = false;

// Debounce tracking for each button
unsigned long lastDebounceTomato = 0;
unsigned long lastDebounceLettuce = 0;
unsigned long lastDebounceCheese = 0;
unsigned long lastDebounceMeat = 0;
unsigned long lastDebounceApple = 0;
unsigned long lastDebounceDough = 0;

// State machine for RFID processing
enum ProcessState {
    IDLE,
    WAITING_FOR_RFID,
    PROCESSING_RFID
};

ProcessState currentState = IDLE;
unsigned long rfidStartTime = 0;

// Currently active LED pin
int activeLED = -1;

// New variable to track which LED to light for 2 seconds
int currentLEDToLightPin = -1;

// Audio module pins (DY-HV20T)
#define AUDIO_TX 16              // TX2 for the audio module
#define AUDIO_RX 17              // RX2 for the audio module

// Initialize DYPlayer
HardwareSerial audioSerial(2);   // Serial2 for the DY-HV20T audio module
DY::Player audioModule(&audioSerial); // Initialize audio module

// --- NEW FIRE LOGIC: Global onFire flag + LED flashing ---
bool onFire = false;
unsigned long lastFlashTime = 0;
bool flashState = false;

bool gameRunning = false;

// Function prototypes
bool PICC_IsAnyCardPresent();
void resetData();
void sendDataToServer(struct_message* data);
int getLEDPin(const char* ingredient);
String checkButtons();

// Callback when data is sent
void onDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    Serial.print("Last Packet Send Status: ");
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

// Callback when data is received from the server
void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len == sizeof(struct_message)) {
        memcpy(&serverData, data, sizeof(serverData));

        // Ensure strings are null-terminated
        serverData.rfid[RFID_LENGTH - 1] = '\0';
        serverData.ingredient[INGREDIENT_LENGTH - 1] = '\0';
        serverData.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';
        serverData.recipeName[MAX_RECIPE_NAME_LENGTH - 1] = '\0'; // Ensure recipeName is null-terminated

        // Debugging: Print received data
        Serial.println("Data received from server:");
        Serial.print("RFID: ");
        Serial.println(serverData.rfid);
        Serial.print("Ingredient: ");
        Serial.println(serverData.ingredient);
        Serial.print("Chop Count: ");
        Serial.println(serverData.chopCount);
        Serial.print("Cook Count: ");
        Serial.println(serverData.cookCount);
        Serial.print("Player Score Delta: ");
        Serial.println(serverData.playerScoreDelta);
        Serial.print("Reset: ");
        Serial.println(serverData.reset ? "true" : "false");
        Serial.print("Request Type: ");
        Serial.println(serverData.requestType);
        Serial.print("Role: ");
        Serial.println(serverData.role);
        Serial.print("Recipe Name: ");
        Serial.println(serverData.recipeName); // Print the recipe name

        // --- NEW FIRE LOGIC: handle OnFire / ExtinguishFire ---
        if (strcmp(serverData.requestType, "OnFire") == 0) {
            Serial.println("Ingredient Station on FIRE!");
            //audioModule.stop();
            //audioModule.playSpecified(4);  // Track 4 for "fire" audio
            onFire = true;
        }
        else if (strcmp(serverData.requestType, "ExtinguishFire") == 0) {
            Serial.println("Ingredient Station Fire Extinguished.");
            //audioModule.stop();
            //audioModule.playSpecified(4);  // same track or different, as you like
            onFire = false;
            // Turn off all button LEDs
            digitalWrite(LED_LETTUCE, LOW);
            digitalWrite(LED_DOUGH, LOW);
            digitalWrite(LED_MEAT, LOW);
            digitalWrite(LED_CHEESE, LOW);
            digitalWrite(LED_APPLE, LOW);
            digitalWrite(LED_TOMATO, LOW);
        }else if (strcmp(serverData.requestType, "StartRound1") == 0
                || strcmp(serverData.requestType, "StartRound2") == 0
                || strcmp(serverData.requestType, "StartRound3") == 0) {
            gameRunning = true;
        }else if (strcmp(serverData.requestType, "endgame") == 0
                || strcmp(serverData.requestType, "Reinitialize") == 0) {
            gameRunning = false;
        }
        // Otherwise set dataReceived flag only if a request was sent
        else if (requestSent) {
            dataReceived = true;
        }
    } else {
        Serial.println("Received data size mismatch!");
    }
}

void resetData() {
    memset(&myData, 0, sizeof(myData));
    myData.chopCount = -1;  // Set to -1 to indicate no change
    myData.cookCount = -1;  // Set to -1 to indicate no change
    myData.playerScoreDelta = 0; // Initialize to 0
    myData.reset = false;
    myData.role = 0;        // Set role to 0 (not used in this client)
    lastIngredientPressed[0] = '\0';  // Clear the last ingredient pressed
    myData.recipeName[0] = '\0'; // Clear the recipeName
}

void sendDataToServer(struct_message* data) {
    esp_err_t result = esp_now_send(serverAddress, (uint8_t*)data, sizeof(struct_message));
    if (result == ESP_OK) {
        Serial.println("Data sent to server.");
    } else {
        Serial.print("Error sending data: ");
        Serial.println(result);
    }
}

int getLEDPin(const char* ingredient) {
    if (strcmp(ingredient, "lettuce") == 0) return LED_LETTUCE;
    if (strcmp(ingredient, "dough") == 0) return LED_DOUGH;
    if (strcmp(ingredient, "meat") == 0) return LED_MEAT;
    if (strcmp(ingredient, "cheese") == 0) return LED_CHEESE;
    if (strcmp(ingredient, "apple") == 0) return LED_APPLE;
    if (strcmp(ingredient, "tomato") == 0) return LED_TOMATO;
    return -1;  // Invalid ingredient
}

String checkButtons() {
    String ingredient = "";
    unsigned long currentTime = millis();

    // Check Tomato Button
    if (digitalRead(BUTTON_TOMATO) == HIGH && !previousButtonTomato) {
        if (currentTime - lastDebounceTomato > DEBOUNCE_DELAY) {
            lastDebounceTomato = currentTime;
            ingredient = "tomato";
            Serial.println("Button pressed: TOMATO");
        }
    }
    previousButtonTomato = (digitalRead(BUTTON_TOMATO) == HIGH);

    // Check Lettuce Button
    if (digitalRead(BUTTON_LETTUCE) == HIGH && !previousButtonLettuce) {
        if (currentTime - lastDebounceLettuce > DEBOUNCE_DELAY) {
            lastDebounceLettuce = currentTime;
            ingredient = "lettuce";
            Serial.println("Button pressed: LETTUCE");
        }
    }
    previousButtonLettuce = (digitalRead(BUTTON_LETTUCE) == HIGH);

    // Check Cheese Button
    if (digitalRead(BUTTON_CHEESE) == HIGH && !previousButtonCheese) {
        if (currentTime - lastDebounceCheese > DEBOUNCE_DELAY) {
            lastDebounceCheese = currentTime;
            ingredient = "cheese";
            Serial.println("Button pressed: CHEESE");
        }
    }
    previousButtonCheese = (digitalRead(BUTTON_CHEESE) == HIGH);

    // Check Meat Button
    if (digitalRead(BUTTON_MEAT) == HIGH && !previousButtonMeat) {
        if (currentTime - lastDebounceMeat > DEBOUNCE_DELAY) {
            lastDebounceMeat = currentTime;
            ingredient = "meat";
            Serial.println("Button pressed: MEAT");
        }
    }
    previousButtonMeat = (digitalRead(BUTTON_MEAT) == HIGH);

    // Check Apple Button
    if (digitalRead(BUTTON_APPLE) == HIGH && !previousButtonApple) {
        if (currentTime - lastDebounceApple > DEBOUNCE_DELAY) {
            lastDebounceApple = currentTime;
            ingredient = "apple";
            Serial.println("Button pressed: APPLE");
        }
    }
    previousButtonApple = (digitalRead(BUTTON_APPLE) == HIGH);

    // Check Dough Button
    if (digitalRead(BUTTON_DOUGH) == HIGH && !previousButtonDough) {
        if (currentTime - lastDebounceDough > DEBOUNCE_DELAY) {
            lastDebounceDough = currentTime;
            ingredient = "dough";
            Serial.println("Button pressed: DOUGH");
        }
    }
    previousButtonDough = (digitalRead(BUTTON_DOUGH) == HIGH);

    return ingredient;
}

void setup() {
    Serial.begin(115200);
    // Force channel 6 so it matches the server's channel
    WiFi.mode(WIFI_STA);
    delay(100);
    // Connect to WiFi (required for OTA)
    WiFi.begin("MyScoreboardAP", "MySecretPassword"); // Replace with your network credentials
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("\nWiFi connected, IP address: ");
    Serial.println(WiFi.localIP());
    
    // Set channel for ESP-NOW
    SPI.begin();
    esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);
    Serial.print("Home channel: ");
    Serial.println(WiFi.channel());  // Should print 6

    // Initialize OTA
    ArduinoOTA.onStart([]() {
      Serial.println("OTA Update Start");
    });
    ArduinoOTA.onEnd([]() {
      Serial.println("\nOTA Update End");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("OTA Progress: %u%%\n", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("OTA Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    ArduinoOTA.begin();
    Serial.println("OTA Initialized");

    rfid.PCD_Init();

    // Set button pins as INPUT_PULLDOWN
    pinMode(BUTTON_TOMATO, INPUT_PULLDOWN);
    pinMode(BUTTON_LETTUCE, INPUT_PULLDOWN);
    pinMode(BUTTON_CHEESE, INPUT_PULLDOWN);
    pinMode(BUTTON_MEAT, INPUT_PULLDOWN);
    pinMode(BUTTON_APPLE, INPUT_PULLDOWN);
    pinMode(BUTTON_DOUGH, INPUT_PULLDOWN);

    // Set LED pins as OUTPUT
    pinMode(LED_LETTUCE, OUTPUT);
    pinMode(LED_DOUGH, OUTPUT);
    pinMode(LED_MEAT, OUTPUT);
    pinMode(LED_CHEESE, OUTPUT);
    pinMode(LED_APPLE, OUTPUT);
    pinMode(LED_TOMATO, OUTPUT);

    // Initialize all LEDs to OFF
    digitalWrite(LED_LETTUCE, LOW);
    digitalWrite(LED_DOUGH, LOW);
    digitalWrite(LED_MEAT, LOW);
    digitalWrite(LED_CHEESE, LOW);
    digitalWrite(LED_APPLE, LOW);
    digitalWrite(LED_TOMATO, LOW);

    // Initialize ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("Error initializing ESP-NOW");
        return;
    }

    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);

    // Add server as a peer
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, serverAddress, 6);
    peerInfo.channel = 6;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Failed to add server peer");
        return;
    }

    resetData();  // Initialize myData
    memset(&serverData, 0, sizeof(serverData));  // Initialize serverData

    // Initialize DY-HV20T audio module
    audioSerial.begin(9600, SERIAL_8N1, AUDIO_RX, AUDIO_TX);  // Define RX/TX pins
    audioModule.begin();  // No arguments needed
    audioModule.setCycleMode(DY::PlayMode::OneOff);
    audioModule.setVolume(24);
    audioModule.stop();  // Reset the audio module

    Serial.print("Size of struct_message: ");
    Serial.println(sizeof(struct_message));  // Debugging: Print struct size

    Serial.println("Fridge Client ready.");
}

void loop() {
    // Handle OTA updates
    ArduinoOTA.handle();

    if (!gameRunning) {
        delay(50);          // lighten the watchdog
        return;             // do nothing until a round starts
    }


    // --- NEW FIRE LOGIC: If onFire, just flash all button LEDs and skip logic ---
    if (onFire) {
        if (millis() - lastFlashTime > 500) {
            lastFlashTime = millis();
            flashState = !flashState;

            digitalWrite(LED_LETTUCE, flashState);
            digitalWrite(LED_DOUGH,   flashState);
            digitalWrite(LED_MEAT,    flashState);
            digitalWrite(LED_CHEESE,  flashState);
            digitalWrite(LED_APPLE,   flashState);
            digitalWrite(LED_TOMATO,  flashState);
        }
        return; // Skip normal operation if on fire
    }

    String ingredient = checkButtons();

    if (ingredient != "") {
        // Update last ingredient pressed
        strncpy(lastIngredientPressed, ingredient.c_str(), INGREDIENT_LENGTH - 1);
        lastIngredientPressed[INGREDIENT_LENGTH - 1] = '\0';

        // Get the corresponding LED pin
        int ledPin = getLEDPin(lastIngredientPressed);
        if (ledPin != -1) {
            digitalWrite(ledPin, HIGH); // Turn on LED
            activeLED = ledPin;
        }

        // Proceed only if in IDLE state
        if (currentState == IDLE) {
            currentState = WAITING_FOR_RFID;
            rfidStartTime = millis();

            // Initiate RFID reading
            if (PICC_IsAnyCardPresent() && rfid.PICC_ReadCardSerial()) {
                // Format the RFID UID
                snprintf(myData.rfid, RFID_LENGTH, "%02X%02X%02X%02X",
                         rfid.uid.uidByte[0], rfid.uid.uidByte[1],
                         rfid.uid.uidByte[2], rfid.uid.uidByte[3]);
                myData.rfid[RFID_LENGTH - 1] = '\0';  // Ensure null-termination

                Serial.print("RFID detected: ");
                Serial.println(myData.rfid);

                // Prepare DataRequest message
                strncpy(myData.requestType, "DataRequest", REQUEST_TYPE_LENGTH - 1);
                myData.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';
                myData.recipeName[0] = '\0'; // Clear recipeName

                // Clear serverData before sending a new request
                memset(&serverData, 0, sizeof(serverData));

                // Send DataRequest to server
                sendDataToServer(&myData);

                // Set the requestSent flag
                requestSent = true;

                // Transition to PROCESSING_RFID state
                currentState = PROCESSING_RFID;
                rfidStartTime = millis();
            } else {
                Serial.println("No RFID card detected.");
                currentState = IDLE;
                // Turn off the LED if RFID not detected
                if (activeLED != -1) {
                    digitalWrite(activeLED, LOW);
                    activeLED = -1;
                }
            }
        }
    }

    // Handle RFID processing
    if (currentState == PROCESSING_RFID) {
        if (dataReceived && requestSent && strcmp(serverData.requestType, "DataResponse") == 0) {
            dataReceived = false;    // Reset the flag
            requestSent = false;     // Reset the requestSent flag

            // Determine which LED to light based on the conditions
            if (strcmp(serverData.ingredient, "none") == 0) {

                Serial.println("Updating ingredient on server.");

                // Prepare DataUpdate message
                strncpy(myData.ingredient, lastIngredientPressed, INGREDIENT_LENGTH - 1);
                myData.ingredient[INGREDIENT_LENGTH - 1] = '\0';
                strncpy(myData.requestType, "DataUpdate", REQUEST_TYPE_LENGTH - 1);
                myData.requestType[REQUEST_TYPE_LENGTH - 1] = '\0';
                myData.chopCount = -1;  // No change
                myData.cookCount = -1;  // No change
                myData.playerScoreDelta = 0; // No score change
                myData.reset = false;
                myData.recipeName[0] = '\0'; // Clear recipeName

                // Send DataUpdate to server
                sendDataToServer(&myData);

                // Set LED to the pressed ingredient
                currentLEDToLightPin = getLEDPin(lastIngredientPressed);

                // Play track 00001
                audioModule.stop();
                audioModule.playSpecified(1);  // Play track 00001

            }
            else {
                Serial.println("Ingredient already present – cannot switch.");
                playErrorFeedback(serverData.ingredient);
            }

            // Optionally, you can utilize the recipeName here
            if (strlen(serverData.recipeName) > 0) {
                Serial.print("Current Recipe: ");
                Serial.println(serverData.recipeName);
                // Add any additional logic to handle the recipe name
            }

            // Reset RFID state for next loop
            rfid.PICC_HaltA();
            rfid.PCD_StopCrypto1();
            resetData();  // Reset myData
            memset(&serverData, 0, sizeof(serverData));  // Clear serverData

            // Turn off the active LED after processing
            if (activeLED != -1) {
                digitalWrite(activeLED, LOW);
                activeLED = -1;
            }

            currentState = IDLE;
        }
        else if (millis() - rfidStartTime > RFID_TIMEOUT) {
            Serial.println("Timeout waiting for server response.");
            // Turn off the LED on timeout
            if (activeLED != -1) {
                digitalWrite(activeLED, LOW);
                activeLED = -1;
            }
            currentState = IDLE;
            requestSent = false;  // Reset the requestSent flag
        }
    }

    // At the end of the loop, handle the LED indication
    if (currentLEDToLightPin != -1) {
        digitalWrite(currentLEDToLightPin, HIGH); // Turn on the LED
        delay(2000); // Wait for 2 seconds (blocking)
        digitalWrite(currentLEDToLightPin, LOW);  // Turn off the LED
        currentLEDToLightPin = -1; // Reset the variable
    }
}

// Helper function to check if any card is present
bool PICC_IsAnyCardPresent() {
    byte bufferATQA[2];
    byte bufferSize = sizeof(bufferATQA);
    rfid.PCD_WriteRegister(rfid.TxModeReg, 0x00);
    rfid.PCD_WriteRegister(rfid.RxModeReg, 0x00);
    rfid.PCD_WriteRegister(rfid.ModWidthReg, 0x26);
    MFRC522::StatusCode result = rfid.PICC_WakeupA(bufferATQA, &bufferSize);
    return (result == MFRC522::STATUS_OK || result == MFRC522::STATUS_COLLISION);
}

void playErrorFeedback(const char* currentIngredient) {
    // 1) error sound  (choose any free track number, here #9)
    audioModule.stop();
    audioModule.playSpecified(2);

    // 2) blink ALL six button LEDs 3×
    const int leds[] = {LED_LETTUCE, LED_DOUGH, LED_MEAT,
                        LED_CHEESE,  LED_APPLE, LED_TOMATO};
    for (int b = 0; b < 3; b++) {
        for (int i = 0; i < 6; i++) digitalWrite(leds[i], HIGH);
        delay(150);
        for (int i = 0; i < 6; i++) digitalWrite(leds[i], LOW);
        delay(150);
    }

    // 3) light the LED that matches the ingredient already on the plate
    int pin = getLEDPin(currentIngredient);
    if (pin != -1) digitalWrite(pin, HIGH);
    delay(2000);
    if (pin != -1) digitalWrite(pin, LOW);
}
