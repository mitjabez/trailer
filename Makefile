BOARD_TAG    = mega
BOARD_SUB    = atmega2560
#BOARD_TAG    = uno
MONITOR_PORT = /dev/ttyUSB0
MONITOR_BAUDRATE = 115200
ARDUINO_LIBS += MPU6050 I2Cdev Wire Bounce2

include  /usr/share/arduino/Arduino.mk
