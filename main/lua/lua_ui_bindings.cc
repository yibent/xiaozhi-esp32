#include "lua_ui_bindings.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "application.h"
#include "board.h"
#include "display.h"

#include <freertos/queue.h>
#include <freertos/semphr.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {

constexpr char kObjectMetatable[] = "xiaozhi.ui.object";
constexpr int kDisplayTaskTimeoutMs = 5000;
constexpr int kMaxEventWaitMs = 1000;
constexpr size_t kEventQueueLength = 16;

#ifdef HAVE_LVGL

enum class WidgetKind {
    Screen,
    Container,
    Label,
    Button,
    Bar,
    Slider,
    Arc,
    Switch,
    Checkbox,
    Dropdown,
    Roller,
    Textarea,
    Image,
    Line,
    Table,
    Spinner,
    Led,
    Chart,
};

struct UiOptions {
    std::string id;
    std::string text;
    std::string options;
    std::string src;
    std::string align;
    std::string flex;
    std::vector<lv_point_precise_t> points;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int min = 0;
    int max = 100;
    int value = 0;
    int bg_color = 0;
    int text_color = 0;
    int border_color = 0;
    int bg_opa = LV_OPA_COVER;
    int radius = 0;
    int border_width = 0;
    int pad = 0;
    int pad_row = 0;
    int pad_column = 0;
    bool has_x = false;
    bool has_y = false;
    bool has_width = false;
    bool has_height = false;
    bool has_min = false;
    bool has_max = false;
    bool has_value = false;
    bool has_bg_color = false;
    bool has_text_color = false;
    bool has_border_color = false;
    bool has_bg_opa = false;
    bool has_radius = false;
    bool has_border_width = false;
    bool has_pad = false;
    bool has_pad_row = false;
    bool has_pad_column = false;
    bool hidden = false;
    bool has_hidden = false;
    bool clickable = false;
    bool has_clickable = false;
    bool scrollable = false;
    bool has_scrollable = false;
    bool checked = false;
    bool has_checked = false;
    bool events = false;
    bool has_events = false;
    bool one_line = false;
    bool has_one_line = false;
};

struct Widget {
    std::string id;
    std::string parent_id;
    std::string source;
    std::vector<lv_point_precise_t> points;
    WidgetKind kind;
    lv_obj_t* object = nullptr;
};

struct ObjectUserdata {
    std::string id;
};

struct UiEvent {
    char id[48];
    char type[24];
    char text[128];
    int value;
    bool checked;
};

struct ScheduledResult {
    ScheduledResult() : done(xSemaphoreCreateBinary()) {}
    ~ScheduledResult() {
        if (done != nullptr)
            vSemaphoreDelete(done);
    }

    SemaphoreHandle_t done;
    std::atomic<int> state{0};
    bool success = false;
    std::string error;
};

bool ReadIntegerField(lua_State* state, int table_index, const char* key, int* value,
                      bool* present, std::string* error) {
    lua_getfield(state, table_index, key);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_isnumber(state, -1)) {
        *error = std::string("ui option '") + key + "' must be a number";
        lua_pop(state, 1);
        return false;
    }
    *value = static_cast<int>(lua_tointeger(state, -1));
    *present = true;
    lua_pop(state, 1);
    return true;
}

bool ReadBooleanField(lua_State* state, int table_index, const char* key, bool* value,
                      bool* present, std::string* error) {
    lua_getfield(state, table_index, key);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_isboolean(state, -1)) {
        *error = std::string("ui option '") + key + "' must be a boolean";
        lua_pop(state, 1);
        return false;
    }
    *value = lua_toboolean(state, -1);
    *present = true;
    lua_pop(state, 1);
    return true;
}

bool ReadStringField(lua_State* state, int table_index, const char* key, std::string* value,
                     std::string* error) {
    lua_getfield(state, table_index, key);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_isstring(state, -1)) {
        *error = std::string("ui option '") + key + "' must be a string";
        lua_pop(state, 1);
        return false;
    }
    size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    value->assign(text, length);
    lua_pop(state, 1);
    return true;
}

bool ParseColor(lua_State* state, int table_index, const char* key, int* value, bool* present,
                std::string* error) {
    lua_getfield(state, table_index, key);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    if (lua_isnumber(state, -1)) {
        *value = static_cast<int>(lua_tointeger(state, -1));
    } else if (lua_isstring(state, -1)) {
        const char* color = lua_tostring(state, -1);
        if (color == nullptr || color[0] == '\0') {
            *error = std::string("ui option '") + key + "' has an invalid color";
            lua_pop(state, 1);
            return false;
        }
        char* end = nullptr;
        *value = static_cast<int>(std::strtoul(color[0] == '#' ? color + 1 : color, &end, 16));
        if (end == nullptr || *end != '\0') {
            *error = std::string("ui option '") + key + "' must be #RRGGBB or an integer";
            lua_pop(state, 1);
            return false;
        }
    } else {
        *error = std::string("ui option '") + key + "' must be #RRGGBB or an integer";
        lua_pop(state, 1);
        return false;
    }
    *present = true;
    lua_pop(state, 1);
    return true;
}

bool ParsePoints(lua_State* state, int table_index, UiOptions* options, std::string* error) {
    lua_getfield(state, table_index, "points");
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1)) {
        *error = "ui option 'points' must be an array of {x, y} pairs";
        lua_pop(state, 1);
        return false;
    }
    const size_t count = lua_rawlen(state, -1);
    if (count > 128) {
        *error = "ui option 'points' may contain at most 128 points";
        lua_pop(state, 1);
        return false;
    }
    options->points.reserve(count);
    for (size_t index = 1; index <= count; ++index) {
        lua_rawgeti(state, -1, static_cast<lua_Integer>(index));
        if (!lua_istable(state, -1)) {
            *error = "ui option 'points' must be an array of {x, y} pairs";
            lua_pop(state, 2);
            return false;
        }
        lua_rawgeti(state, -1, 1);
        lua_rawgeti(state, -2, 2);
        if (!lua_isnumber(state, -2) || !lua_isnumber(state, -1)) {
            *error = "each ui point must contain numeric x and y values";
            lua_pop(state, 4);
            return false;
        }
        options->points.push_back({static_cast<lv_value_precise_t>(lua_tointeger(state, -2)),
                                   static_cast<lv_value_precise_t>(lua_tointeger(state, -1))});
        lua_pop(state, 3);
    }
    lua_pop(state, 1);
    return true;
}

bool ParseOptions(lua_State* state, int index, UiOptions* options, std::string* error) {
    if (lua_isnoneornil(state, index))
        return true;
    if (!lua_istable(state, index)) {
        *error = "ui options must be a table";
        return false;
    }
    const int table_index = lua_absindex(state, index);
    if (!ReadStringField(state, table_index, "id", &options->id, error) ||
        !ReadStringField(state, table_index, "text", &options->text, error) ||
        !ReadStringField(state, table_index, "options", &options->options, error) ||
        !ReadStringField(state, table_index, "src", &options->src, error) ||
        !ReadStringField(state, table_index, "align", &options->align, error) ||
        !ReadStringField(state, table_index, "flex", &options->flex, error) ||
        !ReadIntegerField(state, table_index, "x", &options->x, &options->has_x, error) ||
        !ReadIntegerField(state, table_index, "y", &options->y, &options->has_y, error) ||
        !ReadIntegerField(state, table_index, "width", &options->width, &options->has_width,
                          error) ||
        !ReadIntegerField(state, table_index, "height", &options->height, &options->has_height,
                          error) ||
        !ReadIntegerField(state, table_index, "min", &options->min, &options->has_min, error) ||
        !ReadIntegerField(state, table_index, "max", &options->max, &options->has_max, error) ||
        !ReadIntegerField(state, table_index, "value", &options->value, &options->has_value,
                          error) ||
        !ReadIntegerField(state, table_index, "bg_opa", &options->bg_opa,
                          &options->has_bg_opa, error) ||
        !ReadIntegerField(state, table_index, "radius", &options->radius,
                          &options->has_radius, error) ||
        !ReadIntegerField(state, table_index, "border_width", &options->border_width,
                          &options->has_border_width, error) ||
        !ReadIntegerField(state, table_index, "pad", &options->pad, &options->has_pad, error) ||
        !ReadIntegerField(state, table_index, "pad_row", &options->pad_row,
                          &options->has_pad_row, error) ||
        !ReadIntegerField(state, table_index, "pad_column", &options->pad_column,
                          &options->has_pad_column, error) ||
        !ReadBooleanField(state, table_index, "hidden", &options->hidden,
                          &options->has_hidden, error) ||
        !ReadBooleanField(state, table_index, "clickable", &options->clickable,
                          &options->has_clickable, error) ||
        !ReadBooleanField(state, table_index, "scrollable", &options->scrollable,
                          &options->has_scrollable, error) ||
        !ReadBooleanField(state, table_index, "checked", &options->checked,
                          &options->has_checked, error) ||
        !ReadBooleanField(state, table_index, "events", &options->events, &options->has_events,
                          error) ||
        !ReadBooleanField(state, table_index, "one_line", &options->one_line,
                          &options->has_one_line, error) ||
        !ParseColor(state, table_index, "bg_color", &options->bg_color, &options->has_bg_color,
                    error) ||
        !ParseColor(state, table_index, "text_color", &options->text_color,
                    &options->has_text_color, error) ||
        !ParseColor(state, table_index, "border_color", &options->border_color,
                    &options->has_border_color, error) ||
        !ParsePoints(state, table_index, options, error)) {
        return false;
    }
    if (options->has_min && options->has_max && options->min >= options->max) {
        *error = "ui option 'min' must be lower than 'max'";
        return false;
    }
    if (options->has_bg_opa && (options->bg_opa < LV_OPA_TRANSP || options->bg_opa > LV_OPA_COVER)) {
        *error = "ui option 'bg_opa' must be between 0 and 255";
        return false;
    }
    return true;
}

WidgetKind ParseKind(const std::string& type, bool* valid) {
    *valid = true;
    if (type == "screen") return WidgetKind::Screen;
    if (type == "container" || type == "object") return WidgetKind::Container;
    if (type == "label") return WidgetKind::Label;
    if (type == "button") return WidgetKind::Button;
    if (type == "bar") return WidgetKind::Bar;
    if (type == "slider") return WidgetKind::Slider;
    if (type == "arc") return WidgetKind::Arc;
    if (type == "switch") return WidgetKind::Switch;
    if (type == "checkbox") return WidgetKind::Checkbox;
    if (type == "dropdown") return WidgetKind::Dropdown;
    if (type == "roller") return WidgetKind::Roller;
    if (type == "textarea") return WidgetKind::Textarea;
    if (type == "image") return WidgetKind::Image;
    if (type == "line") return WidgetKind::Line;
    if (type == "table") return WidgetKind::Table;
    if (type == "spinner") return WidgetKind::Spinner;
    if (type == "led") return WidgetKind::Led;
    if (type == "chart") return WidgetKind::Chart;
    *valid = false;
    return WidgetKind::Container;
}

const char* EventName(lv_event_code_t code) {
    switch (code) {
        case LV_EVENT_CLICKED:
            return "clicked";
        case LV_EVENT_PRESSED:
            return "pressed";
        case LV_EVENT_RELEASED:
            return "released";
        case LV_EVENT_LONG_PRESSED:
            return "long_pressed";
        case LV_EVENT_VALUE_CHANGED:
            return "value_changed";
        case LV_EVENT_FOCUSED:
            return "focused";
        case LV_EVENT_DEFOCUSED:
            return "defocused";
        default:
            return nullptr;
    }
}

int WidgetValue(const Widget& widget) {
    switch (widget.kind) {
        case WidgetKind::Bar:
#if LV_USE_BAR
            return lv_bar_get_value(widget.object);
#else
            return 0;
#endif
        case WidgetKind::Slider:
#if LV_USE_SLIDER
            return lv_slider_get_value(widget.object);
#else
            return 0;
#endif
        case WidgetKind::Arc:
#if LV_USE_ARC
            return lv_arc_get_value(widget.object);
#else
            return 0;
#endif
        case WidgetKind::Dropdown:
#if LV_USE_DROPDOWN
            return lv_dropdown_get_selected(widget.object);
#else
            return 0;
#endif
        case WidgetKind::Roller:
#if LV_USE_ROLLER
            return lv_roller_get_selected(widget.object);
#else
            return 0;
#endif
        default:
            return 0;
    }
}

void ApplyValue(Widget& widget, const UiOptions& options) {
    if (options.has_min || options.has_max) {
        const int min = options.has_min ? options.min : 0;
        const int max = options.has_max ? options.max : 100;
        switch (widget.kind) {
            case WidgetKind::Bar:
#if LV_USE_BAR
                lv_bar_set_range(widget.object, min, max);
#endif
                break;
            case WidgetKind::Slider:
#if LV_USE_SLIDER
                lv_slider_set_range(widget.object, min, max);
#endif
                break;
            case WidgetKind::Arc:
#if LV_USE_ARC
                lv_arc_set_range(widget.object, min, max);
#endif
                break;
            default:
                break;
        }
    }
    if (options.has_value) {
        switch (widget.kind) {
            case WidgetKind::Bar:
#if LV_USE_BAR
                lv_bar_set_value(widget.object, options.value, LV_ANIM_OFF);
#endif
                break;
            case WidgetKind::Slider:
#if LV_USE_SLIDER
                lv_slider_set_value(widget.object, options.value, LV_ANIM_OFF);
#endif
                break;
            case WidgetKind::Arc:
#if LV_USE_ARC
                lv_arc_set_value(widget.object, options.value);
#endif
                break;
            case WidgetKind::Dropdown:
#if LV_USE_DROPDOWN
                lv_dropdown_set_selected(widget.object, options.value);
#endif
                break;
            case WidgetKind::Roller:
#if LV_USE_ROLLER
                lv_roller_set_selected(widget.object, options.value, LV_ANIM_OFF);
#endif
                break;
            default:
                break;
        }
    }
    if (options.has_checked) {
        if (options.checked)
            lv_obj_add_state(widget.object, LV_STATE_CHECKED);
        else
            lv_obj_remove_state(widget.object, LV_STATE_CHECKED);
    }
}

void ApplyOptions(Widget& widget, const UiOptions& options) {
    lv_obj_t* object = widget.object;
    if (options.has_width) lv_obj_set_width(object, options.width);
    if (options.has_height) lv_obj_set_height(object, options.height);
    if (options.has_x || options.has_y) lv_obj_set_pos(object, options.has_x ? options.x : 0,
                                                       options.has_y ? options.y : 0);
    if (options.has_bg_color) lv_obj_set_style_bg_color(object, lv_color_hex(options.bg_color), 0);
    if (options.has_text_color)
        lv_obj_set_style_text_color(object, lv_color_hex(options.text_color), 0);
    if (options.has_border_color)
        lv_obj_set_style_border_color(object, lv_color_hex(options.border_color), 0);
    if (options.has_bg_opa) lv_obj_set_style_bg_opa(object, static_cast<lv_opa_t>(options.bg_opa), 0);
    if (options.has_radius) lv_obj_set_style_radius(object, options.radius, 0);
    if (options.has_border_width) lv_obj_set_style_border_width(object, options.border_width, 0);
    if (options.has_pad) lv_obj_set_style_pad_all(object, options.pad, 0);
    if (options.has_pad_row) lv_obj_set_style_pad_row(object, options.pad_row, 0);
    if (options.has_pad_column) lv_obj_set_style_pad_column(object, options.pad_column, 0);
    if (options.has_hidden) {
        if (options.hidden)
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
    if (options.has_clickable) {
        if (options.clickable)
            lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
        else
            lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    }
    if (options.has_scrollable) {
        if (options.scrollable)
            lv_obj_add_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        else
            lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    }
    if (!options.align.empty()) {
        static const std::pair<const char*, lv_align_t> aligns[] = {
            {"top_left", LV_ALIGN_TOP_LEFT},       {"top_mid", LV_ALIGN_TOP_MID},
            {"top_right", LV_ALIGN_TOP_RIGHT},     {"bottom_left", LV_ALIGN_BOTTOM_LEFT},
            {"bottom_mid", LV_ALIGN_BOTTOM_MID},   {"bottom_right", LV_ALIGN_BOTTOM_RIGHT},
            {"left_mid", LV_ALIGN_LEFT_MID},       {"right_mid", LV_ALIGN_RIGHT_MID},
            {"center", LV_ALIGN_CENTER},
        };
        for (const auto& [name, align] : aligns) {
            if (options.align == name) {
                lv_obj_align(object, align, 0, 0);
                break;
            }
        }
    }
#if LV_USE_FLEX
    if (options.flex == "row")
        lv_obj_set_flex_flow(object, LV_FLEX_FLOW_ROW);
    else if (options.flex == "column")
        lv_obj_set_flex_flow(object, LV_FLEX_FLOW_COLUMN);
#endif
    ApplyValue(widget, options);
}

class LuaUiManager {
public:
    static LuaUiManager& GetInstance() {
        static LuaUiManager instance;
        return instance;
    }

    bool IsAvailable() const {
        Display* display = Board::GetInstance().GetDisplay();
        return display != nullptr && display->width() > 0 && display->height() > 0;
    }

    bool Create(const std::string& type, const std::string& parent_id, const UiOptions& options,
                std::string* id, std::string* error) {
        if (!IsAvailable()) {
            *error = "this device has no LVGL display";
            return false;
        }
        bool valid_kind = false;
        const WidgetKind kind = ParseKind(type, &valid_kind);
        if (!valid_kind) {
            *error = "unsupported ui widget type: " + type;
            return false;
        }
        if (kind == WidgetKind::Screen && !parent_id.empty()) {
            *error = "a ui screen cannot have a parent";
            return false;
        }
        if (kind != WidgetKind::Screen && parent_id.empty()) {
            *error = "a ui widget requires a parent screen or container id";
            return false;
        }

        std::shared_ptr<Widget> parent;
        if (!parent_id.empty()) {
            auto it = widgets_.find(parent_id);
            if (it == widgets_.end() || it->second->object == nullptr) {
                *error = "ui parent does not exist: " + parent_id;
                return false;
            }
            parent = it->second;
        }
        *id = options.id.empty() ? NextId(type) : options.id;
        if (widgets_.find(*id) != widgets_.end()) {
            *error = "ui id already exists: " + *id;
            return false;
        }

        auto widget = std::make_shared<Widget>();
        widget->id = *id;
        widget->parent_id = parent_id;
        widget->kind = kind;
        widget->source = options.src;
        widget->points = options.points;
        if (!OnDisplay([this, widget, parent, options](std::string* action_error) {
                Widget* raw = widget.get();
                if (raw->kind == WidgetKind::Screen && screen_ != nullptr) {
                    if (native_screen_ != nullptr && lv_obj_is_valid(native_screen_))
                        lv_screen_load(native_screen_);
                    if (lv_obj_is_valid(screen_->object)) lv_obj_delete(screen_->object);
                    widgets_.clear();
                    screen_ = nullptr;
                    ClearEvents();
                }
                lv_obj_t* object = nullptr;
                lv_obj_t* parent_object = parent == nullptr ? nullptr : parent->object;
                switch (raw->kind) {
                    case WidgetKind::Screen:
                        native_screen_ = native_screen_ == nullptr ? lv_screen_active() : native_screen_;
                        object = lv_obj_create(nullptr);
                        break;
                    case WidgetKind::Container:
                        object = lv_obj_create(parent_object);
                        break;
                    case WidgetKind::Label:
#if LV_USE_LABEL
                        object = lv_label_create(parent_object);
#endif
                        break;
                    case WidgetKind::Button:
#if LV_USE_BUTTON
                        object = lv_button_create(parent_object);
#endif
                        break;
                    case WidgetKind::Bar:
#if LV_USE_BAR
                        object = lv_bar_create(parent_object);
#endif
                        break;
                    case WidgetKind::Slider:
#if LV_USE_SLIDER
                        object = lv_slider_create(parent_object);
#endif
                        break;
                    case WidgetKind::Arc:
#if LV_USE_ARC
                        object = lv_arc_create(parent_object);
#endif
                        break;
                    case WidgetKind::Switch:
#if LV_USE_SWITCH
                        object = lv_switch_create(parent_object);
#endif
                        break;
                    case WidgetKind::Checkbox:
#if LV_USE_CHECKBOX
                        object = lv_checkbox_create(parent_object);
#endif
                        break;
                    case WidgetKind::Dropdown:
#if LV_USE_DROPDOWN
                        object = lv_dropdown_create(parent_object);
#endif
                        break;
                    case WidgetKind::Roller:
#if LV_USE_ROLLER
                        object = lv_roller_create(parent_object);
#endif
                        break;
                    case WidgetKind::Textarea:
#if LV_USE_TEXTAREA
                        object = lv_textarea_create(parent_object);
#endif
                        break;
                    case WidgetKind::Image:
#if LV_USE_IMAGE
                        object = lv_image_create(parent_object);
#endif
                        break;
                    case WidgetKind::Line:
#if LV_USE_LINE
                        object = lv_line_create(parent_object);
#endif
                        break;
                    case WidgetKind::Table:
#if LV_USE_TABLE
                        object = lv_table_create(parent_object);
#endif
                        break;
                    case WidgetKind::Spinner:
#if LV_USE_SPINNER
                        object = lv_spinner_create(parent_object);
#endif
                        break;
                    case WidgetKind::Led:
#if LV_USE_LED
                        object = lv_led_create(parent_object);
#endif
                        break;
                    case WidgetKind::Chart:
#if LV_USE_CHART
                        object = lv_chart_create(parent_object);
#endif
                        break;
                }
                if (object == nullptr) {
                    *action_error = "this firmware build does not include the requested ui widget";
                    return false;
                }
                raw->object = object;
#if LV_USE_LABEL
                if (raw->kind == WidgetKind::Label && !options.text.empty())
                    lv_label_set_text(object, options.text.c_str());
#endif
                if (raw->kind == WidgetKind::Button && !options.text.empty()) {
#if LV_USE_LABEL
                    lv_obj_t* label = lv_label_create(object);
                    lv_label_set_text(label, options.text.c_str());
                    lv_obj_center(label);
#endif
                }
#if LV_USE_CHECKBOX
                if (raw->kind == WidgetKind::Checkbox && !options.text.empty())
                    lv_checkbox_set_text(object, options.text.c_str());
#endif
#if LV_USE_DROPDOWN
                if (raw->kind == WidgetKind::Dropdown && !options.options.empty())
                    lv_dropdown_set_options(object, options.options.c_str());
#endif
#if LV_USE_ROLLER
                if (raw->kind == WidgetKind::Roller && !options.options.empty())
                    lv_roller_set_options(object, options.options.c_str(), LV_ROLLER_MODE_NORMAL);
#endif
                if (raw->kind == WidgetKind::Textarea) {
#if LV_USE_TEXTAREA
                    if (!options.text.empty()) lv_textarea_set_text(object, options.text.c_str());
                    if (options.one_line) lv_textarea_set_one_line(object, true);
#endif
                }
#if LV_USE_IMAGE
                if (raw->kind == WidgetKind::Image && !raw->source.empty())
                    lv_image_set_src(object, raw->source.c_str());
#endif
#if LV_USE_LINE
                if (raw->kind == WidgetKind::Line && !raw->points.empty())
                    lv_line_set_points(object, raw->points.data(), raw->points.size());
#endif
#if LV_USE_TABLE
                if (raw->kind == WidgetKind::Table && !options.text.empty())
                    lv_table_set_cell_value(object, 0, 0, options.text.c_str());
#endif
                ApplyOptions(*raw, options);
                if (options.events) {
                    if (!EnsureEventQueue()) {
                        lv_obj_delete(object);
                        raw->object = nullptr;
                        *action_error = "unable to create ui event queue";
                        return false;
                    }
                    lv_obj_add_event_cb(object, OnLvglEvent, LV_EVENT_ALL, raw);
                }
                if (raw->kind == WidgetKind::Screen) screen_ = raw;
                return true;
            }, error)) {
            return false;
        }
        widgets_.emplace(*id, std::move(widget));
        return true;
    }

    bool Set(const std::string& id, const UiOptions& options, std::string* error) {
        auto it = widgets_.find(id);
        if (it == widgets_.end()) {
            *error = "ui id does not exist: " + id;
            return false;
        }
        std::shared_ptr<Widget> widget = it->second;
        if (!options.src.empty()) widget->source = options.src;
        if (!options.points.empty()) widget->points = options.points;
        return OnDisplay([widget, options](std::string* action_error) {
            if (!lv_obj_is_valid(widget->object)) {
                *action_error = "ui object is no longer valid";
                return false;
            }
#if LV_USE_LABEL
            if (widget->kind == WidgetKind::Label && !options.text.empty())
                lv_label_set_text(widget->object, options.text.c_str());
#endif
#if LV_USE_CHECKBOX
            if (widget->kind == WidgetKind::Checkbox && !options.text.empty())
                lv_checkbox_set_text(widget->object, options.text.c_str());
#endif
#if LV_USE_DROPDOWN
            if (widget->kind == WidgetKind::Dropdown && !options.options.empty())
                lv_dropdown_set_options(widget->object, options.options.c_str());
#endif
#if LV_USE_ROLLER
            if (widget->kind == WidgetKind::Roller && !options.options.empty())
                lv_roller_set_options(widget->object, options.options.c_str(), LV_ROLLER_MODE_NORMAL);
#endif
#if LV_USE_TEXTAREA
            if (widget->kind == WidgetKind::Textarea) {
                if (!options.text.empty()) lv_textarea_set_text(widget->object, options.text.c_str());
                if (options.one_line) lv_textarea_set_one_line(widget->object, true);
            }
#endif
#if LV_USE_IMAGE
            if (widget->kind == WidgetKind::Image && !widget->source.empty())
                lv_image_set_src(widget->object, widget->source.c_str());
#endif
#if LV_USE_LINE
            if (widget->kind == WidgetKind::Line && !widget->points.empty())
                lv_line_set_points(widget->object, widget->points.data(), widget->points.size());
#endif
            ApplyOptions(*widget, options);
            return true;
        }, error);
    }

    bool Load(const std::string& id, std::string* error) {
        auto it = widgets_.find(id);
        if (it == widgets_.end() || it->second->kind != WidgetKind::Screen) {
            *error = "ui load requires a screen id";
            return false;
        }
        std::shared_ptr<Widget> screen = it->second;
        return OnDisplay([screen](std::string*) {
            lv_screen_load(screen->object);
            return true;
        }, error);
    }

    bool Restore(std::string* error) {
        if (screen_ == nullptr) return true;
        auto it = widgets_.find(screen_->id);
        if (it == widgets_.end()) {
            // Screen pointer without a registered widget (invariant was broken);
            // just clear the stale state instead of throwing out of the Lua task.
            screen_ = nullptr;
            ClearEvents();
            return true;
        }
        std::shared_ptr<Widget> screen = it->second;
        if (!OnDisplay([this, screen](std::string*) {
                if (native_screen_ != nullptr && lv_obj_is_valid(native_screen_))
                    lv_screen_load(native_screen_);
                if (screen->object != nullptr && lv_obj_is_valid(screen->object))
                    lv_obj_delete(screen->object);
                return true;
            }, error)) {
            return false;
        }
        widgets_.clear();
        screen_ = nullptr;
        ClearEvents();
        return true;
    }

    bool Delete(const std::string& id, std::string* error) {
        auto it = widgets_.find(id);
        if (it == widgets_.end()) {
            *error = "ui id does not exist: " + id;
            return false;
        }
        if (it->second.get() == screen_) return Restore(error);
        std::shared_ptr<Widget> widget = it->second;
        if (!OnDisplay([widget](std::string*) {
                if (widget->object != nullptr && lv_obj_is_valid(widget->object))
                    lv_obj_delete(widget->object);
                return true;
            }, error)) {
            return false;
        }
        std::vector<std::string> removed = {id};
        for (size_t index = 0; index < removed.size(); ++index) {
            for (const auto& [candidate_id, candidate] : widgets_) {
                if (candidate->parent_id == removed[index]) removed.push_back(candidate_id);
            }
        }
        for (const auto& removed_id : removed) widgets_.erase(removed_id);
        return true;
    }

    bool PollEvent(int timeout_ms, UiEvent* event) {
        if (!EnsureEventQueue()) return false;
        timeout_ms = std::clamp(timeout_ms, 0, kMaxEventWaitMs);
        return xQueueReceive(event_queue_, event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    }

private:
    std::string NextId(const std::string& type) {
        return type + "-" + std::to_string(next_id_.fetch_add(1));
    }

    bool EnsureEventQueue() {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (event_queue_ == nullptr) event_queue_ = xQueueCreate(kEventQueueLength, sizeof(UiEvent));
        return event_queue_ != nullptr;
    }

    void ClearEvents() {
        std::lock_guard<std::mutex> lock(event_mutex_);
        if (event_queue_ != nullptr) xQueueReset(event_queue_);
    }

    static void OnLvglEvent(lv_event_t* event) {
        const char* name = EventName(lv_event_get_code(event));
        Widget* widget = static_cast<Widget*>(lv_event_get_user_data(event));
        if (name == nullptr || widget == nullptr) return;
        LuaUiManager& manager = GetInstance();
        if (!manager.EnsureEventQueue()) return;
        UiEvent queued{};
        std::snprintf(queued.id, sizeof(queued.id), "%s", widget->id.c_str());
        std::snprintf(queued.type, sizeof(queued.type), "%s", name);
        queued.value = WidgetValue(*widget);
        queued.checked = lv_obj_has_state(widget->object, LV_STATE_CHECKED);
#if LV_USE_TEXTAREA
        if (widget->kind == WidgetKind::Textarea)
            std::snprintf(queued.text, sizeof(queued.text), "%s", lv_textarea_get_text(widget->object));
#endif
        xQueueSend(manager.event_queue_, &queued, 0);
    }

    bool OnDisplay(std::function<bool(std::string*)>&& action, std::string* error) {
        if (!IsAvailable()) {
            *error = "this device has no LVGL display";
            return false;
        }
        auto result = std::make_shared<ScheduledResult>();
        if (result->done == nullptr) {
            *error = "unable to create display completion semaphore";
            return false;
        }
        Display* display = Board::GetInstance().GetDisplay();
        Application::GetInstance().Schedule([result, display, action = std::move(action)]() mutable {
            int expected = 0;
            if (!result->state.compare_exchange_strong(expected, 1)) {
                xSemaphoreGive(result->done);
                return;
            }
            DisplayLockGuard lock(display);
            result->success = action(&result->error);
            result->state.store(3);
            xSemaphoreGive(result->done);
        });
        if (xSemaphoreTake(result->done, pdMS_TO_TICKS(kDisplayTaskTimeoutMs)) != pdTRUE) {
            int expected = 0;
            if (!result->state.compare_exchange_strong(expected, 2))
                xSemaphoreTake(result->done, portMAX_DELAY);
            *error = "display task timed out";
            return false;
        }
        if (!result->success) {
            *error = result->error.empty() ? "display operation failed" : result->error;
            return false;
        }
        return true;
    }

    std::unordered_map<std::string, std::shared_ptr<Widget>> widgets_;
    Widget* screen_ = nullptr;
    lv_obj_t* native_screen_ = nullptr;
    QueueHandle_t event_queue_ = nullptr;
    std::mutex event_mutex_;
    std::atomic_uint32_t next_id_{1};
};

ObjectUserdata* CheckObject(lua_State* state, int index) {
    return static_cast<ObjectUserdata*>(luaL_checkudata(state, index, kObjectMetatable));
}

int PushResult(lua_State* state, bool success, const std::string& error) {
    if (success) {
        lua_pushboolean(state, true);
        return 1;
    }
    lua_pushnil(state);
    lua_pushlstring(state, error.data(), error.size());
    return 2;
}

int CreateWidget(lua_State* state, const std::string& type, int parent_index, int options_index) {
    std::string parent_id;
    if (parent_index != 0 && !lua_isnoneornil(state, parent_index)) {
        parent_id = CheckObject(state, parent_index)->id;
    }
    UiOptions options;
    std::string error;
    if (!ParseOptions(state, options_index, &options, &error))
        return PushResult(state, false, error);
    std::string id;
    if (!LuaUiManager::GetInstance().Create(type, parent_id, options, &id, &error)) {
        lua_pushnil(state);
        lua_pushlstring(state, error.data(), error.size());
        return 2;
    }
    auto* object = static_cast<ObjectUserdata*>(lua_newuserdatauv(state, sizeof(ObjectUserdata), 0));
    new (object) ObjectUserdata{id};
    luaL_getmetatable(state, kObjectMetatable);
    lua_setmetatable(state, -2);
    return 1;
}

int LuaUiCreate(lua_State* state) {
    const char* type = luaL_checkstring(state, 1);
    return CreateWidget(state, type, 2, 3);
}

int LuaUiScreen(lua_State* state) { return CreateWidget(state, "screen", 0, 1); }

int LuaUiNamedCreate(lua_State* state) {
    const char* type = lua_tostring(state, lua_upvalueindex(1));
    return CreateWidget(state, type, 1, 2);
}

int LuaUiObjectSet(lua_State* state) {
    ObjectUserdata* object = CheckObject(state, 1);
    UiOptions options;
    std::string error;
    if (!ParseOptions(state, 2, &options, &error)) return PushResult(state, false, error);
    return PushResult(state, LuaUiManager::GetInstance().Set(object->id, options, &error), error);
}

int LuaUiObjectLoad(lua_State* state) {
    ObjectUserdata* object = CheckObject(state, 1);
    std::string error;
    return PushResult(state, LuaUiManager::GetInstance().Load(object->id, &error), error);
}

int LuaUiObjectDelete(lua_State* state) {
    ObjectUserdata* object = CheckObject(state, 1);
    std::string error;
    return PushResult(state, LuaUiManager::GetInstance().Delete(object->id, &error), error);
}

int LuaUiRestore(lua_State* state) {
    std::string error;
    return PushResult(state, LuaUiManager::GetInstance().Restore(&error), error);
}

int LuaUiInfo(lua_State* state) {
    Display* display = Board::GetInstance().GetDisplay();
    lua_createtable(state, 0, 3);
    lua_pushboolean(state, LuaUiManager::GetInstance().IsAvailable());
    lua_setfield(state, -2, "available");
    lua_pushinteger(state, display == nullptr ? 0 : display->width());
    lua_setfield(state, -2, "width");
    lua_pushinteger(state, display == nullptr ? 0 : display->height());
    lua_setfield(state, -2, "height");
    return 1;
}

int LuaUiPollEvent(lua_State* state) {
    const int timeout_ms = static_cast<int>(luaL_optinteger(state, 1, 0));
    UiEvent event{};
    if (!LuaUiManager::GetInstance().PollEvent(timeout_ms, &event)) {
        lua_pushnil(state);
        return 1;
    }
    lua_createtable(state, 0, 5);
    lua_pushstring(state, event.id);
    lua_setfield(state, -2, "id");
    lua_pushstring(state, event.type);
    lua_setfield(state, -2, "type");
    lua_pushinteger(state, event.value);
    lua_setfield(state, -2, "value");
    lua_pushboolean(state, event.checked);
    lua_setfield(state, -2, "checked");
    lua_pushstring(state, event.text);
    lua_setfield(state, -2, "text");
    return 1;
}

int LuaUiObjectToString(lua_State* state) {
    ObjectUserdata* object = CheckObject(state, 1);
    lua_pushfstring(state, "xiaozhi.ui.object(%s)", object->id.c_str());
    return 1;
}

int LuaUiObjectGc(lua_State* state) {
    ObjectUserdata* object = CheckObject(state, 1);
    object->~ObjectUserdata();
    return 0;
}

const luaL_Reg kObjectMethods[] = {
    {"set", LuaUiObjectSet},
    {"load", LuaUiObjectLoad},
    {"delete", LuaUiObjectDelete},
    {nullptr, nullptr},
};

#endif  // HAVE_LVGL

}  // namespace

bool IsLuaUiAvailable() {
#ifdef HAVE_LVGL
    return LuaUiManager::GetInstance().IsAvailable();
#else
    return false;
#endif
}

void RegisterLuaUiBindings(lua_State* state) {
    lua_newtable(state);
#ifdef HAVE_LVGL
    luaL_newmetatable(state, kObjectMetatable);
    lua_newtable(state);
    luaL_setfuncs(state, kObjectMethods, 0);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, LuaUiObjectToString);
    lua_setfield(state, -2, "__tostring");
    lua_pushcfunction(state, LuaUiObjectGc);
    lua_setfield(state, -2, "__gc");
    lua_pop(state, 1);

    lua_pushcfunction(state, LuaUiInfo);
    lua_setfield(state, -2, "info");
    lua_pushcfunction(state, LuaUiScreen);
    lua_setfield(state, -2, "screen");
    lua_pushcfunction(state, LuaUiCreate);
    lua_setfield(state, -2, "create");
    lua_pushcfunction(state, LuaUiRestore);
    lua_setfield(state, -2, "restore");
    lua_pushcfunction(state, LuaUiPollEvent);
    lua_setfield(state, -2, "poll_event");
    static const char* kNamedWidgets[] = {"container", "label", "button", "bar", "slider",
                                          "arc",       "switch", "checkbox", "dropdown", "roller",
                                          "textarea",  "image",  "line",     "table",    "spinner",
                                          "led",       "chart"};
    for (const char* widget : kNamedWidgets) {
        lua_pushstring(state, widget);
        lua_pushcclosure(state, LuaUiNamedCreate, 1);
        lua_setfield(state, -2, widget);
    }
#else
    lua_pushboolean(state, false);
    lua_setfield(state, -2, "available");
#endif
    lua_setfield(state, -2, "ui");
}
