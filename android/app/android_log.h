// Forced into every game source file so printf output reaches logcat.
#ifndef LINKSAVER_ANDROID_LOG_H_
#define LINKSAVER_ANDROID_LOG_H_
#include <stdio.h>
#include <android/log.h>
#define printf(...) __android_log_print(ANDROID_LOG_INFO, "linksaver", __VA_ARGS__)
#endif
