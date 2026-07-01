(function() {
  function def(proto, name, fn) {
    if (proto[name]) return;
    Object.defineProperty(proto, name, { value: fn, writable: true, configurable: true });
  }
  def(String.prototype, "trimStart", function() { return String(this).replace(/^[\s\uFEFF\xA0]+/, ""); });
  def(String.prototype, "trimLeft", String.prototype.trimStart);
  def(String.prototype, "replaceAll", function(search, replacement) {
    var source = String(this);
    if (search instanceof RegExp) {
      if (String(search.flags).indexOf("g") === -1) throw new TypeError("String.prototype.replaceAll called with a non-global RegExp");
      return source.replace(search, replacement);
    }
    return source.split(String(search)).join(String(replacement));
  });
  def(Array.prototype, "at", function(index) {
    var i = Math.trunc(index) || 0;
    if (i < 0) i += this.length;
    return this[i];
  });
  if (typeof Uint8Array !== "undefined") def(Uint8Array.prototype, "at", Array.prototype.at);
})();
function _toConsumableArray(r) { return _arrayWithoutHoles(r) || _iterableToArray(r) || _unsupportedIterableToArray(r) || _nonIterableSpread(); }
function _nonIterableSpread() { throw new TypeError("Invalid attempt to spread non-iterable instance.\nIn order to be iterable, non-array objects must have a [Symbol.iterator]() method."); }
function _iterableToArray(r) { if ("undefined" != typeof Symbol && null != r[Symbol.iterator] || null != r["@@iterator"]) return Array.from(r); }
function _arrayWithoutHoles(r) { if (Array.isArray(r)) return _arrayLikeToArray(r); }
function _createForOfIteratorHelper(r, e) { var t = "undefined" != typeof Symbol && r[Symbol.iterator] || r["@@iterator"]; if (!t) { if (Array.isArray(r) || (t = _unsupportedIterableToArray(r)) || e && r && "number" == typeof r.length) { t && (r = t); var _n = 0, F = function F() {}; return { s: F, n: function n() { return _n >= r.length ? { done: !0 } : { done: !1, value: r[_n++] }; }, e: function e(r) { throw r; }, f: F }; } throw new TypeError("Invalid attempt to iterate non-iterable instance.\nIn order to be iterable, non-array objects must have a [Symbol.iterator]() method."); } var o, a = !0, u = !1; return { s: function s() { t = t.call(r); }, n: function n() { var r = t.next(); return a = r.done, r; }, e: function e(r) { u = !0, o = r; }, f: function f() { try { a || null == t.return || t.return(); } finally { if (u) throw o; } } }; }
function _slicedToArray(r, e) { return _arrayWithHoles(r) || _iterableToArrayLimit(r, e) || _unsupportedIterableToArray(r, e) || _nonIterableRest(); }
function _nonIterableRest() { throw new TypeError("Invalid attempt to destructure non-iterable instance.\nIn order to be iterable, non-array objects must have a [Symbol.iterator]() method."); }
function _unsupportedIterableToArray(r, a) { if (r) { if ("string" == typeof r) return _arrayLikeToArray(r, a); var t = {}.toString.call(r).slice(8, -1); return "Object" === t && r.constructor && (t = r.constructor.name), "Map" === t || "Set" === t ? Array.from(r) : "Arguments" === t || /^(?:Ui|I)nt(?:8|16|32)(?:Clamped)?Array$/.test(t) ? _arrayLikeToArray(r, a) : void 0; } }
function _arrayLikeToArray(r, a) { (null == a || a > r.length) && (a = r.length); for (var e = 0, n = Array(a); e < a; e++) n[e] = r[e]; return n; }
function _iterableToArrayLimit(r, l) { var t = null == r ? null : "undefined" != typeof Symbol && r[Symbol.iterator] || r["@@iterator"]; if (null != t) { var e, n, i, u, a = [], f = !0, o = !1; try { if (i = (t = t.call(r)).next, 0 === l) { if (Object(t) !== t) return; f = !1; } else for (; !(f = (e = i.call(t)).done) && (a.push(e.value), a.length !== l); f = !0); } catch (r) { o = !0, n = r; } finally { try { if (!f && null != t.return && (u = t.return(), Object(u) !== u)) return; } finally { if (o) throw n; } } return a; } }
function _arrayWithHoles(r) { if (Array.isArray(r)) return r; }
import stringWidth from "./index_96f343b4f9.mjs";
import stripAnsi from "./index_04d8eb3cec.mjs";
import ansiStyles from "./index_a0c3739537.mjs";
var ANSI_ESCAPE = "\x1B";
var ANSI_ESCAPE_CSI = "\x9B";
var ESCAPES = new Set([ANSI_ESCAPE, ANSI_ESCAPE_CSI]);
var ANSI_ESCAPE_BELL = "\x07";
var ANSI_CSI = '[';
var ANSI_OSC = ']';
var ANSI_SGR_TERMINATOR = 'm';
var ANSI_SGR_RESET = 0;
var ANSI_SGR_RESET_FOREGROUND = 39;
var ANSI_SGR_RESET_BACKGROUND = 49;
var ANSI_SGR_RESET_UNDERLINE_COLOR = 59;
var ANSI_SGR_FOREGROUND_EXTENDED = 38;
var ANSI_SGR_BACKGROUND_EXTENDED = 48;
var ANSI_SGR_UNDERLINE_COLOR_EXTENDED = 58;
var ANSI_SGR_COLOR_MODE_256 = 5;
var ANSI_SGR_COLOR_MODE_RGB = 2;
var ANSI_ESCAPE_LINK = "".concat(ANSI_OSC, "8;;");
var ANSI_ESCAPE_REGEX = new __SqRegExp("^\\u001B(?:\\".concat(ANSI_CSI, "(?<sgr>[0-9;]*)").concat(ANSI_SGR_TERMINATOR, "|").concat(ANSI_ESCAPE_LINK, "(?<uri>[^\\u0007\\u001B]*)(?:\\u0007|\\u001B\\\\))"));
var ANSI_ESCAPE_CSI_REGEX = new __SqRegExp("^\\u009B(?<sgr>[0-9;]*)".concat(ANSI_SGR_TERMINATOR));
var ANSI_SGR_MODIFIER_CLOSE_CODES = new Set(ansiStyles.codes.values());
ANSI_SGR_MODIFIER_CLOSE_CODES.delete(ANSI_SGR_RESET);
var segmenter = new Intl.Segmenter();
var getGraphemes = function getGraphemes(string) {
  return Array.from(segmenter.segment(string), function (_ref) {
    var segment = _ref.segment;
    return segment;
  });
};
var TAB_SIZE = 8;
var wrapAnsiCode = function wrapAnsiCode(code) {
  return "".concat(ANSI_ESCAPE).concat(ANSI_CSI).concat(code).concat(ANSI_SGR_TERMINATOR);
};
var wrapAnsiHyperlink = function wrapAnsiHyperlink(url) {
  return "".concat(ANSI_ESCAPE).concat(ANSI_ESCAPE_LINK).concat(url).concat(ANSI_ESCAPE_BELL);
};
var getSgrTokens = function getSgrTokens(sgrParameters) {
  var codes = sgrParameters.split(';').map(function (sgrParameter) {
    return sgrParameter === '' ? ANSI_SGR_RESET : Number.parseInt(sgrParameter, 10);
  });
  var sgrTokens = [];
  for (var index = 0; index < codes.length; index++) {
    var code = codes[index];
    if (!Number.isFinite(code)) {
      continue;
    }
    if (code === ANSI_SGR_FOREGROUND_EXTENDED || code === ANSI_SGR_BACKGROUND_EXTENDED || code === ANSI_SGR_UNDERLINE_COLOR_EXTENDED) {
      if (index + 1 >= codes.length) {
        break;
      }
      var mode = codes[index + 1];
      if (mode === ANSI_SGR_COLOR_MODE_256 && Number.isFinite(codes[index + 2])) {
        sgrTokens.push([code, mode, codes[index + 2]]);
        index += 2;
        continue;
      }
      var red = codes[index + 2];
      var green = codes[index + 3];
      var blue = codes[index + 4];
      if (mode === ANSI_SGR_COLOR_MODE_RGB && Number.isFinite(red) && Number.isFinite(green) && Number.isFinite(blue)) {
        sgrTokens.push([code, mode, red, green, blue]);
        index += 4;
        continue;
      }
      break;
    }
    sgrTokens.push([code]);
  }
  return sgrTokens;
};
var removeActiveStyle = function removeActiveStyle(activeStyles, family) {
  var activeStyleIndex = activeStyles.findIndex(function (activeStyle) {
    return activeStyle.family === family;
  });
  if (activeStyleIndex !== -1) {
    activeStyles.splice(activeStyleIndex, 1);
  }
};
var upsertActiveStyle = function upsertActiveStyle(activeStyles, nextActiveStyle) {
  removeActiveStyle(activeStyles, nextActiveStyle.family);
  activeStyles.push(nextActiveStyle);
};
var removeModifierStylesByClose = function removeModifierStylesByClose(activeStyles, closeCode) {
  for (var index = activeStyles.length - 1; index >= 0; index--) {
    var activeStyle = activeStyles[index];
    if (activeStyle.family.startsWith('modifier-') && activeStyle.close === closeCode) {
      activeStyles.splice(index, 1);
    }
  }
};
var getColorStyle = function getColorStyle(code, sgrToken) {
  if (code >= 30 && code <= 37 || code >= 90 && code <= 97 || code === ANSI_SGR_FOREGROUND_EXTENDED && sgrToken.length > 1) {
    return {
      family: 'foreground',
      open: sgrToken.join(';'),
      close: ANSI_SGR_RESET_FOREGROUND
    };
  }
  if (code >= 40 && code <= 47 || code >= 100 && code <= 107 || code === ANSI_SGR_BACKGROUND_EXTENDED && sgrToken.length > 1) {
    return {
      family: 'background',
      open: sgrToken.join(';'),
      close: ANSI_SGR_RESET_BACKGROUND
    };
  }
  if (code === ANSI_SGR_UNDERLINE_COLOR_EXTENDED && sgrToken.length > 1) {
    return {
      family: 'underlineColor',
      open: sgrToken.join(';'),
      close: ANSI_SGR_RESET_UNDERLINE_COLOR
    };
  }
};
var applySgrResetCode = function applySgrResetCode(code, activeStyles) {
  if (code === ANSI_SGR_RESET) {
    activeStyles.length = 0;
    return true;
  }
  if (code === ANSI_SGR_RESET_FOREGROUND) {
    removeActiveStyle(activeStyles, 'foreground');
    return true;
  }
  if (code === ANSI_SGR_RESET_BACKGROUND) {
    removeActiveStyle(activeStyles, 'background');
    return true;
  }
  if (code === ANSI_SGR_RESET_UNDERLINE_COLOR) {
    removeActiveStyle(activeStyles, 'underlineColor');
    return true;
  }
  if (ANSI_SGR_MODIFIER_CLOSE_CODES.has(code)) {
    removeModifierStylesByClose(activeStyles, code);
    return true;
  }
  return false;
};
var applySgrToken = function applySgrToken(sgrToken, activeStyles) {
  var _sgrToken = _slicedToArray(sgrToken, 1),
    code = _sgrToken[0];
  if (applySgrResetCode(code, activeStyles)) {
    return;
  }
  var colorStyle = getColorStyle(code, sgrToken);
  if (colorStyle) {
    upsertActiveStyle(activeStyles, colorStyle);
    return;
  }
  var close = ansiStyles.codes.get(code);
  if (close !== undefined && close !== ANSI_SGR_RESET) {
    upsertActiveStyle(activeStyles, {
      family: "modifier-".concat(code),
      open: sgrToken.join(';'),
      close: close
    });
  }
};
var applySgrParameters = function applySgrParameters(sgrParameters, activeStyles) {
  var _iterator = _createForOfIteratorHelper(getSgrTokens(sgrParameters)),
    _step;
  try {
    for (_iterator.s(); !(_step = _iterator.n()).done;) {
      var sgrToken = _step.value;
      applySgrToken(sgrToken, activeStyles);
    }
  } catch (err) {
    _iterator.e(err);
  } finally {
    _iterator.f();
  }
};
var applySgrResets = function applySgrResets(sgrParameters, activeStyles) {
  var _iterator2 = _createForOfIteratorHelper(getSgrTokens(sgrParameters)),
    _step2;
  try {
    for (_iterator2.s(); !(_step2 = _iterator2.n()).done;) {
      var sgrToken = _step2.value;
      var _sgrToken2 = _slicedToArray(sgrToken, 1),
        code = _sgrToken2[0];
      applySgrResetCode(code, activeStyles);
    }
  } catch (err) {
    _iterator2.e(err);
  } finally {
    _iterator2.f();
  }
};
var applyLeadingSgrResets = function applyLeadingSgrResets(string, activeStyles) {
  var remainder = string;
  while (remainder.length > 0) {
    if (remainder.startsWith(ANSI_ESCAPE) && remainder[1] !== '\\') {
      var match = ANSI_ESCAPE_REGEX.exec(remainder);
      if (!match) {
        break;
      }
      if (match.groups.sgr !== undefined) {
        applySgrResets(match.groups.sgr, activeStyles);
      }
      remainder = remainder.slice(match[0].length);
      continue;
    }
    if (remainder.startsWith(ANSI_ESCAPE_CSI)) {
      var _match = ANSI_ESCAPE_CSI_REGEX.exec(remainder);
      if (!_match || _match.groups.sgr === undefined) {
        break;
      }
      applySgrResets(_match.groups.sgr, activeStyles);
      remainder = remainder.slice(_match[0].length);
      continue;
    }
    break;
  }
};
var getClosingSgrSequence = function getClosingSgrSequence(activeStyles) {
  return _toConsumableArray(activeStyles).reverse().map(function (activeStyle) {
    return wrapAnsiCode(activeStyle.close);
  }).join('');
};
var getOpeningSgrSequence = function getOpeningSgrSequence(activeStyles) {
  return activeStyles.map(function (activeStyle) {
    return wrapAnsiCode(activeStyle.open);
  }).join('');
};
var wordLengths = function wordLengths(string) {
  return string.split(' ').map(function (word) {
    return stringWidth(word);
  });
};
var wrapWord = function wrapWord(rows, word, columns) {
  var characters = getGraphemes(word);
  var isInsideEscape = false;
  var isInsideLinkEscape = false;
  var visible = stringWidth(stripAnsi(rows[rows.length - 1]));
  var _iterator3 = _createForOfIteratorHelper(characters.entries()),
    _step3;
  try {
    for (_iterator3.s(); !(_step3 = _iterator3.n()).done;) {
      var _step3$value = _slicedToArray(_step3.value, 2),
        index = _step3$value[0],
        character = _step3$value[1];
      var characterLength = stringWidth(character);
      if (visible + characterLength <= columns) {
        rows[rows.length - 1] += character;
      } else {
        rows.push(character);
        visible = 0;
      }
      if (ESCAPES.has(character) && !(isInsideLinkEscape && character === ANSI_ESCAPE && characters[index + 1] === '\\')) {
        isInsideEscape = true;
        var ansiEscapeLinkCandidate = characters.slice(index + 1, index + 1 + ANSI_ESCAPE_LINK.length).join('');
        isInsideLinkEscape = ansiEscapeLinkCandidate === ANSI_ESCAPE_LINK;
      }
      if (isInsideEscape) {
        if (isInsideLinkEscape) {
          if (character === ANSI_ESCAPE_BELL || character === '\\' && index > 0 && characters[index - 1] === ANSI_ESCAPE) {
            isInsideEscape = false;
            isInsideLinkEscape = false;
          }
        } else if (character === ANSI_SGR_TERMINATOR) {
          isInsideEscape = false;
        }
        continue;
      }
      visible += characterLength;
      if (visible === columns && index < characters.length - 1) {
        rows.push('');
        visible = 0;
      }
    }
  } catch (err) {
    _iterator3.e(err);
  } finally {
    _iterator3.f();
  }
  if (!visible && rows[rows.length - 1].length > 0 && rows.length > 1) {
    rows[rows.length - 2] += rows.pop();
  }
};
var stringVisibleTrimSpacesRight = function stringVisibleTrimSpacesRight(string) {
  var words = string.split(' ');
  var last = words.length;
  while (last > 0) {
    if (stringWidth(words[last - 1]) > 0) {
      break;
    }
    last--;
  }
  if (last === words.length) {
    return string;
  }
  return words.slice(0, last).join(' ') + words.slice(last).join('');
};
var expandTabs = function expandTabs(line) {
  if (!line.includes('\t')) {
    return line;
  }
  var segments = line.split('\t');
  var visible = 0;
  var expandedLine = '';
  var _iterator4 = _createForOfIteratorHelper(segments.entries()),
    _step4;
  try {
    for (_iterator4.s(); !(_step4 = _iterator4.n()).done;) {
      var _step4$value = _slicedToArray(_step4.value, 2),
        index = _step4$value[0],
        segment = _step4$value[1];
      expandedLine += segment;
      visible += stringWidth(segment);
      if (index < segments.length - 1) {
        var spaces = TAB_SIZE - visible % TAB_SIZE;
        expandedLine += ' '.repeat(spaces);
        visible += spaces;
      }
    }
  } catch (err) {
    _iterator4.e(err);
  } finally {
    _iterator4.f();
  }
  return expandedLine;
};
var exec = function exec(string, columns) {
  var options = arguments.length > 2 && arguments[2] !== undefined ? arguments[2] : {};
  if (options.trim !== false && string.trim() === '') {
    return '';
  }
  var returnValue = '';
  var escapeUrl;
  var activeStyles = [];
  var lengths = wordLengths(string);
  var rows = [''];
  var _iterator5 = _createForOfIteratorHelper(string.split(' ').entries()),
    _step5;
  try {
    for (_iterator5.s(); !(_step5 = _iterator5.n()).done;) {
      var _step5$value = _slicedToArray(_step5.value, 2),
        index = _step5$value[0],
        word = _step5$value[1];
      if (options.trim !== false) {
        rows[rows.length - 1] = rows[rows.length - 1].trimStart();
      }
      var rowLength = stringWidth(rows[rows.length - 1]);
      if (index !== 0) {
        if (rowLength >= columns && (options.wordWrap === false || options.trim === false)) {
          rows.push('');
          rowLength = 0;
        }
        if (rowLength > 0 || options.trim === false) {
          rows[rows.length - 1] += ' ';
          rowLength++;
        }
      }
      if (options.hard && options.wordWrap !== false && lengths[index] > columns) {
        var remainingColumns = columns - rowLength;
        var breaksStartingThisLine = 1 + Math.floor((lengths[index] - remainingColumns - 1) / columns);
        var breaksStartingNextLine = Math.floor((lengths[index] - 1) / columns);
        if (breaksStartingNextLine < breaksStartingThisLine) {
          rows.push('');
        }
        wrapWord(rows, word, columns);
        continue;
      }
      if (rowLength + lengths[index] > columns && rowLength > 0 && lengths[index] > 0) {
        if (options.wordWrap === false && rowLength < columns) {
          wrapWord(rows, word, columns);
          continue;
        }
        rows.push('');
      }
      if (rowLength + lengths[index] > columns && options.wordWrap === false) {
        wrapWord(rows, word, columns);
        continue;
      }
      rows[rows.length - 1] += word;
    }
  } catch (err) {
    _iterator5.e(err);
  } finally {
    _iterator5.f();
  }
  if (options.trim !== false) {
    rows = rows.map(function (row) {
      return stringVisibleTrimSpacesRight(row);
    });
  }
  var preString = rows.join('\n');
  var pre = getGraphemes(preString);
  var preStringIndex = 0;
  var _iterator6 = _createForOfIteratorHelper(pre.entries()),
    _step6;
  try {
    for (_iterator6.s(); !(_step6 = _iterator6.n()).done;) {
      var _step6$value = _slicedToArray(_step6.value, 2),
        _index = _step6$value[0],
        character = _step6$value[1];
      returnValue += character;
      if (character === ANSI_ESCAPE && pre[_index + 1] !== '\\') {
        var _ref2 = ANSI_ESCAPE_REGEX.exec(preString.slice(preStringIndex)) || {
            groups: {}
          },
          groups = _ref2.groups;
        if (groups.sgr !== undefined) {
          applySgrParameters(groups.sgr, activeStyles);
        } else if (groups.uri !== undefined) {
          escapeUrl = groups.uri.length === 0 ? undefined : groups.uri;
        }
      } else if (character === ANSI_ESCAPE_CSI) {
        var _ref3 = ANSI_ESCAPE_CSI_REGEX.exec(preString.slice(preStringIndex)) || {
            groups: {}
          },
          _groups = _ref3.groups;
        if (_groups.sgr !== undefined) {
          applySgrParameters(_groups.sgr, activeStyles);
        }
      }
      if (pre[_index + 1] === '\n') {
        if (escapeUrl) {
          returnValue += wrapAnsiHyperlink('');
        }
        returnValue += getClosingSgrSequence(activeStyles);
      } else if (character === '\n') {
        var openingStyles = [].concat(activeStyles);
        applyLeadingSgrResets(preString.slice(preStringIndex + 1), openingStyles);
        returnValue += getOpeningSgrSequence(openingStyles);
        if (escapeUrl) {
          returnValue += wrapAnsiHyperlink(escapeUrl);
        }
      }
      preStringIndex += character.length;
    }
  } catch (err) {
    _iterator6.e(err);
  } finally {
    _iterator6.f();
  }
  return returnValue;
};
export default function wrapAnsi(string, columns, options) {
  return String(string).normalize().split('\r\n').join('\n').split('\n').map(function (line) {
    return exec(expandTabs(line), columns, options);
  }).join('\n');
}
export { wrapAnsi as Default };