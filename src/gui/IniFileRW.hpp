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

#ifndef __INIFILERW_HPP_INCLUDED__
#define __INIFILERW_HPP_INCLUDED__

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace bc { namespace ini {

// A single line entry in the INI file, preserving order and structure
struct IniEntry {
    std::string section;     // Current section this entry belongs to (empty if before first section)
    std::string key;         // Key (lowercased for lookup; empty for comment/blank/section lines)
    std::string value;       // Value (quotes stripped)
    std::string description; // From matching _DESC key, if any
    std::string rawLine;     // Original line text (for comments, blanks, section headers)
    bool isSectionHeader = false;
    bool isComment = false;  // Non-key, non-section, non-blank line (e.g. preamble text)
    bool isBlank = false;

    // Options from _OPTION keys
    std::vector<std::string> options;
};

class IniFile {
public:
    IniFile() = default;

    // Load from file. Returns true on success.
    bool load(const std::string& filePath);

    // Save to file. Preserves original structure (comments, blanks, sections).
    // If outputPath is empty, saves to the path used in load().
    bool save(const std::string& outputPath = "") const;

    // Get the file path this was loaded from
    const std::string& filePath() const { return filePath_; }

    // String access (key is case-insensitive)
    std::string getString(const std::string& key, const std::string& defValue = "") const;
    void setString(const std::string& key, const std::string& value);

    // Numeric access
    uint32_t getUInt(const std::string& key, uint32_t defValue = 0) const;
    void setUInt(const std::string& key, uint32_t value);

    int32_t getSInt(const std::string& key, int32_t defValue = 0) const;
    void setSInt(const std::string& key, int32_t value);

    float getFloat(const std::string& key, float defValue = 0.f) const;
    void setFloat(const std::string& key, float value);

    // Description for a key (from _DESC entries)
    std::string getDescription(const std::string& key) const;

    // Options for a key (from _OPTION entries)
    std::vector<std::string> getOptions(const std::string& key) const;

    // Get all section names in order
    std::vector<std::string> sections() const;

    // Get all keys in a section (in file order)
    std::vector<std::string> keysInSection(const std::string& section) const;

    // Check if a key exists
    bool hasKey(const std::string& key) const;

private:
    std::string filePath_;
    std::vector<IniEntry> entries_;
    // Lookup: lowercased key -> index in entries_
    std::map<std::string, size_t> keyIndex_;
    // Descriptions: lowercased base key -> description string
    std::map<std::string, std::string> descriptions_;
    // Options: lowercased base key -> list of option values
    std::map<std::string, std::vector<std::string>> optionsMap_;

    static std::string toLower(const std::string& s);
    static std::string trimWhitespace(const std::string& s);
    static std::string trimQuotes(const std::string& s);

    // Find or add entry for key, returns index
    size_t findOrAddKey(const std::string& key);
};

}} // namespace bc::ini

#endif
