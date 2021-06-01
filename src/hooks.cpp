#include "hooks.hpp"
#include "replay_system.hpp"
#include "overlay_layer.hpp"

auto add_gd_hook(uintptr_t offset, void* hook, void* orig) {
    return MH_CreateHook(cast<void*>(gd::base + offset), hook, cast<void**>(orig));
}
#define ADD_GD_HOOK(o, f) add_gd_hook(o, f##_H, &f)

auto add_cocos_hook(const char* symbol, void* hook, void* orig) {
    return MH_CreateHook(GetProcAddress(GetModuleHandleA("libcocos2d.dll"), symbol), hook, cast<void**>(orig));
}
#define ADD_COCOS_HOOK(o, f) add_cocos_hook(o, f##_H, &f)

void Hooks::init() {
    ADD_COCOS_HOOK("?update@CCScheduler@cocos2d@@UAEXM@Z", CCScheduler_update);
    ADD_COCOS_HOOK("?dispatchKeyboardMSG@CCKeyboardDispatcher@cocos2d@@QAE_NW4enumKeyCodes@2@_N@Z", CCKeyboardDispatcher_dispatchKeyboardMSG);

    ADD_GD_HOOK(0x1FB780, PlayLayer::init);
    ADD_GD_HOOK(0x2029C0, PlayLayer::update);

    ADD_GD_HOOK(0x111500, PlayLayer::pushButton);
    ADD_GD_HOOK(0x111660, PlayLayer::releaseButton);
    ADD_GD_HOOK(0x20BF00, PlayLayer::resetLevel);

    ADD_GD_HOOK(0x20D3C0, PlayLayer::pauseGame);

    ADD_GD_HOOK(0x20B050, PlayLayer::createCheckpoint);
    ADD_GD_HOOK(0x20B830, PlayLayer::removeLastCheckpoint);

    ADD_GD_HOOK(0x1FD3D0, PlayLayer::levelComplete);
    ADD_GD_HOOK(0x20D810, PlayLayer::onQuit);
    ADD_GD_HOOK(0x1E60E0, PlayLayer::onEditor);

    ADD_GD_HOOK(0x1E4620, PauseLayer_init);

    ADD_GD_HOOK(0x1f4ff0, PlayerObject_ringJump);
    ADD_GD_HOOK(0xef0e0, GameObject_activateObject);
}

void __fastcall Hooks::CCScheduler_update_H(CCScheduler* self, int, float dt) {
    auto rs = ReplaySystem::get_instance();
    if (rs->is_playing() || rs->is_recording() && gd::GameManager::sharedState()->getPlayLayer()) {
        auto fps = rs->get_replay().get_fps();
        auto speedhack = self->getTimeScale();
        dt = 1.f / fps / speedhack;
    }
    CCScheduler_update(self, dt);
}

void __fastcall Hooks::CCKeyboardDispatcher_dispatchKeyboardMSG_H(CCKeyboardDispatcher* self, int, int key, bool down) {
    auto rs = ReplaySystem::get_instance();
    if (down) {
        auto play_layer = gd::GameManager::sharedState()->getPlayLayer();
        if (rs->is_recording() && play_layer) {
            if (key == 'C') {
                rs->set_frame_advance(false);
                PlayLayer::update_H(play_layer, 0, 1.f / rs->get_default_fps());
                rs->set_frame_advance(true);
            } else if (key == 'F') {
                rs->set_frame_advance(false);
            } else if (key == 'R') {
                PlayLayer::resetLevel_H(play_layer, 0);
            }
        }
    }
    CCKeyboardDispatcher_dispatchKeyboardMSG(self, key, down);
}


bool __fastcall Hooks::PlayLayer::init_H(gd::PlayLayer* self, int, void* level) {
    return init(self, level);
}

void __fastcall Hooks::PlayLayer::update_H(gd::PlayLayer* self, int, float dt) {
    auto rs = ReplaySystem::get_instance();
    if (rs->get_frame_advance()) return;
    if (rs->is_playing())
        rs->handle_playing();
    update(self, dt);
}


bool _player_button_handler(bool hold, bool button) {
    if (gd::GameManager::sharedState()->getPlayLayer()) {
        auto rs = ReplaySystem::get_instance();
        if (rs->is_playing()) return true;
        rs->record_action(hold, button);
    }
    return false;
}

int __fastcall Hooks::PlayLayer::pushButton_H(gd::PlayLayer* self, int, int idk, bool button) {
    if (_player_button_handler(true, button)) return 0;
    return pushButton(self, idk, button);
}

int __fastcall Hooks::PlayLayer::releaseButton_H(gd::PlayLayer* self, int, int idk, bool button) {
    if (_player_button_handler(false, button)) return 0;
    return releaseButton(self, idk, button);
}

int __fastcall Hooks::PlayLayer::resetLevel_H(gd::PlayLayer* self, int) {
    auto ret = resetLevel(self);
    ReplaySystem::get_instance()->on_reset();
    return ret;
}


void __fastcall Hooks::PlayLayer::pauseGame_H(gd::PlayLayer* self, int, bool idk) {
    auto addr = cast<void*>(gd::base + 0x20D43C);
    auto rs = ReplaySystem::get_instance();
    if (rs->is_recording())
        rs->record_action(false, true, false);

    bool should_patch = rs->is_playing();
    if (should_patch)
        patch(addr, {0x83, 0xC4, 0x04, 0x90, 0x90});
    
    pauseGame(self, idk);

    if (should_patch)
        patch(addr, {0xe8, 0x2f, 0x7b, 0xfe, 0xff});
}


int __fastcall Hooks::PlayLayer::createCheckpoint_H(gd::PlayLayer* self, int) {
    auto rs = ReplaySystem::get_instance();
    if (rs->is_recording()) rs->get_practice_fixes().add_checkpoint();
    return createCheckpoint(self);
}

void* __fastcall Hooks::PlayLayer::removeLastCheckpoint_H(gd::PlayLayer* self, int) {
    ReplaySystem::get_instance()->get_practice_fixes().remove_checkpoint();
    return removeLastCheckpoint(self);
}


void* __fastcall Hooks::PlayLayer::levelComplete_H(gd::PlayLayer* self, int) {
    ReplaySystem::get_instance()->reset_state();
    return levelComplete(self);
}

void _on_exit_level() {
    auto rs = ReplaySystem::get_instance();
    rs->get_practice_fixes().clear_checkpoints();
    rs->reset_state();
}

void* __fastcall Hooks::PlayLayer::onQuit_H(gd::PlayLayer* self, int) {
    _on_exit_level();
    return onQuit(self);
}

void* __fastcall Hooks::PlayLayer::onEditor_H(gd::PlayLayer* self, int, void* idk) {
    _on_exit_level();
    return onEditor(self, idk);
}

bool __fastcall Hooks::PauseLayer_init_H(gd::PauseLayer* self, int) {
    if (PauseLayer_init(self)) {
        auto win_size = CCDirector::sharedDirector()->getWinSize();
        
        auto menu = CCMenu::create();
        menu->setPosition(35, win_size.height - 40.f);
        self->addChild(menu);
        
        auto sprite = CCSprite::create("GJ_button_01.png");
        sprite->setScale(0.72f);
        auto btn = gd::CCMenuItemSpriteExtra::create(sprite, self, menu_selector(OverlayLayer::open_btn_callback));
        menu->addChild(btn);
        
        auto label = CCLabelBMFont::create("ReplayBot", "bigFont.fnt");
        label->setAnchorPoint({0, 0.5});
        label->setScale(0.5f);
        label->setPositionX(20);
        menu->addChild(label);
        return true;
    }
    return false;
}

void __fastcall Hooks::PlayerObject_ringJump_H(gd::PlayerObject* self, int, gd::GameObject* ring) {
    PlayerObject_ringJump(self, ring);
    auto rs = ReplaySystem::get_instance();
    auto play_layer = gd::GameManager::sharedState()->getPlayLayer();
    if (play_layer && play_layer->is_practice_mode && rs->is_recording() && ring->m_hasBeenActivated) {
        rs->get_practice_fixes().add_activated_object(ring);
    }
}

void __fastcall Hooks::GameObject_activateObject_H(gd::GameObject* self, int, gd::PlayerObject* player) {
    GameObject_activateObject(self, player);
    auto rs = ReplaySystem::get_instance();
    auto play_layer = gd::GameManager::sharedState()->getPlayLayer();
    if (play_layer && play_layer->is_practice_mode && rs->is_recording() && self->m_hasBeenActivated) {
        rs->get_practice_fixes().add_activated_object(self);
    }
}