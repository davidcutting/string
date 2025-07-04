#include <SDL3/SDL.h>
#include <iostream>

int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL_Init Error: " << SDL_GetError() << std::endl;
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow("Vulkan Window", 640, 480, SDL_WINDOW_RESIZABLE);
    if (!win) {
        std::cerr << "SDL_CreateWindow Error: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    bool quit = false;
    SDL_Event e;

    while (!quit) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                quit = true;
            }
        }

        static int counter = 0;
        if (++counter % 60 == 0) {
            SDL_SetWindowTitle(win, "Tick...");
        }

        SDL_Delay(16); // Delay ~60fps to prevent CPU spinning
    }

    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}