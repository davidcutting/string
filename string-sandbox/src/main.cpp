#include <string/application.hpp>

int main() {
    String::Application app;

    String::ApplicationInfo app_info {
        .application_name = "String Sandbox",
        .resources_directory = "/home/dcutting/.config/string/"
    };

    try {
        app.initialize(app_info);
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
