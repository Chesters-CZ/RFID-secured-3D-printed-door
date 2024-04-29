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
      morse.send("BAD KEY");
      delay(500);

      while (true) {
        beepErr(0);
        morse.send("BAD KEY");
        delay(5000);
      }
  }

  // todo: detect pushed INIT_BTN and create write and delete cards.
  if (digitalRead(INIT_BTN) == LOW) {
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

    // create write card
    byte tagId[4];

    while (true) {
      while (!mfrc.detectTag(tagId)) {
      }

      int result = addCard(tagId);

      if (result < 0) {
        mfrc.unselectMifareTag();
        beepErr(result);
        continue;
      }

      beepSucc();

      result = addSpecialCard(false);

      if (result < 0) {
        mfrc.unselectMifareTag();
        beepErr(result);
        continue;
      }

      mfrc.unselectMifareTag();

      if (writeToEEPROM(tagId)) {
        beepSucc();
        break;
      }

      beepErr(-69);
      break;
    }

    delay(500);

    // create delete card
    while (true) {
      while (!mfrc.detectTag(tagId)) {
      }

      int result = addCard(tagId);

      if (result < 0) {
        mfrc.unselectMifareTag();
        beepErr(result);
        continue;
      }

      beepSucc();

      result = addSpecialCard(true);

      if (result < 0) {
        mfrc.unselectMifareTag();
        beepErr(result);
        continue;
      }

      mfrc.unselectMifareTag();
      beepSucc();

      if (writeToEEPROM(tagId)) {
        beepSucc();
        break;
      }

      beepErr(-69);
      break;
    }
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
}

void loop() {
  byte tagId[4];
  switch (current) {
    case LOCKED:
      if (mfrc.detectTag(tagId)) {
        // wait for the chip to get close enough
        delay(100);

        int readSize = mfrc.readFileSize(1, "encryptedID");

        // if error, beep and loop again
        if (readSize < 0) {
          beepErr(readSize);
          mfrc.unselectMifareTag();
          return 0;
        }

        // if card is not known, beep and loop
        if (!validateCardId(tagId, readSize)) {
          mfrc.unselectMifareTag();
          break;
        }


        // read admin identifier and check if it exists
        readSize = mfrc.readFileSize(2, "adminID");

        // if no admin record found, the card is unlock only
        if (readSize < 0) {
          next = UNLOCKED;
          mfrc.unselectMifareTag();
          break;
        }

        // admin record was found, validate it
        byte cardType = validateAdminId(tagId, readSize);

        // process the results
        switch (cardType) {
          case -1:
            // admin identifier is not valid,
            // it's just an unlock card
            beepSucc();
            next = UNLOCKED;
            break;
          case 0:
            // admin identifier valid
            beepAdd();
            next = WRITING_CARD;
            break;
          case 1:
            // admin identifier valid
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
        next = LOCKED;
      }
      break;

    case WRITING_CARD:
      if (mfrc.detectTag(tagId)) {
        delay(100);

        // check if card is registered
        if (!findEntryOnEEPROM(tagId)) {
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
        beepErr(0);
        next = LOCKED;
      }
      break;

    case DELETING_CARD:
      if (mfrc.detectTag(tagId)) {
        delay(100);

        // delete data on card
        byte blank[32] = { 0 };
        mfrc.writeFile(1, "encryptedID", blank, 32);

        // delete data on EEPROM
        deleteEntryFromEEPROM(tagId);

        // success
        mfrc.unselectMifareTag();
        beepSucc();
        next = LOCKED;
        break;
      }

      // timeout
      if (millis() > changeStateAt) {
        beepErr(0);
        next = LOCKED;
      }
      break;
  }

  if (next != current) {
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
  aes.do_aes_encrypt(cardID, 4, cipher, key, keyBits + 1);
  return mfrc.writeFile(1, "encryptedID", cipher, 32);
}

int addSpecialCard(bool isDelCard) {
  byte cipher[32] = { 0 };
  aes.do_aes_encrypt(adminKeyIdentifiers[isDelCard], 7, cipher, key, keyBits + 1);
  return mfrc.writeFile(2, "adminID", cipher, 32);
}

// find first empty spot on EEPROM
int findSpaceOnEEPROM() {
  for (int cardNum = 0; cardNum < 256; cardNum++) {
    bool isEmpty = true;

    for (byte idChar = 0; idChar < 4; idChar++) {
      if (EEPROM.read(cardNum * 4 + idChar) != 0) {
        isEmpty = false;
        break;
      }
    }

    if (isEmpty) {
      return cardNum;
    }
  }

  return -1;
}

// find first place the tag is present at
int findEntryOnEEPROM(byte data[4]) {
  for (int cardNum = 0; cardNum < 256; cardNum++) {
    bool isEqual = true;

    for (byte idChar = 0; idChar < 4; idChar++) {
      if (EEPROM.read(cardNum * 4 + idChar) != data[idChar]) {
        isEqual = false;
        break;
      }
    }

    if (isEqual) {
      return cardNum;
    }
  }

  return -1;
}

void deleteEntryFromEEPROM(byte data[4]) {
  int dataAddr = findEntryOnEEPROM(data);

  if (dataAddr == -1){
    return;
  }

  for (byte i = 0; i < 4; i++) {
    EEPROM.update(dataAddr + i, rand());
  }
}

// compares strings of different lengths, expecting the rest of the longer string to be zeroed
bool strEqual(byte smaller[], byte larger[], int smallerSize, int largerSize) {
  int i = 0;

  for (; i < smallerSize; i++) {
    if (smaller[i] != larger[i]) {
      return false;
    }
  }

  for (; i < largerSize; i++) {
    if (larger[i] != 0) {
      return false;
    }
  }

  return true;
}

// beeps failures only
bool validateCardId(byte tagId[4], int readSize) {
  byte readBytes[readSize];
  char plainBytes[readSize];

  // read
  int result = mfrc.readFile(1, "encryptedID", (unsigned char *)&readBytes, readSize);

  if (result < 0) {
    beepErr(result);
    return false;
  }

  // cryptography
  aes.set_IV(0);
  aes.do_aes_decrypt((unsigned char *)&readBytes, readSize, (unsigned char *)&plainBytes, key, keyBits + 1, 0);

  // check if strings are equal
  if (!strEqual(tagId, plainBytes, 4, readSize)) {
    beepWrong();
    return false;
  }

  if (findEntryOnEEPROM(tagId) == -1) {
    beepWrong();
    return false;
  }

  return true;
}

byte validateAdminId(byte tagId[4], int readSize) {
  byte readBytes[readSize];
  char plainBytes[readSize];

  // read
  int result = mfrc.readFile(2, "adminID", (unsigned char *)&readBytes, readSize);

  if (result < 0) {
    beepErr(result);
    return -1;
  }

  // cryptography
  aes.set_IV(0);
  aes.do_aes_decrypt((unsigned char *)&readBytes, readSize, (unsigned char *)&plainBytes, key, keyBits + 1, 0);

  // check if strings are equal
  if (strEqual(adminKeyIdentifiers[0], plainBytes, 7, readSize)) {
    return 0;
  }

  if (strEqual(adminKeyIdentifiers[1], plainBytes, 7, readSize)) {
    return 1;
  }

  return -1;
}

// ok
void beepSucc() {
  digitalWrite(BUZZ, HIGH);
  delay(50);
  digitalWrite(BUZZ, LOW);
  delay(50);
}

// d-á-d (add)
void beepAdd() {
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

bool writeToEEPROM(byte data[4]) {
  int address = findSpaceOnEEPROM();

  if (address == -1)
    return false;

  for (byte i = 0; i < 4; i++) {
    EEPROM.update(address++, data[i]);
  }

  return true;
}

// ... (e-rror code)
void beepErr(int code) {
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

  if (code < -2000) {
    // writeFile + writeRaw
    morse.send("BAD WRITEF CUZ ");
  }

  if (code < -1000 && code > -1020) {
    // readFile error + readFileSize
    morse.send("BAD READF CUZ ");
    code += 1000;
  }

  if (code < -200 && code > -220) {
    // writeRaw error + writeBlockAndVerify
    morse.send("BAD WRITE CUZ ");
    code += 200;
  }

  if (code < -100 && code > -120) {
    // readRaw error + readBlock
    morse.send("BAD READ CUZ ");
    code += 100;
  }

  switch (code) {
    case -1:  // readBlock
    case -3:  // verifyBlock
    case -9:  // readFileSize
      morse.send("BAD READ");
      break;
    case -2:  // readBlock
    case -7:  // writeBlockAndVerify
      morse.send("BAD SIZE");
      break;
    case -4:  // verifyBlock
      morse.send("BAD VERIF");
      break;
    case -5:  // writeBlockAndVerify
      morse.send("BAD WRITE");
      break;
    case -8:    // readFileSize
    case -121:  // readRaw
    case -221:  // writeRaw
      morse.send("BAD AUTH");
      break;
    case -10:  // readFileSize
      morse.send("NO FILE");
      break;
    case -11:  // readFileSize
      morse.send("BAD LABEL");
      break;
    case -120:  // readRaw
    case -220:  // writeRaw
    case -69:   // addToEEPROM
      morse.send("NO SPACE");
      break;
    case -70:  // unexpected behavior
      morse.send("BAD CODE");
    case -1020:  // readFile
      morse.send("SMALL BUFF");
    default:
      morse.send("BAD ERR");
  }
}