#include "StratusEntityManager.h"
#include "StratusEntity2.h"
#include "StratusApplicationThread.h"

namespace stratus {
    EntityManager::EntityManager() {}

    void EntityManager::AddEntity(const Entity2Ptr& e) {
        std::unique_lock<std::shared_mutex> ul(_m);
        _entitiesToAdd.insert(e);
    }

    void EntityManager::RemoveEntity(const Entity2Ptr& e) {
        std::unique_lock<std::shared_mutex> ul(_m);
        _entitiesToRemove.insert(e);
    }

    bool EntityManager::Initialize() {
        return true;
    }
    
    SystemStatus EntityManager::Update(const double deltaSeconds) {
        CHECK_IS_APPLICATION_THREAD();

        // Notify processes of added/removed entities and allow them to
        // perform their process routine
        auto entitiesToAdd = std::move(_entitiesToAdd);
        auto addedComponents = std::move(_addedComponents);
        auto entitiesToRemove = std::move(_entitiesToRemove);
        for (EntityProcessPtr& ptr : _processes) {
            if (entitiesToAdd.size() > 0) ptr->EntitiesAdded(entitiesToAdd);
            if (addedComponents.size() > 0) ptr->EntityComponentsAdded(addedComponents);
            if (entitiesToRemove.size() > 0) ptr->EntitiesRemoved(entitiesToRemove);
            ptr->Process(deltaSeconds);
        }

        // Commit added/removed entities
        _entities.insert(entitiesToAdd.begin(), entitiesToAdd.end());
        _entities.erase(entitiesToRemove.begin(), entitiesToRemove.end());

        // If any processes have been added, tell them about all available entities
        // and allow them to perform their process routine for the first time
        auto processesToAdd = std::move(_processesToAdd);
        for (EntityProcessPtr& ptr : processesToAdd) {
            if (_entities.size() > 0) ptr->EntitiesAdded(_entities);
            ptr->Process(deltaSeconds);

            // Commit process to list
            _processes.push_back(std::move(ptr));
        }

        return SystemStatus::SYSTEM_CONTINUE;
    }
    
    void EntityManager::Shutdown() {
        _entities.clear();
        _entitiesToAdd.clear();
        _entitiesToRemove.clear();
        _processes.clear();
        _processesToAdd.clear();
        _addedComponents.clear();
    }
    
    void EntityManager::_RegisterEntityProcess(EntityProcessPtr& ptr) {
        std::unique_lock<std::shared_mutex> ul(_m);
        _processesToAdd.push_back(std::move(ptr));
    }
    
    void EntityManager::_NotifyComponentsAdded(const Entity2Ptr& ptr, Entity2Component * component) {
        std::unique_lock<std::shared_mutex> ul(_m);
        auto it = _addedComponents.find(ptr);
        if (it == _addedComponents.end()) {
            _addedComponents.insert(std::make_pair(ptr, std::vector<Entity2Component *>{component}));
        }
        else {
            it->second.push_back(component);
        }
    }
}