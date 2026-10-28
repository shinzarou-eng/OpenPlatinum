// msg.h — banques de textes Gen4 (pl_msg.narc) : table XORée par seed*i*0x2FD,
// strings XORées clé roulante 0x91BD3*(i+1) +0x493D/car. Charset Nitro FR.
// Port de scripts/msgbank.gd — sortie UTF-8.
#pragma once
#include "common.h"

std::vector<std::string> msgbank_decode(const Bytes& d);

// code car GF -> indice glyphe NFGR (glyphe = code-1)
int msg_char_code_of(char32_t ch);
// itère les caractères UTF-8 d'une chaîne (retourne le code GF ou -1)
int msg_next_utf8(const std::string& s, size_t& i);
