/* FontExp L1 glyf builder — raw TTF glyf slices, LGF1 output (see tools/build_l1glyf_cache.py) */
(function (global) {
  'use strict';

  var L1GLYF_MAGIC = 0x3146474c;
  var L1GLYF_VERSION = 1;
  var ASCII_START = 0x20;
  var ASCII_END = 0x7e;
  var PRIO_CJK_PUNCT = [
    0x3002, 0x3001, 0x3000, 0x3010, 0x3011, 0x2014, 0x201c, 0x201d,
    0x2026, 0x00b7, 0xff0c, 0xff0e, 0xff01, 0xff1f, 0xff1b, 0xff1a,
    0xff08, 0xff09, 0xff3b, 0xff3d, 0x300a, 0x300b, 0x2018, 0x2019
  ];
  var PRIO_ASCII_PUNCT = '.,;:!?\\"\'()[]-_+=*/\\@#$%&<>{}|~`^';

  /* LEVEL1_CHARS replaced by tools/gen_l1glyf_web_js.py */
  var LEVEL1_CHARS = window.__LEVEL1_CHARS__ || [];

  var crcTable = (function () {
    var t = new Uint32Array(256);
    for (var n = 0; n < 256; n++) {
      var c = n;
      for (var k = 0; k < 8; k++) {
        c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
      }
      t[n] = c >>> 0;
    }
    return t;
  })();

  function crc32(buf, init) {
    var c = (init === undefined) ? 0xffffffff : init;
    for (var i = 0; i < buf.length; i++) {
      c = crcTable[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
    }
    return (c ^ 0xffffffff) >>> 0;
  }

  function tag4(s) {
    return ((s.charCodeAt(0) << 24) | (s.charCodeAt(1) << 16) |
      (s.charCodeAt(2) << 8) | s.charCodeAt(3)) >>> 0;
  }

  function findTable(u8, tagStr) {
    if (u8.length < 12) return null;
    var num = (u8[4] << 8) | u8[5];
    var want = tag4(tagStr);
    for (var i = 0; i < num; i++) {
      var o = 12 + i * 16;
      var tg = (u8[o] << 24) | (u8[o + 1] << 16) | (u8[o + 2] << 8) | u8[o + 3];
      if (tg === want) {
        var off = (u8[o + 8] << 24) | (u8[o + 9] << 16) | (u8[o + 10] << 8) | u8[o + 11];
        var len = (u8[o + 12] << 24) | (u8[o + 13] << 16) | (u8[o + 14] << 8) | u8[o + 15];
        return { offset: off, length: len };
      }
    }
    return null;
  }

  function u16be(u8, o) { return (u8[o] << 8) | u8[o + 1]; }
  function u32be(u8, o) {
    return ((u8[o] << 24) | (u8[o + 1] << 16) | (u8[o + 2] << 8) | u8[o + 3]) >>> 0;
  }

  function buildPreloadList() {
    var out = [];
    var seen = {};
    function add(cp) {
      if (!seen[cp]) { seen[cp] = 1; out.push(cp); }
    }
    var d;
    for (d = 0x30; d <= 0x39; d++) add(d);
    for (d = 0xff10; d <= 0xff19; d++) add(d);
    for (d = 0; d < PRIO_ASCII_PUNCT.length; d++) add(PRIO_ASCII_PUNCT.charCodeAt(d));
    for (d = 0; d < PRIO_CJK_PUNCT.length; d++) add(PRIO_CJK_PUNCT[d]);
    for (d = ASCII_START; d <= ASCII_END; d++) add(d);
    for (d = 0; d < LEVEL1_CHARS.length; d++) add(LEVEL1_CHARS[d]);
    return out;
  }

  function pickCmapSub(u8, cmap) {
    var num = u16be(u8, cmap.offset + 2);
    var best = null;
    var bestFmt = 0;
    for (var i = 0; i < num; i++) {
      var rec = cmap.offset + 4 + i * 8;
      var pid = u16be(u8, rec);
      var soff = u32be(u8, rec + 4);
      var sub = cmap.offset + soff;
      var fmt = u16be(u8, sub);
      if (fmt === 12 && (pid === 0 || pid === 3)) return { sub: sub, fmt: 12 };
      if (fmt === 4 && (pid === 0 || pid === 3) && bestFmt !== 12) {
        best = sub;
        bestFmt = 4;
      }
    }
    return best ? { sub: best, fmt: bestFmt } : null;
  }

  function cmapLookupFmt12(u8, sub, cp) {
    var ng = u32be(u8, sub + 12);
    var lo = 0;
    var hi = ng - 1;
    while (lo <= hi) {
      var mid = (lo + hi) >>> 1;
      var g = sub + 16 + mid * 12;
      var start = u32be(u8, g);
      var end = u32be(u8, g + 4);
      if (cp < start) hi = mid - 1;
      else if (cp > end) lo = mid + 1;
      else return (u32be(u8, g + 8) + (cp - start)) & 0xffff;
    }
    return 0;
  }

  function cmapLookupFmt4(u8, sub, cp) {
    var seg = u16be(u8, sub + 6) >> 1;
    var endCodes = sub + 14;
    var startCodes = endCodes + seg * 2 + 2;
    var idDelta = startCodes + seg * 2;
    var idRange = idDelta + seg * 2;
    var glyphIdArray = idRange + seg * 2;
    for (var i = 0; i < seg; i++) {
      var ec = u16be(u8, endCodes + i * 2);
      var sc = u16be(u8, startCodes + i * 2);
      if (cp < sc || cp > ec) continue;
      var ro = u16be(u8, idRange + i * 2);
      if (ro === 0) return ((cp + u16be(u8, idDelta + i * 2)) & 0xffff);
      var idx = ro + (cp - sc) * 2;
      return u16be(u8, glyphIdArray + idx);
    }
    return 0;
  }

  function cmapLookup(u8, cmap, cp) {
    var pick = pickCmapSub(u8, cmap);
    if (!pick) return 0;
    if (pick.fmt === 12) return cmapLookupFmt12(u8, pick.sub, cp);
    if (pick.fmt === 4) return cmapLookupFmt4(u8, pick.sub, cp);
    return 0;
  }

  function headIndexFormat(u8, head) {
    return u16be(u8, head.offset + 50);
  }

  function maxpNumGlyphs(u8, maxp) {
    return u16be(u8, maxp.offset + 4);
  }

  function glyfBytes(u8, loca, glyf, gid, locFmt, numGlyphs) {
    if (gid <= 0 || gid >= numGlyphs) return null;
    var g1, g2;
    if (locFmt === 0) {
      g1 = u16be(u8, loca.offset + gid * 2) * 2;
      g2 = u16be(u8, loca.offset + (gid + 1) * 2) * 2;
    } else {
      g1 = u32be(u8, loca.offset + gid * 4);
      g2 = u32be(u8, loca.offset + (gid + 1) * 4);
    }
    var sz = g2 - g1;
    if (sz <= 0) return null;
    var abs = glyf.offset + g1;
    if (abs + sz > u8.length) return null;
    var slice = u8.subarray(abs, abs + sz);
    if (slice.length >= 2) {
      var nc = (slice[0] << 8) | slice[1];
      if (nc === 0) return null;
    }
    return slice;
  }

  function stemFromName(name) {
    var i = name.lastIndexOf('.');
    return (i > 0) ? name.substring(0, i) : name;
  }

  function buildFromBuffer(ab, ttfName) {
    var u8 = new Uint8Array(ab);
    var cmap = findTable(u8, 'cmap');
    var loca = findTable(u8, 'loca');
    var glyf = findTable(u8, 'glyf');
    var head = findTable(u8, 'head');
    var maxp = findTable(u8, 'maxp');
    if (!cmap || !loca || !glyf || !head || !maxp) {
      throw new Error('Not a valid TTF (missing tables)');
    }
    var locFmt = headIndexFormat(u8, head);
    var numGlyphs = maxpNumGlyphs(u8, maxp);
    var unicodes = buildPreloadList();
    var entries = [];
    var glyfParts = [];
    var totalGlyf = 0;
    var ui, gid, gb;
    for (ui = 0; ui < unicodes.length; ui++) {
      gid = cmapLookup(u8, cmap, unicodes[ui]);
      if (!gid) continue;
      gb = glyfBytes(u8, loca, glyf, gid, locFmt, numGlyphs);
      if (!gb) continue;
      entries.push({ u: unicodes[ui], gid: gid, off: totalGlyf, sz: gb.length });
      glyfParts.push(gb);
      totalGlyf += gb.length;
    }
    entries.sort(function (a, b) { return a.gid - b.gid; });

    var lookupCount = entries.length;
    var lookupBytes = lookupCount * 16;
    var headerSize = 32;
    var lookupOff = headerSize + 4;
    var dataOff = lookupOff + lookupBytes;
    var totalSize = dataOff + totalGlyf;
    var out = new Uint8Array(totalSize);
    var ttfSize = u8.length;
    var ttfCrc = crc32(u8);

    function w32(o, v) {
      out[o] = v & 0xff; out[o + 1] = (v >> 8) & 0xff;
      out[o + 2] = (v >> 16) & 0xff; out[o + 3] = (v >> 24) & 0xff;
    }
    function w16(o, v) {
      out[o] = (v >> 8) & 0xff; out[o + 1] = v & 0xff;
    }

    w32(0, L1GLYF_MAGIC);
    w16(4, L1GLYF_VERSION);
    w16(6, headerSize);
    w32(8, ttfSize);
    w32(12, ttfCrc);
    w32(16, lookupCount);
    w32(20, totalGlyf);
    w32(24, lookupOff);
    w32(28, dataOff);
    var hdrCrc = crc32(out.subarray(0, headerSize));
    w32(32, hdrCrc);

    var lp = lookupOff;
    for (ui = 0; ui < entries.length; ui++) {
      w32(lp, entries[ui].u);
      w16(lp + 4, entries[ui].gid);
      w16(lp + 6, 0);
      w32(lp + 8, entries[ui].off);
      w32(lp + 12, entries[ui].sz);
      lp += 16;
    }
    var dp = dataOff;
    for (ui = 0; ui < glyfParts.length; ui++) {
      out.set(glyfParts[ui], dp);
      dp += glyfParts[ui].length;
    }

    var stem = stemFromName(ttfName || 'font.ttf');
    return {
      blob: new Blob([out], { type: 'application/octet-stream' }),
      name: stem + '.l1glyf',
      stats: {
        listed: unicodes.length,
        cached: lookupCount,
        glyfBytes: totalGlyf,
        totalBytes: totalSize
      }
    };
  }

  function buildFromFile(file) {
    return new Promise(function (resolve, reject) {
      var fr = new FileReader();
      fr.onload = function () {
        try {
          var r = buildFromBuffer(fr.result, file.name);
          fr = null;
          resolve(r);
        } catch (e) {
          reject(e);
        }
      };
      fr.onerror = function () { reject(fr.error || new Error('read failed')); };
      fr.readAsArrayBuffer(file);
    });
  }

  function uploadForm(fileOrBlob, path, fileName, onProgress) {
    return new Promise(function (resolve, reject) {
      var fd = new FormData();
      fd.append('path', path);
      fd.append('file', fileOrBlob, fileName);
      var xhr = new XMLHttpRequest();
      xhr.open('POST', '/upload', true);
      if (onProgress && xhr.upload) {
        xhr.upload.onprogress = function (e) {
          if (e.lengthComputable) onProgress(e.loaded, e.total);
        };
      }
      xhr.onload = function () {
        if (xhr.status === 200) resolve(xhr.responseText);
        else reject(new Error('HTTP ' + xhr.status + ' ' + xhr.responseText));
      };
      xhr.onerror = function () { reject(new Error('network')); };
      xhr.send(fd);
    });
  }

  /** Font dir TTF: build l1glyf then upload .l1glyf then .ttf (sequential, device-friendly) */
  function uploadTtfWithCache(file, fontPath, hooks) {
    hooks = hooks || {};
    return buildFromFile(file).then(function (built) {
      if (hooks.onBuilt) hooks.onBuilt(built.stats);
      if (hooks.onPhase) hooks.onPhase('l1glyf', built.name);
      return uploadForm(built.blob, '0:/Font/.l1glyf', built.name, hooks.onProgress).then(function () {
        if (hooks.onPhase) hooks.onPhase('ttf', file.name);
        return uploadForm(file, fontPath, file.name, hooks.onProgress);
      });
    });
  }

  global.L1GlyfBuilder = {
    buildFromFile: buildFromFile,
    buildFromBuffer: buildFromBuffer,
    uploadForm: uploadForm,
    uploadTtfWithCache: uploadTtfWithCache
  };
})(typeof window !== 'undefined' ? window : this);
