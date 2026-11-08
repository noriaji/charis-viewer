/**
 * @file realskyplaces.h
 * @brief The places of the real sky's "Seen From" list in the photo tools (research/177 §2.2):
 *        name (en, ja), region, latitude and longitude. No viewer code: realsky_test.cpp reads it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef PLUS_REALSKYPLACES_H
#define PLUS_REALSKYPLACES_H

#include <cmath>
#include <cstddef>

namespace plus::realstars
{
    // The list's groups, in the order they are shown (a heading line over each).
    enum Region
    {
        REGION_JAPAN,
        REGION_ASIA,
        REGION_EUROPE,
        REGION_AFRICA_MIDEAST,
        REGION_NORTH_AMERICA,
        REGION_SOUTH_AMERICA,
        REGION_OCEANIA,
        REGION_SPECIAL,
        REGION_COUNT
    };

    struct RegionName
    {
        const char* en;
        const char* ja;
    };

    inline constexpr RegionName REGIONS[REGION_COUNT] = {
        { "Japan", "日本" },
        { "Asia", "アジア" },
        { "Europe", "ヨーロッパ" },
        { "Africa and the Middle East", "アフリカ・中東" },
        { "North America", "北アメリカ" },
        { "South America", "南アメリカ" },
        { "Oceania", "オセアニア" },
        { "Special Latitudes", "特別な緯度" },
    };

    struct Place
    {
        const char* en;
        const char* ja;
        Region      region;
        float       lat;    // degrees, north positive
        float       lon;    // degrees, east positive
    };

    // Grouped by region (the list shows them in this order). Two decimals (about a kilometre):
    // the latitude and longitude rows step by 0.01, so a place typed in by hand is found again.
    // Nothing keeps the place's index: the list's choice is read back from PlusSkyStarsLat / Lon
    // (placeIndexOf), so places may be added or moved; the eight of the first list (Tokyo, Sapporo,
    // Naha, Mauna Kea, Sydney, London, New York, North Pole) keep their values and names.
    inline constexpr Place PLACES[] = {
        { "Tokyo", "東京", REGION_JAPAN, 35.68f, 139.77f },
        { "Sapporo", "札幌", REGION_JAPAN, 43.06f, 141.35f },
        { "Naha", "那覇", REGION_JAPAN, 26.21f, 127.68f },

        { "Seoul", "ソウル", REGION_ASIA, 37.57f, 126.98f },
        { "Beijing", "北京", REGION_ASIA, 39.90f, 116.41f },
        { "Shanghai", "上海", REGION_ASIA, 31.23f, 121.47f },
        { "Hong Kong", "香港", REGION_ASIA, 22.32f, 114.17f },
        { "Taipei", "台北", REGION_ASIA, 25.03f, 121.57f },
        { "Bangkok", "バンコク", REGION_ASIA, 13.76f, 100.50f },
        { "Singapore", "シンガポール", REGION_ASIA, 1.35f, 103.82f },
        { "Mumbai", "ムンバイ", REGION_ASIA, 19.08f, 72.88f },
        { "Dubai", "ドバイ", REGION_ASIA, 25.20f, 55.27f },

        { "London", "ロンドン", REGION_EUROPE, 51.51f, -0.13f },
        { "Paris", "パリ", REGION_EUROPE, 48.86f, 2.35f },
        { "Rome", "ローマ", REGION_EUROPE, 41.90f, 12.50f },
        { "Berlin", "ベルリン", REGION_EUROPE, 52.52f, 13.40f },
        { "Moscow", "モスクワ", REGION_EUROPE, 55.76f, 37.62f },
        { "Helsinki", "ヘルシンキ", REGION_EUROPE, 60.17f, 24.94f },
        { "Reykjavik", "レイキャビク", REGION_EUROPE, 64.15f, -21.94f },
        { "Tromso (Norway)", "トロムソ（ノルウェー）", REGION_EUROPE, 69.65f, 18.96f },
        { "La Palma (observatory)", "ラ・パルマ島（天文台）", REGION_EUROPE, 28.76f, -17.88f },

        { "Cairo", "カイロ", REGION_AFRICA_MIDEAST, 30.04f, 31.24f },
        { "Marrakesh", "マラケシュ", REGION_AFRICA_MIDEAST, 31.63f, -7.98f },
        { "Nairobi", "ナイロビ", REGION_AFRICA_MIDEAST, -1.29f, 36.82f },
        { "Windhoek (Namibia)", "ウィントフック（ナミビア）", REGION_AFRICA_MIDEAST, -22.56f, 17.07f },
        { "Cape Town", "ケープタウン", REGION_AFRICA_MIDEAST, -33.92f, 18.42f },

        { "New York", "ニューヨーク", REGION_NORTH_AMERICA, 40.71f, -74.01f },
        { "Chicago", "シカゴ", REGION_NORTH_AMERICA, 41.88f, -87.63f },
        { "Los Angeles", "ロサンゼルス", REGION_NORTH_AMERICA, 34.05f, -118.24f },
        { "Vancouver", "バンクーバー", REGION_NORTH_AMERICA, 49.28f, -123.12f },
        { "Mexico City", "メキシコシティ", REGION_NORTH_AMERICA, 19.43f, -99.13f },
        { "Fairbanks (Alaska)", "フェアバンクス（アラスカ）", REGION_NORTH_AMERICA, 64.84f, -147.72f },
        { "Mauna Kea", "マウナケア", REGION_NORTH_AMERICA, 19.82f, -155.47f },

        { "Rio de Janeiro", "リオデジャネイロ", REGION_SOUTH_AMERICA, -22.91f, -43.17f },
        { "Buenos Aires", "ブエノスアイレス", REGION_SOUTH_AMERICA, -34.60f, -58.38f },
        { "Atacama Desert", "アタカマ砂漠", REGION_SOUTH_AMERICA, -22.91f, -68.20f },
        { "Galapagos Islands", "ガラパゴス諸島", REGION_SOUTH_AMERICA, -0.74f, -90.31f },
        { "Ushuaia", "ウシュアイア", REGION_SOUTH_AMERICA, -54.80f, -68.30f },

        { "Sydney", "シドニー", REGION_OCEANIA, -33.87f, 151.21f },
        { "Uluru", "ウルル", REGION_OCEANIA, -25.34f, 131.04f },
        { "Christchurch", "クライストチャーチ", REGION_OCEANIA, -43.53f, 172.64f },
        { "Lake Tekapo", "テカポ湖", REGION_OCEANIA, -44.00f, 170.48f },
        { "Tahiti", "タヒチ", REGION_OCEANIA, -17.55f, -149.56f },

        { "North Pole", "北極", REGION_SPECIAL, 90.f, 0.f },
        { "Arctic Circle (Finland)", "北極圏の境（フィンランド）", REGION_SPECIAL, 66.56f, 25.85f },
        { "Tropic of Cancer (Taiwan)", "北回帰線（台湾）", REGION_SPECIAL, 23.44f, 120.42f },
        { "Equator (Ecuador)", "赤道（エクアドル）", REGION_SPECIAL, 0.f, -78.46f },
        { "Tropic of Capricorn (Australia)", "南回帰線（オーストラリア）", REGION_SPECIAL, -23.44f, 150.50f },
        { "South Pole", "南極点", REGION_SPECIAL, -90.f, 0.f },
    };

    inline constexpr std::size_t PLACE_COUNT = sizeof(PLACES) / sizeof(PLACES[0]);

    // Within this of a place's latitude and longitude, the settings are that place.
    inline constexpr double PLACE_MATCH_DEGREES = 0.005;

    // The place whose latitude and longitude these are, or PLACE_COUNT (none: "Custom").
    inline std::size_t placeIndexOf(double lat, double lon)
    {
        for (std::size_t i = 0; i < PLACE_COUNT; ++i)
        {
            if (std::fabs(lat - PLACES[i].lat) < PLACE_MATCH_DEGREES && std::fabs(lon - PLACES[i].lon) < PLACE_MATCH_DEGREES)
            {
                return i;
            }
        }
        return PLACE_COUNT;
    }
}

#endif // PLUS_REALSKYPLACES_H
