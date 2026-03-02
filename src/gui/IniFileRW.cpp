/*   Bridge Command 5.0 Ship Simulator
     Copyright (C) 2024 James Packer

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License version 2 as
     published by the Free Software Foundation

     This program is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY Or FITNESS For A PARTICULAR PURPOSE.  See the
     GNU General Public License For more details.

     You should have received a copy of the GNU General Public License along
     with this program; if not, write to the Free Software Foundation, Inc.,
     51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "IniFileRW.hpp"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <cstring>

namespace bc { namespace ini {

std::string IniFile::toLower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return r;
}

std::string IniFile::trimWhitespace(const std::string& s) {
    const char* ws = " \f\n\r\t\v";
    size_t start = s.find_first_not_of(ws);
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(ws);
    return s.substr(start, end - start + 1);
}

std::string IniFile::trimQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

bool IniFile::load(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "IniFileRW: Unable to open file " << filePath << std::endl;
        return false;
    }

    filePath_ = filePath;
    entries_.clear();
    keyIndex_.clear();
    descriptions_.clear();
    optionsMap_.clear();

    std::string currentSection;
    std::string line;

    while (std::getline(file, line)) {
        // Remove trailing \r if present (Windows line endings)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::string trimmed = trimWhitespace(line);

        // Blank line
        if (trimmed.empty()) {
            IniEntry entry;
            entry.section = currentSection;
            entry.rawLine = line;
            entry.isBlank = true;
            entries_.push_back(entry);
            continue;
        }

        // Section header [SectionName]
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            currentSection = trimmed.substr(1, trimmed.size() - 2);
            IniEntry entry;
            entry.section = currentSection;
            entry.rawLine = line;
            entry.isSectionHeader = true;
            entries_.push_back(entry);
            continue;
        }

        // Key=value line
        size_t eqPos = line.find('=');
        if (eqPos != std::string::npos) {
            std::string rawKey = trimWhitespace(line.substr(0, eqPos));
            std::string rawValue = trimWhitespace(line.substr(eqPos + 1));
            std::string keyLow = toLower(rawKey);
            std::string value = trimQuotes(rawValue);

            // Check for _DESC suffix
            const std::string descSuffix = "_desc";
            if (keyLow.size() > descSuffix.size() &&
                keyLow.substr(keyLow.size() - descSuffix.size()) == descSuffix)
            {
                std::string baseKey = keyLow.substr(0, keyLow.size() - descSuffix.size());
                descriptions_[baseKey] = value;

                // Store as raw line (preserves on save)
                IniEntry entry;
                entry.section = currentSection;
                entry.rawLine = line;
                entry.isComment = true; // Treated as metadata, not a user-visible key
                entries_.push_back(entry);
                continue;
            }

            // Check for _OPTION suffix
            const std::string optSuffix = "_option";
            if (keyLow.size() > optSuffix.size() &&
                keyLow.substr(keyLow.size() - optSuffix.size()) == optSuffix)
            {
                std::string baseKey = keyLow.substr(0, keyLow.size() - optSuffix.size());
                optionsMap_[baseKey].push_back(value);

                IniEntry entry;
                entry.section = currentSection;
                entry.rawLine = line;
                entry.isComment = true;
                entries_.push_back(entry);
                continue;
            }

            // Normal key=value
            IniEntry entry;
            entry.section = currentSection;
            entry.key = keyLow;
            entry.value = value;
            entry.rawLine = line;
            entries_.push_back(entry);
            keyIndex_[keyLow] = entries_.size() - 1;
        } else {
            // Non-key line (comment/preamble)
            IniEntry entry;
            entry.section = currentSection;
            entry.rawLine = line;
            entry.isComment = true;
            entries_.push_back(entry);
        }
    }

    file.close();
    return true;
}

bool IniFile::save(const std::string& outputPath) const {
    std::string path = outputPath.empty() ? filePath_ : outputPath;
    if (path.empty()) return false;

    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "IniFileRW: Unable to write file " << path << std::endl;
        return false;
    }

    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& entry = entries_[i];

        if (entry.isBlank) {
            file << "\n";
        } else if (entry.isSectionHeader || entry.isComment) {
            file << entry.rawLine << "\n";
        } else if (!entry.key.empty()) {
            // Reconstruct key=value, preserving original key casing from rawLine if possible
            std::string originalKey;
            size_t eqPos = entry.rawLine.find('=');
            if (eqPos != std::string::npos) {
                originalKey = trimWhitespace(entry.rawLine.substr(0, eqPos));
            } else {
                originalKey = entry.key;
            }
            file << originalKey << "=" << entry.value << "\n";
        }
    }

    file.close();
    return true;
}

std::string IniFile::getString(const std::string& key, const std::string& defValue) const {
    std::string keyLow = toLower(key);
    auto it = keyIndex_.find(keyLow);
    if (it == keyIndex_.end()) return defValue;
    return entries_[it->second].value;
}

void IniFile::setString(const std::string& key, const std::string& value) {
    size_t idx = findOrAddKey(key);
    entries_[idx].value = value;
}

uint32_t IniFile::getUInt(const std::string& key, uint32_t defValue) const {
    std::string s = getString(key, "");
    if (s.empty()) return defValue;
    try {
        size_t pos = 0;
        unsigned long val = std::stoul(s, &pos);
        if (pos == s.size()) return static_cast<uint32_t>(val);
        return defValue;
    } catch (...) {
        return defValue;
    }
}

void IniFile::setUInt(const std::string& key, uint32_t value) {
    setString(key, std::to_string(value));
}

int32_t IniFile::getSInt(const std::string& key, int32_t defValue) const {
    std::string s = getString(key, "");
    if (s.empty()) return defValue;
    try {
        size_t pos = 0;
        long val = std::stol(s, &pos);
        if (pos == s.size()) return static_cast<int32_t>(val);
        return defValue;
    } catch (...) {
        return defValue;
    }
}

void IniFile::setSInt(const std::string& key, int32_t value) {
    setString(key, std::to_string(value));
}

float IniFile::getFloat(const std::string& key, float defValue) const {
    std::string s = getString(key, "");
    if (s.empty()) return defValue;
    try {
        size_t pos = 0;
        float val = std::stof(s, &pos);
        if (pos == s.size()) return val;
        return defValue;
    } catch (...) {
        return defValue;
    }
}

void IniFile::setFloat(const std::string& key, float value) {
    // Use enough precision to round-trip, but trim trailing zeros
    std::ostringstream oss;
    oss << value;
    std::string s = oss.str();
    setString(key, s);
}

std::string IniFile::getDescription(const std::string& key) const {
    std::string keyLow = toLower(key);
    auto it = descriptions_.find(keyLow);
    if (it == descriptions_.end()) return "";
    return it->second;
}

std::vector<std::string> IniFile::getOptions(const std::string& key) const {
    std::string keyLow = toLower(key);
    auto it = optionsMap_.find(keyLow);
    if (it == optionsMap_.end()) return {};
    return it->second;
}

std::vector<std::string> IniFile::sections() const {
    std::vector<std::string> result;
    for (const auto& entry : entries_) {
        if (entry.isSectionHeader) {
            result.push_back(entry.section);
        }
    }
    return result;
}

std::vector<std::string> IniFile::keysInSection(const std::string& section) const {
    std::string sectionLow = toLower(section);
    std::vector<std::string> result;
    for (const auto& entry : entries_) {
        if (!entry.key.empty() && toLower(entry.section) == sectionLow) {
            result.push_back(entry.key);
        }
    }
    return result;
}

bool IniFile::hasKey(const std::string& key) const {
    return keyIndex_.find(toLower(key)) != keyIndex_.end();
}

size_t IniFile::findOrAddKey(const std::string& key) {
    std::string keyLow = toLower(key);
    auto it = keyIndex_.find(keyLow);
    if (it != keyIndex_.end()) {
        return it->second;
    }
    // Add new entry at end
    IniEntry entry;
    entry.key = keyLow;
    entry.rawLine = key + "=";
    if (!entries_.empty()) {
        // Use the last section
        for (auto rit = entries_.rbegin(); rit != entries_.rend(); ++rit) {
            if (rit->isSectionHeader) {
                entry.section = rit->section;
                break;
            }
        }
    }
    entries_.push_back(entry);
    size_t idx = entries_.size() - 1;
    keyIndex_[keyLow] = idx;
    return idx;
}

}} // namespace bc::ini
