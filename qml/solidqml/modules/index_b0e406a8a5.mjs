function _typeof(o) { "@babel/helpers - typeof"; return _typeof = "function" == typeof Symbol && "symbol" == typeof Symbol.iterator ? function (o) { return typeof o; } : function (o) { return o && "function" == typeof Symbol && o.constructor === Symbol && o !== Symbol.prototype ? "symbol" : typeof o; }, _typeof(o); }
import { getCategory, isAmbiguous, isFullWidth, isWide } from "./lookup_08ee6eeb88.mjs";
function validate(codePoint) {
  if (!Number.isSafeInteger(codePoint)) {
    throw new TypeError("Expected a code point, got `".concat(_typeof(codePoint), "`."));
  }
}
export function eastAsianWidthType(codePoint) {
  validate(codePoint);
  return getCategory(codePoint);
}
export function eastAsianWidth(codePoint) {
  var _ref = arguments.length > 1 && arguments[1] !== undefined ? arguments[1] : {},
    _ref$ambiguousAsWide = _ref.ambiguousAsWide,
    ambiguousAsWide = _ref$ambiguousAsWide === void 0 ? false : _ref$ambiguousAsWide;
  validate(codePoint);
  if (isFullWidth(codePoint) || isWide(codePoint) || ambiguousAsWide && isAmbiguous(codePoint)) {
    return 2;
  }
  return 1;
}
export { isFullWidth as _isFullWidth, isWide as _isWide } from "./lookup_08ee6eeb88.mjs";