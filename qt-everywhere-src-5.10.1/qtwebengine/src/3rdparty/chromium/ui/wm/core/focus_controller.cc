// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/wm/core/focus_controller.h"

#include "base/auto_reset.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/client/capture_client.h"
#include "ui/aura/client/focus_change_observer.h"
#include "ui/aura/env.h"
#include "ui/aura/window_tracker.h"
#include "ui/events/event.h"
#include "ui/wm/core/focus_rules.h"
#include "ui/wm/core/window_util.h"
#include "ui/wm/public/activation_change_observer.h"

namespace wm {
namespace {

// When a modal window is activated, we bring its entire transient parent chain
// to the front. This function must be called before the modal transient is
// stacked at the top to ensure correct stacking order.
void StackTransientParentsBelowModalWindow(aura::Window* window) {
  if (window->GetProperty(aura::client::kModalKey) != ui::MODAL_TYPE_WINDOW)
    return;

  aura::Window* transient_parent = wm::GetTransientParent(window);
  while (transient_parent) {
    transient_parent->parent()->StackChildAtTop(transient_parent);
    transient_parent = wm::GetTransientParent(transient_parent);
  }
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////
// FocusController, public:

FocusController::FocusController(FocusRules* rules)
    : active_window_(nullptr),
      focused_window_(nullptr),
      updating_focus_(false),
      updating_activation_(false),
      rules_(rules),
      observer_manager_(this) {
  DCHECK(rules);
}

FocusController::~FocusController() = default;

////////////////////////////////////////////////////////////////////////////////
// FocusController, ActivationClient implementation:

void FocusController::AddObserver(ActivationChangeObserver* observer) {
  activation_observers_.AddObserver(observer);
}

void FocusController::RemoveObserver(ActivationChangeObserver* observer) {
  activation_observers_.RemoveObserver(observer);
}

void FocusController::ActivateWindow(aura::Window* window) {
  FocusWindow(window);
}

void FocusController::DeactivateWindow(aura::Window* window) {
  if (window)
    FocusWindow(rules_->GetNextActivatableWindow(window));
}

const aura::Window* FocusController::GetActiveWindow() const {
  return active_window_;
}

aura::Window* FocusController::GetActivatableWindow(aura::Window* window) {
  return rules_->GetActivatableWindow(window);
}

aura::Window* FocusController::GetToplevelWindow(aura::Window* window) {
  return rules_->GetToplevelWindow(window);
}

bool FocusController::CanActivateWindow(aura::Window* window) const {
  return rules_->CanActivateWindow(window);
}

////////////////////////////////////////////////////////////////////////////////
// FocusController, aura::client::FocusClient implementation:

void FocusController::AddObserver(
    aura::client::FocusChangeObserver* observer) {
  focus_observers_.AddObserver(observer);
}

void FocusController::RemoveObserver(
    aura::client::FocusChangeObserver* observer) {
  focus_observers_.RemoveObserver(observer);
}

void FocusController::FocusWindow(aura::Window* window) {
  FocusAndActivateWindow(
      ActivationChangeObserver::ActivationReason::ACTIVATION_CLIENT, window);
}

void FocusController::ResetFocusWithinActiveWindow(aura::Window* window) {
  DCHECK(window);
  if (!active_window_)
    return;
  if (!active_window_->Contains(window))
    return;
  SetFocusedWindow(window);
}

aura::Window* FocusController::GetFocusedWindow() {
  return focused_window_;
}

////////////////////////////////////////////////////////////////////////////////
// FocusController, ui::EventHandler implementation:
void FocusController::OnKeyEvent(ui::KeyEvent* event) {
}

void FocusController::OnMouseEvent(ui::MouseEvent* event) {
  if (event->type() == ui::ET_MOUSE_PRESSED && !event->handled())
    WindowFocusedFromInputEvent(static_cast<aura::Window*>(event->target()),
                                event);
}

void FocusController::OnScrollEvent(ui::ScrollEvent* event) {
}

void FocusController::OnTouchEvent(ui::TouchEvent* event) {
}

void FocusController::OnGestureEvent(ui::GestureEvent* event) {
  if (event->type() == ui::ET_GESTURE_BEGIN &&
      event->details().touch_points() == 1 &&
      !event->handled()) {
    WindowFocusedFromInputEvent(static_cast<aura::Window*>(event->target()),
                                event);
  }
}

////////////////////////////////////////////////////////////////////////////////
// FocusController, aura::WindowObserver implementation:

void FocusController::OnWindowVisibilityChanged(aura::Window* window,
                                                bool visible) {
  if (!visible)
    WindowLostFocusFromDispositionChange(window, window->parent());
}

void FocusController::OnWindowDestroying(aura::Window* window) {
  // A window's modality state will interfere with focus restoration during its
  // destruction.
  window->ClearProperty(aura::client::kModalKey);
  WindowLostFocusFromDispositionChange(window, window->parent());
}

void FocusController::OnWindowHierarchyChanging(
    const HierarchyChangeParams& params) {
  if (params.receiver == active_window_ &&
      params.target->Contains(params.receiver) && (!params.new_parent ||
      aura::client::GetFocusClient(params.new_parent) !=
          aura::client::GetFocusClient(params.receiver))) {
    WindowLostFocusFromDispositionChange(params.receiver, params.old_parent);
  }
}

void FocusController::OnWindowHierarchyChanged(
    const HierarchyChangeParams& params) {
  if (params.receiver == focused_window_ &&
      params.target->Contains(params.receiver) && (!params.new_parent ||
      aura::client::GetFocusClient(params.new_parent) !=
          aura::client::GetFocusClient(params.receiver))) {
    WindowLostFocusFromDispositionChange(params.receiver, params.old_parent);
  }
}

////////////////////////////////////////////////////////////////////////////////
// FocusController, private:

void FocusController::FocusAndActivateWindow(
    ActivationChangeObserver::ActivationReason reason,
    aura::Window* window) {
  if (window &&
      (window->Contains(focused_window_) || window->Contains(active_window_))) {
    StackActiveWindow();
    return;
  }

  // Focusing a window also activates its containing activatable window. Note
  // that the rules could redirect activation activation and/or focus.
  aura::Window* focusable = rules_->GetFocusableWindow(window);
  aura::Window* activatable =
      focusable ? rules_->GetActivatableWindow(focusable) : nullptr;

  // We need valid focusable/activatable windows in the event we're not clearing
  // focus. "Clearing focus" is inferred by whether or not |window| passed to
  // this function is non-NULL.
  if (window && (!focusable || !activatable))
    return;
  DCHECK((focusable && activatable) || !window);

  // Activation change observers may change the focused window. If this happens
  // we must not adjust the focus below since this will clobber that change.
  aura::Window* last_focused_window = focused_window_;
  if (!updating_activation_)
    SetActiveWindow(reason, window, activatable);

  // If the window's ActivationChangeObserver shifted focus to a valid window,
  // we don't want to focus the window we thought would be focused by default.
  bool activation_changed_focus = last_focused_window != focused_window_;
  if (!updating_focus_ && (!activation_changed_focus || !focused_window_)) {
    if (active_window_ && focusable)
      DCHECK(active_window_->Contains(focusable));
    SetFocusedWindow(focusable);
  }
}

void FocusController::SetFocusedWindow(aura::Window* window) {
  if (updating_focus_ || window == focused_window_)
    return;
  DCHECK(rules_->CanFocusWindow(window, nullptr));
  if (window)
    DCHECK_EQ(window, rules_->GetFocusableWindow(window));

  base::AutoReset<bool> updating_focus(&updating_focus_, true);
  aura::Window* lost_focus = focused_window_;

  // Allow for the window losing focus to be deleted during dispatch. If it is
  // deleted pass NULL to observers instead of a deleted window.
  aura::WindowTracker window_tracker;
  if (lost_focus)
    window_tracker.Add(lost_focus);
  if (focused_window_ && observer_manager_.IsObserving(focused_window_) &&
      focused_window_ != active_window_) {
    observer_manager_.Remove(focused_window_);
  }
  focused_window_ = window;
  if (focused_window_ && !observer_manager_.IsObserving(focused_window_))
    observer_manager_.Add(focused_window_);

  for (auto& observer : focus_observers_) {
    observer.OnWindowFocused(
        focused_window_,
        window_tracker.Contains(lost_focus) ? lost_focus : nullptr);
  }
  if (window_tracker.Contains(lost_focus)) {
    aura::client::FocusChangeObserver* observer =
        aura::client::GetFocusChangeObserver(lost_focus);
    if (observer)
      observer->OnWindowFocused(focused_window_, lost_focus);
  }
  aura::client::FocusChangeObserver* observer =
      aura::client::GetFocusChangeObserver(focused_window_);
  if (observer) {
    observer->OnWindowFocused(
        focused_window_,
        window_tracker.Contains(lost_focus) ? lost_focus : nullptr);
  }
}

void FocusController::SetActiveWindow(
    ActivationChangeObserver::ActivationReason reason,
    aura::Window* requested_window,
    aura::Window* window) {
  if (updating_activation_)
    return;

  if (window == active_window_) {
    if (requested_window) {
      for (auto& observer : activation_observers_)
        observer.OnAttemptToReactivateWindow(requested_window, active_window_);
    }
    return;
  }

  DCHECK(rules_->CanActivateWindow(window));
  if (window)
    DCHECK_EQ(window, rules_->GetActivatableWindow(window));

  base::AutoReset<bool> updating_activation(&updating_activation_, true);
  aura::Window* lost_activation = active_window_;
  // Allow for the window losing activation to be deleted during dispatch. If
  // it is deleted pass NULL to observers instead of a deleted window.
  aura::WindowTracker window_tracker;
  if (lost_activation)
    window_tracker.Add(lost_activation);
  if (active_window_ && observer_manager_.IsObserving(active_window_) &&
      focused_window_ != active_window_) {
    observer_manager_.Remove(active_window_);
  }
  active_window_ = window;
  if (active_window_ && !observer_manager_.IsObserving(active_window_))
    observer_manager_.Add(active_window_);
  if (active_window_)
    StackActiveWindow();

  ActivationChangeObserver* observer = nullptr;
  if (window_tracker.Contains(lost_activation)) {
    observer = GetActivationChangeObserver(lost_activation);
    if (observer)
      observer->OnWindowActivated(reason, active_window_, lost_activation);
  }
  observer = GetActivationChangeObserver(active_window_);
  if (observer) {
    observer->OnWindowActivated(
        reason, active_window_,
        window_tracker.Contains(lost_activation) ? lost_activation : nullptr);
  }
  for (auto& observer : activation_observers_) {
    observer.OnWindowActivated(
        reason, active_window_,
        window_tracker.Contains(lost_activation) ? lost_activation : nullptr);
  }
}

void FocusController::StackActiveWindow() {
  if (active_window_) {
    StackTransientParentsBelowModalWindow(active_window_);
    active_window_->parent()->StackChildAtTop(active_window_);
  }
}

void FocusController::WindowLostFocusFromDispositionChange(
    aura::Window* window,
    aura::Window* next) {
  // TODO(beng): See if this function can be replaced by a call to
  //             FocusWindow().
  // Activation adjustments are handled first in the event of a disposition
  // changed. If an activation change is necessary, focus is reset as part of
  // that process so there's no point in updating focus independently.
  if (window == active_window_) {
    aura::Window* next_activatable = rules_->GetNextActivatableWindow(window);
    SetActiveWindow(
        ActivationChangeObserver::ActivationReason::WINDOW_DISPOSITION_CHANGED,
        nullptr, next_activatable);
    if (!(active_window_ && active_window_->Contains(focused_window_)))
      SetFocusedWindow(next_activatable);
  } else if (window->Contains(focused_window_)) {
    // Active window isn't changing, but focused window might be.
    SetFocusedWindow(rules_->GetFocusableWindow(next));
  }
}

void FocusController::WindowFocusedFromInputEvent(aura::Window* window,
                                                  const ui::Event* event) {
  // Only focus |window| if it or any of its parents can be focused. Otherwise
  // FocusWindow() will focus the topmost window, which may not be the
  // currently focused one.
  if (rules_->CanFocusWindow(GetToplevelWindow(window), event)) {
    FocusAndActivateWindow(
        ActivationChangeObserver::ActivationReason::INPUT_EVENT, window);
  }
}

}  // namespace wm