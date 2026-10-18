// Minimal ZIP reader (stored / deflate), used for .mxl and Guitar Pro 7 .gp files.
#include <zlib.h>

#include <algorithm>

#include "song/SongInternal.h"

namespace gs {
namespace detail {

bool isZip(const std::string& data) { return data.size() >= 4 && data.compare(0, 4, "PK\x03\x04", 4) == 0; }

bool zipList(const std::string& data, std::vector<ZipEntry>& out, std::string& err) {
    out.clear();
    const size_t n = data.size();
    if (n < 22) { err = tr("ZIP file too small"); return false; }
    // search for the End Of Central Directory record from the end
    size_t eocd = std::string::npos;
    size_t minPos = n > 22 + 65535 ? n - 22 - 65535 : 0;
    for (size_t p = n - 22 + 1; p-- > minPos;) {
        if ((unsigned char)data[p] == 0x50 && data.compare(p, 4, "PK\x05\x06", 4) == 0) { eocd = p; break; }
    }
    if (eocd == std::string::npos) { err = tr("Not a valid ZIP (central directory not found)"); return false; }
    ByteReader r(data.data(), n);
    r.seek(eocd + 10);
    uint16_t total = r.u16le();
    uint32_t cdSize = r.u32le();
    uint32_t cdOff = r.u32le();
    if (!r.ok() || cdOff == 0xFFFFFFFFu || (size_t)cdOff + cdSize > n) { err = tr("ZIP directory corrupt or ZIP64 (unsupported)"); return false; }
    r.seek(cdOff);
    for (unsigned k = 0; k < total && r.ok(); ++k) {
        if (r.u32le() != 0x02014b50u) { err = tr("Corrupt ZIP directory entry"); return false; }
        r.skip(4);  // version made by / needed
        uint16_t flags = r.u16le();
        ZipEntry e;
        e.method = r.u16le();
        r.skip(8);  // time, date, crc
        e.compSize = r.u32le();
        e.uncompSize = r.u32le();
        uint16_t nameLen = r.u16le(), extraLen = r.u16le(), commentLen = r.u16le();
        r.skip(8);  // disk, int attr, ext attr
        e.localOffset = r.u32le();
        e.name = r.bytes(nameLen);
        r.skip((size_t)extraLen + commentLen);
        if (!r.ok()) break;
        if (flags & 1) continue;  // encrypted, skip
        out.push_back(std::move(e));
    }
    if (!r.ok()) { err = tr("ZIP directory read out of bounds"); return false; }
    return true;
}

bool zipExtract(const std::string& data, const ZipEntry& e, std::string& out, std::string& err, size_t maxOut) {
    out.clear();
    ByteReader r(data.data(), data.size());
    r.seek(e.localOffset);
    if (r.u32le() != 0x04034b50u) { err = tr("Corrupt ZIP local header"); return false; }
    r.skip(22);
    uint16_t nameLen = r.u16le(), extraLen = r.u16le();
    r.skip((size_t)nameLen + extraLen);
    if (!r.ok() || !r.has(e.compSize)) { err = tr("ZIP data out of bounds"); return false; }
    const unsigned char* src = r.ptr();
    if (e.method == 0) {
        if (e.compSize > maxOut) { err = tr("ZIP entry too large"); return false; }
        out.assign(reinterpret_cast<const char*>(src), e.compSize);
        return true;
    }
    if (e.method != 8) { err = tr("Unsupported ZIP compression method"); return false; }
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) { err = tr("zlib initialization failed"); return false; }
    zs.next_in = const_cast<unsigned char*>(src);
    zs.avail_in = e.compSize;
    size_t cap = std::min<size_t>(maxOut, e.uncompSize > 0 ? (size_t)e.uncompSize + 16 : 1 << 16);
    if (cap == 0) cap = 1;
    out.resize(cap);
    size_t produced = 0;
    int ret = Z_OK;
    while (true) {
        if (produced == out.size()) {
            if (out.size() >= maxOut) { inflateEnd(&zs); out.clear(); err = tr("Decompressed data too large"); return false; }
            out.resize(std::min(maxOut, out.size() * 2));
        }
        zs.next_out = reinterpret_cast<unsigned char*>(&out[produced]);
        zs.avail_out = (uInt)std::min<size_t>(out.size() - produced, 1u << 30);
        uInt before = zs.avail_out;
        ret = inflate(&zs, Z_NO_FLUSH);
        produced += before - zs.avail_out;
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK && ret != Z_BUF_ERROR) break;
        if (ret == Z_BUF_ERROR && zs.avail_in == 0) break;  // input exhausted before the end of the stream
    }
    inflateEnd(&zs);
    if (ret != Z_STREAM_END) { out.clear(); err = tr("ZIP decompression failed"); return false; }
    out.resize(produced);
    return true;
}

bool zipExtractByName(const std::string& data, const std::string& name, std::string& out, std::string& err) {
    std::vector<ZipEntry> es;
    if (!zipList(data, es, err)) return false;
    for (const ZipEntry& e : es)
        if (e.name == name) return zipExtract(data, e, out, err);
    err = tr("Not found in ZIP: ") + name;
    return false;
}

}  // namespace detail
}  // namespace gs
