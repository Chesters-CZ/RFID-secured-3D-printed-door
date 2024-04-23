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

// THIS PROGRAM SAVES TO EEPROM ADDRESSES 0-1024
// Maximum number of saved keycards is 255

// HOLDING THE INIT_BTN AT STARTUP WILL:
//  - forget ALL saved keycards
//  - set the next two cards as the
//    writing and deleting card respectively

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

// card registers
// fixme: this shit is really fucking big
byte cards[255][4] = { 0 };


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
        delay(1000);
      }
  }

  // todo: detect pushed INIT_BTN and create write and delete cards.
  if (digitalRead(INIT_BTN) == LOW) {
    digitalWrite(EMAG, HIGH);  // lock the door
    digitalWrite(BUZZ, HIGH);  // an "I'm ready" beep
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

      if (writeToEEPROM(tagId);) {
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

      if (writeToEEPROM(tagId);) {
        beepSucc();
        break;
      }

      beepErr(-69);
      break;
    }
  }

  delay(500);

  // read known card ids
  reloadFromEEPROM();

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
  switch (current) {
    case LOCKED:
      // todo: authenticate card
      byte tagId[4];
      if (mfrc.detectTag(tagId)) {
        // make sure the chip is close enough
        delay(100);

        int readSize = mfrc.readFileSize(1, "encryptedID");

        // if error, deal with it and try again
        if (readSize < 0) {
          beepErr(readSize);
          mfrc.unselectMifareTag();
          return 0;
        }

        if (!validateCardID(tagId, readSize)) {
          mfrc.unselectMifareTag();
          break;
        }

        // clear readBytes and plainBytes
        for (int i = 0; i < readSize; i++) {
          readBytes[i] = plainBytes[i] = 0;
        }


        // read admin identifier and check admin
        readSize = mfrc.readFileSize(2, "adminID");

        // if error, deal with it and try again
        if (readSize < 0) {
          next = UNLOCKED;
          mfrc.unselectMifareTag();
          break;
        }

        byte cardType = validateAdminID(tagId, readSize);

        switch (cardType) {
          case -1:
            beepSucc();
            next = UNLOCKED;
            break;
          case 0:
            beepAdd();
            next = WRITING_CARD;
            break;
          case 1:
            beepDel();
            next = DELETING_CARD;
            break;
          default:
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
        if(!findEntryOnEEPROM(tagId)){
              // todo: chyba
        }


        int result = addCard(tagId);


        if (result < 0) {
          mfrc.unselectMifareTag();
          beepErr(result);
          next = LOCKED;
          continue;
        }

        mfrc.unselectMifareTag();
              
      if (writeToEEPROM(tagId);) {
        beepSucc();
        break;
      }

      beepErr(-69);
        beepSucc();
        break;
      }

      if (millis() > statechangeAt) {
        beepErr();
        next = LOCKED
      }
      break;
      // todo: case DELETING_CARD
  }

  if (next != current) {
    switch (next) {
      case LOCKED:
        digitalWrite(EMAG, HIGH);
        digitalWrite(BUZZ, LOW);
        break;

      case UNLOCKED:
        digitalWrite(EMAG, LOW);
        digitalWrite(BUZZ, HIGH);
        changeStateAt = millis() + 2000;
        break;

      case WRITING_CARD:
        changeStateAt = millis() + 2000;
        break;

      case DELETING_CARD:
        stateChangeAt = millis() + 2000;
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

// find first place the data is present at
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

// beeps failures
bool validateCardId(bytes tagId[4], int readSize) {
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

  // todo: check if string present on EEPROM

  return true;
}

byte validateAdminId(bytes tagId[4], int readSize) {
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

  for (byte b : data) {
    EEPROM.update(address++, b);
  }

  return true;
}

void readFromEEPROM(){
  for (int cardNum = 0; cardNum < 256; cardNum++) {
    for (byte idChar = 0; idChar < 4; idChar++) {
      cards[cardNum][idChar] = EEPROM.read(cardNum * 4 + idChar);
    }
  }
}

// ... (error code)
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
    case -1020:  // readFile
      morse.send("SMALL BUFF");
    default:
      morse.send("BAD ERR");
  }
}