# The status bar without its views: the tray (StatusNotifierWatcher and its
# items), a dbusmenu client, the AppMenu registrar and the application menu,
# media players, battery, brightness and power glue, the clock's format and
# com.cutefish.Statusbar.
add_library(cutefish-shell-statusbar STATIC
    app_menu_registrar.cpp
    app_menu_registrar.h
    application_menu.cpp
    application_menu.h
    battery.cpp
    battery.h
    brightness.cpp
    brightness.h
    clock_format.cpp
    clock_format.h
    dbus_menu.cpp
    dbus_menu.h
    mpris.cpp
    mpris.h
    power_actions.cpp
    power_actions.h
    status_bar.cpp
    status_bar.h
    status_notifier_item.cpp
    status_notifier_item.h
    status_notifier_watcher.cpp
    status_notifier_watcher.h
    system_tray_model.cpp
    system_tray_model.h
    vpn_notifier.cpp
    vpn_notifier.h
)
target_link_libraries(cutefish-shell-statusbar PUBLIC cutefish-shell-common Cutefish::UPowerClient
    Cutefish::PowerProfilesClient)
