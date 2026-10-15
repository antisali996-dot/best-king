LOCAL_PATH := $(call my-dir)

# ---- 预编译: Paradise 驱动 API 静态库（arm64-v8a） ----
include $(CLEAR_VARS)
LOCAL_MODULE    := paradise_api
LOCAL_SRC_FILES := third_party/paradise/libparadise_api.a
include $(PREBUILT_STATIC_LIBRARY)

# ---- 主程序 ----
include $(CLEAR_VARS)
LOCAL_MODULE    := King.sh
LOCAL_SRC_FILES := src/main.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/third_party/paradise
LOCAL_CPPFLAGS  := -std=c++17 -O2 -fexceptions
LOCAL_STATIC_LIBRARIES := paradise_api
LOCAL_LDLIBS    := -llog
include $(BUILD_EXECUTABLE)
