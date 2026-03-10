#pragma once

#include <NsGui/UIElement.h>

BEGIN_NS(ecl::lua)

using namespace bg3se::lua;

struct GameStateChangedEvent : public lua::EventBase
{
    ecl::GameState FromState;
    ecl::GameState ToState;
};

struct KeyInputEvent : public lua::EventBase
{
    SDLKeyEvent Event;
    SDLScanCode Key;
    SDLKeyModifier Modifiers;
    bool Pressed;
    bool Repeat;
};

struct MouseButtonEvent : public lua::EventBase
{
    int Button;
    bool Pressed;
    int Clicks;
    int X;
    int Y;
};

struct MouseWheelEvent : public lua::EventBase
{
    float ScrollX;
    float ScrollY;
    int X;
    int Y;
};

struct ControllerAxisEvent : public lua::EventBase
{
    int32_t DeviceId;
    SDLControllerAxis Axis;
    float Value;
};

struct ControllerButtonEvent : public lua::EventBase
{
    int32_t DeviceId;
    SDLKeyEvent Event;
    SDLControllerButton Button;
    bool Pressed;
};

struct ViewportResizedEvent : public lua::EventBase
{
    int Width;
    int Height;
};

struct UIFocusChangedEvent : public lua::EventBase
{
    Noesis::UIElement* OldFocus{ nullptr };
    Noesis::UIElement* NewFocus{ nullptr };
};

END_NS()
