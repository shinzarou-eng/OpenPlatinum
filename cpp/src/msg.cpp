// msg.cpp — déchiffrement + charmap.
#include "msg.h"

static const std::unordered_map<int, const char*>& charmap() {
    static std::unordered_map<int, const char*> cm;
    if (!cm.empty()) return cm;
    static char az[26][2], lz[26][2], dg[10][2];
    for (int i = 0; i < 26; i++) {
        az[i][0] = (char)('A' + i); az[i][1] = 0; cm[0x12B + i] = az[i];
        lz[i][0] = (char)('a' + i); lz[i][1] = 0; cm[0x145 + i] = lz[i];
    }
    for (int i = 0; i < 10; i++) {
        dg[i][0] = (char)('0' + i); dg[i][1] = 0; cm[0x121 + i] = dg[i];
    }
    // accents (UTF-8)
    static const char* up[] = {"À","Á","Â","Ã","Ä","Å","Æ","Ç","È","É","Ê","Ë",
        "Ì","Í","Î","Ï","Ð","Ñ","Ò","Ó","Ô","Õ","Ö","×","Ø","Ù","Ú","Û","Ü","Ý","Þ","ß"};
    static const char* lo[] = {"à","á","â","ã","ä","å","æ","ç","è","é","ê","ë",
        "ì","í","î","ï","ð","ñ","ò","ó","ô","õ","ö","÷","ø","ù","ú","û","ü","ý","þ","ÿ"};
    for (int i = 0; i < 32; i++) { cm[0x15F + i] = up[i]; cm[0x17F + i] = lo[i]; }
    static const std::pair<int, const char*> misc[] = {
        {0x0001," "},{0x00A2,"0"},{0x0105,"♪"},{0x011B,"←"},{0x011C,"↑"},
        {0x011D,"↓"},{0x011E,"→"},{0x0120,"&"},{0x019F,"Œ"},{0x1A0,"œ"},
        {0x1A8,"$"},{0x1A9,"¡"},
        {0x1AA,"¿"},{0x1AB,"!"},{0x1AC,"?"},{0x1AD,","},{0x1AE,"."},
        {0x1AF,"…"},{0x1B0,"·"},{0x1B1,"/"},{0x1B2,"'"},{0x1B3,"'"},
        {0x1B4,"“"},{0x1B5,"”"},{0x1B7,"«"},{0x1B8,"»"},{0x1B9,"("},
        {0x1BA,")"},{0x1BB,"♂"},{0x1BC,"♀"},{0x1BD,"+"},{0x1BE,"-"},
        {0x1BF,"*"},{0x1C0,"#"},{0x1C1,"="},{0x1C2,"&"},{0x1C3,"~"},
        {0x1C4,":"},{0x1C5,";"},{0x1C6,"♠"},{0x1C7,"♣"},{0x1C8,"♥"},
        {0x1C9,"♦"},{0x1CA,"★"},{0x1CC,"●"},{0x1CD,"●"},{0x1CE,"■"},
        {0x1CF,"◆"},{0x1D0,"@"},{0x1D1,"♪"},{0x1D2,"%"},{0x1DE," "},
        {0x1E2," "},{0x1E8,"°"},{0x1E9,"_"},{0x1EA,"_"},
    };
    for (auto& p : misc) cm[p.first] = p.second;
    return cm;
}

static void glyph(int c, std::string& out) {
    auto it = charmap().find(c);
    if (it != charmap().end()) { out += it->second; return; }
    if (c > 0 && c <= 0xFFFF) {          // glyphe DS hors table ASCII :
        char32_t u = 0xF0000 + c;        // transporte le code via PUA-A
        out += (char)(0xF0 | (u >> 18));
        out += (char)(0x80 | ((u >> 12) & 0x3F));
        out += (char)(0x80 | ((u >> 6) & 0x3F));
        out += (char)(0x80 | (u & 0x3F));
    }
}

static std::vector<int> unpack9(const std::vector<int>& chars, size_t start) {
    std::vector<int> out;
    uint32_t acc = 0; int nbits = 0;
    for (size_t i = start; i < chars.size(); i++) {
        acc |= chars[i] << nbits;
        nbits += 16;
        while (nbits >= 9) { out.push_back(acc & 0x1FF); acc >>= 9; nbits -= 9; }
    }
    return out;
}

static std::string dec_str(const Bytes& d, size_t off, size_t ln, int idx) {
    uint32_t key = (0x91BD3 * (idx + 1)) & 0xFFFF;
    std::vector<int> chars;
    for (size_t j = 0; j + 1 < ln; j += 2) {
        if (off + j + 1 >= d.size()) break;
        chars.push_back(u16(d, off + j) ^ key);
        key = (key + 0x493D) & 0xFFFF;
    }
    if (!chars.empty() && chars[0] == 0xF100)
        chars = unpack9(chars, 1);
    std::string t;
    for (size_t i = 0; i < chars.size(); i++) {
        int c = chars[i];
        if (c == 0xFFFF || c == 0x1FF || c == 0x0000) break;
        if (c == 0xFFFE || c == 0x1FE) {
            int tag = i + 1 < chars.size() ? chars[i + 1] : 0;
            int cnt = i + 2 < chars.size() ? chars[i + 2] : 0;
            char b[64]; int n = snprintf(b, 64, "{%X", tag);
            for (int k = 0; k < cnt && i + 3 + k < (int)chars.size(); k++)
                n += snprintf(b + n, 64 - n, ":%X", chars[i + 3 + k]);
            snprintf(b + n, 64 - n, "}"); t += b;
            i += 2 + cnt;
            continue;
        }
        if (c == 0xE000) { t += '\n'; continue; }      // saut de ligne
        if (c == 0x25BC || c == 0x25BD) { t += "\n\n"; continue; } // page/scroll
        glyph(c, t);
    }
    return t;
}

std::vector<std::string> msgbank_decode(const Bytes& d) {
    std::vector<std::string> out;
    if (d.size() < 4) return out;
    int num = u16(d, 0), seed = u16(d, 2);
    if (num <= 0 || num > 8000 || 4 + num * 8 > (int)d.size()) return out;
    for (int i = 1; i <= num; i++) {
        uint32_t key = ((uint32_t)seed * i * 0x2FD & 0xFFFF) * 0x10001;
        uint32_t o = u32(d, 4 + (i - 1) * 8) ^ key;
        uint32_t l = u32(d, 4 + (i - 1) * 8 + 4) ^ key;   // mots u16, term. inclus
        out.push_back(dec_str(d, o, std::min(l * 2, 8192u), i - 1));
    }
    return out;
}

// ── charmap inverse : caractère UTF-8 -> code GF (-> glyphe code-1) ──────────
int msg_next_utf8(const std::string& s, size_t& i) {
    if (i >= s.size()) return -1;
    uint8_t b = (uint8_t)s[i];
    int cp; int n;
    if (b < 0x80) { cp = b; n = 1; }
    else if ((b >> 5) == 0x6) { cp = b & 0x1F; n = 2; }
    else if ((b >> 4) == 0xE) { cp = b & 0x0F; n = 3; }
    else { cp = b & 7; n = 4; }
    for (int k = 1; k < n && i + k < s.size(); k++)
        cp = (cp << 6) | (s[i + k] & 0x3F);
    i += n;
    return cp;
}

int msg_char_code_of(char32_t ch) {
    if (ch >= 0xF0000 && ch <= 0xF0FFFF)         // glyphe DS transporté via PUA
        return (int)(ch - 0xF0000);
    static std::unordered_map<char32_t, int> rev;
    if (rev.empty())
        for (auto& kv : charmap()) {           // garde le 1er code par car
            char tmp[8] = {};
            size_t i = 0;
            int cp = msg_next_utf8(std::string(kv.second), i);
            // kv.second est UTF-8 : décoder le 1er codepoint
            const std::string& u = kv.second;
            uint8_t b = (uint8_t)u[0]; int c;
            if (b < 0x80) c = b;
            else if ((b >> 5) == 6) c = ((b & 0x1F) << 6) | (u[1] & 0x3F);
            else if ((b >> 4) == 0xE) c = ((b & 15) << 12) | ((u[1] & 63) << 6) | (u[2] & 63);
            else c = ((b & 7) << 18) | ((u[1] & 63) << 12) | ((u[2] & 63) << 6) | (u[3] & 63);
            (void)cp;
            if (!rev.count((char32_t)c)) rev[(char32_t)c] = kv.first;
        }
    auto it = rev.find(ch);
    return it != rev.end() ? it->second : -1;
}
