#include "AIconLabel.hpp"

#include <gdkmm/pixbuf.h>

namespace wabar {

AIconLabel::AIconLabel(const Json::Value &config, const std::string &name, const std::string &id,
                       const std::string &format, uint16_t interval, bool ellipsize,
                       bool enable_click, bool enable_scroll)
    : ALabel(config, name, id, format, interval, ellipsize, enable_click, enable_scroll) {
  event_box_.remove();
  label_.unset_name();
  label_.get_style_context()->remove_class(MODULE_CLASS);
  box_.get_style_context()->add_class(MODULE_CLASS);
  if (!id.empty()) {
    label_.get_style_context()->remove_class(id);
    box_.get_style_context()->add_class(id);
  }

  box_.set_orientation(Gtk::Orientation::ORIENTATION_HORIZONTAL);
  box_.set_name(name);

  int spacing = config_["icon-spacing"].isInt() ? config_["icon-spacing"].asInt() : 8;
  box_.set_spacing(spacing);

  box_.add(image_);
  box_.add(label_);

  event_box_.add(box_);
}

auto AIconLabel::update() -> void {
  image_.set_visible(image_.get_visible() && iconEnabled());
  ALabel::update();
}

bool AIconLabel::iconEnabled() const {
  return config_["icon"].isBool() ? config_["icon"].asBool() : false;
}

}  // namespace wabar
