# The launcher without its view: its applications in the user's order, the
# search, the pages, and com.cutefish.Launcher.
add_library(cutefish-shell-launcher STATIC
    launcher.cpp
    launcher.h
    launcher_model.cpp
    launcher_model.h
)
target_link_libraries(cutefish-shell-launcher PUBLIC cutefish-shell-dock)
