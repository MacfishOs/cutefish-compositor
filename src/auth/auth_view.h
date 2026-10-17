// Copyright 2026 Reion Wong <reionwong@gmail.com>
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUTEFISH_AUTH_AUTH_VIEW_H
#define CUTEFISH_AUTH_AUTH_VIEW_H

#include <ctime>
#include <memory>
#include <optional>
#include <vector>

#include "base/geometry.h"
#include "base/signal.h"
#include "base/timer.h"
#include "ui/view.h"

namespace cutefish::ui {
class ColumnLayout;
class Window;
} // namespace cutefish::ui

namespace cutefish::auth {

class AuthController;
class ChoiceMenu;
class ErrorLabel;
class PasswordField;
class RaisedLabel;
class UserNameField;
class UserView;

// The lock screen and the greeter: the clock at the top and the login column
// at the bottom, as tall as the output. The compositor draws the wallpaper and
// the blur behind the password field; this is what goes over them.
// Upstream: LockContentsView.
class AuthScreen final : public ui::View
{
public:
    AuthScreen(ui::Window *window, AuthController *controller);
    ~AuthScreen() override;

    // Set while the session is blocked; the clock only ticks then, so a
    // prepared, unseen panel never wakes the compositor.
    void setBlocked(bool blocked);
    // Whether the panel has the keyboard: the password field looks focused
    // and its caret blinks only then.
    void setKeyboardFocused(bool focused);
    void focusPassword();

    // The password field in panel coordinates, for the blur behind it; it
    // moves with the shake.
    base::RectF fieldBounds() const;
    // Seconds since the epoch the clock shows instead of the time, for tests.
    void setClockForTesting(std::time_t now);

protected:
    void layout() override;
    void childrenLaidOut() override;

private:
    void tick();
    void closeMenus(ui::PointF position);

    ui::Window *m_window;
    AuthController *m_controller;
    base::Timer m_clockTimer;
    std::vector<base::Subscription> m_subscriptions;
    std::optional<std::time_t> m_fixedClock;
    bool m_blocked = false;
    RaisedLabel *m_date = nullptr;
    RaisedLabel *m_time = nullptr;
    ui::ColumnLayout *m_pod = nullptr;
    UserView *m_user = nullptr;
    ChoiceMenu *m_users = nullptr;
    UserNameField *m_userName = nullptr;
    PasswordField *m_password = nullptr;
    ErrorLabel *m_error = nullptr;
    ChoiceMenu *m_sessions = nullptr;
};

} // namespace cutefish::auth

#endif // CUTEFISH_AUTH_AUTH_VIEW_H
