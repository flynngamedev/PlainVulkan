// font_data.cpp -- a small, self-contained 3x5 pixel bitmap font.
//
// Tier note (see runtime/README.md): this is a genuinely working text
// renderer, not a stub -- every glyph below actually draws -- but it's a
// simple built-in dot-matrix font, not real typography. Swapping in
// stb_truetype + a real .ttf for production-quality text is a documented
// extension point; this keeps Pv::DrawText usable with zero external
// font assets in the meantime.
//
// Each glyph is authored as 5 rows of 3 characters ('#'/'.') for
// readability and parsed once into a bitmask at first use.
#include "pv/pv_internal.h"

#include <cstring>
#include <unordered_map>

namespace pv {

// clang-format off
static const std::unordered_map<char, Glyph3x5>& glyphTable() {
    static const std::unordered_map<char, Glyph3x5> table = {
        {' ', {{"...", "...", "...", "...", "..."}}},
        {'0', {{"###", "#.#", "#.#", "#.#", "###"}}},
        {'1', {{".#.", "##.", ".#.", ".#.", "###"}}},
        {'2', {{"###", "..#", "###", "#..", "###"}}},
        {'3', {{"###", "..#", "###", "..#", "###"}}},
        {'4', {{"#.#", "#.#", "###", "..#", "..#"}}},
        {'5', {{"###", "#..", "###", "..#", "###"}}},
        {'6', {{"###", "#..", "###", "#.#", "###"}}},
        {'7', {{"###", "..#", "..#", "..#", "..#"}}},
        {'8', {{"###", "#.#", "###", "#.#", "###"}}},
        {'9', {{"###", "#.#", "###", "..#", "###"}}},
        {'A', {{".#.", "#.#", "###", "#.#", "#.#"}}},
        {'B', {{"##.", "#.#", "##.", "#.#", "##."}}},
        {'C', {{"###", "#..", "#..", "#..", "###"}}},
        {'D', {{"##.", "#.#", "#.#", "#.#", "##."}}},
        {'E', {{"###", "#..", "##.", "#..", "###"}}},
        {'F', {{"###", "#..", "##.", "#..", "#.."}}},
        {'G', {{"###", "#..", "#.#", "#.#", "###"}}},
        {'H', {{"#.#", "#.#", "###", "#.#", "#.#"}}},
        {'I', {{"###", ".#.", ".#.", ".#.", "###"}}},
        {'J', {{"..#", "..#", "..#", "#.#", "###"}}},
        {'K', {{"#.#", "#.#", "##.", "#.#", "#.#"}}},
        {'L', {{"#..", "#..", "#..", "#..", "###"}}},
        {'M', {{"#.#", "###", "###", "#.#", "#.#"}}},
        {'N', {{"#.#", "###", "###", "###", "#.#"}}},
        {'O', {{"###", "#.#", "#.#", "#.#", "###"}}},
        {'P', {{"###", "#.#", "###", "#..", "#.."}}},
        {'Q', {{"###", "#.#", "#.#", "###", "..#"}}},
        {'R', {{"###", "#.#", "##.", "#.#", "#.#"}}},
        {'S', {{"###", "#..", "###", "..#", "###"}}},
        {'T', {{"###", ".#.", ".#.", ".#.", ".#."}}},
        {'U', {{"#.#", "#.#", "#.#", "#.#", "###"}}},
        {'V', {{"#.#", "#.#", "#.#", "#.#", ".#."}}},
        {'W', {{"#.#", "#.#", "###", "###", "#.#"}}},
        {'X', {{"#.#", "#.#", ".#.", "#.#", "#.#"}}},
        {'Y', {{"#.#", "#.#", ".#.", ".#.", ".#."}}},
        {'Z', {{"###", "..#", ".#.", "#..", "###"}}},
        {'.', {{"...", "...", "...", "...", ".#."}}},
        {',', {{"...", "...", "...", ".#.", "#.."}}},
        {':', {{"...", ".#.", "...", ".#.", "..."}}},
        {'-', {{"...", "...", "###", "...", "..."}}},
        {'!', {{".#.", ".#.", ".#.", "...", ".#."}}},
        {'?', {{"###", "..#", ".##", "...", ".#."}}},
        {'\'', {{".#.", ".#.", "...", "...", "..."}}},
        {'/', {{"..#", "..#", ".#.", "#..", "#.."}}},
        {'_', {{"...", "...", "...", "...", "###"}}},
        {'+', {{"...", ".#.", "###", ".#.", "..."}}},
    };
    return table;
}
// clang-format on

// 3 wide x 5 tall pixels, converted to uppercase, with a solid block for
// characters outside the table (still visibly distinct text, never blank).
const Glyph3x5& lookupGlyph(char c) {
    char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    auto& table = glyphTable();
    auto it = table.find(upper);
    if (it != table.end()) return it->second;
    static const Glyph3x5 fallback{{"###", "###", "###", "###", "###"}};
    return fallback;
}

} // namespace pv
