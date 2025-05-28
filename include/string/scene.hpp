#pragma once

#include <entt/entt.hpp>

namespace String {

class Scene {
public:
    Scene();

    entt::entity create_entity();
    void delete_entity(const entt::entity& entity);
    
private:
    entt::registry registry;
};

}  // namespace String
