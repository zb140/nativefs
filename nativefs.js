(function(){
  "use strict";

  var native_fs = require('./build/Release/native_fs');

  module.exports = {};
  module.exports.copy = function() {
    return native_fs.copy.apply(null, arguments);
  };

  module.exports.move = function() {
      return native_fs.move.apply(null, arguments);
  };

  return;

})();
