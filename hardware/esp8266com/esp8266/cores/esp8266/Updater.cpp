#include "Updater.h"
#include "Arduino.h"
#include "eboot_command.h"
extern "C"{
  #include "mem.h"
}
//#define DEBUG_UPDATER Serial

extern "C" uint32_t _SPIFFS_start;

UpdaterClass::UpdaterClass() : _error(0), _buffer(0), _bufferLen(0), _size(0), _startAddress(0), _currentAddress(0) {}

bool UpdaterClass::begin(size_t size){
  if(_size > 0){
#ifdef DEBUG_UPDATER
    DEBUG_UPDATER.println("already running");
#endif
    return false;
  }
  
  if(size == 0){
    _error = UPDATE_ERROR_SIZE;
#ifdef DEBUG_UPDATER
    printError(DEBUG_UPDATER);
#endif
    return false;
  }
  
  if(_buffer) os_free(_buffer);
  
  _bufferLen = 0;
  _startAddress = 0;
  _currentAddress = 0;
  _size = 0;
  _error = 0;
  
  uint32_t usedSize = ESP.getSketchSize();
  uint32_t freeSpaceEnd = (uint32_t)&_SPIFFS_start - 0x40200000 - (5 * FLASH_SECTOR_SIZE);
  uint32_t roundedSize = (size + FLASH_SECTOR_SIZE - 1) & (~(FLASH_SECTOR_SIZE - 1));
  uint32_t freeSpaceStart = freeSpaceEnd - roundedSize;
  
  //new sketch can not be more then half the size or more than the free space
  //this means that max sketch size is (1MB - 20KB) / 2 for flash 2MB and above
  //and the current sketch should not be more than that either
  if(freeSpaceStart < usedSize || roundedSize > (freeSpaceEnd/2)){
    _error = UPDATE_ERROR_SPACE;
#ifdef DEBUG_UPDATER
    printError(DEBUG_UPDATER);
#endif
    return false;
  }
  noInterrupts();
  int rc = SPIEraseAreaEx(freeSpaceStart, roundedSize);
  interrupts();
  if (rc){
    _error = UPDATE_ERROR_ERASE;
#ifdef DEBUG_UPDATER
    printError(DEBUG_UPDATER);
#endif
    return false;
  }
  _startAddress = freeSpaceStart;
  _currentAddress = _startAddress;
  _size = size;
  _buffer = (uint8_t*)os_malloc(FLASH_SECTOR_SIZE);
  
  return true;
}

bool UpdaterClass::end(bool evenIfRemaining){
  if(_size == 0){
#ifdef DEBUG_UPDATER
    DEBUG_UPDATER.println("no update");
#endif
    return false;
  }
  
  if(hasError() || (!isFinished() && !evenIfRemaining)){
#ifdef DEBUG_UPDATER
    DEBUG_UPDATER.printf("premature end: res:%u, pos:%u/%u\n", getError(), progress(), _size);
#endif
    if(_buffer) os_free(_buffer);
    _bufferLen = 0;
    _currentAddress = 0;
    _startAddress = 0;
    _size = 0;
    return false;
  }
  
  if(evenIfRemaining){
    if(_bufferLen > 0){
      _writeBuffer();
    }
    _size = progress();
  }
  if(_buffer) os_free(_buffer);
  _bufferLen = 0;
  _currentAddress = 0;
  
  eboot_command ebcmd;
  ebcmd.action = ACTION_COPY_RAW;
  ebcmd.args[0] = _startAddress;
  ebcmd.args[1] = 0x00000;
  ebcmd.args[2] = _size;
  eboot_command_write(&ebcmd);
  
#ifdef DEBUG_UPDATER
    DEBUG_UPDATER.printf("Staged: address:0x%08X, size:0x%08X\n", _startAddress, _size);
#endif
  
  _startAddress = 0;
  _size = 0;
  _error = UPDATE_ERROR_OK;
  return true;
}

bool UpdaterClass::_writeBuffer(){
  WDT_FEED();
  noInterrupts();
  int rc = SPIWrite(_currentAddress, _buffer, _bufferLen);
  interrupts();
  if (rc) {
    _error = UPDATE_ERROR_WRITE;
    _currentAddress = (_startAddress + _size);
#ifdef DEBUG_UPDATER
    printError(DEBUG_UPDATER);
#endif
    return false;
  }
  _currentAddress += _bufferLen;
  _bufferLen = 0;
  return true;
}

size_t UpdaterClass::write(uint8_t *data, size_t len){
  size_t left = len;
  if(hasError())
    return 0;
  
  if(len > remaining())
    len = remaining();
  
  while((_bufferLen + left) > FLASH_SECTOR_SIZE){
    size_t toBuff = FLASH_SECTOR_SIZE - _bufferLen;
    memcpy(_buffer + _bufferLen, data + (len - left), toBuff);
    _bufferLen += toBuff;
    if(!_writeBuffer()){
      return len - left;
    }
    left -= toBuff;
    yield();
  }
  //lets see whats left
  memcpy(_buffer + _bufferLen, data + (len - left), left);
  _bufferLen += left;
  if(_bufferLen == remaining()){
    //we are at the end of the update, so should write what's left to flash
    if(!_writeBuffer()){
      return len - left;
    }
  }
  return len;
}

size_t UpdaterClass::writeStream(Stream &data){
  size_t written = 0;
  size_t toRead = 0;
  if(hasError())
    return 0;
  
  while(remaining()){
    toRead = FLASH_SECTOR_SIZE - _bufferLen;
    toRead = data.readBytes(_buffer + _bufferLen, toRead);
    if(toRead == 0){ //Timeout
      _error = UPDATE_ERROR_STREAM;
      _currentAddress = (_startAddress + _size);
#ifdef DEBUG_UPDATER
      printError(DEBUG_UPDATER);
#endif
      return written;
    }
    _bufferLen += toRead;
    if((_bufferLen == remaining() || _bufferLen == FLASH_SECTOR_SIZE) && !_writeBuffer())
      return written;
    written += toRead;
    yield();
  }
  return written;
}

void UpdaterClass::printError(Stream &out){
  out.printf("ERROR[%u]: ", _error);
  if(_error == UPDATE_ERROR_OK){
    out.println("No Error");
  } else if(_error == UPDATE_ERROR_WRITE){
    out.println("Flash Write Failed");
  } else if(_error == UPDATE_ERROR_ERASE){
    out.println("Flash Erase Failed");
  } else if(_error == UPDATE_ERROR_SPACE){
    out.println("Not Enough Space");
  } else if(_error == UPDATE_ERROR_SIZE){
    out.println("Bad Size Given");
  } else if(_error == UPDATE_ERROR_STREAM){
    out.println("Stream Read Timeout");
  } else {
    out.println("UNKNOWN");
  }
}

UpdaterClass Update;
