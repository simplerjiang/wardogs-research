#pragma once

#include <windows.h>

#include <string>

#include "draw_list.h"

class D3D12Overlay {
public:
    D3D12Overlay();
    ~D3D12Overlay();

    D3D12Overlay(const D3D12Overlay&) = delete;
    D3D12Overlay& operator=(const D3D12Overlay&) = delete;

    bool Create(bool vsync, std::string* error);
    bool Attach(HWND target, std::string* error);
    bool Render(const DrawList& list, std::string* error);
    void SetCapturePath(const std::wstring& path);
    bool capture_saved() const;
    const std::string& capture_note() const;
    void Detach();
    void SetVisible(bool visible);
    bool visible() const;
    HWND window() const;
    int width() const;
    int height() const;
    const char* present_path() const;
    const std::string& adapter_name() const;
    const std::string& shader_note() const;
    void Shutdown();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
