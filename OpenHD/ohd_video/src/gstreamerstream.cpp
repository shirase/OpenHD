#include "gstreamerstream.h"

#include <gst/gst.h>
#include <unistd.h>

#include <iostream>
#include <regex>
#include <vector>

#include "air_recording_helper.hpp"
#include "ffmpeg_videosamples.hpp"
#include "gst_helper.hpp"

#include "gst_appsink_helper.h"
#include "rtp_eof_helper.h"

GStreamerStream::GStreamerStream(PlatformType platform,std::shared_ptr<CameraHolder> camera_holder,
                                 std::shared_ptr<openhd::ITransmitVideo> i_transmit_video)
    //: CameraStream(platform, camera_holder, video_udp_port) {
    : CameraStream(platform,camera_holder,i_transmit_video){
  m_console=openhd::log::create_or_get("v_gststream");
  assert(m_console);
  m_console->debug("GStreamerStream::GStreamerStream()");
  // Since the dummy camera is SW, we generally cannot do more than 640x480@30 anyways.
  // (640x48@30 might already be too much on embedded devices).
  const auto& camera= m_camera_holder->get_camera();
  const auto& setting= m_camera_holder->get_settings();
  if (camera.type == CameraType::DUMMY_SW && (setting.streamed_video_format.width > 640 ||
      setting.streamed_video_format.height > 480 || setting.streamed_video_format.framerate > 30)) {
    m_console->warn("Warning- Dummy camera is done in sw, high resolution/framerate might not work");
    m_console->warn("Configured dummy for:"+setting.streamed_video_format.toString());
  }
  m_camera_holder->register_listener([this](){
    // right now, every time the settings for this camera change, we just re-start the whole stream.
    // That is not ideal, since some cameras support changing for example the bitrate or white balance during operation.
    // But wiring that up is not that easy.
    // We call restart_async() to make sure to not perform heavy operation(s) on the mavlink settings callback, since we need to send the
    // acknowledging response in time. Also, gstreamer and camera(s) are sometimes buggy, so in the worst case gstreamer can become unresponsive
    // and block on the restart operation(s) which would be fatal for telemetry.
    this->restart_async();
  });
  assert(setting.streamed_video_format.isValid());
  OHDGstHelper::initGstreamerOrThrow();
  m_console->debug("GStreamerStream::GStreamerStream done");
}

GStreamerStream::~GStreamerStream() {
  // they are safe to call, regardless if we are already in cleaned up state or not
  GStreamerStream::stop();
  GStreamerStream::cleanup_pipe();
}

void GStreamerStream::setup() {
  m_console->debug("GStreamerStream::setup() begin");
  const auto& camera= m_camera_holder->get_camera();
  const auto& setting= m_camera_holder->get_settings();
  if(!setting.enable_streaming){
    // When streaming is disabled, we just don't create the pipeline. We fully restart on all changes anyways.
    m_console->info("Streaming disabled");
    return;
  }
  m_pipeline_content.str("");
  m_pipeline_content.clear();
  m_bitrate_ctrl_element= nullptr;
  m_curr_dynamic_bitrate_kbits =-1;
  switch (camera.type) {
    case CameraType::RPI_CSI_MMAL: {
      setup_raspberrypi_csi();
      break;
    }
    case CameraType::RPI_CSI_LIBCAMERA: {
      setup_libcamera();
      break;
    }
    case CameraType::JETSON_CSI: {
      setup_jetson_csi();
      break;
    }
    case CameraType::UVC: {
      setup_usb_uvc();
      break;
    }
    case CameraType::UVC_H264: {
      setup_usb_uvch264();
      break;
    }
    case CameraType::IP: {
      setup_ip_camera();
      break;
    }
    case CameraType::DUMMY_SW: {
      setup_sw_dummy_camera();
      break;
    }
    case CameraType::RPI_VEYE_CSI_MMAL:
    case CameraType::ROCKCHIP_CSI:
      m_console->error("Veye and rockchip are unsupported at the time");
      return;
    case CameraType::ROCKCHIP_HDMI: {
      setup_rockchip_hdmi();
      break;
    }
    case CameraType::CUSTOM_UNMANAGED_CAMERA:{
      setup_custom_unmanaged_camera();
      break;
    }break;
    case CameraType::UNKNOWN: {
      m_console->warn( "Unknown camera type");
      return;
    }
  }
  // quick check,here the pipeline should end with a "! ";
  if(!OHDUtil::endsWith(m_pipeline_content.str(),"! ")){
    m_console->error("Probably ill-formatted pipeline:"+
                     m_pipeline_content.str());
  }
  // for safety we only add the tee command at the right place if recording is enabled.
  if(setting.air_recording==Recording::ENABLED && camera.type != CameraType::ROCKCHIP_HDMI){
    m_console->info("Air recording active");
    m_pipeline_content <<"tee name=t ! ";
  }
  // After we've written the parts for the different camera implementation(s) we just need to append the rtp part and the udp out
  // add rtp part
  m_pipeline_content << OHDGstHelper::createRtpForVideoCodec(setting.streamed_video_format.videoCodec);
  // forward data via udp localhost or using appsink and data callback
  //m_pipeline_content << OHDGstHelper::createOutputUdpLocalhost(m_video_udp_port);
  m_pipeline_content << OHDGstHelper::createOutputAppSink();
  if(setting.air_recording==Recording::ENABLED){
    const auto recording_filename=openhd::video::create_unused_recording_filename(
        OHDGstHelper::file_suffix_for_video_codec(setting.streamed_video_format.videoCodec));
    {
      std::stringstream ss;
      ss<<"Using ["<<recording_filename<<"] for recording\n";
      m_console->debug(ss.str());
    }
    m_pipeline_content <<OHDGstHelper::createRecordingForVideoCodec(setting.streamed_video_format.videoCodec,recording_filename);
  }
  m_console->debug("Starting pipeline:"+ m_pipeline_content.str());
  // Protect against unwanted use - stop and free the pipeline first
  assert(m_gst_pipeline == nullptr);
  // Now start the (as a string) built pipeline
  GError *error = nullptr;
  m_gst_pipeline = gst_parse_launch(m_pipeline_content.str().c_str(), &error);
  m_console->debug("GStreamerStream::setup() end");
  m_stream_creation_time=std::chrono::steady_clock::now();
  if (error) {
    m_console->error( "Failed to create pipeline: {}",error->message);
    return;
  }
  if(camera.type==CameraType::RPI_CSI_MMAL){
    m_bitrate_ctrl_element= gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "rpicamsrc");
    m_console->debug("Has bitrate control element: {}",(m_bitrate_ctrl_element!=nullptr) ? "yes":"no");
    m_bitrate_ctrl_element_takes_kbit=false;
  }else if(camera.type==CameraType::DUMMY_SW){
    m_bitrate_ctrl_element= gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "swencoder");
    m_console->debug("Has bitrate control element: {}",(m_bitrate_ctrl_element!=nullptr) ? "yes":"no");
    // sw encoder(s) take kbit/s
    m_bitrate_ctrl_element_takes_kbit= true;
  }
  //test_add_data_listener();
  m_app_sink_element=gst_bin_get_by_name(GST_BIN(m_gst_pipeline), "out_appsink");
  assert(m_app_sink_element);
  m_pull_samples_run= true;
  m_pull_samples_thread=std::make_unique<std::thread>(&GStreamerStream::loop_pull_samples, this);
}

void GStreamerStream::setup_raspberrypi_csi() {
  m_console->debug("Setting up Raspberry Pi CSI camera");
  // similar to jetson, for now we assume there is only one CSI camera connected.
  const auto& setting= m_camera_holder->get_settings();
  m_pipeline_content << OHDGstHelper::createRpicamsrcStream(-1, setting);
}

void GStreamerStream::setup_libcamera() {
  m_console->debug("Setting up Raspberry Pi libcamera camera");
  // similar to jetson, for now we assume there is only one CSI camera
  // connected.
  const auto& setting = m_camera_holder->get_settings();
  m_pipeline_content << OHDGstHelper::createLibcamerasrcStream(
      m_camera_holder->get_camera().name, setting);
}

void GStreamerStream::setup_jetson_csi() {
  m_console->debug("Setting up Jetson CSI camera");
  // Well, i fixed the bug in the detection, with v4l2_open.
  // But still, /dev/video1 can be camera index 0 on jetson.
  // Therefore, for now, we just default to no camera index rn and let nvarguscamerasrc figure out the camera index.
  // This will work as long as there is no more than 1 CSI camera.
  const auto& setting= m_camera_holder->get_settings();
  m_pipeline_content << OHDGstHelper::createJetsonStream(-1,setting);
}

void GStreamerStream::setup_rockchip_hdmi() {
  m_console->debug("Setting up Rockchip HDMI");
  const auto& setting= m_camera_holder->get_settings();
  m_pipeline_content << OHDGstHelper::createRockchipHDMIStream(setting.air_recording==Recording::ENABLED, setting.h26x_bitrate_kbits, setting.streamed_video_format, setting.recordingFormat, setting.h26x_keyframe_interval);
}

void GStreamerStream::setup_usb_uvc() {
  const auto& camera= m_camera_holder->get_camera();
  const auto& setting= m_camera_holder->get_settings();
  m_console->debug("Setting up usb UVC camera Name:"+camera.name+" type:"+camera_type_to_string(camera.type));
  // First we try and start a hw encoded path, where v4l2src directly provides encoded video buffers
  for (const auto &endpoint: camera.endpoints) {
    if (setting.streamed_video_format.videoCodec == VideoCodec::H264 && endpoint.support_h264) {
      m_console->debug("h264");
      const auto device_node = endpoint.device_node;
      m_pipeline_content << OHDGstHelper::createV4l2SrcAlreadyEncodedStream(device_node, setting.streamed_video_format);
      return;
    }
    if (setting.streamed_video_format.videoCodec == VideoCodec::MJPEG && endpoint.support_mjpeg) {
      m_console->debug("MJPEG");
      const auto device_node = endpoint.device_node;
      m_pipeline_content << OHDGstHelper::createV4l2SrcAlreadyEncodedStream(device_node, setting.streamed_video_format);
      return;
    }
  }
  // If we land here, we need to do SW encoding, the v4l2src can only do raw video formats like YUV
  for (const auto &endpoint: camera.endpoints) {
    m_console->warn("Cannot do HW encode for camera, fall back to RAW out and SW encode");
    if (endpoint.support_raw) {
      const auto device_node = endpoint.device_node;
      m_pipeline_content << OHDGstHelper::createV4l2SrcRawAndSwEncodeStream(device_node,
                                                                            setting.streamed_video_format.videoCodec,
                                                                            setting.h26x_bitrate_kbits,setting.h26x_keyframe_interval);
      return;
    }
  }
  // If we land here, we couldn't create a stream for this camera.
  m_console->error("Setup USB UVC failed");
}

void GStreamerStream::setup_usb_uvch264() {
  m_console->debug("Setting up UVC H264 camera");
  const auto& camera= m_camera_holder->get_camera();
  const auto& setting= m_camera_holder->get_settings();
  const auto endpoint = camera.endpoints.front();
  // uvch265 cameras don't seem to exist, codec setting is ignored
  m_pipeline_content << OHDGstHelper::createUVCH264Stream(endpoint.device_node,
                                                          setting.h26x_bitrate_kbits,
                                                          setting.streamed_video_format);
}

void GStreamerStream::setup_ip_camera() {
  m_console->debug("Setting up IP camera");
  const auto& camera= m_camera_holder->get_camera();
  const auto& setting= m_camera_holder->get_settings();
  if (setting.ip_cam_url.empty()) {
    //setting.url = "rtsp://192.168.0.10:554/user=admin&password=&channel=1&stream=0.sdp";
  }
  m_pipeline_content << OHDGstHelper::createIpCameraStream(setting.ip_cam_url);
}

void GStreamerStream::setup_sw_dummy_camera() {
  m_console->debug("Setting up SW dummy camera");
  const auto& camera= m_camera_holder->get_camera();
  const auto& setting= m_camera_holder->get_settings();
  m_pipeline_content << OHDGstHelper::createDummyStream(setting);
}

void GStreamerStream::setup_custom_unmanaged_camera() {
  m_console->debug("Setting up custom unmanaged camera");
  const auto& camera= m_camera_holder->get_camera();
  const auto& setting= m_camera_holder->get_settings();
  m_pipeline_content << OHDGstHelper::create_input_custom_udp_rtp_port(setting);
}

std::string GStreamerStream::createDebug(){
  std::unique_lock<std::mutex> lock(m_pipeline_mutex, std::try_to_lock);
  if(!lock.owns_lock()){
    // We can just discard statistics data during a re-start
    return "GStreamerStream::No debug during restart\n";
  }
  if(!m_camera_holder->get_settings().enable_streaming){
    std::stringstream ss;
    ss << "GStreamerStream for camera:"<< m_camera_holder->get_camera().debugName()<<" disabled";
    return ss.str();
  }
  std::stringstream ss;
  GstState state;
  GstState pending;
  auto returnValue = gst_element_get_state(m_gst_pipeline, &state, &pending, 1000000000);
  ss << "GStreamerStream for camera:"<< m_camera_holder->get_camera().debugName()<<" State:"<< returnValue << "." << state << "." << pending << ".";
  return ss.str();
}

void GStreamerStream::start() {
  m_console->debug("GStreamerStream::start()");
  if(!m_gst_pipeline){
    m_console->warn("gst_pipeline==null");
    return;
  }
  gst_element_set_state(m_gst_pipeline, GST_STATE_PLAYING);
  GstState state;
  GstState pending;
  auto returnValue = gst_element_get_state(m_gst_pipeline, &state, &pending, 1000000000);
  m_console->debug("Gst state: {} {}.{}",returnValue,state,pending);
}

void GStreamerStream::stop() {
  m_console->debug("GStreamerStream::stop()");
  if(!m_gst_pipeline){
    m_console->debug("gst_pipeline==null");
    return;
  }
  gst_element_set_state(m_gst_pipeline, GST_STATE_PAUSED);
}

void GStreamerStream::cleanup_pipe() {
  m_console->debug("GStreamerStream::cleanup_pipe() begin");
  if(m_pull_samples_thread){
    m_pull_samples_run= false;
    if(m_pull_samples_thread->joinable())m_pull_samples_thread->join();
    m_pull_samples_thread= nullptr;
  }
  if(!m_gst_pipeline){
    m_console->debug("gst_pipeline==null");
    return;
  }
  // according to @Alex W we need a EOS signal here to properly shut down the pipeline
  if(!gst_element_send_event (m_gst_pipeline, gst_event_new_eos())){
    m_console->info("error gst_element_send_event eos"); // No idea what that means
  }else{
    m_console->info("success gst_element_send_event eos");
  }
  // TODO wait for the eos event to travel down the pipeline,but do it in a safe manner to not block for infinity
  gst_element_set_state (m_gst_pipeline, GST_STATE_NULL);
  gst_object_unref (m_gst_pipeline);
  m_gst_pipeline =nullptr;
  m_console->debug("GStreamerStream::cleanup_pipe() end");
}

void GStreamerStream::restartIfStopped() {
  std::lock_guard<std::mutex> guard(m_pipeline_mutex);
  if(!m_gst_pipeline){
    m_console->debug("gst_pipeline==null");
    return;
  }
  if(m_camera_holder->get_camera().type==CameraType::CUSTOM_UNMANAGED_CAMERA){
    // this pattern doesn't work here
    return;
  }
  const auto elapsed_since_start=std::chrono::steady_clock::now()-m_stream_creation_time;
  if(elapsed_since_start<std::chrono::seconds(5)){
    // give the cam X seconds in the beginning to properly start before restarting
    return;
  }
  GstState state;
  GstState pending;
  auto returnValue = gst_element_get_state(m_gst_pipeline, &state, &pending, 1000000000); // timeout in ns
  if (returnValue == 0) {
    m_console->debug("Panic gstreamer pipeline state is not running, restarting camera stream for camera:{}",m_camera_holder->get_camera().name);
    // We fully restart the whole pipeline, since some issues might not be fixable by just setting paused
    // This will also show up in QOpenHD (log level >= warn), but we are limited by the n of characters in mavlink
    m_console->warn("Restarting camera, check your parameters / connection");
    stop();
    cleanup_pipe();
    setup();
    start();
    m_console->debug("Restarted");
  }
}

// Restart after a new settings value has been applied
void GStreamerStream::restart_after_new_setting() {
  std::lock_guard<std::mutex> guard(m_pipeline_mutex);
  m_console->debug("GStreamerStream::restart_after_new_setting() begin");
  stop();
  // R.N we need to fully re-set the pipeline if any camera setting has changed
  cleanup_pipe();
  setup();
  start();
  m_console->debug("GStreamerStream::restart_after_new_setting() end");
}

void GStreamerStream::restart_async() {
  std::lock_guard<std::mutex> guard(m_async_thread_mutex);
  // If there is already an async operation running, we need to wait for it to complete.
  // If the user was to change parameters to quickly, this would be a problem.
  if(m_async_thread != nullptr){
    if(m_async_thread->joinable()){
      m_console->info("restart_async: waiting for previous operation to finish");
      m_async_thread->join();
    }
    m_async_thread =nullptr;
  }
  m_async_thread =std::make_unique<std::thread>(&GStreamerStream::restart_after_new_setting,this);
}

void GStreamerStream::handle_change_bitrate_request(openhd::ActionHandler::LinkBitrateInformation lb) {
  std::lock_guard<std::mutex> guard(m_pipeline_mutex);
  const auto max_bitrate_kbits=m_camera_holder->get_settings().h26x_bitrate_kbits;
  auto recommended_encoder_bitrate_kbits=lb.recommended_encoder_bitrate_kbits;
  // pi encoder cannot do less than 1MBit/s anyways
  if(recommended_encoder_bitrate_kbits<=1000){
    recommended_encoder_bitrate_kbits=1000;
  }
  // and the bitrate set by the user has become the max upper level
  if(recommended_encoder_bitrate_kbits>max_bitrate_kbits){
    recommended_encoder_bitrate_kbits=max_bitrate_kbits;
  }
  if(m_curr_dynamic_bitrate_kbits!=recommended_encoder_bitrate_kbits){
    m_console->debug("Changing bitrate to {} kBit/s",recommended_encoder_bitrate_kbits);
    if(try_dynamically_change_bitrate(recommended_encoder_bitrate_kbits)){
      m_curr_dynamic_bitrate_kbits=recommended_encoder_bitrate_kbits;
    }
  }
}

bool GStreamerStream::try_dynamically_change_bitrate(uint32_t bitrate_kbits) {
  if(m_bitrate_ctrl_element!= nullptr){
    if(m_bitrate_ctrl_element_takes_kbit){
      g_object_set(m_bitrate_ctrl_element, "bitrate", bitrate_kbits, NULL);
      gint actual_kbits_per_second;
      g_object_get(m_bitrate_ctrl_element,"bitrate",&actual_kbits_per_second,NULL);
      m_console->debug("try_dynamically_change_bitrate wanted:{} kBit/s set:{} kBit/s",bitrate_kbits,actual_kbits_per_second);
      return true;
    }else{
      //rpicamsrc for example takes bit/s instead of kbit/s
      const int bitrate_bits_per_second = kbits_to_bits_per_second(bitrate_kbits);
      g_object_set(m_bitrate_ctrl_element, "bitrate", bitrate_bits_per_second, NULL);
      gint actual_bits_per_second;
      g_object_get(m_bitrate_ctrl_element,"bitrate",&actual_bits_per_second,NULL);
      m_console->debug("try_dynamically_change_bitrate wanted:{} kBit/s set:{} kBit/s",
                       bits_per_second_to_kbits_per_second(bitrate_bits_per_second),
                       bits_per_second_to_kbits_per_second(actual_bits_per_second));
      return true;
    }
  }
  m_console->warn("camera {} does not support dynamic bitrate control",m_camera_holder->get_camera().index);
  return false;
}

void GStreamerStream::on_new_rtp_fragmented_frame(std::vector<std::shared_ptr<std::vector<uint8_t>>> frame_fragments) {
  //m_console->debug("Got frame with {} fragments",frame_fragments.size());
  if(m_transmit_interface){
    m_transmit_interface->transmit_video_data(0,openhd::FragmentedVideoFrame{frame_fragments});
  }else{
    m_console->debug("No transmit interface");
  }
}

void GStreamerStream::on_new_rtp_frame_fragment(std::shared_ptr<std::vector<uint8_t>> fragment,uint64_t dts) {
  m_frame_fragments.push_back(fragment);
  const auto curr_video_codec=m_camera_holder->get_settings().streamed_video_format.videoCodec;
  bool is_last_fragment_of_frame=false;
  if(curr_video_codec==VideoCodec::H264){
    if(openhd::rtp_eof_helper::h264_end_block(fragment->data(),fragment->size())){
      is_last_fragment_of_frame= true;
    }
  }else if(curr_video_codec==VideoCodec::H265){
    if(openhd::rtp_eof_helper::h265_end_block(fragment->data(),fragment->size())){
      is_last_fragment_of_frame= true;
    }
  }else{
    // Not supported yet, forward them in chuncks of 20 (NOTE: This workaround is not ideal, since it creates ~1 frame of latency).
    is_last_fragment_of_frame=m_frame_fragments.size()>=20;
  }
  if(is_last_fragment_of_frame || m_frame_fragments.size()>1000){
    on_new_rtp_fragmented_frame(m_frame_fragments);
    m_frame_fragments.resize(0);
  }
}

void GStreamerStream::loop_pull_samples() {
  assert(m_app_sink_element);
  auto cb=[this](std::shared_ptr<std::vector<uint8_t>> fragment,uint64_t dts){
    on_new_rtp_frame_fragment(fragment,dts);
  };
  openhd::loop_pull_appsink_samples(m_pull_samples_run,m_app_sink_element,cb);
}
