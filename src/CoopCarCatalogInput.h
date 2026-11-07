// Copyright (C) 2026 batuhan.
// This file is part of Undercoop: NFSU2.
// Licensed under GNU GPL version 3 only. See COPYING and LICENSE.md.
// Additional 7-Zip permission is in LICENSE-7ZIP-EXCEPTION.txt.

#pragma once
#include "CoopCarCatalog.h"
#include <vector>

// Portable file adapter. Binary may save native chunks into a .LZC filename;
// original game files instead wrap those chunks in a single JDLZ stream.
namespace coop_car_catalog_input {
enum class Error { None, FileSize, Header, Length, Truncated, BackReference,
    OutputBounds, TrailingData, Catalog };

inline bool DecodeJdlz(const void* input, std::size_t size,
    std::vector<std::uint8_t>& output, Error& error) {
    output.clear(); error = Error::None;
    if (!input || size > coop_car_catalog::kMaxFileBytes) {
        error = Error::FileSize; return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(input);
    if (size < 16 || coop_car_catalog::U32(bytes) != 0x5A4C444Au ||
        bytes[4] != 2 || bytes[5] != 16 || bytes[6] || bytes[7]) {
        error = Error::Header; return false;
    }
    const std::size_t decodedSize = coop_car_catalog::U32(bytes + 8);
    if (decodedSize < 8 || decodedSize > coop_car_catalog::kMaxFileBytes ||
        coop_car_catalog::U32(bytes + 12) != size) {
        error = Error::Length; return false;
    }
    std::vector<std::uint8_t> decoded(decodedSize);
    std::size_t in = 16, out = 0;
    unsigned flags1 = 1, flags2 = 1;
    // JDLZ's two flag streams and near/far match layout are documented by
    // MWisBest/OpenNFSTools, LibNFS/Compression/JDLZ.cs (decompress).
    // https://github.com/MWisBest/OpenNFSTools/blob/master/LibNFS/Compression/JDLZ.cs
    while (out < decodedSize) {
        if (flags1 == 1) {
            if (in == size) { error = Error::Truncated; return false; }
            flags1 = bytes[in++] | 0x100u;
        }
        if (flags2 == 1) {
            if (in == size) { error = Error::Truncated; return false; }
            flags2 = bytes[in++] | 0x100u;
        }
        if (flags1 & 1u) {
            if (size - in < 2) { error = Error::Truncated; return false; }
            const auto a = bytes[in], b = bytes[in + 1];
            const std::size_t length = (flags2 & 1u)
                ? (std::size_t(b) | (std::size_t(a & 0xF0u) << 4)) + 3
                : std::size_t(a & 0x1Fu) + 3;
            const std::size_t distance = (flags2 & 1u)
                ? std::size_t(a & 0x0Fu) + 1
                : (std::size_t(b) | (std::size_t(a & 0xE0u) << 3)) + 17;
            in += 2; flags2 >>= 1;
            if (distance > out) { error = Error::BackReference; return false; }
            if (length > decodedSize - out) { error = Error::OutputBounds; return false; }
            // Forward copy intentionally permits overlapping matches.
            for (std::size_t i = 0; i < length; ++i)
                decoded[out + i] = decoded[out + i - distance];
            out += length;
        } else {
            if (in == size) { error = Error::Truncated; return false; }
            decoded[out++] = bytes[in++];
        }
        flags1 >>= 1;
    }
    // Retail GLOBALB.LZC includes one terminal zero byte after the complete
    // stream. Accept that padding only; the header must still cover it exactly.
    if (in != size && (size - in != 1 || bytes[in] != 0)) {
        error = Error::TrailingData; return false;
    }
    output.swap(decoded); return true;
}

inline bool Parse(const void* input, std::size_t size,
    coop_car_catalog::Catalog& output, Error& error,
    coop_car_catalog::Error& catalogError) {
    output = {}; error = Error::None; catalogError = coop_car_catalog::Error::None;
    if (!input || size < 8 || size > coop_car_catalog::kMaxFileBytes) {
        error = Error::FileSize; return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(input);
    if (coop_car_catalog::U32(bytes) == 0x5A4C444Au) {
        std::vector<std::uint8_t> decoded;
        if (!DecodeJdlz(input, size, decoded, error)) return false;
        if (coop_car_catalog::Parse(decoded.data(), decoded.size(), output, catalogError)) return true;
    } else if (coop_car_catalog::Parse(input, size, output, catalogError)) return true;
    error = Error::Catalog; return false;
}

inline const char* ErrorText(Error error, coop_car_catalog::Error catalogError) {
    switch (error) {
    case Error::None: return "ready";
    case Error::FileSize: return "car catalog file size is unsupported";
    case Error::Header: return "car catalog JDLZ header is unsupported";
    case Error::Length: return "car catalog JDLZ length is invalid";
    case Error::Truncated: return "car catalog JDLZ stream is truncated";
    case Error::BackReference: return "car catalog JDLZ reference is invalid";
    case Error::OutputBounds: return "car catalog JDLZ match exceeds its output";
    case Error::TrailingData: return "car catalog JDLZ stream has trailing data";
    case Error::Catalog: return coop_car_catalog::ErrorText(catalogError);
    }
    return "car catalog input is unsupported";
}
}
