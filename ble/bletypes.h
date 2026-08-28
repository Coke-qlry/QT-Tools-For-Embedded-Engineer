#pragma once

// =====================================================================
// bletypes.h - BLE 模块共享类型与常量
// 供 blescanner / bleadvertiser / bleconnection 三个子模块共同使用。
// =====================================================================

#include <QString>

namespace BleTypes {

// ---- 广播默认参数 ----
inline constexpr int kDefaultAdvertisingIntervalMs = 100;   // 默认广播间隔(ms)
inline constexpr auto kDefaultLocalName = "BLE SAR";        // 默认广播名

// ---- SAR 服务 UUID（占位，请按实际设备固件修改） ----
// 典型 Nordic 自定义服务：0xFFF0 / 特征 0xFFF1
inline constexpr auto kSarServiceUuid =
    "0000fff0-0000-1000-8000-00805f9b34fb";
inline constexpr auto kSarCharacteristicUuid =
    "0000fff1-0000-1000-8000-00805f9b34fb";

} // namespace BleTypes
