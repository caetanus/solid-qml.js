// V4 fix: Function.prototype.apply does not spread a TypedArray's values (right length, but
// every element reads as 0), and apply is frozen so it can't be patched. Normalize the argument
// list to a real array; a no-op for anything that isn't a TypedArray view.
function __v4ApplySpread(a) {
  return (a != null && typeof a === "object" && ArrayBuffer.isView(a) && !(a instanceof DataView))
    ? Array.prototype.slice.call(a) : a;
}
var version = '3.8.0';
var VERSION = version;
var _hasBuffer = typeof Buffer === 'function';
var _TD = typeof TextDecoder === 'function' ? new TextDecoder('utf-8', {
  ignoreBOM: true
}) : undefined;
var _TE = typeof TextEncoder === 'function' ? new TextEncoder() : undefined;
var b64ch = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=';
var b64chs = Array.prototype.slice.call(b64ch);
var b64tab = function (a) {
  var tab = {};
  a.forEach(function (c, i) {
    return tab[c] = i;
  });
  return tab;
}(b64chs);
var b64re = /^(?:[A-Za-z\d+\/]{4})*?(?:[A-Za-z\d+\/]{2}(?:==)?|[A-Za-z\d+\/]{3}=?)?$/;
var _fromCC = String.fromCharCode.bind(String);
var _U8Afrom = typeof Uint8Array.from === 'function' ? Uint8Array.from.bind(Uint8Array) : function (it) {
  return new Uint8Array(Array.prototype.slice.call(it, 0));
};
var _mkUriSafe = function _mkUriSafe(src) {
  return src.replace(/=/g, '').replace(/[+\/]/g, function (m0) {
    return m0 == '+' ? '-' : '_';
  });
};
var _tidyB64 = function _tidyB64(s) {
  return s.replace(/[^A-Za-z0-9\+\/]/g, '');
};
var btoaPolyfill = function btoaPolyfill(bin) {
  var u32,
    c0,
    c1,
    c2,
    asc = '';
  var pad = bin.length % 3;
  for (var i = 0; i < bin.length;) {
    if ((c0 = bin.charCodeAt(i++)) > 255 || (c1 = bin.charCodeAt(i++)) > 255 || (c2 = bin.charCodeAt(i++)) > 255) throw new TypeError('invalid character found');
    u32 = c0 << 16 | c1 << 8 | c2;
    asc += b64chs[u32 >> 18 & 63] + b64chs[u32 >> 12 & 63] + b64chs[u32 >> 6 & 63] + b64chs[u32 & 63];
  }
  return pad ? asc.slice(0, pad - 3) + "===".substring(pad) : asc;
};
var _btoa = typeof btoa === 'function' ? function (bin) {
  return btoa(bin);
} : _hasBuffer ? function (bin) {
  return Buffer.from(bin, 'binary').toString('base64');
} : btoaPolyfill;
var _fromUint8Array = _hasBuffer ? function (u8a) {
  return Buffer.from(u8a).toString('base64');
} : function (u8a) {
  var maxargs = 0x1000;
  var strs = [];
  for (var i = 0, l = u8a.length; i < l; i += maxargs) {
    strs.push(_fromCC.apply(null, __v4ApplySpread(u8a.subarray(i, i + maxargs))));
  }
  return _btoa(strs.join(''));
};
var fromUint8Array = function fromUint8Array(u8a) {
  var urlsafe = arguments.length > 1 && arguments[1] !== undefined ? arguments[1] : false;
  return urlsafe ? _mkUriSafe(_fromUint8Array(u8a)) : _fromUint8Array(u8a);
};
var cb_utob = function cb_utob(c) {
  if (c.length < 2) {
    var cc = c.charCodeAt(0);
    return cc < 0x80 ? c : cc < 0x800 ? _fromCC(0xc0 | cc >>> 6) + _fromCC(0x80 | cc & 0x3f) : _fromCC(0xe0 | cc >>> 12 & 0x0f) + _fromCC(0x80 | cc >>> 6 & 0x3f) + _fromCC(0x80 | cc & 0x3f);
  } else {
    var cc = 0x10000 + (c.charCodeAt(0) - 0xD800) * 0x400 + (c.charCodeAt(1) - 0xDC00);
    return _fromCC(0xf0 | cc >>> 18 & 0x07) + _fromCC(0x80 | cc >>> 12 & 0x3f) + _fromCC(0x80 | cc >>> 6 & 0x3f) + _fromCC(0x80 | cc & 0x3f);
  }
};
var re_utob = /[\uD800-\uDBFF][\uDC00-\uDFFFF]|[^\x00-\x7F]/g;
var utob = function utob(u) {
  return u.replace(re_utob, cb_utob);
};
var _encode = _hasBuffer ? function (s) {
  return Buffer.from(s, 'utf8').toString('base64');
} : _TE ? function (s) {
  return _fromUint8Array(_TE.encode(s));
} : function (s) {
  return _btoa(utob(s));
};
var encode = function encode(src) {
  var urlsafe = arguments.length > 1 && arguments[1] !== undefined ? arguments[1] : false;
  return urlsafe ? _mkUriSafe(_encode(src)) : _encode(src);
};
var encodeURI = function encodeURI(src) {
  return encode(src, true);
};
var re_btou = /[\xC0-\xDF][\x80-\xBF]|[\xE0-\xEF][\x80-\xBF]{2}|[\xF0-\xF7][\x80-\xBF]{3}/g;
var cb_btou = function cb_btou(cccc) {
  switch (cccc.length) {
    case 4:
      var cp = (0x07 & cccc.charCodeAt(0)) << 18 | (0x3f & cccc.charCodeAt(1)) << 12 | (0x3f & cccc.charCodeAt(2)) << 6 | 0x3f & cccc.charCodeAt(3),
        offset = cp - 0x10000;
      return _fromCC((offset >>> 10) + 0xD800) + _fromCC((offset & 0x3FF) + 0xDC00);
    case 3:
      return _fromCC((0x0f & cccc.charCodeAt(0)) << 12 | (0x3f & cccc.charCodeAt(1)) << 6 | 0x3f & cccc.charCodeAt(2));
    default:
      return _fromCC((0x1f & cccc.charCodeAt(0)) << 6 | 0x3f & cccc.charCodeAt(1));
  }
};
var btou = function btou(b) {
  return b.replace(re_btou, cb_btou);
};
var atobPolyfill = function atobPolyfill(asc) {
  asc = asc.replace(/\s+/g, '');
  if (!b64re.test(asc)) throw new TypeError('malformed base64.');
  asc += '=='.slice(2 - (asc.length & 3));
  var u24, r1, r2;
  var binArray = [];
  for (var i = 0; i < asc.length;) {
    u24 = b64tab[asc.charAt(i++)] << 18 | b64tab[asc.charAt(i++)] << 12 | (r1 = b64tab[asc.charAt(i++)]) << 6 | (r2 = b64tab[asc.charAt(i++)]);
    if (r1 === 64) {
      binArray.push(_fromCC(u24 >> 16 & 255));
    } else if (r2 === 64) {
      binArray.push(_fromCC(u24 >> 16 & 255, u24 >> 8 & 255));
    } else {
      binArray.push(_fromCC(u24 >> 16 & 255, u24 >> 8 & 255, u24 & 255));
    }
  }
  return binArray.join('');
};
var _atob = typeof atob === 'function' ? function (asc) {
  return atob(_tidyB64(asc));
} : _hasBuffer ? function (asc) {
  return Buffer.from(asc, 'base64').toString('binary');
} : atobPolyfill;
var _toUint8Array = _hasBuffer ? function (a) {
  return _U8Afrom(Buffer.from(a, 'base64'));
} : function (a) {
  return _U8Afrom(_atob(a).split('').map(function (c) {
    return c.charCodeAt(0);
  }));
};
var toUint8Array = function toUint8Array(a) {
  return _toUint8Array(_unURI(a));
};
var _decode = _hasBuffer ? function (a) {
  return Buffer.from(a, 'base64').toString('utf8');
} : _TD ? function (a) {
  return _TD.decode(_toUint8Array(a));
} : function (a) {
  return btou(_atob(a));
};
var _unURI = function _unURI(a) {
  return _tidyB64(a.replace(/[-_]/g, function (m0) {
    return m0 == '-' ? '+' : '/';
  }));
};
var decode = function decode(src) {
  return _decode(_unURI(src));
};
var isValid = function isValid(src) {
  if (typeof src !== 'string') return false;
  var s = src.replace(/\s+/g, '').replace(/={0,2}$/, '');
  return !/[^\s0-9a-zA-Z\+/]/.test(s) || !/[^\s0-9a-zA-Z\-_]/.test(s);
};
var _noEnum = function _noEnum(v) {
  return {
    value: v,
    enumerable: false,
    writable: true,
    configurable: true
  };
};
var extendString = function extendString() {
  var _add = function _add(name, body) {
    return Object.defineProperty(String.prototype, name, _noEnum(body));
  };
  _add('fromBase64', function () {
    return decode(this);
  });
  _add('toBase64', function (urlsafe) {
    return encode(this, urlsafe);
  });
  _add('toBase64URI', function () {
    return encode(this, true);
  });
  _add('toBase64URL', function () {
    return encode(this, true);
  });
  _add('toUint8Array', function () {
    return toUint8Array(this);
  });
};
var extendUint8Array = function extendUint8Array() {
  var _add = function _add(name, body) {
    return Object.defineProperty(Uint8Array.prototype, name, _noEnum(body));
  };
  _add('toBase64', function (urlsafe) {
    return fromUint8Array(this, urlsafe);
  });
  _add('toBase64URI', function () {
    return fromUint8Array(this, true);
  });
  _add('toBase64URL', function () {
    return fromUint8Array(this, true);
  });
};
var extendBuiltins = function extendBuiltins() {
  extendString();
  extendUint8Array();
};
var gBase64 = {
  version: version,
  VERSION: VERSION,
  atob: _atob,
  atobPolyfill: atobPolyfill,
  btoa: _btoa,
  btoaPolyfill: btoaPolyfill,
  fromBase64: decode,
  toBase64: encode,
  encode: encode,
  encodeURI: encodeURI,
  encodeURL: encodeURI,
  utob: utob,
  btou: btou,
  decode: decode,
  isValid: isValid,
  fromUint8Array: fromUint8Array,
  toUint8Array: toUint8Array,
  extendString: extendString,
  extendUint8Array: extendUint8Array,
  extendBuiltins: extendBuiltins
};
export { version };
export { VERSION };
export { _atob as atob };
export { atobPolyfill };
export { _btoa as btoa };
export { btoaPolyfill };
export { decode as fromBase64 };
export { encode as toBase64 };
export { utob };
export { encode };
export { encodeURI };
export { encodeURI as encodeURL };
export { btou };
export { decode };
export { isValid };
export { fromUint8Array };
export { toUint8Array };
export { extendString };
export { extendUint8Array };
export { extendBuiltins };
export { gBase64 as Base64 };