# The status bar's global menu bar: the user's menu, the active application's
# name and its exported menus, and the desktop's menus.
find_package(CutefishClients REQUIRED COMPONENTS accounts)

add_library(cutefish-shell-menubar STATIC
    src/shell/menubar/menu_bar_model.cpp
    src/shell/menubar/menu_bar_model.h
    src/shell/menubar/menu_bar_view.cpp
    src/shell/menubar/menu_bar_view.h
)
target_link_libraries(cutefish-shell-menubar PUBLIC cutefish-shell cutefish-shell-statusbar
    cutefish-shell-statusbar-view Cutefish::AccountsClient)
target_link_libraries(cutefish-compositor PRIVATE cutefish-shell-menubar)
