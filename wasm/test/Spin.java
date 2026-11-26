/*
 * Regression test for safepoint polls on backward branches in the Zero
 * interpreter: System.gc() needs a safepoint while another thread spins in a
 * loop without calls. Without the polls this hangs forever (also on native Zero).
 */
public class Spin {
    static volatile boolean stop;
    public static void main(String[] a) throws Exception {
        Thread t = new Thread(() -> { long x = 0; while (!stop) x++; });
        t.setDaemon(true);
        t.start();
        Thread.sleep(200);
        long t0 = System.nanoTime();
        System.gc();   // needs a safepoint
        System.out.println("System.gc() with a spinning thread took " + (System.nanoTime() - t0) / 1_000_000 + " ms");
        stop = true;
    }
}
