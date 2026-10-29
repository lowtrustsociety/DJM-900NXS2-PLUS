/* DJM-900NXS2 PLUS patcher: the engine of the web page (and of its Node test).
 * SPDX-License-Identifier: GPL-3.0-only */
(function (root) {
    "use strict";

    var KEY = [0xaa, 0xb1, 0xab, 0xac, 0xb5, 0xb5, 0xa8, 0xb6, 0xb0, 0xb6,
               0xb0, 0xad, 0xa6, 0xab, 0xaa, 0xac, 0xb5, 0xbc, 0xa6, 0xff];

    function PatchError(message) {
        this.name = "PatchError";
        this.message = message;
    }
    PatchError.prototype = Object.create(Error.prototype);

    var NOT_OFFICIAL = "this is not the official DJM-900NXS2 firmware version 2.08 UPD file. " +
        "please provide that, unchanged. any other file is refused";

    /* SHA-256, so the page also works where the browser's own is not offered (a local file). */
    var K = new Uint32Array([
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]);

    function sha256Plain(data) {
        var h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
        var w = new Uint32Array(64);
        var length = data.length;
        var padded = new Uint8Array(((length + 9 + 63) >> 6) << 6);
        padded.set(data);
        padded[length] = 0x80;
        var bits = length * 8;
        var view = new DataView(padded.buffer);
        view.setUint32(padded.length - 8, Math.floor(bits / 0x100000000));
        view.setUint32(padded.length - 4, bits >>> 0);
        for (var at = 0; at < padded.length; at += 64) {
            for (var i = 0; i < 16; i++) w[i] = view.getUint32(at + 4 * i);
            for (i = 16; i < 64; i++) {
                var a15 = w[i - 15], a2 = w[i - 2];
                var s0 = ((a15 >>> 7) | (a15 << 25)) ^ ((a15 >>> 18) | (a15 << 14)) ^ (a15 >>> 3);
                var s1 = ((a2 >>> 17) | (a2 << 15)) ^ ((a2 >>> 19) | (a2 << 13)) ^ (a2 >>> 10);
                w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
            }
            var a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
            for (i = 0; i < 64; i++) {
                var S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
                var t1 = (k + S1 + ((e & f) ^ (~e & g)) + K[i] + w[i]) >>> 0;
                var S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
                var t2 = (S0 + ((a & b) ^ (a & c) ^ (b & c))) >>> 0;
                k = g; g = f; f = e; e = (d + t1) >>> 0; d = c; c = b; b = a; a = (t1 + t2) >>> 0;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
        }
        var out = "";
        for (i = 0; i < 8; i++) out += ("0000000" + h[i].toString(16)).slice(-8);
        return out;
    }

    function sha256(data) {
        var subtle = root.crypto && root.crypto.subtle;
        if (!subtle) return Promise.resolve(sha256Plain(data));
        return subtle.digest("SHA-256", data).then(function (digest) {
            return Array.prototype.map.call(new Uint8Array(digest), function (v) {
                return ("0" + v.toString(16)).slice(-2);
            }).join("");
        }, function () {
            return sha256Plain(data);
        });
    }

    var TABLE = (function () {
        var table = new Uint16Array(256);
        for (var i = 0; i < 256; i++) {
            var c = i << 8;
            for (var j = 0; j < 8; j++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff;
            table[i] = c;
        }
        return table;
    })();

    function crc16(data, start, end) {
        var crc = 0;
        for (var i = start; i < end; i++) crc = ((crc << 8) & 0xffff) ^ TABLE[(crc >> 8) ^ data[i]];
        return crc;
    }

    function xor(data) {
        var out = new Uint8Array(data.length);
        for (var i = 0; i < data.length; i++) out[i] = data[i] ^ KEY[i % 20] ^ 0xff;
        return out;
    }

    function u16(d, at) { return d[at] | (d[at + 1] << 8); }
    function u32(d, at) { return (d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) | (d[at + 3] << 24)) >>> 0; }

    function fromBase64(text) {
        var binary = root.atob ? root.atob(text) : Buffer.from(text, "base64").toString("binary");
        var out = new Uint8Array(binary.length);
        for (var i = 0; i < binary.length; i++) out[i] = binary.charCodeAt(i);
        return out;
    }

    function isZip(d) { return d.length > 22 && d[0] === 0x50 && d[1] === 0x4b && d[2] === 0x03 && d[3] === 0x04; }

    /* Pioneer's download is a zip with the one update file in it: take that file out. */
    function fromZip(d) {
        var end = -1;
        for (var at = d.length - 22; at >= Math.max(0, d.length - 65558); at--) {
            if (u32(d, at) === 0x06054b50) { end = at; break; }
        }
        if (end < 0) return Promise.resolve(d);
        var count = u16(d, end + 10), entry = u32(d, end + 16), found = [];
        for (var n = 0; n < count && u32(d, entry) === 0x02014b50; n++) {
            var nameLength = u16(d, entry + 28);
            var name = String.fromCharCode.apply(null, d.subarray(entry + 46, entry + 46 + nameLength));
            if (/\.upd$/i.test(name)) {
                found.push({ method: u16(d, entry + 10), size: u32(d, entry + 20), local: u32(d, entry + 42) });
            }
            entry += 46 + nameLength + u16(d, entry + 30) + u16(d, entry + 32);
        }
        if (found.length !== 1) return Promise.resolve(d);
        var f = found[0];
        var start = f.local + 30 + u16(d, f.local + 26) + u16(d, f.local + 28);
        var body = d.subarray(start, start + f.size);
        if (f.method === 0) return Promise.resolve(body.slice());
        if (f.method !== 8 || typeof root.DecompressionStream === "undefined") return Promise.resolve(d);
        var stream = new Blob([body]).stream().pipeThrough(new root.DecompressionStream("deflate-raw"));
        return new Response(stream).arrayBuffer().then(function (buffer) {
            return new Uint8Array(buffer);
        }, function () {
            return d;
        });
    }

    function readOfficialFile(d) {
        return isZip(d) ? fromZip(d) : Promise.resolve(d);
    }

    /* The segments of the unmasked file: where each one's data begins, and its length. */
    function segments(plain) {
        var list = {}, at = 0x24;
        while (at + 38 <= plain.length) {
            var name = String.fromCharCode.apply(null, plain.subarray(at + 10, at + 16)).trim();
            var length = u32(plain, at + 32);
            list[name] = { start: at + 36, length: length };
            at += 38 + length;
        }
        if (at !== plain.length) throw new PatchError(NOT_OFFICIAL);
        return list;
    }

    async function buildMod(stock, data) {
        if (await sha256(stock) !== data.stock_sha256) throw new PatchError(NOT_OFFICIAL);
        var plain = xor(stock);
        var list = segments(plain);
        Object.keys(data.changes).forEach(function (name) {
            var segment = list[name];
            data.changes[name].forEach(function (change) {
                var bytes = fromBase64(change[1]);
                if (change[0] + bytes.length > segment.length) throw new PatchError(NOT_OFFICIAL);
                plain.set(bytes, segment.start + change[0]);
            });
            var crc = crc16(plain, segment.start, segment.start + segment.length);
            plain[segment.start + segment.length] = crc & 0xff;
            plain[segment.start + segment.length + 1] = crc >> 8;
        });
        for (var i = 0; i < 3; i++) plain[20 + i] = data.tag.charCodeAt(i);
        var mod = xor(plain);
        if (await sha256(mod) !== data.mod_sha256) {
            throw new PatchError("the result is not the tested build");
        }
        return mod;
    }

    /* The version in a DJM-900NXS2 file's header ("208" for the official 2.08), or null. */
    function versionTag(raw) {
        if (raw.length < 0x24) return null;
        var top = String.fromCharCode.apply(null, xor(raw.subarray(0, 0x24)).subarray(0, 23));
        var found = /^DJM-TOP-NEXT {4}Ver\.([0-9]{3})$/.exec(top);
        return found ? found[1] : null;
    }

    /* "mod" (the tested build), "official", "damaged" (named as this build but not it),
       "plus" (another version of the mod), "zip" or "other". */
    async function identify(raw, data) {
        var hash = await sha256(raw);
        if (hash === data.mod_sha256) return "mod";
        if (hash === data.stock_sha256) return "official";
        var tag = versionTag(raw);
        if (tag === data.tag) return "damaged";
        if (tag && tag[0] === "0") return "plus";
        if (isZip(raw)) return "zip";
        return "other";
    }

    /* "PLUS V0.91" for a file of the mod's version 0.91, as the mixer names it. */
    function plusVersion(raw) {
        var tag = versionTag(raw);
        return tag ? "PLUS V" + tag[0] + "." + tag.slice(1) : "";
    }

    var api = {
        PatchError: PatchError, sha256: sha256, sha256Plain: sha256Plain, crc16: crc16, xor: xor,
        readOfficialFile: readOfficialFile, buildMod: buildMod, identify: identify,
        versionTag: versionTag, plusVersion: plusVersion
    };
    root.DjmPatcher = api;
    if (typeof module !== "undefined" && module.exports) module.exports = api;
})(typeof window !== "undefined" ? window : globalThis);
