#pragma once

#define _ATL_APARTMENT_THREADED
#include "atlbase.h"
#include "atlcom.h"
#include "sapi.h"
#include "sphelper.h"


bool ttsInit();
void ttsCleanup();

void ttsSay(const char* str);
