#include "modules/wireplumber.hpp"

#include <spdlog/spdlog.h>

bool isValidNodeId(uint32_t id) { return id > 0 && id < G_MAXUINT32; }

wabar::modules::Wireplumber::Wireplumber(const std::string& id, const Json::Value& config)
    : ALabel(config, "wireplumber", id, "{volume}%"),
      wp_core_(nullptr),
      apis_(nullptr),
      om_(nullptr),
      mixer_api_(nullptr),
      def_nodes_api_(nullptr),
      default_node_name_(nullptr),
      pending_plugins_(0),
      muted_(false),
      volume_(0.0),
      min_step_(0.0),
      node_id_(0) {
  wp_init(WP_INIT_PIPEWIRE);
  wp_core_ = wp_core_new(nullptr, nullptr, nullptr);
  apis_ = g_ptr_array_new_with_free_func(g_object_unref);
  om_ = wp_object_manager_new();

  prepare();

  spdlog::debug("[{}]: connecting to pipewire...", name_);

  if (wp_core_connect(wp_core_) == 0) {
    spdlog::error("[{}]: Could not connect to PipeWire", name_);
    throw std::runtime_error("Could not connect to PipeWire\n");
  }

  spdlog::debug("[{}]: connected!", name_);

  g_signal_connect_swapped(om_, "installed", (GCallback)onObjectManagerInstalled, this);

  asyncLoadRequiredApiModules();
}

wabar::modules::Wireplumber::~Wireplumber() {
  wp_core_disconnect(wp_core_);
  g_clear_pointer(&apis_, g_ptr_array_unref);
  g_clear_object(&om_);
  g_clear_object(&wp_core_);
  g_clear_object(&mixer_api_);
  g_clear_object(&def_nodes_api_);
  g_free(default_node_name_);
}

void wabar::modules::Wireplumber::updateNodeName(wabar::modules::Wireplumber* self, uint32_t id) {
  spdlog::debug("[{}]: updating node name with node.id {}", self->name_, id);

  if (!isValidNodeId(id)) {
    spdlog::warn("[{}]: '{}' is not a valid node ID. Ignoring node name update.", self->name_, id);
    return;
  }

  auto* proxy = static_cast<WpProxy*>(wp_object_manager_lookup(self->om_, WP_TYPE_GLOBAL_PROXY,
                                                               WP_CONSTRAINT_TYPE_G_PROPERTY,
                                                               "bound-id", "=u", id, nullptr));

  if (proxy == nullptr) {
    auto err = fmt::format("Object '{}' not found\n", id);
    spdlog::error("[{}]: {}", self->name_, err);
    throw std::runtime_error(err);
  }

  g_autoptr(WpProperties) properties =
      WP_IS_PIPEWIRE_OBJECT(proxy) != 0
          ? wp_pipewire_object_get_properties(WP_PIPEWIRE_OBJECT(proxy))
          : wp_properties_new_empty();
  g_autoptr(WpProperties) globalP = wp_global_proxy_get_global_properties(WP_GLOBAL_PROXY(proxy));
  properties = wp_properties_ensure_unique_owner(properties);
  wp_properties_add(properties, globalP);
  wp_properties_set(properties, "object.id", nullptr);
  const auto* nick = wp_properties_get(properties, "node.nick");
  const auto* description = wp_properties_get(properties, "node.description");

  self->node_name_ = nick != nullptr          ? nick
                     : description != nullptr ? description
                                              : "Unknown node name";
  spdlog::debug("[{}]: Updating node name to: {}", self->name_, self->node_name_);
}

void wabar::modules::Wireplumber::updateVolume(wabar::modules::Wireplumber* self, uint32_t id) {
  spdlog::debug("[{}]: updating volume", self->name_);
  GVariant* variant = nullptr;

  if (!isValidNodeId(id)) {
    spdlog::error("[{}]: '{}' is not a valid node ID. Ignoring volume update.", self->name_, id);
    return;
  }

  g_signal_emit_by_name(self->mixer_api_, "get-volume", id, &variant);

  if (variant == nullptr) {
    auto err = fmt::format("Node {} does not support volume\n", id);
    spdlog::error("[{}]: {}", self->name_, err);
    throw std::runtime_error(err);
  }

  g_variant_lookup(variant, "volume", "d", &self->volume_);
  g_variant_lookup(variant, "step", "d", &self->min_step_);
  g_variant_lookup(variant, "mute", "b", &self->muted_);
  g_clear_pointer(&variant, g_variant_unref);

  self->dp.emit();
}

void wabar::modules::Wireplumber::onMixerChanged(wabar::modules::Wireplumber* self, uint32_t id) {
  spdlog::debug("[{}]: (onMixerChanged) - id: {}", self->name_, id);

  g_autoptr(WpNode) node = static_cast<WpNode*>(wp_object_manager_lookup(
      self->om_, WP_TYPE_NODE, WP_CONSTRAINT_TYPE_G_PROPERTY, "bound-id", "=u", id, nullptr));

  if (node == nullptr) {
    spdlog::warn("[{}]: (onMixerChanged) - Object with id {} not found", self->name_, id);
    return;
  }

  const gchar* name = wp_pipewire_object_get_property(WP_PIPEWIRE_OBJECT(node), "node.name");

  if (self->node_id_ != id) {
    spdlog::debug(
        "[{}]: (onMixerChanged) - ignoring mixer update for node: id: {}, name: {} as it is not "
        "the default node: {} with id: {}",
        self->name_, id, name, self->default_node_name_, self->node_id_);
    return;
  }

  spdlog::debug("[{}]: (onMixerChanged) - Need to update volume for node with id {} and name {}",
                self->name_, id, name);
  updateVolume(self, id);
}

void wabar::modules::Wireplumber::onDefaultNodesApiChanged(wabar::modules::Wireplumber* self) {
  spdlog::debug("[{}]: (onDefaultNodesApiChanged)", self->name_);

  uint32_t defaultNodeId;
  g_signal_emit_by_name(self->def_nodes_api_, "get-default-node", "Audio/Sink", &defaultNodeId);

  if (!isValidNodeId(defaultNodeId)) {
    spdlog::warn("[{}]: '{}' is not a valid node ID. Ignoring node change.", self->name_,
                 defaultNodeId);
    return;
  }

  g_autoptr(WpNode) node = static_cast<WpNode*>(
      wp_object_manager_lookup(self->om_, WP_TYPE_NODE, WP_CONSTRAINT_TYPE_G_PROPERTY, "bound-id",
                               "=u", defaultNodeId, nullptr));

  if (node == nullptr) {
    spdlog::warn("[{}]: (onDefaultNodesApiChanged) - Object with id {} not found", self->name_,
                 defaultNodeId);
    return;
  }

  const gchar* defaultNodeName =
      wp_pipewire_object_get_property(WP_PIPEWIRE_OBJECT(node), "node.name");

  spdlog::debug(
      "[{}]: (onDefaultNodesApiChanged) - got the following default node: Node(name: {}, id: {})",
      self->name_, defaultNodeName, defaultNodeId);

  if (g_strcmp0(self->default_node_name_, defaultNodeName) == 0) {
    spdlog::debug(
        "[{}]: (onDefaultNodesApiChanged) - Default node has not changed. Node(name: {}, id: {}). "
        "Ignoring.",
        self->name_, self->default_node_name_, defaultNodeId);
    return;
  }

  spdlog::debug(
      "[{}]: (onDefaultNodesApiChanged) - Default node changed to -> Node(name: {}, id: {})",
      self->name_, defaultNodeName, defaultNodeId);

  g_free(self->default_node_name_);
  self->default_node_name_ = g_strdup(defaultNodeName);
  self->node_id_ = defaultNodeId;
  updateVolume(self, defaultNodeId);
  updateNodeName(self, defaultNodeId);
}

void wabar::modules::Wireplumber::onObjectManagerInstalled(wabar::modules::Wireplumber* self) {
  spdlog::debug("[{}]: onObjectManagerInstalled", self->name_);

  self->def_nodes_api_ = wp_plugin_find(self->wp_core_, "default-nodes-api");

  if (self->def_nodes_api_ == nullptr) {
    spdlog::error("[{}]: default nodes api is not loaded.", self->name_);
    throw std::runtime_error("Default nodes API is not loaded\n");
  }

  self->mixer_api_ = wp_plugin_find(self->wp_core_, "mixer-api");

  if (self->mixer_api_ == nullptr) {
    spdlog::error("[{}]: mixer api is not loaded.", self->name_);
    throw std::runtime_error("Mixer api is not loaded\n");
  }

  g_signal_emit_by_name(self->def_nodes_api_, "get-default-configured-node-name", "Audio/Sink",
                        &self->default_node_name_);
  g_signal_emit_by_name(self->def_nodes_api_, "get-default-node", "Audio/Sink", &self->node_id_);

  if (self->default_node_name_ != nullptr) {
    spdlog::debug("[{}]: (onObjectManagerInstalled) - default configured node name: {} and id: {}",
                  self->name_, self->default_node_name_, self->node_id_);
  }

  updateVolume(self, self->node_id_);
  updateNodeName(self, self->node_id_);

  g_signal_connect_swapped(self->mixer_api_, "changed", (GCallback)onMixerChanged, self);
  g_signal_connect_swapped(self->def_nodes_api_, "changed", (GCallback)onDefaultNodesApiChanged,
                           self);
}

void wabar::modules::Wireplumber::onPluginActivated(WpObject* p, GAsyncResult* res,
                                                     wabar::modules::Wireplumber* self) {
  const auto* pluginName = wp_plugin_get_name(WP_PLUGIN(p));
  spdlog::debug("[{}]: onPluginActivated: {}", self->name_, pluginName);
  g_autoptr(GError) error = nullptr;

  if (wp_object_activate_finish(p, res, &error) == 0) {
    spdlog::error("[{}]: error activating plugin: {}", self->name_, error->message);
    throw std::runtime_error(error->message);
  }

  if (--self->pending_plugins_ == 0) {
    wp_core_install_object_manager(self->wp_core_, self->om_);
  }
}

void wabar::modules::Wireplumber::activatePlugins() {
  spdlog::debug("[{}]: activating plugins", name_);
  for (uint16_t i = 0; i < apis_->len; i++) {
    WpPlugin* plugin = static_cast<WpPlugin*>(g_ptr_array_index(apis_, i));
    pending_plugins_++;
    wp_object_activate(WP_OBJECT(plugin), WP_PLUGIN_FEATURE_ENABLED, nullptr,
                       (GAsyncReadyCallback)onPluginActivated, this);
  }
}

void wabar::modules::Wireplumber::prepare() {
  spdlog::debug("[{}]: preparing object manager", name_);
  wp_object_manager_add_interest(om_, WP_TYPE_NODE, WP_CONSTRAINT_TYPE_PW_PROPERTY, "media.class",
                                 "=s", "Audio/Sink", nullptr);
}

void wabar::modules::Wireplumber::onDefaultNodesApiLoaded(WpObject* p, GAsyncResult* res,
                                                           wabar::modules::Wireplumber* self) {
  gboolean success = FALSE;
  g_autoptr(GError) error = nullptr;

  spdlog::debug("[{}]: callback loading default node api module", self->name_);

  success = wp_core_load_component_finish(self->wp_core_, res, &error);

  if (success == FALSE) {
    spdlog::error("[{}]: default nodes API load failed", self->name_);
    throw std::runtime_error(error->message);
  }
  spdlog::debug("[{}]: loaded default nodes api", self->name_);
  g_ptr_array_add(self->apis_, wp_plugin_find(self->wp_core_, "default-nodes-api"));

  spdlog::debug("[{}]: loading mixer api module", self->name_);
  wp_core_load_component(self->wp_core_, "libwireplumber-module-mixer-api", "module", nullptr,
                         "mixer-api", nullptr, (GAsyncReadyCallback)onMixerApiLoaded, self);
}

void wabar::modules::Wireplumber::onMixerApiLoaded(WpObject* p, GAsyncResult* res,
                                                    wabar::modules::Wireplumber* self) {
  gboolean success = FALSE;
  g_autoptr(GError) error = nullptr;

  success = wp_core_load_component_finish(self->wp_core_, res, nullptr);

  if (success == FALSE) {
    spdlog::error("[{}]: mixer API load failed", self->name_);
    throw std::runtime_error(error->message);
  }

  spdlog::debug("[{}]: loaded mixer API", self->name_);
  g_ptr_array_add(self->apis_, ({
                    WpPlugin* p = wp_plugin_find(self->wp_core_, "mixer-api");
                    g_object_set(G_OBJECT(p), "scale", 1 /* cubic */, nullptr);
                    p;
                  }));

  self->activatePlugins();

  self->dp.emit();

  self->event_box_.add_events(Gdk::SCROLL_MASK | Gdk::SMOOTH_SCROLL_MASK);
  self->event_box_.signal_scroll_event().connect(sigc::mem_fun(*self, &Wireplumber::handleScroll));
}

void wabar::modules::Wireplumber::asyncLoadRequiredApiModules() {
  spdlog::debug("[{}]: loading default nodes api module", name_);
  wp_core_load_component(wp_core_, "libwireplumber-module-default-nodes-api", "module", nullptr,
                         "default-nodes-api", nullptr, (GAsyncReadyCallback)onDefaultNodesApiLoaded,
                         this);
}

auto wabar::modules::Wireplumber::update() -> void {
  auto format = format_;
  std::string tooltipFormat;

  if (muted_) {
    format = config_["format-muted"].isString() ? config_["format-muted"].asString() : format;
    label_.get_style_context()->add_class("muted");
  } else {
    label_.get_style_context()->remove_class("muted");
  }

  int vol = round(volume_ * 100.0);
  std::string markup = fmt::format(fmt::runtime(format), fmt::arg("node_name", node_name_),
                                   fmt::arg("volume", vol), fmt::arg("icon", getIcon(vol)));
  label_.set_markup(markup);

  getState(vol);

  if (tooltipEnabled()) {
    if (tooltipFormat.empty() && config_["tooltip-format"].isString()) {
      tooltipFormat = config_["tooltip-format"].asString();
    }

    if (!tooltipFormat.empty()) {
      label_.set_tooltip_text(fmt::format(fmt::runtime(tooltipFormat),
                                          fmt::arg("node_name", node_name_),
                                          fmt::arg("volume", vol), fmt::arg("icon", getIcon(vol))));
    } else {
      label_.set_tooltip_text(node_name_);
    }
  }

  // Call parent update
  ALabel::update();
}

bool wabar::modules::Wireplumber::handleScroll(GdkEventScroll* e) {
  if (config_["on-scroll-up"].isString() || config_["on-scroll-down"].isString()) {
    return AModule::handleScroll(e);
  }
  auto dir = AModule::getScrollDir(e);
  if (dir == SCROLL_DIR::NONE) {
    return true;
  }
  double maxVolume = 1;
  double step = 1.0 / 100.0;
  if (config_["scroll-step"].isDouble()) {
    step = config_["scroll-step"].asDouble() / 100.0;
  }
  if (config_["max-volume"].isDouble()) {
    maxVolume = config_["max-volume"].asDouble() / 100.0;
  }

  if (step < min_step_) step = min_step_;

  double newVol = volume_;
  if (dir == SCROLL_DIR::UP) {
    if (volume_ < maxVolume) {
      newVol = volume_ + step;
      if (newVol > maxVolume) newVol = maxVolume;
    }
  } else if (dir == SCROLL_DIR::DOWN) {
    if (volume_ > 0) {
      newVol = volume_ - step;
      if (newVol < 0) newVol = 0;
    }
  }
  if (newVol != volume_) {
    GVariant* variant = g_variant_new_double(newVol);
    gboolean ret;
    g_signal_emit_by_name(mixer_api_, "set-volume", node_id_, variant, &ret);
  }
  return true;
}
