// https://github.com/spaniakos/AES
#include <AES.h>
#include <AES_config.h>
#include <printf.h>

// https://github.com/pablo-sampaio/easy_mfrc522
#include <EasyMFRC522.h>
#include <RfidDictionaryView.h>

// Arduino libraries
#include <Wire.h>

// pin definition
#define EMAG 2
#define BUZZ 3
#define RFID_SDA 5
#define RFID_RST 6
// also RFID_MOSI 11
// also RFID_MISO 12
// also RFID_SCK 13

// libraries
AES aes;
EasyMFRC522 mfrc(RFID_SDA, RFID_RST);

enum STATE {
  LOCKED,
  UNLOCKED,
  WRITING_CARD,
  DELETING_CARD
};

STATE current = LOCKED;
STATE next = LOCKED;
unsigned long changeStateAt = 0;

void setup() {
  // init pins
  pinMode(EMAG, OUTPUT);
  pinMode(BUZZ, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  // init libraries
  mfrc.init();

  digitalWrite(EMAG, HIGH);  // lock the door
  digitalWrite(BUZZ, HIGH);  // buzz because you can
  digitalWrite(LED_BUILTIN, HIGH);

  delay(50);

  digitalWrite(BUZZ, LOW);
  digitalWrite(LED_BUILTIN, LOW);
}

void loop() {
  switch (current){
    case LOCKED:
      if (mfrc.detectTag()){
        next = UNLOCKED;
      }
      break;
    
    case UNLOCKED:
      if (millis() > changeStateAt){
        next = LOCKED;
      }
      break;
  }

  if (next != current){
    switch (next){
      case LOCKED:
        digitalWrite(EMAG, HIGH);
        digitalWrite(BUZZ, LOW);
        break;

      case UNLOCKED:
        digitalWrite(EMAG, LOW);
        digitalWrite(BUZZ, HIGH);
        changeStateAt = millis() + 2000;
        break;
    }

    current = next;
  }
}
