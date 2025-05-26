#pragma once

#include <entt/entt.hpp>

namespace String {

struct WindowResizeEvent {
    int width, height;
};

struct KeyPressEvent {
    int key;
    int scancode;
    int action;
    int mods;
};

struct KeyReleaseEvent {
    int key;
    int scancode;
    int mods;
};

class EventManager {
public:
    template<typename Event>
    void publish(Event&& event)
    {
        dispatcher_.enqueue(std::forward(event));
    }

    template<typename Event>
    void trigger(Event&& event)
    {
        dispatcher_.trigger(std::forward(event));
    }
    
    template<typename Event, typename Listener>
    void subscribe(Listener&& listener)
    {
        dispatcher_.sink<Event>().connect(std::forward(listener));
    }
    
    void update();

private:
    entt::dispatcher dispatcher_;
};

}  // namespace String
