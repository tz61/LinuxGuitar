// Chinese translations of audio-module messages and labels.
#include "audio/Engine.h"
#include "common/I18n.h"

namespace gs {

void registerAudioTranslations() {
    static const TrPair kZh[] = {
        // AlsaDuplex
        {"The sound card supports none of S32/S24/S16", "声卡不支持 S32/S24/S16 格式"},
        {"playback parameters", "放音参数"},
        {"capture parameters", "录音参数"},
        {"Cannot open capture device %s", "无法打开录音设备 %s"},
        {"Cannot open playback device %s", "无法打开放音设备 %s"},
        {"Could not set capture and playback to the same sample rate/period", "录音与放音无法使用相同的采样率/周期"},
        {"Failed to set sw_params", "设置 sw_params 失败"},
        {"%s/%s, %s, playback buffer %u frames", "%s/%s, %s, 放音缓冲 %u 帧"},
        {"linked", "已链接"},
        {"not linked", "未链接"},
        {"%s, recording only", "%s, 只录音"},
        {"Cannot play through %s (%s); using the sound card instead", "无法通过 %s 播放 (%s), 改用声卡输出"},
        // CardMixer
        {"Cannot open the mixer", "无法打开混音器"},
        {"Cannot load the mixer of %s", "无法加载声卡混音器 %s"},
        {"Mixer opened", "混音器已打开"},
        {"Rear Mic (pink)", "后置麦克风 (粉色)"},
        {"Front Mic", "前置麦克风"},
        {"Line In (blue)", "线路输入 (蓝色)"},
        {"Mic", "麦克风"},
        {"Internal Mic", "内置麦克风"},
        {"Headset Mic", "耳麦麦克风"},
        // RtKit
        {"rtkit refused: %s", "rtkit 拒绝: %s"},
        {"unknown error", "未知错误"},
        {"SCHED_FIFO priority %d (direct)", "SCHED_FIFO 优先级 %d (直接获得)"},
        {"Cannot connect to the system D-Bus: %s", "无法连接系统 D-Bus: %s"},
        {"%s priority %d (via rtkit)", "%s 优先级 %d (通过 rtkit)"},
        {"No real-time priority (%s)", "未获得实时优先级 (%s)"},
        {", nice raised to %d", ", 已提升 nice=%d"},
        // DeviceReservation
        {"Cannot connect to the session D-Bus (%s); opening the card directly", "无法连接会话 D-Bus (%s), 直接尝试打开声卡"},
        {"Failed to register the D-Bus object", "D-Bus 注册对象失败"},
        {"Card reserved (no other program was using it)", "已预留声卡 (当前无其他程序占用)"},
        {"Another program refused to release the card", "其他程序拒绝释放声卡"},
        {"Failed to take over the card reservation", "接管声卡预留失败"},
        {"PulseAudio released the card", "PulseAudio 已让出声卡"},
        // Engine
        {"; CPU low-latency mode on", "; CPU 低延迟模式已开启"},
        {"Failed to start the sound card", "声卡启动失败"},
        {"No-device mode (simulated)", "无声卡模式 (模拟)"},
        {"Unknown effect: %s", "未知效果器: %s"},
        {"%s has no parameter %s", "%s 没有参数 %s"},
        {"Failed to load IR: %s", "IR 加载失败: %s"},
        // PulseBridge
        {"PulseAudio (pactl) not found; system audio passthrough unavailable", "未检测到 PulseAudio (pactl), 系统声音直通不可用"},
        {"Failed to create the virtual output", "创建虚拟输出失败"},
        {"System audio passthrough on: other programs' audio plays through Guitar Studio",
         "系统声音直通已开启: 其他程序的声音经过本软件播放"},
    };
    addTranslations(kZh);
}

}  // namespace gs
