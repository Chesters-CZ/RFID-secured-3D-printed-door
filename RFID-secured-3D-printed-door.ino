// https://github.com/ridencww/cww_MorseTx
#include <cww_MorseTx.h>

// https://github.com/spaniakos/AES
#include <AES.h>
#include <AES_config.h>
#include <printf.h>

// https://github.com/pablo-sampaio/easy_mfrc522
#include <EasyMFRC522.h>
#include <RfidDictionaryView.h>

// Arduino libraries
#include <Wire.h>
#include <EEPROM.h>

// pin definition
#define EMAG 2
#define BUZZ 3
#define RFID_SDA 5
#define RFID_RST 6
#define INIT_BTN 4
// also RFID_MOSI 11
// also RFID_MISO 12
// also RFID_SCK 13

// libraries
AES aes;
EasyMFRC522 mfrc(RFID_SDA, RFID_RST);
cww_MorseTx morse(3, 10);

enum STATE {
  LOCKED,
  UNLOCKED,
  WRITING_CARD,
  DELETING_CARD
};

STATE current = LOCKED;
STATE next = LOCKED;
unsigned long changeStateAt = 0;
byte registeredChipCount = 0;

// encryption key           "length of AES128"AES 192"AES 256" (longer = more secure)
byte *key = (unsigned char*)"4428472B4B62506561A6184C148E6418";
byte keyBits;

// admin keycard identifier
byte registrationIdentifier[] = "Registration";
byte deletionIdentifier[] = "Deletion";

void setup() {
  // init pins
  pinMode(EMAG, OUTPUT);
  pinMode(BUZZ, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);

  // init libraries
  mfrc.init();

  // init chipCount
  registeredChipCount = EEPROM.read(0);

  // init keyBits
  switch (strlen(key)){
    case 16:
      keyBits = 127;
      break;
    case 24:
      keyBits = 191;
      break;
    case 32:
      keyBits = 255;
      break;
    default:
      // key has invalid length, cannot continue
      beepErr();
      morse.send("BAD KEY");
      delay(500);

      while (true){
        beepErr();
        delay(1000);
      }
  }

  // todo: detect pushed INIT_BTN and create write and delete cards.

  digitalWrite(EMAG, HIGH);  // lock the door
  digitalWrite(BUZZ, HIGH);  // an "I'm ready" beep
  digitalWrite(LED_BUILTIN, HIGH);

  delay(50);

  digitalWrite(BUZZ, LOW);
  digitalWrite(LED_BUILTIN, LOW);
}

void loop() {
  switch (current){
    case LOCKED:
      // todo: authenticate card
      byte tagId[4];
      if (mfrc.detectTag(tagId)){
        // make sure the chip is close enough
        delay(100);

        int readSize = mfrc.readFileSize(1, "encryptedID");
        
        // if error, deal with it and try again
        if (readSize < 0){
          beepErr();
          switch (readSize){
            case -8:
              morse.send("BAD AUTH");
              break;
            case -9:
              morse.send("BAD SIZE");
              break;
            case -10:
              morse.send("NO FILE");
              break;
            case -11:
              morse.send("BAD LABEL");
              break;
            default:
              morse.send("BAD ERR");
          }
          mfrc.unselectMifareTag();
          return 0;
        }

        // no error found, read the file's contents
        byte readBytes[readSize];
        char plainBytes[readSize];

        mfrc.readFile(1, "encryptedID", (unsigned char*)&readBytes, readSize);

        // cryptography
        aes.set_IV(0);
        aes.do_aes_decrypt((unsigned char*)&readBytes, readSize, &plainBytes, key, keyBits+1, 0);

        // todo: check if strings are equal
      }
      break;
    
    case UNLOCKED:
      if (millis() > changeStateAt){
        next = LOCKED;
      }
      break;

    // todo: case WRITING_CARD
    // todo: case DELETING_CARD
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

      // todo: case WRITING_CARD
      // todo: case DELETING_CARD
    }

    current = next;
  }
}

void beepErr(){
  digitalWrite(BUZZ, HIGH);
  delay(50);
  digitalWrite(BUZZ, LOW);
  delay(50);
  
  digitalWrite(BUZZ, HIGH);
  delay(50);
  digitalWrite(BUZZ, LOW);
  delay(50);
  
  digitalWrite(BUZZ, HIGH);
  delay(50);
  digitalWrite(BUZZ, LOW);
  delay(250);
}