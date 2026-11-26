// Preloaded with `node -r` by wasm/run.sh. A VM that fails during initialization
// hangs instead of exiting (vm_exit with live pthreads), so end the process after
// $KILL_AFTER ms. The timer is unref'd and does not keep a healthy run alive.
const ms = +(process.env.KILL_AFTER || 20000);
setTimeout(() => {
  console.error(`[killer] main JS thread alive after ${ms}ms; exiting`);
  process.exit(3);
}, ms).unref?.();
