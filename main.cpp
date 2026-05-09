#include <iostream>
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/logging.h"
// #include "rtc_base/thread.h"

// #include "api/peer_connection_interface.h"
// #include "api/create_peerconnection_factory.h"
// #include "api/audio_codecs/builtin_audio_encoder_factory.h"
// #include "api/audio_codecs/builtin_audio_decoder_factory.h"
// #include "modules/audio_processing/include/audio_processing.h"
// #include "modules/audio_device/include/audio_device.h"

#include "ui/widg.h"
#include "module/pushclient.h"
#include "module/signaling_client.h"
#include <QApplication>
#include <QCoreApplication>
#include <QFileInfo>
#include <QUrl>
#include <QStringList>

#include "modules/desktop_capture/desktop_capturer.h"
#include "modules/desktop_capture/desktop_capture_options.h"
#include "modules/desktop_capture/desktop_frame.h"
#include "modules/desktop_capture/screen_capturer_helper.h"

#if defined(TWEBRTC_PLATFORM_LINUX)
#include <X11/Xlib.h>
#endif
#include <thread>
#include <fstream>
namespace
{

bool InitializePlatformRuntime(bool autostart)
{
#if defined(TWEBRTC_PLATFORM_LINUX)
  const QByteArray sessionType = qgetenv("XDG_SESSION_TYPE");
  const QByteArray waylandDisplay = qgetenv("WAYLAND_DISPLAY");
  if (!autostart && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") &&
      (sessionType == "wayland" || !waylandDisplay.isEmpty()))
  {
    const QString pluginRoot = qEnvironmentVariable("QT_PLUGIN_PATH");
    const QFileInfo waylandPlugin(pluginRoot + "/platforms/libqwayland-generic.so");
    if (waylandPlugin.exists())
    {
      qputenv("QT_QPA_PLATFORM", QByteArray("wayland"));
    }
  }

  const QByteArray platformName = qgetenv("QT_QPA_PLATFORM");
  const bool useWaylandQt = platformName.startsWith("wayland");
  if (!useWaylandQt && !XInitThreads())
  {
    RTC_LOG(LS_ERROR) << "Failed to initialize XInitThreads";
    return false;
  }

  return true;
#elif defined(TWEBRTC_PLATFORM_WINDOWS)
  static_cast<void>(autostart);
  RTC_LOG(LS_INFO) << "Windows runtime initialization placeholder active.";
  return true;
#elif defined(TWEBRTC_PLATFORM_MACOS)
  static_cast<void>(autostart);
  RTC_LOG(LS_INFO) << "macOS runtime initialization placeholder active.";
  return true;
#else
  static_cast<void>(autostart);
  RTC_LOG(LS_WARNING) << "Running on an unverified platform; using default runtime initialization.";
  return true;
#endif
}

} // namespace

int main(int argc, char *argv[])
{
#if 0
    std::cout << "[webrtc-smoke] SSL initialized" << std::endl;

    //webrtc连接到一个信令服务器

#endif

#if 1
  bool autostart = false;
  for (int index = 1; index < argc; ++index)
  {
    if (QString::fromLocal8Bit(argv[index]) == "--autostart")
    {
      autostart = true;
      break;
    }
  }
#endif

  webrtc::LogMessage::SetLogToStderr(true);
  // webrtc::LogMessage::LogToDebug(webrtc::LS_INFO); // 或者 rtc::LS_INFO
  std::cout << "[webrtc-smoke] Start" << std::endl;

  if (!webrtc::InitializeSSL())
  {
    RTC_LOG(LS_ERROR) << "Failed to initialize SSL";
    return 2;
  }

  if (!InitializePlatformRuntime(autostart))
  {
    webrtc::CleanupSSL();
    return 3;
  }

  if (autostart)
  {
    QCoreApplication app(argc, argv);
    SignalingClient signalingClient;
    signalingClient.connectToServer("ws://localhost:8000/server");
    std::cout << "Headless autostart mode" << std::endl;
    const int exitCode = app.exec();
    webrtc::CleanupSSL();
    return exitCode;
  }

  QApplication app(argc, argv);

  widg window(false);
  window.show();

  // WebRTCPushClient rtcClient;

  // // 1. 初始化 WebRTC
  // std::vector<IceServerConfig> iceServers = {
  //     {"stun:stun.l.google.com:19302", "", ""}};
  // if (!rtcClient.Init(iceServers))
  // {
  //   std::cerr << "WebRTC Init failed" << std::endl;
  //   return -1;
  // }

  // // 2. 添加视频源
  // if (!rtcClient.AddDesktopVideo(30, 2000000))
  // {
  //   std::cerr << "Add Video failed" << std::endl;
  //   return -1;
  // }

  // 3. 创建信令客户端并连接
  // 假设你的信令服务器地址是 ws://localhost:8000
  // SignalingClient sigClient;
  // sigClient.connectToServer("ws://localhost:8000/server");

  std::cout << "Hello, World!" << std::endl;
  app.exec();
  webrtc::CleanupSSL();
  return 0;
}