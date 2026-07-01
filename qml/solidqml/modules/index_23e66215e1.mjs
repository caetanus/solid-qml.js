export default function ansiRegex() {
  var _ref = arguments.length > 0 && arguments[0] !== undefined ? arguments[0] : {},
    _ref$onlyFirst = _ref.onlyFirst,
    onlyFirst = _ref$onlyFirst === void 0 ? false : _ref$onlyFirst;
  var ST = "(?:\\u0007|\\u001B\\u005C|\\u009C)";
  var osc = "(?:\\u001B\\][\\s\\S]*?".concat(ST, ")");
  var csi = "[\\u001B\\u009B][[\\]()#;?]*(?:\\d{1,4}(?:[;:]\\d{0,4})*)?[\\dA-PR-TZcf-nq-uy=><~]";
  var pattern = "".concat(osc, "|").concat(csi);
  return new __SqRegExp(pattern, onlyFirst ? undefined : 'g');
}
export { ansiRegex as Default };