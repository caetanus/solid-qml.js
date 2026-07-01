function _slicedToArray(r, e) { return _arrayWithHoles(r) || _iterableToArrayLimit(r, e) || _unsupportedIterableToArray(r, e) || _nonIterableRest(); }
function _nonIterableRest() { throw new TypeError("Invalid attempt to destructure non-iterable instance.\nIn order to be iterable, non-array objects must have a [Symbol.iterator]() method."); }
function _unsupportedIterableToArray(r, a) { if (r) { if ("string" == typeof r) return _arrayLikeToArray(r, a); var t = {}.toString.call(r).slice(8, -1); return "Object" === t && r.constructor && (t = r.constructor.name), "Map" === t || "Set" === t ? Array.from(r) : "Arguments" === t || /^(?:Ui|I)nt(?:8|16|32)(?:Clamped)?Array$/.test(t) ? _arrayLikeToArray(r, a) : void 0; } }
function _arrayLikeToArray(r, a) { (null == a || a > r.length) && (a = r.length); for (var e = 0, n = Array(a); e < a; e++) n[e] = r[e]; return n; }
function _iterableToArrayLimit(r, l) { var t = null == r ? null : "undefined" != typeof Symbol && r[Symbol.iterator] || r["@@iterator"]; if (null != t) { var e, n, i, u, a = [], f = !0, o = !1; try { if (i = (t = t.call(r)).next, 0 === l) { if (Object(t) !== t) return; f = !1; } else for (; !(f = (e = i.call(t)).done) && (a.push(e.value), a.length !== l); f = !0); } catch (r) { o = !0, n = r; } finally { try { if (!f && null != t.return && (u = t.return(), Object(u) !== u)) return; } finally { if (o) throw n; } } return a; } }
function _arrayWithHoles(r) { if (Array.isArray(r)) return r; }
import { ambiguousMinimalCodePoint, ambiguousMaximumCodePoint, ambiguousRanges, fullwidthMinimalCodePoint, fullwidthMaximumCodePoint, fullwidthRanges, halfwidthMinimalCodePoint, halfwidthMaximumCodePoint, halfwidthRanges, narrowMinimalCodePoint, narrowMaximumCodePoint, narrowRanges, wideMinimalCodePoint, wideMaximumCodePoint, wideRanges } from "./lookup_data_24ce5d42a6.mjs";
import { isInRange } from "./utilities_08ec90d49e.mjs";
var commonCjkCodePoint = 0x4E00;
var _findWideFastPathRang = findWideFastPathRange(wideRanges),
  _findWideFastPathRang2 = _slicedToArray(_findWideFastPathRang, 2),
  wideFastPathStart = _findWideFastPathRang2[0],
  wideFastPathEnd = _findWideFastPathRang2[1];
function findWideFastPathRange(ranges) {
  var fastPathStart = ranges[0];
  var fastPathEnd = ranges[1];
  for (var index = 0; index < ranges.length; index += 2) {
    var start = ranges[index];
    var end = ranges[index + 1];
    if (commonCjkCodePoint >= start && commonCjkCodePoint <= end) {
      return [start, end];
    }
    if (end - start > fastPathEnd - fastPathStart) {
      fastPathStart = start;
      fastPathEnd = end;
    }
  }
  return [fastPathStart, fastPathEnd];
}
export var isAmbiguous = function isAmbiguous(codePoint) {
  if (codePoint < ambiguousMinimalCodePoint || codePoint > ambiguousMaximumCodePoint) {
    return false;
  }
  return isInRange(ambiguousRanges, codePoint);
};
export var isFullWidth = function isFullWidth(codePoint) {
  if (codePoint < fullwidthMinimalCodePoint || codePoint > fullwidthMaximumCodePoint) {
    return false;
  }
  return isInRange(fullwidthRanges, codePoint);
};
var isHalfWidth = function isHalfWidth(codePoint) {
  if (codePoint < halfwidthMinimalCodePoint || codePoint > halfwidthMaximumCodePoint) {
    return false;
  }
  return isInRange(halfwidthRanges, codePoint);
};
var isNarrow = function isNarrow(codePoint) {
  if (codePoint < narrowMinimalCodePoint || codePoint > narrowMaximumCodePoint) {
    return false;
  }
  return isInRange(narrowRanges, codePoint);
};
export var isWide = function isWide(codePoint) {
  if (codePoint >= wideFastPathStart && codePoint <= wideFastPathEnd) {
    return true;
  }
  if (codePoint < wideMinimalCodePoint || codePoint > wideMaximumCodePoint) {
    return false;
  }
  return isInRange(wideRanges, codePoint);
};
export function getCategory(codePoint) {
  if (isAmbiguous(codePoint)) {
    return 'ambiguous';
  }
  if (isFullWidth(codePoint)) {
    return 'fullwidth';
  }
  if (isHalfWidth(codePoint)) {
    return 'halfwidth';
  }
  if (isNarrow(codePoint)) {
    return 'narrow';
  }
  if (isWide(codePoint)) {
    return 'wide';
  }
  return 'neutral';
}