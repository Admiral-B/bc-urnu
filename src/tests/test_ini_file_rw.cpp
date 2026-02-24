#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <fstream>
#include <string>
#include <cstdio>
#include <filesystem>

#include "gui/IniFileRW.hpp"

using Catch::Approx;
using bc::ini::IniFile;

// Helper: write a temp INI file and return the path
static std::string writeTempIni(const std::string& content, const std::string& name = "test_rw.ini") {
    namespace fs = std::filesystem;
    fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p);
    f << content;
    f.close();
    return p.string();
}

static void removeTempIni(const std::string& path) {
    std::remove(path.c_str());
}

// ── Load: basic key=value ────────────────────────────────────────────────────

TEST_CASE("IniFileRW loads basic key=value pairs", "[ini]") {
    auto path = writeTempIni("width=1024\nheight=768\n");
    IniFile ini;
    REQUIRE(ini.load(path));
    REQUIRE(ini.getString("width") == "1024");
    REQUIRE(ini.getString("height") == "768");
    removeTempIni(path);
}

TEST_CASE("IniFileRW keys are case-insensitive", "[ini]") {
    auto path = writeTempIni("Graphics_Width=800\n");
    IniFile ini;
    REQUIRE(ini.load(path));
    REQUIRE(ini.getString("graphics_width") == "800");
    REQUIRE(ini.getString("GRAPHICS_WIDTH") == "800");
    REQUIRE(ini.getString("Graphics_Width") == "800");
    removeTempIni(path);
}

TEST_CASE("IniFileRW strips surrounding quotes from values", "[ini]") {
    auto path = writeTempIni("lang=\"en\"\nname=\"Bridge Command\"\n");
    IniFile ini;
    REQUIRE(ini.load(path));
    REQUIRE(ini.getString("lang") == "en");
    REQUIRE(ini.getString("name") == "Bridge Command");
    removeTempIni(path);
}

TEST_CASE("IniFileRW trims whitespace around key and value", "[ini]") {
    auto path = writeTempIni("  key1  =  value1  \n  key2=value2\n");
    IniFile ini;
    REQUIRE(ini.load(path));
    REQUIRE(ini.getString("key1") == "value1");
    REQUIRE(ini.getString("key2") == "value2");
    removeTempIni(path);
}

TEST_CASE("IniFileRW returns default for missing key", "[ini]") {
    auto path = writeTempIni("existing=yes\n");
    IniFile ini;
    REQUIRE(ini.load(path));
    REQUIRE(ini.getString("nonexistent") == "");
    REQUIRE(ini.getString("nonexistent", "fallback") == "fallback");
    REQUIRE(ini.getUInt("nonexistent", 42) == 42);
    REQUIRE(ini.getFloat("nonexistent", 3.14f) == Approx(3.14f));
    removeTempIni(path);
}

TEST_CASE("IniFileRW handles missing file gracefully", "[ini]") {
    IniFile ini;
    REQUIRE_FALSE(ini.load("nonexistent_file_xyz.ini"));
    REQUIRE(ini.getString("key") == "");
    REQUIRE(ini.getUInt("key", 99) == 99);
}

TEST_CASE("IniFileRW handles empty value", "[ini]") {
    auto path = writeTempIni("key1=\nkey2=\"\"\n");
    IniFile ini;
    REQUIRE(ini.load(path));
    REQUIRE(ini.getString("key1") == "");
    REQUIRE(ini.getString("key2") == "");
    removeTempIni(path);
}

// ── Sections ─────────────────────────────────────────────────────────────────

TEST_CASE("IniFileRW parses section headers", "[ini]") {
    auto path = writeTempIni(
        "[Graphics]\n"
        "width=800\n"
        "[Sound]\n"
        "volume=0.5\n"
    );
    IniFile ini;
    REQUIRE(ini.load(path));
    // Keys are global (not section-scoped) matching existing BC behavior
    REQUIRE(ini.getString("width") == "800");
    REQUIRE(ini.getString("volume") == "0.5");

    auto sections = ini.sections();
    REQUIRE(sections.size() == 2);
    REQUIRE(sections[0] == "Graphics");
    REQUIRE(sections[1] == "Sound");
    removeTempIni(path);
}

TEST_CASE("IniFileRW keysInSection returns keys for given section", "[ini]") {
    auto path = writeTempIni(
        "[Graphics]\n"
        "width=800\n"
        "height=600\n"
        "[Sound]\n"
        "volume=0.5\n"
    );
    IniFile ini;
    REQUIRE(ini.load(path));
    auto gfxKeys = ini.keysInSection("Graphics");
    REQUIRE(gfxKeys.size() == 2);
    REQUIRE(gfxKeys[0] == "width");
    REQUIRE(gfxKeys[1] == "height");

    auto sndKeys = ini.keysInSection("Sound");
    REQUIRE(sndKeys.size() == 1);
    REQUIRE(sndKeys[0] == "volume");
    removeTempIni(path);
}

// ── Descriptions and Options ─────────────────────────────────────────────────

TEST_CASE("IniFileRW captures _DESC entries", "[ini]") {
    auto path = writeTempIni(
        "view_angle=90\n"
        "view_angle_DESC=The angle of view in degrees\n"
    );
    IniFile ini;
    REQUIRE(ini.load(path));
    REQUIRE(ini.getString("view_angle") == "90");
    REQUIRE(ini.getDescription("view_angle") == "The angle of view in degrees");
    removeTempIni(path);
}

TEST_CASE("IniFileRW captures _OPTION entries", "[ini]") {
    auto path = writeTempIni(
        "graphics_mode=3\n"
        "graphics_mode_OPTION=1\n"
        "graphics_mode_OPTION=2\n"
        "graphics_mode_OPTION=3\n"
    );
    IniFile ini;
    REQUIRE(ini.load(path));
    auto opts = ini.getOptions("graphics_mode");
    REQUIRE(opts.size() == 3);
    REQUIRE(opts[0] == "1");
    REQUIRE(opts[1] == "2");
    REQUIRE(opts[2] == "3");
    removeTempIni(path);
}

// ── Numeric parsing ──────────────────────────────────────────────────────────

TEST_CASE("IniFileRW parses unsigned integers", "[ini]") {
    auto path = writeTempIni("port=18304\nzero=0\nbig=4294967295\n");
    IniFile ini;
    REQUIRE(ini.load(path));
    REQUIRE(ini.getUInt("port") == 18304);
    REQUIRE(ini.getUInt("zero") == 0);
    REQUIRE(ini.getUInt("big") == 4294967295u);
    removeTempIni(path);
}

TEST_CASE("IniFileRW parses signed integers", "[ini]") {
    auto path = writeTempIni("positive=42\nnegative=-10\n");
    IniFile ini;
    REQUIRE(ini.load(path));
    REQUIRE(ini.getSInt("positive") == 42);
    REQUIRE(ini.getSInt("negative") == -10);
    removeTempIni(path);
}

TEST_CASE("IniFileRW parses floats", "[ini]") {
    auto path = writeTempIni("scale=1.5\nnegfloat=-0.25\nint_as_float=10\n");
    IniFile ini;
    REQUIRE(ini.load(path));
    REQUIRE(ini.getFloat("scale") == Approx(1.5f));
    REQUIRE(ini.getFloat("negfloat") == Approx(-0.25f));
    REQUIRE(ini.getFloat("int_as_float") == Approx(10.0f));
    removeTempIni(path);
}

TEST_CASE("IniFileRW returns default for malformed numbers", "[ini]") {
    auto path = writeTempIni("bad_int=abc\nbad_float=xyz\n");
    IniFile ini;
    REQUIRE(ini.load(path));
    REQUIRE(ini.getUInt("bad_int", 99) == 99);
    REQUIRE(ini.getSInt("bad_int", -1) == -1);
    REQUIRE(ini.getFloat("bad_float", 1.0f) == Approx(1.0f));
    removeTempIni(path);
}

// ── Write / round-trip ───────────────────────────────────────────────────────

TEST_CASE("IniFileRW round-trip preserves all entries", "[ini]") {
    std::string content =
        "BRIDGE COMMAND INI FILE TEMPLATE\n"
        "\n"
        "[Graphics]\n"
        "width=800\n"
        "width_DESC=Screen width\n"
        "height=600\n"
        "\n"
        "[Sound]\n"
        "volume=0.5\n";

    auto inPath = writeTempIni(content, "roundtrip_in.ini");
    auto outPath = std::filesystem::temp_directory_path() / "roundtrip_out.ini";

    IniFile ini;
    REQUIRE(ini.load(inPath));
    REQUIRE(ini.save(outPath.string()));

    // Re-read saved file
    IniFile ini2;
    REQUIRE(ini2.load(outPath.string()));
    REQUIRE(ini2.getString("width") == "800");
    REQUIRE(ini2.getString("height") == "600");
    REQUIRE(ini2.getString("volume") == "0.5");
    REQUIRE(ini2.getDescription("width") == "Screen width");
    REQUIRE(ini2.sections().size() == 2);

    removeTempIni(inPath);
    removeTempIni(outPath.string());
}

TEST_CASE("IniFileRW modified values are written correctly", "[ini]") {
    auto path = writeTempIni("[Graphics]\nwidth=800\nheight=600\n", "modify.ini");
    auto outPath = std::filesystem::temp_directory_path() / "modify_out.ini";

    IniFile ini;
    REQUIRE(ini.load(path));
    ini.setString("width", "1920");
    ini.setUInt("height", 1080);
    REQUIRE(ini.save(outPath.string()));

    IniFile ini2;
    REQUIRE(ini2.load(outPath.string()));
    REQUIRE(ini2.getString("width") == "1920");
    REQUIRE(ini2.getUInt("height") == 1080);

    removeTempIni(path);
    removeTempIni(outPath.string());
}

TEST_CASE("IniFileRW set adds new key if not found", "[ini]") {
    auto path = writeTempIni("[Graphics]\nwidth=800\n", "addkey.ini");
    auto outPath = std::filesystem::temp_directory_path() / "addkey_out.ini";

    IniFile ini;
    REQUIRE(ini.load(path));
    ini.setString("new_key", "new_value");
    ini.setFloat("new_float", 2.5f);
    REQUIRE(ini.save(outPath.string()));

    IniFile ini2;
    REQUIRE(ini2.load(outPath.string()));
    REQUIRE(ini2.getString("new_key") == "new_value");
    REQUIRE(ini2.getFloat("new_float") == Approx(2.5f));

    removeTempIni(path);
    removeTempIni(outPath.string());
}

TEST_CASE("IniFileRW preserves comments and blank lines on save", "[ini]") {
    std::string content =
        "PREAMBLE TEXT LINE\n"
        "\n"
        "[Section]\n"
        "key=value\n"
        "\n";

    auto inPath = writeTempIni(content, "preserve.ini");
    auto outPath = std::filesystem::temp_directory_path() / "preserve_out.ini";

    IniFile ini;
    REQUIRE(ini.load(inPath));
    REQUIRE(ini.save(outPath.string()));

    // Read raw output and verify structure is preserved
    std::ifstream f(outPath.string());
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(f, line)) {
        lines.push_back(line);
    }
    REQUIRE(lines.size() >= 4);
    REQUIRE(lines[0] == "PREAMBLE TEXT LINE");
    REQUIRE(lines[1] == "");
    REQUIRE(lines[2] == "[Section]");
    REQUIRE(lines[3] == "key=value");

    removeTempIni(inPath);
    removeTempIni(outPath.string());
}

TEST_CASE("IniFileRW setFloat uses reasonable precision", "[ini]") {
    auto path = writeTempIni("scale=1.0\n", "precision.ini");

    IniFile ini;
    REQUIRE(ini.load(path));
    ini.setFloat("scale", 1.09f);
    auto outPath = std::filesystem::temp_directory_path() / "precision_out.ini";
    REQUIRE(ini.save(outPath.string()));

    IniFile ini2;
    REQUIRE(ini2.load(outPath.string()));
    REQUIRE(ini2.getFloat("scale") == Approx(1.09f));

    removeTempIni(path);
    removeTempIni(outPath.string());
}

// ── hasKey ────────────────────────────────────────────────────────────────────

TEST_CASE("IniFileRW hasKey works", "[ini]") {
    auto path = writeTempIni("existing=yes\n");
    IniFile ini;
    REQUIRE(ini.load(path));
    REQUIRE(ini.hasKey("existing"));
    REQUIRE(ini.hasKey("EXISTING"));
    REQUIRE_FALSE(ini.hasKey("missing"));
    removeTempIni(path);
}

// ── Real-world BC ini format ─────────────────────────────────────────────────

TEST_CASE("IniFileRW handles real bc5.ini format", "[ini]") {
    std::string content =
        "BRIDGE COMMAND INI FILE TEMPLATE\n"
        "NOTE THAT THIS FILE IS OVERRIDDEN BY THE bc5.ini\n"
        "\n"
        "[Graphics]\n"
        "font=noto-sans\n"
        "font_OPTION=noto-sans, open-sans, tinos\n"
        "font_DESC=Font (either noto-sans, open-sans, or tinos)\n"
        "font_scale=1.0\n"
        "font_scale_DESC=Bridge Command uses (12 x scale) as font size\n"
        "graphics_mode=3\n"
        "graphics_mode_DESC=3 for borderless full screen\n"
        "graphics_mode_OPTION=1\n"
        "graphics_mode_OPTION=2\n"
        "graphics_mode_OPTION=3\n"
        "view_angle=90\n"
        "[Sound]\n"
        "wave_volume=0.25\n"
        "wave_volume_DESC=Adjustment for wave volume, between 0.0 and 1.0\n"
        "[Network]\n"
        "udp_send_port=18304\n";

    auto path = writeTempIni(content, "bc5_test.ini");
    IniFile ini;
    REQUIRE(ini.load(path));

    // Basic values
    REQUIRE(ini.getString("font") == "noto-sans");
    REQUIRE(ini.getFloat("font_scale") == Approx(1.0f));
    REQUIRE(ini.getUInt("graphics_mode") == 3);
    REQUIRE(ini.getUInt("view_angle") == 90);
    REQUIRE(ini.getFloat("wave_volume") == Approx(0.25f));
    REQUIRE(ini.getUInt("udp_send_port") == 18304);

    // Descriptions
    REQUIRE(ini.getDescription("font") == "Font (either noto-sans, open-sans, or tinos)");
    REQUIRE(ini.getDescription("graphics_mode") == "3 for borderless full screen");

    // Options
    auto modeOpts = ini.getOptions("graphics_mode");
    REQUIRE(modeOpts.size() == 3);

    // Sections
    auto sects = ini.sections();
    REQUIRE(sects.size() == 3);
    REQUIRE(sects[0] == "Graphics");
    REQUIRE(sects[1] == "Sound");
    REQUIRE(sects[2] == "Network");

    removeTempIni(path);
}

TEST_CASE("IniFileRW handles enumerated keys like joystick_map(1,1)", "[ini]") {
    auto path = writeTempIni(
        "joystick_map(1,1)=-1\n"
        "joystick_map(1,2)=-1\n"
        "joystick_map(2,1)=0\n"
    );
    IniFile ini;
    REQUIRE(ini.load(path));
    REQUIRE(ini.getString("joystick_map(1,1)") == "-1");
    REQUIRE(ini.getString("joystick_map(2,1)") == "0");
    REQUIRE(ini.getSInt("joystick_map(1,1)") == -1);
    removeTempIni(path);
}
