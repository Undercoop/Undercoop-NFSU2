// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include <array>
#include <cstddef>
#include <cstring>
namespace coop_lobby_text {
using Lines=std::array<std::array<char,32>,8>;
// The native lobby name column is narrow; use several owned rows instead of
// asking one FEng label to clip a long status at the right edge.
inline Lines Wrap(const char* text,std::size_t width=22) {
    Lines result{};if(!text||!width||width>31)return result;
    std::size_t line=0,used=0;
    while(*text&&line<result.size()) {
        while(*text==' ')++text;if(!*text)break;
        std::size_t word=0;while(text[word]&&text[word]!=' ')++word;
        if(used&&used+1+word>width){++line;used=0;if(line==result.size())break;}
        if(used)result[line][used++]=' ';
        while(word&&line<result.size()) {
            const auto room=width-used,take=word<room?word:room;
            std::memcpy(result[line].data()+used,text,take);used+=take;text+=take;word-=take;
            if(word){++line;used=0;}
        }
    }
    return result;
}
}
