// *************************************************************************
// *                  Revenant Revisited (port) - 2026                     *
// *   recording.cpp - playtest recordings (*.rvrec) and their bundles     *
// *************************************************************************

#include "recording.h"

#include "jsonout.h"
#include "logging.h"
#include "sha256.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

// ***********
// * Bundles *
// ***********

std::string Bundle::SlotSha(const fs::path& slot)
{
    std::error_code ec;
    if (!fs::is_directory(slot, ec))
        return {};

    std::vector<std::pair<std::string, fs::path>> files;
    for (fs::recursive_directory_iterator it(slot, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        std::string name = fs::relative(it->path(), slot, ec).generic_string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        files.emplace_back(std::move(name), it->path());
    }
    if (ec)
        return {};
    std::sort(files.begin(), files.end());

    TSha256 sha;
    static constexpr char kNul = '\0';
    for (const auto& [name, path] : files)
    {
        sha.Update(name);
        sha.Update(&kNul, 1);
        std::ifstream in(path, std::ios::binary);
        char buf[65536];
        while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
            sha.Update(buf, size_t(in.gcount()));
        sha.Update(&kNul, 1);
    }
    return sha.HexDigest();
}

bool Bundle::CopyDirectory(const fs::path& from, const fs::path& to, std::string& error)
{
    std::error_code ec;
    if (!fs::is_directory(from, ec))
    {
        error = "no directory " + from.string();
        return false;
    }
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        error = "can't copy " + from.string() + " to " + to.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// *************
// * Key names *
// *************

namespace
{

struct SKeyName
{
    int32_t     code;
    const char* name;
};

// PROTOCOL.md §2, with the rest of the Windows names the director knows.
constexpr SKeyName kKeyNames[] = {
    {0x08, "BACK"},     {0x09, "TAB"},      {0x0D, "RETURN"},   {0x10, "SHIFT"},
    {0x11, "CONTROL"},  {0x12, "MENU"},     {0x13, "PAUSE"},    {0x14, "CAPITAL"},
    {0x1B, "ESCAPE"},   {0x20, "SPACE"},    {0x21, "PRIOR"},    {0x22, "NEXT"},
    {0x23, "END"},      {0x24, "HOME"},     {0x25, "LEFT"},     {0x26, "UP"},
    {0x27, "RIGHT"},    {0x28, "DOWN"},     {0x2D, "INSERT"},   {0x2E, "DELETE"},
    {0x5B, "LWIN"},     {0x5C, "RWIN"},     {0x5D, "APPS"},
    {0x6A, "MULTIPLY"}, {0x6B, "ADD"},      {0x6C, "SEPARATOR"}, {0x6D, "SUBTRACT"},
    {0x6E, "DECIMAL"},  {0x6F, "DIVIDE"},   {0x90, "NUMLOCK"},  {0x91, "SCROLL"},
    {0xBA, "OEM_1"},    {0xBB, "OEM_PLUS"}, {0xBC, "OEM_COMMA"}, {0xBD, "OEM_MINUS"},
    {0xBE, "OEM_PERIOD"}, {0xBF, "OEM_2"},  {0xC0, "OEM_3"},    {0xDB, "OEM_4"},
    {0xDC, "OEM_5"},    {0xDD, "OEM_6"},    {0xDE, "OEM_7"},
};

} // namespace

std::string KeyNames::Name(int32_t vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return std::string(1, char(vk));
    if (vk >= 0x60 && vk <= 0x69)
        return "NUMPAD" + std::to_string(vk - 0x60);
    if (vk >= 0x70 && vk <= 0x87)
        return "F" + std::to_string(vk - 0x70 + 1);
    for (const SKeyName& k : kKeyNames)
        if (k.code == vk)
            return k.name;
    char hex[8];
    std::snprintf(hex, sizeof(hex), "0x%02X", unsigned(vk & 0xff));
    return hex;
}

int32_t KeyNames::Code(const std::string& name)
{
    if (name.size() == 1 && ((name[0] >= '0' && name[0] <= '9') || (name[0] >= 'A' && name[0] <= 'Z')))
        return name[0];
    if (name.size() == 7 && name.compare(0, 6, "NUMPAD") == 0 && name[6] >= '0' && name[6] <= '9')
        return 0x60 + (name[6] - '0');
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'F')
    {
        const int32_t n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24 && std::to_string(n) == name.substr(1))
            return 0x70 + n - 1;
    }
    for (const SKeyName& k : kKeyNames)
        if (name == k.name)
            return k.code;
    if (name.size() > 2 && (name.compare(0, 2, "0x") == 0 || name.compare(0, 2, "0X") == 0))
    {
        char* end = nullptr;
        const long code = std::strtol(name.c_str() + 2, &end, 16);
        if (end && *end == '\0' && code > 0 && code < 0x100)
            return int32_t(code);
    }
    return 0;
}

// **********
// * Events *
// **********

void WriteEventFields(JsonOut& out, const SInputEvent& event)
{
    switch (event.kind)
    {
      case SInputEvent::EKind::Key:
        out.FieldString("key", KeyNames::Name(event.code));
        out.FieldBool("down", event.down);
        break;
      case SInputEvent::EKind::Char:
        out.Field("char", event.code);
        break;
      case SInputEvent::EKind::Wheel:
        out.Field("wheel", event.code);
        out.Key("mouse").Begin('[').Value(event.x).Value(event.y).End(']');
        break;
      case SInputEvent::EKind::Pointer:
        out.Key("mouse").Begin('[').Value(event.x).Value(event.y).End(']');
        out.Field("buttons", event.buttons);
        break;
    }
}

bool ReadEventFields(const JsonValue& line, SInputEvent& event, std::string& error)
{
    if (line.Has("key"))
    {
        const int32_t vk = KeyNames::Code(line["key"].Str());
        if (!vk)
        {
            error = "unknown key '" + line["key"].Str() + "'";
            return false;
        }
        if (line["down"].Kind() != JsonValue::EKind::Bool)
        {
            error = "a key event needs down: true or false";
            return false;
        }
        event = SInputEvent::Key(vk, line["down"].Bool());
        return true;
    }
    if (line.Has("char"))
    {
        const int64_t code = line["char"].Int(-1);
        if (!line["char"].IsInt() || code <= 0 || code >= 0x100)
        {
            error = "char must be a Windows-1252 code";
            return false;
        }
        event = SInputEvent::Char(int32_t(code));
        return true;
    }
    if (line.Has("mouse"))
    {
        const JsonValue& pos = line["mouse"];
        const int64_t buttons = line["buttons"].Int(-1);
        if (!pos.IsArray() || pos.Items().size() != 2 || !pos[0].IsInt() || !pos[1].IsInt())
        {
            error = "mouse must be [x, y]";
            return false;
        }
        if (line.Has("wheel"))
        {
            const int64_t units = line["wheel"].Int();
            if (!line["wheel"].IsInt() || units < INT32_MIN || units > INT32_MAX)
            {
                error = "wheel must be an integer number of 1/120-notch units";
                return false;
            }
            event = SInputEvent::Wheel(int32_t(units), int32_t(pos[0].Int()), int32_t(pos[1].Int()));
            return true;
        }
        if (!line["buttons"].IsInt() || buttons < 0 || (buttons & ~SInputEvent::kButtonsAll))
        {
            error = "buttons must be a mask of 1, 2 and 4";
            return false;
        }
        event = SInputEvent::Pointer(int32_t(pos[0].Int()), int32_t(pos[1].Int()), int32_t(buttons));
        return true;
    }
    error = "not an input event";
    return false;
}

// ***********
// * Headers *
// ***********

bool SRecordingHeader::HasPatch(const std::string& patch) const
{
    return std::find(patches.begin(), patches.end(), patch) != patches.end();
}

namespace
{

std::string HeaderLine(const SRecordingHeader& h)
{
    JsonOut out;
    out.Begin('{');
    out.Field("rvrec", 1);
    out.FieldString("game", "port");
    out.Key("build").Begin('{');
    out.FieldString("sha", h.buildSha);
    out.FieldString("name", h.buildName);
    out.Key("patches").Begin('[');
    for (const std::string& p : h.patches)
        out.String(p);
    out.End(']');
    out.End('}');
    out.Key("start").Begin('{');
    out.FieldString("from", h.fromSlot ? "slot" : "newgame");
    if (h.fromSlot)
    {
        out.FieldString("slot", h.slot);
        out.FieldString("slot_sha", h.slotSha);
    }
    out.End('}');
    out.FieldString("ini_sha", h.iniSha);
    out.Field("seed", h.seed);
    out.Field("tick_hz", 24);
    out.Key("screen").Begin('[').Value(640).Value(480).End(']');
    out.FieldString("timing", h.timing == ETiming::Frames ? "frames" : "lockstep");
    out.FieldString("created", h.created);
    out.FieldString("by", h.by);
    if (h.revisited)
        out.FieldBool("revisited", true);
    out.End('}');
    return out.str();
}

bool ReadHeader(const JsonValue& v, SRecordingHeader& h, std::string& error)
{
    if (v["rvrec"].Int() != 1)
    {
        error = "not an rvrec 1 header";
        return false;
    }
    if (v["game"].Str() != "port")
    {
        error = "a recording of '" + v["game"].Str() + "' (this host replays the port's)";
        return false;
    }
    h.buildSha  = v["build"]["sha"].Str();
    h.buildName = v["build"]["name"].Str();
    for (const JsonValue& p : v["build"]["patches"].Items())
        h.patches.push_back(p.Str());
    h.fromSlot = v["start"]["from"].Str() == "slot";
    h.slot     = v["start"]["slot"].Str();
    h.slotSha  = v["start"]["slot_sha"].Str();
    h.iniSha   = v["ini_sha"].Str();
    if (!v["seed"].IsInt())
    {
        error = "the header has no seed";
        return false;
    }
    h.seed    = uint32_t(v["seed"].Int());
    const std::string timing = v["timing"].Str();
    if (timing != "lockstep" && timing != "frames")
    {
        error = "unknown timing '" + timing + "'";
        return false;
    }
    h.timing    = timing == "frames" ? ETiming::Frames : ETiming::Lockstep;
    h.created   = v["created"].Str();
    h.by        = v["by"].Str();
    h.revisited = v["revisited"].Bool();
    return true;
}

// The 16 hex digits of a double's IEEE-754 bits, most significant first.
std::string HexDouble(double d)
{
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016" PRIx64, bits);
    return hex;
}

bool ParseHexDouble(const std::string& hex, double& d)
{
    if (hex.size() != 16)
        return false;
    char* end = nullptr;
    const uint64_t bits = std::strtoull(hex.c_str(), &end, 16);
    if (!end || *end != '\0')
        return false;
    std::memcpy(&d, &bits, sizeof(d));
    return true;
}

} // namespace

// **********
// * Writer *
// **********

bool TRecordingWriter::Open(const fs::path& path, const SRecordingHeader& header)
{
    Close();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    file = std::fopen(path.string().c_str(), "wb");
    if (!file)
    {
        log_error("[playtest] can't write %s", path.string().c_str());
        return false;
    }
    timing    = header.timing;
    lastCheck = -1;
    Line(HeaderLine(header));
    return true;
}

void TRecordingWriter::Line(const std::string& json)
{
    if (!file)
        return;
    std::fputs(json.c_str(), file);
    std::fputc('\n', file);
    std::fflush(file);      // a session cut short keeps what it played
}

void TRecordingWriter::Event(int64_t when, const SInputEvent& event)
{
    JsonOut out;
    out.Begin('{');
    out.Field(timing == ETiming::Frames ? "f" : "t", when);
    WriteEventFields(out, event);
    out.End('}');
    Line(out.str());
}

void TRecordingWriter::Frame(int64_t f, double dt)
{
    JsonOut out;
    out.Begin('{').Field("f", f).FieldString("dt", HexDouble(dt)).End('}');
    Line(out.str());
}

void TRecordingWriter::Check(int64_t t, const std::string& fields)
{
    if (t == lastCheck)
        return;
    lastCheck = t;
    JsonOut out;
    out.Begin('{').Field("t", t).Key("check").Raw(fields).End('}');
    Line(out.str());
}

void TRecordingWriter::Mark(int64_t t, const std::string& kind,
                            const std::vector<std::pair<std::string, std::string>>& fields)
{
    JsonOut out;
    out.Begin('{').Field("t", t).FieldString("mark", kind);
    for (const auto& [key, value] : fields)
        if (key != "t" && key != "mark")
            out.Key(key.c_str()).Raw(value);
    out.End('}');
    Line(out.str());
}

void TRecordingWriter::End(int64_t t)
{
    JsonOut out;
    out.Begin('{').Field("t", t).FieldBool("end", true).End('}');
    Line(out.str());
    Close();
}

void TRecordingWriter::Close()
{
    if (file)
        std::fclose(file);
    file = nullptr;
}

// **********
// * Reader *
// **********

bool TRecording::Load(const fs::path& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        error = "can't read " + path.string();
        return false;
    }

    std::string text;
    int64_t number = 0;
    bool first = true;
    while (std::getline(in, text))
    {
        ++number;
        if (!text.empty() && text.back() == '\r')
            text.pop_back();
        if (text.empty())
            continue;
        JsonValue line;
        std::string why;
        if (!JsonValue::Parse(text, line, why) || !line.IsObject())
        {
            error = "line " + std::to_string(number) + ": " + (why.empty() ? "not an object" : why);
            return false;
        }
        if (first)
        {
            first = false;
            if (!ReadHeader(line, header, error))
                return false;
            continue;
        }

        const bool frames = header.timing == ETiming::Frames;
        if (line.Has("dt"))
        {
            double dt = 0.0;
            if (!ParseHexDouble(line["dt"].Str(), dt) || line["f"].Int(-1) != Frames())
            {
                error = "line " + std::to_string(number) + ": a frame out of order or a bad dt";
                return false;
            }
            frameDts.push_back(dt);
        }
        else if (line.Has("check"))
            checks[line["t"].Int()] = line["check"];
        else if (line.Has("mark"))
            continue;       // notes: their check line is what a replay compares
        else if (line["end"].Bool())
            end = line["t"].Int();
        else
        {
            SInputEvent event;
            if (!ReadEventFields(line, event, why))
            {
                error = "line " + std::to_string(number) + ": " + why;
                return false;
            }
            events[line[frames ? "f" : "t"].Int()].push_back(event);
        }
    }
    if (first)
    {
        error = path.string() + " is empty";
        return false;
    }
    return true;
}

const std::vector<SInputEvent>* TRecording::EventsAt(int64_t when) const
{
    const auto it = events.find(when);
    return it == events.end() ? nullptr : &it->second;
}

const JsonValue* TRecording::CheckAt(int64_t t) const
{
    const auto it = checks.find(t);
    return it == checks.end() ? nullptr : &it->second;
}
