#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace completion {
    struct MapVisits {
        uint32_t width = 0;
        std::set<uint64_t> discovered;
        std::set<uint64_t> skipped;
    };

    class VisitHistory {
        using Key = std::pair<std::string, uint32_t>;
        std::map<Key, std::shared_ptr<MapVisits>> records;

        static void Put(std::vector<uint8_t> &out, uint64_t value)
        {
            do {
                out.push_back(static_cast<uint8_t>((value & 127) | (value > 127 ? 128 : 0)));
                value >>= 7;
            } while (value);
        }

        static bool Get(std::span<const uint8_t> &data, uint64_t &value)
        {
            value = 0;
            for (unsigned shift = 0; shift <= 63; shift += 7) {
                if (data.empty()) return false;
                const uint8_t byte = data.front();
                data = data.subspan(1);
                if (shift == 63 && (byte & 254)) return false;
                value |= static_cast<uint64_t>(byte & 127) << shift;
                if (!(byte & 128)) return true;
            }
            return false;
        }

        static void PutCells(std::vector<uint8_t> &out, const std::set<uint64_t> &cells)
        {
            std::vector<std::pair<uint64_t, uint64_t>> runs;
            for (const uint64_t cell : cells) {
                if (!runs.empty() && runs.back().second != UINT64_MAX && cell == runs.back().second + 1)
                    runs.back().second = cell;
                else
                    runs.emplace_back(cell, cell);
            }
            std::vector<uint8_t> packed_runs, deltas;
            Put(packed_runs, runs.size());
            uint64_t previous = 0;
            for (const auto &[first, last] : runs) {
                Put(packed_runs, first - previous);
                Put(packed_runs, last - first + 1);
                previous = last;
            }
            Put(deltas, cells.size());
            previous = 0;
            for (const uint64_t cell : cells) {
                Put(deltas, cell - previous);
                previous = cell;
            }
            const bool use_runs = packed_runs.size() < deltas.size();
            out.push_back(use_runs ? 1 : 0);
            const auto &packed = use_runs ? packed_runs : deltas;
            out.insert(out.end(), packed.begin(), packed.end());
        }

        static bool GetCells(std::span<const uint8_t> &data, std::set<uint64_t> &cells)
        {
            if (data.empty() || data.front() > 1) return false;
            const bool use_runs = data.front() == 1;
            data = data.subspan(1);
            uint64_t count, previous = 0, total = 0;
            if (!use_runs) {
                if (!Get(data, count) || count > 1000000 || count > data.size()) return false;
                for (uint64_t i = 0; i < count; ++i) {
                    uint64_t gap;
                    if (!Get(data, gap) || (i && !gap) || gap > UINT64_MAX - previous) return false;
                    previous += gap;
                    cells.insert(previous);
                }
                return true;
            }
            if (!Get(data, count) || count > 1000000 || count > data.size() / 2) return false;
            for (uint64_t i = 0; i < count; ++i) {
                uint64_t gap, length;
                if (!Get(data, gap) || !Get(data, length) || !length || (i && !gap)) return false;
                if (gap > UINT64_MAX - previous || length > 1000000 - total) return false;
                const uint64_t first = previous + gap;
                if (length - 1 > UINT64_MAX - first) return false;
                for (uint64_t j = 0; j < length; ++j) cells.insert(first + j);
                previous = first + length - 1;
                total += length;
            }
            return true;
        }

    public:
        MapVisits &For(const std::string &character, const uint32_t map, const uint32_t width)
        {
            auto &record = records[{character, map}];
            if (!record || record->width != width)
                record = std::make_shared<MapVisits>(MapVisits{width, {}, {}});
            else if (record.use_count() != 1)
                record = std::make_shared<MapVisits>(*record);
            return *record;
        }

        std::vector<uint8_t> Encode() const
        {
            std::vector<uint8_t> out{'W', 'C', 'V', 'M', 2};
            Put(out, records.size());
            for (const auto &[key, saved] : records) {
                const auto &record = *saved;
                Put(out, key.first.size());
                out.insert(out.end(), key.first.begin(), key.first.end());
                Put(out, key.second);
                Put(out, record.width);
                PutCells(out, record.discovered);
                PutCells(out, record.skipped);
            }
            return out;
        }

        bool Decode(std::span<const uint8_t> data)
        {
            constexpr uint8_t magic[] = {'W', 'C', 'V', 'M', 2};
            if (data.size() < sizeof(magic) || data.size() > 32 * 1024 * 1024) return false;
            for (size_t i = 0; i < sizeof(magic); ++i)
                if (data[i] != magic[i]) return false;
            data = data.subspan(sizeof(magic));
            uint64_t count;
            if (!Get(data, count) || count > 50000) return false;
            decltype(records) decoded;
            for (uint64_t i = 0; i < count; ++i) {
                uint64_t length, map, width;
                if (!Get(data, length) || length > 256 || length > data.size()) return false;
                std::string character(reinterpret_cast<const char *>(data.data()), static_cast<size_t>(length));
                data = data.subspan(static_cast<size_t>(length));
                if (!Get(data, map) || map > UINT32_MAX || !Get(data, width) || !width || width > UINT32_MAX) return false;
                MapVisits record{static_cast<uint32_t>(width), {}, {}};
                if (!GetCells(data, record.discovered) || !GetCells(data, record.skipped)) return false;
                for (const uint64_t fog : record.discovered)
                    if (fog > UINT32_MAX) return false;
                for (const uint64_t fog : record.skipped)
                    if (fog > UINT32_MAX) return false;
                if (!decoded.emplace(Key{character, static_cast<uint32_t>(map)}, std::make_shared<MapVisits>(std::move(record))).second) return false;
            }
            if (!data.empty()) return false;
            records.swap(decoded);
            return true;
        }
    };
}
