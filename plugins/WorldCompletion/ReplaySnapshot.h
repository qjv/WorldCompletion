#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <string>
#include <vector>

namespace completion {
// Versioned little-endian dump of planner inputs only: no character or account
// data. Mesh addresses become stable ordinal IDs, suitable for a native replay.
template<class Snapshot>
void SaveReplaySnapshot(const Snapshot& s, const std::filesystem::path& path)
{
    const auto temporary = path.string() + ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot create route replay");
    const auto word = [&](uint32_t n) { out.write(reinterpret_cast<const char*>(&n), 4); };
    const auto scalar = [&](float n) { out.write(reinterpret_cast<const char*>(&n), 4); };
    const auto point = [&](const auto& p) { scalar(p.x); scalar(p.y); };
    const auto position = [&](const auto& p) { point(p); word(p.zplane); };
    const auto set = [&](const auto& values) { word(static_cast<uint32_t>(values.size())); for (const auto v : values) word(static_cast<uint32_t>(v)); };
    out.write("WCRP", 4); word(1);
    word(static_cast<uint32_t>(s.map)); word(static_cast<uint32_t>(s.instance));
    word(s.width); word(s.height); word(s.quality); word(s.relaxation); word(s.restarts); word(static_cast<uint32_t>(s.end_portal));
    word(s.reveal_radius); word(s.previous_reveal_radius); scalar(s.entry_margin); word(s.include_outside);
    position(s.player); point(s.anchor); point(s.bounds.Min); point(s.bounds.Max); point(s.discovery_bounds.Min); point(s.discovery_bounds.Max);
    set(s.bits); set(s.skipped);
    word(static_cast<uint32_t>(s.confirmed_fog.size()));
    for (const auto fog : s.confirmed_fog) { word(static_cast<uint32_t>(fog)); word(static_cast<uint32_t>(fog >> 32)); }
    word(static_cast<uint32_t>(s.previous_waypoints.size())); for (const auto& p : s.previous_waypoints) position(p);
    word(static_cast<uint32_t>(s.endpoints.size())); for (const auto& p : s.endpoints) point(p);
    word(static_cast<uint32_t>(s.doorways.size()));
    for (const auto& d : s.doorways) { point(d.pos); scalar(d.radius_sq); }
    word(s.mask != nullptr);
    if (s.mask) {
        word(static_cast<uint32_t>(s.mask->x0)); word(static_cast<uint32_t>(s.mask->y0));
        word(s.mask->width); word(s.mask->height); word(s.mask->byte_count);
        out.write(reinterpret_cast<const char*>(s.mask->bits), s.mask->byte_count);
    }
    std::unordered_map<uintptr_t, uint32_t> addresses;
    uint32_t next = 1;
    for (const auto& plane : s.maps) for (const auto address : plane.addresses) addresses.emplace(address, next++);
    word(static_cast<uint32_t>(s.maps.size()));
    for (const auto& plane : s.maps) {
        word(plane.blocked); word(static_cast<uint32_t>(plane.trapezoids.size()));
        for (size_t i = 0; i < plane.trapezoids.size(); ++i) {
            const auto& t = plane.trapezoids[i];
            scalar(t.XTL); scalar(t.XTR); scalar(t.YT); scalar(t.XBL); scalar(t.XBR); scalar(t.YB);
            word(addresses.at(plane.addresses[i]));
            std::vector<uint32_t> links;
            for (const auto address : plane.links[i])
                if (const auto found = addresses.find(address); found != addresses.end()) links.push_back(found->second);
            set(links);
        }
    }
    out.close();
    if (!out) throw std::runtime_error("Route replay write failed");
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::rename(temporary, path);
}

template<class Snapshot, class Mask>
Snapshot LoadReplaySnapshot(const std::filesystem::path& path, Mask& mask, std::vector<uint8_t>& mask_bytes)
{
    if (std::filesystem::file_size(path) > 128 * 1024 * 1024) throw std::runtime_error("Replay too large");
    std::ifstream in(path, std::ios::binary);
    const auto word = [&] { uint32_t n = 0; if (!in.read(reinterpret_cast<char*>(&n), 4)) throw std::runtime_error("Truncated replay"); return n; };
    const auto count = [&] { const auto n = word(); if (n > 4000000) throw std::runtime_error("Invalid replay count"); return n; };
    const auto scalar = [&] { float n = 0; if (!in.read(reinterpret_cast<char*>(&n), 4)) throw std::runtime_error("Truncated replay"); return n; };
    const auto point = [&](auto& p) { p.x = scalar(); p.y = scalar(); };
    const auto position = [&](auto& p) { point(p); p.zplane = word(); };
    const auto set = [&](auto& values) { const auto n = count(); for (uint32_t i = 0; i < n; ++i) values.insert(word()); };
    char magic[4]{};
    in.read(magic, 4);
    if (std::string(magic, 4) != "WCRP" || word() != 1) throw std::runtime_error("Invalid replay version");
    Snapshot s;
    s.map = static_cast<decltype(s.map)>(word()); s.instance = static_cast<decltype(s.instance)>(word());
    s.width = word(); s.height = word(); s.quality = static_cast<int>(word()); s.relaxation = static_cast<int>(word()); s.restarts = static_cast<int>(word());
    s.end_portal = static_cast<int32_t>(word()); s.reveal_radius = static_cast<int>(word()); s.previous_reveal_radius = static_cast<int>(word());
    s.entry_margin = scalar(); s.include_outside = word() != 0;
    position(s.player); point(s.anchor); point(s.bounds.Min); point(s.bounds.Max); point(s.discovery_bounds.Min); point(s.discovery_bounds.Max);
    s.bits.resize(count()); for (auto& n : s.bits) n = word();
    set(s.skipped);
    const auto confirmed_count = count();
    for (uint32_t i = 0; i < confirmed_count; ++i) { const uint64_t low = word(); s.confirmed_fog.insert(low | (uint64_t{word()} << 32)); }
    s.previous_waypoints.resize(count()); for (auto& p : s.previous_waypoints) position(p);
    s.endpoints.resize(count()); for (auto& p : s.endpoints) point(p);
    s.doorways.resize(count()); for (auto& d : s.doorways) { point(d.pos); d.radius_sq = scalar(); }
    if (word()) {
        mask.x0 = static_cast<int32_t>(word()); mask.y0 = static_cast<int32_t>(word());
        mask.width = static_cast<int>(word()); mask.height = static_cast<int>(word()); mask.byte_count = static_cast<int>(count());
        mask_bytes.resize(static_cast<size_t>(mask.byte_count));
        if (!in.read(reinterpret_cast<char*>(mask_bytes.data()), mask.byte_count)) throw std::runtime_error("Truncated replay mask");
        mask.bits = mask_bytes.data(); s.mask = &mask;
    }
    s.maps.resize(count());
    for (auto& plane : s.maps) {
        plane.blocked = word() != 0;
        plane.trapezoid_count = count();
        plane.trapezoids.resize(plane.trapezoid_count); plane.addresses.resize(plane.trapezoid_count); plane.links.resize(plane.trapezoid_count);
        for (size_t i = 0; i < plane.trapezoids.size(); ++i) {
            auto& t = plane.trapezoids[i];
            t.XTL = scalar(); t.XTR = scalar(); t.YT = scalar(); t.XBL = scalar(); t.XBR = scalar(); t.YB = scalar();
            plane.addresses[i] = word();
            plane.links[i].resize(count()); for (auto& address : plane.links[i]) address = word();
        }
    }
    if (in.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Trailing replay data");
    return s;
}
}
