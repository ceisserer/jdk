/*
 * Test for the WASM_JIT_SPIKE hook (see README.md). Spike$Fast.add/count are
 * replaced by Wasm code from spike_add.so when the hook is enabled.
 *  1. install while 3 threads spin in a tight interpreted loop (no calls, no
 *     locks): does dlopen's thread synchronization still complete?
 *  2. single-thread cost of 1M calls
 *  3. 4 threads calling concurrently
 */
public class Spike {
    static final class Fast {
        static int add(int a, int b) { return a + b; }
        static int count() { return -1; }   // replaced by Wasm code: number of add() calls
    }
    static volatile boolean stop;

    public static void main(String[] args) throws Exception {
        // 1. install (first call links Fast -> dlopen) while other threads are busy
        Thread[] spinners = new Thread[3];
        for (int i = 0; i < spinners.length; i++) {
            spinners[i] = new Thread(() -> { long x = 0; while (!stop) x++; });
            spinners[i].start();
        }
        Thread.sleep(200);
        long t0 = System.nanoTime();
        int first = Fast.add(40, 2);
        long installMs = (System.nanoTime() - t0) / 1_000_000;
        stop = true;
        for (Thread t : spinners) t.join();
        System.out.println("1. first call (link/install) with 3 busy threads: " + installMs + " ms, result " + first);

        // 2. single thread
        int n = 1_000_000, s = 0;
        t0 = System.nanoTime();
        for (int i = 0; i < n; i++) s = Fast.add(s, 1);
        long ms = (System.nanoTime() - t0) / 1_000_000;
        System.out.println("2. single thread: " + n + " calls in " + ms + " ms (" + (ms * 1_000_000L / n) + " ns/call), sum ok=" + (s == n));

        // 3. four threads
        int threads = 4, per = 250_000;
        long[] times = new long[threads];
        Thread[] ts = new Thread[threads];
        for (int t = 0; t < threads; t++) {
            final int id = t;
            ts[t] = new Thread(() -> {
                long t1 = System.nanoTime();
                int x = 0;
                for (int i = 0; i < per; i++) x = Fast.add(x, 1);
                times[id] = (System.nanoTime() - t1) / 1_000_000;
                if (x != per) throw new AssertionError("thread " + id + ": " + x);
            });
        }
        for (Thread t : ts) t.start();
        for (Thread t : ts) t.join();
        System.out.println("3. " + threads + " threads x " + per + " calls: ms/thread=" + java.util.Arrays.toString(times));
        System.out.println("count()=" + Fast.count() + " (Wasm code: " + (1 + n + threads * per) + ", interpreted: -1)");
    }
}
