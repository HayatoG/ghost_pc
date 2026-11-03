#include "ps2_virtual_pad.h"
#include "Kernel/Stubs/Pad.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace ps2_virtual_pad
{
    namespace
    {
        struct ScriptState
        {
            std::mutex mutex;
            std::vector<Step> steps;
            size_t next = 0;
            bool loaded = false;
        };

        ScriptState &state()
        {
            static ScriptState s;
            return s;
        }

        std::string upper(std::string_view text)
        {
            std::string out(text);
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c)
                           { return static_cast<char>(std::toupper(c)); });
            return out;
        }

        bool parseButtons(const std::string &token, uint16_t &buttons)
        {
            static const std::unordered_map<std::string, uint16_t> kNames = {
                {"SELECT", ps2_stubs::pad_button::Select}, {"L3", ps2_stubs::pad_button::L3},
                {"R3", ps2_stubs::pad_button::R3}, {"START", ps2_stubs::pad_button::Start},
                {"UP", ps2_stubs::pad_button::Up}, {"RIGHT", ps2_stubs::pad_button::Right},
                {"DOWN", ps2_stubs::pad_button::Down}, {"LEFT", ps2_stubs::pad_button::Left},
                {"L2", ps2_stubs::pad_button::L2}, {"R2", ps2_stubs::pad_button::R2},
                {"L1", ps2_stubs::pad_button::L1}, {"R1", ps2_stubs::pad_button::R1},
                {"TRIANGLE", ps2_stubs::pad_button::Triangle}, {"CIRCLE", ps2_stubs::pad_button::Circle},
                {"CROSS", ps2_stubs::pad_button::Cross}, {"SQUARE", ps2_stubs::pad_button::Square}};

            buttons = 0xFFFFu;
            if (token == "-")
            {
                return true;
            }

            std::istringstream names(upper(token));
            std::string name;
            while (std::getline(names, name, '+'))
            {
                const auto it = kNames.find(name);
                if (it == kNames.end())
                {
                    return false;
                }
                buttons = static_cast<uint16_t>(buttons & ~it->second);
            }
            return true;
        }

        bool parseAnalog(const std::string &token, uint8_t &value)
        {
            int raw = 0;
            std::istringstream in(token);
            if (!(in >> raw) || !in.eof() || raw < 0 || raw > 255)
            {
                return false;
            }
            value = static_cast<uint8_t>(raw);
            return true;
        }
    }

    bool parseScript(std::istream &in, std::vector<Step> &steps, std::string &error)
    {
        steps.clear();
        std::string text;
        int lineNo = 0;
        while (std::getline(in, text))
        {
            ++lineNo;
            const size_t comment = text.find('#');
            if (comment != std::string::npos)
            {
                text.erase(comment);
            }

            std::istringstream line(text);
            Step step{};
            std::string action;
            if (!(line >> step.frame))
            {
                if (text.find_first_not_of(" \t\r") == std::string::npos)
                {
                    continue; // blank line
                }
                error = "line " + std::to_string(lineNo) + ": expected a frame number";
                return false;
            }
            if (!steps.empty() && step.frame < steps.back().frame)
            {
                error = "line " + std::to_string(lineNo) + ": frames must be non-decreasing";
                return false;
            }
            if (!(line >> action))
            {
                error = "line " + std::to_string(lineNo) + ": missing buttons or 'quit'";
                return false;
            }

            if (upper(action) == "QUIT")
            {
                step.quit = true;
            }
            else
            {
                if (!parseButtons(action, step.buttons))
                {
                    error = "line " + std::to_string(lineNo) + ": unknown button in '" + action + "'";
                    return false;
                }
                std::vector<std::string> analog;
                for (std::string token; line >> token;)
                {
                    analog.push_back(token);
                }
                if (!analog.empty())
                {
                    uint8_t *targets[] = {&step.lx, &step.ly, &step.rx, &step.ry};
                    if (analog.size() != 4)
                    {
                        error = "line " + std::to_string(lineNo) + ": expected 4 analog values 0..255";
                        return false;
                    }
                    for (size_t i = 0; i < analog.size(); ++i)
                    {
                        if (!parseAnalog(analog[i], *targets[i]))
                        {
                            error = "line " + std::to_string(lineNo) + ": invalid analog value '" + analog[i] + "'";
                            return false;
                        }
                    }
                }
            }
            steps.push_back(step);
        }
        return true;
    }

    bool loadScript(const std::string &path, std::string &error)
    {
        std::ifstream file(path);
        if (!file)
        {
            error = "cannot open " + path;
            return false;
        }

        std::vector<Step> steps;
        if (!parseScript(file, steps, error))
        {
            return false;
        }

        ScriptState &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.steps = std::move(steps);
        s.next = 0;
        s.loaded = true;
        return true;
    }

    bool isLoaded()
    {
        ScriptState &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.loaded;
    }

    bool onFrame(uint64_t frame)
    {
        ScriptState &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        bool quit = false;
        while (s.next < s.steps.size() && s.steps[s.next].frame <= frame)
        {
            const Step &step = s.steps[s.next++];
            if (step.quit)
            {
                quit = true;
                continue;
            }
            ps2_stubs::setPadOverrideState(step.buttons, step.lx, step.ly, step.rx, step.ry);
        }
        return quit;
    }
}
