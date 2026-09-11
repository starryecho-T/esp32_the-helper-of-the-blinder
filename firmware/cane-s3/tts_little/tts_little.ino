#include "VTX316.h"

void setup(){
  VTX316_Init(6,5);

}

void loop(){
  Voice_BOBAO("今天天气晴，适合外出");
  delay(5000);

}