// Emscripten --pre-js (wasm/link.sh TIMING=1): print startup phase timestamps
// as "[timing] <ms since epoch> <ms since process start> <label>" on stderr,
// comparable with -Xlog:...:timemillis and System.currentTimeMillis().
(function () {
  if (typeof process !== 'object') return;
  var isWorker = typeof ENVIRONMENT_IS_PTHREAD !== 'undefined' && ENVIRONMENT_IS_PTHREAD;
  if (isWorker) return;
  var origin = performance.timeOrigin;
  function mark(label) {
    var now = origin + performance.now();
    process.stderr.write('[timing] ' + now.toFixed(1) + ' ' + performance.now().toFixed(1) + ' ' + label + '\n');
  }
  mark('pre-js start (java.js loaded and parsed)');
  var depsMax = 0;
  Module['monitorRunDependencies'] = function (left) {
    if (left > depsMax) { depsMax = left; mark('run dependencies pending: ' + left); }
    if (left === 0) mark('run dependencies done (wasm instantiated, thread pool ready)');
  };
  Module['preRun'] = Module['preRun'] || [];
  Module['preRun'].push(function () { mark('preRun'); });
  Module['onRuntimeInitialized'] = function () { mark('runtime initialized (static ctors done), main() starts'); };
  process.on('exit', function () { mark('process exit'); });
})();
