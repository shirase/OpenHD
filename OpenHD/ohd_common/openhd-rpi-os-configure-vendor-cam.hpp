//
// Created by consti10 on 24.10.22.
//

#ifndef OPENHD_OPENHD_OHD_COMMON_OPENHD_RPI_OS_CONFIGURE_VENDOR_CAM_HPP_
#define OPENHD_OPENHD_OHD_COMMON_OPENHD_RPI_OS_CONFIGURE_VENDOR_CAM_HPP_

#include "openhd-util.hpp"

// Helper to reconfigure the rpi os for the different camera types
namespace openhd::rpi::os{
static void OHDRpiConfigClear(){
  OHDUtil::run_command("sed -i '/camera_auto_detect=1/d' /boot/config.txt",{});
  OHDUtil::run_command("sed -i '/dtoverlay=vc4-kms-v3d/d' /boot/config.txt",{});
  OHDUtil::run_command("sed -i '/dtoverlay=vc4-fkms-v3d/d' /boot/config.txt",{});
  OHDUtil::run_command("sed -i '/start_x=1/d' /boot/config.txt",{});
  OHDUtil::run_command("sed -i '/enable_uart=1/d' /boot/config.txt",{});
  OHDUtil::run_command("sed -i '/dtoverlay=arducam-pivariety/d' /boot/config.txt",{});
};

static void OHDRpiConfigExecute(){
  OHDUtil::run_command("rm -Rf /boot/OpenHD/libcamera.txt",{});
  OHDUtil::run_command("rm -Rf /boot/OpenHD/raspicamsrc.txt",{});
  OHDUtil::run_command("rm -Rf /boot/OpenHD/arducam.txt",{});
  OHDUtil::run_command("echo",{"This device will now reboot to enable configs"});
  OHDUtil::run_command("reboot",{});
};

static void OHDRpiConfigLibcamera(){
  OHDUtil::run_command("sed -i '$ a camera_auto_detect=1' /boot/config.txt",{});
  OHDUtil::run_command("sed -i '$ a dtoverlay=vc4-kms-v3d' /boot/config.txt",{});
  OHDUtil::run_command("sed -i '$ a enable_uart=1' /boot/config.txt",{});
};

static void OHDRpiConfigRaspicam(){
  OHDUtil::run_command("sed -i '$ a start_x=1' /boot/config.txt",{});
  OHDUtil::run_command("sed -i '$ a dtoverlay=vc4-fkms-v3d' /boot/config.txt",{});
  OHDUtil::run_command("sed -i '$ a enable_uart=1' /boot/config.txt",{});
};

static void OHDRpiConfigArducam(){
  OHDUtil::run_command("sed -i '$ a dtoverlay=arducam-pivariety' /boot/config.txt",{});
};

enum class CamConfig {
  MMAL = 0, // raspivid / gst-rpicamsrc
  LIBCAMERA, // "normal" libcamera
  LIBCAMERA_ARDUCAM, // pivariety libcamera (arducam special)
};
static CamConfig cam_config_from_string(const std::string& config){
  if(OHDUtil::equal_after_uppercase(config,"mmal")){
    return CamConfig::MMAL;
  }else if(OHDUtil::startsWith(config,"libcamera")){
    return CamConfig::LIBCAMERA;
  }else if(OHDUtil::equal_after_uppercase(config,"libcamera_arducam")){
    return CamConfig::LIBCAMERA_ARDUCAM;
  }
  std::cerr<<"cam_config_from_string error\n";
  // return default
  return CamConfig::MMAL;
}
static std::string cam_config_to_string(const CamConfig& cam_config){
  if(cam_config==CamConfig::MMAL){
    return "mmal";
  }else if(cam_config==CamConfig::LIBCAMERA){
    return "libcamera";
  }
  return "libcamera_arducam";
}

static bool validate_cam_config_settings_string(const std::string s){
  return s=="mmal" || s=="libcamera" || s=="libcamera_arducam";
}

static constexpr auto CAM_CONFIG_FILENAME="/boot/OpenHD/rpi_cam_config.txt";

static CamConfig get_current_cam_config(){
  OHDFilesystemUtil::create_directories("/boot/OpenHD/");
  if(!OHDFilesystemUtil::exists(CAM_CONFIG_FILENAME)){
    // The OHD image builder defaults to mmal, NOTE this is in contrast to the default rpi os release.
    OHDFilesystemUtil::write_file(CAM_CONFIG_FILENAME, cam_config_to_string(CamConfig::MMAL));
    return CamConfig::MMAL;
  }
  auto content=OHDFilesystemUtil::read_file(CAM_CONFIG_FILENAME);
  return cam_config_from_string(content);
}

static void update_current_cam_config(CamConfig new_cam_config){
  OHDFilesystemUtil::create_directories("/boot/OpenHD/");
  OHDFilesystemUtil::write_file(CAM_CONFIG_FILENAME, cam_config_to_string(new_cam_config));
}


}
#endif  // OPENHD_OPENHD_OHD_COMMON_OPENHD_RPI_OS_CONFIGURE_VENDOR_CAM_HPP_
