echo $env:IDF_TARGET
$env:IDF_TARGET = "esp32s2"

python -m esptool --chip esp32s2 --port COM6 --baud 460800 write_flash 0x0 factory.bin
python -m esptool --chip esp32s2 --port COM6 write_flash <APP_OFFSET> firmware.bin


