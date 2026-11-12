echo $env:IDF_TARGET
$env:IDF_TARGET = "esp32s2"

python -m esptool --chip esp32s2 --port COM6 --baud 460800 write_flash 0x0 factory.bin
python -m esptool --chip esp32s2 --port COM6 write_flash <APP_OFFSET> firmware.bin

#include "esp_heap_caps.h"

ESP_LOGI("heap", "Free=%u, largest=%u",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));


