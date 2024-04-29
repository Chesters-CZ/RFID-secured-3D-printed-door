#include <MemoryUsage.h>

// https://github.com/ridencww/cww_MorseTx
#include <cww_MorseTx.h>

// https://github.com/spaniakos/AES
#include <AES.h>
#include <AES_config.h>

// https://github.com/pablo-sampaio/easy_mfrc522
#include <EasyMFRC522.h>

// Arduino libraries
#include <Wire.h>
#include <EEPROM.h>

// pin definition
#define EMAG 2
#define BUZZ 3
#define INIT_BTN 4
#define RFID_SDA 5
#define RFID_RST 6
// also RFID_MOSI 11
// also RFID_MISO 12
// also RFID_SCK 13

// libraries
AES aes;
EasyMFRC522 mfrc(RFID_SDA, RFID_RST);
cww_MorseTx morse(3, 10);

// THIS PROGRAM SAVES TO EEPROM ADDRESSES 0-1024
// Maximum number of saved keycards is 255

// HOLDING THE INIT_BTN AT STARTUP WILL:
//  - forget ALL saved keycards
//  - set the first two cards as the writing and deleting card respectively

enum STATE {
  LOCKED,
  UNLOCKED,
  WRITING_CARD,
  DELETING_CARD
};

STATE current = LOCKED;
STATE next = LOCKED;
unsigned long changeStateAt = 0;

// encryption key           "length of AES128"AES 192"AES 256" (longer = more secure)
byte *key = (unsigned char *)"4428472B4B62506561A6184C148E6418";
byte keyBits;

// admin keycard identifier
byte adminKeyIdentifiers[][7] = { "AddCard", "DelCard" };

void setup() {
  // init pins
  pinMode(EMAG, OUTPUT);
  pinMode(BUZZ, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(INIT_BTN, INPUT_PULLUP);

  // init libraries
  mfrc.init();
  Serial.begin(9600);

  while (!Serial)
    ;

  Serial.println(F("\n\nStarting door module..."));

  // init keyBits
  switch (strlen(key)) {
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
      beepErr(0);
      doMorse("BAD KEY");
      delay(500);

      while (true) {
        beepErr(0);
        doMorse("BAD KEY");
        delay(5000);
      }
  }

  Serial.println(F("Key size OK"));

  // todo: detect pushed INIT_BTN and create write and delete cards.
  if (digitalRead(INIT_BTN) == LOW) {
    Serial.println(F("\nINIT BTN pushed, resetting saved cards..."));

    digitalWrite(EMAG, HIGH);  // lock the door
    digitalWrite(BUZZ, HIGH);  // a "reset" beep
    digitalWrite(LED_BUILTIN, HIGH);

    delay(1500);

    digitalWrite(BUZZ, LOW);
    digitalWrite(LED_BUILTIN, LOW);

    // clear PROGMEM
    for (int i = 0; i < 1025; i++) {
      EEPROM.update(i, 0);
    }

    Serial.println(F("EEPROM cleared"));

    // create write card
    byte tagId[4];

    while (true) {
      Serial.println(F("Approach WRITE card"));

      while (!mfrc.detectTag(tagId)) {
      }

      Serial.println(F("Card detected"));

      int result = addCard(tagId);

      if (result < 0) {
        mfrc.unselectMifareTag();
        beepErr(result);
        continue;
      }

      result = addSpecialCard(false);

      if (result < 0) {
        mfrc.unselectMifareTag();
        beepErr(result);
        continue;
      }

      mfrc.unselectMifareTag();

      if (writeToEEPROM(tagId)) {
        beepAdd();
        break;
      }

      beepErr(-69);
      break;
    }

    Serial.println(F("WRITE card created.\n"));

    delay(500);

    // create delete card
    while (true) {
      Serial.println(F("Approach DELETE card"));

      while (!mfrc.detectTag(tagId)) {
      }

      Serial.println(F("Card detected"));

      int result = addCard(tagId);

      if (result < 0) {
        mfrc.unselectMifareTag();
        beepErr(result);
        continue;
      }

      result = addSpecialCard(true);

      if (result < 0) {
        mfrc.unselectMifareTag();
        beepErr(result);
        continue;
      }

      mfrc.unselectMifareTag();

      if (writeToEEPROM(tagId)) {
        beepAdd();
        break;
      }

      beepErr(-69);
      break;
    }

    Serial.println(F("DELETE card created.\n"));
  }

  delay(500);

  digitalWrite(EMAG, HIGH);  // lock the door

  // an "I'm ready" beep
  digitalWrite(BUZZ, HIGH);
  delay(50);
  digitalWrite(BUZZ, LOW);

  delay(50);

  digitalWrite(BUZZ, HIGH);
  delay(50);
  digitalWrite(BUZZ, LOW);

  Serial.println(F("Device init complete\n"));
}

void loop() {
  if (digitalRead(INIT_BTN) == LOW){
    debugPrintEEPROM();
  }

  byte tagId[4];
  switch (current) {
    case LOCKED:
      if (mfrc.detectTag(tagId)) {
        Serial.println(F("\nCard detected!"));

        // wait for the chip to get close enough
        delay(100);

        Serial.println(F("Reading file size..."));

        FREERAM_PRINT;

        int readSize = mfrc.readFileSize(1, "eID");

        // if error, beep and loop again
        if (readSize < 0) {
          beepErr(readSize);
          mfrc.unselectMifareTag();
          return 0;
        }

        // if card is not known, beep and loop
        if (!validateCardId(tagId, readSize)) {
          Serial.println(F("Card could not be authenticated."));
          mfrc.unselectMifareTag();
          break;
        }


        // read admin identifier and check if it exists
        readSize = mfrc.readFileSize(3, "aID");

        // if no admin record found, the card is unlock only
        if (readSize < 0) {
          Serial.println("Card authenticated as UNLOCK\n");
          next = UNLOCKED;
          mfrc.unselectMifareTag();
          break;
        }

        Serial.println(F("Admin sector detected, authenticating..."));

        // admin record was found, validate it
        byte cardType = validateAdminId(tagId, readSize);

        // process the results
        switch (cardType) {
          case -1:
            // admin identifier is not valid,
            // it's just an unlock card
            Serial.println(F("Admin sector invalid, card is UNLOCK only."));
            beepSucc();
            next = UNLOCKED;
            break;
          case 0:
            // admin identifier valid
            Serial.println(F("Card authenticated as WRITE."));
            beepAdd();
            next = WRITING_CARD;
            break;
          case 1:
            // admin identifier valid
            Serial.println(F("Card authenticated as DELETE."));
            beepDel();
            next = DELETING_CARD;
            break;
          default:
            // this should never occur
            beepErr(-70);
            break;
        }
        mfrc.unselectMifareTag();
      }
      break;

    case UNLOCKED:
      if (millis() > changeStateAt) {
        Serial.println(F("\nUNLOCK timeout reached."));
        next = LOCKED;
      }
      break;

    case WRITING_CARD:
      if (mfrc.detectTag(tagId)) {
        Serial.println(F("\nCard detected, attempting to add..."));

        delay(100);

        // check if card is registered
        if (findEntryOnEEPROM(tagId) != -1) {
          Serial.println(F("Card is already added."));
          mfrc.unselectMifareTag();
          beepWrong();
          next = LOCKED;
          break;
        }

        int result = addCard(tagId);

        // deal with errors from addCard
        if (result < 0) {
          mfrc.unselectMifareTag();
          beepErr(result);
          next = LOCKED;
          break;
        }

        // disconnect tag
        mfrc.unselectMifareTag();

        // write to eeprom was successful
        if (writeToEEPROM(tagId)) {
          Serial.println(F("Card added successfully."));
          beepSucc();
          next = LOCKED;
          break;
        }

        // ran out of eeprom space
        beepErr(-69);
        next = LOCKED;
        break;
      }

      // timeout
      if (millis() > changeStateAt) {
        Serial.println(F("\nWRITE timeout reached."));
        beepErr(0);
        next = LOCKED;
      }
      break;

    case DELETING_CARD:
      if (mfrc.detectTag(tagId)) {
        Serial.println(F("\nCard detected, deleting..."));

        delay(100);

        // delete data on card
        byte blank[32] = { 0 };
        mfrc.writeFile(1, "eID", blank, 32);

        // delete data on EEPROM
        deleteEntryFromEEPROM(tagId);

        // success
        Serial.println(F("Card successfully deleted."));
        mfrc.unselectMifareTag();
        beepSucc();
        next = LOCKED;
        break;
      }

      // timeout
      if (millis() > changeStateAt) {
        Serial.println(F("DELETE timeout reached."));
        beepErr(0);
        next = LOCKED;
      }
      break;
  }

  if (next != current) {
    Serial.print(F("STATE CHANGE "));
    Serial.print(current);
    Serial.print(F(" -> "));
    Serial.println(next);

    switch (next) {
      case LOCKED:
        // stop beeping and lock door
        digitalWrite(EMAG, HIGH);
        digitalWrite(BUZZ, LOW);
        break;

      case UNLOCKED:
        // start beepiong and unlock door
        digitalWrite(EMAG, LOW);
        digitalWrite(BUZZ, HIGH);
        changeStateAt = millis() + 2000;
        break;

      case WRITING_CARD:
        // set timeout and do stuff in the switch case
        changeStateAt = millis() + 2000;
        break;

      case DELETING_CARD:
        // set timeout and do stuff in the switch case
        changeStateAt = millis() + 2000;
        break;
    }

    current = next;
  }
}

int addCard(byte cardID[4]) {
  byte cipher[32] = { 0 };
  aes.set_IV(0);
  aes.do_aes_encrypt(cardID, 4, cipher, key, keyBits + 1);

  for (byte i = 0; i < 32; i++)
    Serial.print(cipher[i]);

  Serial.println();

  return mfrc.writeFile(1, "eID", cipher, 32);
}

int addSpecialCard(bool isDelCard) {
  byte cipher[32] = { 0 };
  aes.set_IV(0);
  aes.do_aes_encrypt(adminKeyIdentifiers[isDelCard], 7, cipher, key, keyBits + 1);
  return mfrc.writeFile(3, "aID", cipher, 32);
}

// find first empty spot on EEPROM
int findSpaceOnEEPROM() {
  Serial.println(F("Searching for space on EEPROM"));
  for (int cardNum = 0; cardNum < 256; cardNum++) {
    bool isEmpty = true;

    for (byte idChar = 0; idChar < 4; idChar++) {
      if (EEPROM.read(cardNum * 4 + idChar) != 0) {
        isEmpty = false;
        break;
      }
    }

    if (isEmpty) {
      Serial.print(F("Found space starting at "));
      Serial.println(cardNum * 4);
      return cardNum * 4;
    }
  }

  Serial.println(F("No space found, EEPROM is full."));
  return -1;
}

// find first place the tag is present at
int findEntryOnEEPROM(byte data[4]) {
  Serial.print(F("Searching for "));

  for (byte i = 0; i < 4; i++) {
    Serial.print(data[i]);
    Serial.print(F(" "));
  }

  // debugPrintEEPROM();

  Serial.println();

  for (int cardNum = 0; cardNum < 256; cardNum++) {
    bool isEqual = true;

    for (byte idChar = 0; idChar < 4; idChar++) {
      if (EEPROM.read(cardNum * 4 + idChar) != data[idChar]) {
        isEqual = false;
        break;
      }
    }

    if (isEqual) {
      Serial.print(F("Found starting at "));
      Serial.println(cardNum*4);

      return cardNum*4;
    }
  }

  Serial.println(F("ID not present on EEPROM"));
  return -1;
}

void debugPrintEEPROM() {
  Serial.println("EEPROM:");
  for (int cardNum = 0; cardNum < 256; cardNum++) {
    for (byte idChar = 0; idChar < 4; idChar++) {
      Serial.print(EEPROM.read(cardNum * 4 + idChar));
      Serial.print(" ");
    }
    Serial.println();
  }
  Serial.println();
}

void deleteEntryFromEEPROM(byte data[4]) {
  int dataAddr = findEntryOnEEPROM(data);

  if (dataAddr == -1) {
    return;
  }

  for (byte i = 0; i < 4; i++) {
    EEPROM.update(dataAddr + i, rand());
  }
}

bool writeToEEPROM(byte data[4]) {
  int address = findSpaceOnEEPROM();

  if (address == -1)
    return false;

  for (byte i = 0; i < 4; i++) {
    EEPROM.update(address++, data[i]);
  }

  return true;
}

// compares strings of different lengths, expecting the rest of the longer string to be zeroed
bool strEqual(byte smaller[], byte larger[], int charsChecked) {
  int i = 0;

  for (; i < charsChecked; i++) {
    Serial.print(smaller[i]);
  }

  Serial.println();

  for (i = 0; i < 32; i++) {
    Serial.print(larger[i]);
  }

  Serial.println();

  for (i = 0; i < charsChecked; i++) {
    if (smaller[i] != larger[i]) {
      return false;
    }
  }

  return true;
}

// beeps failures only
bool validateCardId(byte tagId[4], int readSize) {
  Serial.print("Validating card ID ");
  Serial.print(tagId[0]);
  Serial.print(" ");
  Serial.print(tagId[1]);
  Serial.print(" ");
  Serial.print(tagId[2]);
  Serial.print(" ");
  Serial.print(tagId[3]);
  Serial.print(", ");
  Serial.println(readSize);

  unsigned char readBytes[readSize];
  unsigned char plainBytes[readSize];

  Serial.println("Reading file...");

  // read
  int result = mfrc.readFile(1, "eID", (unsigned char *)&readBytes, readSize);

  if (result < 0) {
    beepErr(result);
    return false;
  }

  Serial.print("Decrypting ");
  Serial.write(readBytes, readSize + 1);

  // cryptography
  aes.set_IV(0);
  Serial.println("...");
  aes.do_aes_decrypt(readBytes, readSize, plainBytes, key, keyBits + 1);

  Serial.println("Verifying...");

  // check if strings are equal
  if (!strEqual(tagId, plainBytes, 4)) {
    Serial.println("Decrypted ID does not match tag's");
    beepWrong();
    return false;
  }

  if (findEntryOnEEPROM(tagId) == -1) {
    Serial.println("Tag ID unknown");
    beepWrong();
    return false;
  }

  return true;
}

byte validateAdminId(byte tagId[4], int readSize) {
  byte readBytes[readSize];
  char plainBytes[readSize];

  // read
  int result = mfrc.readFile(3, "aID", (unsigned char *)&readBytes, readSize);

  if (result < 0) {
    beepErr(result);
    return -1;
  }

  // cryptography
  aes.set_IV(0);
  aes.do_aes_decrypt(readBytes, readSize, plainBytes, key, keyBits + 1);

  // check if strings are equal
  if (strEqual(adminKeyIdentifiers[0], plainBytes, 7)) {
    return 0;
  }

  if (strEqual(adminKeyIdentifiers[1], plainBytes, 7)) {
    return 1;
  }

  return -1;
}

void doMorse(const char *data) {
  Serial.print(F("Morse: "));
  Serial.println(data);
  morse.send(data);
}

// ok
void beepSucc() {
  Serial.println(F("Success!"));
  digitalWrite(BUZZ, HIGH);
  delay(50);
  digitalWrite(BUZZ, LOW);
  delay(50);
}

// d-á-d (add)
void beepAdd() {
  Serial.println(F("Added."));

  digitalWrite(BUZZ, HIGH);
  delay(50);
  digitalWrite(BUZZ, LOW);
  delay(50);

  digitalWrite(BUZZ, HIGH);
  delay(250);
  digitalWrite(BUZZ, LOW);
  delay(50);

  digitalWrite(BUZZ, HIGH);
  delay(50);
  digitalWrite(BUZZ, LOW);
  delay(50);
}

// d-é-lll
void beepDel() {
  Serial.println(F("Deleted."));

  digitalWrite(BUZZ, HIGH);
  delay(50);
  digitalWrite(BUZZ, LOW);
  delay(50);

  digitalWrite(BUZZ, HIGH);
  delay(250);
  digitalWrite(BUZZ, LOW);
  delay(50);

  digitalWrite(BUZZ, HIGH);
  delay(250);
  digitalWrite(BUZZ, LOW);
  delay(50);
}

// !!!!!!!!
void beepWrong() {
  Serial.println(F("Wrong!"));

  digitalWrite(BUZZ, HIGH);
  delay(10);
  digitalWrite(BUZZ, LOW);
  delay(10);

  digitalWrite(BUZZ, HIGH);
  delay(10);
  digitalWrite(BUZZ, LOW);
  delay(10);

  digitalWrite(BUZZ, HIGH);
  delay(10);
  digitalWrite(BUZZ, LOW);
  delay(10);

  digitalWrite(BUZZ, HIGH);
  delay(10);
  digitalWrite(BUZZ, LOW);
  delay(10);

  digitalWrite(BUZZ, HIGH);
  delay(10);
  digitalWrite(BUZZ, LOW);
  delay(10);

  digitalWrite(BUZZ, HIGH);
  delay(10);
  digitalWrite(BUZZ, LOW);
  delay(10);

  digitalWrite(BUZZ, HIGH);
  delay(10);
  digitalWrite(BUZZ, LOW);
  delay(10);

  digitalWrite(BUZZ, HIGH);
  delay(10);
  digitalWrite(BUZZ, LOW);
  delay(250);
}

// ... (e-rror code)
void beepErr(int code) {
  Serial.print(F("ERROR "));
  Serial.println(code);

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

  if (code == 0) {
    return;
  }

  if (code < -2500) {
    // writeFile + writeRaw
    doMorse("BAD WRITEF CUZ ");
    code += 2500;
  }

  if (code < -2000) {
    // writeFile + writeRaw
    doMorse("BAD WRITEF CUZ ");
    code += 2000;
  }

  if (code < -1000 && code > -1020) {
    // readFile error + readFileSize
    doMorse("BAD READF CUZ ");
    code += 1000;
  }

  if (code < -200 && code > -220) {
    // writeRaw error + writeBlockAndVerify
    doMorse("BAD WRITE CUZ ");
    code += 200;
  }

  if (code < -100 && code > -120) {
    // readRaw error + readBlock
    doMorse("BAD READ CUZ ");
    code += 100;
  }

  switch (code) {
    case -1:  // readBlock
    case -3:  // verifyBlock
    case -9:  // readFileSize
      doMorse("BAD READ");
      break;
    case -2:  // readBlock
    case -7:  // writeBlockAndVerify
      doMorse("BAD SIZE");
      break;
    case -4:  // verifyBlock
      doMorse("BAD VERIF");
      break;
    case -5:  // writeBlockAndVerify
      doMorse("BAD WRITE");
      break;
    case -8:    // readFileSize
    case -121:  // readRaw
    case -221:  // writeRaw
      doMorse("BAD AUTH");
      break;
    case -10:  // readFileSize
      doMorse("NO FILE");
      break;
    case -11:  // readFileSize
      doMorse("BAD LABEL");
      break;
    case -120:  // readRaw
    case -220:  // writeRaw
    case -69:   // addToEEPROM
      doMorse("NO SPACE");
      break;
    case -70:  // unexpected behavior
      doMorse("BAD CODE");
    case -1020:  // readFile
      doMorse("SMALL BUFF");
    default:
      doMorse("BAD ERR");
  }
}