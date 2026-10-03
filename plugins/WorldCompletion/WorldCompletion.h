#pragma once

#include <ToolboxUIPlugin.h>
#include <GWCA/Constants/Constants.h>
#include <GWCA/Constants/Maps.h>

class WorldCompletionPlugin final : public ToolboxUIPlugin {
public:
    WorldCompletionPlugin() = default;
    ~WorldCompletionPlugin() override = default;

    bool ShowOnWorldMap() const override { return true; }
    const char* Name() const override { return "World Completion"; }

    void Initialize(ImGuiContext* ctx, ImGuiAllocFns allocator_fns, HMODULE toolbox_dll) override;
    void SignalTerminate() override;
    bool CanTerminate() override;
    void Update(float delta) override;
    void Draw(IDirect3DDevice9* pDevice) override;
    void DrawSettings() override;
    void LoadSettings(const wchar_t* folder) override;
    void SaveSettings(const wchar_t* folder) override;

private:
    bool IsLoadedContext() const;
    bool IsPogahnMissionAdaptationActive() const;

    GW::Constants::MapID current_map_ = GW::Constants::MapID::None;
    GW::Constants::InstanceType current_instance_ = GW::Constants::InstanceType::Loading;
    bool show_debug_status_ = false;
    bool show_route_ = true;
    bool show_ground_route_ = true;
    bool occlude_ground_route_ = true;
    bool show_numbers_ = true;
    bool show_cursor_coordinates_ = true;
    float route_thickness_ = 2.5f;
    std::wstring settings_folder_;
};
