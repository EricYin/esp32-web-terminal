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
