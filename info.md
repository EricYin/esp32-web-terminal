1. python -m esptool --port /dev/ttyUSB0 --baud 460800 --chip esp32c3 write-flash --erase-all @flash_args

2. idf.py -p /dev/ttyUSB0 erase-flash
idf.py -p /dev/ttyUSB0 flash

=>idf.py -p /dev/ttyUSB0 erase-flash flash

3. python -m esptool --port /dev/ttyUSB0 --baud 460800 --chip esp32c3 write-flash --erase-all \
  0x0 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0xF000 ota_data_initial.bin \
  0x20000 esp32-web-terminal.bin


# Name,    Type,  SubType,  Offset,   Size
nvs,       data,  nvs,      0x9000,   0x10000
otadata,   data,  ota,      0x19000,  0x2000
ota_0,     app,   ota_0,    0x20000,  0x500000
ota_1,     app,   ota_1,    0x520000, 0x500000
storage,   data,  spiffs,   0xA20000, 0x200000


分区	起始地址	大小
NVS	0x9000	64 KB
otadata	0x19000	8 KB
ota_0	0x20000	5 MB
ota_1	0x520000	5 MB
storage	0xA20000	2 MB


typedef enum {
  WIFI_POWER_21dBm = 84,      // 21dBm
  WIFI_POWER_20_5dBm = 82,    // 20.5dBm
  WIFI_POWER_20dBm = 80,      // 20dBm
  WIFI_POWER_19_5dBm = 78,    // 19.5dBm
  WIFI_POWER_19dBm = 76,      // 19dBm
  WIFI_POWER_18_5dBm = 74,    // 18.5dBm
  WIFI_POWER_17dBm = 68,      // 17dBm
  WIFI_POWER_15dBm = 60,      // 15dBm
  WIFI_POWER_13dBm = 52,      // 13dBm
  WIFI_POWER_11dBm = 44,      // 11dBm
  WIFI_POWER_8_5dBm = 34,     // 8.5dBm
  WIFI_POWER_7dBm = 28,       // 7dBm
  WIFI_POWER_5dBm = 20,       // 5dBm
  WIFI_POWER_2dBm = 8,        // 2dBm
  WIFI_POWER_MINUS_1dBm = -4  // -1dBm
} wifi_power_t;
