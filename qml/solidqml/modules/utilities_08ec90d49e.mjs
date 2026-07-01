export var isInRange = function isInRange(ranges, codePoint) {
  var low = 0;
  var high = Math.floor(ranges.length / 2) - 1;
  while (low <= high) {
    var mid = Math.floor((low + high) / 2);
    var i = mid * 2;
    if (codePoint < ranges[i]) {
      high = mid - 1;
    } else if (codePoint > ranges[i + 1]) {
      low = mid + 1;
    } else {
      return true;
    }
  }
  return false;
};