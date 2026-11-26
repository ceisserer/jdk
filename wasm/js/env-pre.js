// Emscripten --pre-js: copy the Node process environment into the program's
// environment so getenv() sees JAVA_HOME, _JAVA_LAUNCHER_DEBUG, etc.
Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  if (typeof process === 'object' && process.env) {
    for (var k in process.env) ENV[k] = process.env[k];
  }
});
