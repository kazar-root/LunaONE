// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <jni.h>
#include <string>
#include <string_view>

namespace lunaone::android
{
void InitializeHttp(JNIEnv* env);
void CheckJava(JNIEnv* env, const char* message);
std::string Utf8(JNIEnv* env, jstring value);
jstring Text(JNIEnv* env, std::string_view value);
}
