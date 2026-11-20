/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "scene.hpp"
#include "phone_keyboard.hpp"
#include "ui_components.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>

#include <lvgl.h>

#include "design.hpp"
#include "aeraui/engine.hpp"
#include "aeraui/status_bar.hpp"

namespace aeraui {
namespace {
using namespace design;

constexpr int kMaxPatternDots = 36;

struct DecryptState {
  ActionCallback callback = nullptr;
  void *context = nullptr;
  int credential_type = 0;
  bool screen_lock = false;
  int grid_size = 3;
  lv_obj_t *input = nullptr;
  lv_obj_t *status = nullptr;
  lv_obj_t *submit = nullptr;
  lv_obj_t *submit_label = nullptr;
  lv_obj_t *screen = nullptr;
  lv_obj_t *title = nullptr;
  lv_obj_t *submit_icon = nullptr;
  lv_obj_t *language_button = nullptr;
  lv_obj_t *language_picker = nullptr;
  int user_id = 0;
  std::string user_name;
  lv_obj_t *keyboard = nullptr;
  lv_obj_t *pattern_area = nullptr;
  lv_obj_t *pattern_line = nullptr;
  std::array<lv_obj_t *, kMaxPatternDots> nodes{};
  std::array<lv_obj_t *, 4> grid_buttons{};
  std::array<bool, kMaxPatternDots> selected{};
  std::array<int, kMaxPatternDots> sequence{};
  std::array<lv_point_precise_t, kMaxPatternDots + 1> points{};
  lv_point_precise_t cursor{};
  int sequence_length = 0;
  bool tracking = false;
  bool busy = false;
  bool submit_pending = false;
  bool secondary = false;
};

void DeleteState(lv_event_t *event) {
  auto *state = static_cast<DecryptState *>(lv_event_get_user_data(event));
  // The picker has delete callbacks that still refer to this scene state.
  if (state->language_picker) lv_obj_delete(state->language_picker);
  delete state;
}

void SubmitDecrypt(DecryptState *state) {
  if (state == nullptr || state->callback == nullptr || state->busy ||
      state->submit_pending)
    return;
  state->submit_pending = true;
  state->callback(Action::kDecryptSubmit, state->context);
}

void Dispatch(lv_event_t *event) {
  auto *state = static_cast<DecryptState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->callback == nullptr || state->busy) return;
  const auto action = static_cast<Action>(reinterpret_cast<uintptr_t>(
      lv_obj_get_user_data(lv_event_get_target_obj(event))));
  if (action == Action::kDecryptSubmit) {
    SubmitDecrypt(state);
    return;
  }
  state->callback(action, state->context);
}

void Bind(lv_obj_t *object, Action action, DecryptState *state) {
  lv_obj_set_user_data(
      object, reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
  lv_obj_add_event_cb(object, Dispatch, LV_EVENT_CLICKED, state);
}

void RefreshPatternLine(DecryptState *state) {
  uint32_t count = static_cast<uint32_t>(state->sequence_length);
  if (state->tracking && count > 0) {
    state->points[count] = state->cursor;
    ++count;
  }
  lv_line_set_points(state->pattern_line, state->points.data(), count);
}

void RefreshPattern(DecryptState *state) {
  if (state == nullptr) return;
  for (int i = 0; i < state->grid_size * state->grid_size; ++i) {
    const bool active = state->selected[i];
    lv_obj_set_style_bg_color(state->nodes[i],
                              active ? kAccent : kMainPanel, 0);
    lv_obj_set_style_border_color(state->nodes[i],
                                  active ? kAccent : kLineBright, 0);
    lv_obj_set_style_transform_scale(state->nodes[i], active ? 116 : 100, 0);
  }
  RefreshPatternLine(state);
  lv_obj_invalidate(state->pattern_area);

  if (state->sequence_length == 0) {
    i18n::BindLabel(state->status, "Draw your unlock pattern");
  } else {
    const std::string copy =
        i18n::Format("%d dots connected", state->sequence_length);
    i18n::BindLabel(state->status, copy.c_str());
  }
}

void ResetPattern(DecryptState *state) {
  state->selected.fill(false);
  state->sequence_length = 0;
  RefreshPattern(state);
}

void AddPatternDot(DecryptState *state, int index) {
  if (index < 0 || index >= state->grid_size * state->grid_size ||
      state->selected[index] || state->sequence_length >= kMaxPatternDots)
    return;

  if (state->sequence_length > 0) {
    const int previous = state->sequence[state->sequence_length - 1];
    int px = previous % state->grid_size;
    int py = previous / state->grid_size;
    const int nx = index % state->grid_size;
    const int ny = index / state->grid_size;
    int dx = nx > px ? 1 : -1;
    int dy = ny > py ? 1 : -1;
    if (px == nx) dx = 0;
    else if (py == ny) dy = 0;
    else if (std::abs(px - nx) != std::abs(py - ny)) dx = dy = 2;
    if (dx != 2) {
      while ((dx == 0 || px != nx - dx) && (dy == 0 || py != ny - dy)) {
        px += dx;
        py += dy;
        const int intermediate = py * state->grid_size + px;
        if (!state->selected[intermediate] &&
            state->sequence_length < kMaxPatternDots) {
          state->selected[intermediate] = true;
          state->sequence[state->sequence_length] = intermediate;
          state->points[state->sequence_length] = {
              static_cast<lv_value_precise_t>(lv_obj_get_x(state->nodes[intermediate]) + 34),
              static_cast<lv_value_precise_t>(lv_obj_get_y(state->nodes[intermediate]) + 34)};
          ++state->sequence_length;
        }
      }
    }
  }

  state->selected[index] = true;
  state->sequence[state->sequence_length] = index;
  state->points[state->sequence_length] = {
      static_cast<lv_value_precise_t>(lv_obj_get_x(state->nodes[index]) + 34),
      static_cast<lv_value_precise_t>(lv_obj_get_y(state->nodes[index]) + 34)};
  ++state->sequence_length;
  RefreshPattern(state);
}

int PatternDotAt(DecryptState *state, const lv_point_t &point) {
  lv_area_t area{};
  lv_obj_get_coords(state->pattern_area, &area);
  const int local_x = point.x - area.x1;
  const int local_y = point.y - area.y1;
  int closest = -1;
  int closest_distance = 1000000;
  const int threshold = state->grid_size <= 3 ? 112 : 76;
  for (int i = 0; i < state->grid_size * state->grid_size; ++i) {
    const int dx = local_x - (lv_obj_get_x(state->nodes[i]) + 34);
    const int dy = local_y - (lv_obj_get_y(state->nodes[i]) + 34);
    const int distance = dx * dx + dy * dy;
    if (distance < threshold * threshold && distance < closest_distance) {
      closest = i;
      closest_distance = distance;
    }
  }
  return closest;
}

void PatternTouch(lv_event_t *event) {
  auto *state = static_cast<DecryptState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->busy) return;
  lv_indev_t *input = lv_indev_active();
  if (input == nullptr) return;
  lv_point_t point{};
  lv_indev_get_point(input, &point);
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_PRESSED) {
    ResetPattern(state);
    state->tracking = true;
  }
  if ((code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) &&
      state->tracking) {
    lv_area_t area{};
    lv_obj_get_coords(state->pattern_area, &area);
    state->cursor = {
        static_cast<lv_value_precise_t>(point.x - area.x1),
        static_cast<lv_value_precise_t>(point.y - area.y1)};
    AddPatternDot(state, PatternDotAt(state, point));
    RefreshPatternLine(state);
  }
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (!state->tracking) return;
    state->tracking = false;
    RefreshPatternLine(state);
    if (state->sequence_length >= 4) {
      SubmitDecrypt(state);
    } else if (state->sequence_length > 0) {
      i18n::BindLabel(state->status, "Pattern too short - try again");
    }
  }
}

lv_obj_t *MakeButton(lv_obj_t *parent, const char *text, lv_color_t fill,
                     lv_color_t foreground) {
  lv_obj_t *button = lv_button_create(parent);
  NoScroll(button);
  lv_obj_set_style_radius(button, 28, 0);
  lv_obj_set_style_bg_color(button, fill, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 0, 0);
  // Fixed geometry avoids transformed off-screen layers clipping rounded keys.
  lv_obj_set_style_transform_scale(button, LV_SCALE_NONE, LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(button, lv_color_mix(fill, kAccent, 192), LV_STATE_PRESSED);
  lv_obj_t *label = Label(button, text, &lv_font_montserrat_32, foreground);
  lv_obj_center(label);
  return button;
}

void RefreshGridButtons(DecryptState *state) {
  for (int index = 0; index < 4; ++index) {
    auto *button = state->grid_buttons[index];
    if (button == nullptr) continue;
    const bool active = state->grid_size == index + 3;
    lv_obj_set_style_bg_color(button, active ? kAccentSoft : kMainPanel, 0);
    lv_obj_set_style_border_width(button, active ? 2 : 0, 0);
    lv_obj_set_style_border_color(button, active ? kAccent : kLineBright, 0);
    auto *label = lv_obj_get_child(button, 0);
    if (label != nullptr)
      lv_obj_set_style_text_color(label, active ? kAccent : kMutedStrong, 0);
  }
}

void LayoutPatternNodes(DecryptState *state) {
  const int count = state->grid_size;
  const int visible = count * count;
  const int margin = count <= 3 ? 128 : 72;
  const int span = 1012 - margin * 2 - 68;
  for (int index = 0; index < kMaxPatternDots; ++index) {
    auto *node = state->nodes[index];
    if (node == nullptr) continue;
    if (index >= visible) {
      lv_obj_add_flag(node, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(node, LV_OBJ_FLAG_HIDDEN);
    const int row = index / count;
    const int column = index % count;
    lv_obj_set_pos(node, margin + column * span / (count - 1),
                   margin + row * span / (count - 1));
  }
}

void SetPatternGridSize(DecryptState *state, int size) {
  if (state == nullptr || state->busy || size < 3 || size > 6 ||
      state->grid_size == size)
    return;
  state->tracking = false;
  state->selected.fill(false);
  state->sequence_length = 0;
  state->grid_size = size;
  LayoutPatternNodes(state);
  RefreshGridButtons(state);
  RefreshPattern(state);
}

void GridSizeClicked(lv_event_t *event) {
  auto *state = static_cast<DecryptState *>(lv_event_get_user_data(event));
  const int size = static_cast<int>(reinterpret_cast<uintptr_t>(
      lv_obj_get_user_data(lv_event_get_target_obj(event))));
  SetPatternGridSize(state, size);
}

void MakePatternGridSelector(DecryptState *state, lv_obj_t *panel, int x,
                             int y) {
  auto *selector = lv_obj_create(panel);
  NoScroll(selector);
  lv_obj_set_pos(selector, x, y);
  lv_obj_set_size(selector, 1012, 88);
  lv_obj_set_style_radius(selector, 28, 0);
  lv_obj_set_style_bg_color(selector, kInset, 0);
  lv_obj_set_style_bg_opa(selector, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(selector, 1, 0);
  lv_obj_set_style_border_color(selector, kLine, 0);
  lv_obj_set_style_pad_all(selector, 4, 0);

  constexpr int gap = 8;
  constexpr int width = (1012 - 8 - gap * 3) / 4;
  for (int index = 0; index < 4; ++index) {
    const int size = index + 3;
    auto *button = lv_button_create(selector);
    state->grid_buttons[index] = button;
    NoScroll(button);
    lv_obj_set_pos(button, index * (width + gap), 0);
    lv_obj_set_size(button, width, 80);
    lv_obj_set_style_radius(button, 24, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, kAccentSoft, LV_STATE_PRESSED);
    char text[8];
    snprintf(text, sizeof(text), "%dx%d", size, size);
    auto *label = Label(button, text, &lv_font_montserrat_24, kMutedStrong);
    lv_obj_center(label);
    lv_obj_set_user_data(button,
                         reinterpret_cast<void *>(static_cast<uintptr_t>(size)));
    lv_obj_add_event_cb(button, GridSizeClicked, LV_EVENT_CLICKED, state);
  }
  RefreshGridButtons(state);
}

void MakePattern(DecryptState *state, lv_obj_t *panel, int x, int y) {
  state->pattern_area = lv_obj_create(panel);
  NoScroll(state->pattern_area);
  lv_obj_set_pos(state->pattern_area, x, y);
  lv_obj_set_size(state->pattern_area, 1012, 1012);
  lv_obj_set_style_radius(state->pattern_area, 34, 0);
  lv_obj_set_style_bg_color(state->pattern_area, kInset, 0);
  lv_obj_set_style_bg_opa(state->pattern_area, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(state->pattern_area, 1, 0);
  lv_obj_set_style_border_color(state->pattern_area, kLine, 0);
  lv_obj_set_style_pad_all(state->pattern_area, 0, 0);
  lv_obj_add_flag(state->pattern_area, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(state->pattern_area, PatternTouch, LV_EVENT_PRESSED, state);
  lv_obj_add_event_cb(state->pattern_area, PatternTouch, LV_EVENT_PRESSING, state);
  lv_obj_add_event_cb(state->pattern_area, PatternTouch, LV_EVENT_RELEASED, state);
  lv_obj_add_event_cb(state->pattern_area, PatternTouch, LV_EVENT_PRESS_LOST, state);

  state->pattern_line = lv_line_create(state->pattern_area);
  lv_obj_set_size(state->pattern_line, 1012, 1012);
  lv_obj_set_pos(state->pattern_line, 0, 0);
  lv_obj_set_style_line_color(state->pattern_line, kAccent, 0);
  lv_obj_set_style_line_width(state->pattern_line, 18, 0);
  lv_obj_set_style_line_rounded(state->pattern_line, true, 0);

  for (int index = 0; index < kMaxPatternDots; ++index) {
    lv_obj_t *node = lv_obj_create(state->pattern_area);
    state->nodes[index] = node;
    NoScroll(node);
    lv_obj_remove_flag(node, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(node, 68, 68);
    lv_obj_set_style_radius(node, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(node, kMainPanel, 0);
    lv_obj_set_style_bg_opa(node, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(node, 5, 0);
    lv_obj_set_style_border_color(node, kLineBright, 0);
  }
  LayoutPatternNodes(state);
  MakePatternGridSelector(state, panel, x, y - 106);
  RefreshPattern(state);
}

void PinKeyPressed(lv_event_t *event) {
  auto *state = static_cast<DecryptState *>(lv_event_get_user_data(event));
  if (state == nullptr || state->busy) return;
  const auto key = reinterpret_cast<uintptr_t>(
      lv_obj_get_user_data(lv_event_get_target_obj(event)));
  if (key == 10) {
    lv_textarea_set_text(state->input, "");
  } else if (key == 11) {
    lv_textarea_set_cursor_pos(state->input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_delete_char(state->input);
  } else {
    lv_textarea_set_cursor_pos(state->input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_add_char(state->input, static_cast<uint32_t>('0' + key));
  }
}

void MakePinPad(DecryptState *state, lv_obj_t *panel, int x, int y,
                int key_size, int gap_x, int gap_y, int side_pad) {
  const int width = side_pad * 2 + key_size * 3 + gap_x * 2;
  const int height = key_size * 4 + gap_y * 3;
  state->keyboard = lv_obj_create(panel);
  Clear(state->keyboard);
  lv_obj_set_pos(state->keyboard, x, y);
  lv_obj_set_size(state->keyboard, width, height);
  constexpr std::array<const char *, 12> labels{
      "1", "2", "3", "4", "5", "6", "7", "8", "9", "Clear", "0",
      LV_SYMBOL_BACKSPACE};
  constexpr std::array<uintptr_t, 12> keys{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11};
  static const lv_style_prop_t press_properties[] = {
      LV_STYLE_BG_COLOR, LV_STYLE_PROP_INV};
  static lv_style_transition_dsc_t press_transition;
  lv_style_transition_dsc_init(&press_transition, press_properties,
                               lv_anim_path_ease_out, 110, 0, nullptr);
  for (size_t i = 0; i < labels.size(); ++i) {
    const bool utility = i == 9 || i == 11;
    lv_obj_t *key = MakeButton(state->keyboard, labels[i],
                               kMainPanel, kText);
    if (utility) lv_obj_set_style_bg_opa(key, LV_OPA_TRANSP, 0);
    const int step_x = key_size + gap_x;
    const int step_y = key_size + gap_y;
    lv_obj_set_pos(key, side_pad + static_cast<int32_t>(i % 3) * step_x,
                   static_cast<int32_t>(i / 3) * step_y);
    lv_obj_set_size(key, key_size, key_size);
    lv_obj_set_style_radius(key, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(key, kAccentSoft, LV_STATE_PRESSED);
    lv_obj_set_style_transition(key, &press_transition, 0);
    lv_obj_set_style_border_width(key, 0, 0);
    auto *label = lv_obj_get_child(key, 0);
    if (utility) {
      lv_obj_set_style_text_font(label, UiFont(key_size >= 200
                                                   ? &lv_font_montserrat_32
                                                   : &lv_font_montserrat_24), 0);
    } else {
      // Render digits at their displayed size instead of scaling a label layer.
      const int font = key_size >= 200 && kInterfaceSize != 0
                           ? 66
                           : std::max(28, key_size * 25 / 100);
      lv_obj_set_style_text_font(label, fonts::DisplayFont(font), 0);
      lv_obj_center(label);
    }
    lv_obj_set_user_data(key, reinterpret_cast<void *>(keys[i]));
    lv_obj_add_event_cb(key, PinKeyPressed, LV_EVENT_CLICKED, state);
  }
}

struct DecryptKeyboardLayout {
  int input_x = 86;
  int input_y = 470;
  int input_width = 1012;
  lv_obj_t *pad_parent = nullptr;
  int pad_x = 86;
  int pad_y = 420;
  int key_size = 224;
  int gap_x = 120;
  int gap_y = 32;
  int side_pad = 50;
  int keyboard_x = 36;
  int keyboard_y = 720;
  int keyboard_width = 1112;
  int keyboard_height = 820;
};

void MakeKeyboard(DecryptState *state, lv_obj_t *panel, bool pin,
                  const DecryptKeyboardLayout &layout) {
  state->input = widgets::TextArea(panel);
  lv_obj_set_pos(state->input, layout.input_x, layout.input_y);
  lv_obj_set_size(state->input, layout.input_width, 160);
  lv_textarea_set_one_line(state->input, true);
  lv_textarea_set_password_mode(state->input, true);
  lv_textarea_set_password_show_time(state->input, 0);
  lv_textarea_set_text(state->input, "");
  lv_textarea_set_placeholder_text(state->input,
                                   i18n::Translate(pin ? "Enter PIN" : "Enter password"));
  lv_textarea_set_max_length(state->input, state->screen_lock && pin ? 16 : 128);
  if (pin) lv_textarea_set_accepted_chars(state->input, "0123456789");
  lv_obj_set_style_text_font(state->input, UiFont(&lv_font_montserrat_32), 0);
  lv_obj_set_style_text_color(state->input, kText, 0);
  lv_obj_set_style_text_color(state->input, kMuted, LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(state->input, kMainBottom, 0);
  lv_obj_set_style_border_color(state->input, kLineBright, 0);
  lv_obj_set_style_border_color(state->input, kAccent, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(state->input, 2, 0);
  lv_obj_set_style_radius(state->input, 28, 0);
  lv_obj_set_style_pad_all(state->input, 38, 0);

  if (pin) {
    lv_obj_set_style_text_font(state->input, UiFont(&lv_font_montserrat_48), 0);
    lv_obj_set_style_text_align(state->input, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(state->input, 12, 0);
    lv_obj_set_style_bg_opa(state->input, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->input, 0, 0);
    lv_obj_set_style_border_width(state->input, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_opa(state->input, LV_OPA_TRANSP, LV_PART_CURSOR);
    MakePinPad(state, layout.pad_parent ? layout.pad_parent : panel,
               layout.pad_x, layout.pad_y, layout.key_size, layout.gap_x,
               layout.gap_y, layout.side_pad);
    return;
  }

  lv_obj_t *keyboard_parent = layout.pad_parent ? layout.pad_parent : panel;
  state->keyboard = lv_keyboard_create(keyboard_parent);
  phone_keyboard::Apply(state->keyboard);
  lv_obj_set_align(state->keyboard, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(state->keyboard, layout.keyboard_x, layout.keyboard_y);
  lv_obj_set_size(state->keyboard, layout.keyboard_width,
                  layout.keyboard_height);
  lv_keyboard_set_mode(state->keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(state->keyboard, state->input);
}

void RefreshDecryptLanguage(DecryptState *state) {
  const std::string target = state->user_name.empty()
      ? i18n::Format("Android user %d", state->user_id) : state->user_name;
  const std::string title = state->screen_lock ? i18n::Translate("Unlock AERA") : state->secondary
      ? i18n::Format("Unlock %s", target.c_str())
      : i18n::Translate("Unlock your storage");
  lv_label_set_text(state->title, title.c_str());
  if (state->input)
    lv_textarea_set_placeholder_text(state->input, i18n::Translate(
        state->credential_type == 3 ? "Enter PIN" : "Enter password"));
  const bool rtl = i18n::IsRightToLeft();
  lv_label_set_text(state->submit_icon, rtl ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT);
  lv_obj_align(state->submit_icon, rtl ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
               rtl ? 44 : -44, 0);
  i18n::ApplyDirection(state->screen);
  if (state->pattern_area && state->sequence_length > 0)
    lv_label_set_text(state->status,
        i18n::Format("%d dots connected", state->sequence_length).c_str());
}

void OpenDecryptLanguage(DecryptState *state) {
  if (state->busy || state->submit_pending || state->language_picker) return;
  const int screen_width = lv_obj_get_width(state->screen);
  const int screen_height = lv_obj_get_height(state->screen);
  auto *overlay = lv_obj_create(state->screen);
  Clear(overlay);
  lv_obj_set_user_data(overlay, &widgets::kModalMarker);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(overlay, screen_width, screen_height);
  lv_obj_set_style_bg_color(overlay, kCanvas, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  state->language_picker = overlay;
  lv_obj_add_event_cb(overlay, [](lv_event_t *event) {
    static_cast<DecryptState *>(lv_event_get_user_data(event))->language_picker = nullptr;
  }, LV_EVENT_DELETE, state);

  auto *title = Label(overlay, "Language", &lv_font_montserrat_48, kText);
  lv_obj_set_pos(title, 80, 72);
  lv_obj_set_width(title, screen_width - 640);
  auto *close = widgets::Button(overlay, "Back", [overlay] { lv_obj_delete_async(overlay); });
  lv_obj_set_size(close, 360, 100);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -80, 56);
  auto *list = widgets::Scroll(overlay, 220, screen_height - 280);
  lv_obj_update_layout(list);
  const int width = lv_obj_get_width(list) - 28;
  const auto &languages = i18n::AvailableLanguages();
  for (size_t index = 0; index < languages.size(); ++index) {
    const auto language = languages[index];
    auto *row = widgets::Button(list, "", {},
        std::string(i18n::CurrentLanguage()) == language.code);
    lv_obj_set_pos(row, 0, static_cast<int>(index) * 154);
    lv_obj_set_size(row, width, 142);
    auto *native = Label(row, language.native_name, &lv_font_montserrat_32, kText);
    lv_obj_set_pos(native, 28, 20);
    lv_obj_set_width(native, width - 100);
    auto *english = Label(row, language.name, &lv_font_montserrat_24, kMuted);
    lv_obj_set_pos(english, 28, 80);
    lv_obj_set_width(english, width - 100);
    widgets::OnClick(row, [state, overlay, language] {
      if (state->busy || state->submit_pending) return;
      if (!RecoverySetLanguage(language.code) || !i18n::SetLanguage(language.code)) {
        widgets::Sheet(overlay, "Language unavailable",
            "AERA could not apply the selected language.");
        return;
      }
      RefreshDecryptLanguage(state);
      // Storage may still be encrypted. Keep the choice in memory without
      // attempting to create preference files or enter the settings scene.
      lv_obj_delete_async(overlay);
    });
  }
}

}  // namespace

DecryptScene BuildDecryptScene(lv_obj_t *screen, int credential_type,
                               bool file_based, int user_id,
                               int pattern_grid_size,
                               ActionCallback callback, void *context,
                               const std::string &user_name, bool screen_lock) {
  auto *state = new DecryptState;
  state->screen_lock = screen_lock;
  state->callback = callback;
  state->context = context;
  state->credential_type = credential_type;
  state->grid_size = std::clamp(pattern_grid_size, 3, 6);
  state->secondary = user_id != 0;
  state->screen = screen;
  state->user_id = user_id;
  state->user_name = user_name;
  lv_obj_add_event_cb(screen, DeleteState, LV_EVENT_DELETE, state);
  MainBackground(screen);
  AttachStatusBar(screen, callback, context, StatusBarAction::kNone, true);
  const int screen_width = lv_obj_get_width(screen);
  const int screen_height = lv_obj_get_height(screen);
  const bool landscape = widgets::Landscape(screen);
  state->language_button = widgets::Button(screen, "Language", [state] { OpenDecryptLanguage(state); });
  lv_obj_set_style_text_align(lv_obj_get_child(state->language_button, 0),
                             LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_size(state->language_button, 420, 100);
  lv_obj_align(state->language_button, LV_ALIGN_TOP_RIGHT, -80,
               landscape ? 130 : 172);

  if(screen_lock)lv_obj_add_flag(state->language_button,LV_OBJ_FLAG_HIDDEN);
  (void)file_based;
  const bool pin = credential_type == 3;

  const std::string target = user_name.empty()
      ? i18n::Format("Android user %d", user_id)
      : user_name;
  const std::string title_text = state->screen_lock ? i18n::Translate("Unlock AERA") : state->secondary
      ? i18n::Format("Unlock %s", target.c_str())
      : i18n::Translate("Unlock your storage");
  lv_obj_t *title =
      Label(screen, title_text.c_str(), &lv_font_montserrat_48, kText);
  state->title = title;
  lv_obj_t *subtitle = Label(
      screen,
      state->screen_lock ? "Use your AERA PIN to continue." :
      pin ? "Use your phone's PIN to continue." :
      (credential_type == 2 ? "Draw your phone's unlock pattern." :
                              "Use your phone's password to continue."),
      &lv_font_montserrat_32, kMuted);

  lv_obj_t *emblem = nullptr;
  int panel_x = 0, panel_y = 0, panel_width = 0, panel_height = 0;
  lv_obj_t *pad_panel = nullptr;
  int pad_width = 0, pad_height = 0;

  if (!landscape) {
    // A small native lock illustration keeps startup lightweight.
    emblem = lv_obj_create(screen);
    Clear(emblem);
    lv_obj_set_size(emblem, 208, 208);
    lv_obj_align(emblem, LV_ALIGN_TOP_MID, 0, 310);
    lv_obj_set_style_pad_all(emblem, 0, 0);
    lv_obj_remove_flag(emblem, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *shackle = lv_obj_create(emblem);
    Clear(shackle);
    lv_obj_set_size(shackle, 74, 86);
    lv_obj_set_pos(shackle, 67, 44);
    lv_obj_set_style_radius(shackle, 38, 0);
    lv_obj_set_style_border_width(shackle, 9, 0);
    lv_obj_set_style_border_color(shackle, kAccent, 0);
    lv_obj_t *lock_body = lv_obj_create(emblem);
    Panel(lock_body, 16, kAccent);
    lv_obj_set_size(lock_body, 104, 80);
    lv_obj_set_pos(lock_body, 52, 94);
    lv_obj_set_style_pad_all(lock_body, 0, 0);
    lv_obj_set_style_bg_opa(lock_body, LV_OPA_COVER, 0);
    lv_obj_set_style_blur_backdrop(lock_body, false, 0);
    lv_obj_set_style_blur_radius(lock_body, 0, 0);
    lv_obj_t *keyhole_dot = lv_obj_create(lock_body);
    Clear(keyhole_dot);
    lv_obj_set_size(keyhole_dot, 18, 18);
    lv_obj_align(keyhole_dot, LV_ALIGN_CENTER, 0, -7);
    lv_obj_set_style_radius(keyhole_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(keyhole_dot, kCanvas, 0);
    lv_obj_set_style_bg_opa(keyhole_dot, LV_OPA_COVER, 0);
    lv_obj_t *keyhole_stem = lv_obj_create(lock_body);
    Clear(keyhole_stem);
    lv_obj_set_size(keyhole_stem, 8, 24);
    lv_obj_align(keyhole_stem, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_radius(keyhole_stem, 4, 0);
    lv_obj_set_style_bg_color(keyhole_stem, kCanvas, 0);
    lv_obj_set_style_bg_opa(keyhole_stem, LV_OPA_COVER, 0);

    lv_obj_set_width(title, 1280);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 584);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 672);
    panel_width = std::min(1600, screen_width - 128);
    panel_x = (screen_width - panel_width) / 2;
    panel_y = pin ? 780 : 740;
    panel_height = std::max(1200, screen_height - panel_y - 24);
  } else {
    lv_obj_set_width(title, 1200);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(title, 80, 150);
    lv_obj_set_width(subtitle, 1200);
    lv_obj_set_pos(subtitle, 80, 240);

    panel_x = 80;
    panel_y = 330;
    panel_width = std::min(1200, screen_width / 2 - 40);
    panel_height = screen_height - panel_y - 24;
    pad_width = screen_width - (panel_x + panel_width + 40) - 80;
    pad_height = screen_height - 174;
    pad_panel = lv_obj_create(screen);
    Clear(pad_panel);
    lv_obj_set_pos(pad_panel, panel_x + panel_width + 40, 150);
    lv_obj_set_size(pad_panel, pad_width, pad_height);
    lv_obj_set_style_pad_all(pad_panel, 0, 0);
  }

  lv_obj_t *panel = lv_obj_create(screen);
  Clear(panel);
  lv_obj_set_pos(panel, panel_x, panel_y);
  lv_obj_set_size(panel, panel_width, panel_height);
  lv_obj_set_style_pad_all(panel, 0, 0);

  const int field_x = landscape ? 20 : 86;
  const int field_width = landscape ? panel_width - 40 : panel_width - 172;
  state->status = Label(panel,
                        credential_type == 2 ? "Draw your unlock pattern" :
                                               "",
                        &lv_font_montserrat_24, kMuted);
  lv_obj_set_pos(state->status, field_x, landscape ? 10 : pin ? 286 : 350);
  lv_obj_set_width(state->status, field_width);
  lv_obj_set_style_text_align(state->status, LV_TEXT_ALIGN_CENTER, 0);

  DecryptKeyboardLayout keyboard_layout;
  keyboard_layout.input_x = field_x;
  keyboard_layout.input_y = landscape ? 70 : (pin ? 90 : 470);
  keyboard_layout.input_width = field_width;
  keyboard_layout.pad_parent = pad_panel;
  const int portrait_key =
      screen_width >= 1700 ? 320 : 300;
  keyboard_layout.key_size = landscape ? 220 : portrait_key;
  keyboard_layout.gap_x = landscape ? 32 : (screen_width >= 1700 ? 60 : 80);
  keyboard_layout.gap_y = 32;
  keyboard_layout.side_pad = landscape
      ? 0
      : (field_width - (keyboard_layout.key_size * 3 +
                        keyboard_layout.gap_x * 2)) / 2;
  keyboard_layout.pad_x = landscape
      ? (pad_width - (keyboard_layout.key_size * 3 + keyboard_layout.gap_x * 2)) / 2
      : field_x;
  keyboard_layout.pad_y = landscape
      ? (pad_height - (keyboard_layout.key_size * 4 + keyboard_layout.gap_y * 3)) / 2
      : 420;
  keyboard_layout.keyboard_x = 40;
  keyboard_layout.keyboard_y = 40;
  keyboard_layout.keyboard_width = landscape
      ? std::max(600, pad_width - 80)
      : std::max(600, panel_width - 160);
  keyboard_layout.keyboard_height = std::min(850, pad_height - 80);

  if (credential_type == 2) {
    if (landscape)
      MakePattern(state, pad_panel, (pad_width - 1012) / 2, 150);
    else
      MakePattern(state, panel, 86, 500);
  } else {
    MakeKeyboard(state, panel, pin, keyboard_layout);
  }

  const int portrait_submit_y = pin
      ? std::max(1510, 420 + (keyboard_layout.key_size * 4 +
                              keyboard_layout.gap_y * 3) + 70)
      : 1818;
  const int submit_y = landscape ? 600 : portrait_submit_y;
  state->submit =
      MakeButton(panel, state->screen_lock ? "Unlock" : state->secondary ? "Unlock user" : "Unlock storage", kAccent, kCanvas);
  lv_obj_set_pos(state->submit, field_x, submit_y);
  lv_obj_set_size(state->submit, field_width, 142);
  lv_obj_set_style_radius(state->submit, 71, 0);
  state->submit_label = lv_obj_get_child(state->submit, 0);
  lv_obj_t* submit_icon =
      Label(state->submit, i18n::IsRightToLeft() ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT,
            &lv_font_montserrat_32, kCanvas);
  state->submit_icon = submit_icon;
  lv_obj_align(submit_icon, i18n::IsRightToLeft() ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
               i18n::IsRightToLeft() ? 44 : -44, 0);
  Bind(state->submit, Action::kDecryptSubmit, state);

  lv_obj_t *skip = MakeButton(panel, state->screen_lock ? "Cancel" : "Skip for now", kCanvas,
                              kMutedStrong);
  lv_obj_set_style_bg_opa(skip, LV_OPA_TRANSP, 0);
  lv_obj_set_pos(skip, field_x, landscape ? 760 : submit_y + 180);
  lv_obj_set_size(skip, field_width, 120);
  Bind(skip, Action::kDecryptSkip, state);

  lv_obj_t *hint = Label(
      panel,
      state->screen_lock ? "Your recovery session stays locked until you unlock it." :
      state->secondary ? "This user remains locked until you unlock it."
                       : "Skipping keeps internal storage locked.",
      &lv_font_montserrat_24, kDim);
  lv_obj_set_width(hint, field_width);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, landscape ? -24 : -30);

  lv_obj_move_foreground(state->language_button);
  if (emblem != nullptr) AnimateEnter(emblem, 0, 16);
  AnimateEnter(title, 30, 20);
  AnimateEnter(panel, 70, 28);
  if (pad_panel != nullptr) AnimateEnter(pad_panel, 70, 28);
  return {state->status, state->input, state->submit, state};
}

std::string GetDecryptCredential(const DecryptScene &scene) {
  auto *state = static_cast<DecryptState *>(scene.state);
  if (state == nullptr) return {};
  if (state->credential_type != 2)
    return state->input == nullptr ? std::string()
                                   : lv_textarea_get_text(state->input);
  std::string passphrase;
  for (int i = 0; i < state->sequence_length; ++i)
    passphrase.push_back(static_cast<char>(state->sequence[i] + '1'));
  return passphrase;
}

void SetDecryptBusy(const DecryptScene &scene) {
  auto *state = static_cast<DecryptState *>(scene.state);
  if (state == nullptr) return;
  state->busy = true;
  if (state->language_button) lv_obj_add_state(state->language_button, LV_STATE_DISABLED);
  if (state->language_picker) lv_obj_delete_async(state->language_picker);
  i18n::BindLabel(state->status, state->screen_lock ? "Checking PIN..." : state->secondary
                                       ? "Unlocking Android user..."
                                       : "Unlocking and preparing /data...");
  i18n::BindLabel(state->submit_label, state->screen_lock ? "Checking..." : "Unlocking...");
  lv_obj_add_state(state->submit, LV_STATE_DISABLED);
  if (state->keyboard != nullptr) lv_obj_add_state(state->keyboard, LV_STATE_DISABLED);
  for (auto *button : state->grid_buttons)
    if (button != nullptr) lv_obj_add_state(button, LV_STATE_DISABLED);
}

void CompleteDecryptAttempt(const DecryptScene &scene, bool success) {
  auto *state = static_cast<DecryptState *>(scene.state);
  if (state == nullptr) return;
  if (success) {
    i18n::BindLabel(state->status, state->screen_lock ? "AERA unlocked" : state->secondary
                                         ? "Android user unlocked"
                                         : "Data unlocked successfully");
    lv_obj_set_style_text_color(state->status, kGreen, 0);
    return;
  }
  state->busy = false;
  state->submit_pending = false;
  if (state->language_button) lv_obj_remove_state(state->language_button, LV_STATE_DISABLED);
  i18n::BindLabel(state->status,
                    state->screen_lock ? "Incorrect AERA PIN. Try again." : state->secondary
                        ? "That credential did not unlock this user. Please try again."
                        : "That credential did not unlock data. Please try again.");
  lv_obj_set_style_text_color(state->status, kRed, 0);
  i18n::BindLabel(state->submit_label, "Try again");
  lv_obj_remove_state(state->submit, LV_STATE_DISABLED);
  if (state->keyboard != nullptr) lv_obj_remove_state(state->keyboard, LV_STATE_DISABLED);
  for (auto *button : state->grid_buttons)
    if (button != nullptr) lv_obj_remove_state(button, LV_STATE_DISABLED);
  if (state->input != nullptr) {
    lv_textarea_set_text(state->input, "");
    lv_obj_send_event(state->input, LV_EVENT_FOCUSED, nullptr);
  } else {
    ResetPattern(state);
  }
}

void BuildPreparingScene(lv_obj_t *screen, bool decrypted) {
  MainBackground(screen);
  AttachStatusBar(screen, nullptr, nullptr, StatusBarAction::kNone, true);
  lv_obj_t *icon = IconPlate(screen, decrypted ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
                             decrypted ? kGreen : kAmber,
                             decrypted ? kGreenSoft : kAmberSoft, 156);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -250);
  lv_obj_t *title = Label(screen,
                          decrypted ? "Data unlocked" : "Continuing encrypted",
                          &lv_font_montserrat_48, kText);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);
  lv_obj_t *copy = Label(
      screen,
      decrypted ? "Finishing recovery startup and mounting your storage..."
                : "Finishing recovery startup with internal storage locked...",
      &lv_font_montserrat_24, kMuted);
  lv_obj_align(copy, LV_ALIGN_CENTER, 0, 20);
  lv_obj_t *bar = lv_bar_create(screen);
  lv_obj_set_size(bar, 680, 18);
  lv_obj_align(bar, LV_ALIGN_CENTER, 0, 150);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, 100, LV_ANIM_ON);
  lv_obj_set_style_bg_color(bar, kPanelStrong, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, decrypted ? kGreen : kAmber,
                            LV_PART_INDICATOR);
  AnimateEnter(icon, 0, 24);
  AnimateEnter(title, 60, 20);
}

}  // namespace aeraui
