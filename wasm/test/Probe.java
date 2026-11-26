/*
 * Startup timing probe: prints wall-clock timestamps at the start and end of
 * main() in the "[timing] <epoch ms> - <label>" format that
 * wasm/tools/timeline.py merges with the JS (TIMING=1 link) and HotSpot
 * (-Xlog:...:timemillis) timestamps. "Probe hello" also runs Hello.main.
 *
 *   javac -d /tmp/probe wasm/test/Hello.java wasm/test/Probe.java
 *   jar cf /tmp/probe.jar -C /tmp/probe .
 *   TIMING=1 OUT_DIR=/tmp/wasm-timing wasm/link.sh
 *   OUT_DIR=/tmp/wasm-timing wasm/run.sh -Xlog:startuptime:stderr:timemillis \
 *       -cp /tmp/probe.jar Probe hello 2>&1 | wasm/tools/timeline.py
 */
public class Probe {
    public static void main(String[] args) throws Exception {
        System.err.println("[timing] " + System.currentTimeMillis() + " - java main() start");
        if (args.length > 0 && args[0].equals("hello")) {
            Hello.main(new String[0]);
        }
        System.err.println("[timing] " + System.currentTimeMillis() + " - java main() end");
    }
}
