#pragma once
#include <Arduino.h>
static const size_t QUEUE_CAPACITY = 24;

// 0=DEVICE_ON, 1=BUTTON_CLICK, 2=RFID_SCAN
struct StoredEvent {
  char uuid[37];
  char badgeUid[24];
  uint32_t sequence;
  uint32_t bootId;
  uint32_t uptimeSeconds;
  uint16_t batteryMv;
  uint8_t plugged;
  uint8_t kind;
  uint8_t testMode;
};
struct QueueImage {
  uint32_t magic;
  uint32_t nextSequence;
  uint16_t count;
  StoredEvent events[QUEUE_CAPACITY];
};

