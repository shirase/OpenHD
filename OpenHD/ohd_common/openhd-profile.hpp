#ifndef OPENHD_PROFILE_H
#define OPENHD_PROFILE_H

#include <string>
#include <utility>
#include <vector>
#include <sstream>
#include <fstream>

#include "include_json.hpp"
#include "openhd-util.hpp"
#include "openhd-settings.hpp"
#include "openhd-spdlog.hpp"

/**
 * The profile is created on startup and then doesn't change during run time.
 * Note that while the unit id never changes between successive re-boots of OpenHD,
 * the is_air variable might change, but not during run time
 * (aka a ground pi might become an air pi when the user switches the SD card around).
 */
class OHDProfile {
 public:
  explicit OHDProfile(bool is_air,bool started_from_service1,std::string unit_id1):
  is_air(is_air),developer_mode(started_from_service1),unit_id(std::move(unit_id1)){};
  // Weather we run on an air or ground "pi" (air or ground system).
  // R.n this is determined by checking if there is at least one camera connected to the system
  // or by using the force_air (development) variable.
  const bool is_air;
  // The unique id of this system, it is created once then never changed again.
  const std::string unit_id;
  // we handle errors / missing hardware slightly different depending on how openhd was started
  const bool developer_mode;
  [[nodiscard]] bool is_ground()const{
    return !is_air;
  }
  // Wait for at least one card that does wifi-broadcast on startup
  [[nodiscard]] bool wait_for_wifi_cards()const{
    return developer_mode;
  }

  [[nodiscard]] std::string to_string()const{
	std::stringstream ss;
	ss<<"OHDProfile{"<<(is_air ? "Air":"Ground")<<":"<<unit_id<<
            " developer_mode:"<<OHDUtil::yes_or_no(developer_mode)<<"}";
	return ss.str();
  }
};

namespace DProfile{

static std::shared_ptr<OHDProfile>  discover(bool is_air,bool developer_mode) {
  openhd::log::get_default()->debug("Profile::discover()");
  // We read the unit id from the persistent storage, later write it to the tmp storage json
  const auto unit_id = getOrCreateUnitId();
  // We are air pi if there is at least one camera
  auto ret=std::make_shared<OHDProfile>(is_air,developer_mode,unit_id);
  return ret;
}

}
#endif

