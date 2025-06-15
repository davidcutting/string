#include <string/application.hpp>

int main() {
    String::Application app;

    try {
        app.initialize();
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
